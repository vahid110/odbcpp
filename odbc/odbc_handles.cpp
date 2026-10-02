#include "core/util/hex.h"
#include "core/database/result_validation.h"
#include "odbc_handles.h"
#include "transaction_metadata.h"
#include "connection_string.h"
#include "resource_limits.h"
#include "result_types.h"
#include "core/database/sql_translation.h"
#include "text_data_converter.h"
#include "unicode.h"
#include "core/transport/transport_factory.h"
#include "core/transport/transport_options.h"
#include "core/util/deadline.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace rs::odbc {
using rs::core::database::SqlTranslationError;
namespace {

std::atomic<std::uint64_t> next_connection_id{1};

template <typename T>
T load_application_value(const void* source) {
  T value{};
  std::memcpy(&value, source, sizeof(value));
  return value;
}

template <typename T>
void store_application_value(T* destination, T value) {
  std::memcpy(reinterpret_cast<std::byte*>(destination), &value,
              sizeof(value));
}

template <typename T>
std::string format_floating_parameter(T value) {
  if (std::isnan(value)) return "NaN";
  if (std::isinf(value)) return std::signbit(value) ? "-Infinity" : "Infinity";
  char text[128];
  const auto [end, error] = std::to_chars(
      text, text + sizeof(text), value, std::chars_format::general);
  if (error != std::errc{}) {
    throw std::runtime_error("Floating parameter formatting failed");
  }
  return {text, end};
}

struct DecimalDigits {
  std::size_t whole;
  std::size_t fractional;
};

struct SignedIntegerLimits {
  SQLBIGINT minimum;
  SQLBIGINT maximum;
  int value_bits;
};

std::optional<SignedIntegerLimits> signed_integer_limits(
    SQLSMALLINT sql_type) {
  switch (sql_type) {
    case SQL_TINYINT:
      return SignedIntegerLimits{std::numeric_limits<SQLSCHAR>::min(),
          std::numeric_limits<SQLSCHAR>::max(),
          std::numeric_limits<SQLSCHAR>::digits};
    case SQL_SMALLINT:
      return SignedIntegerLimits{std::numeric_limits<SQLSMALLINT>::min(),
          std::numeric_limits<SQLSMALLINT>::max(),
          std::numeric_limits<SQLSMALLINT>::digits};
    case SQL_INTEGER:
      return SignedIntegerLimits{std::numeric_limits<SQLINTEGER>::min(),
          std::numeric_limits<SQLINTEGER>::max(),
          std::numeric_limits<SQLINTEGER>::digits};
    case SQL_BIGINT:
      return SignedIntegerLimits{std::numeric_limits<SQLBIGINT>::min(),
          std::numeric_limits<SQLBIGINT>::max(),
          std::numeric_limits<SQLBIGINT>::digits};
    default:
      return std::nullopt;
  }
}

// Count significant decimal places without converting through floating point.
// Leave malformed input and non-decimal spellings to the server's diagnostics.
std::optional<DecimalDigits> decimal_digits(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
    text.remove_prefix(1);
  }
  bool any_digit = false;
  bool seen_nonzero_digit = false;
  std::size_t leading_zeroes = 0;
  std::size_t digits_before_point = 0;
  std::size_t total_digits = 0;
  std::size_t last_nonzero_position = 0;
  const auto record_digit = [&](char digit) {
    any_digit = true;
    ++total_digits;
    if (digit != '0') last_nonzero_position = total_digits;
    if (!seen_nonzero_digit) {
      if (digit == '0') ++leading_zeroes;
      else seen_nonzero_digit = true;
    }
  };
  while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
    record_digit(text.front());
    ++digits_before_point;
    text.remove_prefix(1);
  }
  if (!text.empty() && text.front() == '.') {
    text.remove_prefix(1);
    while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
      record_digit(text.front());
      text.remove_prefix(1);
    }
  }
  if (!any_digit) return std::nullopt;

  std::size_t exponent = 0;
  bool negative_exponent = false;
  if (!text.empty() && (text.front() == 'e' || text.front() == 'E')) {
    text.remove_prefix(1);
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
      negative_exponent = text.front() == '-';
      text.remove_prefix(1);
    }
    if (text.empty() || text.front() < '0' || text.front() > '9') {
      return std::nullopt;
    }
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    while (!text.empty() && text.front() >= '0' && text.front() <= '9') {
      const auto digit = static_cast<std::size_t>(text.front() - '0');
      exponent = exponent > (maximum - digit) / 10
          ? maximum : exponent * 10 + digit;
      text.remove_prefix(1);
    }
  }
  if (!text.empty()) return std::nullopt;
  if (!seen_nonzero_digit) return DecimalDigits{0, 0};
  const auto saturating_add = [](std::size_t left, std::size_t right) {
    constexpr auto limit = std::numeric_limits<std::size_t>::max();
    return right > limit - left ? limit : left + right;
  };
  const auto shifted_digits = negative_exponent
      ? (exponent >= digits_before_point ? 0 : digits_before_point - exponent)
      : saturating_add(digits_before_point, exponent);
  const auto fractional_digits = negative_exponent &&
          exponent > digits_before_point
      ? saturating_add(last_nonzero_position,
                       exponent - digits_before_point)
      : (last_nonzero_position > shifted_digits
             ? last_nonzero_position - shifted_digits : 0);
  return DecimalDigits{
      shifted_digits > leading_zeroes ? shifted_digits - leading_zeroes : 0,
      fractional_digits};
}


bool is_character_sql_type(SQLSMALLINT sql_type) {
  switch (sql_type) {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
      return true;
    default:
      return false;
  }
}

bool value_preserving_character_buffer_fits(SQLSMALLINT sql_type,
                                            SQLSMALLINT target_type,
                                            std::string_view value,
                                            SQLLEN buffer_length,
                                            std::size_t offset = 0) {
  const bool integer_type = sql_type == SQL_TINYINT ||
      sql_type == SQL_SMALLINT || sql_type == SQL_INTEGER ||
      sql_type == SQL_BIGINT;
  const bool decimal_type = sql_type == SQL_DECIMAL || sql_type == SQL_NUMERIC;
  const bool approximate_type = sql_type == SQL_REAL || sql_type == SQL_FLOAT ||
      sql_type == SQL_DOUBLE;
  const bool date_type = sql_type == SQL_TYPE_DATE;
  const bool time_type = sql_type == SQL_TYPE_TIME;
  const bool timestamp_type = sql_type == SQL_TYPE_TIMESTAMP;
  if ((!integer_type && !decimal_type && !approximate_type && !date_type &&
       !time_type && !timestamp_type) ||
      (target_type != SQL_C_CHAR && target_type != SQL_C_WCHAR)) {
    return true;
  }
  const auto unit_size = target_type == SQL_C_WCHAR
      ? sizeof(SQLWCHAR) : 1;
  const auto decimal_point = (decimal_type || approximate_type || time_type ||
                             timestamp_type)
      ? value.find('.') : std::string_view::npos;
  // A prefix of scientific notation can change the number's magnitude.
  const auto scientific = approximate_type &&
      value.find_first_of("eE") != std::string_view::npos;
  // A time-zone suffix is also value-bearing, not fractional seconds.
  const auto time_zone = (time_type || timestamp_type) &&
      value.find_first_of("+-", time_type ? 8 : 19) != std::string_view::npos;
  const auto required_length = scientific || date_type || time_zone ||
      decimal_point == std::string_view::npos
      ? value.size() : decimal_point;
  if (offset >= required_length) return true;
  return static_cast<std::size_t>(buffer_length) / unit_size >
      required_length - offset;
}

SQLRETURN convert_character_result_to_binary(std::string_view value,
                                             void* buffer, SQLLEN buffer_length,
                                             SQLLEN* indicator) {
  const auto copy_length = std::min(
      static_cast<std::size_t>(buffer_length), value.size());
  if (copy_length > 0) std::memcpy(buffer, value.data(), copy_length);
  if (indicator) {
    store_application_value(indicator, static_cast<SQLLEN>(value.size()));
  }
  return copy_length < value.size() ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

std::optional<std::string> bit_result_as_text(std::string_view value) {
  if (value == "1") return "1";
  if (value == "0") return "0";
  return std::nullopt;
}

bool bit_uses_decimal_representation(SQLSMALLINT target_type) {
  switch (target_type) {
    case SQL_C_CHAR:
    case SQL_C_WCHAR:
    case SQL_C_STINYINT:
    case SQL_C_UTINYINT:
    case SQL_C_USHORT:
    case SQL_C_ULONG:
    case SQL_C_UBIGINT:
    case SQL_C_SSHORT:
    case SQL_C_SLONG:
    case SQL_C_SBIGINT:
    case SQL_C_FLOAT:
    case SQL_C_DOUBLE:
    case SQL_C_NUMERIC:
      return true;
    default:
      return false;
  }
}

SQLRETURN convert_bit_result_to_binary(std::string_view value, void* buffer,
                                       SQLLEN buffer_length, SQLLEN* indicator,
                                       ConversionIssue* issue) {
  const auto bit = bit_result_as_text(value);
  if (!bit) {
    if (issue) *issue = ConversionIssue::InvalidCharacterValue;
    return SQL_ERROR;
  }
  if (buffer_length < 1) {
    if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
    return SQL_ERROR;
  }
  *static_cast<SQLCHAR*>(buffer) = (*bit)[0] == '1' ? 1 : 0;
  if (indicator) store_application_value(indicator, static_cast<SQLLEN>(1));
  return SQL_SUCCESS;
}

std::string elapsed_milliseconds(std::chrono::steady_clock::time_point start) {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count());
}

std::optional<std::string> format_date_parameter(SQL_DATE_STRUCT date) {
  const auto calendar_date = std::chrono::year_month_day{
      std::chrono::year{date.year}, std::chrono::month{date.month},
      std::chrono::day{date.day}};
  if (date.year < 1 || date.year > 9999 || !calendar_date.ok()) {
    return std::nullopt;
  }
  char iso_date[32]{};
  std::snprintf(iso_date, sizeof(iso_date), "%04d-%02u-%02u",
                date.year, date.month, date.day);
  return std::string(iso_date);
}

std::optional<std::string> format_time_parameter(SQL_TIME_STRUCT time) {
  if (time.hour > 23 || time.minute > 59 || time.second > 61) {
    return std::nullopt;
  }
  char iso_time[32]{};
  std::snprintf(iso_time, sizeof(iso_time), "%02u:%02u:%02u",
                time.hour, time.minute, time.second);
  return std::string(iso_time);
}

std::optional<std::string> current_local_date_parameter() {
  const auto now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  const bool valid = localtime_s(&local, &now) == 0;
#else
  const bool valid = localtime_r(&now, &local) != nullptr;
#endif
  if (!valid || local.tm_year + 1900 < 1 ||
      local.tm_year + 1900 > 9999) {
    return std::nullopt;
  }
  return format_date_parameter(SQL_DATE_STRUCT{
      static_cast<SQLSMALLINT>(local.tm_year + 1900),
      static_cast<SQLUSMALLINT>(local.tm_mon + 1),
      static_cast<SQLUSMALLINT>(local.tm_mday)});
}

const char* invalid_temporal_parameter_state(
    rs::core::database::QueryParameterType target) {
  return target == rs::core::database::QueryParameterType::Text
      ? SQLSTATE_DATETIME_FIELD_OVERFLOW
      : SQLSTATE_INVALID_DATETIME_FORMAT;
}

std::optional<std::uint32_t> timestamp_fractional_quantum(
    SQLSMALLINT precision) {
  if (precision < 0 || precision > 6) return std::nullopt;
  std::uint32_t quantum = 1000000000u;
  for (SQLSMALLINT digit = 0; digit < precision; ++digit) {
    quantum /= 10u;
  }
  return quantum;
}

bool has_temporal_timezone_suffix(const std::string& value) {
  const auto text = ConnectionString::trim(value);
  std::size_t time_start = std::string::npos;
  if (text.size() >= 8 && text[2] == ':' && text[5] == ':') {
    time_start = 0;
  } else if (text.size() >= 19 &&
             (text[10] == ' ' || text[10] == 'T')) {
    time_start = 11;
  }
  return time_start != std::string::npos &&
      text.find_first_of("+-", time_start + 8) != std::string::npos;
}

bool has_temporal_t_separator(const std::string& value) {
  const auto text = ConnectionString::trim(value);
  return text.size() >= 19 && text[10] == 'T';
}

bool parse_ssl(const std::string& value) {
  const auto normalized = ConnectionString::to_upper(ConnectionString::trim(value));
  if (normalized == "1" || normalized == "TRUE" || normalized == "YES" ||
      normalized == "ON") return true;
  if (normalized == "0" || normalized == "FALSE" ||
      normalized == "NO" || normalized == "OFF") return false;
  throw std::invalid_argument(
      "SSL must be true or false (1/0, yes/no, on/off)");
}

void require_no_nul(std::string_view value, std::string_view name) {
  if (value.find('\0') != std::string_view::npos) {
    throw std::invalid_argument(std::string(name) +
                                " contains an embedded NUL byte");
  }
}

std::uint16_t parse_port(std::string_view value) {
  if (value.empty()) {
    throw std::invalid_argument("PORT must be an integer from 1 to 65535");
  }
  std::uint32_t parsed = 0;
  const auto [end, error] = std::from_chars(
      value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
      parsed > std::numeric_limits<std::uint16_t>::max()) {
    throw std::invalid_argument("PORT must be an integer from 1 to 65535");
  }
  return static_cast<std::uint16_t>(parsed);
}

enum class BitNumericLiteral { Zero, One, Fraction, OutOfRange, Invalid };

BitNumericLiteral classify_bit_numeric_literal(std::string_view text) {
  const auto is_blank = [](char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' ||
        ch == '\f' || ch == '\v';
  };
  while (!text.empty() && is_blank(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_blank(text.back())) text.remove_suffix(1);

  std::size_t position = 0;
  bool negative = false;
  if (position < text.size() &&
      (text[position] == '+' || text[position] == '-')) {
    negative = text[position++] == '-';
  }
  const auto is_digit = [](char ch) { return ch >= '0' && ch <= '9'; };
  std::string digits;
  while (position < text.size() && is_digit(text[position])) {
    digits += text[position++];
  }
  const std::size_t whole_digits = digits.size();
  if (position < text.size() && text[position] == '.') {
    ++position;
    while (position < text.size() && is_digit(text[position])) {
      digits += text[position++];
    }
  }
  if (digits.empty()) return BitNumericLiteral::Invalid;

  std::int64_t exponent = 0;
  if (position < text.size() &&
      (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    bool negative_exponent = false;
    if (position < text.size() &&
        (text[position] == '+' || text[position] == '-')) {
      negative_exponent = text[position++] == '-';
    }
    if (position == text.size() || !is_digit(text[position])) {
      return BitNumericLiteral::Invalid;
    }
    while (position < text.size() && is_digit(text[position])) {
      exponent = std::min<std::int64_t>(
          exponent * 10 + (text[position++] - '0'), 1000000000);
    }
    if (negative_exponent) exponent = -exponent;
  }
  if (position != text.size()) return BitNumericLiteral::Invalid;

  const std::size_t first = digits.find_first_not_of('0');
  if (first == std::string::npos) return BitNumericLiteral::Zero;
  if (negative) return BitNumericLiteral::OutOfRange;
  const auto integer_digits = static_cast<std::int64_t>(whole_digits) +
      exponent - static_cast<std::int64_t>(first);
  if (integer_digits <= 0) return BitNumericLiteral::Fraction;
  if (integer_digits > 1 || digits[first] > '1') {
    return BitNumericLiteral::OutOfRange;
  }
  return digits.find_first_not_of('0', first + 1) == std::string::npos
      ? BitNumericLiteral::One : BitNumericLiteral::Fraction;
}

bool is_timeout_error(const std::error_code& error) {
  return error == rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
}

bool is_connection_loss(const std::error_code& error) {
  return error == rs::util::make_error_code(rs::util::DbErrorCode::NetworkError) ||
      error == rs::util::make_error_code(rs::util::DbErrorCode::TLSError) ||
      error == rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError) ||
      error == rs::util::make_error_code(rs::util::DbErrorCode::NotConnected) ||
      error == rs::util::make_error_code(rs::util::DbErrorCode::ConnectionFailed);
}

bool is_recognized_unsupported_connection_attribute(SQLINTEGER attribute) {
  if (attribute == SQL_ATTR_ODBC_CURSORS ||
      attribute == SQL_ATTR_TRACE ||
      attribute == SQL_ATTR_TRACEFILE ||
      attribute == SQL_ATTR_TRANSLATE_LIB ||
      attribute == SQL_ATTR_TRANSLATE_OPTION ||
      attribute == SQL_ATTR_DISCONNECT_BEHAVIOR ||
      attribute == SQL_ATTR_ENLIST_IN_DTC) {
    return true;
  }
#ifdef SQL_ATTR_ENLIST_IN_XA
  if (attribute == SQL_ATTR_ENLIST_IN_XA) return true;
#endif
#ifdef SQL_ATTR_RESET_CONNECTION
  if (attribute == SQL_ATTR_RESET_CONNECTION) return true;
#endif
#ifdef SQL_ATTR_ASYNC_DBC_EVENT
  if (attribute == SQL_ATTR_ASYNC_DBC_EVENT) return true;
#endif
#ifdef SQL_ATTR_ASYNC_DBC_PCALLBACK
  if (attribute == SQL_ATTR_ASYNC_DBC_PCALLBACK) return true;
#endif
#ifdef SQL_ATTR_ASYNC_DBC_PCONTEXT
  if (attribute == SQL_ATTR_ASYNC_DBC_PCONTEXT) return true;
#endif
  return false;
}

bool is_recognized_unsupported_statement_attribute(SQLINTEGER attribute) {
  if (attribute == SQL_ATTR_SIMULATE_CURSOR) {
    return true;
  }
#ifdef SQL_ATTR_ASYNC_STMT_EVENT
  if (attribute == SQL_ATTR_ASYNC_STMT_EVENT) return true;
#endif
#ifdef SQL_ATTR_ASYNC_STMT_PCALLBACK
  if (attribute == SQL_ATTR_ASYNC_STMT_PCALLBACK) return true;
#endif
#ifdef SQL_ATTR_ASYNC_STMT_PCONTEXT
  if (attribute == SQL_ATTR_ASYNC_STMT_PCONTEXT) return true;
#endif
  return false;
}

std::optional<std::string> statement_sql(
    ODBCHandle& handle,
    const rs::core::database::ISqlDialect& dialect,
    std::string_view sql, bool no_scan) {
  if (no_scan) return std::string(sql);
  auto translated = dialect.translate_sql(sql);
  if (translated) return std::move(translated.sql);
  switch (translated.error) {
    case SqlTranslationError::InvalidDatetime:
      handle.set_error(SQLSTATE_INVALID_DATETIME_FORMAT, translated.message);
      break;
    case SqlTranslationError::Unsupported:
      handle.set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                       translated.message);
      break;
    case SqlTranslationError::InvalidSyntax:
      handle.set_error(SQLSTATE_SYNTAX_ERROR, translated.message);
      break;
    case SqlTranslationError::None:
      break;
  }
  return std::nullopt;
}

const char* request_sqlstate(const std::error_code& error,
                             const char* fallback) {
  if (is_timeout_error(error)) return SQLSTATE_TIMEOUT;
  if (error == rs::util::make_error_code(rs::util::DbErrorCode::AllocationFailure)) return "HY001";
  if (error == rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit)) return SQLSTATE_GENERAL_ERROR;
  if (error == rs::util::make_error_code(
                   rs::util::DbErrorCode::UnsupportedFeature)) {
    return SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED;
  }
  if (is_connection_loss(error)) return SQLSTATE_COMMUNICATION_LINK_FAILURE;
  return fallback;
}

std::string mapped_backend_sqlstate(
    const rs::core::database::IBackendProvider& provider,
    std::string_view server_state, const char* fallback,
    SQLINTEGER statement_code = SQL_DIAG_UNKNOWN_STATEMENT) {
  using rs::core::database::ErrorContext;
  auto context = ErrorContext::Unknown;
  switch (statement_code) {
    case SQL_DIAG_CREATE_TABLE: context = ErrorContext::CreateTable; break;
    case SQL_DIAG_CREATE_VIEW: context = ErrorContext::CreateView; break;
    case SQL_DIAG_CREATE_INDEX: context = ErrorContext::CreateIndex; break;
    case SQL_DIAG_DROP_INDEX: context = ErrorContext::DropIndex; break;
    default: break;
  }
  const auto state = provider.normalize_error_sqlstate(server_state, context);
  // Validate the backend contract before exposing a diagnostic to applications.
  if (!state || state->size() != 5 ||
      !std::all_of(state->begin(), state->end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
      })) return fallback;
  return *state;
}

std::string query_failure_sqlstate(
    const rs::core::database::IBackendProvider& provider,
    const rs::core::database::BackendError& error, const char* fallback,
    SQLINTEGER statement_code) {
  if (error.error_class == rs::core::database::BackendErrorClass::InvalidMetadata) {
    return SQLSTATE_GENERAL_ERROR;
  }
  if (error.operation == rs::core::database::BackendOperation::ResolveTypes) {
    return request_sqlstate(error.code, SQLSTATE_GENERAL_ERROR);
  }
  const auto* default_state = request_sqlstate(error.code, fallback);
  if (error.code != rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed)) {
    return default_state;
  }
  return mapped_backend_sqlstate(
      provider, error.native_state.value_or(""), default_state, statement_code);
}

std::chrono::milliseconds timeout_duration(SQLULEN seconds) {
  if (seconds == 0) return std::chrono::milliseconds::max();
  constexpr auto maximum = std::chrono::milliseconds::max().count();
  constexpr auto scale = std::chrono::milliseconds::period::den /
      std::chrono::seconds::period::den;
  if (seconds > static_cast<SQLULEN>(maximum / scale)) {
    return std::chrono::milliseconds::max();
  }
  return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(
      seconds * scale));
}

void set_conversion_diagnostic(ODBCHandle& handle, SQLRETURN result,
                               ConversionIssue issue) {
  if (result == SQL_SUCCESS_WITH_INFO &&
      issue == ConversionIssue::FractionalTruncation) {
    handle.set_error(SQLSTATE_FRACTIONAL_TRUNCATION,
                     "Fractional result digits were truncated");
    return;
  }
  if (result == SQL_SUCCESS_WITH_INFO) {
    handle.set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                     "Result value was truncated to fit the application buffer");
    return;
  }
  if (result != SQL_ERROR) return;

  switch (issue) {
    case ConversionIssue::NumericValueOutOfRange:
      handle.set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                       "Result value is outside the requested numeric range");
      break;
    case ConversionIssue::InvalidDatetimeFormat:
      handle.set_error(SQLSTATE_INVALID_DATETIME_FORMAT,
                       "Result value is not a valid date or time");
      break;
    case ConversionIssue::None:
    case ConversionIssue::FractionalTruncation:
    case ConversionIssue::InvalidCharacterValue:
      handle.set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                       "Result value could not be converted to the requested C type");
      break;
  }
}

struct DynamicFunction {
  std::string name;
  SQLINTEGER code{SQL_DIAG_UNKNOWN_STATEMENT};
};

DynamicFunction completed_dynamic_function(rs::core::database::StatementKind kind) {
  using rs::core::database::StatementKind;
  switch (kind) {
    case StatementKind::SelectCursor: return {"SELECT CURSOR", SQL_DIAG_SELECT_CURSOR};
    case StatementKind::Insert: return {"INSERT", SQL_DIAG_INSERT};
    case StatementKind::UpdateWhere: return {"UPDATE WHERE", SQL_DIAG_UPDATE_WHERE};
    case StatementKind::DeleteWhere: return {"DELETE WHERE", SQL_DIAG_DELETE_WHERE};
    case StatementKind::Call: return {"CALL", SQL_DIAG_CALL};
    case StatementKind::Grant: return {"GRANT", SQL_DIAG_GRANT};
    case StatementKind::Revoke: return {"REVOKE", SQL_DIAG_REVOKE};
    case StatementKind::AlterDomain: return {"ALTER DOMAIN", SQL_DIAG_ALTER_DOMAIN};
    case StatementKind::AlterTable: return {"ALTER TABLE", SQL_DIAG_ALTER_TABLE};
    case StatementKind::CreateAssertion: return {"CREATE ASSERTION", SQL_DIAG_CREATE_ASSERTION};
    case StatementKind::CreateCharacterSet: return {"CREATE CHARACTER SET", SQL_DIAG_CREATE_CHARACTER_SET};
    case StatementKind::CreateCollation: return {"CREATE COLLATION", SQL_DIAG_CREATE_COLLATION};
    case StatementKind::CreateDomain: return {"CREATE DOMAIN", SQL_DIAG_CREATE_DOMAIN};
    case StatementKind::CreateIndex: return {"CREATE INDEX", SQL_DIAG_CREATE_INDEX};
    case StatementKind::CreateSchema: return {"CREATE SCHEMA", SQL_DIAG_CREATE_SCHEMA};
    case StatementKind::CreateTable: return {"CREATE TABLE", SQL_DIAG_CREATE_TABLE};
    case StatementKind::CreateTranslation: return {"CREATE TRANSLATION", SQL_DIAG_CREATE_TRANSLATION};
    case StatementKind::CreateView: return {"CREATE VIEW", SQL_DIAG_CREATE_VIEW};
    case StatementKind::DropAssertion: return {"DROP ASSERTION", SQL_DIAG_DROP_ASSERTION};
    case StatementKind::DropCharacterSet: return {"DROP CHARACTER SET", SQL_DIAG_DROP_CHARACTER_SET};
    case StatementKind::DropCollation: return {"DROP COLLATION", SQL_DIAG_DROP_COLLATION};
    case StatementKind::DropDomain: return {"DROP DOMAIN", SQL_DIAG_DROP_DOMAIN};
    case StatementKind::DropIndex: return {"DROP INDEX", SQL_DIAG_DROP_INDEX};
    case StatementKind::DropSchema: return {"DROP SCHEMA", SQL_DIAG_DROP_SCHEMA};
    case StatementKind::DropTable: return {"DROP TABLE", SQL_DIAG_DROP_TABLE};
    case StatementKind::DropTranslation: return {"DROP TRANSLATION", SQL_DIAG_DROP_TRANSLATION};
    case StatementKind::DropView: return {"DROP VIEW", SQL_DIAG_DROP_VIEW};
    case StatementKind::Unknown: return {};
  }
  return {};
}

