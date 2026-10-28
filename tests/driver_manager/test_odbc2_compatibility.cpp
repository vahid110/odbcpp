#include "tests/test_dm_odbc.h"
#include "tests/test_dm_identity.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#ifdef _MSC_VER
// These tests intentionally exercise deprecated ODBC 2 entry points.
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace {
constexpr const char* kDriver = ODBCPP_ODBC2_DRIVER_PATH;

bool driver_loaded() {
#ifdef _WIN32
  return GetModuleHandleW(std::filesystem::path(kDriver).wstring().c_str()) != nullptr;
#else
  auto* module = dlopen(kDriver, RTLD_LAZY | RTLD_NOLOAD);
  if (module) {
    dlclose(module);
  }
  return module != nullptr;
#endif
}

struct Diagnostic {
  SQLRETURN result{};
  std::array<SQLCHAR, 6> state{};
  std::array<SQLCHAR, 1024> message{};
  std::string sqlstate() const { return reinterpret_cast<const char*>(state.data()); }
};

Diagnostic diagnostic(SQLHDBC connection) {
  Diagnostic result;
  result.result = SQLGetDiagRec(SQL_HANDLE_DBC, connection, 1,
      result.state.data(), nullptr, result.message.data(),
      static_cast<SQLSMALLINT>(result.message.size()), nullptr);
  return result;
}

class Odbc2PreDriver : public ::testing::Test {
 protected:
  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  std::string manager_version_;

  void SetUp() override {
    ASSERT_FALSE(driver_loaded());
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
    ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
        reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC2), 0));
    SQLINTEGER version{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
        &version, sizeof(version), nullptr));
    ASSERT_EQ(SQL_OV_ODBC2, version);
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    SQLCHAR manager_version[64]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_DM_VER,
        manager_version, sizeof(manager_version), nullptr));
    const auto manager = odbcpp::test::dm::observed_manager_library_path();
    ASSERT_FALSE(manager.empty());
    RecordProperty("dm_loaded_library", manager);
    RecordProperty("dm_version", reinterpret_cast<const char*>(manager_version));
    manager_version_ = reinterpret_cast<const char*>(manager_version);
    RecordProperty("dm_odbc_environment", static_cast<int>(version));
    RecordProperty("dm_pointer_bytes", static_cast<int>(sizeof(void*)));
    RecordProperty("dm_application_sqlwchar_bytes", static_cast<int>(sizeof(SQLWCHAR)));
    RecordProperty("configured_driver_sqlwchar_bytes", ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE);
    RecordProperty("qualification_boundary", "manager_before_driver_load");
    RecordProperty("driver_loaded", "false");
    ASSERT_FALSE(driver_loaded());
  }

  void TearDown() override {
    if (connection_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection_));
    }
    if (environment_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment_));
    }
    EXPECT_FALSE(driver_loaded());
  }
};

TEST_F(Odbc2PreDriver, LegacyAllocationSelectsOdbc2AndFreesInOrder) {
  SQLHENV legacy_environment = SQL_NULL_HENV;
  SQLHDBC legacy_connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocEnv(&legacy_environment));
  SQLINTEGER version{};
  EXPECT_EQ(SQL_SUCCESS, SQLGetEnvAttr(legacy_environment, SQL_ATTR_ODBC_VERSION,
      &version, sizeof(version), nullptr));
  EXPECT_EQ(SQL_OV_ODBC2, version);
  EXPECT_EQ(SQL_SUCCESS, SQLAllocConnect(legacy_environment, &legacy_connection));
  if (legacy_connection) {
    EXPECT_EQ(SQL_SUCCESS, SQLFreeConnect(legacy_connection));
  }
  EXPECT_EQ(SQL_SUCCESS, SQLFreeEnv(legacy_environment));
}

TEST_F(Odbc2PreDriver, ExplicitOdbc2SupportsLegacyConnectionAllocation) {
  SQLHDBC legacy_connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocConnect(environment_, &legacy_connection));
  EXPECT_NE(nullptr, legacy_connection);
  EXPECT_EQ(SQL_SUCCESS, SQLFreeConnect(legacy_connection));
}

