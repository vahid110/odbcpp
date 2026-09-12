#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "core/util/deadline.h"
#include "tests/test_handle_helpers.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>

namespace {

class AttributeApisTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
                            integer_value(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    statement_ = odbcpp::test::make_statement(connection_);
    ASSERT_NE(nullptr, statement_);
  }

  void TearDown() override {
    if (statement_) SQLFreeHandle(SQL_HANDLE_STMT, statement_);
    if (connection_) SQLFreeHandle(SQL_HANDLE_DBC, connection_);
    if (environment_) SQLFreeHandle(SQL_HANDLE_ENV, environment_);
  }

  static SQLPOINTER integer_value(SQLULEN value) {
    return reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(value));
  }

  static std::string diagnostic_state(SQLSMALLINT handle_type,
                                      SQLHANDLE handle) {
    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        handle_type, handle, 1, state, nullptr, nullptr, 0, nullptr));
    return reinterpret_cast<const char*>(state);
  }

  SQLHENV environment_{nullptr};
  SQLHDBC connection_{nullptr};
  SQLHSTMT statement_{nullptr};
};

TEST_F(AttributeApisTest, StoresLoginTimeout) {
  SQLUINTEGER value = 0;
  SQLINTEGER length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, &value, sizeof(value), &length));
  EXPECT_EQ(30u, value);
  EXPECT_EQ(sizeof(SQLUINTEGER), static_cast<std::size_t>(length));

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, integer_value(7), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, &value, sizeof(value), nullptr));
  EXPECT_EQ(7u, value);
}

TEST_F(AttributeApisTest, StoresConnectionTimeoutAndClassifiesPacketSize) {
  SQLUINTEGER value = 99;
  SQLINTEGER length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_CONNECTION_TIMEOUT, &value, sizeof(value),
      &length));
  EXPECT_EQ(0u, value);
  EXPECT_EQ(sizeof(SQLUINTEGER), static_cast<std::size_t>(length));

  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, SQL_ATTR_CONNECTION_TIMEOUT, integer_value(11), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttrW(
      connection_, SQL_ATTR_CONNECTION_TIMEOUT, &value, sizeof(value),
      nullptr));
  EXPECT_EQ(11u, value);
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttrW(
      connection_, SQL_ATTR_CONNECTION_TIMEOUT, integer_value(0), 0));

  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(
      connection_, SQL_ATTR_PACKET_SIZE, integer_value(8192), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
  value = 123;
  EXPECT_EQ(SQL_ERROR, SQLGetConnectAttr(
      connection_, SQL_ATTR_PACKET_SIZE, &value, sizeof(value), nullptr));
  EXPECT_EQ(123u, value);
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, StoresEnvironmentVersion) {
  SQLINTEGER version = 0;
  SQLINTEGER length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetEnvAttr(
      environment_, SQL_ATTR_ODBC_VERSION, &version, sizeof(version), &length));
  EXPECT_EQ(SQL_OV_ODBC3, version);
  EXPECT_EQ(sizeof(SQLINTEGER), static_cast<std::size_t>(length));

#ifdef SQL_OV_ODBC3_80
  EXPECT_EQ(SQL_ERROR, SQLSetEnvAttr(
      environment_, SQL_ATTR_ODBC_VERSION, integer_value(SQL_OV_ODBC3_80), 0));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_ENV, environment_));
#endif

  EXPECT_EQ(SQL_ERROR, SQLSetEnvAttr(
      environment_, SQL_ATTR_ODBC_VERSION, integer_value(999), 0));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_ENV, environment_));
}

