#include "pg_database_connection.h"
#include "pg_value.h"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <locale.h>
#include <string>
#include <system_error>

namespace rs::core::database::postgres {
namespace {
// Process-lifetime C numeric locale; the process locale is never changed.
struct CNumericLocale {
#ifdef _WIN32
  _locale_t handle = _create_locale(LC_NUMERIC, "C");
  ~CNumericLocale() { if (handle) _free_locale(handle); }
#else
  locale_t handle = newlocale(LC_NUMERIC_MASK, "C", nullptr);
  ~CNumericLocale() { if (handle) freelocale(handle); }
#endif
};

const CNumericLocale& c_numeric_locale() {
  static const CNumericLocale locale;
  return locale;
}

// Redshift float text arrives with extra_float_digits=2 (%.17g for float8,
// %.8g for float4), which identifies the stored value. Re-emit the shortest
// decimal that round-trips, using the server's default %g notation threshold
// (15 or 6 significant digits), so exact values keep their default spelling.
// Anything unrecognised is returned unchanged by the caller.
template <typename T>
std::optional<std::string> shortest_floating_text(std::string_view value, int notation_digits) {
  if (value.empty() || value.size() > 64 || value == "NaN" ||
      value == "Infinity" || value == "-Infinity") return std::nullopt;
  // Only the server's plain decimal spelling; no whitespace, hex or words.
  if (value.find_first_not_of("0123456789.-+eE") != std::string_view::npos) return std::nullopt;
  const auto& locale = c_numeric_locale();
  if (!locale.handle) return std::nullopt;
  const std::string text(value);
  const int saved_errno = errno;
  char* end = nullptr;
  T parsed{};
#ifdef _WIN32
  if constexpr (sizeof(T) == 4) parsed = _strtof_l(text.c_str(), &end, locale.handle);
  else parsed = _strtod_l(text.c_str(), &end, locale.handle);
#else
  if constexpr (sizeof(T) == 4) parsed = strtof_l(text.c_str(), &end, locale.handle);
  else parsed = strtod_l(text.c_str(), &end, locale.handle);
#endif
  errno = saved_errno;
  if (end != text.c_str() + text.size() || !std::isfinite(parsed)) return std::nullopt;
  // Underflow to zero from a nonzero spelling keeps the server text.
  if (parsed == 0 && text.find_first_of("123456789") < text.find_first_of("eE")) return std::nullopt;
  char buffer[64];
  const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), parsed,
                                       std::chars_format::scientific);
  if (converted.ec != std::errc{}) return std::nullopt;
  std::string_view scientific(buffer, static_cast<std::size_t>(converted.ptr - buffer));
  const bool negative = scientific.front() == '-';
  if (negative) scientific.remove_prefix(1);
  const auto marker = scientific.find('e');
  if (marker == std::string_view::npos) return std::nullopt;
  std::string digits;
  for (const char ch : scientific.substr(0, marker)) {
    if (ch != '.') digits.push_back(ch);
  }
  auto exponent_text = scientific.substr(marker + 1);
  if (!exponent_text.empty() && exponent_text.front() == '+') exponent_text.remove_prefix(1);
  int exponent = 0;
  const auto parsed_exponent = std::from_chars(
      exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent);
  if (parsed_exponent.ec != std::errc{} || digits.empty()) return std::nullopt;
  std::string result = negative ? "-" : "";
  if (exponent < -4 || exponent >= notation_digits) {
    result += digits.front();
    if (digits.size() > 1) result += "." + digits.substr(1);
    const auto magnitude = std::to_string(exponent < 0 ? -exponent : exponent);
    result += exponent < 0 ? "e-" : "e+";
    if (magnitude.size() < 2) result += '0';
    result += magnitude;
  } else if (exponent >= 0) {
    const auto whole = static_cast<std::size_t>(exponent) + 1;
    if (digits.size() <= whole) {
      result += digits + std::string(whole - digits.size(), '0');
    } else {
      result += digits.substr(0, whole) + "." + digits.substr(whole);
    }
  } else {
    result += "0." + std::string(static_cast<std::size_t>(-exponent - 1), '0') + digits;
  }
  return result;
}
} // namespace

NativeTypeInfo PgDatabaseConnection::describe_type(
    std::uint32_t id, std::int16_t size, std::int32_t modifier) const {
  if (catalog_profile_ == PgCatalogProfile::Redshift && id == 6551) {
    // The result family is evidenced; wire size/modifier semantics are not.
    return {ScalarType::LongVarBinary, 0, 0, true};
  }
  if (catalog_profile_ == PgCatalogProfile::Redshift && id == 4000) {
    // SUPER: server-serialized JSON text. The 16 MB object limit is not a
    // per-column character length, so the dimension stays unknown.
    NativeTypeInfo super{ScalarType::LongVarChar, 0, 0, true};
    super.type_name = "super";
    return super;
  }
  if (catalog_profile_ == PgCatalogProfile::Redshift &&
      (id == 3000 || id == 3001 || id == 3999)) {
    // GEOMETRY (3000 raw, 3999 hex) and GEOGRAPHY (3001 hex) are EWKB values.
    // The documented 1,048,447-byte object limit is not a column dimension.
    NativeTypeInfo spatial{ScalarType::LongVarBinary, 0, 0, true};
    spatial.type_name = id == 3001 ? "geography" : "geometry";
    return spatial;
  }
  return GenericDatabaseConnection::describe_type(id, size, modifier);
}

std::optional<std::string> PgDatabaseConnection::normalize_native_result_value(
    std::uint32_t id, ScalarType type, std::string_view value) const {
  if (catalog_profile_ == PgCatalogProfile::Redshift &&
      (type == ScalarType::Double || type == ScalarType::Real)) {
    auto shortest = type == ScalarType::Double
        ? shortest_floating_text<double>(value, 15) : shortest_floating_text<float>(value, 6);
    if (shortest) return shortest;
    return std::string(value);
  }
  if (catalog_profile_ == PgCatalogProfile::Redshift && id == 3000 &&
      type == ScalarType::LongVarBinary) {
    // Raw EWKB in a text-format cell (AWS ODBC rsutil.c, AWS JDBC getBytes).
    // A WKB value starts with its byte-order flag; anything else, including
    // hexadecimal text, is a cell error rather than silently returned bytes.
    if (!value.empty() && value.front() != '\0' && value.front() != '\1') return std::nullopt;
    return std::string(value);
  }
  return normalize_result_value(type, value);
}

std::optional<std::string> PgDatabaseConnection::normalize_result_value(
    ScalarType type, std::string_view value) const {
  if (type == ScalarType::LongVarBinary) {
    if (catalog_profile_ != PgCatalogProfile::Redshift) return std::nullopt;
    return rs::util::decode_hex(value);
  }
  return normalize_pg_result_value(type, value);
}

} // namespace rs::core::database::postgres