TEST_F(Odbc2PreDriver, LoginTimeoutAliasesRoundTripWithoutLoadingDriver) {
  struct Output {
    SQLULEN value{};
    std::uint64_t canary{0xaabbccddeeff1234ULL};
  } output;
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectOption(connection_, SQL_LOGIN_TIMEOUT, 7));
  ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(connection_, SQL_ATTR_LOGIN_TIMEOUT,
      &output.value, sizeof(output.value), nullptr));
  EXPECT_EQ(7u, output.value);
  EXPECT_EQ(0xaabbccddeeff1234ULL, output.canary);
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(connection_, SQL_ATTR_LOGIN_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(5), 0));
  output.value = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetConnectOption(connection_, SQL_LOGIN_TIMEOUT, &output.value));
  EXPECT_EQ(5u, output.value);
  EXPECT_EQ(0xaabbccddeeff1234ULL, output.canary);
}

TEST_F(Odbc2PreDriver, StatementAllocationRequiresConnection) {
  SQLHSTMT statement = SQL_NULL_HSTMT;
  EXPECT_EQ(SQL_ERROR, SQLAllocStmt(connection_, &statement));
  EXPECT_EQ(nullptr, statement);
  const auto record = diagnostic(connection_);
  ASSERT_EQ(SQL_SUCCESS, record.result);
  EXPECT_EQ("08003", record.sqlstate()) << record.message.data();
}

TEST_F(Odbc2PreDriver, FunctionArrayBeforeConnectionIsNotCapabilityEvidence) {
  std::array<SQLUSMALLINT, 102> output;
  output.fill(0xbeef);
  EXPECT_EQ(SQL_ERROR, SQLGetFunctions(connection_, SQL_API_ALL_FUNCTIONS, output.data() + 1));
  for (const auto value : output) {
    EXPECT_EQ(0xbeef, value);
  }
  const auto record = diagnostic(connection_);
  ASSERT_EQ(SQL_SUCCESS, record.result);
  EXPECT_EQ("S1010", record.sqlstate()) << record.message.data();
}

TEST_F(Odbc2PreDriver, LegacyErrorIsConsumedAndNewCallClearsDiagnostics) {
  std::array<SQLUSMALLINT, 100> supported{};
  ASSERT_EQ(SQL_ERROR, SQLGetFunctions(connection_, SQL_API_ALL_FUNCTIONS, supported.data()));
  Diagnostic record;
  ASSERT_EQ(SQL_SUCCESS, SQLError(environment_, connection_, SQL_NULL_HSTMT,
      record.state.data(), nullptr, record.message.data(),
      static_cast<SQLSMALLINT>(record.message.size()), nullptr));
  EXPECT_EQ("S1010", record.sqlstate()) << record.message.data();
  EXPECT_EQ(SQL_NO_DATA, SQLError(environment_, connection_, SQL_NULL_HSTMT,
      record.state.data(), nullptr, record.message.data(),
      static_cast<SQLSMALLINT>(record.message.size()), nullptr));
  // Recreate an unread error: draining the previous error alone would not
  // prove that the successful option call clears diagnostics.
  ASSERT_EQ(SQL_ERROR, SQLGetFunctions(connection_, SQL_API_ALL_FUNCTIONS, supported.data()));
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectOption(connection_, SQL_LOGIN_TIMEOUT, 3));
  EXPECT_EQ(SQL_NO_DATA, SQLError(environment_, connection_, SQL_NULL_HSTMT,
      record.state.data(), nullptr, record.message.data(),
      static_cast<SQLSMALLINT>(record.message.size()), nullptr));
}

TEST_F(Odbc2PreDriver, ModernDiagnosticBridgeAfterSuccessfulCall) {
  std::array<SQLUSMALLINT, 100> supported{};
  ASSERT_EQ(SQL_ERROR, SQLGetFunctions(connection_, SQL_API_ALL_FUNCTIONS, supported.data()));
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectOption(connection_, SQL_LOGIN_TIMEOUT, 3));
  SQLRETURN expected = SQL_NO_DATA;
#ifdef ODBCPP_ODBC2_IODBC_SNAPSHOT
  // Evidence: compatibility132/predriver002, iODBC herr.c:1096. The exact
  // manager has no driver diagnostic entry point before load and returns ERROR.
  // This is a manager defect, not ODBC conformance or an ODBC3-profile waiver.
  const auto manager = odbcpp::test::dm::observed_manager_library_path();
  SQLINTEGER version{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
      &version, sizeof(version), nullptr));
  if (manager_version_ == "03.52.1623.0502" && version == SQL_OV_ODBC2 &&
      sizeof(void*) == 8 && sizeof(SQLWCHAR) == 4 && !driver_loaded() &&
      manager == ODBCPP_ODBC2_IODBC_PATH &&
      odbcpp::test::dm::same_manager_file_bytes(manager, ODBCPP_ODBC2_IODBC_SNAPSHOT)) {
    expected = SQL_ERROR;
    RecordProperty("known_manager_defect", "iodbc-cleared-predriver-dbc-diagnostic");
    RecordProperty("dm_qualified_sha256", ODBCPP_ODBC2_IODBC_SHA256);
  }