TEST(AttributeApisStandaloneTest, RejectsInvalidEnvironmentVersion) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  EXPECT_EQ(SQL_ERROR,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(std::uintptr_t{999}),
                          0));
  SQLCHAR state[6]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(SQL_HANDLE_ENV, environment, 1, state, nullptr,
                          nullptr, 0, nullptr));
  EXPECT_STREQ("HY024", reinterpret_cast<const char*>(state));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(AttributeApisStandaloneTest, EnvironmentVersionGetSetMatrix) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));

  SQLINTEGER value = 99;
  SQLINTEGER length = -1;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetEnvAttr(environment, SQL_ATTR_ODBC_VERSION, &value, -1,
                          &length));
  EXPECT_EQ(0, value);
  EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(value)), length);

  constexpr SQLINTEGER versions[]{
      SQL_OV_ODBC2,
      SQL_OV_ODBC3,
#ifdef SQL_OV_ODBC3_80
      SQL_OV_ODBC3_80,
#endif
  };
  for (const auto version : versions) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(
                  environment, SQL_ATTR_ODBC_VERSION,
                  reinterpret_cast<SQLPOINTER>(
                      static_cast<std::uintptr_t>(version)),
                  123));
    value = 0;
    length = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetEnvAttr(environment, SQL_ATTR_ODBC_VERSION, &value, -1,
                            &length));
    EXPECT_EQ(version, value);
    EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(value)), length);
  }

  length = 71;
  EXPECT_EQ(SQL_ERROR,
            SQLGetEnvAttr(environment, SQL_ATTR_ODBC_VERSION, nullptr, 0,
                          &length));
  EXPECT_EQ(71, length);
  SQLCHAR state[6]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(SQL_HANDLE_ENV, environment, 1, state, nullptr,
                          nullptr, 0, nullptr));
  EXPECT_STREQ("HY009", reinterpret_cast<const char*>(state));

  value = 42;
  length = 71;
  EXPECT_EQ(SQL_ERROR,
            SQLGetEnvAttr(environment, 0x7fffffff, &value, sizeof(value),
                          &length));
  EXPECT_EQ(42, value);
  EXPECT_EQ(71, length);
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(SQL_HANDLE_ENV, environment, 1, state, nullptr,
                          nullptr, 0, nullptr));
  EXPECT_STREQ("HY092", reinterpret_cast<const char*>(state));
  EXPECT_EQ(SQL_ERROR,
            SQLSetEnvAttr(environment, 0x7fffffff, nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(SQL_HANDLE_ENV, environment, 1, state, nullptr,
                          nullptr, 0, nullptr));
  EXPECT_STREQ("HY092", reinterpret_cast<const char*>(state));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(AttributeApisStandaloneTest, SupportsNullTerminatedEnvironmentOutput) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));

  SQLINTEGER value = 99;
  EXPECT_EQ(SQL_ERROR, SQLGetEnvAttr(
      environment, SQL_ATTR_OUTPUT_NTS, &value, sizeof(value), nullptr));
  SQLCHAR state[6]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
      SQL_HANDLE_ENV, environment, 1, state, nullptr, nullptr, 0, nullptr));
  EXPECT_STREQ("HY010", reinterpret_cast<const char*>(state));

  ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(
      environment, SQL_ATTR_ODBC_VERSION,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_OV_ODBC3}), 0));
  SQLINTEGER length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetEnvAttr(
      environment, SQL_ATTR_OUTPUT_NTS, &value, -1, &length));
  EXPECT_EQ(SQL_TRUE, value);
  EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(value)), length);
  EXPECT_EQ(SQL_SUCCESS, SQLSetEnvAttr(
      environment, SQL_ATTR_OUTPUT_NTS,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_TRUE}), 0));
  EXPECT_EQ(SQL_ERROR, SQLSetEnvAttr(
      environment, SQL_ATTR_OUTPUT_NTS,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_FALSE}), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
      SQL_HANDLE_ENV, environment, 1, state, nullptr, nullptr, 0, nullptr));
  EXPECT_STREQ("HYC00", reinterpret_cast<const char*>(state));
  EXPECT_EQ(SQL_ERROR, SQLSetEnvAttr(
      environment, SQL_ATTR_OUTPUT_NTS,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{99}), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
      SQL_HANDLE_ENV, environment, 1, state, nullptr, nullptr, 0, nullptr));
  EXPECT_STREQ("HY024", reinterpret_cast<const char*>(state));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST_F(AttributeApisTest, RejectsEnvironmentChangesAfterConnectionAllocation) {
  EXPECT_EQ(SQL_ERROR, SQLSetEnvAttr(
      environment_, SQL_ATTR_OUTPUT_NTS, integer_value(SQL_TRUE), 0));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_ENV, environment_));
}

