#define NOMINMAX
#include <windows.h>
#include <sql.h>
#include <odbcinst.h>
#include <gtest/gtest.h>
#include <cwchar>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
#ifdef ODBCPP_TEST_REDSHIFT_PACKAGE
constexpr auto driver = L"ODBCPP Redshift";
constexpr auto ansi_driver = "ODBCPP Redshift";
constexpr auto other_driver = L"ODBCPP PostgreSQL";
constexpr auto other_ansi_driver = "ODBCPP PostgreSQL";
constexpr auto product = L"Redshift";
constexpr auto prefix = L"ODBCPP_Reg_RS_";
constexpr auto ansi_prefix = "ODBCPP_Reg_RS_";
#else
constexpr auto driver = L"ODBCPP PostgreSQL";
constexpr auto ansi_driver = "ODBCPP PostgreSQL";
constexpr auto other_driver = L"ODBCPP Redshift";
constexpr auto other_ansi_driver = "ODBCPP Redshift";
constexpr auto product = L"PostgreSQL";
constexpr auto prefix = L"ODBCPP_Reg_PG_";
constexpr auto ansi_prefix = "ODBCPP_Reg_PG_";
#endif

std::optional<std::wstring> value(HKEY hive, const std::wstring& path,
                                const wchar_t* name) {
  HKEY key = nullptr;
  const auto opened = RegOpenKeyExW(hive, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key);
  if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return std::nullopt;
  if (opened != ERROR_SUCCESS) throw std::runtime_error("Registry open failed");
  struct Close { HKEY key; ~Close() { RegCloseKey(key); } } close{key};
  wchar_t buffer[32768]{};
  DWORD type = 0, bytes = sizeof(buffer);
  const auto code = RegQueryValueExW(key, name, nullptr, &type,
      reinterpret_cast<BYTE*>(buffer), &bytes);
  if (code == ERROR_FILE_NOT_FOUND) return std::nullopt;
  if (code != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t) ||
      bytes > sizeof(buffer) || bytes % sizeof(wchar_t) || buffer[bytes / sizeof(wchar_t) - 1]) {
    throw std::runtime_error("Registry string read failed");
  }
  return std::wstring(buffer, bytes / sizeof(wchar_t) - 1);
}

std::wstring attributes(std::initializer_list<std::wstring> rows) {
  std::wstring result;
  for (const auto& row : rows) { result += row; result += L'\0'; }
  result += L'\0';
  return result;
}

class InstalledSetup : public testing::Test {
 protected:
  std::wstring name, section;
  std::string ascii_name;
  UWORD original = ODBC_BOTH_DSN;
  bool mode_captured = false;
  bool clean_before = false;
  HMODULE setup = nullptr;
  using WideConfig = BOOL(INSTAPI*)(HWND, WORD, LPCWSTR, LPCWSTR);
  using AnsiConfig = BOOL(INSTAPI*)(HWND, WORD, LPCSTR, LPCSTR);
  WideConfig wide = nullptr;
  AnsiConfig ansi = nullptr;
  static constexpr auto ini = L"SOFTWARE\\ODBC\\ODBC.INI\\";
  static constexpr auto mappings = L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources";

  void SetUp() override {
    ASSERT_TRUE(SQLGetConfigMode(&original));
    mode_captured = true;
    ASSERT_TRUE(SQLSetConfigMode(ODBC_USER_DSN));
    name = std::wstring(prefix) + std::to_wstring(GetCurrentProcessId());
    ascii_name = std::string(ansi_prefix) + std::to_string(GetCurrentProcessId());
    if (std::string(testing::UnitTest::GetInstance()->current_test_info()->name()) ==
        "WideInstallerAddConfigureAndRemove") {
      name += L"_\u8868";
    }
    section = std::wstring(ini) + name;
    HKEY key = nullptr;
    const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, section.c_str(), 0,
        KEY_READ | KEY_WOW64_64KEY, &key);
    if (key) { RegCloseKey(key); }
    ASSERT_EQ(opened, ERROR_FILE_NOT_FOUND) << "Refuse an existing fixture section";
    ASSERT_FALSE(value(HKEY_CURRENT_USER, mappings, name.c_str()).has_value());
    ASSERT_FALSE(value(HKEY_LOCAL_MACHINE, mappings, name.c_str()).has_value());
    clean_before = true;
    const auto registration = std::wstring(L"SOFTWARE\\ODBC\\ODBCINST.INI\\") + driver;
    const auto installed = value(HKEY_LOCAL_MACHINE, registration, L"Setup");
    ASSERT_TRUE(installed.has_value());
    wchar_t program_files[32768]{};
    ASSERT_GT(GetEnvironmentVariableW(L"ProgramFiles", program_files, 32768), 0u);
    const auto expected = std::wstring(program_files) + L"\\ODBCPP\\" + product + L"\\odbcpp_setup.dll";
    ASSERT_EQ(_wcsicmp(installed->c_str(), expected.c_str()), 0);
    setup = LoadLibraryExW(installed->c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(setup, nullptr) << GetLastError();
    wide = reinterpret_cast<WideConfig>(GetProcAddress(setup, "ConfigDSNW"));
    ansi = reinterpret_cast<AnsiConfig>(GetProcAddress(setup, "ConfigDSN"));
    ASSERT_NE(wide, nullptr);
    ASSERT_NE(ansi, nullptr);
  }