DynamicFunction classify_dynamic_function(std::string_view statement) {
  const auto first = statement.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  std::string normalized(statement.substr(first));
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });
  const auto matches = [&](std::string_view prefix) {
    return normalized.starts_with(prefix) &&
        (normalized.size() == prefix.size() ||
         std::isspace(static_cast<unsigned char>(normalized[prefix.size()])));
  };
  struct PrefixMapping {
    std::string_view prefix;
    std::string_view name;
    SQLINTEGER code;
  };
  static constexpr std::array<PrefixMapping, 31> mappings{{
      {"SELECT", "SELECT CURSOR", SQL_DIAG_SELECT_CURSOR},
      {"INSERT", "INSERT", SQL_DIAG_INSERT},
      {"UPDATE", "UPDATE WHERE", SQL_DIAG_UPDATE_WHERE},
      {"DELETE", "DELETE WHERE", SQL_DIAG_DELETE_WHERE},
      {"CALL", "CALL", SQL_DIAG_CALL},
      {"GRANT", "GRANT", SQL_DIAG_GRANT},
      {"REVOKE", "REVOKE", SQL_DIAG_REVOKE},
      {"ALTER DOMAIN", "ALTER DOMAIN", SQL_DIAG_ALTER_DOMAIN},
      {"ALTER TABLE", "ALTER TABLE", SQL_DIAG_ALTER_TABLE},
      {"CREATE ASSERTION", "CREATE ASSERTION", SQL_DIAG_CREATE_ASSERTION},
      {"CREATE CHARACTER SET", "CREATE CHARACTER SET",
       SQL_DIAG_CREATE_CHARACTER_SET},
      {"CREATE COLLATION", "CREATE COLLATION", SQL_DIAG_CREATE_COLLATION},
      {"CREATE DOMAIN", "CREATE DOMAIN", SQL_DIAG_CREATE_DOMAIN},
      {"CREATE INDEX", "CREATE INDEX", SQL_DIAG_CREATE_INDEX},
      {"CREATE UNIQUE INDEX", "CREATE INDEX", SQL_DIAG_CREATE_INDEX},
      {"CREATE SCHEMA", "CREATE SCHEMA", SQL_DIAG_CREATE_SCHEMA},
      {"CREATE TABLE", "CREATE TABLE", SQL_DIAG_CREATE_TABLE},
      {"CREATE TEMP TABLE", "CREATE TABLE", SQL_DIAG_CREATE_TABLE},
      {"CREATE TEMPORARY TABLE", "CREATE TABLE", SQL_DIAG_CREATE_TABLE},
      {"CREATE UNLOGGED TABLE", "CREATE TABLE", SQL_DIAG_CREATE_TABLE},
      {"CREATE TRANSLATION", "CREATE TRANSLATION",
       SQL_DIAG_CREATE_TRANSLATION},
      {"CREATE VIEW", "CREATE VIEW", SQL_DIAG_CREATE_VIEW},
      {"DROP ASSERTION", "DROP ASSERTION", SQL_DIAG_DROP_ASSERTION},
      {"DROP CHARACTER SET", "DROP CHARACTER SET",
       SQL_DIAG_DROP_CHARACTER_SET},
      {"DROP COLLATION", "DROP COLLATION", SQL_DIAG_DROP_COLLATION},
      {"DROP DOMAIN", "DROP DOMAIN", SQL_DIAG_DROP_DOMAIN},
      {"DROP INDEX", "DROP INDEX", SQL_DIAG_DROP_INDEX},
      {"DROP SCHEMA", "DROP SCHEMA", SQL_DIAG_DROP_SCHEMA},
      {"DROP TABLE", "DROP TABLE", SQL_DIAG_DROP_TABLE},
      {"DROP TRANSLATION", "DROP TRANSLATION",
       SQL_DIAG_DROP_TRANSLATION},
      {"DROP VIEW", "DROP VIEW", SQL_DIAG_DROP_VIEW}}};
  for (const auto& mapping : mappings) {
    if (matches(mapping.prefix)) {
      return {std::string(mapping.name), mapping.code};
    }
  }
  return {};
}

rs::core::database::QueryParameterType parameter_type_for(
    SQLSMALLINT parameter_type, SQLSMALLINT value_type) {
  using rs::core::database::QueryParameterType;
  switch (parameter_type) {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
      return QueryParameterType::Text;
    case SQL_TINYINT:
    case SQL_INTEGER:
      return QueryParameterType::Int32;
    case SQL_SMALLINT:
      return QueryParameterType::Int16;
    case SQL_BIGINT:
      return QueryParameterType::Int64;
    case SQL_REAL:
      return QueryParameterType::Float32;
    case SQL_FLOAT:
    case SQL_DOUBLE:
      return QueryParameterType::Float64;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
      return QueryParameterType::Numeric;
    case SQL_BIT:
      return QueryParameterType::Boolean;
    case SQL_TYPE_DATE:
      return QueryParameterType::Date;
    case SQL_TYPE_TIME:
      return QueryParameterType::Time;
    case SQL_TYPE_TIMESTAMP:
      return QueryParameterType::Timestamp;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
      return QueryParameterType::Binary;
    default:
      break;
  }

  switch (value_type) {
    case SQL_C_SLONG: return QueryParameterType::Int32;
    case SQL_C_SBIGINT: return QueryParameterType::Int64;
    case SQL_C_DOUBLE: return QueryParameterType::Float64;
    case SQL_C_BIT: return QueryParameterType::Boolean;
    case SQL_C_BINARY: return QueryParameterType::Binary;
    case SQL_C_CHAR:
    case SQL_C_WCHAR: return QueryParameterType::Text;
    default: return QueryParameterType::Unspecified;
  }
}

struct OdbcTypeInfo {
  SQLSMALLINT sql_type{SQL_VARCHAR};
  SQLULEN column_size{255};
  SQLSMALLINT decimal_digits{0};
};

SQLSMALLINT odbc_scalar_type(rs::core::database::ScalarType type) {
  using rs::core::database::ScalarType;
  SQLSMALLINT sql_type = SQL_VARCHAR;
  switch (type) {
    case ScalarType::Boolean: sql_type = SQL_BIT; break;
    case ScalarType::Binary: sql_type = SQL_VARBINARY; break;
    case ScalarType::Char: sql_type = SQL_CHAR; break;
    case ScalarType::VarChar: sql_type = SQL_VARCHAR; break;
    case ScalarType::BigInt: sql_type = SQL_BIGINT; break;
    case ScalarType::SmallInt: sql_type = SQL_SMALLINT; break;
    case ScalarType::Integer: sql_type = SQL_INTEGER; break;
    case ScalarType::Real: sql_type = SQL_REAL; break;
    case ScalarType::Double: sql_type = SQL_DOUBLE; break;
    case ScalarType::Date: sql_type = SQL_TYPE_DATE; break;
    case ScalarType::Time: sql_type = SQL_TYPE_TIME; break;
    case ScalarType::Timestamp: sql_type = SQL_TYPE_TIMESTAMP; break;
    case ScalarType::Numeric: sql_type = SQL_NUMERIC; break;
    case ScalarType::Decimal: sql_type = SQL_DECIMAL; break;
    case ScalarType::LongVarChar: sql_type = SQL_LONGVARCHAR; break;
  }
  return sql_type;
}

OdbcTypeInfo odbc_type_info(const rs::core::database::NativeTypeInfo& native) {
  return {odbc_scalar_type(native.type), static_cast<SQLULEN>(native.column_size),
          native.decimal_digits};
}

void require_normalized_columns(const rs::core::database::QueryResult& result) {
  if (!rs::core::database::valid_result_structure(result)) {
    throw std::invalid_argument("Data source returned invalid result metadata");
  }
  if (!std::is_sorted(result.cell_errors.begin(), result.cell_errors.end()) ||
      std::adjacent_find(result.cell_errors.begin(), result.cell_errors.end()) != result.cell_errors.end()) {
    throw std::invalid_argument("Data source returned invalid cell error coordinates");
  }
  for (const auto& error : result.cell_errors) {
    if (error.row >= result.rows.size() || error.column >= result.rows[error.row].size() ||
        !result.rows[error.row][error.column]) {
      throw std::invalid_argument("Data source returned invalid cell error coordinates");
    }
  }
  for (const auto& column : result.columns) {
    if (!column.normalized_type) {
      throw std::invalid_argument("Data source returned unnormalized column metadata");
    }
  }
}

ColumnInfo column_info_for(
    const rs::core::database::ResultColumnMetadata& metadata) {
  const auto type = odbc_type_info(*metadata.normalized_type);
  return ColumnInfo{metadata.name, type.sql_type, type.column_size,
                    type.decimal_digits, SQL_NULLABLE_UNKNOWN};
}

SQLSMALLINT descriptor_type_for(SQLSMALLINT concise_type) {
  switch (concise_type) {
    case SQL_TYPE_DATE:
    case SQL_TYPE_TIME:
    case SQL_TYPE_TIMESTAMP:
      return SQL_DATETIME;
    case SQL_INTERVAL_YEAR:
    case SQL_INTERVAL_MONTH:
    case SQL_INTERVAL_DAY:
    case SQL_INTERVAL_HOUR:
    case SQL_INTERVAL_MINUTE:
    case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_YEAR_TO_MONTH:
    case SQL_INTERVAL_DAY_TO_HOUR:
    case SQL_INTERVAL_DAY_TO_MINUTE:
    case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_MINUTE:
    case SQL_INTERVAL_HOUR_TO_SECOND:
    case SQL_INTERVAL_MINUTE_TO_SECOND:
      return SQL_INTERVAL;
    default:
      return concise_type;
  }
}

SQLSMALLINT descriptor_subtype_for(SQLSMALLINT concise_type) {
  switch (concise_type) {
    case SQL_TYPE_DATE: return SQL_CODE_DATE;
    case SQL_TYPE_TIME: return SQL_CODE_TIME;
    case SQL_TYPE_TIMESTAMP: return SQL_CODE_TIMESTAMP;
    case SQL_INTERVAL_YEAR: return SQL_CODE_YEAR;
    case SQL_INTERVAL_MONTH: return SQL_CODE_MONTH;
    case SQL_INTERVAL_DAY: return SQL_CODE_DAY;
    case SQL_INTERVAL_HOUR: return SQL_CODE_HOUR;
    case SQL_INTERVAL_MINUTE: return SQL_CODE_MINUTE;
    case SQL_INTERVAL_SECOND: return SQL_CODE_SECOND;
    case SQL_INTERVAL_YEAR_TO_MONTH: return SQL_CODE_YEAR_TO_MONTH;
    case SQL_INTERVAL_DAY_TO_HOUR: return SQL_CODE_DAY_TO_HOUR;
    case SQL_INTERVAL_DAY_TO_MINUTE: return SQL_CODE_DAY_TO_MINUTE;
    case SQL_INTERVAL_DAY_TO_SECOND: return SQL_CODE_DAY_TO_SECOND;
    case SQL_INTERVAL_HOUR_TO_MINUTE: return SQL_CODE_HOUR_TO_MINUTE;
    case SQL_INTERVAL_HOUR_TO_SECOND: return SQL_CODE_HOUR_TO_SECOND;
    case SQL_INTERVAL_MINUTE_TO_SECOND: return SQL_CODE_MINUTE_TO_SECOND;
    default: return 0;
  }
}

std::optional<SQLSMALLINT> concise_type_for(
    SQLSMALLINT type, SQLSMALLINT subtype) {
  if (type != SQL_DATETIME && type != SQL_INTERVAL) return type;
  if (type == SQL_DATETIME) {
    switch (subtype) {
      case SQL_CODE_DATE: return SQL_TYPE_DATE;
      case SQL_CODE_TIME: return SQL_TYPE_TIME;
      case SQL_CODE_TIMESTAMP: return SQL_TYPE_TIMESTAMP;
      default: return std::nullopt;
    }
  }
  switch (subtype) {
    case SQL_CODE_YEAR: return SQL_INTERVAL_YEAR;
    case SQL_CODE_MONTH: return SQL_INTERVAL_MONTH;
    case SQL_CODE_DAY: return SQL_INTERVAL_DAY;
    case SQL_CODE_HOUR: return SQL_INTERVAL_HOUR;
    case SQL_CODE_MINUTE: return SQL_INTERVAL_MINUTE;
    case SQL_CODE_SECOND: return SQL_INTERVAL_SECOND;
    case SQL_CODE_YEAR_TO_MONTH: return SQL_INTERVAL_YEAR_TO_MONTH;
    case SQL_CODE_DAY_TO_HOUR: return SQL_INTERVAL_DAY_TO_HOUR;
    case SQL_CODE_DAY_TO_MINUTE: return SQL_INTERVAL_DAY_TO_MINUTE;
    case SQL_CODE_DAY_TO_SECOND: return SQL_INTERVAL_DAY_TO_SECOND;
    case SQL_CODE_HOUR_TO_MINUTE: return SQL_INTERVAL_HOUR_TO_MINUTE;
    case SQL_CODE_HOUR_TO_SECOND: return SQL_INTERVAL_HOUR_TO_SECOND;
    case SQL_CODE_MINUTE_TO_SECOND: return SQL_INTERVAL_MINUTE_TO_SECOND;
    default: return std::nullopt;
  }
}

bool valid_descriptor_type(SQLSMALLINT concise_type, DescriptorKind kind) {
  return kind == DescriptorKind::Application
      ? ResultTypes::is_valid_c_type(concise_type)
      : ResultTypes::is_valid_sql_type(concise_type);
}

SQLSMALLINT modern_temporal_type(SQLSMALLINT type) {
  switch (type) {
    case SQL_DATE: return SQL_TYPE_DATE;
    case SQL_TIME: return SQL_TYPE_TIME;
    case SQL_TIMESTAMP: return SQL_TYPE_TIMESTAMP;
    default: return type;
  }
}

const rs::core::database::TypeDefinition* find_type_info(
    std::span<const rs::core::database::TypeDefinition> catalog, SQLSMALLINT type) {
  const auto found = std::find_if(catalog.begin(), catalog.end(),
      [type](const auto& candidate) {
        return odbc_scalar_type(candidate.type) == modern_temporal_type(type);
      });
  return found == catalog.end() ? nullptr : &*found;
}

SQLSMALLINT descriptor_precision(SQLSMALLINT type, SQLULEN length,
                                 SQLSMALLINT scale) {
  switch (type) {
    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
    case SQL_BIGINT:
    case SQL_DECIMAL:
    case SQL_NUMERIC:
      return static_cast<SQLSMALLINT>(std::min<SQLULEN>(
          length, static_cast<SQLULEN>(std::numeric_limits<SQLSMALLINT>::max())));
    case SQL_REAL: return 24;
    case SQL_FLOAT:
    case SQL_DOUBLE: return 53;
    case SQL_TYPE_TIME:
    case SQL_TYPE_TIMESTAMP: return scale;
    default: return 0;
  }
}

SQLLEN bounded_descriptor_length(SQLULEN length, SQLULEN multiplier = 1,
                                 SQLULEN extra = 0) {
  const auto maximum =
      static_cast<SQLULEN>(std::numeric_limits<SQLLEN>::max());
  if (extra > maximum ||
      length > (maximum - extra) / multiplier) {
    return std::numeric_limits<SQLLEN>::max();
  }
  return static_cast<SQLLEN>(length * multiplier + extra);
}

SQLULEN numeric_character_extra(SQLULEN precision, SQLSMALLINT scale) {
  if (scale < 0) {
    return std::max<SQLULEN>(
        2, static_cast<SQLULEN>(1 - static_cast<int>(scale)));
  }
  if (static_cast<SQLULEN>(scale) >= precision) {
    return static_cast<SQLULEN>(scale) - precision + 3;
  }
  return 2;
}

SQLLEN descriptor_octet_length(SQLSMALLINT type, SQLULEN length,
                              SQLSMALLINT scale) {
  switch (type) {
    case SQL_BIT:
    case SQL_TINYINT: return 1;
    case SQL_SMALLINT: return 2;
    case SQL_INTEGER:
    case SQL_REAL: return 4;
    case SQL_BIGINT:
    case SQL_FLOAT:
    case SQL_DOUBLE: return 8;
    case SQL_TYPE_DATE:
    case SQL_TYPE_TIME: return 6;
    case SQL_TYPE_TIMESTAMP: return 16;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
      return length == 0 ? SQL_NO_TOTAL
                         : bounded_descriptor_length(
                               length, 1, numeric_character_extra(
                                   length, scale));
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
      return length == 0 ? SQL_NO_TOTAL : bounded_descriptor_length(length);
    default: return bounded_descriptor_length(length);
  }
}

SQLLEN descriptor_display_size(SQLSMALLINT type, SQLULEN length,
                               SQLSMALLINT scale) {
  switch (type) {
    case SQL_BIT: return 1;
    case SQL_TINYINT: return 4;
    case SQL_SMALLINT: return 6;
    case SQL_INTEGER: return 11;
    case SQL_BIGINT: return 20;
    case SQL_REAL: return 14;
    case SQL_FLOAT:
    case SQL_DOUBLE: return 24;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
      return length == 0 ? SQL_NO_TOTAL
                         : bounded_descriptor_length(
                               length, 1, numeric_character_extra(
                                   length, scale));
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
      return length == 0 ? SQL_NO_TOTAL
                         : bounded_descriptor_length(length, 2);
    case SQL_TYPE_DATE: return 10;
    case SQL_TYPE_TIME: return scale == 0 ? 8 : 9 + scale;
    case SQL_TYPE_TIMESTAMP: return scale == 0 ? 19 : 20 + scale;
    default:
      return length == 0 ? SQL_NO_TOTAL : bounded_descriptor_length(length);
  }
}

void complete_descriptor_record(DescriptorRecord& record,
    std::span<const rs::core::database::TypeDefinition> catalog) {
  const auto* type_info = find_type_info(catalog, record.concise_type);
  record.type = descriptor_type_for(record.concise_type);
  record.datetime_interval_code = descriptor_subtype_for(record.concise_type);
  record.precision = descriptor_precision(
      record.concise_type, record.length, record.scale);
  record.octet_length = descriptor_octet_length(
      record.concise_type, record.length, record.scale);
  record.label = record.name;
  record.unnamed = record.name.empty() ? SQL_UNNAMED : SQL_NAMED;
  record.type_name = type_info ? type_info->name : "";
  record.local_type_name = record.type_name;
  record.literal_prefix = type_info && type_info->literal_prefix
      ? *type_info->literal_prefix : "";
  record.literal_suffix = type_info && type_info->literal_suffix
      ? *type_info->literal_suffix : "";
  record.case_sensitive = type_info ? type_info->case_sensitive : SQL_FALSE;
  record.num_prec_radix = type_info ? type_info->numeric_radix : 0;
  record.unsigned_attribute = type_info && type_info->unsigned_attribute
      ? static_cast<SQLSMALLINT>(*type_info->unsigned_attribute) : SQL_TRUE;
  record.fixed_prec_scale =
      (record.concise_type == SQL_DECIMAL ||
       record.concise_type == SQL_NUMERIC) && record.scale != 0
      ? SQL_TRUE : SQL_FALSE;
  record.display_size = descriptor_display_size(
      record.concise_type, record.length, record.scale);
}

ParameterMetadata parameter_metadata_for(
    const rs::core::database::NativeTypeInfo& native,
    const DescriptorRecord* prior_record) {
  const auto type = odbc_type_info(native);
  ParameterMetadata metadata{type.sql_type, type.column_size,
                             type.decimal_digits, SQL_NULLABLE_UNKNOWN, {}};
  if (!prior_record || prior_record->concise_type != metadata.sql_type) {
    return metadata;
  }
  if ((metadata.sql_type == SQL_DECIMAL ||
       metadata.sql_type == SQL_NUMERIC ||
       metadata.sql_type == SQL_FLOAT ||
       metadata.sql_type == SQL_REAL ||
       metadata.sql_type == SQL_DOUBLE) && prior_record->bound_sql_precision > 0) {
    metadata.column_size = static_cast<SQLULEN>(prior_record->bound_sql_precision);
  } else if (prior_record->bound_sql_length > 0) {
    metadata.column_size = prior_record->bound_sql_length;
  }
  if (metadata.sql_type == SQL_DECIMAL || metadata.sql_type == SQL_NUMERIC ||
      metadata.sql_type == SQL_TYPE_TIME ||
      metadata.sql_type == SQL_TYPE_TIMESTAMP) {
    metadata.decimal_digits = prior_record->bound_sql_scale.value_or(
        metadata.decimal_digits);
  }
  metadata.nullable = prior_record->nullable;
  metadata.name = prior_record->name;
  return metadata;
}

DescriptorRecord descriptor_record_for(const ColumnInfo& column,
    std::span<const rs::core::database::TypeDefinition> catalog) {
  DescriptorRecord record;
  record.concise_type = column.sql_type;
  record.length = column.column_size;
  record.scale = column.decimal_digits;
  record.nullable = column.nullable;
  record.name = column.name;
  complete_descriptor_record(record, catalog);
  return record;
}

DescriptorRecord descriptor_record_for(const ParameterMetadata& parameter,
    std::span<const rs::core::database::TypeDefinition> catalog) {
  DescriptorRecord record;
  record.concise_type = parameter.sql_type;
  record.length = parameter.column_size;
  record.scale = parameter.decimal_digits;
  record.nullable = parameter.nullable;
  record.parameter_type = SQL_PARAM_INPUT;
  record.name = parameter.name;
  complete_descriptor_record(record, catalog);
  return record;
}

rs::core::database::ResultCell type_info_text(std::optional<std::string_view> value) {
  if (!value) return std::nullopt;
  return std::string(*value);
}

rs::core::database::ResultCell type_info_number(long long value) {
  return std::to_string(value);
}

std::vector<std::string> parse_table_types(const std::string& value) {
  std::vector<std::string> types;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto comma = value.find(',', start);
    auto type = ConnectionString::trim(value.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start));
    if (type.size() >= 2 && type.front() == '\'' && type.back() == '\'') {
      type = type.substr(1, type.size() - 2);
    }
    type = ConnectionString::to_upper(type);
    if (type == "TABLE" || type == "VIEW" || type == "SYSTEM TABLE" ||
        type == "FOREIGN TABLE" || type == "LOCAL TEMPORARY") {
      types.push_back(std::move(type));
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return types;
}

template <std::size_t Size>
bool contains_attribute(
    SQLUSMALLINT field_identifier,
    const std::array<SQLUSMALLINT, Size>& attributes) {
  return std::find(attributes.begin(), attributes.end(), field_identifier) !=
      attributes.end();
}

bool is_count_column_attribute(SQLUSMALLINT field_identifier) {
  static constexpr std::array<SQLUSMALLINT, 2> attributes{
      SQL_DESC_COUNT, SQL_COLUMN_COUNT};
  return contains_attribute(field_identifier, attributes);
}

bool is_supported_column_attribute(SQLUSMALLINT field_identifier) {
  static constexpr auto attributes = std::to_array<SQLUSMALLINT>({
      SQL_DESC_COUNT, SQL_COLUMN_COUNT,
      SQL_DESC_AUTO_UNIQUE_VALUE, SQL_COLUMN_AUTO_INCREMENT,
      SQL_DESC_CASE_SENSITIVE, SQL_COLUMN_CASE_SENSITIVE,
      SQL_DESC_DISPLAY_SIZE, SQL_COLUMN_DISPLAY_SIZE,
      SQL_DESC_FIXED_PREC_SCALE, SQL_COLUMN_MONEY,
      SQL_DESC_NAME, SQL_COLUMN_NAME,
      SQL_DESC_LABEL, SQL_COLUMN_LABEL,
      SQL_DESC_LITERAL_PREFIX, SQL_DESC_LITERAL_SUFFIX,
      SQL_DESC_LOCAL_TYPE_NAME,
      SQL_DESC_NUM_PREC_RADIX, SQL_DESC_OCTET_LENGTH,
      SQL_DESC_SEARCHABLE, SQL_COLUMN_SEARCHABLE,
      SQL_DESC_TYPE,
      SQL_DESC_CONCISE_TYPE, SQL_COLUMN_TYPE,
      SQL_DESC_TYPE_NAME, SQL_COLUMN_TYPE_NAME,
      SQL_DESC_LENGTH, SQL_COLUMN_LENGTH,
      SQL_DESC_PRECISION, SQL_COLUMN_PRECISION,
      SQL_DESC_SCALE, SQL_COLUMN_SCALE,
      SQL_DESC_NULLABLE, SQL_COLUMN_NULLABLE,
      SQL_DESC_UNNAMED,
      SQL_DESC_UNSIGNED, SQL_COLUMN_UNSIGNED,
      SQL_DESC_UPDATABLE, SQL_COLUMN_UPDATABLE});
  return contains_attribute(field_identifier, attributes);
}

bool is_known_column_attribute(SQLUSMALLINT field_identifier) {
  static constexpr std::array<SQLUSMALLINT, 47> attributes{
      SQL_DESC_AUTO_UNIQUE_VALUE, SQL_COLUMN_AUTO_INCREMENT,
      SQL_DESC_BASE_COLUMN_NAME,
      SQL_DESC_BASE_TABLE_NAME,
      SQL_DESC_CASE_SENSITIVE, SQL_COLUMN_CASE_SENSITIVE,
      SQL_DESC_CATALOG_NAME, SQL_COLUMN_QUALIFIER_NAME,
      SQL_DESC_CONCISE_TYPE, SQL_COLUMN_TYPE,
      SQL_DESC_COUNT, SQL_COLUMN_COUNT,
      SQL_DESC_DISPLAY_SIZE, SQL_COLUMN_DISPLAY_SIZE,
      SQL_DESC_FIXED_PREC_SCALE, SQL_COLUMN_MONEY,
      SQL_DESC_LABEL, SQL_COLUMN_LABEL,
      SQL_DESC_LENGTH, SQL_COLUMN_LENGTH,
      SQL_DESC_LITERAL_PREFIX,
      SQL_DESC_LITERAL_SUFFIX,
      SQL_DESC_LOCAL_TYPE_NAME,
      SQL_DESC_NAME, SQL_COLUMN_NAME,
      SQL_DESC_NULLABLE, SQL_COLUMN_NULLABLE,
      SQL_DESC_NUM_PREC_RADIX,
      SQL_DESC_OCTET_LENGTH,
      SQL_DESC_PRECISION, SQL_COLUMN_PRECISION,
      SQL_DESC_SCALE, SQL_COLUMN_SCALE,
      SQL_DESC_SCHEMA_NAME, SQL_COLUMN_OWNER_NAME,
      SQL_DESC_SEARCHABLE, SQL_COLUMN_SEARCHABLE,
      SQL_DESC_TABLE_NAME, SQL_COLUMN_TABLE_NAME,
      SQL_DESC_TYPE,
      SQL_DESC_TYPE_NAME, SQL_COLUMN_TYPE_NAME,
      SQL_DESC_UNNAMED,
      SQL_DESC_UNSIGNED, SQL_COLUMN_UNSIGNED,
      SQL_DESC_UPDATABLE, SQL_COLUMN_UPDATABLE};
  return contains_attribute(field_identifier, attributes);
}

} // namespace