TEST_F(AttributeApisTest, NegotiatesNativeSqlwcharEncodingWithIodbc) {
  SQLINTEGER driver_encoding = 0;
  SQLINTEGER length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetEnvAttr(
      environment_, rs::odbc::IODBC_ATTR_DRIVER_UNICODE_TYPE,
      &driver_encoding, sizeof(driver_encoding), &length));
  EXPECT_EQ(static_cast<SQLINTEGER>(rs::odbc::NATIVE_SQLWCHAR_ENCODING),
            driver_encoding);
  EXPECT_EQ(sizeof(SQLINTEGER), static_cast<std::size_t>(length));

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, rs::odbc::IODBC_ATTR_APP_WCHAR_TYPE,
      integer_value(rs::odbc::NATIVE_SQLWCHAR_ENCODING), 0));

  const auto other_encoding = sizeof(SQLWCHAR) == 2
      ? rs::odbc::IODBC_CP_UCS4 : rs::odbc::IODBC_CP_UTF16;
  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(
      connection_, rs::odbc::IODBC_ATTR_APP_WCHAR_TYPE,
      integer_value(other_encoding), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, StoresAutocommitMode) {
  SQLUINTEGER value = 99;
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_AUTOCOMMIT, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_AUTOCOMMIT_ON, value);

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, SQL_ATTR_AUTOCOMMIT,
      integer_value(SQL_AUTOCOMMIT_OFF), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_AUTOCOMMIT, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_AUTOCOMMIT_OFF, value);

  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(
      connection_, SQL_ATTR_AUTOCOMMIT, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, ReportsCommonConnectionAttributeDefaults) {
  const auto expect_attribute = [&](SQLINTEGER attribute,
                                    SQLUINTEGER expected) {
    SQLUINTEGER value = std::numeric_limits<SQLUINTEGER>::max();
    SQLINTEGER length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetConnectAttr(connection_, attribute, &value,
                                sizeof(value), &length));
    EXPECT_EQ(expected, value);
    EXPECT_EQ(sizeof(SQLUINTEGER), static_cast<std::size_t>(length));
  };

  expect_attribute(SQL_ATTR_ACCESS_MODE, SQL_MODE_READ_WRITE);
  expect_attribute(SQL_ATTR_ASYNC_ENABLE, SQL_ASYNC_ENABLE_OFF);
  expect_attribute(SQL_ATTR_AUTO_IPD, SQL_FALSE);
  expect_attribute(SQL_ATTR_METADATA_ID, SQL_FALSE);

  SQLUINTEGER dead = SQL_CD_TRUE;
  EXPECT_EQ(SQL_ERROR,
            SQLGetConnectAttr(connection_, SQL_ATTR_CONNECTION_DEAD, &dead,
                              sizeof(dead), nullptr));
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(connection_, SQL_ATTR_ACCESS_MODE,
                              integer_value(SQL_MODE_READ_WRITE), 0));
  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(connection_, SQL_ATTR_ACCESS_MODE,
                              integer_value(SQL_MODE_READ_ONLY), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(connection_, SQL_ATTR_ASYNC_ENABLE,
                              integer_value(SQL_ASYNC_ENABLE_OFF), 0));
  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(connection_, SQL_ATTR_ASYNC_ENABLE,
                              integer_value(SQL_ASYNC_ENABLE_ON), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(connection_, SQL_ATTR_METADATA_ID,
                              integer_value(SQL_FALSE), 0));
  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(connection_, SQL_ATTR_METADATA_ID,
                              integer_value(SQL_TRUE), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(connection_, SQL_ATTR_CONNECTION_DEAD,
                              integer_value(SQL_CD_FALSE), 0));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest,
       ClassifiesStandardConnectionAttributesAndPreservesPointers) {
  auto quiet_mode = reinterpret_cast<SQLHWND>(std::uintptr_t{0x12345678});
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(connection_, SQL_ATTR_QUIET_MODE, quiet_mode, 0));
  SQLHWND returned_quiet_mode = nullptr;
  SQLINTEGER length = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection_, SQL_ATTR_QUIET_MODE,
                              &returned_quiet_mode,
                              sizeof(returned_quiet_mode), &length));
  EXPECT_EQ(quiet_mode, returned_quiet_mode);
  EXPECT_EQ(sizeof(SQLHWND), static_cast<std::size_t>(length));

#ifdef SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE
  SQLUINTEGER async_mode = 99;
  EXPECT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(
                connection_, SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE,
                integer_value(SQL_ASYNC_DBC_ENABLE_OFF), 0));
  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(
                connection_, SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE,
                integer_value(SQL_ASYNC_DBC_ENABLE_ON), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(
                connection_, SQL_ATTR_ASYNC_DBC_FUNCTIONS_ENABLE,
                &async_mode, sizeof(async_mode), nullptr));
  EXPECT_EQ(SQL_ASYNC_DBC_ENABLE_OFF, async_mode);
#endif

  SQLUINTEGER untouched = 77;
  for (const auto attribute : {
           SQL_ATTR_TRANSLATE_OPTION, SQL_ATTR_DISCONNECT_BEHAVIOR,
           SQL_ATTR_ENLIST_IN_DTC}) {
    EXPECT_EQ(SQL_ERROR,
              SQLSetConnectAttr(connection_, attribute, integer_value(0), 0));
    EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
    EXPECT_EQ(SQL_ERROR,
              SQLGetConnectAttr(connection_, attribute, &untouched,
                                sizeof(untouched), nullptr));
    EXPECT_EQ(77u, untouched);
    EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
  }

  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(connection_, SQL_ATTR_AUTO_IPD,
                              integer_value(SQL_FALSE), 0));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, StoresCurrentCatalogBeforeConnecting) {
  SQLINTEGER length = -1;
  EXPECT_EQ(SQL_NO_DATA,
            SQLGetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              nullptr, 0, &length));
  EXPECT_EQ(-1, length);

  SQLCHAR catalog[]{'p', 'o', 's', 't', 'g', 'r', 'e', 's', 0};
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              catalog, SQL_NTS));

  SQLCHAR narrow[16]{};
  length = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              narrow, sizeof(narrow), &length));
  EXPECT_STREQ("postgres", reinterpret_cast<char*>(narrow));
  EXPECT_EQ(8, length);

  SQLWCHAR wide[16]{};
  length = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttrW(connection_, SQL_ATTR_CURRENT_CATALOG,
                               wide, sizeof(wide), &length));
  EXPECT_EQ(8 * static_cast<SQLINTEGER>(sizeof(SQLWCHAR)), length);
  EXPECT_EQ(static_cast<SQLWCHAR>('p'), wide[0]);
  EXPECT_EQ(0, wide[8]);

  length = 0;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              nullptr, 0, &length));
  EXPECT_EQ(8, length);

  SQLCHAR truncated[5]{};
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLGetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              truncated, sizeof(truncated), &length));
  EXPECT_STREQ("post", reinterpret_cast<char*>(truncated));
  EXPECT_EQ(8, length);
  EXPECT_EQ("01004", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              nullptr, SQL_NTS));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              catalog, -2));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttrW(connection_, SQL_ATTR_CURRENT_CATALOG,
                               wide, 1));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_DBC, connection_));

  SQLCHAR replacement[]{'t', 'e', 'm', 'p', 'l', 'a', 't', 'e', '1', 0};
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              replacement, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection_, SQL_ATTR_CURRENT_CATALOG,
                              narrow, sizeof(narrow), &length));
  EXPECT_STREQ("template1", reinterpret_cast<char*>(narrow));
  EXPECT_EQ(9, length);
}

