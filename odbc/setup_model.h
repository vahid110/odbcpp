#pragma once

#include "odbcpp/database/backend_provider.h"
#include "odbcpp/util/utf8.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rs::odbc::setup {
using Attributes = std::map<std::wstring, std::wstring>;

inline std::wstring from_utf8(std::string_view text) {
  std::wstring result;
  for (std::size_t i = 0; i < text.size();) {
    const auto scalar = rs::util::next_utf8_code_point(text, i);
    if (!scalar) throw std::invalid_argument("Invalid UTF-8 text.");
    auto cp = *scalar;
    if constexpr (sizeof(wchar_t) == 2) {
      if (cp > 0xffff) {
        cp -= 0x10000;
        result.push_back(static_cast<wchar_t>(0xd800 + (cp >> 10)));
        result.push_back(static_cast<wchar_t>(0xdc00 + (cp & 0x3ff)));
      } else result.push_back(static_cast<wchar_t>(cp));
    } else result.push_back(static_cast<wchar_t>(cp));
  }
  return result;
}
inline std::wstring driver_name() {
  return from_utf8(rs::core::database::configured_backend_provider().identity().driver_name);
}
inline void secure_clear(std::wstring& value) noexcept {
  volatile wchar_t* data = value.empty() ? nullptr : value.data();
  for (std::size_t i = 0; i < value.size(); ++i) data[i] = L'\0';
  value.clear();
}

struct Fields {
  std::wstring dsn, server, port, database, user, password;
  bool ssl = true;
  // Absence inherits the driver default; an empty value is explicit.
  Attributes advanced;
  std::set<std::wstring> removed;
  Fields() {
    const auto& defaults = rs::core::database::configured_backend_provider().connection_defaults();
    server = from_utf8(defaults.host);
    port = std::to_wstring(defaults.port);
    database = from_utf8(defaults.database.value_or(""));
    ssl = defaults.use_ssl;
  }
  ~Fields() { secure_clear(password); }
};