  void TearDown() override {
    if (clean_before) {
      EXPECT_TRUE(SQLSetConfigMode(ODBC_USER_DSN));
      const auto owner = value(HKEY_CURRENT_USER, mappings, name.c_str());
      if (owner) {
        EXPECT_EQ(*owner, driver) << "Refuse cleanup of another owner";
        if (*owner == driver && wide) {
          EXPECT_TRUE(wide(nullptr, ODBC_REMOVE_DSN, driver, attributes({L"DSN=" + name}).c_str()));
        }
      }
      EXPECT_FALSE(value(HKEY_CURRENT_USER, mappings, name.c_str()).has_value());
      HKEY key = nullptr;
      const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, section.c_str(), 0,
          KEY_READ | KEY_WOW64_64KEY, &key);
      if (key) { RegCloseKey(key); }
      EXPECT_EQ(opened, ERROR_FILE_NOT_FOUND);
    }
    if (setup) { FreeLibrary(setup); }
    if (mode_captured) {
      EXPECT_TRUE(SQLSetConfigMode(original));
    }
  }

  void expect_stored(const std::wstring& database) {
    EXPECT_EQ(value(HKEY_CURRENT_USER, mappings, name.c_str()), std::optional<std::wstring>(driver));
    EXPECT_EQ(value(HKEY_CURRENT_USER, section, L"Server"), std::optional<std::wstring>(L"example.invalid"));
    EXPECT_EQ(value(HKEY_CURRENT_USER, section, L"Port"), std::optional<std::wstring>(L"15439"));
    EXPECT_EQ(value(HKEY_CURRENT_USER, section, L"Database"), std::optional<std::wstring>(database));
    EXPECT_EQ(value(HKEY_CURRENT_USER, section, L"UID"), std::optional<std::wstring>(L"fixture_user"));
    EXPECT_FALSE(value(HKEY_CURRENT_USER, section, L"PWD").has_value());
    EXPECT_FALSE(value(HKEY_CURRENT_USER, section, L"PASSWORD").has_value());
  }
};

TEST_F(InstalledSetup, WideInstallerAddConfigureAndRemove) {
  const auto initial = attributes({L"DSN=" + name, L"SERVER=example.invalid", L"PORT=15439",
      L"DATABASE=caf\u00e9 \u8868", L"UID=fixture_user", L"PWD=synthetic-only", L"SSL=1"});
  ASSERT_TRUE(SQLConfigDataSourceW(nullptr, ODBC_ADD_DSN, driver, initial.c_str()));
  expect_stored(L"caf\u00e9 \u8868");
  ASSERT_TRUE(SQLConfigDataSourceW(nullptr, ODBC_CONFIG_DSN, driver,
      attributes({L"DSN=" + name, L"DATABASE=updated \u8868"}).c_str()));
  expect_stored(L"updated \u8868");
  ASSERT_TRUE(SQLConfigDataSourceW(nullptr, ODBC_REMOVE_DSN, driver, attributes({L"DSN=" + name}).c_str()));
  EXPECT_FALSE(value(HKEY_CURRENT_USER, mappings, name.c_str()).has_value());
}

TEST_F(InstalledSetup, AnsiInstallerAddConfigureAndRemove) {
  std::string initial = "DSN=" + ascii_name;
  for (const auto* row : {"SERVER=example.invalid", "PORT=15439", "DATABASE=fixture",
                         "UID=fixture_user", "PWD=synthetic-only", "SSL=1"}) {
    initial += '\0'; initial += row;
  }
  initial += '\0'; initial += '\0';
  ASSERT_TRUE(SQLConfigDataSource(nullptr, ODBC_ADD_DSN, ansi_driver, initial.c_str()));
  expect_stored(L"fixture");
  std::string update = "DSN=" + ascii_name;
  update += '\0'; update += "DATABASE=updated"; update += '\0'; update += '\0';
  ASSERT_TRUE(SQLConfigDataSource(nullptr, ODBC_CONFIG_DSN, ansi_driver, update.c_str()));
  expect_stored(L"updated");
  const auto remove = "DSN=" + ascii_name + '\0';
  ASSERT_TRUE(SQLConfigDataSource(nullptr, ODBC_REMOVE_DSN, ansi_driver, remove.c_str()));
}

TEST_F(InstalledSetup, WrongProductAndDuplicateAddPreserveOwnedDsn) {
  const auto initial = attributes({L"DSN=" + name, L"SERVER=example.invalid", L"PORT=15439",
      L"DATABASE=fixture", L"UID=fixture_user", L"SSL=1"});
  ASSERT_TRUE(SQLConfigDataSourceW(nullptr, ODBC_ADD_DSN, driver, initial.c_str()));
  EXPECT_FALSE(SQLConfigDataSourceW(nullptr, ODBC_ADD_DSN, driver, initial.c_str()));
  EXPECT_FALSE(wide(nullptr, ODBC_CONFIG_DSN, other_driver,
      attributes({L"DSN=" + name, L"DATABASE=wrong"}).c_str()));
  const auto remove = "DSN=" + ascii_name + '\0';
  EXPECT_FALSE(ansi(nullptr, ODBC_REMOVE_DSN, other_ansi_driver, remove.c_str()));
  expect_stored(L"fixture");
}
}  // namespace