TEST_F(AttributeApisTest, StoresTransactionIsolation) {
  SQLUINTEGER value = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_TXN_ISOLATION, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_TXN_READ_COMMITTED, value);

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, SQL_ATTR_TXN_ISOLATION,
      integer_value(SQL_TXN_REPEATABLE_READ), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_TXN_ISOLATION, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_TXN_REPEATABLE_READ, value);

  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(
      connection_, SQL_ATTR_TXN_ISOLATION, integer_value(0), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, EndTransactionValidatesState) {
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_DBC, connection_, SQL_COMMIT));
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_DBC, connection_, 99));
  EXPECT_EQ("HY012", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_SUCCESS,
            SQLEndTran(SQL_HANDLE_ENV, environment_, SQL_COMMIT));
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_ENV, environment_, 99));
  EXPECT_EQ("HY012", diagnostic_state(SQL_HANDLE_ENV, environment_));
}

TEST_F(AttributeApisTest, OnlyOdbcVersionIsAvailableBeforeConnect) {
  SQLCHAR odbc_version[8]{};
  SQLSMALLINT version_length = -1;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetInfo(connection_, SQL_ODBC_VER, odbc_version,
                       sizeof(odbc_version), &version_length));
  EXPECT_STREQ("03.80", reinterpret_cast<const char*>(odbc_version));
  EXPECT_EQ(5, version_length);

  SQLCHAR value[]{'k', 'e', 'e', 'p', 0};
  SQLSMALLINT length = 77;
  constexpr std::array<SQLUSMALLINT, 8> connected_info_types{
      SQL_DRIVER_NAME, SQL_TXN_CAPABLE, SQL_DATA_SOURCE_NAME,
      SQL_DATABASE_NAME, SQL_DBMS_VER, SQL_SERVER_NAME, SQL_USER_NAME,
      static_cast<SQLUSMALLINT>(0xffff)};
  for (const auto info_type : connected_info_types) {
    EXPECT_EQ(SQL_ERROR, SQLGetInfo(
        connection_, info_type, value, sizeof(value), &length));
    EXPECT_STREQ("keep", reinterpret_cast<const char*>(value));
    EXPECT_EQ(77, length);
    EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));
  }
}

TEST_F(AttributeApisTest, ReportsInformationStringTruncation) {
  SQLCHAR value[5]{};
  SQLSMALLINT required = 0;
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLGetInfo(connection_, SQL_ODBC_VER, value, sizeof(value),
                       &required));
  EXPECT_STREQ("03.8", reinterpret_cast<char*>(value));
  EXPECT_EQ(5, required);
  EXPECT_EQ("01004", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, GetFunctionsRequiresACompletedConnection) {
  SQLUSMALLINT supported = SQL_TRUE;
  EXPECT_EQ(SQL_ERROR,
            SQLGetFunctions(connection_, SQL_API_SQLCONNECT, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_ERROR,
            SQLGetFunctions(connection_, SQL_API_SQLCONNECT, nullptr));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, StoresQueryTimeoutAndAllowsZero) {
  SQLULEN value = 99;
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, &value, sizeof(value), nullptr));
  EXPECT_EQ(0u, value);

  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, integer_value(3), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, &value, sizeof(value), nullptr));
  EXPECT_EQ(3u, value);
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, integer_value(0), 0));
}

TEST_F(AttributeApisTest, WideAttributeEntryPointsMatchAnsiBehavior) {
  SQLUINTEGER connection_value = 0;
  SQLINTEGER connection_length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttrW(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, integer_value(9), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttrW(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, &connection_value,
      sizeof(connection_value), &connection_length));
  EXPECT_EQ(9u, connection_value);
  EXPECT_EQ(sizeof(SQLUINTEGER),
            static_cast<std::size_t>(connection_length));

  SQLULEN statement_value = 0;
  SQLINTEGER statement_length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttrW(
      statement_, SQL_ATTR_QUERY_TIMEOUT, integer_value(4), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttrW(
      statement_, SQL_ATTR_QUERY_TIMEOUT, &statement_value,
      sizeof(statement_value), &statement_length));
  EXPECT_EQ(4u, statement_value);
  EXPECT_EQ(sizeof(SQLULEN), static_cast<std::size_t>(statement_length));
}