enum class PropertyKind { Text, Boolean, Unsigned, Choice };
struct Property {
  std::wstring key, label, description;
  PropertyKind kind;
  std::uint64_t maximum = 0;
  std::vector<std::wstring> choices{};
};
inline std::vector<Property> properties() {
  using Kind = PropertyKind;
  std::vector<Property> result{
      {L"SSLCAFILE", L"CA certificate file", L"Trusted CA certificate file used to verify the server.", Kind::Text},
      {L"SSLCADIR", L"CA certificate directory", L"Directory of trusted CA certificates.", Kind::Text},
      {L"APPLICATIONNAME", L"Application name", L"Client application name sent to the server.", Kind::Text},
      {L"LOGINTIMEOUT", L"Login timeout (seconds)", L"Connection timeout; zero disables the timeout.", Kind::Unsigned, UINT32_MAX},
      {L"QUERYTIMEOUT", L"Query timeout (seconds)", L"Default statement timeout; zero disables the timeout.", Kind::Unsigned, UINTPTR_MAX},
      {L"KEEPALIVE", L"TCP keepalive", L"Enable TCP connection health probes.", Kind::Boolean},
      {L"KEEPALIVEIDLE", L"Keepalive idle (seconds)", L"Idle time before probes; zero uses the system default.", Kind::Unsigned, INT32_MAX},
      {L"KEEPALIVEINTERVAL", L"Keepalive interval (seconds)", L"Time between probes; zero uses the system default.", Kind::Unsigned, INT32_MAX},
      {L"KEEPALIVECOUNT", L"Keepalive probe count", L"Unanswered probes before failure; zero uses the system default.", Kind::Unsigned, 255},
      {L"LOGLEVEL", L"Log level", L"Logging verbosity; detailed logs may contain operational information.", Kind::Choice, 0, {L"Off", L"Error", L"Warn", L"Info", L"Debug", L"Trace"}},
      {L"LOGFILE", L"Log file", L"Destination file when the file logging sink is enabled.", Kind::Text}};
  const auto support = rs::core::database::configured_backend_provider().supported_connection_options();
  if (support.read_only)
    result.push_back({L"READONLY", L"Read-only session", L"Request a read-only session.", Kind::Boolean});
  if (support.character_metadata) {
    result.push_back({L"MAXVARCHARSIZE", L"VARCHAR metadata threshold", L"Character metadata threshold; this does not limit result data.", Kind::Unsigned, INT32_MAX});
    result.push_back({L"MAXLONGVARCHARSIZE", L"Long VARCHAR metadata size", L"Reported long character size; this does not limit result data.", Kind::Unsigned, 65535});
  }
  if (support.boolean_as_character)
    result.push_back({L"BOOLSASCHAR", L"Boolean values as characters", L"Expose boolean result metadata as character data.", Kind::Boolean});
  if (support.detailed_table_types)
    result.push_back({L"ENABLETABLETYPES", L"Detailed table types", L"Expose detailed table types in catalog metadata.", Kind::Boolean});
  if (support.catalog_database_scope)
    result.push_back({L"DATABASEMETADATACURRENTDBONLY", L"Current database metadata only", L"Restrict catalog metadata to the current database.", Kind::Boolean});
  if (support.catalog_mode)
    result.push_back({L"REDSHIFTCATALOGMODE", L"Catalog mode", L"Select the backend catalog discovery implementation.", Kind::Choice, 0, {L"SHOW", L"LEGACY"}});
  return result;
}
inline std::wstring uppercase(std::wstring value) {
  // ODBC keywords and symbolic values are ASCII, independent of locale.
  for (auto& ch : value) if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
  return value;
}
inline std::wstring trim(std::wstring value) {
  const auto first = value.find_first_not_of(L" \t\r\n\f\v");
  if (first == std::wstring::npos) return {};
  return value.substr(first, value.find_last_not_of(L" \t\r\n\f\v") - first + 1);
}
inline std::wstring canonical_key(std::wstring key) {
  key = uppercase(std::move(key));
  for (const auto& pair : {std::pair{L"HOST", L"SERVER"}, {L"DB", L"DATABASE"},
       {L"USER", L"UID"}, {L"PASSWORD", L"PWD"}, {L"LT", L"LOGINTIMEOUT"}, {L"QT", L"QUERYTIMEOUT"}}) {
    if (key == pair.first) return pair.second;
  }
  return key;
}
inline bool safe_key(const std::wstring& key) {
  return !key.empty() && key.size() <= 128 &&
      key.find_first_not_of(L"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::wstring::npos;
}
inline bool routing_or_credentials(const std::wstring& key) {
  return key == L"DSN" || key == L"DRIVER" || key == L"FILEDSN" || key == L"SAVEFILE" ||
      key == L"PWD" || key == L"PASSWORD";
}
inline bool basic_key(const std::wstring& key) {
  return key == L"SERVER" || key == L"PORT" || key == L"DATABASE" || key == L"UID" || key == L"SSL";
}
inline bool backend_property(const std::wstring& key) {
  return key == L"READONLY" || key == L"MAXVARCHARSIZE" || key == L"MAXLONGVARCHARSIZE" ||
      key == L"BOOLSASCHAR" || key == L"ENABLETABLETYPES" ||
      key == L"DATABASEMETADATACURRENTDBONLY" || key == L"REDSHIFTCATALOGMODE";
}
inline bool supported_property(const std::wstring& key) {
  const auto canonical = canonical_key(key);
  for (const auto& property : properties()) if (property.key == canonical) return true;
  return false;
}
inline bool parse_boolean(const std::wstring& value) {
  const auto selected = uppercase(trim(value));
  if (selected == L"1" || selected == L"TRUE") return true;
  if (selected == L"0" || selected == L"FALSE") return false;
  throw std::invalid_argument("Enter a valid Boolean setting (0, 1, false or true).");
}
inline bool parse_ssl(const std::wstring& value) {
  const auto selected = uppercase(trim(value));
  if (selected == L"YES" || selected == L"ON") return true;
  if (selected == L"NO" || selected == L"OFF") return false;
  return parse_boolean(value);
}
// Each precedence layer is independent; primary spelling wins within a layer.
inline void overlay(Fields& fields, const Attributes& input) {
  Attributes layer;
  for (const auto& [key, value] : input) layer[uppercase(key)] = value;
  Attributes normalized;
  for (const auto& [key, value] : layer) {
    const auto canonical = canonical_key(key);
    if (canonical != key && layer.count(canonical) != 0) continue;
    normalized[canonical] = value;
  }
  for (const auto& [key, value] : normalized) {
    if (key == L"DSN") fields.dsn = value;
    else if (key == L"SERVER") fields.server = value;
    else if (key == L"PORT") fields.port = value;
    else if (key == L"DATABASE") fields.database = value;
    else if (key == L"UID") fields.user = value;
    else if (key == L"PWD") { secure_clear(fields.password); fields.password = value; }
    else if (key == L"SSL") fields.ssl = parse_ssl(value);
    else if (!routing_or_credentials(key) && safe_key(key)) {
      fields.advanced[key] = value;
      fields.removed.erase(key);
    }
  }
  for (auto& [key, value] : layer) {
    if (key == L"PWD" || key == L"PASSWORD") secure_clear(value);
  }
  const auto password = normalized.find(L"PWD");
  if (password != normalized.end()) secure_clear(password->second);
}
inline void validate_name(const std::wstring& name) {
  if (name.empty() || name.size() > 32 ||
      name.find_first_of(L"[]{}(),;?*=!@\\/") != std::wstring::npos ||
      uppercase(name) == L"ODBC DATA SOURCES" ||
      std::any_of(name.begin(), name.end(), [](wchar_t c) { return c < 32; }))
    throw std::invalid_argument("Enter a valid DSN name (1-32 characters).");
}
inline void validate_text(const std::wstring& value) {
  if (value.size() > 1024 || std::any_of(value.begin(), value.end(), [](wchar_t c) { return c < 32 || c == 127; }))
    throw std::invalid_argument("Fields must contain at most 1024 characters and no control characters.");
}
inline void validate_unsigned(const std::wstring& raw, std::uint64_t maximum) {
  const auto value = trim(raw);
  if (value.empty()) throw std::invalid_argument("Enter a valid non-negative integer within the supported range.");
  std::uint64_t number = 0;
  for (const auto ch : value) {
    if (ch < L'0' || ch > L'9' || number > maximum / 10 ||
        (number == maximum / 10 && static_cast<std::uint64_t>(ch - L'0') > maximum % 10))
      throw std::invalid_argument("Enter a valid non-negative integer within the supported range.");
    number = number * 10 + static_cast<std::uint64_t>(ch - L'0');
  }
}
inline void validate(const Fields& fields) {
  validate_name(fields.dsn);
  for (const auto* value : {&fields.dsn, &fields.server, &fields.database, &fields.user, &fields.password}) validate_text(*value);
  if (fields.server.empty() || fields.database.empty()) throw std::invalid_argument("Server and database are required.");
  if (fields.port.empty() || fields.port.size() > 5 || fields.port == L"0" ||
      fields.port.find_first_not_of(L"0123456789") != std::wstring::npos)
    throw std::invalid_argument("Port must be between 1 and 65535.");
  validate_unsigned(fields.port, 65535);
  if (std::stoul(fields.port) == 0) throw std::invalid_argument("Port must be between 1 and 65535.");
  const auto available = properties();
  for (const auto& [raw_key, value] : fields.advanced) {
    const auto key = canonical_key(raw_key);
    if (fields.removed.count(key) != 0 || routing_or_credentials(key) || basic_key(key)) continue;
    if (!safe_key(key)) throw std::invalid_argument("An advanced setting has an invalid keyword.");
    const auto property = std::find_if(available.begin(), available.end(), [&](const auto& item) { return item.key == key; });
    if (property == available.end()) {
      if (backend_property(key)) throw std::invalid_argument("An advanced setting is unavailable for this driver.");
      // Unexposed stored options are round-tripped, including multiline text.
      // The installer API bounds each value and cannot represent embedded NULs.
      if (value.size() >= 32767 || value.find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("A stored advanced setting exceeds the installer limits.");
      continue;
    }
    validate_text(value);
    if (property->kind == PropertyKind::Boolean) (void)parse_boolean(value);
    else if (property->kind == PropertyKind::Unsigned) validate_unsigned(value, property->maximum);
    else if (property->kind == PropertyKind::Choice) {
      auto selected = uppercase(key == L"REDSHIFTCATALOGMODE" ? value : trim(value));
      if (key == L"LOGLEVEL" && selected == L"WARNING") selected = L"WARN";
      if (std::none_of(property->choices.begin(), property->choices.end(), [&](const auto& choice) { return uppercase(choice) == selected; }))
        throw std::invalid_argument("Choose a supported value for the advanced setting.");
    }
  }
  const auto has_trust = [&](const wchar_t* selected) {
    for (const auto& [key, value] : fields.advanced) {
      const auto canonical = canonical_key(key);
      if (canonical == selected && fields.removed.count(canonical) == 0 && !value.empty()) return true;
    }
    return false;
  };
  const auto ca_file = has_trust(L"SSLCAFILE");
  const auto ca_directory = has_trust(L"SSLCADIR");
  if (ca_file && ca_directory)
    throw std::invalid_argument("Choose either a CA certificate file or a CA certificate directory.");
  if (!fields.ssl && (ca_file || ca_directory))
    throw std::invalid_argument("Enable TLS to use a CA certificate file or directory.");
}
inline Attributes persisted(const Fields& fields) {
  validate(fields);
  Attributes result{{L"Server", fields.server}, {L"Port", fields.port}, {L"Database", fields.database},
      {L"UID", fields.user}, {L"SSL", fields.ssl ? L"1" : L"0"}};
  for (const auto& [raw_key, value] : fields.advanced) {
    const auto key = canonical_key(raw_key);
    if (fields.removed.count(key) == 0 && !routing_or_credentials(key) && !basic_key(key)) result[key] = value;
  }
  return result;
}
inline std::wstring braced(const std::wstring& value) {
  std::wstring output = L"{";
  for (auto c : value) { output += c; if (c == L'}') output += c; }
  return output + L"}";
}
inline std::wstring connection_string(const Fields& fields) {
  auto result = L"DRIVER=" + braced(driver_name()) + L";";
  for (const auto& [key, value] : persisted(fields)) result += key + L"=" + braced(value) + L";";
  return result + L"PWD=" + braced(fields.password) + L";";
}
}  // namespace rs::odbc::setup
