#define NOMINMAX
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <odbcinst.h>
#include "../setup_model.h"
#include "setup_dialog.h"
#include <mutex>
#include <optional>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <vector>

namespace {
using namespace rs::odbc::setup;
HINSTANCE module;
std::recursive_mutex mutex;
constexpr auto ini = L"ODBC.INI";
struct WipeText {
  std::wstring& value;
  ~WipeText() { secure_clear(value); }
};
struct WipeAttributes {
  Attributes& values;
  ~WipeAttributes() {
    for (auto& [key, value] : values) {
      if (key == L"PWD" || key == L"PASSWORD") secure_clear(value);
    }
  }
};

std::wstring upper(std::wstring value) {
  for (auto& c : value) c = std::towupper(c);
  return value;
}

bool input_key(const std::wstring& key) {
  if (key == L"DSN" || key == L"SERVER" || key == L"HOST" || key == L"PORT" ||
      key == L"DATABASE" || key == L"DB" || key == L"UID" || key == L"USER" ||
      key == L"PWD" || key == L"PASSWORD" || key == L"SSL" ||
      key == L"LT" || key == L"QT") return true;
  for (const auto& property : properties()) {
    if (property.key == key) return true;
  }
  return false;
}

Attributes attributes(const wchar_t* input) {
  Attributes result;
  if (!input) return result;
  std::size_t total = 0;
  for (const auto* p = input; *p; p += std::wcslen(p) + 1) {
    std::wstring entry(p);
    WipeText wipe{entry};
    total += entry.size() + 1;
    if (total > 1024 * 1024) throw std::invalid_argument("Setup attributes are too long.");
    const auto equals = entry.find(L'=');
    if (equals == std::wstring::npos) throw std::invalid_argument("Invalid setup attribute.");
    const auto key = upper(entry.substr(0, equals));
    if (!input_key(key)) throw std::invalid_argument("Unsupported setup attribute.");
    if (!result.emplace(key, entry.substr(equals + 1)).second)
      throw std::invalid_argument("Duplicate setup attribute.");
  }
  return result;
}

std::optional<std::wstring> read(const std::wstring& section, const wchar_t* key) {
  constexpr auto missing = L"ODBCPP_MISSING_VALUE_A";
  std::vector<wchar_t> value(32768);
  const auto size = SQLGetPrivateProfileStringW(section.c_str(), key, missing,
      value.data(), static_cast<int>(value.size()), ini);
  if (size >= 32767) throw std::runtime_error("Stored setting is too long.");
  if (std::wstring(value.data()) == missing) {
    constexpr auto second = L"ODBCPP_MISSING_VALUE_B";
    SQLGetPrivateProfileStringW(section.c_str(), key, second, value.data(),
                                static_cast<int>(value.size()), ini);
    if (std::wstring(value.data()) == second) return std::nullopt;
  }
  return std::wstring(value.data());
}

Attributes read_settings(const std::wstring& section) {
  // Keep the selected installer scope, rather than runtime User-first selection.
  std::vector<wchar_t> names(4096);
  for (;;) {
    const auto size = SQLGetPrivateProfileStringW(section.c_str(), nullptr, L"", names.data(),
                                                static_cast<int>(names.size()), ini);
    if (size < static_cast<int>(names.size()) - 2) break;
    if (names.size() >= 1024 * 1024)
      throw std::runtime_error("Too many stored data source settings.");
    names.resize(names.size() * 2);
  }
  Attributes result;
  std::size_t total = 0;
  for (const auto* key = names.data(); *key; key += std::wcslen(key) + 1) {
    const auto canonical = upper(key);
    if (canonical == L"PWD" || canonical == L"PASSWORD" ||
        canonical == L"DSN" || canonical == L"DRIVER") continue;
    if (auto value = read(section, key)) {
      total += canonical.size() + value->size();
      if (total > 1024 * 1024) throw std::runtime_error("Stored settings are too long.");
      result[canonical] = std::move(*value);
    }
  }
  return result;
}

bool save(const Fields& fields, bool add) {
  const auto values = persisted(fields);
  std::vector<std::wstring> owned{L"Server", L"Port", L"Database", L"UID", L"SSL"};
  for (const auto& property : properties()) owned.push_back(property.key);
  for (const auto key : {L"LT", L"QT", L"PWD", L"PASSWORD"}) owned.emplace_back(key);
  std::map<std::wstring, std::optional<std::wstring>> before;
  struct WipeSnapshot {
    decltype(before)& values;
    ~WipeSnapshot() {
      for (auto& [key, value] : values) {
        if (value && (key == L"PWD" || key == L"PASSWORD")) secure_clear(*value);
      }
    }
  } wipe_snapshot{before};
  if (!add) {
    for (const auto& key : owned) before[key] = read(fields.dsn, key.c_str());
  }
  const auto driver = driver_name();
  if (add && !SQLWriteDSNToIniW(fields.dsn.c_str(), driver.c_str()))
    throw std::runtime_error("Cannot register DSN with the installer.");
  bool ok = true;
  for (const auto& key : owned) {
    const auto canonical = upper(key);
    const auto found = values.find(key);
    const auto normalized = values.find(canonical);
    const auto selected = found != values.end() ? found : normalized;
    const auto* desired = selected != values.end() ? selected->second.c_str() : nullptr;
    // Only owned keys change. Unknown existing settings remain untouched.
    if (desired || (!add && before.at(key))) {
      if (!SQLWritePrivateProfileStringW(fields.dsn.c_str(), key.c_str(), desired, ini)) {
        ok = false;
        break;
      }
    }
  }
  if (!ok) {
    if (add) SQLRemoveDSNFromIniW(fields.dsn.c_str());
    else {
      for (const auto& [key, value] : before) {
        SQLWritePrivateProfileStringW(fields.dsn.c_str(), key.c_str(),
                                      value ? value->c_str() : nullptr, ini);
      }
    }
    throw std::runtime_error("Cannot write data source settings or remove legacy credentials.");
  }
  return true;
}

BOOL configure(HWND parent, WORD request, const wchar_t* driver, const wchar_t* input) {
  std::lock_guard lock(mutex);
  UWORD original = ODBC_BOTH_DSN;
  if (!SQLGetConfigMode(&original)) return FALSE;
  struct Restore {
    UWORD mode;
    bool active = true;
    ~Restore() { if (active) SQLSetConfigMode(mode); }
  } restore{original};
  auto mode = original == ODBC_SYSTEM_DSN ? ODBC_SYSTEM_DSN : ODBC_USER_DSN;
  if (request >= ODBC_ADD_SYS_DSN && request <= ODBC_REMOVE_SYS_DSN) {
    mode = ODBC_SYSTEM_DSN;
    request -= ODBC_ADD_SYS_DSN - ODBC_ADD_DSN;
  }
  try {
    const auto expected_driver = driver_name();
    if (!driver || std::wstring(driver) != expected_driver ||
        request < ODBC_ADD_DSN || request > ODBC_REMOVE_DSN)
      throw std::invalid_argument("Unsupported setup request or driver.");
    if (!SQLSetConfigMode(mode)) throw std::runtime_error("Cannot select DSN scope.");
    auto supplied = attributes(input);
    WipeAttributes wipe_supplied{supplied};
    Fields fields;
    overlay(fields, supplied);
    const bool add = request == ODBC_ADD_DSN;
    const bool fixed_name = !fields.dsn.empty();
    if (fixed_name || !add) {
      validate_name(fields.dsn);
      if (!SQLValidDSNW(fields.dsn.c_str())) throw std::invalid_argument("Invalid DSN name.");
    }
    const auto owner = fields.dsn.empty() ? std::nullopt :
        read(L"ODBC Data Sources", fields.dsn.c_str());
    if ((add && owner) || (!add && (!owner || *owner != expected_driver)))
      throw std::invalid_argument("DSN already exists or does not belong to this driver.");
    if (!add) {
      if (request == ODBC_REMOVE_DSN) return SQLRemoveDSNFromIniW(fields.dsn.c_str());
      overlay(fields, read_settings(fields.dsn));
      overlay(fields, supplied);
    }
    if (parent && !edit_dialog(module, parent, fields, fixed_name, mode == ODBC_SYSTEM_DSN))
      return FALSE;
    validate(fields);
    if (!SQLValidDSNW(fields.dsn.c_str())) throw std::invalid_argument("Invalid DSN name.");
    if (add) {
      wchar_t existing[4]{};
      if (read(L"ODBC Data Sources", fields.dsn.c_str()) ||
          SQLGetPrivateProfileStringW(fields.dsn.c_str(), nullptr, L"", existing, 4, ini) > 0)
        throw std::invalid_argument("DSN already exists.");
    }
    return save(fields, add) ? TRUE : FALSE;
  } catch (const std::exception& error) {
    const std::string reason(error.what());
    const std::wstring message(reason.begin(), reason.end());
    SQLSetConfigMode(original);
    restore.active = false;
    SQLPostInstallerErrorW(ODBC_ERROR_REQUEST_FAILED, message.c_str());
    if (parent) MessageBoxW(parent, L"Could not save the data source. Check its settings and registry permissions.",
                            L"ODBCPP Data Source Setup", MB_OK | MB_ICONERROR);
    return FALSE;
  }
}

std::wstring ansi(const char* value) {
  if (!value) return {};
  const int size = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
  if (!size) throw std::invalid_argument("Invalid ANSI setup input.");
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), size);
  result.pop_back();
  return result;
}
}  // namespace

extern "C" BOOL INSTAPI ConfigDSNW(HWND parent, WORD request, LPCWSTR driver, LPCWSTR input) {
  return configure(parent, request, driver, input);
}
extern "C" BOOL INSTAPI ConfigDSN(HWND parent, WORD request, LPCSTR driver, LPCSTR input) {
  try {
    std::wstring converted;
    WipeText wipe{converted};
    if (input) {
      for (auto p = input; *p; p += std::strlen(p) + 1) {
        converted += ansi(p);
        converted += L'\0';
      }
    }
    converted += L'\0';
    return configure(parent, request, ansi(driver).c_str(), converted.c_str());
  } catch (...) { return FALSE; }
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) module = instance;
  return TRUE;
}