TEST_F(AttributeApisTest, ReportsAllImplicitDescriptorHandles) {
  const SQLINTEGER attributes[] = {
      SQL_ATTR_APP_ROW_DESC, SQL_ATTR_APP_PARAM_DESC,
      SQL_ATTR_IMP_ROW_DESC, SQL_ATTR_IMP_PARAM_DESC};
  for (const auto attribute : attributes) {
    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        statement_, attribute, &descriptor, sizeof(descriptor), nullptr));
    ASSERT_NE(nullptr, descriptor);

    SQLSMALLINT count = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 0, SQL_DESC_COUNT, &count, 0, nullptr));
    EXPECT_EQ(0, count);
    EXPECT_EQ(SQL_ERROR, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
    EXPECT_EQ("HY017", diagnostic_state(SQL_HANDLE_DESC, descriptor));
  }
}

TEST_F(AttributeApisTest, AssociatesAndReleasesExplicitDescriptors) {
  SQLHDESC automatic_row = SQL_NULL_HDESC;
  SQLHDESC automatic_param = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, &automatic_row, 0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_PARAM_DESC, &automatic_param, 0, nullptr));
  const auto descriptor = odbcpp::test::make_descriptor(connection_);
  ASSERT_NE(nullptr, descriptor);

  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, descriptor, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttrW(
      statement_, SQL_ATTR_APP_PARAM_DESC, descriptor, 0));
  const auto other_statement = odbcpp::test::make_statement(connection_);
  ASSERT_NE(nullptr, other_statement);
  SQLHDESC other_automatic = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      other_statement, SQL_ATTR_APP_ROW_DESC, &other_automatic, 0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      other_statement, SQL_ATTR_APP_ROW_DESC, descriptor, 0));
  SQLHDESC associated = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, &associated, 0, nullptr));
  EXPECT_EQ(descriptor, associated);
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttrW(
      statement_, SQL_ATTR_APP_PARAM_DESC, &associated, 0, nullptr));
  EXPECT_EQ(descriptor, associated);

  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, &associated, 0, nullptr));
  EXPECT_EQ(automatic_row, associated);
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_PARAM_DESC, &associated, 0, nullptr));
  EXPECT_EQ(automatic_param, associated);
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      other_statement, SQL_ATTR_APP_ROW_DESC, &associated, 0, nullptr));
  EXPECT_EQ(other_automatic, associated);
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other_statement));
}

TEST_F(AttributeApisTest, ValidatesApplicationDescriptorAssociations) {
  SQLHDESC implementation = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_IMP_ROW_DESC, &implementation, 0, nullptr));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_IMP_ROW_DESC, implementation, 0));
  EXPECT_EQ("HY017", diagnostic_state(SQL_HANDLE_STMT, statement_));

  const auto other_statement = odbcpp::test::make_statement(connection_);
  ASSERT_NE(nullptr, other_statement);
  SQLHDESC other_automatic = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      other_statement, SQL_ATTR_APP_ROW_DESC, &other_automatic, 0, nullptr));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, other_automatic, 0));
  EXPECT_EQ("HY017", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other_statement));

  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{1}), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLHENV other_environment = SQL_NULL_HENV;
  SQLHDBC other_connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
      SQL_HANDLE_ENV, SQL_NULL_HANDLE, &other_environment));
  ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(
      other_environment, SQL_ATTR_ODBC_VERSION,
      integer_value(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
      SQL_HANDLE_DBC, other_environment, &other_connection));
  const auto foreign_descriptor =
      odbcpp::test::make_descriptor(other_connection);
  ASSERT_NE(nullptr, foreign_descriptor);
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_APP_PARAM_DESC, foreign_descriptor, 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_SUCCESS,
            SQLFreeHandle(SQL_HANDLE_DESC, foreign_descriptor));
  EXPECT_EQ(SQL_SUCCESS,
            SQLFreeHandle(SQL_HANDLE_DBC, other_connection));
  EXPECT_EQ(SQL_SUCCESS,
            SQLFreeHandle(SQL_HANDLE_ENV, other_environment));
}

TEST_F(AttributeApisTest, NullDescriptorRestoresAutomaticAssociation) {
  SQLHDESC automatic = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, &automatic, 0, nullptr));
  const auto descriptor = odbcpp::test::make_descriptor(connection_);
  ASSERT_NE(nullptr, descriptor);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, descriptor, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, SQL_NULL_HDESC, 0));

  SQLHDESC associated = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, &associated, 0, nullptr));
  EXPECT_EQ(automatic, associated);
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