bool is_character_column_attribute(SQLUSMALLINT field_identifier) {
  static constexpr std::array<SQLUSMALLINT, 17> attributes{
      SQL_DESC_BASE_COLUMN_NAME,
      SQL_DESC_BASE_TABLE_NAME,
      SQL_DESC_CATALOG_NAME, SQL_COLUMN_QUALIFIER_NAME,
      SQL_DESC_LABEL, SQL_COLUMN_LABEL,
      SQL_DESC_LITERAL_PREFIX,
      SQL_DESC_LITERAL_SUFFIX,
      SQL_DESC_LOCAL_TYPE_NAME,
      SQL_DESC_NAME, SQL_COLUMN_NAME,
      SQL_DESC_SCHEMA_NAME, SQL_COLUMN_OWNER_NAME,
      SQL_DESC_TABLE_NAME, SQL_COLUMN_TABLE_NAME,
      SQL_DESC_TYPE_NAME, SQL_COLUMN_TYPE_NAME};
  return contains_attribute(field_identifier, attributes);
}

ODBCConnection::ODBCConnection(
    ODBCEnvironment*,
    std::shared_ptr<const rs::core::database::IBackendProvider> provider)
    : ODBCHandle(HandleType::Connection),
      backend_provider_(provider ? std::move(provider)
          : std::shared_ptr<const rs::core::database::IBackendProvider>(
                &rs::core::database::configured_backend_provider(),
                [](const rs::core::database::IBackendProvider*) {})),
      connection_id_(next_connection_id.fetch_add(1)) {}

std::span<const rs::core::database::TypeDefinition> ODBCConnection::type_catalog() const {
  return backend_provider_->type_catalog(
      backend_observation_.server_version);
}

rs::core::database::BackendCapabilities ODBCConnection::capabilities() const {
  auto result = backend_provider_->capabilities();
  if (backend_lease_ && !backend_observation_.has_statement_description_facet) {
    result.describe_parameters = false;
  }
  return result;
}

rs::core::database::TransactionCapabilities ODBCConnection::transaction_capabilities() const {
  if (!backend_lease_) return backend_provider_->transaction_capabilities();
  return backend_observation_.transactions;
}

void ODBCConnection::log(
    rs::core::logging::LogLevel level, std::string_view event,
    std::string_view message,
    std::initializer_list<rs::core::logging::LogField> fields) const noexcept {
  if (logger_) logger_->log(level, event, message, fields);
}

bool ODBCConnection::logs_queries() const noexcept {
  return logger_ && logger_->logs_queries();
}

bool ODBCConnection::logging_enabled(
    rs::core::logging::LogLevel level) const noexcept {
  return logger_ && logger_->enabled(level);
}

ODBCStatement::ODBCStatement(std::shared_ptr<ODBCConnection> conn)
    : ODBCHandle(HandleType::Statement), conn_(std::move(conn)) {
  try {
    automatic_app_row_descriptor_ = create_implicit_descriptor(
        DescriptorKind::Application);
    app_row_descriptor_ = automatic_app_row_descriptor_;
    automatic_app_param_descriptor_ = create_implicit_descriptor(
        DescriptorKind::Application);
    app_param_descriptor_ = automatic_app_param_descriptor_;
    imp_row_descriptor_ = create_implicit_descriptor(
        DescriptorKind::ImplementationRow);
    imp_param_descriptor_ = create_implicit_descriptor(
        DescriptorKind::ImplementationParameter);
  } catch (...) {
    for (const auto descriptor : {
             automatic_app_row_descriptor_, automatic_app_param_descriptor_,
             imp_row_descriptor_, imp_param_descriptor_}) {
      if (descriptor) HandleRegistry::instance().unregister_handle(descriptor);
    }
    throw;
  }
}

ODBCStatement::~ODBCStatement() {
  for (const auto descriptor : {
           automatic_app_row_descriptor_, automatic_app_param_descriptor_,
           imp_row_descriptor_, imp_param_descriptor_}) {
    if (descriptor) HandleRegistry::instance().unregister_handle(descriptor);
  }
}

SQLHDESC ODBCStatement::create_implicit_descriptor(DescriptorKind kind) {
  auto descriptor = std::make_unique<ODBCDescriptor>(conn_.get(), true, kind);
  const auto handle = reinterpret_cast<SQLHDESC>(descriptor.get());
  HandleRegistry::instance().register_handle(
      handle, std::move(descriptor), reinterpret_cast<SQLHANDLE>(this));
  return handle;
}

// Connection implementation
SQLRETURN ODBCConnection::connect(
    const std::string& dsn, const std::optional<std::string>& user,
    const std::optional<std::string>& password) {
  const auto started = std::chrono::steady_clock::now();
  if (connected_) {
    set_error(SQLSTATE_CONNECTION_IN_USE, "Connection is already open");
    return SQL_ERROR;
  }
  try {
    require_no_nul(dsn, "Connection string or DSN");
    if (user) require_no_nul(*user, "User name");
    if (password) require_no_nul(*password, "Password");
    const auto resolved = ConnectionString::resolve(
        dsn, std::string(backend_provider_->identity().driver_name));
    const auto logging_options = rs::core::logging::LoggingOptions::resolve(
        resolved.driver_parameters, resolved.dsn_parameters,
        resolved.connection_parameters);
    logger_ = rs::core::logging::DriverLogger::create(
        logging_options, connection_id_);
    const auto& params = resolved.effective_parameters;
    if (!resolved.dsn_name.empty() && resolved.dsn_parameters.empty()) {
      set_error(SQLSTATE_CONNECTION_FAILURE,
                "DSN '" + resolved.dsn_name + "' not found");
      log(rs::core::logging::LogLevel::Error, "connection_failed",
          "Database connection failed", {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public},
                                {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
      return SQL_ERROR;
    }

    rs::core::database::ConnectionOptions options;
    if (params.count("SERVER")) options.host = params.at("SERVER");
    else if (params.count("HOST")) options.host = params.at("HOST");
    if (params.count("PORT")) options.port = parse_port(params.at("PORT"));
    if (params.count("DATABASE")) options.database = params.at("DATABASE");
    else if (params.count("DB")) options.database = params.at("DB");
    else if (!resolved.dsn_name.empty()) options.database = resolved.dsn_name;
    if (requested_catalog_) options.database = *requested_catalog_;
    if (user) options.user = *user;
    else if (params.count("UID")) options.user = params.at("UID");
    else if (params.count("USER")) options.user = params.at("USER");
    if (password) options.password = *password;
    else if (params.count("PWD")) options.password = params.at("PWD");
    else if (params.count("PASSWORD")) options.password = params.at("PASSWORD");
    if (params.count("SSL")) options.use_ssl = parse_ssl(params.at("SSL"));
    if (params.count("SSLCAFILE")) options.ssl_ca_file = params.at("SSLCAFILE");
    if (params.count("SSLCADIR")) options.ssl_ca_dir = params.at("SSLCADIR");
    options.timeout = timeout_duration(login_timeout_seconds_);
    parse_resource_limits(params, options);

    auto resolved_settings =
        backend_provider_->resolve_connection_options(std::move(options));
    if (resolved_settings.has_error()) {
      throw std::invalid_argument(resolved_settings.error_message());
    }
    auto settings = std::move(*resolved_settings);

    const auto transport_options = rs::core::transport::TransportOptions::resolve(
        resolved.driver_parameters, resolved.dsn_parameters,
        resolved.connection_parameters);
    log(rs::core::logging::LogLevel::Info, "connection_start",
        "Opening database connection",
        {{"host", settings.host},
         {"port", std::to_string(settings.port), rs::core::logging::FieldSensitivity::Public},
         {"database", settings.database},
         {"tls", settings.use_ssl ? "true" : "false", rs::core::logging::FieldSensitivity::Public},
         {"transport_mode", std::string(rs::core::transport::to_string(
                                rs::core::transport::TransportFactory::resolve_mode(
                                    transport_options))), rs::core::logging::FieldSensitivity::Public},
         {"async_engine", std::string(rs::core::transport::to_string(
                               transport_options.async_engine)), rs::core::logging::FieldSensitivity::Public},
         {"deadline_model", std::string(rs::core::transport::to_string(
                                 transport_options.deadline_model)), rs::core::logging::FieldSensitivity::Public}});
    auto transport = rs::core::transport::TransportFactory::create(
        transport_options, settings.use_ssl);
    struct AttemptCleanup {
      ODBCConnection& connection;
      std::unique_ptr<rs::core::database::IDatabaseConnection> physical;
      bool completed{false};
      ~AttemptCleanup() {
        if (completed) return;
        if (physical) { try { physical->disconnect(); } catch (...) {} }
        connection.backend_lease_.reset(); connection.backend_owner_.reset();
        connection.backend_observation_ = {};
        connection.connected_ = false; connection.transaction_active_ = false;
      }
    } attempt{*this, backend_provider_->create_session(std::move(transport))};
    if (!attempt.physical) throw std::runtime_error("Backend provider returned no session");

    // One authentication followed by one terminal unbound borrow. No pool,
    // credential rebind, extra health I/O or lifetime/idle defaults are introduced.
    auto result = attempt.physical->connect(settings);
    if (result.has_error()) {
      const auto timeout = is_timeout_error(result.error());
      const auto authentication_failed =
          result.error() == rs::util::make_error_code(
                                rs::util::DbErrorCode::AuthenticationFailed);
      const auto allocation_failed = result.error() == rs::util::make_error_code(
          rs::util::DbErrorCode::AllocationFailure);
      set_error(allocation_failed ? "HY001" : timeout ? SQLSTATE_TIMEOUT
                        : (authentication_failed
                               ? SQLSTATE_INVALID_AUTHORIZATION
                               : SQLSTATE_CONNECTION_FAILURE),
                result.error_message());
      log(rs::core::logging::LogLevel::Error, "connection_failed",
          result.backend_error().safe_summary(),
          {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public},
           {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
      return SQL_ERROR;
    }

    if (result.session_snapshot().state != rs::core::database::SessionState::Idle ||
        result.session_snapshot().disposition != rs::core::database::SessionDisposition::Reusable)
      throw std::runtime_error("Backend authentication did not establish an idle session");
    backend_owner_.emplace(std::move(attempt.physical));
    backend_lease_ = backend_owner_->try_acquire();
    if (!backend_lease_) throw std::runtime_error("Backend session adoption failed");
    auto observation = backend_lease_->inspect();
    if (!observation || !observation->connected ||
        observation->state != rs::core::database::SessionState::Idle ||
        observation.session_snapshot().state != rs::core::database::SessionState::Idle ||
        observation.session_snapshot().disposition != rs::core::database::SessionDisposition::Reusable)
      throw std::runtime_error("Backend session observation failed");
    backend_observation_ = std::move(*observation);

    if (transaction_isolation_ != transaction_isolation_to_odbc(
            transaction_capabilities().default_isolation)) {
      auto isolation_result = backend_isolation(
          *transaction_isolation_from_odbc(transaction_isolation_),
          rs::util::make_deadline(settings.timeout));
      if (isolation_result.has_error()) {
        const auto timeout = is_timeout_error(isolation_result.error());
        set_error(timeout ? SQLSTATE_CONNECTION_TIMEOUT
                          : request_sqlstate(isolation_result.error(),
                                             SQLSTATE_CONNECTION_FAILURE),
                  isolation_result.error_message());
        log(rs::core::logging::LogLevel::Error, "connection_failed",
            isolation_result.backend_error().safe_summary(),
            {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public},
             {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
        return SQL_ERROR;
      }
      if (isolation_result.session_snapshot() != rs::core::database::SessionSnapshot{
              rs::core::database::SessionState::Idle, rs::core::database::SessionDisposition::Reusable})
        throw std::runtime_error("Backend isolation did not preserve an idle session");
      auto final_observation = backend_lease_->inspect();
      if (!final_observation || !final_observation->connected ||
          final_observation->state != rs::core::database::SessionState::Idle ||
          final_observation.session_snapshot() != rs::core::database::SessionSnapshot{
              rs::core::database::SessionState::Idle, rs::core::database::SessionDisposition::Reusable})
        throw std::runtime_error("Backend isolation left an invalid session");
    }
    
    if (!*backend_lease_) throw std::runtime_error("Backend retired during connection setup");
    input_limits_ = settings.input_limits;
    connected_ = true;
    transaction_active_ = false;
    current_catalog_ = settings.database;
    data_source_name_ = resolved.dsn_name;
    server_name_ = settings.host;
    user_name_ = settings.user;
    log(rs::core::logging::LogLevel::Info, "connection_opened",
        "Database connection established",
        {{"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
    attempt.completed = true;
    return SQL_SUCCESS;

  } catch (const std::bad_alloc&) {
    set_error("HY001", "Connection memory allocation failed");
    log(rs::core::logging::LogLevel::Error, "connection_failed", "Database memory allocation failed",
        {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public}});
    return SQL_ERROR;
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    log(rs::core::logging::LogLevel::Error, "connection_failed", "Database connection failed",
        {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public},
         {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
    return SQL_ERROR;
  }
}

SQLRETURN ODBCConnection::set_attribute(SQLINTEGER attribute, SQLULEN value) {
  if (attribute == IODBC_ATTR_APP_WCHAR_TYPE) {
    if (value != NATIVE_SQLWCHAR_ENCODING) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "The requested SQLWCHAR encoding is not native to this "
                "driver build");
      return SQL_ERROR;
    }
    clear_diagnostics();
    return SQL_SUCCESS;
  }
  if (attribute == SQL_ATTR_AUTOCOMMIT) {
    if (value != SQL_AUTOCOMMIT_ON && value != SQL_AUTOCOMMIT_OFF) {
      set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                "Autocommit must be SQL_AUTOCOMMIT_ON or SQL_AUTOCOMMIT_OFF");
      return SQL_ERROR;
    }
    if (autocommit_ == value) return SQL_SUCCESS;
    if (value == SQL_AUTOCOMMIT_OFF && !transaction_capabilities().supported) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Transactions are not supported by this backend");
      return SQL_ERROR;
    }
    if (value == SQL_AUTOCOMMIT_ON && connected_ && transaction_active_) {
      const auto result = end_transaction(SQL_COMMIT);
      if (result != SQL_SUCCESS) return result;
    }
    autocommit_ = static_cast<SQLUINTEGER>(value);
    return SQL_SUCCESS;
  }
  if (attribute == SQL_ATTR_ACCESS_MODE) {
    if (value == SQL_MODE_READ_WRITE) return SQL_SUCCESS;
    if (value == SQL_MODE_READ_ONLY) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Read-only connection mode is not implemented");
      return SQL_ERROR;
    }
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Invalid connection access mode");
    return SQL_ERROR;
  }
  if (attribute == SQL_ATTR_ASYNC_ENABLE) {
    if (value == SQL_ASYNC_ENABLE_OFF) return SQL_SUCCESS;
    if (value == SQL_ASYNC_ENABLE_ON) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Asynchronous ODBC function execution is not implemented");
      return SQL_ERROR;
    }
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Invalid asynchronous execution mode");
    return SQL_ERROR;
  }
  if (attribute == SQL_ATTR_METADATA_ID) {
    if (value == SQL_FALSE) return SQL_SUCCESS;
    if (value == SQL_TRUE) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Metadata identifier semantics are not implemented");
      return SQL_ERROR;
    }
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Invalid metadata identifier mode");
    return SQL_ERROR;
  }
#ifdef SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE
  if (attribute == SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE) {
    if (value == SQL_ASYNC_DBC_ENABLE_OFF) return SQL_SUCCESS;
    if (value == SQL_ASYNC_DBC_ENABLE_ON) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Asynchronous connection functions are not implemented");
      return SQL_ERROR;
    }
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Invalid asynchronous connection mode");
    return SQL_ERROR;
  }
#endif
  if (attribute == SQL_ATTR_QUIET_MODE) {
    quiet_mode_ = reinterpret_cast<SQLHWND>(
        static_cast<std::uintptr_t>(value));
    return SQL_SUCCESS;
  }
  if (attribute == SQL_ATTR_TXN_ISOLATION) {
    const auto isolation = transaction_isolation_from_odbc(value);
    if (!isolation) {
      set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                "Unsupported transaction isolation level");
      return SQL_ERROR;
    }
    if (!transaction_capabilities().supports(*isolation)) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Transaction isolation is not supported by this backend");
      return SQL_ERROR;
    }
    if (transaction_active_) {
      set_error(SQLSTATE_ATTRIBUTE_CANNOT_BE_SET,
                "Transaction isolation cannot change during a transaction");
      return SQL_ERROR;
    }
    if (connected_) {
      auto result = backend_isolation(
          *isolation, rs::util::make_deadline(
                       timeout_duration(connection_timeout_seconds_)));
      if (result.has_error()) {
        const auto timeout = is_timeout_error(result.error());
        set_error(request_sqlstate(result.error(), SQLSTATE_GENERAL_ERROR),
                  result.error_message());
        if (timeout) close_connection();
        return SQL_ERROR;
      }
    }
    transaction_isolation_ = static_cast<SQLUINTEGER>(value);
    return SQL_SUCCESS;
  }
  if (attribute == SQL_ATTR_CONNECTION_TIMEOUT) {
    if (value > std::numeric_limits<SQLUINTEGER>::max()) {
      set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                "Connection timeout is outside the supported range");
      return SQL_ERROR;
    }
    connection_timeout_seconds_ = static_cast<SQLUINTEGER>(value);
    return SQL_SUCCESS;
  }
  if (attribute == SQL_ATTR_PACKET_SIZE) {
    if (connected_) {
      set_error(SQLSTATE_ATTRIBUTE_CANNOT_BE_SET,
                "Packet size cannot be set while connected");
      return SQL_ERROR;
    }
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "PostgreSQL network packet sizing is not configurable");
    return SQL_ERROR;
  }
  if (is_recognized_unsupported_connection_attribute(attribute)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Connection attribute is recognized but not implemented");
    return SQL_ERROR;
  }
  if (attribute != SQL_ATTR_LOGIN_TIMEOUT) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE,
              "Unsupported connection attribute");
    return SQL_ERROR;
  }
  if (connected_) {
    set_error(SQLSTATE_ATTRIBUTE_CANNOT_BE_SET,
              "Login timeout cannot be changed while connected");
    return SQL_ERROR;
  }
  if (value > std::numeric_limits<SQLUINTEGER>::max()) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Login timeout is outside the supported range");
    return SQL_ERROR;
  }
  login_timeout_seconds_ = static_cast<SQLUINTEGER>(value);
  return SQL_SUCCESS;
}

SQLRETURN ODBCConnection::get_attribute(SQLINTEGER attribute,
                                        SQLPOINTER value) {
  const auto write_uinteger = [value](SQLUINTEGER result) {
    std::memcpy(value, &result, sizeof(result));
  };
  switch (attribute) {
    case IODBC_ATTR_APP_WCHAR_TYPE:
      write_uinteger(NATIVE_SQLWCHAR_ENCODING);
      return SQL_SUCCESS;
    case SQL_ATTR_LOGIN_TIMEOUT:
      write_uinteger(login_timeout_seconds_);
      return SQL_SUCCESS;
    case SQL_ATTR_CONNECTION_TIMEOUT:
      write_uinteger(connection_timeout_seconds_);
      return SQL_SUCCESS;
    case SQL_ATTR_PACKET_SIZE:
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "PostgreSQL network packet sizing is not configurable");
      return SQL_ERROR;
    case SQL_ATTR_ACCESS_MODE:
      write_uinteger(SQL_MODE_READ_WRITE);
      return SQL_SUCCESS;
    case SQL_ATTR_ASYNC_ENABLE:
      write_uinteger(SQL_ASYNC_ENABLE_OFF);
      return SQL_SUCCESS;
#ifdef SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE
    case SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE:
      write_uinteger(SQL_ASYNC_DBC_ENABLE_OFF);
      return SQL_SUCCESS;
#endif
    case SQL_ATTR_AUTO_IPD:
      write_uinteger(SQL_FALSE);
      return SQL_SUCCESS;
    case SQL_ATTR_CONNECTION_DEAD:
      if (!connected_) {
        set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
        return SQL_ERROR;
      }
      write_uinteger(backend_connected()
                         ? SQL_CD_FALSE : SQL_CD_TRUE);
      return SQL_SUCCESS;
    case SQL_ATTR_METADATA_ID:
      write_uinteger(SQL_FALSE);
      return SQL_SUCCESS;
    case SQL_ATTR_AUTOCOMMIT:
      write_uinteger(autocommit_);
      return SQL_SUCCESS;
    case SQL_ATTR_TXN_ISOLATION:
      write_uinteger(transaction_isolation_);
      return SQL_SUCCESS;
    case SQL_ATTR_QUIET_MODE:
      std::memcpy(value, &quiet_mode_, sizeof(quiet_mode_));
      return SQL_SUCCESS;
    default:
      if (is_recognized_unsupported_connection_attribute(attribute)) {
        set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                  "Connection attribute is recognized but not implemented");
      } else {
        set_error(SQLSTATE_INVALID_ATTRIBUTE,
                  "Unsupported connection attribute");
      }
      return SQL_ERROR;
  }
}

SQLRETURN ODBCConnection::set_current_catalog(std::string catalog) {
  if (catalog.empty()) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Current catalog cannot be empty");
    return SQL_ERROR;
  }
  if (connected_) {
    if (catalog != current_catalog_) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "PostgreSQL cannot change databases on an open connection");
      return SQL_ERROR;
    }
  }
  requested_catalog_ = std::move(catalog);
  return SQL_SUCCESS;
}

std::string ODBCConnection::get_current_catalog() const {
  if (requested_catalog_) return *requested_catalog_;
  return current_catalog_;
}

rs::core::database::BackendResult<rs::core::database::QueryResult> ODBCConnection::backend_query(
    std::string_view sql, rs::util::Deadline deadline) {
  if (backend_lease_) return backend_lease_->execute_query(sql, deadline);
  rs::core::database::BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::NotConnected), "Connection is not open"};
  error.operation = rs::core::database::BackendOperation::ExecuteDirect;
  error.session_state = rs::core::database::SessionState::Disconnected;
  return error;
}
rs::core::database::BackendResult<rs::core::database::QueryResult> ODBCConnection::backend_prepared(
    std::string_view sql, std::span<const rs::core::database::QueryParameter> params, rs::util::Deadline deadline) {
  if (backend_lease_) return backend_lease_->execute_prepared(sql, params, deadline);
  rs::core::database::BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::NotConnected), "Connection is not open"};
  error.operation = rs::core::database::BackendOperation::ExecutePrepared;
  error.session_state = rs::core::database::SessionState::Disconnected;
  return error;
}
rs::core::database::BackendResult<rs::core::database::QueryResult> ODBCConnection::backend_description(
    std::string_view sql, std::span<const rs::core::database::QueryParameterType> types, rs::util::Deadline deadline) {
  using namespace rs::core::database;
  if (backend_lease_) return backend_lease_->describe_statement(sql, types, deadline);
  return local_backend_error(LocalFailure::Unsupported, "Data source does not support statement description",
      BackendOperation::Describe, SessionState::Disconnected);
}
rs::util::Result<std::string> ODBCConnection::backend_catalog(const rs::core::database::CatalogRequest& request) {
  if (backend_lease_) {
    auto result = backend_lease_->catalog_query(request);
    if (result) return std::move(*result);
    return {result.error(), result.error_message()};
  }
  return {rs::util::DbErrorCode::UnsupportedFeature, "Data source does not support catalog discovery"};
}

rs::core::database::BackendResult<void> ODBCConnection::backend_transaction(
    rs::core::database::TransactionAction action, rs::util::Deadline deadline) {
  using namespace rs::core::database;
  if (backend_lease_) return backend_lease_->transaction(action, deadline);
  return local_backend_error(LocalFailure::Unsupported,
      "Transactions are not supported by this backend", BackendOperation::Transaction,
      SessionState::Disconnected);
}

rs::core::database::BackendResult<void> ODBCConnection::backend_isolation(
    rs::core::database::TransactionIsolation level, rs::util::Deadline deadline) {
  using namespace rs::core::database;
  if (backend_lease_) return backend_lease_->set_transaction_isolation(level, deadline);
  return local_backend_error(LocalFailure::Unsupported,
      "Transaction isolation is not supported by this backend", BackendOperation::SetTransactionIsolation,
      SessionState::Disconnected);
}

rs::core::database::BackendResult<void> ODBCConnection::begin_transaction_if_needed(
    rs::util::Deadline deadline) {
  if (autocommit_ == SQL_AUTOCOMMIT_ON || transaction_active_) {
    return {};
  }
  auto result = backend_transaction(
      rs::core::database::TransactionAction::Begin, deadline);
  if (result.has_error()) {
    return result;
  }
  transaction_active_ = true;
  return {};
}

SQLRETURN ODBCConnection::end_transaction(SQLSMALLINT completion_type) {
  if (completion_type != SQL_COMMIT && completion_type != SQL_ROLLBACK) {
    set_error(SQLSTATE_INVALID_TRANSACTION_OPERATION,
              "Completion type must be SQL_COMMIT or SQL_ROLLBACK");
    return SQL_ERROR;
  }
  if (!connected_) {
    set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
    return SQL_ERROR;
  }
  if (autocommit_ == SQL_AUTOCOMMIT_ON) return SQL_SUCCESS;
  if (!transaction_active_) return SQL_SUCCESS;

  const auto action = completion_type == SQL_COMMIT
      ? rs::core::database::TransactionAction::Commit
      : rs::core::database::TransactionAction::Rollback;
  auto result = backend_transaction(
      action, rs::util::make_deadline(
                   timeout_duration(connection_timeout_seconds_)));
  if (result.has_error()) {
    const auto timeout = is_timeout_error(result.error());
    set_error(request_sqlstate(result.error(), SQLSTATE_GENERAL_ERROR),
              result.error_message());
    if (timeout) close_connection();
    return SQL_ERROR;
  }
  transaction_active_ = false;
  return SQL_SUCCESS;
}

SQLRETURN ODBCConnection::disconnect() {
  if (!connected_) {
    set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
    return SQL_ERROR;
  }
  if (transaction_active_ && backend_connected()) {
    set_error(SQLSTATE_INVALID_TRANSACTION_STATE,
              "An active transaction must be committed or rolled back before disconnecting");
    return SQL_ERROR;
  }
  close_connection();
  return SQL_SUCCESS;
}

bool ODBCConnection::backend_connected() {
  if (!backend_lease_ || !*backend_lease_) return false;
  auto observation = backend_lease_->inspect();
  return observation && observation->connected;
}

void ODBCConnection::close_connection() {
  backend_lease_.reset(); backend_owner_.reset(); backend_observation_ = {};
  connected_ = false; transaction_active_ = false;
  log(rs::core::logging::LogLevel::Info, "connection_closed",
      "Database connection closed");
  if (logger_) logger_->flush();
}

