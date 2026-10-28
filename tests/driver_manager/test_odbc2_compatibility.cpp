#include "tests/test_dm_odbc.h"
#include "tests/test_dm_identity.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
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
  SQLUSMALLINT supported = 0xbeef;
  ASSERT_EQ(SQL_ERROR, SQLGetFunctions(connection_, SQL_API_SQLGETFUNCTIONS, &supported));
  Diagnostic record;
  ASSERT_EQ(SQL_SUCCESS, SQLError(environment_, connection_, SQL_NULL_HSTMT,
      record.state.data(), nullptr, record.message.data(),
      static_cast<SQLSMALLINT>(record.message.size()), nullptr));
  EXPECT_EQ("S1010", record.sqlstate()) << record.message.data();
  EXPECT_EQ(SQL_NO_DATA, SQLError(environment_, connection_, SQL_NULL_HSTMT,
      record.state.data(), nullptr, record.message.data(),
      static_cast<SQLSMALLINT>(record.message.size()), nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectOption(connection_, SQL_LOGIN_TIMEOUT, 3));
  EXPECT_EQ(SQL_NO_DATA, diagnostic(connection_).result);
}
}  // namespace
#ifdef _MSC_VER
#pragma warning(pop)
#endif