TEST_F(AttributeApisTest, ReportsForwardOnlyStatementDefaults) {
  struct AttributeExpectation {
    SQLINTEGER attribute;
    SQLULEN value;
  };
  const AttributeExpectation expectations[] = {
      {SQL_ATTR_CURSOR_TYPE, SQL_CURSOR_FORWARD_ONLY},
      {SQL_ATTR_CONCURRENCY, SQL_CONCUR_READ_ONLY},
      {SQL_ATTR_CURSOR_SCROLLABLE, SQL_NONSCROLLABLE},
      {SQL_ATTR_CURSOR_SENSITIVITY, SQL_UNSPECIFIED},
      {SQL_ATTR_ENABLE_AUTO_IPD, SQL_FALSE},
      {SQL_ATTR_KEYSET_SIZE, 0},
      {SQL_ATTR_MAX_LENGTH, 0},
      {SQL_ATTR_ROW_ARRAY_SIZE, 1},
      {SQL_ATTR_ROW_BIND_TYPE, SQL_BIND_BY_COLUMN},
      {SQL_ATTR_RETRIEVE_DATA, SQL_RD_ON},
      {SQL_ATTR_USE_BOOKMARKS, SQL_UB_OFF},
      {SQL_ATTR_ASYNC_ENABLE, SQL_ASYNC_ENABLE_OFF},
      {SQL_ATTR_PARAMSET_SIZE, 1},
      {SQL_ATTR_PARAM_BIND_TYPE, SQL_PARAM_BIND_BY_COLUMN},
      {SQL_ATTR_METADATA_ID, SQL_FALSE},
  };
  for (const auto& expectation : expectations) {
    SQLULEN value = 99;
    EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        statement_, expectation.attribute, &value, sizeof(value), nullptr));
    EXPECT_EQ(expectation.value, value);
    EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        statement_, expectation.attribute,
        integer_value(expectation.value), 0));
  }

  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CURSOR_TYPE,
      integer_value(SQL_CURSOR_STATIC), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CURSOR_TYPE, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CONCURRENCY,
      integer_value(SQL_CONCUR_VALUES), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CONCURRENCY, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CURSOR_SCROLLABLE,
      integer_value(SQL_SCROLLABLE), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CURSOR_SCROLLABLE, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CURSOR_SENSITIVITY,
      integer_value(SQL_INSENSITIVE), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CURSOR_SENSITIVITY, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_ARRAY_SIZE, integer_value(2), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_ARRAY_SIZE, integer_value(0), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_PARAMSET_SIZE, integer_value(2), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_PARAMSET_SIZE, integer_value(0), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_METADATA_ID, integer_value(SQL_TRUE), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_RETRIEVE_DATA, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_USE_BOOKMARKS, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_ASYNC_ENABLE, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_METADATA_ID, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_ENABLE_AUTO_IPD, integer_value(SQL_TRUE), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_ENABLE_AUTO_IPD, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_KEYSET_SIZE, integer_value(1), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_MAX_LENGTH, integer_value(1), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(AttributeApisTest, StoresApplicationDescriptorBindOffsetPointers) {
  SQLLEN row_offset = 8;
  SQLLEN parameter_offset = 16;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_BIND_OFFSET_PTR, &row_offset, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_PARAM_BIND_OFFSET_PTR, &parameter_offset, 0));

  SQLLEN* reported_row_offset = nullptr;
  SQLLEN* reported_parameter_offset = nullptr;
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROW_BIND_OFFSET_PTR, &reported_row_offset,
      sizeof(reported_row_offset), nullptr));
  EXPECT_EQ(&row_offset, reported_row_offset);
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_PARAM_BIND_OFFSET_PTR, &reported_parameter_offset,
      sizeof(reported_parameter_offset), nullptr));
  EXPECT_EQ(&parameter_offset, reported_parameter_offset);

  SQLHDESC row_descriptor = SQL_NULL_HDESC;
  SQLHDESC parameter_descriptor = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, &row_descriptor, 0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_APP_PARAM_DESC, &parameter_descriptor, 0, nullptr));
  SQLLEN* descriptor_offset = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
      row_descriptor, 0, SQL_DESC_BIND_OFFSET_PTR, &descriptor_offset, 0,
      nullptr));
  EXPECT_EQ(&row_offset, descriptor_offset);
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
      parameter_descriptor, 0, SQL_DESC_BIND_OFFSET_PTR, &descriptor_offset,
      0, nullptr));
  EXPECT_EQ(&parameter_offset, descriptor_offset);

  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_BIND_OFFSET_PTR, nullptr, 0));
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_PARAM_BIND_OFFSET_PTR, nullptr, 0));
}