SQLRETURN ODBCDescriptor::get_field(
    SQLSMALLINT record_number, SQLSMALLINT field_identifier,
    SQLPOINTER value, SQLINTEGER buffer_length, SQLINTEGER* string_length) {
  const auto write_value = [value](auto field_value) {
    if (value) {
      store_application_value(
          reinterpret_cast<decltype(field_value)*>(value), field_value);
    }
  };
  switch (field_identifier) {
    case SQL_DESC_ALLOC_TYPE:
      write_value(static_cast<SQLSMALLINT>(automatically_allocated_
          ? SQL_DESC_ALLOC_AUTO : SQL_DESC_ALLOC_USER));
      return SQL_SUCCESS;
    case SQL_DESC_COUNT:
      write_value(static_cast<SQLSMALLINT>(records_.size()));
      return SQL_SUCCESS;
    case SQL_DESC_ARRAY_SIZE:
      write_value(array_size_);
      return SQL_SUCCESS;
    case SQL_DESC_ARRAY_STATUS_PTR:
      write_value(array_status_ptr_);
      return SQL_SUCCESS;
    case SQL_DESC_BIND_OFFSET_PTR:
      write_value(bind_offset_ptr_);
      return SQL_SUCCESS;
    case SQL_DESC_BIND_TYPE:
      write_value(bind_type_);
      return SQL_SUCCESS;
    case SQL_DESC_ROWS_PROCESSED_PTR:
      write_value(rows_processed_ptr_);
      return SQL_SUCCESS;
    default:
      break;
  }

  if (record_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
              "Invalid descriptor record number");
    return SQL_ERROR;
  }
  if (static_cast<std::size_t>(record_number) > records_.size()) {
    return SQL_NO_DATA;
  }
  const auto& record = records_[static_cast<std::size_t>(record_number - 1)];
  const std::string* text = nullptr;
  switch (field_identifier) {
    case SQL_DESC_BASE_COLUMN_NAME: text = &record.base_column_name; break;
    case SQL_DESC_BASE_TABLE_NAME: text = &record.base_table_name; break;
    case SQL_DESC_CATALOG_NAME: text = &record.catalog_name; break;
    case SQL_DESC_LABEL: text = &record.label; break;
    case SQL_DESC_LITERAL_PREFIX: text = &record.literal_prefix; break;
    case SQL_DESC_LITERAL_SUFFIX: text = &record.literal_suffix; break;
    case SQL_DESC_LOCAL_TYPE_NAME: text = &record.local_type_name; break;
    case SQL_DESC_NAME: text = &record.name; break;
    case SQL_DESC_SCHEMA_NAME: text = &record.schema_name; break;
    case SQL_DESC_TABLE_NAME: text = &record.table_name; break;
    case SQL_DESC_TYPE_NAME: text = &record.type_name; break;
    default: break;
  }
  if (text) {
    if (buffer_length < 0) {
      set_error(SQLSTATE_INVALID_STRING_LENGTH,
                "Invalid descriptor output buffer length");
      return SQL_ERROR;
    }
    if (string_length) {
      store_application_value(
          string_length, static_cast<SQLINTEGER>(text->size()));
    }
    if (!value || buffer_length == 0) return SQL_SUCCESS;
    const auto copied = std::min<std::size_t>(
        text->size(), static_cast<std::size_t>(buffer_length - 1));
    std::memcpy(value, text->data(), copied);
    static_cast<char*>(value)[copied] = 0;
    if (copied < text->size()) {
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Descriptor text was truncated");
      return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  }
  switch (field_identifier) {
    case SQL_DESC_TYPE:
      write_value(record.type);
      break;
    case SQL_DESC_CONCISE_TYPE:
      write_value(record.concise_type);
      break;
    case SQL_DESC_DATETIME_INTERVAL_CODE:
      write_value(record.datetime_interval_code);
      break;
    case SQL_DESC_DATETIME_INTERVAL_PRECISION:
      write_value(record.datetime_interval_precision);
      break;
    case SQL_DESC_LENGTH:
      write_value(record.length);
      break;
    case SQL_DESC_PRECISION:
      write_value(record.precision);
      break;
    case SQL_DESC_SCALE:
      write_value(record.scale);
      break;
    case SQL_DESC_NULLABLE:
      write_value(record.nullable);
      break;
    case SQL_DESC_PARAMETER_TYPE:
      write_value(record.parameter_type);
      break;
    case SQL_DESC_DATA_PTR:
      write_value(record.data_ptr);
      break;
    case SQL_DESC_INDICATOR_PTR:
      write_value(record.indicator_ptr);
      break;
    case SQL_DESC_OCTET_LENGTH_PTR:
      write_value(record.octet_length_ptr);
      break;
    case SQL_DESC_OCTET_LENGTH:
      write_value(record.octet_length);
      break;
    case SQL_DESC_AUTO_UNIQUE_VALUE:
      write_value(record.auto_unique_value);
      break;
    case SQL_DESC_CASE_SENSITIVE:
      write_value(record.case_sensitive);
      break;
    case SQL_DESC_DISPLAY_SIZE:
      write_value(record.display_size);
      break;
    case SQL_DESC_FIXED_PREC_SCALE:
      write_value(record.fixed_prec_scale);
      break;
    case SQL_DESC_NUM_PREC_RADIX:
      write_value(record.num_prec_radix);
      break;
    case SQL_DESC_ROWVER:
      write_value(record.rowver);
      break;
    case SQL_DESC_SEARCHABLE:
      write_value(record.searchable);
      break;
    case SQL_DESC_UNNAMED:
      write_value(record.unnamed);
      break;
    case SQL_DESC_UNSIGNED:
      write_value(record.unsigned_attribute);
      break;
    case SQL_DESC_UPDATABLE:
      write_value(record.updatable);
      break;
    default:
      set_error(SQLSTATE_INVALID_DESCRIPTOR_FIELD,
                "Unsupported descriptor field");
      return SQL_ERROR;
  }
  return SQL_SUCCESS;
}

SQLRETURN ODBCDescriptor::set_field(
    SQLSMALLINT record_number, SQLSMALLINT field_identifier,
    SQLPOINTER value, SQLINTEGER buffer_length) {
  if (kind_ == DescriptorKind::ImplementationRow &&
      field_identifier != SQL_DESC_ARRAY_STATUS_PTR &&
      field_identifier != SQL_DESC_ROWS_PROCESSED_PTR) {
    set_error(SQLSTATE_CANNOT_MODIFY_IRD,
              "Implementation row descriptor fields are read-only");
    return SQL_ERROR;
  }
  const auto numeric_signed = static_cast<SQLLEN>(
      reinterpret_cast<std::intptr_t>(value));
  const auto numeric = static_cast<SQLULEN>(
      reinterpret_cast<std::uintptr_t>(value));
  const auto changed = [this]() -> SQLRETURN {
    ++revision_;
    return SQL_SUCCESS;
  };
  switch (field_identifier) {
    case SQL_DESC_ALLOC_TYPE:
      set_error(SQLSTATE_INVALID_DESCRIPTOR_FIELD,
                "Descriptor allocation type is read-only");
      return SQL_ERROR;
    case SQL_DESC_COUNT:
      if (numeric_signed < 0) {
        set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
                  "Descriptor record count cannot be negative");
        return SQL_ERROR;
      }
      if (numeric > static_cast<SQLULEN>(
                        std::numeric_limits<SQLSMALLINT>::max())) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Descriptor record count is out of range");
        return SQL_ERROR;
      }
      records_.resize(static_cast<std::size_t>(numeric));
      return changed();
    case SQL_DESC_ARRAY_SIZE:
      if (kind_ != DescriptorKind::Application) {
        set_error(SQLSTATE_INVALID_DESCRIPTOR_FIELD,
                  "Descriptor array size is not defined for this descriptor");
        return SQL_ERROR;
      }
      if (numeric_signed <= 0) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Descriptor array size must be positive");
        return SQL_ERROR;
      }
      array_size_ = numeric;
      return changed();
    case SQL_DESC_ARRAY_STATUS_PTR:
      array_status_ptr_ = static_cast<SQLUSMALLINT*>(value);
      return changed();
    case SQL_DESC_BIND_OFFSET_PTR:
      if (kind_ != DescriptorKind::Application) {
        set_error(SQLSTATE_INVALID_DESCRIPTOR_FIELD,
                  "Descriptor bind offset is not defined for this descriptor");
        return SQL_ERROR;
      }
      bind_offset_ptr_ = static_cast<SQLLEN*>(value);
      return changed();
    case SQL_DESC_BIND_TYPE:
      if (kind_ != DescriptorKind::Application) {
        set_error(SQLSTATE_INVALID_DESCRIPTOR_FIELD,
                  "Descriptor bind type is not defined for this descriptor");
        return SQL_ERROR;
      }
      bind_type_ = numeric;
      return changed();
    case SQL_DESC_ROWS_PROCESSED_PTR:
      if (kind_ == DescriptorKind::Application) {
        set_error(SQLSTATE_INVALID_DESCRIPTOR_FIELD,
                  "Rows processed is not defined for application descriptors");
        return SQL_ERROR;
      }
      rows_processed_ptr_ = static_cast<SQLULEN*>(value);
      return changed();
    default:
      break;
  }

  if (record_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
              "Invalid descriptor record number");
    return SQL_ERROR;
  }

  const auto writable_for_kind = [this](SQLSMALLINT field) {
    switch (field) {
      case SQL_DESC_TYPE:
      case SQL_DESC_CONCISE_TYPE:
      case SQL_DESC_DATETIME_INTERVAL_CODE:
      case SQL_DESC_DATETIME_INTERVAL_PRECISION:
      case SQL_DESC_LENGTH:
      case SQL_DESC_NUM_PREC_RADIX:
      case SQL_DESC_OCTET_LENGTH:
      case SQL_DESC_PRECISION:
      case SQL_DESC_SCALE:
      case SQL_DESC_DATA_PTR:
        return true;
      case SQL_DESC_INDICATOR_PTR:
      case SQL_DESC_OCTET_LENGTH_PTR:
        return kind_ == DescriptorKind::Application;
      case SQL_DESC_NAME:
      case SQL_DESC_PARAMETER_TYPE:
      case SQL_DESC_UNNAMED:
        return kind_ == DescriptorKind::ImplementationParameter;
      default:
        return false;
    }
  };
  if (!writable_for_kind(field_identifier)) {
    set_error(SQLSTATE_INVALID_DESCRIPTOR_FIELD,
              "Descriptor field is read-only or undefined");
    return SQL_ERROR;
  }
  if (field_identifier == SQL_DESC_OCTET_LENGTH && numeric_signed < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Descriptor buffer length cannot be negative");
    return SQL_ERROR;
  }

  std::optional<SQLSMALLINT> new_concise_type;
  if (field_identifier == SQL_DESC_CONCISE_TYPE) {
    const auto concise = static_cast<SQLSMALLINT>(numeric_signed);
    if (!valid_descriptor_type(concise, kind_)) {
      set_error(SQLSTATE_INCONSISTENT_DESCRIPTOR,
                "Invalid concise descriptor type");
      return SQL_ERROR;
    }
    new_concise_type = concise;
  } else if (field_identifier == SQL_DESC_TYPE) {
    const auto type = static_cast<SQLSMALLINT>(numeric_signed);
    const auto current_subtype =
        static_cast<std::size_t>(record_number) <= records_.size()
        ? records_[static_cast<std::size_t>(record_number - 1)]
              .datetime_interval_code
        : SQLSMALLINT{0};
    new_concise_type = concise_type_for(type, current_subtype);
    if (!new_concise_type ||
        !valid_descriptor_type(*new_concise_type, kind_)) {
      set_error(SQLSTATE_INCONSISTENT_DESCRIPTOR,
                "Descriptor type and subtype are inconsistent");
      return SQL_ERROR;
    }
  } else if (field_identifier == SQL_DESC_PARAMETER_TYPE) {
    const auto parameter_type = static_cast<SQLSMALLINT>(numeric_signed);
    const bool valid = parameter_type == SQL_PARAM_INPUT ||
        parameter_type == SQL_PARAM_INPUT_OUTPUT ||
        parameter_type == SQL_PARAM_OUTPUT
#ifdef SQL_PARAM_INPUT_OUTPUT_STREAM
        || parameter_type == SQL_PARAM_INPUT_OUTPUT_STREAM
#endif
#ifdef SQL_PARAM_OUTPUT_STREAM
        || parameter_type == SQL_PARAM_OUTPUT_STREAM
#endif
        ;
    if (!valid) {
      set_error(SQLSTATE_INVALID_PARAMETER_TYPE,
                "Invalid descriptor parameter type");
      return SQL_ERROR;
    }
  } else if (field_identifier == SQL_DESC_UNNAMED &&
             numeric_signed != SQL_UNNAMED) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE,
              "SQL_DESC_UNNAMED can only be set to SQL_UNNAMED");
    return SQL_ERROR;
  }

  if (static_cast<std::size_t>(record_number) > records_.size()) {
    records_.resize(static_cast<std::size_t>(record_number));
  }
  auto& record = records_[static_cast<std::size_t>(record_number - 1)];
  switch (field_identifier) {
    case SQL_DESC_TYPE:
      record.concise_type = *new_concise_type;
      complete_descriptor_record(record, owner_ ? owner_->type_catalog()
          : std::span<const rs::core::database::TypeDefinition>{});
      break;
    case SQL_DESC_CONCISE_TYPE:
      record.concise_type = *new_concise_type;
      complete_descriptor_record(record, owner_ ? owner_->type_catalog()
          : std::span<const rs::core::database::TypeDefinition>{});
      break;
    case SQL_DESC_DATETIME_INTERVAL_CODE: {
      const auto subtype = static_cast<SQLSMALLINT>(numeric_signed);
      const auto concise = concise_type_for(record.type, subtype);
      if (!concise || !valid_descriptor_type(*concise, kind_)) {
        set_error(SQLSTATE_INCONSISTENT_DESCRIPTOR,
                  "Descriptor type and subtype are inconsistent");
        return SQL_ERROR;
      }
      record.concise_type = *concise;
      complete_descriptor_record(record, owner_ ? owner_->type_catalog()
          : std::span<const rs::core::database::TypeDefinition>{});
      break;
    }
    case SQL_DESC_DATETIME_INTERVAL_PRECISION:
      record.datetime_interval_precision =
          static_cast<SQLINTEGER>(numeric_signed);
      break;
    case SQL_DESC_LENGTH: record.length = numeric; break;
    case SQL_DESC_PRECISION:
      record.precision = static_cast<SQLSMALLINT>(numeric_signed); break;
    case SQL_DESC_SCALE:
      record.scale = static_cast<SQLSMALLINT>(numeric_signed); break;
    case SQL_DESC_NUM_PREC_RADIX:
      record.num_prec_radix = static_cast<SQLINTEGER>(numeric_signed); break;
    case SQL_DESC_PARAMETER_TYPE:
      record.parameter_type = static_cast<SQLSMALLINT>(numeric_signed); break;
    case SQL_DESC_DATA_PTR:
      if (!valid_descriptor_type(record.concise_type, kind_)) {
        set_error(SQLSTATE_INCONSISTENT_DESCRIPTOR,
                  "Descriptor type is invalid for the data pointer");
        return SQL_ERROR;
      }
      // An IPD data pointer only requests a consistency check. It is not a
      // binding pointer and must not be retained or returned.
      record.data_ptr = kind_ == DescriptorKind::Application ? value : nullptr;
      break;
    case SQL_DESC_INDICATOR_PTR:
      record.indicator_ptr = static_cast<SQLLEN*>(value); break;
    case SQL_DESC_OCTET_LENGTH_PTR:
      record.octet_length_ptr = static_cast<SQLLEN*>(value); break;
    case SQL_DESC_OCTET_LENGTH:
      record.octet_length = numeric_signed; break;
    case SQL_DESC_NAME:
      if (!value) {
        set_error(SQLSTATE_INVALID_NULL_POINTER,
                  "Descriptor name pointer is null");
        return SQL_ERROR;
      }
      if (buffer_length == SQL_NTS) {
        record.name = static_cast<const char*>(value);
      } else if (buffer_length >= 0) {
        record.name.assign(static_cast<const char*>(value),
                           static_cast<std::size_t>(buffer_length));
      } else {
        set_error(SQLSTATE_INVALID_STRING_LENGTH,
                  "Invalid descriptor name length");
        return SQL_ERROR;
      }
      record.unnamed = SQL_NAMED;
      break;
    case SQL_DESC_UNNAMED:
      record.unnamed = SQL_UNNAMED;
      record.name.clear();
      break;
    default:
      break;
  }
  // SQL_C_NUMERIC and SQL_NUMERIC share a numeric code, but application
  // descriptors describe a fixed-size C structure rather than SQL text.
  if (kind_ == DescriptorKind::Application && new_concise_type &&
      *new_concise_type == SQL_C_NUMERIC) {
    record.precision = 38;
    record.scale = 0;
    record.octet_length = sizeof(SQL_NUMERIC_STRUCT);
  }
  if (field_identifier != SQL_DESC_DATA_PTR &&
      field_identifier != SQL_DESC_INDICATOR_PTR &&
      field_identifier != SQL_DESC_OCTET_LENGTH_PTR) {
    record.data_ptr = nullptr;
  }
  if (kind_ == DescriptorKind::ImplementationParameter &&
      (new_concise_type ||
       field_identifier == SQL_DESC_DATETIME_INTERVAL_CODE)) {
    record.bound_sql_type = record.concise_type;
  }
  if (kind_ == DescriptorKind::ImplementationParameter &&
      field_identifier == SQL_DESC_LENGTH) {
    record.bound_sql_length = record.length;
  }
  if (kind_ == DescriptorKind::ImplementationParameter &&
      field_identifier == SQL_DESC_PRECISION) {
    record.bound_sql_precision = record.precision;
  }
  if (kind_ == DescriptorKind::ImplementationParameter &&
      field_identifier == SQL_DESC_SCALE) {
    record.bound_sql_scale = record.scale;
  }
  return changed();
}

SQLRETURN ODBCDescriptor::get_record(
    SQLSMALLINT record_number, SQLCHAR* name, SQLSMALLINT buffer_length,
    SQLSMALLINT* string_length, SQLSMALLINT* type, SQLSMALLINT* subtype,
    SQLLEN* length, SQLSMALLINT* precision, SQLSMALLINT* scale,
    SQLSMALLINT* nullable) {
  if (buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Invalid descriptor name buffer length");
    return SQL_ERROR;
  }
  if (record_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
              "Invalid descriptor record number");
    return SQL_ERROR;
  }
  if (static_cast<std::size_t>(record_number) > records_.size()) {
    return SQL_NO_DATA;
  }

  const auto& record = records_[static_cast<std::size_t>(record_number - 1)];
  if (string_length) {
    store_application_value(
        string_length, static_cast<SQLSMALLINT>(std::min<std::size_t>(
            record.name.size(), static_cast<std::size_t>(
                                    std::numeric_limits<SQLSMALLINT>::max()))));
  }
  if (type) store_application_value(type, record.type);
  if (subtype) {
    store_application_value(subtype, record.datetime_interval_code);
  }
  if (length) store_application_value(length, record.octet_length);
  if (precision) store_application_value(precision, record.precision);
  if (scale) store_application_value(scale, record.scale);
  if (nullable) store_application_value(nullable, record.nullable);

  if (!name || buffer_length == 0) return SQL_SUCCESS;
  const auto copied = std::min<std::size_t>(
      record.name.size(), static_cast<std::size_t>(buffer_length - 1));
  std::memcpy(name, record.name.data(), copied);
  name[copied] = 0;
  if (copied < record.name.size()) {
    set_error(SQLSTATE_STRING_DATA_TRUNCATED,
              "Descriptor record name was truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

SQLRETURN ODBCDescriptor::set_record(
    SQLSMALLINT record_number, SQLSMALLINT type, SQLSMALLINT subtype,
    SQLLEN length, SQLSMALLINT precision, SQLSMALLINT scale,
    SQLPOINTER data, SQLLEN* string_length, SQLLEN* indicator) {
  if (kind_ == DescriptorKind::ImplementationRow) {
    set_error(SQLSTATE_CANNOT_MODIFY_IRD,
              "Implementation row descriptor records are read-only");
    return SQL_ERROR;
  }
  if (record_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
              "Invalid descriptor record number");
    return SQL_ERROR;
  }
  if (length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Descriptor buffer length cannot be negative");
    return SQL_ERROR;
  }
  const auto concise_type = concise_type_for(type, subtype);
  if (!concise_type || !valid_descriptor_type(*concise_type, kind_)) {
    set_error(SQLSTATE_INCONSISTENT_DESCRIPTOR,
              "Descriptor type and subtype are inconsistent");
    return SQL_ERROR;
  }

  DescriptorRecord candidate;
  if (static_cast<std::size_t>(record_number) <= records_.size()) {
    candidate = records_[static_cast<std::size_t>(record_number - 1)];
  }
  candidate.concise_type = *concise_type;
  complete_descriptor_record(candidate, owner_ ? owner_->type_catalog()
      : std::span<const rs::core::database::TypeDefinition>{});
  if (kind_ == DescriptorKind::ImplementationParameter) {
    candidate.bound_sql_type = candidate.concise_type;
  }
  candidate.type = type;
  candidate.datetime_interval_code =
      type == SQL_DATETIME || type == SQL_INTERVAL ? subtype : 0;
  candidate.octet_length = length;
  candidate.precision = precision;
  candidate.scale = scale;
  if (kind_ == DescriptorKind::ImplementationParameter) {
    candidate.bound_sql_precision = precision;
    candidate.bound_sql_scale = scale;
  }
  candidate.fixed_prec_scale =
      (candidate.concise_type == SQL_DECIMAL ||
       candidate.concise_type == SQL_NUMERIC) && scale != 0
      ? SQL_TRUE : SQL_FALSE;
  if (kind_ == DescriptorKind::Application) {
    candidate.data_ptr = data;
    candidate.octet_length_ptr = string_length;
    candidate.indicator_ptr = indicator;
  } else {
    candidate.data_ptr = nullptr;
    candidate.octet_length_ptr = nullptr;
    candidate.indicator_ptr = nullptr;
  }

  if (static_cast<std::size_t>(record_number) > records_.size()) {
    records_.resize(static_cast<std::size_t>(record_number));
  }
  records_[static_cast<std::size_t>(record_number - 1)] =
      std::move(candidate);
  ++revision_;
  return SQL_SUCCESS;
}

SQLRETURN ODBCDescriptor::copy_from(const ODBCDescriptor& source) {
  if (kind_ == DescriptorKind::ImplementationRow) {
    set_error(SQLSTATE_CANNOT_MODIFY_IRD,
              "An implementation row descriptor cannot be a copy target");
    return SQL_ERROR;
  }
  records_ = source.records_;
  array_size_ = source.array_size_;
  array_status_ptr_ = source.array_status_ptr_;
  bind_offset_ptr_ = source.bind_offset_ptr_;
  bind_type_ = source.bind_type_;
  rows_processed_ptr_ = source.rows_processed_ptr_;
  ++revision_;
  return SQL_SUCCESS;
}

// Statement implementation
SQLRETURN ODBCStatement::execute_direct(const std::string& sql) {
  const auto started = std::chrono::steady_clock::now();
  const auto dynamic_function = classify_dynamic_function(sql);
  set_statement_diagnostic_header(
      0, 0, dynamic_function.name, dynamic_function.code);
  if (executed_ && (!column_info_.empty() || !pending_results_.empty())) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "Cannot execute while results are pending");
    return SQL_ERROR;
  }
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
               "Database operation failed", {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public},
                                     {"kind", "direct", rs::core::logging::FieldSensitivity::Public}});
    return SQL_ERROR;
  }
  if (sql.find('\0') != std::string::npos) {
    set_error(SQLSTATE_SYNTAX_ERROR,
              "SQL text contains an embedded NUL byte");
    return SQL_ERROR;
  }
  const auto native_sql = statement_sql(
      *this, conn_->sql_dialect(), sql, no_scan_);
  if (!native_sql) return SQL_ERROR;
  if (conn_->logs_queries()) {
    conn_->log(rs::core::logging::LogLevel::Debug, "query_text",
               "Executing direct SQL", {{"sql", *native_sql, rs::core::logging::FieldSensitivity::QueryText}});
  }
  
  clear_current_result();
  pending_results_.clear();
  prepared_ = false;
  prepared_sql_.clear();
  parameter_count_ = 0;
  param_metadata_.clear();
  descriptor(imp_param_descriptor_)->replace_records({});
  try {
    auto deadline = rs::util::make_deadline(
        timeout_duration(query_timeout_seconds_));
    auto transaction = conn_->begin_transaction_if_needed(deadline);
    if (transaction.has_error()) {
      const auto timeout = is_timeout_error(transaction.error());
      set_error(request_sqlstate(transaction.error(), SQLSTATE_GENERAL_ERROR),
                transaction.error_message());
      conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
                 transaction.backend_error().safe_summary(),
                 {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public}, {"kind", "direct", rs::core::logging::FieldSensitivity::Public},
                  {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
      if (timeout) conn_->disconnect();
      return SQL_ERROR;
    }
    auto result = conn_->backend_query(*native_sql,
                                                            deadline);
    
    if (result.has_error()) {
      const auto timeout = is_timeout_error(result.error());
      set_error(query_failure_sqlstate(conn_->backend_provider(),
                                       result.backend_error(), SQLSTATE_SYNTAX_ERROR,
                                       dynamic_function.code),
                result.error_message());
      conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
                 result.backend_error().safe_summary(),
                 {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public}, {"kind", "direct", rs::core::logging::FieldSensitivity::Public},
                  {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
      if (timeout) conn_->disconnect();
      return SQL_ERROR;
    }

    const auto row_count = result->rows.size();
    const auto affected_rows = result->affected_rows;
    apply_query_result(std::move(*result), false);
    conn_->log(rs::core::logging::LogLevel::Info, "query_completed",
               "Direct SQL execution completed",
               {{"kind", "direct", rs::core::logging::FieldSensitivity::Public},
                {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public},
                {"rows", std::to_string(row_count), rs::core::logging::FieldSensitivity::Public},
                {"affected_rows", std::to_string(affected_rows), rs::core::logging::FieldSensitivity::Public}});
    return SQL_SUCCESS;
    
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed", "Database operation failed",
               {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public}, {"kind", "direct", rs::core::logging::FieldSensitivity::Public},
                {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
    return SQL_ERROR;
  }
}

SQLRETURN ODBCStatement::set_attribute(SQLINTEGER attribute, SQLPOINTER value) {
  const auto numeric = static_cast<SQLULEN>(
      reinterpret_cast<std::uintptr_t>(value));
  const auto numeric_signed = static_cast<SQLLEN>(
      reinterpret_cast<std::intptr_t>(value));
  const auto cursor_attribute_settable = [this]() {
    if (executed_ && !column_info_.empty()) {
      set_error(SQLSTATE_INVALID_CURSOR_STATE,
                "Cursor attribute cannot be changed while the cursor is open");
      return false;
    }
    if (prepared_) {
      set_error(SQLSTATE_ATTRIBUTE_CANNOT_BE_SET,
                "Cursor attribute cannot be changed after preparation");
      return false;
    }
    return true;
  };
  switch (attribute) {
    case SQL_ATTR_APP_ROW_DESC:
    case SQL_ATTR_APP_PARAM_DESC:
      return set_application_descriptor(
          attribute, static_cast<SQLHDESC>(value));
    case SQL_ATTR_IMP_ROW_DESC:
    case SQL_ATTR_IMP_PARAM_DESC:
      set_error(SQLSTATE_INVALID_AUTO_DESCRIPTOR_USE,
                "Implementation descriptors are read-only");
      return SQL_ERROR;
    case SQL_ATTR_QUERY_TIMEOUT:
      query_timeout_seconds_ = numeric;
      return SQL_SUCCESS;
    case SQL_ATTR_MAX_ROWS:
      max_rows_ = numeric;
      return SQL_SUCCESS;
    case SQL_ATTR_NOSCAN:
      if (numeric == SQL_NOSCAN_OFF || numeric == SQL_NOSCAN_ON) {
        no_scan_ = numeric == SQL_NOSCAN_ON;
        return SQL_SUCCESS;
      }
      set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                "Invalid escape-scanning mode");
      return SQL_ERROR;
    case SQL_ATTR_CURSOR_TYPE:
      if (!cursor_attribute_settable()) return SQL_ERROR;
      if (numeric == SQL_CURSOR_FORWARD_ONLY) return SQL_SUCCESS;
      if (numeric != SQL_CURSOR_KEYSET_DRIVEN &&
          numeric != SQL_CURSOR_DYNAMIC && numeric != SQL_CURSOR_STATIC) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid cursor type");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_CONCURRENCY:
      if (!cursor_attribute_settable()) return SQL_ERROR;
      if (numeric == SQL_CONCUR_READ_ONLY) return SQL_SUCCESS;
      if (numeric != SQL_CONCUR_LOCK && numeric != SQL_CONCUR_ROWVER &&
          numeric != SQL_CONCUR_VALUES) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid cursor concurrency");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_CURSOR_SCROLLABLE:
      if (numeric == SQL_NONSCROLLABLE) return SQL_SUCCESS;
      if (numeric != SQL_SCROLLABLE) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid cursor scrollability");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_CURSOR_SENSITIVITY:
      if (numeric == SQL_UNSPECIFIED) return SQL_SUCCESS;
      if (numeric != SQL_INSENSITIVE && numeric != SQL_SENSITIVE) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid cursor sensitivity");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_ENABLE_AUTO_IPD:
      if (numeric == SQL_FALSE) return SQL_SUCCESS;
      if (numeric != SQL_TRUE) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid automatic IPD value");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_FETCH_BOOKMARK_PTR:
      fetch_bookmark_ptr_ = static_cast<SQLLEN*>(value);
      return SQL_SUCCESS;
    case SQL_ATTR_KEYSET_SIZE:
      if (numeric == 0) return SQL_SUCCESS;
      break;
    case SQL_ATTR_MAX_LENGTH:
      if (numeric == 0) return SQL_SUCCESS;
      break;
    case SQL_ATTR_ROW_ARRAY_SIZE:
      if (numeric_signed <= 0) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Row array size must be positive");
        return SQL_ERROR;
      }
      if (numeric == 1) {
        return descriptor(app_row_descriptor_)->set_field(
            0, SQL_DESC_ARRAY_SIZE, value, 0);
      }
      break;
    case SQL_ATTR_ROW_BIND_TYPE:
      if (numeric == SQL_BIND_BY_COLUMN) {
        return descriptor(app_row_descriptor_)->set_field(
            0, SQL_DESC_BIND_TYPE, value, 0);
      }
      break;
    case SQL_ATTR_ROW_BIND_OFFSET_PTR:
      return descriptor(app_row_descriptor_)->set_field(
          0, SQL_DESC_BIND_OFFSET_PTR, value, 0);
    case SQL_ATTR_RETRIEVE_DATA:
      if (numeric == SQL_RD_ON) return SQL_SUCCESS;
      if (numeric != SQL_RD_OFF) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid retrieve-data value");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_USE_BOOKMARKS:
      if (!cursor_attribute_settable()) return SQL_ERROR;
      if (numeric == SQL_UB_OFF) return SQL_SUCCESS;
      if (numeric != SQL_UB_FIXED && numeric != SQL_UB_VARIABLE) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid bookmark mode");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_ASYNC_ENABLE:
      if (numeric == SQL_ASYNC_ENABLE_OFF) return SQL_SUCCESS;
      if (numeric != SQL_ASYNC_ENABLE_ON) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid asynchronous execution value");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_PARAMSET_SIZE:
      if (numeric_signed <= 0) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Parameter-set size must be positive");
        return SQL_ERROR;
      }
      if (numeric == 1) {
        return descriptor(app_param_descriptor_)->set_field(
            0, SQL_DESC_ARRAY_SIZE, value, 0);
      }
      break;
    case SQL_ATTR_PARAM_BIND_TYPE:
      if (numeric == SQL_PARAM_BIND_BY_COLUMN) {
        return descriptor(app_param_descriptor_)->set_field(
            0, SQL_DESC_BIND_TYPE, value, 0);
      }
      break;
    case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
      return descriptor(app_param_descriptor_)->set_field(
          0, SQL_DESC_BIND_OFFSET_PTR, value, 0);
    case SQL_ATTR_PARAM_OPERATION_PTR:
      return descriptor(app_param_descriptor_)->set_field(
          0, SQL_DESC_ARRAY_STATUS_PTR, value, 0);
    case SQL_ATTR_METADATA_ID:
      if (numeric == SQL_FALSE) return SQL_SUCCESS;
      if (numeric != SQL_TRUE) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Invalid metadata identifier value");
        return SQL_ERROR;
      }
      break;
    case SQL_ATTR_ROW_NUMBER:
      set_error(SQLSTATE_INVALID_ATTRIBUTE,
                "Current row number is read-only");
      return SQL_ERROR;
    case SQL_ATTR_ROW_OPERATION_PTR:
      return descriptor(app_row_descriptor_)->set_field(
          0, SQL_DESC_ARRAY_STATUS_PTR, value, 0);
    case SQL_ATTR_ROW_STATUS_PTR:
      return descriptor(imp_row_descriptor_)->set_field(
          0, SQL_DESC_ARRAY_STATUS_PTR, value, 0);
    case SQL_ATTR_ROWS_FETCHED_PTR:
      return descriptor(imp_row_descriptor_)->set_field(
          0, SQL_DESC_ROWS_PROCESSED_PTR, value, 0);
    case SQL_ATTR_PARAM_STATUS_PTR:
      return descriptor(imp_param_descriptor_)->set_field(
          0, SQL_DESC_ARRAY_STATUS_PTR, value, 0);
    case SQL_ATTR_PARAMS_PROCESSED_PTR:
      return descriptor(imp_param_descriptor_)->set_field(
          0, SQL_DESC_ROWS_PROCESSED_PTR, value, 0);
    default:
      if (is_recognized_unsupported_statement_attribute(attribute)) {
        set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                  "Statement attribute is recognized but not implemented");
      } else {
        set_error(SQLSTATE_INVALID_ATTRIBUTE,
                  "Unsupported statement attribute");
      }
      return SQL_ERROR;
  }
  set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
            "Requested statement attribute value is not supported");
  return SQL_ERROR;
}

