#include <gtest/gtest.h>

#include "odbc/connection_string.h"
#include "odbc/resource_limits.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace {

using rs::odbc::ConnectionString;

TEST(ConnectionStringTest, RedshiftCatalogModeKeyCasingBracesEmptyAndDuplicatePolicy) {
  const auto parsed = ConnectionString::parse(
      "redshiftcatalogmode={LeGaCy};REDSHIFTCATALOGMODE=SHOW");
  EXPECT_EQ("LeGaCy", parsed.at("REDSHIFTCATALOGMODE"));
  EXPECT_EQ("LEGACY", ConnectionString::parse("RedshiftCatalogMode= LEGACY ").at("REDSHIFTCATALOGMODE"));
  EXPECT_EQ("", ConnectionString::parse("RedshiftCatalogMode={}").at("REDSHIFTCATALOGMODE"));
  EXPECT_EQ(" LEGACY ", ConnectionString::parse("RedshiftCatalogMode={ LEGACY }").at("REDSHIFTCATALOGMODE"));
}

TEST(ConnectionStringTest, LoginTimeoutPrimaryAliasAndDuplicatePolicyArePerLayer) {
  EXPECT_EQ("7", ConnectionString::resolve("LT=invalid;LoginTimeout=7", "").effective_parameters.at("LOGINTIMEOUT"));
  EXPECT_EQ("invalid", ConnectionString::resolve("LoginTimeout=invalid;LT=7", "").effective_parameters.at("LOGINTIMEOUT"));
  EXPECT_EQ(" 0002 ", ConnectionString::resolve("lt={ 0002 }", "").effective_parameters.at("LOGINTIMEOUT"));
  EXPECT_EQ("2", ConnectionString::resolve("LoginTimeout=2;logintimeout=3", "").effective_parameters.at("LOGINTIMEOUT"));
}

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

TEST(ConnectionStringTest, NormalizesKeysWithoutChangingCredentialCase) {
  const auto parsed = ConnectionString::parse(
      " driver = {ODBCPP PostgreSQL} ; uId = MixedCase ; pWd={ w2;secret}}Tail };"
      "UID=ignored;pwd=ignored");
  EXPECT_EQ(parsed.at("DRIVER"), "ODBCPP PostgreSQL");
  EXPECT_EQ(parsed.at("UID"), "MixedCase");
  EXPECT_EQ(parsed.at("PWD"), " w2;secret}Tail ");
  EXPECT_EQ(parsed.size(), 3u);
}

TEST(ConnectionStringTest, ExplicitEmptyCredentialsRemainPresentAndFirst) {
  for (const auto input : {"UID=;PWD=;UID=other;PWD=other;",
                           "UID={};PWD={};UID=other;PWD=other;"}) {
    const auto parsed = ConnectionString::parse(input);
    EXPECT_EQ(parsed.at("UID"), "");
    EXPECT_EQ(parsed.at("PWD"), "");
    EXPECT_EQ(parsed.size(), 2u);
  }
}

#ifndef _WIN32
class UnixIniDiscoveryTest : public ::testing::Test {
 protected:
  class EnvironmentGuard {
   public:
    explicit EnvironmentGuard(const char* name) : name_(name) {
      if (const auto* value = std::getenv(name)) original_ = value;
    }
    ~EnvironmentGuard() {
      if (original_) setenv(name_.c_str(), original_->c_str(), 1);
      else unsetenv(name_.c_str());
    }

   private:
    std::string name_;
    std::optional<std::string> original_;
  };

  EnvironmentGuard odbcini_{"ODBCINI"};
  EnvironmentGuard odbcsysini_{"ODBCSYSINI"};
  EnvironmentGuard odbcinstini_{"ODBCINSTINI"};
  EnvironmentGuard home_{"HOME"};
  std::filesystem::path original_directory_{std::filesystem::current_path()};
  std::filesystem::path root_;

  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() /
        ("odbcpp-ini-discovery-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "work" / "child"));
    ASSERT_TRUE(std::filesystem::create_directories(root_ / "home"));
    unsetenv("ODBCINI");
    unsetenv("ODBCSYSINI");
    unsetenv("ODBCINSTINI");
    ASSERT_EQ(0, setenv("HOME", (root_ / "home").c_str(), 1));
  }