TEST_F(AttributeApisTest, StoresBookmarkAndOperationPointers) {
  SQLLEN bookmark = 42;
  SQLUSMALLINT row_operation = SQL_ROW_PROCEED;
  SQLUSMALLINT parameter_operation = SQL_PARAM_PROCEED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_FETCH_BOOKMARK_PTR, &bookmark, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_OPERATION_PTR, &row_operation, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_PARAM_OPERATION_PTR, &parameter_operation, 0));

  SQLLEN* reported_bookmark = nullptr;
  SQLUSMALLINT* reported_row_operation = nullptr;
  SQLUSMALLINT* reported_parameter_operation = nullptr;
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_FETCH_BOOKMARK_PTR, &reported_bookmark,
      sizeof(reported_bookmark), nullptr));
  EXPECT_EQ(&bookmark, reported_bookmark);
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROW_OPERATION_PTR, &reported_row_operation,
      sizeof(reported_row_operation), nullptr));
  EXPECT_EQ(&row_operation, reported_row_operation);
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_PARAM_OPERATION_PTR,
      &reported_parameter_operation, sizeof(reported_parameter_operation),
      nullptr));
  EXPECT_EQ(&parameter_operation, reported_parameter_operation);

  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_FETCH_BOOKMARK_PTR, nullptr, 0));
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_OPERATION_PTR, nullptr, 0));
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_PARAM_OPERATION_PTR, nullptr, 0));
}

TEST_F(AttributeApisTest, ClassifiesRecognizedUnsupportedStatementAttributes) {
  SQLULEN value = 77;
  for (const auto attribute : {SQL_ATTR_NOSCAN, SQL_ATTR_SIMULATE_CURSOR}) {
    EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
        statement_, attribute, integer_value(0), 0));
    EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
    EXPECT_EQ(SQL_ERROR, SQLGetStmtAttr(
        statement_, attribute, &value, sizeof(value), nullptr));
    EXPECT_EQ(77u, value);
    EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  }
}

TEST_F(AttributeApisTest, CurrentRowNumberIsReadOnlyAndRequiresPosition) {
  SQLULEN row_number = 99;
  EXPECT_EQ(SQL_ERROR, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number),
      nullptr));
  EXPECT_EQ(99u, row_number);
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_NUMBER, integer_value(1), 0));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(AttributeApisTest, StoresSingleRowAndParameterStatusPointers) {
  SQLUSMALLINT row_status = 99;
  SQLULEN rows_fetched = 99;
  SQLUSMALLINT param_status = 99;
  SQLULEN params_processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_STATUS_PTR, &row_status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_PARAM_STATUS_PTR, &param_status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_PARAMS_PROCESSED_PTR, &params_processed, 0));

  SQLUSMALLINT* reported_row_status = nullptr;
  SQLULEN* reported_rows_fetched = nullptr;
  SQLUSMALLINT* reported_param_status = nullptr;
  SQLULEN* reported_params_processed = nullptr;
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROW_STATUS_PTR, &reported_row_status,
      sizeof(reported_row_status), nullptr));
  EXPECT_EQ(&row_status, reported_row_status);
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROWS_FETCHED_PTR, &reported_rows_fetched,
      sizeof(reported_rows_fetched), nullptr));
  EXPECT_EQ(&rows_fetched, reported_rows_fetched);
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_PARAM_STATUS_PTR, &reported_param_status,
      sizeof(reported_param_status), nullptr));
  EXPECT_EQ(&param_status, reported_param_status);
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_PARAMS_PROCESSED_PTR, &reported_params_processed,
      sizeof(reported_params_processed), nullptr));
  EXPECT_EQ(&params_processed, reported_params_processed);

  EXPECT_EQ(SQL_ERROR, SQLFetch(statement_));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(99u, rows_fetched);
  EXPECT_EQ(99, row_status);

  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_STATUS_PTR, nullptr, 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROW_STATUS_PTR, &reported_row_status,
      sizeof(reported_row_status), nullptr));
  EXPECT_EQ(nullptr, reported_row_status);
}

TEST_F(AttributeApisTest, StatementPointersShareDescriptorHeaderState) {
  SQLHDESC row_descriptor = SQL_NULL_HDESC;
  SQLHDESC param_descriptor = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_IMP_ROW_DESC, &row_descriptor, 0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_IMP_PARAM_DESC, &param_descriptor, 0, nullptr));

  SQLUSMALLINT row_status = 99;
  SQLULEN rows_fetched = 99;
  SQLUSMALLINT param_status = 99;
  SQLULEN params_processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
      row_descriptor, 0, SQL_DESC_ARRAY_STATUS_PTR, &row_status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
      row_descriptor, 0, SQL_DESC_ROWS_PROCESSED_PTR, &rows_fetched, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
      param_descriptor, 0, SQL_DESC_ARRAY_STATUS_PTR, &param_status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
      param_descriptor, 0, SQL_DESC_ROWS_PROCESSED_PTR,
      &params_processed, 0));

  SQLUSMALLINT* reported_row_status = nullptr;
  SQLULEN* reported_rows_fetched = nullptr;
  SQLUSMALLINT* reported_param_status = nullptr;
  SQLULEN* reported_params_processed = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROW_STATUS_PTR, &reported_row_status, 0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROWS_FETCHED_PTR, &reported_rows_fetched,
      0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_PARAM_STATUS_PTR, &reported_param_status,
      0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_PARAMS_PROCESSED_PTR,
      &reported_params_processed, 0, nullptr));
  EXPECT_EQ(&row_status, reported_row_status);
  EXPECT_EQ(&rows_fetched, reported_rows_fetched);
  EXPECT_EQ(&param_status, reported_param_status);
  EXPECT_EQ(&params_processed, reported_params_processed);

  EXPECT_EQ(SQL_ERROR, SQLFetch(statement_));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(99u, rows_fetched);
  EXPECT_EQ(99, row_status);
}

