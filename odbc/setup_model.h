#pragma once
#include <map>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cwctype>

namespace rs::odbc::setup {
using Attributes = std::map<std::wstring, std::wstring>;
inline constexpr const wchar_t* driver_name = L"ODBCPP PostgreSQL";
struct Fields {
  std::wstring dsn, server, port = L"5432", database, user, password;
  bool ssl = true;
  ~Fields() { std::fill(password.begin(), password.end(), L'\0'); }
};
inline void validate_name(const std::wstring& name) {
  auto canonical=name;for(auto& c:canonical)c=std::towupper(c);
  if (name.empty() || name.size() > 32 ||
      name.find_first_of(L"[]{}(),;?*=!@\\/") != std::wstring::npos ||
      canonical == L"ODBC DATA SOURCES" ||
      std::any_of(name.begin(),name.end(),[](wchar_t c){return c<32;}))
    throw std::invalid_argument("Enter a valid DSN name (1-32 characters).");
}
inline void validate(const Fields& f) {
  validate_name(f.dsn);
  for (const auto* value : {&f.dsn, &f.server, &f.database, &f.user, &f.password}) {
    if (value->size() > 1024 || std::any_of(value->begin(), value->end(), [](wchar_t c) { return c < 32; }))
      throw std::invalid_argument("Fields must contain at most 1024 characters and no control characters.");
  }
  if (f.server.empty() || f.database.empty()) throw std::invalid_argument("Server and database are required.");
  if (f.port.empty() || f.port.size() > 5 ||
      f.port.find_first_not_of(L"0123456789") != std::wstring::npos ||
      std::stoul(f.port) == 0 || std::stoul(f.port) > 65535)
    throw std::invalid_argument("Port must be between 1 and 65535.");
}
inline Attributes persisted(const Fields& f) {
  validate(f);
  return {{L"Server", f.server}, {L"Port", f.port}, {L"Database", f.database},
          {L"UID", f.user}, {L"SSL", f.ssl ? L"1" : L"0"}};
}
inline std::wstring braced(const std::wstring& value) {
  std::wstring output = L"{";
  for (auto c : value) { output += c; if (c == L'}') output += c; }
  return output + L"}";
}
inline std::wstring connection_string(const Fields& f) {
  auto result = L"DRIVER=" + braced(driver_name) + L";";
  for (const auto& [key, value] : persisted(f)) result += key + L"=" + braced(value) + L";";
  return result + L"PWD=" + braced(f.password) + L";";
}
}