SQLRETURN ODBCStatement::set_application_descriptor(
    SQLINTEGER attribute, SQLHDESC descriptor) {
  auto& active = attribute == SQL_ATTR_APP_ROW_DESC
      ? app_row_descriptor_ : app_param_descriptor_;
  const auto automatic = attribute == SQL_ATTR_APP_ROW_DESC
      ? automatic_app_row_descriptor_ : automatic_app_param_descriptor_;
  if (!descriptor || descriptor == automatic) {
    active = automatic;
    return SQL_SUCCESS;
  }

  auto candidate = HandleRegistry::instance().get_handle_as<ODBCDescriptor>(
      descriptor);
  if (!candidate) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Application descriptor handle is invalid");
    return SQL_ERROR;
  }
  if (candidate->is_automatically_allocated()) {
    set_error(SQLSTATE_INVALID_AUTO_DESCRIPTOR_USE,
              "A different implicit descriptor cannot be associated");
    return SQL_ERROR;
  }
  auto owner = HandleRegistry::instance().get_connection_for_handle(
      descriptor);
  if (!owner || owner.get() != conn_.get()) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Application descriptor belongs to another connection");
    return SQL_ERROR;
  }
  active = descriptor;
  return SQL_SUCCESS;
}

void ODBCStatement::detach_descriptor(SQLHDESC descriptor) noexcept {
  if (app_row_descriptor_ == descriptor) {
    app_row_descriptor_ = automatic_app_row_descriptor_;
  }
  if (app_param_descriptor_ == descriptor) {
    app_param_descriptor_ = automatic_app_param_descriptor_;
  }
}

std::shared_ptr<ODBCDescriptor> ODBCStatement::descriptor(
    SQLHDESC handle) const {
  return HandleRegistry::instance().get_handle_as<ODBCDescriptor>(handle);
}

SQLRETURN ODBCStatement::get_attribute(SQLINTEGER attribute, SQLPOINTER value) {
  const auto write_ulen = [value](SQLULEN result) {
    std::memcpy(value, &result, sizeof(result));
  };
  const auto write_pointer = [value](auto result) {
    std::memcpy(value, &result, sizeof(result));
  };
  switch (attribute) {
    case SQL_ATTR_APP_ROW_DESC:
      write_pointer(app_row_descriptor_);
      break;
    case SQL_ATTR_APP_PARAM_DESC:
      write_pointer(app_param_descriptor_);
      break;
    case SQL_ATTR_IMP_ROW_DESC:
      write_pointer(imp_row_descriptor_);
      break;
    case SQL_ATTR_IMP_PARAM_DESC:
      write_pointer(imp_param_descriptor_);
      break;
    case SQL_ATTR_QUERY_TIMEOUT:
      write_ulen(query_timeout_seconds_); break;
    case SQL_ATTR_MAX_ROWS:
      write_ulen(max_rows_); break;
    case SQL_ATTR_NOSCAN:
      write_ulen(no_scan_ ? SQL_NOSCAN_ON : SQL_NOSCAN_OFF); break;
    case SQL_ATTR_CURSOR_TYPE:
      write_ulen(SQL_CURSOR_FORWARD_ONLY); break;
    case SQL_ATTR_CONCURRENCY:
      write_ulen(SQL_CONCUR_READ_ONLY); break;
    case SQL_ATTR_CURSOR_SCROLLABLE:
      write_ulen(SQL_NONSCROLLABLE); break;
    case SQL_ATTR_CURSOR_SENSITIVITY:
      write_ulen(SQL_UNSPECIFIED); break;
    case SQL_ATTR_ENABLE_AUTO_IPD:
      write_ulen(SQL_FALSE); break;
    case SQL_ATTR_FETCH_BOOKMARK_PTR:
      write_pointer(fetch_bookmark_ptr_); break;
    case SQL_ATTR_KEYSET_SIZE:
    case SQL_ATTR_MAX_LENGTH:
      write_ulen(0); break;
    case SQL_ATTR_ROW_ARRAY_SIZE:
      write_ulen(descriptor(app_row_descriptor_)->array_size()); break;
    case SQL_ATTR_ROW_BIND_TYPE:
      write_ulen(descriptor(app_row_descriptor_)->bind_type()); break;
    case SQL_ATTR_ROW_BIND_OFFSET_PTR:
      write_pointer(descriptor(app_row_descriptor_)->bind_offset_ptr()); break;
    case SQL_ATTR_RETRIEVE_DATA:
      write_ulen(SQL_RD_ON); break;
    case SQL_ATTR_USE_BOOKMARKS:
      write_ulen(SQL_UB_OFF); break;
    case SQL_ATTR_ASYNC_ENABLE:
      write_ulen(SQL_ASYNC_ENABLE_OFF); break;
    case SQL_ATTR_PARAMSET_SIZE:
      write_ulen(descriptor(app_param_descriptor_)->array_size()); break;
    case SQL_ATTR_PARAM_BIND_TYPE:
      write_ulen(descriptor(app_param_descriptor_)->bind_type()); break;
    case SQL_ATTR_PARAM_BIND_OFFSET_PTR:
      write_pointer(descriptor(app_param_descriptor_)->bind_offset_ptr()); break;
    case SQL_ATTR_PARAM_OPERATION_PTR:
      write_pointer(descriptor(app_param_descriptor_)->array_status_ptr()); break;
    case SQL_ATTR_METADATA_ID:
      write_ulen(SQL_FALSE); break;
    case SQL_ATTR_ROW_NUMBER:
      if (!executed_ || column_info_.empty() || !row_positioned_) {
        set_error(SQLSTATE_INVALID_CURSOR_STATE,
                  "Cursor is not positioned on a row");
        return SQL_ERROR;
      }
      write_ulen(static_cast<SQLULEN>(current_row_));
      break;
    case SQL_ATTR_ROW_OPERATION_PTR:
      write_pointer(descriptor(app_row_descriptor_)->array_status_ptr()); break;
    case SQL_ATTR_ROW_STATUS_PTR:
      write_pointer(descriptor(imp_row_descriptor_)->array_status_ptr()); break;
    case SQL_ATTR_ROWS_FETCHED_PTR:
      write_pointer(descriptor(imp_row_descriptor_)->rows_processed_ptr()); break;
    case SQL_ATTR_PARAM_STATUS_PTR:
      write_pointer(descriptor(imp_param_descriptor_)->array_status_ptr()); break;
    case SQL_ATTR_PARAMS_PROCESSED_PTR:
      write_pointer(descriptor(imp_param_descriptor_)->rows_processed_ptr()); break;
    default:
      if (is_recognized_unsupported_statement_attribute(attribute)) {
        set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                  "Statement attribute is recognized but not implemented");
      } else {
        set_error(SQLSTATE_INVALID_ATTRIBUTE,
                  "Unsupported statement attribute");
      }
      return SQL_ERROR;
  }
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::close_cursor(bool report_missing_cursor) {
  const bool cursor_open = executed_ && !column_info_.empty();
  if (!cursor_open && report_missing_cursor) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE, "No cursor is open");
    return SQL_ERROR;
  }
  clear_current_result();
  pending_results_.clear();
  return SQL_SUCCESS;
}

void ODBCStatement::unbind_columns() {
  descriptor(app_row_descriptor_)->set_field(
      0, SQL_DESC_COUNT, nullptr, 0);
}

void ODBCStatement::reset_parameters() {
  descriptor(app_param_descriptor_)->set_field(
      0, SQL_DESC_COUNT, nullptr, 0);
}

SQLRETURN ODBCStatement::fetch() {
  if (!executed_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
              "Statement has not been executed");
    return SQL_ERROR;
  }
  if (column_info_.empty()) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "Executed statement did not produce a result set");
    return SQL_ERROR;
  }
  const auto application_descriptor = descriptor(app_row_descriptor_);
  if (application_descriptor->array_size() != 1 ||
      application_descriptor->bind_type() != SQL_BIND_BY_COLUMN ||
      application_descriptor->bind_offset_ptr()) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Only single-row column-wise descriptor binding is supported");
    return SQL_ERROR;
  }
  const auto implementation_descriptor = descriptor(imp_row_descriptor_);
  auto* rows_fetched = implementation_descriptor->rows_processed_ptr();
  auto* row_status = implementation_descriptor->array_status_ptr();
  if (current_row_ >= result_rows_.size()) {
    row_positioned_ = false;
    if (rows_fetched) {
      store_application_value(rows_fetched, static_cast<SQLULEN>(0));
    }
    if (row_status) {
      store_application_value(
          row_status, static_cast<SQLUSMALLINT>(SQL_ROW_NOROW));
    }
    return SQL_NO_DATA;
  }
  
  current_row_++;
  row_positioned_ = true;
  if (rows_fetched) {
    store_application_value(rows_fetched, static_cast<SQLULEN>(1));
  }
  get_data_column_ = 0;
  get_data_offset_ = 0;
  get_data_target_type_ = 0;
  SQLRETURN fetch_result = SQL_SUCCESS;
  
  // Auto-populate bound columns from ARD
  const auto& row = result_rows_[current_row_ - 1];
  for (size_t i = 0;
       i < application_descriptor->record_count() && i < row.size(); ++i) {
    const auto& binding = *application_descriptor->record(i);
    if (binding.data_ptr) {
      const auto& cell = row[i];
      const SQLSMALLINT sql_type = i < column_info_.size()
          ? column_info_[i].sql_type : static_cast<SQLSMALLINT>(SQL_VARCHAR);
      const SQLSMALLINT target_type = binding.concise_type == SQL_C_DEFAULT
          ? ResultTypes::default_c_type(sql_type) : binding.concise_type;
      if (!ResultTypes::is_conversion_supported(sql_type, target_type)) {
        set_error(SQLSTATE_RESTRICTED_DATA_TYPE,
                  "Unsupported result data type conversion");
        if (row_status) {
          store_application_value(
              row_status, static_cast<SQLUSMALLINT>(SQL_ROW_ERROR));
        }
        return SQL_ERROR;
      }

      if (!cell) {
        if (!binding.indicator_ptr) {
          set_error(SQLSTATE_INDICATOR_VARIABLE_REQUIRED,
                    "NULL column requires an indicator variable");
          if (row_status) {
            store_application_value(
                row_status, static_cast<SQLUSMALLINT>(SQL_ROW_ERROR));
          }
          return SQL_ERROR;
        }
        store_application_value(
            binding.indicator_ptr, static_cast<SQLLEN>(SQL_NULL_DATA));
        continue;
      }

      if (std::binary_search(result_cell_errors_.begin(), result_cell_errors_.end(),
              rs::core::database::CellEncodingError{current_row_ - 1, i})) {
        set_error(SQLSTATE_INVALID_CHARACTER_VALUE, "Invalid backend result encoding");
        if (row_status) store_application_value(row_status, static_cast<SQLUSMALLINT>(SQL_ROW_ERROR));
        return SQL_ERROR;
      }
      const auto& value = *cell;

      const bool binary_as_text =
          (sql_type == SQL_BINARY || sql_type == SQL_VARBINARY ||
           sql_type == SQL_LONGVARBINARY) &&
          (target_type == SQL_C_CHAR || target_type == SQL_C_WCHAR);
      const bool bit_as_text = sql_type == SQL_BIT &&
          bit_uses_decimal_representation(target_type);
      std::optional<std::string> formatted_text;
      if (binary_as_text) {
        formatted_text = rs::util::encode_hex(value);
      } else if (bit_as_text) {
        formatted_text = bit_result_as_text(value);
        if (!formatted_text) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid normalized boolean result");
          if (row_status) {
            store_application_value(
                row_status, static_cast<SQLUSMALLINT>(SQL_ROW_ERROR));
          }
          return SQL_ERROR;
        }
      }
      const auto& conversion_value = formatted_text ? *formatted_text : value;
      SQLLEN conversion_length = binding.octet_length;
      if (!value_preserving_character_buffer_fits(
              sql_type, target_type, conversion_value, conversion_length)) {
        set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                  "Result value does not fit in the character buffer");
        if (row_status) {
          store_application_value(
              row_status, static_cast<SQLUSMALLINT>(SQL_ROW_ERROR));
        }
        return SQL_ERROR;
      }
      const auto unit_size = target_type == SQL_C_WCHAR
          ? sizeof(SQLWCHAR) : 1;
      if (bit_as_text &&
          (target_type == SQL_C_CHAR || target_type == SQL_C_WCHAR) &&
          conversion_length <
              static_cast<SQLLEN>(2 * unit_size)) {
        set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                  "Bit character result does not fit in the application buffer");
        if (row_status) {
          store_application_value(
              row_status, static_cast<SQLUSMALLINT>(SQL_ROW_ERROR));
        }
        return SQL_ERROR;
      }
      if (binary_as_text && conversion_length > 0) {
        const auto units = static_cast<std::size_t>(conversion_length) /
            unit_size;
        if (units > 1) {
          const auto capacity = units - 1;
          if (capacity < conversion_value.size() && capacity % 2 != 0) {
            conversion_length = static_cast<SQLLEN>(
                (units - 1) * unit_size);
          }
        }
      }
      ConversionIssue conversion_issue = ConversionIssue::None;
      auto* output_length = binding.octet_length_ptr
          ? binding.octet_length_ptr : binding.indicator_ptr;
      SQLRETURN conv_result;
      if (sql_type == SQL_BIT && target_type == SQL_C_BINARY) {
        conv_result = convert_bit_result_to_binary(
            value, binding.data_ptr, conversion_length, output_length,
            &conversion_issue);
      } else if (is_character_sql_type(sql_type) &&
                 target_type == SQL_C_BINARY) {
        conv_result = convert_character_result_to_binary(
            value, binding.data_ptr, conversion_length, output_length);
      } else {
        conv_result = TextDataConverter::convert_data(
            conversion_value, target_type, binding.data_ptr,
            conversion_length, output_length, &conversion_issue,
            binding.precision, binding.scale);
      }

      if (conv_result == SQL_ERROR) {
        set_conversion_diagnostic(*this, conv_result, conversion_issue);
        if (row_status) {
          store_application_value(
              row_status, static_cast<SQLUSMALLINT>(SQL_ROW_ERROR));
        }
        return SQL_ERROR;
      }
      if (binding.indicator_ptr &&
          binding.indicator_ptr != binding.octet_length_ptr) {
        store_application_value(binding.indicator_ptr, static_cast<SQLLEN>(0));
      }
      if (conv_result == SQL_SUCCESS_WITH_INFO) {
        set_conversion_diagnostic(*this, conv_result, conversion_issue);
        fetch_result = SQL_SUCCESS_WITH_INFO;
      }
    }
  }

  if (row_status) {
    store_application_value(
        row_status, static_cast<SQLUSMALLINT>(
                        fetch_result == SQL_SUCCESS_WITH_INFO
                            ? SQL_ROW_SUCCESS_WITH_INFO : SQL_ROW_SUCCESS));
  }
  return fetch_result;
}

SQLRETURN ODBCStatement::more_results() {
  clear_current_result();
  if (pending_results_.empty()) return SQL_NO_DATA;

  auto next = std::move(pending_results_.front());
  pending_results_.erase(pending_results_.begin());
  if (next.error) {
    pending_results_.clear();
    set_error(query_failure_sqlstate(conn_->backend_provider(),
                                     *next.error, SQLSTATE_SYNTAX_ERROR,
                                     SQL_DIAG_UNKNOWN_STATEMENT),
              next.error->message);
    return SQL_ERROR;
  }
  apply_query_result(std::move(next), false);
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::get_data(SQLUSMALLINT col, SQLSMALLINT target_type,
                                 void* buffer, SQLLEN buffer_length,
                                 SQLLEN* indicator) {
  if (!executed_ || !row_positioned_ || current_row_ == 0 ||
      current_row_ > result_rows_.size()) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE, "No current row");
    return SQL_ERROR;
  }
  
  const auto& row = result_rows_[current_row_ - 1];
  if (col < 1 || col > row.size()) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid column number");
    return SQL_ERROR;
  }

  if (target_type != SQL_ARD_TYPE &&
      !ResultTypes::is_valid_c_type(target_type)) {
    set_error(SQLSTATE_INVALID_APPLICATION_BUFFER_TYPE,
              "Invalid SQLGetData target type");
    return SQL_ERROR;
  }

  const SQLSMALLINT sql_type = col <= column_info_.size()
      ? column_info_[col - 1].sql_type : static_cast<SQLSMALLINT>(SQL_VARCHAR);
  SQLSMALLINT effective_target_type = target_type == SQL_C_DEFAULT
      ? ResultTypes::default_c_type(sql_type) : target_type;
  if (target_type == SQL_ARD_TYPE) {
    const auto application_descriptor = descriptor(app_row_descriptor_);
    const auto* record = application_descriptor->record(col - 1);
    if (!record || !ResultTypes::is_valid_c_type(record->concise_type)) {
      set_error(SQLSTATE_INVALID_APPLICATION_BUFFER_TYPE,
                "ARD does not define a valid target type for the column");
      return SQL_ERROR;
    }
    effective_target_type = record->concise_type == SQL_C_DEFAULT
        ? ResultTypes::default_c_type(sql_type) : record->concise_type;
  }
  if (!ResultTypes::is_supported_c_type(effective_target_type)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "SQLGetData target type is not supported");
    return SQL_ERROR;
  }

  constexpr auto complete = std::numeric_limits<std::size_t>::max();
  auto offset = get_data_column_ == col ? get_data_offset_ : 0;
  if (offset == complete) return SQL_NO_DATA;
  if (get_data_column_ == col && offset != 0 &&
      get_data_target_type_ != effective_target_type) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
              "SQLGetData target type changed during chunked retrieval");
    return SQL_ERROR;
  }

  if (!buffer) {
    set_error(SQLSTATE_INVALID_NULL_POINTER, "Null result buffer");
    return SQL_ERROR;
  }
  if (buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH, "Invalid result buffer length");
    return SQL_ERROR;
  }
  const auto save_offset = [&](std::size_t value) {
    get_data_column_ = col;
    get_data_offset_ = value;
    get_data_target_type_ = effective_target_type;
  };

  const auto& cell = row[col - 1];
  if (!ResultTypes::is_conversion_supported(sql_type, effective_target_type)) {
    set_error(SQLSTATE_RESTRICTED_DATA_TYPE,
              "Unsupported result data type conversion");
    return SQL_ERROR;
  }
  if (!cell) {
    if (!indicator) {
      set_error(SQLSTATE_INDICATOR_VARIABLE_REQUIRED,
                "NULL column requires an indicator variable");
      return SQL_ERROR;
    }
    store_application_value(indicator, static_cast<SQLLEN>(SQL_NULL_DATA));
    save_offset(complete);
    return SQL_SUCCESS;
  }
  
  if (std::binary_search(result_cell_errors_.begin(), result_cell_errors_.end(),
          rs::core::database::CellEncodingError{current_row_ - 1, static_cast<std::size_t>(col - 1)})) {
    set_error(SQLSTATE_INVALID_CHARACTER_VALUE, "Invalid backend result encoding");
    return SQL_ERROR;
  }
  const auto& value = *cell;

  const bool binary_as_text =
      (sql_type == SQL_BINARY || sql_type == SQL_VARBINARY ||
       sql_type == SQL_LONGVARBINARY) &&
      (effective_target_type == SQL_C_CHAR ||
       effective_target_type == SQL_C_WCHAR);
  const bool bit_as_text = sql_type == SQL_BIT &&
      bit_uses_decimal_representation(effective_target_type);
  std::optional<std::string> formatted_text;
  if (binary_as_text) {
    formatted_text = rs::util::encode_hex(value);
  } else if (bit_as_text) {
    formatted_text = bit_result_as_text(value);
    if (!formatted_text) {
      set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                "Invalid normalized boolean result");
      return SQL_ERROR;
    }
    if (effective_target_type == SQL_C_CHAR ||
        effective_target_type == SQL_C_WCHAR) {
      const auto required_bytes = effective_target_type == SQL_C_WCHAR
          ? 2 * sizeof(SQLWCHAR) : 2;
      if (buffer_length < static_cast<SQLLEN>(required_bytes)) {
        set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                  "Bit character result does not fit in the application buffer");
        return SQL_ERROR;
      }
    }
  }
  const auto& character_cell = formatted_text ? *formatted_text : value;
  if (effective_target_type == SQL_C_NUMERIC &&
      (sql_type == SQL_REAL || sql_type == SQL_FLOAT ||
       sql_type == SQL_DOUBLE) &&
      (character_cell == "NaN" || character_cell == "Infinity" ||
       character_cell == "-Infinity")) {
    set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
              "Non-finite numeric result cannot be represented as SQL_C_NUMERIC");
    return SQL_ERROR;
  }
  if (!value_preserving_character_buffer_fits(
          sql_type, effective_target_type, character_cell, buffer_length,
          offset)) {
    set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
              "Result value does not fit in the character buffer");
    return SQL_ERROR;
  }
  if (effective_target_type == SQL_C_CHAR) {
    if (offset > character_cell.size()) {
      set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
                "SQLGetData target type changed during chunked retrieval");
      return SQL_ERROR;
    }
    const auto remaining = character_cell.size() - offset;
    if (indicator) {
      store_application_value(indicator, static_cast<SQLLEN>(remaining));
    }
    const auto capacity = buffer_length > 0
        ? static_cast<std::size_t>(buffer_length - 1) : 0;
    auto copy_length = std::min(capacity, remaining);
    if (binary_as_text && copy_length < remaining) {
      copy_length -= copy_length % 2;
    }
    if (copy_length > 0) {
      std::memcpy(buffer, character_cell.data() + offset, copy_length);
    }
    if (buffer_length > 0) {
      static_cast<char*>(buffer)[copy_length] = '\0';
    }
    offset += copy_length;
    if (offset < character_cell.size() || buffer_length == 0) {
      save_offset(offset);
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Result value was truncated to fit the application buffer");
      return SQL_SUCCESS_WITH_INFO;
    }
    save_offset(complete);
    return SQL_SUCCESS;
  }

  if (effective_target_type == SQL_C_WCHAR) {
    const auto wide = utf8_to_wide(character_cell);
    if (!wide) {
      set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                "Result value is not valid UTF-8");
      return SQL_ERROR;
    }
    if (offset > wide->size()) {
      set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
                "SQLGetData target type changed during chunked retrieval");
      return SQL_ERROR;
    }

    const auto remaining = wide->size() - offset;
    if (indicator) {
      store_application_value(
          indicator, static_cast<SQLLEN>(remaining * sizeof(SQLWCHAR)));
    }
    const auto buffer_units = buffer_length > 0
        ? static_cast<std::size_t>(buffer_length) / sizeof(SQLWCHAR) : 0;
    const auto capacity = buffer_units > 0 ? buffer_units - 1 : 0;
    auto copy_length = std::min(capacity, remaining);
    if (binary_as_text && copy_length < remaining) {
      copy_length -= copy_length % 2;
    }
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (copy_length < remaining && copy_length > 0 &&
          (*wide)[offset + copy_length - 1] >= 0xd800 &&
          (*wide)[offset + copy_length - 1] <= 0xdbff) {
        --copy_length;
      }
    }
    const auto copied_bytes = copy_length * sizeof(SQLWCHAR);
    if (copied_bytes > 0) {
      std::memcpy(buffer, wide->data() + offset, copied_bytes);
    }
    if (buffer_units > 0) {
      const SQLWCHAR terminator = 0;
      std::memcpy(static_cast<unsigned char*>(buffer) + copied_bytes,
                  &terminator, sizeof(terminator));
    }
    offset += copy_length;
    if (offset < wide->size() || buffer_units == 0) {
      save_offset(offset);
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Result value was truncated to fit the application buffer");
      return SQL_SUCCESS_WITH_INFO;
    }
    save_offset(complete);
    return SQL_SUCCESS;
  }

  if (effective_target_type == SQL_C_BINARY) {
    if (sql_type == SQL_BIT) {
      ConversionIssue issue = ConversionIssue::None;
      const auto result = convert_bit_result_to_binary(
          value, buffer, buffer_length, indicator, &issue);
      set_conversion_diagnostic(*this, result, issue);
      if (result == SQL_SUCCESS) save_offset(complete);
      return result;
    }
    const std::span<const std::byte> source(
        reinterpret_cast<const std::byte*>(value.data()), value.size());
    if (offset > source.size()) {
      set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
                "SQLGetData target type changed during chunked retrieval");
      return SQL_ERROR;
    }
    const auto remaining = source.size() - offset;
    if (indicator) {
      store_application_value(indicator, static_cast<SQLLEN>(remaining));
    }
    const auto capacity = static_cast<std::size_t>(buffer_length);
    const auto copy_length = std::min(capacity, remaining);
    if (copy_length > 0) {
      std::memcpy(buffer, source.data() + offset, copy_length);
    }
    offset += copy_length;
    if (offset < source.size()) {
      save_offset(offset);
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Binary result value was truncated to fit the application buffer");
      return SQL_SUCCESS_WITH_INFO;
    }
    save_offset(complete);
    return SQL_SUCCESS;
  }

  ConversionIssue conversion_issue = ConversionIssue::None;
  SQLSMALLINT numeric_precision = 38;
  SQLSMALLINT numeric_scale = 0;
  if (effective_target_type == SQL_C_NUMERIC &&
      target_type == SQL_ARD_TYPE) {
    const auto* record = descriptor(app_row_descriptor_)->record(col - 1);
    numeric_precision = record->precision;
    numeric_scale = record->scale;
  }
  SQLRETURN result = TextDataConverter::convert_data(
      character_cell, effective_target_type, buffer, buffer_length, indicator,
      &conversion_issue, numeric_precision, numeric_scale);

  set_conversion_diagnostic(*this, result, conversion_issue);
  if (result != SQL_ERROR) save_offset(complete);
  
  return result;
}