  void TearDown() override {
    std::filesystem::current_path(original_directory_);
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  static void write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path);
    ASSERT_TRUE(output.is_open());
    output << text;
    ASSERT_TRUE(output.good());
  }
};

TEST_F(UnixIniDiscoveryTest, IgnoresCurrentAndParentDirectoryIniFiles) {
  constexpr auto dsn = "ODBCPP_CwdFixture_MustNotLoad";
  constexpr auto driver = "ODBCPP Cwd Driver Must Not Load";
  const auto dsn_contents = std::string("[") + dsn + "]\nSERVER=untrusted\n";
  const auto driver_contents =
      std::string("[") + driver + "]\nPORT=6543\n";

  write(root_ / "work" / "child" / "odbc.ini", dsn_contents);
  write(root_ / "work" / "odbc.ini", dsn_contents);
  write(root_ / "work" / "child" / "odbcpp.dsn", dsn_contents);
  write(root_ / "work" / "odbcpp.dsn", dsn_contents);
  write(root_ / "work" / "child" / "odbcinst.ini", driver_contents);
  write(root_ / "work" / "odbcinst.ini", driver_contents);
  std::filesystem::current_path(root_ / "work" / "child");

  EXPECT_TRUE(ConnectionString::load_dsn(dsn).empty());
  EXPECT_TRUE(rs::odbc::DSNReader::read_driver_config(driver).empty());
}

