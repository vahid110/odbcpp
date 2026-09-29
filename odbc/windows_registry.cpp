#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "windows_registry.h"
#include "connection_string.h"
#include <algorithm>
#include <limits>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace rs::odbc::windows_registry {
namespace {
constexpr REGSAM view = sizeof(void*) == 8 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;
constexpr DWORD maximum_value_bytes = 1024 * 1024;
struct Key {
  HKEY value{};
  ~Key() { if (value) RegCloseKey(value); }
  Key() = default;
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
};
void check(LSTATUS status) {
  if (status != ERROR_SUCCESS) throw std::system_error(
      static_cast<int>(status), std::system_category(), "Cannot read ODBC registry configuration");
}
std::wstring wide(const std::string& text) {
  if (text.empty() || text.find_first_of("\\/") != std::string::npos ||
      text.find('\0') != std::string::npos ||
      text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("Invalid ODBC registry entry name");
  }
  const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (!length) throw std::invalid_argument("Invalid UTF-8 ODBC registry entry name");
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(), length))
    throw std::invalid_argument("Invalid UTF-8 ODBC registry entry name");
  return result;
}
std::string utf8(std::wstring_view text) {
  if (text.empty()) return {};
  const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (!length) throw std::invalid_argument("Invalid UTF-16 ODBC registry value");
  std::string result(static_cast<std::size_t>(length), '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
      static_cast<int>(text.size()), result.data(), length, nullptr, nullptr))
    throw std::invalid_argument("Invalid UTF-16 ODBC registry value");
  return result;
}
bool open(HKEY root, const std::wstring& path, Key& key) {
  const auto status = RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | view, &key.value);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return false;
  check(status); return true;
}
std::optional<std::string> read_value(HKEY key, const wchar_t* name) {
  // Retry boundedly if configuration changes between the size and data reads.
  for (int attempt = 0; attempt < 3; ++attempt) {
    DWORD type = 0, bytes = 0;
    auto status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
    check(status);
    if (type == REG_DWORD) {
      DWORD number = 0;
      bytes = sizeof(number);
      status = RegQueryValueExW(key, name, nullptr, &type,
          reinterpret_cast<BYTE*>(&number), &bytes);
      if (status == ERROR_MORE_DATA) continue;
      check(status);
      if (type != REG_DWORD || bytes != sizeof(number))
        throw std::invalid_argument("Malformed ODBC registry DWORD");
      return std::to_string(number);
    }
    if (type != REG_SZ || bytes < sizeof(wchar_t) ||
        bytes % sizeof(wchar_t) != 0 || bytes > maximum_value_bytes)
      throw std::invalid_argument("ODBC registry attributes must be REG_SZ strings or DWORDs");
    std::vector<wchar_t> data(bytes / sizeof(wchar_t));
    status = RegQueryValueExW(key, name, nullptr, &type,
        reinterpret_cast<BYTE*>(data.data()), &bytes);
    if (status == ERROR_MORE_DATA) continue;
    check(status);
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0 ||
        bytes / sizeof(wchar_t) > data.size())
      throw std::invalid_argument("ODBC registry attribute changed during read");
    const auto count = bytes / sizeof(wchar_t);
    if (data[count - 1] != L'\0' ||
        std::find(data.begin(), data.begin() + count - 1, L'\0') != data.begin() + count - 1)
      throw std::invalid_argument("Malformed ODBC registry string");
    return utf8(std::wstring_view(data.data(), count - 1));
  }
  throw std::runtime_error("ODBC registry configuration changed during read");
}
std::optional<std::map<std::string, std::string>> read_section(HKEY root, const std::wstring& path) {
  Key key;
  if (!open(root, path, key)) return std::nullopt;
  std::map<std::string, std::string> result;
  // Registry value names are limited to 16,383 UTF-16 characters.
  std::vector<wchar_t> name(16384);
  for (DWORD index = 0; ; ++index) {
    DWORD size = static_cast<DWORD>(name.size());
    const auto status = RegEnumValueW(key.value, index, name.data(), &size,
                                      nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    check(status);
    if (size == 0) continue; // unnamed default value is not an ODBC attribute
    const auto value = read_value(key.value, name.data());
    if (!value) throw std::runtime_error("ODBC registry attribute disappeared during read");
    result[ConnectionString::to_upper(utf8(std::wstring_view(name.data(), size)))] = *value;
  }
  return result;
}
}

std::optional<std::map<std::string, std::string>> load_dsn(const std::string& name) {
  if (ConnectionString::to_upper(name) == "ODBC DATA SOURCES")
    throw std::invalid_argument("Reserved ODBC registry DSN name");
  const auto entry = wide(name);
  for (const auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    auto attributes = read_section(root, L"SOFTWARE\\ODBC\\ODBC.INI\\" + entry);
    if (!attributes) continue;
    Key sources;
    if (open(root, L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources", sources)) {
      const auto driver = read_value(sources.value, entry.c_str());
      if (driver && !driver->empty()) (*attributes)["DRIVER"] = *driver;
    }
    return attributes;
  }
  return std::nullopt;
}
std::optional<std::map<std::string, std::string>> load_driver(const std::string& name) {
  // A DSN may contain a DLL path instead of a registered driver name. It is not
  // a registry subkey; callers use the configured product registration instead.
  if (name.find_first_of("\\/") != std::string::npos) return std::nullopt;
  return read_section(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\" + wide(name));
}
} // namespace rs::odbc::windows_registry
#endif
