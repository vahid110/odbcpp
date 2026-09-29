#include <gtest/gtest.h>

#include "odbc/connection_string.h"

#include <stdexcept>

namespace {

using rs::odbc::ConnectionString;

TEST(ConnectionStringTest, BracedSemicolonsCannotIntroduceNewOptions) {
  const auto parsed = ConnectionString::parse(
      "PWD={secret;SERVER=attacker.example;TransportMode=Sync};"
      "SERVER=database.example;TransportMode=Async");

  EXPECT_EQ(parsed.at("PWD"),
            "secret;SERVER=attacker.example;TransportMode=Sync");
  EXPECT_EQ(parsed.at("SERVER"), "database.example");
  EXPECT_EQ(parsed.at("TRANSPORTMODE"), "Async");
  EXPECT_EQ(parsed.size(), 3u);
}

TEST(ConnectionStringTest, DecodesEscapedClosingBracesAndPreservesBracedSpace) {
  const auto parsed = ConnectionString::parse(
      "DRIVER={ODBC}}PP; Driver};PWD={ a}}b; c };UID=  alice  ;EMPTY={}");

  EXPECT_EQ(parsed.at("DRIVER"), "ODBC}PP; Driver");
  EXPECT_EQ(parsed.at("PWD"), " a}b; c ");
  EXPECT_EQ(parsed.at("UID"), "alice");
  EXPECT_EQ(parsed.at("EMPTY"), "");
}

TEST(ConnectionStringTest, RejectsMalformedBracedValues) {
  EXPECT_THROW(ConnectionString::parse("PWD={secret;SERVER=attacker.example"),
               std::invalid_argument);
  EXPECT_THROW(ConnectionString::parse("PWD={secret}suffix;SERVER=database.example"),
               std::invalid_argument);
  EXPECT_THROW(ConnectionString::parse("PWD={secret}}"),
               std::invalid_argument);
}

TEST(ConnectionStringTest, RejectsEmbeddedNulBeforeParsingAttributes) {
  const std::string input("UID=alice\0;SERVER=attacker.example",
                          sizeof("UID=alice\0;SERVER=attacker.example") - 1);
  EXPECT_THROW(ConnectionString::parse(input), std::invalid_argument);
}

TEST(ConnectionStringTest, KeepsFirstRepeatedOption) {
  const auto parsed = ConnectionString::parse(
      "; NO_EQUALS ; UID=first;UID=second;PWD= plain ;");

  EXPECT_EQ(parsed.at("UID"), "first");
  EXPECT_EQ(parsed.at("PWD"), "plain");
  EXPECT_EQ(parsed.size(), 2u);
}

} // namespace

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "odbc/windows_registry.h"
#include <system_error>

namespace {
class NativeDsnTest : public ::testing::Test {
 protected:
  HKEY scratch{}, user{}, machine{};
  std::wstring scratch_path;
  void SetUp() override {
    scratch_path = L"Software\\ODBCPP-W1-tests-" + std::to_wstring(GetCurrentProcessId());
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExW(HKEY_CURRENT_USER, scratch_path.c_str(),
        0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &scratch, nullptr));
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExW(scratch, L"User", 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &user, nullptr));
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExW(scratch, L"Machine", 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &machine, nullptr));
    ASSERT_EQ(ERROR_SUCCESS, RegOverridePredefKey(HKEY_CURRENT_USER, user));
    ASSERT_EQ(ERROR_SUCCESS, RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine));
  }
  void TearDown() override {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
    if (user) RegCloseKey(user);
    if (machine) RegCloseKey(machine);
    if (scratch) RegCloseKey(scratch);
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch_path.c_str());
  }
  void value(HKEY root, const wchar_t* path, const wchar_t* name, const wchar_t* text) {
    HKEY key{};
    ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExW(root, path, 0, nullptr, 0,
        KEY_ALL_ACCESS, nullptr, &key, nullptr));
    const auto status = RegSetValueExW(key, name, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(text), static_cast<DWORD>((wcslen(text) + 1) * sizeof(wchar_t)));
    RegCloseKey(key); ASSERT_EQ(ERROR_SUCCESS, status);
  }
};

TEST_F(NativeDsnTest, UserShadowsSystemWithoutMergingAndExplicitAliasesOverride) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\NativeTest";
  value(HKEY_LOCAL_MACHINE, path, L"Server", L"system");
  value(HKEY_LOCAL_MACHINE, path, L"PWD", L"system-secret");
  value(HKEY_CURRENT_USER, path, L"Server", L"user");
  value(HKEY_CURRENT_USER, path, L"Driver", L"C:\\driver.dll");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources", L"NativeTest", L"Native Driver");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\Native Driver", L"Database", L"default_db");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\Native Driver", L"Port", L"5432");
  const auto resolved = ConnectionString::resolve("DSN=NativeTest;HOST=explicit;PASSWORD={}", "Unused");
  EXPECT_EQ("Native Driver", resolved.driver_name);
  EXPECT_EQ("user", resolved.dsn_parameters.at("SERVER"));
  EXPECT_EQ(0u, resolved.dsn_parameters.count("PWD"));
  EXPECT_EQ("explicit", resolved.effective_parameters.at("SERVER"));
  EXPECT_EQ("", resolved.effective_parameters.at("PWD"));
  EXPECT_EQ("default_db", resolved.effective_parameters.at("DATABASE"));
  EXPECT_EQ("5432", resolved.effective_parameters.at("PORT"));
  EXPECT_TRUE(rs::odbc::DSNReader::dsn_exists("NativeTest"));
  EXPECT_EQ(ERROR_SUCCESS, RegDeleteTreeW(HKEY_CURRENT_USER, path));
  EXPECT_EQ("system", ConnectionString::load_dsn("NativeTest").at("SERVER"));
}