TEST_F(UnixIniDiscoveryTest, HonorsExplicitDsnAndDriverManagerIniPaths) {
  constexpr auto dsn = "ODBCPP_ExplicitFixture";
  constexpr auto driver = "ODBCPP Explicit Driver";
  const auto dsn_path = root_ / "trusted-dsn.ini";
  write(dsn_path, std::string("[") + dsn + "]\nSERVER=trusted\n");
  write(root_ / "drivers.ini",
        std::string("[") + driver + "]\nPORT=6543\n");

  ASSERT_EQ(0, setenv("ODBCINI", dsn_path.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));

  const auto loaded_dsn = ConnectionString::load_dsn(dsn);
  ASSERT_EQ(1u, loaded_dsn.size());
  EXPECT_EQ("trusted", loaded_dsn.at("SERVER"));
  const auto loaded_driver = rs::odbc::DSNReader::read_driver_config(driver);
  ASSERT_EQ(1u, loaded_driver.size());
  EXPECT_EQ("6543", loaded_driver.at("PORT"));
}
TEST_F(UnixIniDiscoveryTest, RedshiftCatalogModeFollowsDriverDsnAndConnectionPrecedence) {
  const auto dsn_path = root_ / "trusted-dsn.ini";
  write(dsn_path, "[ModeFixture]\nDRIVER=ModeDriver\nRedshiftCatalogMode=LEGACY\n");
  write(root_ / "drivers.ini", "[ModeDriver]\nRedshiftCatalogMode=SHOW\n");
  ASSERT_EQ(0, setenv("ODBCINI", dsn_path.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  EXPECT_EQ("LEGACY", ConnectionString::resolve("DSN=ModeFixture", "ModeDriver").effective_parameters.at("REDSHIFTCATALOGMODE"));
  EXPECT_EQ("SHOW", ConnectionString::resolve("DSN=ModeFixture;RedshiftCatalogMode=SHOW", "ModeDriver").effective_parameters.at("REDSHIFTCATALOGMODE"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=ModeFixture;RedshiftCatalogMode={}", "ModeDriver").effective_parameters.at("REDSHIFTCATALOGMODE"));
}

TEST_F(UnixIniDiscoveryTest, ApplicationNameRespectsDriverDsnExplicitEmptyAndLiteralLayers) {
  const auto dsn_path = root_ / "trusted-dsn.ini";
  write(dsn_path, "[AppFixture]\nDRIVER=AppDriver\nApplicationName=dsn-name\n");
  write(root_ / "drivers.ini", "[AppDriver]\nApplicationName=driver-name\n");
  ASSERT_EQ(0, setenv("ODBCINI", dsn_path.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  EXPECT_EQ("driver-name", ConnectionString::resolve("DRIVER=AppDriver", "AppDriver").effective_parameters.at("APPLICATIONNAME"));
  EXPECT_EQ("dsn-name", ConnectionString::resolve("DSN=AppFixture", "AppDriver").effective_parameters.at("APPLICATIONNAME"));
  EXPECT_EQ("unicode-é表😀;literal}name", ConnectionString::resolve("DSN=AppFixture;ApplicationName={unicode-é表😀;literal}}name}", "AppDriver").effective_parameters.at("APPLICATIONNAME"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=AppFixture;ApplicationName={}", "AppDriver").effective_parameters.at("APPLICATIONNAME"));
  EXPECT_EQ("dsn-name", ConnectionString::resolve("DSN=AppFixture", "AppDriver").effective_parameters.at("APPLICATIONNAME"));
}

TEST_F(UnixIniDiscoveryTest, ReadOnlyLayersPreserveExplicitFalseAndEmptyForValidation) {
  const auto dsn_path = root_ / "readonly-dsn.ini";
  write(dsn_path, "[ReadOnlyFixture]\nDRIVER=ReadOnlyDriver\nReadOnly=true\n");
  write(root_ / "drivers.ini", "[ReadOnlyDriver]\nReadOnly=false\n");
  ASSERT_EQ(0, setenv("ODBCINI", dsn_path.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  EXPECT_EQ("false", ConnectionString::resolve("DRIVER=ReadOnlyDriver", "").effective_parameters.at("READONLY"));
  EXPECT_EQ("true", ConnectionString::resolve("DSN=ReadOnlyFixture", "").effective_parameters.at("READONLY"));
  EXPECT_EQ("false", ConnectionString::resolve("DSN=ReadOnlyFixture;ReadOnly=false", "").effective_parameters.at("READONLY"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=ReadOnlyFixture;ReadOnly={}", "").effective_parameters.at("READONLY"));
}

TEST_F(UnixIniDiscoveryTest, QueryTimeoutAliasesRespectEachLayerAndExplicitEmpty) {
  const auto dsn_path = root_ / "dsn.ini";
  write(dsn_path, "[QueryFixture]\nDRIVER=QueryDriver\nQT=2\n");
  write(root_ / "drivers.ini", "[QueryDriver]\nQueryTimeout=invalid\nQT=4\n");
  ASSERT_EQ(0, setenv("ODBCINI", dsn_path.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  EXPECT_EQ("invalid", ConnectionString::resolve("DRIVER=QueryDriver", "").effective_parameters.at("QUERYTIMEOUT"));
  EXPECT_EQ("2", ConnectionString::resolve("QueryFixture", "QueryDriver").effective_parameters.at("QUERYTIMEOUT"));
  EXPECT_EQ("0", ConnectionString::resolve("DSN=QueryFixture;QT=0", "QueryDriver").effective_parameters.at("QUERYTIMEOUT"));
  EXPECT_EQ("7", ConnectionString::resolve("DSN=QueryFixture;QT=invalid;QueryTimeout=7", "QueryDriver").effective_parameters.at("QUERYTIMEOUT"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=QueryFixture;QT={}", "QueryDriver").effective_parameters.at("QUERYTIMEOUT"));
  write(dsn_path, "[QueryFixture]\nDRIVER=QueryDriver\nQT=invalid\nQueryTimeout=5\n");
  EXPECT_EQ("5", ConnectionString::resolve("QueryFixture", "QueryDriver").effective_parameters.at("QUERYTIMEOUT"));
}

TEST_F(UnixIniDiscoveryTest, LoginTimeoutAliasesRespectDriverDsnAndExplicitLayers) {
  const auto dsn_path = root_ / "trusted-dsn.ini";
  write(dsn_path, "[LoginFixture]\nDRIVER=LoginDriver\nLT=2\n");
  write(root_ / "drivers.ini", "[LoginDriver]\nLoginTimeout=invalid\nLT=4\n");
  ASSERT_EQ(0, setenv("ODBCINI", dsn_path.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  EXPECT_EQ("2", ConnectionString::resolve("LoginFixture", "LoginDriver").effective_parameters.at("LOGINTIMEOUT"));
  EXPECT_EQ("3", ConnectionString::resolve("DSN=LoginFixture;LT=3", "LoginDriver").effective_parameters.at("LOGINTIMEOUT"));
  EXPECT_EQ("7", ConnectionString::resolve("DSN=LoginFixture;LT=invalid;LoginTimeout=7", "LoginDriver").effective_parameters.at("LOGINTIMEOUT"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=LoginFixture;LoginTimeout={}", "LoginDriver").effective_parameters.at("LOGINTIMEOUT"));
  write(dsn_path, "[LoginFixture]\nDRIVER=LoginDriver\nLT=invalid\nLoginTimeout=5\n");
  EXPECT_EQ("5", ConnectionString::resolve("LoginFixture", "LoginDriver").effective_parameters.at("LOGINTIMEOUT"));
}

TEST_F(UnixIniDiscoveryTest, ResourceLimitsFollowDriverDsnAndConnectionPrecedence) {
  const auto dsn_path = root_ / "trusted-dsn.ini";
  write(dsn_path, "[LimitFixture]\nDRIVER=LimitDriver\nMaxRows=20\nMaxSqlBytes=512\n");
  write(root_ / "drivers.ini", "[LimitDriver]\nMaxRows=30\nMaxCells=90\n");
  ASSERT_EQ(0, setenv("ODBCINI", dsn_path.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  const auto resolved = ConnectionString::resolve("DSN=LimitFixture;MaxRows=10", "LimitDriver");
  rs::core::database::ConnectionOptions options;
  rs::odbc::parse_resource_limits(resolved.effective_parameters, options);
  EXPECT_EQ(options.result_limits.max_rows, 10u);
  EXPECT_EQ(options.result_limits.max_cells, 90u);
  EXPECT_EQ(options.input_limits.max_sql_bytes, 512u);
}
TEST_F(UnixIniDiscoveryTest, BooleanCharacterDriverDsnStringPrecedenceRetainsExplicitFalseAndEmpty) {
  write(root_ / "drivers.ini", "[Boolean Driver]\nBoolsAsChar=invalid\n");
  write(root_ / "data.ini", "[BooleanFixture]\nDriver=Boolean Driver\nBoolsAsChar=1\n");
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  ASSERT_EQ(0, setenv("ODBCINI", (root_ / "data.ini").c_str(), 1));
  EXPECT_EQ("invalid", ConnectionString::resolve("DRIVER=Boolean Driver", "").effective_parameters.at("BOOLSASCHAR"));
  EXPECT_EQ("1", ConnectionString::resolve("DSN=BooleanFixture", "").effective_parameters.at("BOOLSASCHAR"));
  EXPECT_EQ("FALSE", ConnectionString::resolve("DSN=BooleanFixture;BoolsAsChar=FALSE", "").effective_parameters.at("BOOLSASCHAR"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=BooleanFixture;BoolsAsChar={}", "").effective_parameters.at("BOOLSASCHAR"));
}

TEST_F(UnixIniDiscoveryTest, TableTypesDriverDsnStringPrecedenceRetainsExplicitFalseAndEmpty) {
  write(root_ / "drivers.ini", "[Table Types Driver]\nEnableTableTypes=invalid\n");
  write(root_ / "data.ini", "[TableTypesFixture]\nDriver=Table Types Driver\nEnableTableTypes=1\n");
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  ASSERT_EQ(0, setenv("ODBCINI", (root_ / "data.ini").c_str(), 1));
  EXPECT_EQ("invalid", ConnectionString::resolve("DRIVER=Table Types Driver", "").effective_parameters.at("ENABLETABLETYPES"));
  EXPECT_EQ("1", ConnectionString::resolve("DSN=TableTypesFixture", "").effective_parameters.at("ENABLETABLETYPES"));
  EXPECT_EQ("FALSE", ConnectionString::resolve("DSN=TableTypesFixture;EnableTableTypes=FALSE", "").effective_parameters.at("ENABLETABLETYPES"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=TableTypesFixture;EnableTableTypes={}", "").effective_parameters.at("ENABLETABLETYPES"));
}

TEST_F(UnixIniDiscoveryTest, VarcharSizesRespectDriverDsnAndExplicitZeroEmptyOverrides) {
  write(root_ / "drivers.ini", "[Sizing Driver]\nMaxVarcharSize=invalid\nMaxLongVarcharSize=65535\n");
  write(root_ / "data.ini", "[SizingFixture]\nDriver=Sizing Driver\nMaxVarcharSize=8\nMaxLongVarcharSize=17\n");
  ASSERT_EQ(0, setenv("ODBCSYSINI", root_.c_str(), 1));
  ASSERT_EQ(0, setenv("ODBCINSTINI", "drivers.ini", 1));
  ASSERT_EQ(0, setenv("ODBCINI", (root_ / "data.ini").c_str(), 1));
  const auto driver = ConnectionString::resolve("DRIVER=Sizing Driver", "");
  EXPECT_EQ("invalid", driver.effective_parameters.at("MAXVARCHARSIZE"));
  const auto dsn = ConnectionString::resolve("DSN=SizingFixture", "");
  EXPECT_EQ("8", dsn.effective_parameters.at("MAXVARCHARSIZE"));
  EXPECT_EQ("17", dsn.effective_parameters.at("MAXLONGVARCHARSIZE"));
  const auto zero = ConnectionString::resolve("DSN=SizingFixture;MaxVarcharSize=0;MaxLongVarcharSize=0", "");
  EXPECT_EQ("0", zero.effective_parameters.at("MAXVARCHARSIZE"));
  EXPECT_EQ("0", zero.effective_parameters.at("MAXLONGVARCHARSIZE"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=SizingFixture;MaxVarcharSize={}", "").effective_parameters.at("MAXVARCHARSIZE"));
}

#endif

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

TEST_F(NativeDsnTest, QueryTimeoutNativeLayersAndAliasEmptyRemainLiteral) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\QueryFixture";
  value(HKEY_CURRENT_USER, path, L"Driver", L"Query Driver");
  value(HKEY_CURRENT_USER, path, L"QT", L"2");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources", L"QueryFixture", L"Query Driver");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\Query Driver", L"QueryTimeout", L"invalid");
  EXPECT_EQ("invalid", ConnectionString::resolve("DRIVER=Query Driver", "").effective_parameters.at("QUERYTIMEOUT"));
  EXPECT_EQ("2", ConnectionString::resolve("QueryFixture", "Query Driver").effective_parameters.at("QUERYTIMEOUT"));
  EXPECT_EQ("3", ConnectionString::resolve("DSN=QueryFixture;QT=3", "Query Driver").effective_parameters.at("QUERYTIMEOUT"));
  EXPECT_EQ("7", ConnectionString::resolve("DSN=QueryFixture;QT=invalid;QueryTimeout=7", "Query Driver").effective_parameters.at("QUERYTIMEOUT"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=QueryFixture;QT={}", "Query Driver").effective_parameters.at("QUERYTIMEOUT"));
}

TEST_F(NativeDsnTest, BooleanCharacterNativeRegistryDsnStringPrecedenceRetainsFalseAndEmpty) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\BooleanFixture";
  value(HKEY_CURRENT_USER, path, L"Driver", L"Boolean Driver");
  value(HKEY_CURRENT_USER, path, L"BoolsAsChar", L"1");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources", L"BooleanFixture", L"Boolean Driver");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\Boolean Driver", L"BoolsAsChar", L"invalid");
  EXPECT_EQ("invalid", ConnectionString::resolve("DRIVER=Boolean Driver", "").effective_parameters.at("BOOLSASCHAR"));
  EXPECT_EQ("1", ConnectionString::resolve("DSN=BooleanFixture", "").effective_parameters.at("BOOLSASCHAR"));
  EXPECT_EQ("FALSE", ConnectionString::resolve("DSN=BooleanFixture;BoolsAsChar=FALSE", "").effective_parameters.at("BOOLSASCHAR"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=BooleanFixture;BoolsAsChar={}", "").effective_parameters.at("BOOLSASCHAR"));
}

TEST_F(NativeDsnTest, TableTypesNativeRegistryDsnStringPrecedenceRetainsFalseAndEmpty) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\TableTypesFixture";
  value(HKEY_CURRENT_USER, path, L"Driver", L"Table Types Driver");
  value(HKEY_CURRENT_USER, path, L"EnableTableTypes", L"1");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources", L"TableTypesFixture", L"Table Types Driver");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\Table Types Driver", L"EnableTableTypes", L"invalid");
  EXPECT_EQ("invalid", ConnectionString::resolve("DRIVER=Table Types Driver", "").effective_parameters.at("ENABLETABLETYPES"));
  EXPECT_EQ("1", ConnectionString::resolve("DSN=TableTypesFixture", "").effective_parameters.at("ENABLETABLETYPES"));
  EXPECT_EQ("FALSE", ConnectionString::resolve("DSN=TableTypesFixture;EnableTableTypes=FALSE", "").effective_parameters.at("ENABLETABLETYPES"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=TableTypesFixture;EnableTableTypes={}", "").effective_parameters.at("ENABLETABLETYPES"));
}

TEST_F(NativeDsnTest, VarcharSizesRespectNativeRegistryDriverDsnAndExplicitZeroEmptyOverrides) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\SizingFixture";
  value(HKEY_CURRENT_USER, path, L"Driver", L"Sizing Driver");
  value(HKEY_CURRENT_USER, path, L"MaxVarcharSize", L"8");
  value(HKEY_CURRENT_USER, path, L"MaxLongVarcharSize", L"17");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources", L"SizingFixture", L"Sizing Driver");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\Sizing Driver", L"MaxVarcharSize", L"invalid");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\Sizing Driver", L"MaxLongVarcharSize", L"65535");
  EXPECT_EQ("invalid", ConnectionString::resolve("DRIVER=Sizing Driver", "").effective_parameters.at("MAXVARCHARSIZE"));
  const auto dsn = ConnectionString::resolve("SizingFixture", "Sizing Driver");
  EXPECT_EQ("8", dsn.effective_parameters.at("MAXVARCHARSIZE"));
  EXPECT_EQ("17", dsn.effective_parameters.at("MAXLONGVARCHARSIZE"));
  const auto zero = ConnectionString::resolve("DSN=SizingFixture;MaxVarcharSize=0;MaxLongVarcharSize=0", "");
  EXPECT_EQ("0", zero.effective_parameters.at("MAXVARCHARSIZE"));
  EXPECT_EQ("0", zero.effective_parameters.at("MAXLONGVARCHARSIZE"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=SizingFixture;MaxVarcharSize={}", "").effective_parameters.at("MAXVARCHARSIZE"));
}

TEST_F(NativeDsnTest, LoginTimeoutAliasesRespectNativeDriverDsnAndExplicitLayers) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\LoginFixture";
  value(HKEY_CURRENT_USER, path, L"Driver", L"Login Driver");
  value(HKEY_CURRENT_USER, path, L"LT", L"2");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources", L"LoginFixture", L"Login Driver");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\Login Driver", L"LoginTimeout", L"invalid");
  EXPECT_EQ("2", ConnectionString::resolve("LoginFixture", "Login Driver").effective_parameters.at("LOGINTIMEOUT"));
  EXPECT_EQ("3", ConnectionString::resolve("DSN=LoginFixture;LT=3", "Login Driver").effective_parameters.at("LOGINTIMEOUT"));
  EXPECT_EQ("7", ConnectionString::resolve("DSN=LoginFixture;LT=invalid;LoginTimeout=7", "Login Driver").effective_parameters.at("LOGINTIMEOUT"));
}

TEST_F(NativeDsnTest, ReadOnlyNativeDriverDsnAndExplicitFalseOrEmptyLayers) {
  constexpr auto path = L"SOFTWARE\\ODBC\\ODBC.INI\\ReadOnlyFixture";
  value(HKEY_CURRENT_USER, path, L"Driver", L"ReadOnly Driver");
  value(HKEY_CURRENT_USER, path, L"ReadOnly", L"true");
  value(HKEY_CURRENT_USER, L"SOFTWARE\\ODBC\\ODBC.INI\\ODBC Data Sources", L"ReadOnlyFixture", L"ReadOnly Driver");
  value(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ODBC\\ODBCINST.INI\\ReadOnly Driver", L"ReadOnly", L"false");
  EXPECT_EQ("false", ConnectionString::resolve("DRIVER=ReadOnly Driver", "ReadOnly Driver").effective_parameters.at("READONLY"));
  EXPECT_EQ("true", ConnectionString::resolve("ReadOnlyFixture", "ReadOnly Driver").effective_parameters.at("READONLY"));
  EXPECT_EQ("false", ConnectionString::resolve("DSN=ReadOnlyFixture;ReadOnly=false", "ReadOnly Driver").effective_parameters.at("READONLY"));
  EXPECT_EQ("", ConnectionString::resolve("DSN=ReadOnlyFixture;ReadOnly={}", "ReadOnly Driver").effective_parameters.at("READONLY"));
}

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