// Prepared statement implementation
SQLRETURN ODBCStatement::prepare(const std::string& sql) {
  if (executed_ && (!column_info_.empty() || !pending_results_.empty())) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "Cannot prepare while results are pending");
    return SQL_ERROR;
  }
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    return SQL_ERROR;
  }
  if (sql.find('\0') != std::string::npos) {
    set_error(SQLSTATE_SYNTAX_ERROR,
              "SQL text contains an embedded NUL byte");
    return SQL_ERROR;
  }
  const auto native_sql = statement_sql(
      *this, conn_->sql_dialect(), sql, no_scan_);
  if (!native_sql) return SQL_ERROR;
  
  const auto marker_count =
      conn_->sql_dialect().count_parameter_markers(*native_sql);
  if (marker_count > conn_->input_limits().max_parameters ||
      marker_count > static_cast<std::size_t>(
          std::numeric_limits<SQLSMALLINT>::max())) {
    set_error(SQLSTATE_GENERAL_ERROR, "Too many parameter markers");
    return SQL_ERROR;
  }
  prepared_sql_ = *native_sql;
  parameter_count_ = static_cast<SQLSMALLINT>(marker_count);
  param_metadata_.clear();
  clear_current_result();
  pending_results_.clear();
  prepared_ = true;
  if (conn_->logs_queries()) {
    conn_->log(rs::core::logging::LogLevel::Debug, "query_prepared",
               "Prepared SQL statement",
               {{"sql", *native_sql, rs::core::logging::FieldSensitivity::QueryText},
                {"parameters", std::to_string(marker_count), rs::core::logging::FieldSensitivity::Public}});
  }
  
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::num_params(SQLSMALLINT* parameter_count) {
  if (!parameter_count) {
    set_error(SQLSTATE_INVALID_NULL_POINTER,
              "Parameter count output pointer is null");
    return SQL_ERROR;
  }
  if (!prepared_ && !executed_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
              "Statement has not been prepared or executed");
    return SQL_ERROR;
  }
  if (prepared_ && !executed_) {
    const auto metadata_result = describe_prepared_metadata();
    if (metadata_result != SQL_SUCCESS) return metadata_result;
  }
  store_application_value(
      parameter_count,
      static_cast<SQLSMALLINT>(prepared_ ? parameter_count_ : 0));
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::complete_parameter_set(SQLRETURN result) {
  if (parameter_count_ == 0) return result;
  const auto implementation_descriptor = descriptor(imp_param_descriptor_);
  auto* params_processed = implementation_descriptor->rows_processed_ptr();
  auto* param_status = implementation_descriptor->array_status_ptr();
  if (params_processed) {
    store_application_value(params_processed, static_cast<SQLULEN>(1));
  }
  if (param_status) {
    SQLUSMALLINT status = SQL_PARAM_ERROR;
    if (result == SQL_SUCCESS) {
      status = SQL_PARAM_SUCCESS;
    } else if (result == SQL_SUCCESS_WITH_INFO) {
      status = SQL_PARAM_SUCCESS_WITH_INFO;
    }
    store_application_value(param_status, status);
  }
  return result;
}

SQLRETURN ODBCStatement::execute() {
  const auto started = std::chrono::steady_clock::now();
  const auto dynamic_function = classify_dynamic_function(prepared_sql_);
  set_statement_diagnostic_header(
      0, 0, dynamic_function.name, dynamic_function.code);
  if (!prepared_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR, "Statement not prepared");
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
               "Database operation failed", {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public},
                                     {"kind", "prepared", rs::core::logging::FieldSensitivity::Public}});
    return SQL_ERROR;
  }
  if (executed_ && (!column_info_.empty() || !pending_results_.empty())) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "Cannot re-execute while results are pending");
    return SQL_ERROR;
  }
  
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
               "Database operation failed", {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public},
                                     {"kind", "prepared", rs::core::logging::FieldSensitivity::Public}});
    return SQL_ERROR;
  }
  const auto application_descriptor = descriptor(app_param_descriptor_);
  if (application_descriptor->array_size() != 1 ||
      application_descriptor->bind_type() != SQL_PARAM_BIND_BY_COLUMN ||
      application_descriptor->bind_offset_ptr() ||
      application_descriptor->array_status_ptr()) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Only one column-wise parameter set is supported");
    return SQL_ERROR;
  }
  if (parameter_count_ > 0) {
    const auto implementation_descriptor = descriptor(imp_param_descriptor_);
    if (auto* processed = implementation_descriptor->rows_processed_ptr()) {
      store_application_value(processed, static_cast<SQLULEN>(0));
    }
    if (auto* status = implementation_descriptor->array_status_ptr()) {
      store_application_value(status,
                              static_cast<SQLUSMALLINT>(SQL_PARAM_UNUSED));
    }
  }
  if (conn_->logs_queries()) {
    conn_->log(rs::core::logging::LogLevel::Debug, "query_text",
               "Executing prepared SQL",
               {{"sql", prepared_sql_, rs::core::logging::FieldSensitivity::QueryText},
                {"parameters", std::to_string(parameter_count_), rs::core::logging::FieldSensitivity::Public}});
  }
  
  clear_current_result();
  pending_results_.clear();
  try {
    const auto implementation_descriptor = descriptor(imp_param_descriptor_);
    if (application_descriptor->record_count() <
            static_cast<std::size_t>(parameter_count_) ||
        implementation_descriptor->record_count() <
            static_cast<std::size_t>(parameter_count_)) {
      set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
                "Not all statement parameters are bound");
      return complete_parameter_set(SQL_ERROR);
    }
    const auto& input_limits = conn_->input_limits();
    if (static_cast<std::size_t>(parameter_count_) > input_limits.max_parameters) {
      set_error(SQLSTATE_GENERAL_ERROR, "Parameter count exceeds configured limit");
      return complete_parameter_set(SQL_ERROR);
    }
    std::size_t parameter_bytes = 0;
    const auto reject_parameter_limit = [&]() {
      set_error(SQLSTATE_GENERAL_ERROR, "Parameter bytes exceed configured limit");
      return complete_parameter_set(SQL_ERROR);
    };
    std::vector<rs::core::database::QueryParameter> param_values;
    param_values.reserve(static_cast<std::size_t>(parameter_count_));
    
    for (SQLSMALLINT index = 0; index < parameter_count_; ++index) {
      const auto& application = *application_descriptor->record(
          static_cast<std::size_t>(index));
      const auto& implementation = *implementation_descriptor->record(
          static_cast<std::size_t>(index));
      const auto* length_or_indicator = application.octet_length_ptr
          ? application.octet_length_ptr : application.indicator_ptr;
      const bool is_null = application.indicator_ptr &&
          load_application_value<SQLLEN>(application.indicator_ptr) ==
              SQL_NULL_DATA;
      if (!application.data_ptr && !is_null) {
        set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
                  "Not all statement parameters are bound");
        return complete_parameter_set(SQL_ERROR);
      }
      if (implementation.parameter_type != SQL_PARAM_INPUT) {
        set_error(SQLSTATE_GENERAL_ERROR,
                  "Only input parameters are currently supported");
        return complete_parameter_set(SQL_ERROR);
      }

      SQLSMALLINT value_type = application.concise_type;
      const auto declared_sql_type = implementation.bound_sql_type != 0
          ? implementation.bound_sql_type : implementation.concise_type;
      const auto declared_sql_length = implementation.bound_sql_type != 0
          ? implementation.bound_sql_length : implementation.length;
      const auto declared_sql_precision =
          implementation.bound_sql_precision != 0
          ? implementation.bound_sql_precision : implementation.precision;
      const auto declared_sql_scale =
          implementation.bound_sql_scale.value_or(implementation.scale);
      if (value_type == SQL_C_DEFAULT) {
        value_type = ResultTypes::default_c_type(declared_sql_type);
      }
      rs::core::database::QueryParameter query_param;
      query_param.type = parameter_type_for(
          implementation.concise_type, value_type);
      if (is_null) {
        query_param.value = std::nullopt;
        param_values.push_back(std::move(query_param));
        continue;
      }

      const auto parameter_limit = std::min(input_limits.max_parameter_bytes,
          input_limits.max_parameter_total_bytes - parameter_bytes);
      std::string value;
      std::optional<SQLBIGINT> signed_number;
      std::optional<SQLUBIGINT> unsigned_number;
      if (value_type == SQL_C_CHAR) {
        const auto* text = static_cast<const char*>(application.data_ptr);
        SQLLEN length = application.octet_length;
        if (length_or_indicator) {
          length = load_application_value<SQLLEN>(length_or_indicator);
        }
        if (length == SQL_NTS || (length == 0 && !length_or_indicator)) {
          std::size_t bytes = 0;
          while (bytes < parameter_limit && text[bytes] != '\0') ++bytes;
          if (bytes == parameter_limit && text[bytes] != '\0') {
            return reject_parameter_limit();
          }
          value.assign(text, bytes);
        } else if (length >= 0) {
          if (static_cast<std::size_t>(length) > parameter_limit) {
            return reject_parameter_limit();
          }
          value.assign(text, static_cast<std::size_t>(length));
        } else {
          set_error(SQLSTATE_GENERAL_ERROR,
                    "Data-at-execution parameters are not supported yet");
          return complete_parameter_set(SQL_ERROR);
        }
      } else if (value_type == SQL_C_WCHAR) {
        SQLLEN length = application.octet_length;
        if (length_or_indicator) {
          length = load_application_value<SQLLEN>(length_or_indicator);
        }
        SQLINTEGER units = SQL_NTS;
        if (length != SQL_NTS && (length != 0 || length_or_indicator)) {
          if (length < 0 || length % sizeof(SQLWCHAR) != 0 ||
              static_cast<SQLULEN>(length / sizeof(SQLWCHAR)) >
                  static_cast<SQLULEN>(
                      std::numeric_limits<SQLINTEGER>::max())) {
            set_error(SQLSTATE_INVALID_STRING_LENGTH,
                      "Invalid wide-character parameter length");
            return complete_parameter_set(SQL_ERROR);
          }
          units = static_cast<SQLINTEGER>(length / sizeof(SQLWCHAR));
        }
        bool exceeded = false;
        auto converted = sqlwchar_to_utf8_bounded(
            application.data_ptr, units, parameter_limit, exceeded);
        if (exceeded) return reject_parameter_limit();
        if (!converted) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid wide-character parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        value = std::move(*converted);
      } else if (value_type == SQL_C_STINYINT) {
        signed_number = load_application_value<SQLSCHAR>(application.data_ptr);
        value = std::to_string(*signed_number);
      } else if (value_type == SQL_C_SSHORT) {
        signed_number = load_application_value<SQLSMALLINT>(
            application.data_ptr);
        value = std::to_string(*signed_number);
      } else if (value_type == SQL_C_SLONG) {
        signed_number = load_application_value<SQLINTEGER>(
            application.data_ptr);
        value = std::to_string(*signed_number);
      } else if (value_type == SQL_C_SBIGINT) {
        signed_number = load_application_value<SQLBIGINT>(
            application.data_ptr);
        value = std::to_string(*signed_number);
      } else if (value_type == SQL_C_UTINYINT) {
        unsigned_number = load_application_value<SQLCHAR>(
            application.data_ptr);
        value = std::to_string(*unsigned_number);
      } else if (value_type == SQL_C_USHORT) {
        unsigned_number = load_application_value<SQLUSMALLINT>(
            application.data_ptr);
        value = std::to_string(*unsigned_number);
      } else if (value_type == SQL_C_ULONG) {
        unsigned_number = load_application_value<SQLUINTEGER>(
            application.data_ptr);
        value = std::to_string(*unsigned_number);
      } else if (value_type == SQL_C_UBIGINT) {
        unsigned_number = load_application_value<SQLUBIGINT>(
            application.data_ptr);
        value = std::to_string(*unsigned_number);
      } else if (value_type == SQL_C_NUMERIC) {
        const auto formatted = TextDataConverter::format_numeric(
            load_application_value<SQL_NUMERIC_STRUCT>(application.data_ptr),
            application.precision, application.scale);
        if (!formatted) {
          set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                    "Numeric parameter exceeds APD precision or has invalid fields");
          return complete_parameter_set(SQL_ERROR);
        }
        value = *formatted;
        if (const auto limits = signed_integer_limits(declared_sql_type)) {
          SQLBIGINT integer = 0;
          if (TextDataConverter::convert_data(value, SQL_C_SBIGINT,
                  &integer, 0, nullptr) == SQL_ERROR ||
              integer < limits->minimum || integer > limits->maximum) {
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Numeric parameter is outside SQL integer range");
            return complete_parameter_set(SQL_ERROR);
          }
          value = std::to_string(integer);
        }
      } else if (value_type == SQL_C_FLOAT) {
        value = format_floating_parameter(
            load_application_value<SQLREAL>(application.data_ptr));
      } else if (value_type == SQL_C_DOUBLE) {
        value = format_floating_parameter(
            load_application_value<SQLDOUBLE>(application.data_ptr));
      } else if (value_type == SQL_C_BIT) {
        value = *static_cast<unsigned char*>(application.data_ptr) ? "1" : "0";
      } else if (value_type == SQL_C_DATE ||
                 value_type == SQL_C_TYPE_DATE) {
        const auto formatted = format_date_parameter(
            load_application_value<SQL_DATE_STRUCT>(application.data_ptr));
        if (!formatted) {
          set_error(invalid_temporal_parameter_state(query_param.type),
                    "Invalid date parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        if (query_param.type ==
                rs::core::database::QueryParameterType::Text &&
            declared_sql_length < formatted->size()) {
          set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                    "Date parameter exceeds SQL character length");
          return complete_parameter_set(SQL_ERROR);
        }
        value = query_param.type ==
                rs::core::database::QueryParameterType::Timestamp
            ? *formatted + " 00:00:00"
            : *formatted;
      } else if (value_type == SQL_C_TIME ||
                 value_type == SQL_C_TYPE_TIME) {
        const auto formatted = format_time_parameter(
            load_application_value<SQL_TIME_STRUCT>(application.data_ptr));
        if (!formatted) {
          set_error(invalid_temporal_parameter_state(query_param.type),
                    "Invalid time parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        if (query_param.type ==
                rs::core::database::QueryParameterType::Text &&
            declared_sql_length < formatted->size()) {
          set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                    "Time parameter exceeds SQL character length");
          return complete_parameter_set(SQL_ERROR);
        }
        if (query_param.type ==
            rs::core::database::QueryParameterType::Timestamp) {
          const auto date = current_local_date_parameter();
          if (!date) {
            set_error(SQLSTATE_GENERAL_ERROR,
                      "Current local date is unavailable");
            return complete_parameter_set(SQL_ERROR);
          }
          value = *date + " " + *formatted;
        } else {
          value = *formatted;
        }
      } else if (value_type == SQL_C_TIMESTAMP ||
                 value_type == SQL_C_TYPE_TIMESTAMP) {
        const auto timestamp = load_application_value<SQL_TIMESTAMP_STRUCT>(
            application.data_ptr);
        const auto date = format_date_parameter(SQL_DATE_STRUCT{
            timestamp.year, timestamp.month, timestamp.day});
        const auto time = format_time_parameter(SQL_TIME_STRUCT{
            timestamp.hour, timestamp.minute, timestamp.second});
        const auto target = query_param.type;
        using rs::core::database::QueryParameterType;
        if ((target != QueryParameterType::Time && !date) || !time ||
            timestamp.fraction >= 1000000000u) {
          set_error(invalid_temporal_parameter_state(target),
                    "Invalid timestamp parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        if (target == QueryParameterType::Date) {
          if (timestamp.hour != 0 || timestamp.minute != 0 ||
              timestamp.second != 0 || timestamp.fraction != 0) {
            set_error(SQLSTATE_DATETIME_FIELD_OVERFLOW,
                      "Timestamp time fields cannot fit a date parameter");
            return complete_parameter_set(SQL_ERROR);
          }
          value = *date;
        } else if (target == QueryParameterType::Time) {
          if (timestamp.fraction != 0) {
            set_error(SQLSTATE_DATETIME_FIELD_OVERFLOW,
                      "Timestamp fraction cannot fit a time parameter");
            return complete_parameter_set(SQL_ERROR);
          }
          value = *time;
        } else {
          const bool character_target = target == QueryParameterType::Text;
          std::uint32_t fractional_quantum = 1000u;
          if (target == QueryParameterType::Timestamp) {
            const auto quantum = timestamp_fractional_quantum(
                implementation.precision);
            if (!quantum) {
              set_error(SQLSTATE_INVALID_PRECISION_OR_SCALE,
                        "Unsupported timestamp parameter precision");
              return complete_parameter_set(SQL_ERROR);
            }
            fractional_quantum = *quantum;
          }
          if (!character_target &&
              timestamp.fraction % fractional_quantum != 0) {
            set_error(SQLSTATE_DATETIME_FIELD_OVERFLOW,
                      "Timestamp fraction exceeds parameter precision");
            return complete_parameter_set(SQL_ERROR);
          }
          char fraction[11]{};
          std::string fractional_text;
          if (character_target) {
            std::snprintf(fraction, sizeof(fraction), ".%09u",
                          timestamp.fraction);
            fractional_text = fraction;
            while (fractional_text.size() > 1 &&
                   fractional_text.back() == '0') {
              fractional_text.pop_back();
            }
            if (fractional_text.size() == 1) {
              fractional_text.clear();
            }
          } else {
            std::snprintf(fraction, sizeof(fraction), ".%06u",
                          timestamp.fraction / 1000u);
            fractional_text = fraction;
          }
          value = *date + " " + *time + fractional_text;
          if (character_target && declared_sql_length < value.size()) {
            set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                      "Timestamp parameter exceeds SQL character length");
            return complete_parameter_set(SQL_ERROR);
          }
        }
      } else if (value_type == SQL_C_BINARY) {
        SQLLEN length = application.octet_length;
        if (length_or_indicator) {
          length = load_application_value<SQLLEN>(length_or_indicator);
        }
        if (length < 0) {
          set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid binary parameter length");
          return complete_parameter_set(SQL_ERROR);
        }
        if (static_cast<std::size_t>(length) > parameter_limit) {
          return reject_parameter_limit();
        }
        if (query_param.type ==
                rs::core::database::QueryParameterType::Binary &&
            static_cast<SQLULEN>(length) > declared_sql_length) {
          set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                    "Binary parameter exceeds SQL binary length");
          return complete_parameter_set(SQL_ERROR);
        }
        value.assign(static_cast<const char*>(application.data_ptr),
                     static_cast<std::size_t>(length));
        query_param.binary_input = true;
      } else {
        set_error(SQLSTATE_GENERAL_ERROR,
                  "Unsupported C parameter type");
        return complete_parameter_set(SQL_ERROR);
      }

      const bool character_input =
          value_type == SQL_C_CHAR || value_type == SQL_C_WCHAR;
      const bool floating_input =
          value_type == SQL_C_FLOAT || value_type == SQL_C_DOUBLE;
      const bool signed_integer_input = signed_number.has_value();
      const bool unsigned_integer_input = unsigned_number.has_value();
      const bool numeric_input = signed_integer_input ||
          unsigned_integer_input || floating_input ||
          value_type == SQL_C_NUMERIC;
      if (floating_input &&
          (declared_sql_type == SQL_DECIMAL ||
           declared_sql_type == SQL_NUMERIC)) {
        const double number = value_type == SQL_C_FLOAT
            ? static_cast<double>(load_application_value<SQLREAL>(
                  application.data_ptr))
            : load_application_value<SQLDOUBLE>(application.data_ptr);
        if (!std::isfinite(number)) {
          set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                    "Nonfinite floating parameter cannot bind to SQL numeric");
          return complete_parameter_set(SQL_ERROR);
        }
        if (declared_sql_precision > 0 && declared_sql_scale >= 0) {
          const auto digits = decimal_digits(value);
          const auto available_digits = std::max<int>(
              0, declared_sql_precision - declared_sql_scale);
          if (!digits ||
              digits->whole > static_cast<std::size_t>(available_digits)) {
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Floating parameter exceeds SQL numeric precision");
            return complete_parameter_set(SQL_ERROR);
          }
        }
      }
      using rs::core::database::QueryParameterType;
      const auto integer_limits = signed_integer_limits(declared_sql_type);
      if (character_input && integer_limits) {
        if (!decimal_digits(value)) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Character parameter is not a numeric literal");
          return complete_parameter_set(SQL_ERROR);
        }
        SQLBIGINT integer = 0;
        const auto converted = TextDataConverter::convert_data(
            value, SQL_C_SBIGINT, &integer, 0, nullptr);
        if (converted == SQL_ERROR) {
          set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                    "Character parameter is outside SQL integer range");
          return complete_parameter_set(SQL_ERROR);
        }
        if (converted == SQL_SUCCESS_WITH_INFO ||
            integer < integer_limits->minimum ||
            integer > integer_limits->maximum) {
          set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                    "Character parameter loses digits as SQL integer");
          return complete_parameter_set(SQL_ERROR);
        }
        value = std::to_string(integer);
      }
      if (character_input &&
          (declared_sql_type == SQL_REAL ||
           declared_sql_type == SQL_FLOAT ||
           declared_sql_type == SQL_DOUBLE)) {
        if (!decimal_digits(value)) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Character parameter is not a numeric literal");
          return complete_parameter_set(SQL_ERROR);
        }
        if (declared_sql_type == SQL_REAL) {
          SQLREAL number = 0;
          if (TextDataConverter::convert_data(
                  value, SQL_C_FLOAT, &number, 0, nullptr) ==
              SQL_ERROR) {
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Character parameter is outside SQL_REAL range");
            return complete_parameter_set(SQL_ERROR);
          }
          value = format_floating_parameter(number);
        } else {
          SQLDOUBLE number = 0;
          if (TextDataConverter::convert_data(
                  value, SQL_C_DOUBLE, &number, 0, nullptr) ==
              SQL_ERROR) {
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Character parameter is outside SQL floating range");
            return complete_parameter_set(SQL_ERROR);
          }
          value = format_floating_parameter(number);
        }
      }
      if ((character_input || value_type == SQL_C_NUMERIC) &&
          implementation.concise_type == SQL_BIT) {
        switch (classify_bit_numeric_literal(value)) {
          case BitNumericLiteral::Zero:
            value = "0";
            break;
          case BitNumericLiteral::One:
            value = "1";
            break;
          case BitNumericLiteral::Fraction:
            set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                      "Character parameter has fractional SQL_BIT value");
            return complete_parameter_set(SQL_ERROR);
          case BitNumericLiteral::OutOfRange:
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Character parameter is outside SQL_BIT range");
            return complete_parameter_set(SQL_ERROR);
          case BitNumericLiteral::Invalid:
            set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                      "Character parameter is not a numeric literal");
            return complete_parameter_set(SQL_ERROR);
        }
      }
      if (floating_input &&
          (integer_limits || implementation.concise_type == SQL_BIT)) {
        const double number = value_type == SQL_C_FLOAT
            ? static_cast<double>(load_application_value<SQLREAL>(
                  application.data_ptr))
            : load_application_value<SQLDOUBLE>(application.data_ptr);
        if (implementation.concise_type == SQL_BIT) {
          if (!std::isfinite(number) || number < 0 || number >= 2) {
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Floating parameter is outside SQL_BIT range");
            return complete_parameter_set(SQL_ERROR);
          }
          if (number != 0 && number != 1) {
            set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                      "Floating parameter has fractional SQL_BIT value");
            return complete_parameter_set(SQL_ERROR);
          }
          value = number == 0 ? "0" : "1";
        } else {
          const double truncated = std::trunc(number);
          const double upper_exclusive = std::ldexp(
              1.0, integer_limits->value_bits);
          if (!std::isfinite(number) ||
              truncated < -upper_exclusive || truncated >= upper_exclusive) {
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Floating parameter is outside SQL integer range");
            return complete_parameter_set(SQL_ERROR);
          }
          value = std::to_string(static_cast<SQLBIGINT>(truncated));
        }
      }
      if (signed_integer_input && integer_limits) {
        if (*signed_number < integer_limits->minimum ||
            *signed_number > integer_limits->maximum) {
          set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                    "Signed parameter is outside SQL integer range");
          return complete_parameter_set(SQL_ERROR);
        }
      }
      if (unsigned_integer_input && integer_limits) {
        if (*unsigned_number >
            static_cast<SQLUBIGINT>(integer_limits->maximum)) {
          set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                    "Unsigned parameter is outside SQL integer range");
          return complete_parameter_set(SQL_ERROR);
        }
      }
      if ((signed_integer_input || unsigned_integer_input) &&
          implementation.concise_type == SQL_BIT &&
          value != "0" && value != "1") {
        set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                  "Integer parameter is outside SQL_BIT range");
        return complete_parameter_set(SQL_ERROR);
      }
      if ((signed_integer_input || unsigned_integer_input) &&
          (declared_sql_type == SQL_DECIMAL ||
           declared_sql_type == SQL_NUMERIC) &&
          declared_sql_precision > 0 && declared_sql_scale >= 0) {
        const auto sign_size = value.front() == '-' ? std::size_t{1} : 0;
        const auto whole_digits = value.size() - sign_size;
        const auto available_digits = std::max<int>(
            0, declared_sql_precision - declared_sql_scale);
        const bool is_zero = whole_digits == 1 && value[sign_size] == '0';
        if (!is_zero &&
            whole_digits > static_cast<std::size_t>(available_digits)) {
          set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                    "Integer parameter exceeds SQL numeric precision");
          return complete_parameter_set(SQL_ERROR);
        }
      }
      if (character_input &&
          (declared_sql_type == SQL_DECIMAL ||
           declared_sql_type == SQL_NUMERIC)) {
        const auto digits = decimal_digits(value);
        if (!digits) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Character parameter is not an ODBC numeric literal");
          return complete_parameter_set(SQL_ERROR);
        }
        if (declared_sql_precision > 0 && declared_sql_scale >= 0) {
          const auto available_digits = std::max<int>(
              0, declared_sql_precision - declared_sql_scale);
          if (digits->whole > static_cast<std::size_t>(available_digits)) {
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Character parameter exceeds SQL numeric precision");
            return complete_parameter_set(SQL_ERROR);
          }
          if (digits->fractional >
              static_cast<std::size_t>(declared_sql_scale)) {
            set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                      "Character parameter exceeds SQL numeric scale");
            return complete_parameter_set(SQL_ERROR);
          }
        }
      }
      if (value_type == SQL_C_NUMERIC &&
          (declared_sql_type == SQL_DECIMAL ||
           declared_sql_type == SQL_NUMERIC) &&
          declared_sql_precision > 0 && declared_sql_scale >= 0) {
        const auto digits = decimal_digits(value);
        const auto available_digits = std::max<int>(
            0, declared_sql_precision - declared_sql_scale);
        if (!digits ||
            digits->whole > static_cast<std::size_t>(available_digits) ||
            digits->fractional >
                static_cast<std::size_t>(declared_sql_scale)) {
          set_error(SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE,
                    "Numeric parameter exceeds SQL precision or scale");
          return complete_parameter_set(SQL_ERROR);
        }
      }
      if (numeric_input && declared_sql_length > 0 &&
          is_character_sql_type(declared_sql_type) &&
          static_cast<SQLULEN>(value.size()) > declared_sql_length) {
        set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                  "Numeric parameter exceeds SQL character length");
        return complete_parameter_set(SQL_ERROR);
      }
      if (character_input && declared_sql_length > 0 &&
          (declared_sql_type == SQL_CHAR ||
           declared_sql_type == SQL_VARCHAR ||
           declared_sql_type == SQL_LONGVARCHAR) &&
          static_cast<SQLULEN>(value.size()) > declared_sql_length) {
        set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                  "Character parameter exceeds SQL byte length");
        return complete_parameter_set(SQL_ERROR);
      }
      if (character_input &&
          (declared_sql_type == SQL_WCHAR ||
           declared_sql_type == SQL_WVARCHAR ||
           declared_sql_type == SQL_WLONGVARCHAR)) {
        if (!utf8_to_wide(value)) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Character parameter is not valid Unicode");
          return complete_parameter_set(SQL_ERROR);
        }
        const auto character_count = std::count_if(
            value.begin(), value.end(), [](unsigned char byte) {
              return (byte & 0xc0) != 0x80;
            });
        if (declared_sql_length > 0 &&
            static_cast<SQLULEN>(character_count) > declared_sql_length) {
          set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                    "Character parameter exceeds SQL character length");
          return complete_parameter_set(SQL_ERROR);
        }
      }
      if (character_input && query_param.type == QueryParameterType::Binary) {
        std::size_t hex_length = value.size() / 2 * 2;
        if (value_type == SQL_C_WCHAR) {
          std::size_t character_count = 0;
          std::size_t last_character_start = 0;
          for (std::size_t position = 0; position < value.size(); ++position) {
            const auto byte = static_cast<unsigned char>(value[position]);
            if ((byte & 0xc0) != 0x80) {
              ++character_count;
              last_character_start = position;
            }
          }
          hex_length = character_count % 2 == 0
              ? value.size() : last_character_start;
        }
        const auto decoded = rs::util::decode_hex(
            std::string_view(value).substr(0, hex_length));
        if (!decoded) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Character binary parameter is not hexadecimal");
          return complete_parameter_set(SQL_ERROR);
        }
        if (static_cast<SQLULEN>(decoded->size()) > declared_sql_length) {
          set_error(SQLSTATE_STRING_DATA_RIGHT_TRUNCATION,
                    "Character binary parameter exceeds SQL binary length");
          return complete_parameter_set(SQL_ERROR);
        }
        value = *decoded;
      }
      if (character_input &&
          (query_param.type == QueryParameterType::Date ||
           query_param.type == QueryParameterType::Time ||
           query_param.type == QueryParameterType::Timestamp) &&
          (has_temporal_timezone_suffix(value) ||
           has_temporal_t_separator(value))) {
        set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                  "Invalid ODBC temporal parameter literal form");
        return complete_parameter_set(SQL_ERROR);
      }
      if (character_input && query_param.type == QueryParameterType::Date) {
        SQL_DATE_STRUCT parsed{};
        const auto converted = TextDataConverter::convert_data(
            value, SQL_C_TYPE_DATE, &parsed, sizeof(parsed), nullptr,
            nullptr);
        if (converted == SQL_ERROR) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid character date parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        if (converted == SQL_SUCCESS_WITH_INFO) {
          set_error(SQLSTATE_DATETIME_FIELD_OVERFLOW,
                    "Character date parameter contains nonzero time");
          return complete_parameter_set(SQL_ERROR);
        }
        const auto date = format_date_parameter(parsed);
        if (!date) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid character date parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        value = *date;
      } else if (character_input &&
                 query_param.type == QueryParameterType::Time) {
        SQL_TIME_STRUCT parsed{};
        const auto converted = TextDataConverter::convert_data(
            value, SQL_C_TYPE_TIME, &parsed, sizeof(parsed), nullptr,
            nullptr);
        if (converted == SQL_ERROR) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid character time parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        if (converted == SQL_SUCCESS_WITH_INFO) {
          set_error(SQLSTATE_DATETIME_FIELD_OVERFLOW,
                    "Character time parameter contains nonzero fraction");
          return complete_parameter_set(SQL_ERROR);
        }
        const auto time = format_time_parameter(parsed);
        if (!time) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid character time parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        value = *time;
      } else if (character_input &&
                 query_param.type == QueryParameterType::Timestamp) {
        const auto quantum = timestamp_fractional_quantum(
            implementation.precision);
        if (!quantum) {
          set_error(SQLSTATE_INVALID_PRECISION_OR_SCALE,
                    "Unsupported timestamp parameter precision");
          return complete_parameter_set(SQL_ERROR);
        }
        SQL_TIMESTAMP_STRUCT parsed{};
        const auto converted = TextDataConverter::convert_data(
            value, SQL_C_TYPE_TIMESTAMP, &parsed, sizeof(parsed), nullptr,
            nullptr);
        if (converted == SQL_ERROR) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid character timestamp parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        if (converted == SQL_SUCCESS_WITH_INFO ||
            parsed.fraction % *quantum != 0) {
          set_error(SQLSTATE_DATETIME_FIELD_OVERFLOW,
                    "Character timestamp fraction exceeds parameter precision");
          return complete_parameter_set(SQL_ERROR);
        }
        const auto trimmed = ConnectionString::trim(value);
        const bool time_only = trimmed.size() >= 8 &&
            trimmed[2] == ':' && trimmed[5] == ':' &&
            (trimmed.size() == 8 ||
             (trimmed[8] == '.' && trimmed.size() > 9 &&
              std::all_of(trimmed.begin() + 9, trimmed.end(),
                          [](char digit) {
                            return digit >= '0' && digit <= '9';
                          })));
        if (time_only) {
          const auto date = format_date_parameter(SQL_DATE_STRUCT{
              parsed.year, parsed.month, parsed.day});
          if (!date) {
            set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                      "Current local date is unavailable for timestamp");
            return complete_parameter_set(SQL_ERROR);
          }
          value = *date + " " + trimmed;
        }
      }

      // Normalization can expand a value (for example a time into a timestamp).
      if (value.size() > parameter_limit) return reject_parameter_limit();
      parameter_bytes += value.size();
      query_param.value = std::move(value);
      param_values.push_back(std::move(query_param));
    }
    
    // Use PostgreSQL Parse/Bind/Execute protocol
    auto deadline = rs::util::make_deadline(
        timeout_duration(query_timeout_seconds_));
    auto transaction = conn_->begin_transaction_if_needed(deadline);
    if (transaction.has_error()) {
      const auto timeout = is_timeout_error(transaction.error());
      set_error(request_sqlstate(transaction.error(), SQLSTATE_GENERAL_ERROR),
                transaction.error_message());
      conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
                 transaction.backend_error().safe_summary(),
                 {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public}, {"kind", "prepared", rs::core::logging::FieldSensitivity::Public},
                  {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
      if (timeout) conn_->disconnect();
      return complete_parameter_set(SQL_ERROR);
    }
    auto result = conn_->backend_prepared(prepared_sql_, param_values, deadline);
    
    if (result.has_error()) {
      const auto timeout = is_timeout_error(result.error());
      set_error(query_failure_sqlstate(conn_->backend_provider(),
                                       result.backend_error(), SQLSTATE_SYNTAX_ERROR,
                                       dynamic_function.code),
                result.error_message());
      conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
                 result.backend_error().safe_summary(),
                 {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public}, {"kind", "prepared", rs::core::logging::FieldSensitivity::Public},
                  {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
      if (timeout) conn_->disconnect();
      return complete_parameter_set(SQL_ERROR);
    }

    const auto row_count = result->rows.size();
    const auto affected_rows = result->affected_rows;
    apply_query_result(std::move(*result), true);
    conn_->log(rs::core::logging::LogLevel::Info, "query_completed",
               "Prepared SQL execution completed",
               {{"kind", "prepared", rs::core::logging::FieldSensitivity::Public},
                {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public},
                {"rows", std::to_string(row_count), rs::core::logging::FieldSensitivity::Public},
                {"affected_rows", std::to_string(affected_rows), rs::core::logging::FieldSensitivity::Public},
                {"parameters", std::to_string(parameter_count_), rs::core::logging::FieldSensitivity::Public}});
    return complete_parameter_set(SQL_SUCCESS);
    
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed", "Database operation failed",
               {{"sqlstate", get_sqlstate(), rs::core::logging::FieldSensitivity::Public}, {"kind", "prepared", rs::core::logging::FieldSensitivity::Public},
                {"duration_ms", elapsed_milliseconds(started), rs::core::logging::FieldSensitivity::Public}});
    return complete_parameter_set(SQL_ERROR);
  }
}