#endif
  EXPECT_EQ(expected, diagnostic(connection_).result);
}

// A separate CTest process loads the exact candidate. This process must never
// share the pre-driver filter: unloading cannot prove that a driver never ran.
class DriverModule {
 public:
  DriverModule() {
#ifdef _WIN32
    module_ = LoadLibraryExW(std::filesystem::path(kDriver).wstring().c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    module_ = dlopen(kDriver, RTLD_NOW | RTLD_LOCAL);
#endif
  }
  ~DriverModule() {
    if (module_) {
#ifdef _WIN32
      FreeLibrary(module_);
#else
      dlclose(module_);
#endif
    }
  }
  DriverModule(const DriverModule&) = delete;
  DriverModule& operator=(const DriverModule&) = delete;
  bool loaded() const { return module_ != nullptr; }
  bool exports(const char* name) const {
#ifdef _WIN32
    return GetProcAddress(module_, name) != nullptr;
#else
    return dlsym(module_, name) != nullptr;
#endif
  }
 private:
#ifdef _WIN32
  HMODULE module_{};
#else
  void* module_{};
#endif
};

TEST(Odbc2DriverExports, ModernReplacementsAndRetainedLegacyDiagnostics) {
  DriverModule driver;
  ASSERT_TRUE(driver.loaded()) << "Could not load the configured candidate driver";
  RecordProperty("qualification_boundary", "driver_loaded_without_connection");
  RecordProperty("driver_library", kDriver);
  for (const auto* name : {"SQLAllocHandle", "SQLFreeHandle", "SQLSetEnvAttr",
      "SQLGetEnvAttr", "SQLGetConnectAttr", "SQLSetConnectAttr", "SQLGetStmtAttr",
      "SQLSetStmtAttr", "SQLEndTran", "SQLGetDiagRec", "SQLError", "SQLErrorW",
      "SQLFreeStmt", "SQLGetFunctions"}) {
    EXPECT_TRUE(driver.exports(name)) << name;
  }
  for (const auto* name : {"SQLAllocEnv", "SQLAllocConnect", "SQLAllocStmt",
      "SQLFreeEnv", "SQLFreeConnect", "SQLGetConnectOption", "SQLSetConnectOption",
      "SQLGetStmtOption", "SQLSetStmtOption", "SQLTransact"}) {
    EXPECT_FALSE(driver.exports(name)) << name << " now has a native implementation; review mapping coverage";
  }
}

class Odbc2Connected : public ::testing::TestWithParam<bool> {
 protected:
  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  SQLHSTMT statement_{SQL_NULL_HSTMT};
  bool connected_{};