TEST_F(AttributeApisTest, AttachedDescriptorHeadersDriveStatementAttributes) {
  const auto descriptor = odbcpp::test::make_descriptor(connection_);
  ASSERT_NE(nullptr, descriptor);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_APP_ROW_DESC, descriptor, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
      descriptor, 0, SQL_DESC_ARRAY_SIZE, integer_value(2), 0));

  SQLULEN array_size = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_ROW_ARRAY_SIZE, &array_size, 0, nullptr));
  EXPECT_EQ(2u, array_size);
  EXPECT_EQ(SQL_ERROR, SQLFetch(statement_));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));

  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
      descriptor, 0, SQL_DESC_ARRAY_SIZE, integer_value(1), 0));
  EXPECT_EQ(SQL_ERROR, SQLFetch(statement_));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

TEST_F(AttributeApisTest, StoresMaximumRowsAndFinishesResultSequence) {
  SQLULEN value = 99;
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_MAX_ROWS, &value, sizeof(value), nullptr));
  EXPECT_EQ(0u, value);
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_MAX_ROWS, integer_value(25), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_MAX_ROWS, &value, sizeof(value), nullptr));
  EXPECT_EQ(25u, value);

  EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(statement_));
  EXPECT_EQ(SQL_INVALID_HANDLE, SQLMoreResults(nullptr));
}

TEST_F(AttributeApisTest, SupportsForwardOnlyFetchScroll) {
  EXPECT_EQ(SQL_ERROR, SQLFetchScroll(
      statement_, SQL_FETCH_NEXT, 0));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLFetchScroll(
      statement_, SQL_FETCH_FIRST, 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLFetchScroll(statement_, 999, 0));
  EXPECT_EQ("HY106", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_INVALID_HANDLE, SQLFetchScroll(nullptr, SQL_FETCH_NEXT, 0));
}

TEST_F(AttributeApisTest, ManagesStatementCursorAndBindings) {
  EXPECT_EQ(SQL_ERROR, SQLCloseCursor(statement_));
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_UNBIND));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_RESET_PARAMS));
  EXPECT_EQ(SQL_ERROR, SQLFreeStmt(statement_, 999));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(AttributeApisTest, ReportsUnsupportedAttributesAndNullOutputs) {
  SQLUINTEGER value = 0;
  EXPECT_EQ(SQL_ERROR, SQLGetConnectAttr(
      connection_, -12345, &value, sizeof(value), nullptr));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, -12345, integer_value(1), 0));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR, SQLGetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, nullptr, 0, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(AttributeApisTest, RejectsInvalidHandles) {
  EXPECT_EQ(SQL_INVALID_HANDLE, SQLSetConnectAttr(
      nullptr, SQL_ATTR_LOGIN_TIMEOUT, integer_value(1), 0));
  EXPECT_EQ(SQL_INVALID_HANDLE, SQLSetStmtAttr(
      nullptr, SQL_ATTR_QUERY_TIMEOUT, integer_value(1), 0));
}

TEST_F(AttributeApisTest, DriverConnectValidatesArgumentsBeforeConnecting) {
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(
      connection_, nullptr, nullptr, 0, nullptr, 0, nullptr,
      SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_DBC, connection_));

  SQLCHAR connection_string[] = "SERVER=127.0.0.1";
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(
      connection_, nullptr, connection_string, SQL_NTS, nullptr, 0, nullptr,
      999));
  EXPECT_EQ("HY110", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(
      connection_, nullptr, connection_string, SQL_NTS, nullptr, 0, nullptr,
      SQL_DRIVER_PROMPT));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, NativeSqlValidatesInputAndConnectionState) {
  EXPECT_EQ(SQL_ERROR, SQLNativeSql(
      connection_, nullptr, 0, nullptr, 0, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_DBC, connection_));

  SQLCHAR query[] = "SELECT 1";
  EXPECT_EQ(SQL_ERROR, SQLNativeSql(
      connection_, query, SQL_NTS, nullptr, 0, nullptr));
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST(AttributeDeadlineTest, SaturatesUnlimitedTimeoutWithoutOverflow) {
  EXPECT_EQ(rs::util::Deadline::max(),
            rs::util::make_deadline(std::chrono::milliseconds::max()));
}

} // namespace