SQLRETURN ODBCStatement::bind_parameter(SQLUSMALLINT parameter_number, SQLSMALLINT input_output_type,
                                       SQLSMALLINT value_type, SQLSMALLINT parameter_type, SQLULEN column_size,
                                       SQLSMALLINT decimal_digits, SQLPOINTER parameter_value, SQLLEN buffer_length,
                                       SQLLEN* strlen_or_indicator) {
  if (parameter_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid parameter number");
    return SQL_ERROR;
  }
  const bool valid_direction = input_output_type == SQL_PARAM_INPUT ||
      input_output_type == SQL_PARAM_INPUT_OUTPUT ||
      input_output_type == SQL_PARAM_OUTPUT
#ifdef SQL_PARAM_INPUT_OUTPUT_STREAM
      || input_output_type == SQL_PARAM_INPUT_OUTPUT_STREAM
#endif
#ifdef SQL_PARAM_OUTPUT_STREAM
      || input_output_type == SQL_PARAM_OUTPUT_STREAM
#endif
      ;
  if (!valid_direction) {
    set_error(SQLSTATE_INVALID_PARAMETER_TYPE,
              "Invalid input/output parameter type");
    return SQL_ERROR;
  }
  if (input_output_type != SQL_PARAM_INPUT) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Output parameters are not supported");
    return SQL_ERROR;
  }
  if (buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Parameter buffer length cannot be negative");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_valid_c_type(value_type)) {
    set_error(SQLSTATE_INVALID_APPLICATION_BUFFER_TYPE,
              "Invalid parameter application buffer type");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_supported_parameter_c_type(value_type)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Parameter application buffer type is not supported");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_valid_sql_type(parameter_type)) {
    set_error(SQLSTATE_INVALID_SQL_DATA_TYPE,
              "Invalid parameter SQL data type");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_supported_parameter_sql_type(parameter_type)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Parameter SQL data type is not supported");
    return SQL_ERROR;
  }
  if (value_type == SQL_C_NUMERIC && parameter_type != SQL_DECIMAL &&
      parameter_type != SQL_NUMERIC && parameter_type != SQL_TINYINT &&
      parameter_type != SQL_SMALLINT &&
      parameter_type != SQL_INTEGER && parameter_type != SQL_BIGINT &&
      parameter_type != SQL_BIT && parameter_type != SQL_REAL &&
      parameter_type != SQL_FLOAT && parameter_type != SQL_DOUBLE &&
      !is_character_sql_type(parameter_type)) {
    set_error(SQLSTATE_RESTRICTED_DATA_TYPE,
              "Numeric C parameter requires an exact numeric SQL type");
    return SQL_ERROR;
  }
  const bool date_c_type = value_type == SQL_C_DATE ||
      value_type == SQL_C_TYPE_DATE;
  const bool time_c_type = value_type == SQL_C_TIME ||
      value_type == SQL_C_TYPE_TIME;
  const bool timestamp_c_type = value_type == SQL_C_TIMESTAMP ||
      value_type == SQL_C_TYPE_TIMESTAMP;
  if (date_c_type || time_c_type || timestamp_c_type) {
    const auto target = parameter_type_for(parameter_type, value_type);
    using rs::core::database::QueryParameterType;
    const bool convertible = target == QueryParameterType::Text ||
        (date_c_type && (target == QueryParameterType::Date ||
                         target == QueryParameterType::Timestamp)) ||
        (time_c_type && (target == QueryParameterType::Time ||
                         target == QueryParameterType::Timestamp)) ||
        (timestamp_c_type && (target == QueryParameterType::Date ||
                              target == QueryParameterType::Time ||
                              target == QueryParameterType::Timestamp));
    if (!convertible) {
      set_error(SQLSTATE_RESTRICTED_DATA_TYPE,
                "Temporal C parameter cannot convert to the SQL data type");
      return SQL_ERROR;
    }
  }
  if ((parameter_type == SQL_TYPE_TIME ||
       parameter_type == SQL_TYPE_TIMESTAMP) &&
      (decimal_digits < 0 || decimal_digits > 6)) {
    set_error(SQLSTATE_INVALID_PRECISION_OR_SCALE,
              "Temporal parameter precision must be from 0 to 6");
    return SQL_ERROR;
  }
  const bool precision_sql_type = parameter_type == SQL_DECIMAL ||
      parameter_type == SQL_NUMERIC || parameter_type == SQL_FLOAT ||
      parameter_type == SQL_REAL || parameter_type == SQL_DOUBLE;
  if (precision_sql_type && column_size > static_cast<SQLULEN>(
          std::numeric_limits<SQLSMALLINT>::max())) {
    set_error(SQLSTATE_INVALID_PRECISION_OR_SCALE,
              "Parameter precision is too large");
    return SQL_ERROR;
  }
  if (!parameter_value && !strlen_or_indicator) {
    set_error(SQLSTATE_INVALID_NULL_POINTER,
              "Input parameter requires a value or indicator pointer");
    return SQL_ERROR;
  }

  const auto number = [](auto numeric) {
    return reinterpret_cast<SQLPOINTER>(
        static_cast<std::uintptr_t>(numeric));
  };
  const auto application_descriptor = descriptor(app_param_descriptor_);
  const auto bound_c_type = value_type == SQL_C_DEFAULT
      ? ResultTypes::default_c_type(parameter_type) : value_type;
  application_descriptor->set_field(
      parameter_number, SQL_DESC_CONCISE_TYPE, number(bound_c_type), 0);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_OCTET_LENGTH, number(buffer_length), 0);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_DATA_PTR, parameter_value, 0);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_INDICATOR_PTR, strlen_or_indicator, 0);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_OCTET_LENGTH_PTR, strlen_or_indicator, 0);

  const auto implementation_descriptor = descriptor(imp_param_descriptor_);
  implementation_descriptor->set_field(
      parameter_number, SQL_DESC_CONCISE_TYPE, number(parameter_type), 0);
  implementation_descriptor->set_field(
      parameter_number,
      precision_sql_type ? SQL_DESC_PRECISION : SQL_DESC_LENGTH,
      number(column_size), 0);
  implementation_descriptor->set_field(
      parameter_number, SQL_DESC_SCALE, number(decimal_digits), 0);
  if (parameter_type == SQL_TYPE_TIME ||
      parameter_type == SQL_TYPE_TIMESTAMP) {
    implementation_descriptor->set_field(
        parameter_number, SQL_DESC_PRECISION, number(decimal_digits), 0);
  }
  implementation_descriptor->set_field(
      parameter_number, SQL_DESC_PARAMETER_TYPE, number(input_output_type), 0);
  if (parameter_number > param_metadata_.size()) {
    param_metadata_.resize(parameter_number);
  }
  auto& metadata = param_metadata_[parameter_number - 1];
  metadata.sql_type = parameter_type;
  metadata.column_size = column_size;
  metadata.decimal_digits = decimal_digits;
  metadata.nullable = SQL_NULLABLE_UNKNOWN;
  if (prepared_ && !executed_) {
    prepared_metadata_available_ = false;
    column_info_.clear();
    descriptor(imp_row_descriptor_)->replace_records({});
  }
  
  return SQL_SUCCESS;
}

void ODBCStatement::apply_query_result(
    rs::core::database::QueryResult result,
    bool include_parameter_metadata) {
  if (!rs::core::database::valid_execution_structure(result)) {
    throw std::invalid_argument("Data source returned invalid execution sequence");
  }
  require_normalized_columns(result);
  for (const auto& item : result.additional_results) require_normalized_columns(item);
  const auto statement_kind = result.statement_kind;
  if (!result.additional_results.empty()) {
    pending_results_.reserve(
        pending_results_.size() + result.additional_results.size());
    for (auto& additional : result.additional_results) {
      pending_results_.push_back(std::move(additional));
    }
  }
  result_rows_ = std::move(result.rows);
  result_cell_errors_ = std::move(result.cell_errors);
  if (max_rows_ > 0 && result_rows_.size() > max_rows_) {
    result_rows_.resize(static_cast<std::size_t>(max_rows_));
    std::erase_if(result_cell_errors_, [&](const auto& error) { return error.row >= result_rows_.size(); });
  }
  current_row_ = 0;
  row_positioned_ = false;
  get_data_column_ = 0;
  get_data_offset_ = 0;
  get_data_target_type_ = 0;
  executed_ = true;

  const auto max_rows = static_cast<std::size_t>(
      std::numeric_limits<SQLLEN>::max());
  affected_rows_ = result.affected_rows > max_rows
      ? std::numeric_limits<SQLLEN>::max()
      : static_cast<SQLLEN>(result.affected_rows);
  auto diagnostic_header = get_diagnostic_header();
  if (statement_kind) {
    const auto dynamic_function = completed_dynamic_function(*statement_kind);
    diagnostic_header.dynamic_function = dynamic_function.name;
    diagnostic_header.dynamic_function_code = dynamic_function.code;
  }
  const auto cursor_rows = result_rows_.size() > max_rows
      ? std::numeric_limits<SQLLEN>::max()
      : static_cast<SQLLEN>(result_rows_.size());
  set_statement_diagnostic_header(
      cursor_rows, affected_rows_, diagnostic_header.dynamic_function,
      diagnostic_header.dynamic_function_code);

  apply_result_metadata(result, include_parameter_metadata);
}

void ODBCStatement::apply_result_metadata(
    const rs::core::database::QueryResult& result,
    bool include_parameter_metadata) {

  require_normalized_columns(result);
  column_info_.clear();
  column_info_.reserve(result.columns.size());
  for (const auto& column : result.columns) {
    column_info_.push_back(column_info_for(column));
  }
  if (column_info_.empty() && !result_rows_.empty()) {
    column_info_.reserve(result_rows_.front().size());
    for (std::size_t i = 0; i < result_rows_.front().size(); ++i) {
      column_info_.push_back(ColumnInfo{
          "column" + std::to_string(i + 1), SQL_VARCHAR, 255, 0,
          SQL_NULLABLE_UNKNOWN});
    }
  }
  std::vector<DescriptorRecord> row_descriptor_records;
  row_descriptor_records.reserve(column_info_.size());
  for (const auto& column : column_info_) {
    row_descriptor_records.push_back(descriptor_record_for(column, conn_->type_catalog()));
  }
  descriptor(imp_row_descriptor_)->replace_records(
      std::move(row_descriptor_records));

  const auto implementation_descriptor = descriptor(imp_param_descriptor_);
  if (!include_parameter_metadata) {
    param_metadata_.clear();
    implementation_descriptor->replace_records({});
    return;
  }

  param_metadata_.clear();
  param_metadata_.reserve(result.normalized_parameter_types.size());
  for (std::size_t index = 0;
       index < result.normalized_parameter_types.size(); ++index) {
    const auto* prior_record = index < implementation_descriptor->record_count()
        ? implementation_descriptor->record(index) : nullptr;
    param_metadata_.push_back(parameter_metadata_for(
        result.normalized_parameter_types[index], prior_record));
  }
  std::vector<DescriptorRecord> parameter_descriptor_records;
  parameter_descriptor_records.reserve(param_metadata_.size());
  for (std::size_t index = 0; index < param_metadata_.size(); ++index) {
    auto record = descriptor_record_for(param_metadata_[index], conn_->type_catalog());
    if (const auto* prior = implementation_descriptor->record(index)) {
      record.bound_sql_type = prior->bound_sql_type;
      record.bound_sql_length = prior->bound_sql_length;
      record.bound_sql_precision = prior->bound_sql_precision;
      record.bound_sql_scale = prior->bound_sql_scale;
    }
    parameter_descriptor_records.push_back(std::move(record));
  }
  implementation_descriptor->replace_records(
      std::move(parameter_descriptor_records));
}

void ODBCStatement::clear_current_result() {
  result_rows_.clear();
  result_cell_errors_.clear();
  column_info_.clear();
  descriptor(imp_row_descriptor_)->replace_records({});
  get_data_column_ = 0;
  get_data_offset_ = 0;
  get_data_target_type_ = 0;
  current_row_ = 0;
  row_positioned_ = false;
  affected_rows_ = 0;
  executed_ = false;
  prepared_metadata_available_ = false;
  prepared_metadata_ipd_revision_ = 0;
}

// Column binding implementation
SQLRETURN ODBCStatement::bind_col(SQLUSMALLINT column_number, SQLSMALLINT target_type,
                                  SQLPOINTER target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator) {
  if (column_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid column number");
    return SQL_ERROR;
  }
  if (buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Column buffer length cannot be negative");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_valid_c_type(target_type)) {
    set_error(SQLSTATE_INVALID_APPLICATION_BUFFER_TYPE,
              "Invalid column application buffer type");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_supported_c_type(target_type)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Column application buffer type is not supported");
    return SQL_ERROR;
  }
  
  // Check if column number is valid (after execution)
  if (executed_ && column_number > column_info_.size()) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
              "Column number out of range");
    return SQL_ERROR;
  }

  const auto number = [](auto numeric) {
    return reinterpret_cast<SQLPOINTER>(
        static_cast<std::uintptr_t>(numeric));
  };
  const auto application_descriptor = descriptor(app_row_descriptor_);
  application_descriptor->set_field(
      column_number, SQL_DESC_CONCISE_TYPE, number(target_type), 0);
  application_descriptor->set_field(
      column_number, SQL_DESC_OCTET_LENGTH, number(buffer_length), 0);
  application_descriptor->set_field(
      column_number, SQL_DESC_DATA_PTR, target_value, 0);
  application_descriptor->set_field(
      column_number, SQL_DESC_INDICATOR_PTR, strlen_or_indicator, 0);
  application_descriptor->set_field(
      column_number, SQL_DESC_OCTET_LENGTH_PTR, strlen_or_indicator, 0);
  return SQL_SUCCESS;
}