  Diagnostic statement_diagnostic() const {
    Diagnostic result;
    result.result = SQLGetDiagRec(SQL_HANDLE_STMT, statement_, 1,
        result.state.data(), nullptr, result.message.data(),
        static_cast<SQLSMALLINT>(result.message.size()), nullptr);
    return result;
  }
  SQLRETURN execute(const char* sql) const {
    return SQLExecDirect(statement_, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)), SQL_NTS);
  }
  void SetUp() override {
    // Only a separately provisioned, explicit loopback PostgreSQL fixture is
    // accepted. Never consume ambient DSNs, cloud inputs or connection strings.
    const auto* port_text = std::getenv("ODBCPP_ODBC2_TEST_PORT");
    ASSERT_NE(nullptr, port_text) << "Provision the isolated PostgreSQL fixture first";
    const std::string port(port_text);
    ASSERT_FALSE(port.empty());
    ASSERT_LE(port.size(), 5u);
    ASSERT_EQ(std::string::npos, port.find_first_not_of("0123456789"));
    const auto port_number = std::strtoul(port.c_str(), nullptr, 10);
    ASSERT_GT(port_number, 0u);
    ASSERT_LE(port_number, 65535u);
    if (GetParam()) {
      ASSERT_EQ(SQL_SUCCESS, SQLAllocEnv(&environment_));
    } else {
      ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
      ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC2), 0));
    }
    SQLINTEGER version{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
        &version, sizeof(version), nullptr));
    ASSERT_EQ(SQL_OV_ODBC2, version);
    ASSERT_EQ(SQL_SUCCESS, SQLAllocConnect(environment_, &connection_));
    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectOption(connection_, SQL_LOGIN_TIMEOUT, 5));
    std::string text = "DRIVER=" + std::string(kDriver) + ";SERVER=127.0.0.1;PORT=" + port +
        ";DATABASE=odbc2_132;UID=odbc2_132;SSL=off;";
    // No output connection-string buffer: no credentials are echoed into XML.
    const auto result = SQLDriverConnect(connection_, nullptr,
        reinterpret_cast<SQLCHAR*>(text.data()), SQL_NTS, nullptr, 0, nullptr,
        SQL_DRIVER_NOPROMPT);
    connected_ = SQL_SUCCEEDED(result);
    ASSERT_TRUE(connected_) << diagnostic(connection_).message.data();
    ASSERT_TRUE(driver_loaded());
    SQLCHAR value[256]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_DM_VER, value, sizeof(value), nullptr));
    RecordProperty("dm_version", reinterpret_cast<const char*>(value));
    RecordProperty("dm_loaded_library", odbcpp::test::dm::observed_manager_library_path());
    RecordProperty("dm_odbc_environment", static_cast<int>(version));
    RecordProperty("dm_pointer_bytes", static_cast<int>(sizeof(void*)));
    RecordProperty("dm_application_sqlwchar_bytes", static_cast<int>(sizeof(SQLWCHAR)));
    RecordProperty("configured_driver_sqlwchar_bytes", ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE);
    RecordProperty("qualification_boundary", "connected_loopback_postgresql");
    RecordProperty("environment_selection", GetParam() ? "SQLAllocEnv" : "explicit_SQL_OV_ODBC2");
    RecordProperty("driver_library", kDriver);
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_DRIVER_VER, value, sizeof(value), nullptr));
    RecordProperty("driver_version", reinterpret_cast<const char*>(value));
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_DRIVER_ODBC_VER, value, sizeof(value), nullptr));
    RecordProperty("driver_odbc_version", reinterpret_cast<const char*>(value));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocStmt(connection_, &statement_));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtOption(statement_, SQL_QUERY_TIMEOUT, 5));
  }
  void TearDown() override {
    if (statement_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_DROP));
    }
    if (connected_) {
      // Transaction cases may abort at any assertion; release their transaction
      // before disconnect, which also retires the session-local temp table.
      EXPECT_EQ(SQL_SUCCESS, SQLTransact(environment_, connection_, SQL_ROLLBACK));
      EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));
    }
    if (connection_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeConnect(connection_));
    }
    if (environment_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeEnv(environment_));
    }
  }
};

TEST_P(Odbc2Connected, LegacyStatementOptionsAndFetch) {
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtOption(statement_, SQL_QUERY_TIMEOUT, 3));
  struct Output { SQLULEN value{}; std::uint64_t guard{0x123456789abcdef0ULL}; } output;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(statement_, SQL_ATTR_QUERY_TIMEOUT,
      &output.value, sizeof(output.value), nullptr));
  EXPECT_EQ(3u, output.value);
  EXPECT_EQ(0x123456789abcdef0ULL, output.guard);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(statement_, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(4), 0));
  output.value = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtOption(statement_, SQL_QUERY_TIMEOUT, &output.value));
  EXPECT_EQ(4u, output.value);
  EXPECT_EQ(0x123456789abcdef0ULL, output.guard);
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT 42::integer")) << statement_diagnostic().message.data();
  SQLINTEGER value{};
  SQLLEN length{};
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(statement_, 1, SQL_C_SLONG, &value, sizeof(value), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  EXPECT_EQ(42, value);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_UNBIND));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
}