TEST_F(NativeDsnTest, UnicodeAndEmptyValuesArePreservedAndMissingIsDistinct) {
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\Case_\u6d4b", L"Database", L"\u00e9;data");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\Case_\u6d4b", L"PWD", L"");
  const auto result = rs::odbc::windows_registry::load_dsn("Case_\xe6\xb5\x8b");
  ASSERT_TRUE(result); EXPECT_EQ("\xc3\xa9;data", result->at("DATABASE"));
  EXPECT_EQ("", result->at("PWD"));
  EXPECT_FALSE(rs::odbc::windows_registry::load_dsn("missing"));
  EXPECT_THROW(rs::odbc::windows_registry::load_dsn("ODBC Data Sources"), std::invalid_argument);
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBC.INI\\EmptyNative", L"Server", L"system");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\EmptyNative", L"", L"");
  const auto empty = rs::odbc::windows_registry::load_dsn("EmptyNative");
  ASSERT_TRUE(empty); EXPECT_TRUE(empty->empty());
  EXPECT_TRUE(ConnectionString::load_dsn("EmptyNative").empty());
  EXPECT_THROW(rs::odbc::windows_registry::load_dsn("bad\\child"), std::invalid_argument);
  EXPECT_THROW(rs::odbc::windows_registry::load_dsn(std::string("a\0b", 3)), std::invalid_argument);
  EXPECT_THROW(rs::odbc::windows_registry::load_dsn("\xff"), std::invalid_argument);
}

TEST_F(NativeDsnTest, MalformedUserEntryDoesNotFallBackToSystem) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\Malformed";
  value(HKEY_LOCAL_MACHINE, path, L"Server", L"system");
  value(HKEY_CURRENT_USER, path, L"Server", L"user");
  HKEY key{};
  ASSERT_EQ(ERROR_SUCCESS, RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_ALL_ACCESS, &key));
  const wchar_t invalid[]{L'a', L'\0', L'b', L'\0'};
  EXPECT_EQ(ERROR_SUCCESS, RegSetValueExW(key, L"Server", 0, REG_SZ,
      reinterpret_cast<const BYTE*>(invalid), sizeof(invalid)));
  EXPECT_THROW(ConnectionString::load_dsn("Malformed"), std::invalid_argument);
  const DWORD numeric = 123;
  EXPECT_EQ(ERROR_SUCCESS, RegSetValueExW(key, L"Server", 0, REG_BINARY,
      reinterpret_cast<const BYTE*>(&numeric), sizeof(numeric)));
  EXPECT_THROW(ConnectionString::load_dsn("Malformed"), std::invalid_argument);
  // Keep a nonzero character after the supplied byte range: Windows can
  // include an adjacent terminator when writing REG_SZ data.
  const wchar_t unterminated[] = L"ab";
  EXPECT_EQ(ERROR_SUCCESS, RegSetValueExW(key, L"Server", 0, REG_SZ,
      reinterpret_cast<const BYTE*>(unterminated), sizeof(wchar_t)));
  DWORD stored_bytes = 0;
  EXPECT_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"Server", nullptr, nullptr,
      nullptr, &stored_bytes));
  EXPECT_EQ(sizeof(wchar_t), stored_bytes);
  wchar_t stored = L'\0';
  DWORD read_bytes = sizeof(stored);
  EXPECT_EQ(ERROR_SUCCESS, RegQueryValueExW(key, L"Server", nullptr, nullptr,
      reinterpret_cast<BYTE*>(&stored), &read_bytes));
  EXPECT_EQ(sizeof(wchar_t), read_bytes);
  EXPECT_EQ(L'a', stored);
  EXPECT_THROW(ConnectionString::load_dsn("Malformed"), std::invalid_argument);
  EXPECT_EQ(ERROR_SUCCESS, RegSetValueExW(key, L"Server", 0, REG_DWORD,
      reinterpret_cast<const BYTE*>(&numeric), sizeof(numeric)));
  EXPECT_EQ("123", ConnectionString::load_dsn("Malformed").at("SERVER"));
  RegCloseKey(key);
}

TEST_F(NativeDsnTest, DeniedUserEntryDoesNotFallBackAndRestoresAccess) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\Denied";
  value(HKEY_CURRENT_USER, path, L"Server", L"user");
  value(HKEY_LOCAL_MACHINE, path, L"Server", L"system");
  HKEY key{};
  ASSERT_EQ(ERROR_SUCCESS, RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_ALL_ACCESS, &key));
  SECURITY_DESCRIPTOR descriptor{};
  ACL empty{};
  ASSERT_TRUE(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION));
  ASSERT_TRUE(InitializeAcl(&empty, sizeof(empty), ACL_REVISION));
  ASSERT_TRUE(SetSecurityDescriptorDacl(&descriptor, TRUE, &empty, FALSE));
  EXPECT_EQ(ERROR_SUCCESS, RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, &descriptor));
  EXPECT_THROW(ConnectionString::load_dsn("Denied"), std::system_error);
  // The already-open handle retains WRITE_DAC so cleanup is possible.
  EXPECT_TRUE(SetSecurityDescriptorDacl(&descriptor, TRUE, nullptr, FALSE));
  EXPECT_EQ(ERROR_SUCCESS, RegSetKeySecurity(key, DACL_SECURITY_INFORMATION, &descriptor));
  RegCloseKey(key);
  EXPECT_EQ("user", ConnectionString::load_dsn("Denied").at("SERVER"));
}
}
#endif