// Metadata functions implementation
SQLRETURN ODBCStatement::describe_prepared_metadata() {
  const auto implementation_descriptor = descriptor(imp_param_descriptor_);
  if (prepared_metadata_available_ &&
      prepared_metadata_ipd_revision_ ==
          implementation_descriptor->revision()) {
    return SQL_SUCCESS;
  }

  if (!conn_->has_statement_description_facet()) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED, "Data source does not support statement description");
    return SQL_ERROR;
  }
  std::vector<rs::core::database::QueryParameterType> parameter_types(
      static_cast<std::size_t>(parameter_count_),
      rs::core::database::QueryParameterType::Unspecified);
  for (std::size_t index = 0;
       index < parameter_types.size() &&
       index < implementation_descriptor->record_count(); ++index) {
    const auto* record = implementation_descriptor->record(index);
    // Inferred IPD types describe the previous query, not an application binding.
    if (record->bound_sql_type != 0) {
      parameter_types[index] = parameter_type_for(
          record->bound_sql_type, SQL_C_DEFAULT);
    }
  }

  auto deadline = rs::util::make_deadline(
      timeout_duration(query_timeout_seconds_));
  auto result = conn_->backend_description(
      prepared_sql_, parameter_types, deadline);
  if (result.has_error()) {
    const auto timeout = is_timeout_error(result.error());
    set_error(query_failure_sqlstate(conn_->backend_provider(),
                                     result.backend_error(), SQLSTATE_SYNTAX_ERROR,
                                     classify_dynamic_function(prepared_sql_).code),
              result.error_message());
    if (timeout) conn_->disconnect();
    return SQL_ERROR;
  }
  if (!rs::core::database::valid_description_structure(*result)) {
    set_error(SQLSTATE_GENERAL_ERROR, "Data source returned invalid description sequence");
    return SQL_ERROR;
  }
  if (result->normalized_parameter_types.size() !=
      static_cast<std::size_t>(parameter_count_)) {
    set_error(SQLSTATE_GENERAL_ERROR,
              "Data source returned an inconsistent parameter count");
    return SQL_ERROR;
  }

  apply_result_metadata(*result, true);
  prepared_metadata_available_ = true;
  prepared_metadata_ipd_revision_ = implementation_descriptor->revision();
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::ensure_result_metadata() {
  if (executed_) return SQL_SUCCESS;
  if (!prepared_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
              "Statement has not been prepared or executed");
    return SQL_ERROR;
  }
  return describe_prepared_metadata();
}

SQLRETURN ODBCStatement::get_num_result_cols(SQLSMALLINT* column_count) {
  if (!column_count) {
    set_error(SQLSTATE_INVALID_NULL_POINTER,
              "Null pointer for column count");
    return SQL_ERROR;
  }
  
  const auto metadata_result = ensure_result_metadata();
  if (metadata_result != SQL_SUCCESS) return metadata_result;
  
  const auto count = static_cast<SQLSMALLINT>(column_info_.size());
  store_application_value(column_count, count);
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::get_type_info(SQLSMALLINT data_type) {
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
    return SQL_ERROR;
  }
  if (executed_ && !column_info_.empty()) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "A result cursor is already open");
    return SQL_ERROR;
  }
  if (data_type != SQL_ALL_TYPES &&
      !ResultTypes::is_valid_sql_type(data_type)) {
    set_error(SQLSTATE_INVALID_SQL_DATA_TYPE,
              "Invalid SQL data type identifier");
    return SQL_ERROR;
  }

  using rs::core::database::QueryResult;
  using rs::core::database::ResultColumnMetadata;
  QueryResult result;
  using rs::core::database::ScalarType;
  const auto column = [](const char* name, ScalarType type, std::uint64_t size) {
    ResultColumnMetadata result;
    result.name = name;
    result.normalized_type = rs::core::database::NativeTypeInfo{type, size, 0, true};
    return result;
  };
  result.columns = {
      column("TYPE_NAME", ScalarType::VarChar, 0),
      column("DATA_TYPE", ScalarType::SmallInt, 5),
      column("COLUMN_SIZE", ScalarType::Integer, 10),
      column("LITERAL_PREFIX", ScalarType::VarChar, 0),
      column("LITERAL_SUFFIX", ScalarType::VarChar, 0),
      column("CREATE_PARAMS", ScalarType::VarChar, 0),
      column("NULLABLE", ScalarType::SmallInt, 5),
      column("CASE_SENSITIVE", ScalarType::SmallInt, 5),
      column("SEARCHABLE", ScalarType::SmallInt, 5),
      column("UNSIGNED_ATTRIBUTE", ScalarType::SmallInt, 5),
      column("FIXED_PREC_SCALE", ScalarType::SmallInt, 5),
      column("AUTO_UNIQUE_VALUE", ScalarType::SmallInt, 5),
      column("LOCAL_TYPE_NAME", ScalarType::VarChar, 0),
      column("MINIMUM_SCALE", ScalarType::SmallInt, 5),
      column("MAXIMUM_SCALE", ScalarType::SmallInt, 5),
      column("SQL_DATA_TYPE", ScalarType::SmallInt, 5),
      column("SQL_DATETIME_SUB", ScalarType::SmallInt, 5),
      column("NUM_PREC_RADIX", ScalarType::Integer, 10),
      column("INTERVAL_PRECISION", ScalarType::SmallInt, 5),
  };

  const auto catalog = conn_->type_catalog();
  std::vector<std::pair<SQLSMALLINT, const rs::core::database::TypeDefinition*>> types;
  for (const auto& type : catalog) {
    const auto sql_type = odbc_scalar_type(type.type);
    types.emplace_back(sql_type, &type);
    if (sql_type == SQL_TYPE_DATE) types.emplace_back(SQL_DATE, &type);
    if (sql_type == SQL_TYPE_TIME) types.emplace_back(SQL_TIME, &type);
    if (sql_type == SQL_TYPE_TIMESTAMP) types.emplace_back(SQL_TIMESTAMP, &type);
  }
  std::stable_sort(types.begin(), types.end(),
      [](const auto& left, const auto& right) { return left.first < right.first; });
  for (const auto& [sql_type, definition] : types) {
    if (data_type != SQL_ALL_TYPES && data_type != sql_type) continue;
    const auto& type = *definition;
    const bool character_type = sql_type == SQL_CHAR || sql_type == SQL_VARCHAR ||
        sql_type == SQL_LONGVARCHAR;
    const auto modern_type = modern_temporal_type(sql_type);
    const auto datetime_sub = descriptor_subtype_for(modern_type);
    result.rows.push_back({
        type_info_text(type.name), type_info_number(sql_type),
        type_info_number(type.column_size), type_info_text(type.literal_prefix),
        type_info_text(type.literal_suffix), type_info_text(type.create_params),
        type_info_number(SQL_NULLABLE), type_info_number(type.case_sensitive),
        type_info_number(character_type ? SQL_SEARCHABLE : SQL_PRED_BASIC),
        type.unsigned_attribute ? type_info_number(*type.unsigned_attribute)
                                : rs::core::database::ResultCell{},
        type_info_number(SQL_FALSE),
        type.unsigned_attribute ? type_info_number(SQL_FALSE)
                                : rs::core::database::ResultCell{},
        rs::core::database::ResultCell{},
        type.minimum_scale ? type_info_number(*type.minimum_scale)
                           : rs::core::database::ResultCell{},
        type.maximum_scale ? type_info_number(*type.maximum_scale)
                           : rs::core::database::ResultCell{},
        type_info_number(datetime_sub != 0 ? SQL_DATETIME : sql_type),
        datetime_sub == 0 ? rs::core::database::ResultCell{}
                          : type_info_number(datetime_sub),
        type.numeric_radix == 0 ? rs::core::database::ResultCell{}
                                : type_info_number(type.numeric_radix),
        rs::core::database::ResultCell{},
    });
  }
  result.affected_rows = result.rows.size();
  apply_query_result(std::move(result), false);
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::execute_catalog(
    const rs::core::database::CatalogRequest& request) {
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    return SQL_ERROR;
  }
  auto query = conn_->backend_catalog(request);
  if (query.has_error()) {
    set_error(request_sqlstate(query.error(), SQLSTATE_GENERAL_ERROR),
              query.error_message());
    return SQL_ERROR;
  }
  return execute_direct(*query);
}

SQLRETURN ODBCStatement::tables(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::optional<std::string>& table_name,
    const std::optional<std::string>& table_type) {
  const bool empty_catalog = catalog_name && catalog_name->empty();
  const bool empty_schema = schema_name && schema_name->empty();
  const bool empty_table = table_name && table_name->empty();
  const bool no_type_filter = !table_type || table_type->empty();

  using Request = rs::core::database::TablesCatalogRequest;
  Request request;
  request.catalog = catalog_name;
  request.schema = schema_name;
  request.table = table_name;
  if (catalog_name && *catalog_name == SQL_ALL_CATALOGS && empty_schema &&
      empty_table && no_type_filter) {
    request.mode = Request::Mode::Catalogs;
  } else if (schema_name && *schema_name == SQL_ALL_SCHEMAS &&
             empty_catalog && empty_table && no_type_filter) {
    request.mode = Request::Mode::Schemas;
  } else if (table_type && *table_type == SQL_ALL_TABLE_TYPES &&
             empty_catalog && empty_schema && empty_table) {
    request.mode = Request::Mode::TableTypes;
  } else if (!no_type_filter) {
    request.types = parse_table_types(*table_type);
  }
  return execute_catalog(request);
}

SQLRETURN ODBCStatement::columns(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::optional<std::string>& table_name,
    const std::optional<std::string>& column_name) {
  return execute_catalog(rs::core::database::ColumnsCatalogRequest{
      catalog_name, schema_name, table_name, column_name});
}

SQLRETURN ODBCStatement::primary_keys(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::string& table_name) {
  return execute_catalog(rs::core::database::PrimaryKeysCatalogRequest{
      catalog_name, schema_name, table_name});
}

SQLRETURN ODBCStatement::foreign_keys(
    const std::optional<std::string>& pk_catalog_name,
    const std::optional<std::string>& pk_schema_name,
    const std::optional<std::string>& pk_table_name,
    const std::optional<std::string>& fk_catalog_name,
    const std::optional<std::string>& fk_schema_name,
    const std::optional<std::string>& fk_table_name) {
  return execute_catalog(rs::core::database::ForeignKeysCatalogRequest{
      pk_catalog_name, pk_schema_name, pk_table_name,
      fk_catalog_name, fk_schema_name, fk_table_name});
}

SQLRETURN ODBCStatement::statistics(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::string& table_name, bool unique_only) {
  return execute_catalog(rs::core::database::StatisticsCatalogRequest{
      catalog_name, schema_name, table_name, unique_only});
}

SQLRETURN ODBCStatement::procedures(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::optional<std::string>& procedure_name) {
  return execute_catalog(rs::core::database::ProceduresCatalogRequest{
      catalog_name, schema_name, procedure_name});
}

SQLRETURN ODBCStatement::procedure_columns(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::optional<std::string>& procedure_name,
    const std::optional<std::string>& column_name) {
  return execute_catalog(rs::core::database::ProcedureColumnsCatalogRequest{
      catalog_name, schema_name, procedure_name, column_name});
}

SQLRETURN ODBCStatement::special_columns(
    SQLUSMALLINT identifier_type,
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::string& table_name, SQLUSMALLINT scope,
    bool require_non_nullable) {
  using Request = rs::core::database::SpecialColumnsCatalogRequest;
  const auto kind = identifier_type == SQL_ROWVER
      ? Request::Identifier::RowVersion : Request::Identifier::RowIdentity;
  const auto lifetime = scope == SQL_SCOPE_CURROW ? Request::Scope::CurrentRow
      : scope == SQL_SCOPE_TRANSACTION ? Request::Scope::Transaction
                                      : Request::Scope::Session;
  return execute_catalog(Request{
      kind, lifetime, catalog_name, schema_name, table_name, require_non_nullable});
}

SQLRETURN ODBCStatement::row_count(SQLLEN* row_count_value) {
  if (!row_count_value) {
    set_error(SQLSTATE_INVALID_NULL_POINTER, "Null pointer for row count");
    return SQL_ERROR;
  }
  if (!executed_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR, "No statement executed");
    return SQL_ERROR;
  }
  const SQLLEN count = affected_rows_;
  store_application_value(row_count_value, count);
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::describe_col(SQLUSMALLINT column_number, SQLCHAR* column_name, SQLSMALLINT name_buffer_length,
                                     SQLSMALLINT* name_length, SQLSMALLINT* data_type, SQLULEN* column_size,
                                     SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable) {
  if (name_buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Invalid column-name buffer length");
    return SQL_ERROR;
  }
  const auto metadata_result = ensure_result_metadata();
  if (metadata_result != SQL_SUCCESS) return metadata_result;
  if (column_info_.empty()) {
    set_error(SQLSTATE_PREPARED_STATEMENT_NOT_CURSOR,
              "Statement does not produce a result set");
    return SQL_ERROR;
  }
  if (column_number < 1 || column_number > column_info_.size()) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid column number");
    return SQL_ERROR;
  }
  
  const auto& col = column_info_[column_number - 1];
  
  // Copy column name
  if (column_name && name_buffer_length > 0) {
    size_t copy_len = std::min(static_cast<size_t>(name_buffer_length - 1), col.name.length());
    std::memcpy(column_name, col.name.c_str(), copy_len);
    column_name[copy_len] = '\0';
  }
  
  if (name_length) {
    store_application_value(name_length,
                            static_cast<SQLSMALLINT>(col.name.length()));
  }
  if (data_type) {
    store_application_value(data_type, static_cast<SQLSMALLINT>(col.sql_type));
  }
  if (column_size) {
    store_application_value(column_size,
                            static_cast<SQLULEN>(col.column_size));
  }
  if (decimal_digits) {
    store_application_value(
        decimal_digits, static_cast<SQLSMALLINT>(col.decimal_digits));
  }
  if (nullable) {
    store_application_value(nullable, static_cast<SQLSMALLINT>(col.nullable));
  }

  if (column_name && !col.name.empty() &&
      static_cast<std::size_t>(name_buffer_length) <= col.name.length()) {
    set_error(SQLSTATE_STRING_DATA_TRUNCATED,
              "Column name was truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::col_attribute(SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                                      SQLPOINTER character_attribute, SQLSMALLINT buffer_length,
                                      SQLSMALLINT* string_length, SQLLEN* numeric_attribute) {
  if (!is_known_column_attribute(field_identifier)) {
    set_error(SQLSTATE_INVALID_DESCRIPTOR_FIELD,
              "Invalid column attribute identifier");
    return SQL_ERROR;
  }
  if (is_character_column_attribute(field_identifier) && buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Invalid column-attribute buffer length");
    return SQL_ERROR;
  }
  if (!is_supported_column_attribute(field_identifier)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Column attribute is not supported");
    return SQL_ERROR;
  }
  const auto metadata_result = ensure_result_metadata();
  if (metadata_result != SQL_SUCCESS) return metadata_result;
  if (is_count_column_attribute(field_identifier)) {
    if (numeric_attribute) {
      store_application_value(
          numeric_attribute, static_cast<SQLLEN>(column_info_.size()));
    }
    return SQL_SUCCESS;
  }
  if (column_info_.empty()) {
    set_error(SQLSTATE_PREPARED_STATEMENT_NOT_CURSOR,
              "Statement does not produce a result set");
    return SQL_ERROR;
  }
  if (column_number < 1 || column_number > column_info_.size()) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid column number");
    return SQL_ERROR;
  }
  
  const auto& col = column_info_[column_number - 1];
  const auto row_descriptor = descriptor(imp_row_descriptor_);
  const auto* record = row_descriptor->record(column_number - 1);
  if (!record) {
    set_error(SQLSTATE_GENERAL_ERROR,
              "Result column descriptor is unavailable");
    return SQL_ERROR;
  }
  const std::string* text = nullptr;
  if (field_identifier == SQL_DESC_NAME ||
      field_identifier == SQL_COLUMN_NAME) {
    text = &record->name;
  } else if (field_identifier == SQL_DESC_LABEL ||
             field_identifier == SQL_COLUMN_LABEL) {
    text = &record->label;
  } else if (field_identifier == SQL_DESC_TYPE_NAME ||
             field_identifier == SQL_COLUMN_TYPE_NAME) {
    text = &record->type_name;
  } else if (field_identifier == SQL_DESC_LOCAL_TYPE_NAME) {
    text = &record->local_type_name;
  } else if (field_identifier == SQL_DESC_LITERAL_PREFIX) {
    text = &record->literal_prefix;
  } else if (field_identifier == SQL_DESC_LITERAL_SUFFIX) {
    text = &record->literal_suffix;
  }
  if (text) {
    if (character_attribute && buffer_length > 0) {
      const auto copy_len = std::min(
          static_cast<std::size_t>(buffer_length - 1), text->length());
      std::memcpy(character_attribute, text->c_str(), copy_len);
      static_cast<char*>(character_attribute)[copy_len] = '\0';
    }
    if (string_length) {
      store_application_value(
          string_length, static_cast<SQLSMALLINT>(text->length()));
    }
    if (character_attribute && !text->empty() &&
        static_cast<std::size_t>(buffer_length) <= text->length()) {
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Column attribute was truncated");
      return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  }

  if (numeric_attribute) {
    std::optional<SQLLEN> value;
    if (field_identifier == SQL_DESC_AUTO_UNIQUE_VALUE ||
        field_identifier == SQL_COLUMN_AUTO_INCREMENT) {
      value = record->auto_unique_value;
    } else if (field_identifier == SQL_DESC_CASE_SENSITIVE ||
               field_identifier == SQL_COLUMN_CASE_SENSITIVE) {
      value = record->case_sensitive;
    } else if (field_identifier == SQL_DESC_DISPLAY_SIZE ||
               field_identifier == SQL_COLUMN_DISPLAY_SIZE) {
      value = record->display_size;
    } else if (field_identifier == SQL_DESC_FIXED_PREC_SCALE ||
               field_identifier == SQL_COLUMN_MONEY) {
      value = record->fixed_prec_scale;
    } else if (field_identifier == SQL_DESC_NUM_PREC_RADIX) {
      value = record->num_prec_radix;
    } else if (field_identifier == SQL_DESC_OCTET_LENGTH) {
      value = record->octet_length;
    } else if (field_identifier == SQL_DESC_SEARCHABLE ||
               field_identifier == SQL_COLUMN_SEARCHABLE) {
      value = record->searchable;
    } else if (field_identifier == SQL_DESC_UNSIGNED ||
               field_identifier == SQL_COLUMN_UNSIGNED) {
      value = record->unsigned_attribute;
    } else if (field_identifier == SQL_DESC_UPDATABLE ||
               field_identifier == SQL_COLUMN_UPDATABLE) {
      value = record->updatable;
    } else if (field_identifier == SQL_DESC_TYPE) {
      value = record->type;
    } else if (field_identifier == SQL_DESC_CONCISE_TYPE ||
        field_identifier == SQL_COLUMN_TYPE) {
      value = record->concise_type;
    } else if (field_identifier == SQL_DESC_LENGTH) {
      value = static_cast<SQLLEN>(record->length);
    } else if (field_identifier == SQL_COLUMN_LENGTH ||
               field_identifier == SQL_COLUMN_PRECISION) {
      value = static_cast<SQLLEN>(col.column_size);
    } else if (field_identifier == SQL_DESC_PRECISION) {
      value = record->precision;
    } else if (field_identifier == SQL_DESC_SCALE) {
      value = record->scale;
    } else if (field_identifier == SQL_COLUMN_SCALE) {
      value = col.decimal_digits;
    } else if (field_identifier == SQL_DESC_NULLABLE ||
               field_identifier == SQL_COLUMN_NULLABLE) {
      value = record->nullable;
    } else if (field_identifier == SQL_DESC_UNNAMED) {
      value = record->unnamed;
    }
    if (value) {
      store_application_value(numeric_attribute, *value);
    }
  }
  return SQL_SUCCESS;
}

// Parameter metadata implementation
SQLRETURN ODBCStatement::describe_param(SQLUSMALLINT parameter_number, SQLSMALLINT* data_type,
                                        SQLULEN* parameter_size, SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable) {
  if (parameter_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid parameter number");
    return SQL_ERROR;
  }
  if (!prepared_ && !executed_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
              "Statement has not been prepared or executed");
    return SQL_ERROR;
  }
  if (parameter_number > static_cast<SQLUSMALLINT>(parameter_count_)) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid parameter number");
    return SQL_ERROR;
  }
  if (prepared_ && !executed_) {
    const auto metadata_result = describe_prepared_metadata();
    if (metadata_result != SQL_SUCCESS) return metadata_result;
  }
  if (parameter_number > param_metadata_.size()) {
    set_error(SQLSTATE_GENERAL_ERROR,
              "Parameter metadata was not returned by the data source");
    return SQL_ERROR;
  }
  
  const auto& meta = param_metadata_[parameter_number - 1];
  if (data_type) {
    store_application_value(data_type, static_cast<SQLSMALLINT>(meta.sql_type));
  }
  if (parameter_size) {
    store_application_value(parameter_size,
                            static_cast<SQLULEN>(meta.column_size));
  }
  if (decimal_digits) {
    store_application_value(
        decimal_digits, static_cast<SQLSMALLINT>(meta.decimal_digits));
  }
  if (nullable) {
    store_application_value(nullable, static_cast<SQLSMALLINT>(meta.nullable));
  }
  
  return SQL_SUCCESS;
}

// Handle registry implementation
HandleRegistry& HandleRegistry::instance() {
  static HandleRegistry registry;
  return registry;
}

void HandleRegistry::register_handle(SQLHANDLE handle,
                                     std::unique_ptr<ODBCHandle> obj,
                                     SQLHANDLE parent) {
  std::lock_guard lock(mutex_);
  handles_[handle] = {
      std::shared_ptr<ODBCHandle>(std::move(obj)), parent};
}

void HandleRegistry::unregister_handle(SQLHANDLE handle) {
  std::vector<std::shared_ptr<ODBCHandle>> removed;
  {
    std::lock_guard lock(mutex_);
    collect_subtree_locked(handle, removed);
  }
}

void HandleRegistry::unregister_children(SQLHANDLE parent) {
  std::vector<std::shared_ptr<ODBCHandle>> removed;
  {
    std::lock_guard lock(mutex_);
    std::vector<SQLHANDLE> children;
    for (const auto& [handle, entry] : handles_) {
      if (entry.parent == parent) children.push_back(handle);
    }
    for (const auto child : children) {
      collect_subtree_locked(child, removed);
    }
  }
}

bool HandleRegistry::has_children(SQLHANDLE parent) {
  std::lock_guard lock(mutex_);
  for (const auto& [handle, entry] : handles_) {
    static_cast<void>(handle);
    if (entry.parent == parent) return true;
  }
  return false;
}

std::vector<SQLHANDLE> HandleRegistry::child_handles(
    SQLHANDLE parent, HandleType type) {
  std::vector<SQLHANDLE> result;
  std::lock_guard lock(mutex_);
  for (const auto& [handle, entry] : handles_) {
    if (entry.parent == parent && entry.object->get_type() == type) {
      result.push_back(handle);
    }
  }
  return result;
}

void HandleRegistry::collect_subtree_locked(
    SQLHANDLE handle, std::vector<std::shared_ptr<ODBCHandle>>& removed) {
  std::vector<SQLHANDLE> children;
  for (const auto& [candidate, entry] : handles_) {
    if (entry.parent == handle) children.push_back(candidate);
  }
  for (const auto child : children) {
    collect_subtree_locked(child, removed);
  }
  const auto it = handles_.find(handle);
  if (it == handles_.end()) return;
  removed.push_back(std::move(it->second.object));
  handles_.erase(it);
}

std::shared_ptr<ODBCHandle> HandleRegistry::get_handle(SQLHANDLE handle) {
  std::lock_guard lock(mutex_);
  auto it = handles_.find(handle);
  return (it != handles_.end()) ? it->second.object : nullptr;
}

std::shared_ptr<ODBCConnection> HandleRegistry::get_connection_for_handle(
    SQLHANDLE handle) {
  std::lock_guard lock(mutex_);
  auto current = handle;
  while (current) {
    const auto it = handles_.find(current);
    if (it == handles_.end()) return nullptr;
    if (it->second.object->get_type() == HandleType::Connection) {
      return std::static_pointer_cast<ODBCConnection>(it->second.object);
    }
    current = it->second.parent;
  }
  return nullptr;
}

void HandleRegistry::detach_descriptor_from_statements(SQLHDESC descriptor) {
  std::vector<std::shared_ptr<ODBCStatement>> statements;
  {
    std::lock_guard lock(mutex_);
    const auto descriptor_entry = handles_.find(descriptor);
    if (descriptor_entry == handles_.end()) return;
    const auto connection = descriptor_entry->second.parent;
    for (const auto& [handle, entry] : handles_) {
      static_cast<void>(handle);
      if (entry.object->get_type() != HandleType::Statement ||
          entry.parent != connection) {
        continue;
      }
      statements.push_back(
          std::static_pointer_cast<ODBCStatement>(entry.object));
    }
  }
  for (const auto& statement : statements) {
    statement->detach_descriptor(descriptor);
  }
}

HandleOperationLease HandleRegistry::lock_handles(
    std::initializer_list<SQLHANDLE> handles) {
  HandleOperationLease lease;
  std::map<SQLHANDLE, std::shared_ptr<ODBCHandle>> ordered_handles;
  {
    std::lock_guard lock(mutex_);
    for (const auto requested_handle : handles) {
      auto current = requested_handle;
      while (current) {
        const auto it = handles_.find(current);
        if (it == handles_.end()) break;
        const auto [inserted, is_new] =
            ordered_handles.emplace(current, it->second.object);
        static_cast<void>(inserted);
        if (!is_new) break;
        if (it->second.object->get_type() == HandleType::Connection) break;
        current = it->second.parent;
      }
    }
  }

  lease.handles_.reserve(ordered_handles.size());
  lease.locks_.reserve(ordered_handles.size());
  for (auto& [handle, object] : ordered_handles) {
    static_cast<void>(handle);
    lease.handles_.push_back(object);
    lease.locks_.emplace_back(object->operation_mutex_);
  }
  return lease;
}

} // namespace rs::odbc