TEST_P(Odbc2Connected, LegacyTransactionCommitAndRollback) {
  ASSERT_EQ(SQL_SUCCESS, execute("CREATE TEMP TABLE odbc2_transaction(value integer)"));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectOption(connection_, SQL_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF));
  SQLULEN autocommit = SQL_AUTOCOMMIT_ON;
  ASSERT_EQ(SQL_SUCCESS, SQLGetConnectOption(connection_, SQL_AUTOCOMMIT, &autocommit));
  EXPECT_EQ(static_cast<SQLULEN>(SQL_AUTOCOMMIT_OFF), autocommit);
  ASSERT_EQ(SQL_SUCCESS, execute("INSERT INTO odbc2_transaction VALUES (1)"));
  ASSERT_EQ(SQL_SUCCESS, SQLTransact(environment_, connection_, SQL_ROLLBACK));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT count(*)::integer FROM odbc2_transaction"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  SQLINTEGER count = -1;
  SQLLEN length{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(statement_, 1, SQL_C_SLONG, &count, sizeof(count), &length));
  EXPECT_EQ(0, count);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, execute("INSERT INTO odbc2_transaction VALUES (2)"));
  ASSERT_EQ(SQL_SUCCESS, SQLTransact(environment_, connection_, SQL_COMMIT));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT count(*)::integer FROM odbc2_transaction"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(statement_, 1, SQL_C_SLONG, &count, sizeof(count), &length));
  EXPECT_EQ(1, count);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  EXPECT_EQ(SQL_SUCCESS, SQLTransact(environment_, connection_, SQL_ROLLBACK));
  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectOption(connection_, SQL_AUTOCOMMIT, SQL_AUTOCOMMIT_ON));
}

TEST_P(Odbc2Connected, FunctionArrayUsesOdbc2BooleansAndAgreesWithSingleQueries) {
  std::array<SQLUSMALLINT, 102> functions;
  functions.fill(0xbeef);
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(connection_, SQL_API_ALL_FUNCTIONS, functions.data() + 1));
  EXPECT_EQ(0xbeef, functions.front());
  EXPECT_EQ(0xbeef, functions.back());
  for (std::size_t i = 1; i <= 100; ++i) {
    EXPECT_TRUE(functions[i] == SQL_TRUE || functions[i] == SQL_FALSE) << i - 1;
  }
  for (const SQLUSMALLINT id : {SQL_API_SQLALLOCENV, SQL_API_SQLALLOCCONNECT,
      SQL_API_SQLALLOCSTMT, SQL_API_SQLFREEENV, SQL_API_SQLFREECONNECT,
      SQL_API_SQLFREESTMT, SQL_API_SQLSETCONNECTOPTION, SQL_API_SQLGETCONNECTOPTION,
      SQL_API_SQLSETSTMTOPTION, SQL_API_SQLGETSTMTOPTION, SQL_API_SQLTRANSACT,
      SQL_API_SQLERROR, SQL_API_SQLGETFUNCTIONS, SQL_API_SQLEXECDIRECT, SQL_API_SQLFETCH}) {
    struct Output { SQLUSMALLINT value{0xbeef}; SQLUSMALLINT guard{0xabcd}; } single;
    ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(connection_, id, &single.value)) << id;
    EXPECT_EQ(0xabcd, single.guard) << id;
    EXPECT_EQ(SQL_TRUE, single.value) << id;
    EXPECT_EQ(single.value, functions[1 + id]) << id;
  }
}

TEST_P(Odbc2Connected, LegacyStatementErrorAndSqlstateMapping) {
  ASSERT_EQ(SQL_ERROR, SQLSetStmtOption(statement_, 0xffff, 1));
  Diagnostic record;
  ASSERT_EQ(SQL_SUCCESS, SQLError(environment_, connection_, statement_,
      record.state.data(), nullptr, record.message.data(),
      static_cast<SQLSMALLINT>(record.message.size()), nullptr));
  EXPECT_EQ("S1092", record.sqlstate()) << record.message.data();
  EXPECT_EQ(SQL_NO_DATA, SQLError(environment_, connection_, statement_,
      record.state.data(), nullptr, record.message.data(),
      static_cast<SQLSMALLINT>(record.message.size()), nullptr));
  ASSERT_EQ(SQL_ERROR, SQLSetStmtOption(statement_, 0xffff, 1));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtOption(statement_, SQL_QUERY_TIMEOUT, 5));
  EXPECT_EQ(SQL_NO_DATA, SQLError(environment_, connection_, statement_,
      record.state.data(), nullptr, record.message.data(),
      static_cast<SQLSMALLINT>(record.message.size()), nullptr));
}

INSTANTIATE_TEST_SUITE_P(EnvironmentSelection, Odbc2Connected, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
      return info.param ? "ImplicitLegacy" : "ExplicitOdbc2";
    });

}  // namespace
#ifdef _MSC_VER
#pragma warning(pop)
#endif
