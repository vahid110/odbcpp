#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "tests/test_connection_config.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::string diagnostic_state(SQLSMALLINT handle_type, SQLHANDLE handle) {
  SQLCHAR state[6]{};
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(handle_type, handle, 1, state, nullptr, nullptr, 0,
                          nullptr));
  return reinterpret_cast<const char*>(state);
}

SQLCHAR* test_dsn() {
  return odbcpp::test::configured_connection_string_data();
}

std::vector<SQLWCHAR> wide_ascii(const std::string& value) {
  std::vector<SQLWCHAR> result;
  result.reserve(value.size() + 1);
  for (const unsigned char character : value) {
    result.push_back(static_cast<SQLWCHAR>(character));
  }
  result.push_back(0);
  return result;
}

class HandleLifecycleIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    const auto result = SQLConnect(connection_, test_dsn(), SQL_NTS,
                                   nullptr, 0, nullptr, 0);
    if (result != SQL_SUCCESS) {
      GTEST_SKIP() << "Database connection failed";
    }
  }

  void TearDown() override {
    if (statement_) SQLFreeHandle(SQL_HANDLE_STMT, statement_);
    if (connection_) {
      SQLDisconnect(connection_);
      SQLFreeHandle(SQL_HANDLE_DBC, connection_);
    }
    if (environment_) SQLFreeHandle(SQL_HANDLE_ENV, environment_);
  }

  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  SQLHSTMT statement_{SQL_NULL_HSTMT};
};

TEST_F(HandleLifecycleIntegrationTest,
       ConnectedConnectionCannotBeFreedOrConnectedAgain) {
  EXPECT_EQ(SQL_ERROR, SQLFreeHandle(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_ERROR,
            SQLConnect(connection_, test_dsn(), SQL_NTS,
                       nullptr, 0, nullptr, 0));
  EXPECT_EQ("08002", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(HandleLifecycleIntegrationTest,
       SqlConnectCredentialsOverrideDsnAndClassifyAuthenticationFailures) {
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));

  auto missing_user = reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("odbcpp_missing_login_role"));
  auto wrong_password =
      reinterpret_cast<SQLCHAR*>(const_cast<char*>("wrong-password"));
  EXPECT_EQ(SQL_ERROR,
            SQLConnect(connection_, test_dsn(), SQL_NTS,
                       missing_user, SQL_NTS, wrong_password, SQL_NTS));
  EXPECT_EQ("28000", diagnostic_state(SQL_HANDLE_DBC, connection_));

  ASSERT_EQ(SQL_SUCCESS,
            SQLConnect(connection_, test_dsn(), SQL_NTS,
                       nullptr, 0, nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));

  auto wide_dsn =
      wide_ascii(odbcpp::test::configured_connection_string());
  auto wide_user = wide_ascii("odbcpp_missing_wide_login_role");
  auto wide_password = wide_ascii("wrong-password");
  EXPECT_EQ(SQL_ERROR,
            SQLConnectW(connection_,
                        wide_dsn.data(), SQL_NTS,
                        wide_user.data(), SQL_NTS,
                        wide_password.data(), SQL_NTS));
  EXPECT_EQ("28000", diagnostic_state(SQL_HANDLE_DBC, connection_));

  ASSERT_EQ(SQL_SUCCESS,
            SQLConnectW(connection_,
                        wide_dsn.data(), SQL_NTS,
                        nullptr, 0, nullptr, 0));
}

TEST_F(HandleLifecycleIntegrationTest, CursorCloseStateMatrix) {
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  EXPECT_EQ(SQL_ERROR, SQLCloseCursor(statement_));
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));

  SQLCHAR prepared[] = "SELECT 1 WHERE FALSE";
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(statement_, prepared, SQL_NTS));
  EXPECT_EQ(SQL_ERROR, SQLCloseCursor(statement_));
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));

  ASSERT_EQ(SQL_SUCCESS, SQLExecute(statement_));
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLCloseCursor(statement_));
  EXPECT_EQ(SQL_ERROR, SQLCloseCursor(statement_));
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLCHAR update[] = "SET application_name TO 'odbcpp_cursor_test'";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, update, SQL_NTS));
  EXPECT_EQ(SQL_ERROR, SQLCloseCursor(statement_));
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));

  SQLCHAR invalid[] = "SELECT FROM";
  ASSERT_EQ(SQL_ERROR, SQLExecDirect(statement_, invalid, SQL_NTS));
  EXPECT_EQ(SQL_ERROR, SQLCloseCursor(statement_));
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
}

TEST_F(HandleLifecycleIntegrationTest,
       SuccessfulDisconnectInvalidatesStatementsAndDescriptors) {
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  SQLHDESC descriptor = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DESC, connection_, &descriptor));

  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));
  EXPECT_EQ(SQL_INVALID_HANDLE,
            SQLFreeHandle(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_INVALID_HANDLE,
            SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
  statement_ = SQL_NULL_HSTMT;
}

TEST_F(HandleLifecycleIntegrationTest,
       ActiveTransactionMustBeCompletedBeforeDisconnect) {
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(
                connection_, SQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<SQLPOINTER>(
                    static_cast<std::uintptr_t>(SQL_AUTOCOMMIT_OFF)),
                0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  ASSERT_EQ(SQL_SUCCESS,
            SQLExecDirect(statement_,
                          reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                              "SELECT 1")),
                          SQL_NTS));

  EXPECT_EQ(SQL_ERROR, SQLDisconnect(connection_));
  EXPECT_EQ("25000", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_SUCCESS,
            SQLEndTran(SQL_HANDLE_DBC, connection_, SQL_ROLLBACK));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement_));
  statement_ = SQL_NULL_HSTMT;
  EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));
}

TEST(ConnectionAttributeIntegrationTest,
     CurrentCatalogAndConnectionDeadTrackConnectionState) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));

  auto catalog = reinterpret_cast<SQLCHAR*>(const_cast<char*>("postgres"));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(connection, SQL_ATTR_CURRENT_CATALOG,
                              catalog, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS,
            SQLConnect(connection, test_dsn(), SQL_NTS,
                       nullptr, 0, nullptr, 0));

  SQLUINTEGER dead = SQL_CD_TRUE;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_DEAD, &dead,
                              sizeof(dead), nullptr));
  EXPECT_EQ(SQL_CD_FALSE, dead);
  SQLCHAR reported_catalog[16]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection, SQL_ATTR_CURRENT_CATALOG,
                              reported_catalog, sizeof(reported_catalog),
                              nullptr));
  EXPECT_STREQ("postgres", reinterpret_cast<char*>(reported_catalog));

  struct StringInfo {
    SQLUSMALLINT type;
    const char* expected;
  };
  const auto* dsn_name = reinterpret_cast<const char*>(test_dsn()) + 4;
  const StringInfo string_info[] = {
      {SQL_DATA_SOURCE_NAME, dsn_name},
      {SQL_DATABASE_NAME, "postgres"},
      {SQL_USER_NAME, "postgres"},
  };
  const auto verify_wide_info = [&](SQLUSMALLINT type,
                                    std::string_view expected) {
    SQLWCHAR value[128]{};
    SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetInfoW(connection, type, value, sizeof(value), &length));
    EXPECT_EQ(static_cast<SQLSMALLINT>(expected.size() * sizeof(SQLWCHAR)),
              length);
    for (std::size_t index = 0; index < expected.size(); ++index) {
      EXPECT_EQ(static_cast<SQLWCHAR>(expected[index]), value[index]);
    }
    EXPECT_EQ(static_cast<SQLWCHAR>(0), value[expected.size()]);
  };
  for (const auto& info : string_info) {
    SQLCHAR value[64]{};
    SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
        connection, info.type, value, sizeof(value), &length));
    EXPECT_STREQ(info.expected, reinterpret_cast<const char*>(value));
    EXPECT_EQ(std::strlen(info.expected),
              static_cast<std::size_t>(length));
    verify_wide_info(info.type, info.expected);
  }
  SQLCHAR server_name[128]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection, SQL_SERVER_NAME, server_name, sizeof(server_name), nullptr));
  EXPECT_NE('\0', server_name[0]);
  verify_wide_info(
      SQL_SERVER_NAME, reinterpret_cast<const char*>(server_name));
  SQLCHAR dbms_version[32]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection, SQL_DBMS_VER, dbms_version, sizeof(dbms_version), nullptr));
  EXPECT_EQ('.', dbms_version[2]);
  EXPECT_EQ('.', dbms_version[5]);
  EXPECT_EQ(10u, std::strlen(reinterpret_cast<const char*>(dbms_version)));
  verify_wide_info(
      SQL_DBMS_VER, reinterpret_cast<const char*>(dbms_version));

  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(
                connection, SQL_ATTR_CURRENT_CATALOG,
                reinterpret_cast<SQLCHAR*>(const_cast<char*>("other")),
                SQL_NTS));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection));

  ASSERT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(connection, SQL_ATTR_CONNECTION_TIMEOUT,
                              reinterpret_cast<SQLPOINTER>(5), 0));
  SQLUINTEGER connection_timeout = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_TIMEOUT,
                              &connection_timeout,
                              sizeof(connection_timeout), nullptr));
  EXPECT_EQ(5u, connection_timeout);
  EXPECT_EQ(SQL_ERROR,
            SQLSetConnectAttr(connection, SQL_ATTR_PACKET_SIZE,
                              reinterpret_cast<SQLPOINTER>(8192), 0));
  EXPECT_EQ("HY011", diagnostic_state(SQL_HANDLE_DBC, connection));

  EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
  dead = SQL_CD_FALSE;
  EXPECT_EQ(SQL_ERROR,
            SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_DEAD, &dead,
                              sizeof(dead), nullptr));
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST_F(HandleLifecycleIntegrationTest,
       FailedServerTripMarksConnectionDeadWithoutAnotherRoundTrip) {
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetConnectAttr(
                connection_, SQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<SQLPOINTER>(
                    static_cast<std::uintptr_t>(SQL_AUTOCOMMIT_OFF)),
                0));

  EXPECT_EQ(SQL_ERROR,
            SQLExecDirect(
                statement_,
                reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                    "SELECT pg_terminate_backend(pg_backend_pid())")),
                SQL_NTS));
  EXPECT_EQ("08S01", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLUINTEGER dead = SQL_CD_FALSE;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection_, SQL_ATTR_CONNECTION_DEAD, &dead,
                              sizeof(dead), nullptr));
  EXPECT_EQ(SQL_CD_TRUE, dead);
  EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));
}

TEST(DriverConnectIntegrationTest,
     NoninteractiveCompletionModesReturnTheExactInputString) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));

  constexpr std::array<SQLUSMALLINT, 3> completion_modes{
      SQL_DRIVER_NOPROMPT, SQL_DRIVER_COMPLETE,
      SQL_DRIVER_COMPLETE_REQUIRED};
  for (const auto completion : completion_modes) {
    SQLHDBC connection = SQL_NULL_HDBC;
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
    const auto* input = reinterpret_cast<const char*>(test_dsn());
    SQLCHAR output[256]{};
    SQLSMALLINT output_length = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLDriverConnect(
                  connection, nullptr, test_dsn(), SQL_NTS, output,
                  static_cast<SQLSMALLINT>(sizeof(output)), &output_length,
                  completion));
    EXPECT_STREQ(input, reinterpret_cast<const char*>(output));
    EXPECT_EQ(std::strlen(input), static_cast<std::size_t>(output_length));
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  }

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(DriverConnectIntegrationTest,
     TruncationAndConnectedStatePreserveRequiredOutputs) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));

  SQLCHAR output[4]{};
  SQLSMALLINT output_length = -1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLDriverConnect(connection, nullptr, test_dsn(), SQL_NTS,
                             output, static_cast<SQLSMALLINT>(sizeof(output)),
                             &output_length,
                             SQL_DRIVER_NOPROMPT));
  EXPECT_STREQ("DSN", reinterpret_cast<const char*>(output));
  EXPECT_EQ(std::strlen(reinterpret_cast<const char*>(test_dsn())),
            static_cast<std::size_t>(output_length));
  EXPECT_EQ("01004", diagnostic_state(SQL_HANDLE_DBC, connection));

  SQLCHAR untouched[]{'k', 'e', 'e', 'p', 0};
  output_length = 77;
  EXPECT_EQ(SQL_ERROR,
            SQLDriverConnect(connection, nullptr, test_dsn(), SQL_NTS,
                             untouched,
                             static_cast<SQLSMALLINT>(sizeof(untouched)),
                             &output_length,
                             SQL_DRIVER_NOPROMPT));
  EXPECT_STREQ("keep", reinterpret_cast<const char*>(untouched));
  EXPECT_EQ(77, output_length);
  EXPECT_EQ("08002", diagnostic_state(SQL_HANDLE_DBC, connection));

  EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(DriverConnectIntegrationTest,
     UnknownKeywordsWarnAfterConnectingForAnsiAndWideCalls) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));

  const std::string input = reinterpret_cast<const char*>(test_dsn()) +
      std::string(";UnknownOption=ignored");
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLDriverConnect(
                connection, nullptr,
                reinterpret_cast<SQLCHAR*>(const_cast<char*>(input.c_str())),
                SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("01S00", diagnostic_state(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));

  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  auto wide_input = wide_ascii(input);
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLDriverConnectW(connection, nullptr, wide_input.data(), SQL_NTS,
                              nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("01S00", diagnostic_state(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(EnvironmentTransactionIntegrationTest,
     CommitAndRollbackApplyToEveryConnectedChild) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC first_connection = SQL_NULL_HDBC;
  SQLHDBC second_connection = SQL_NULL_HDBC;
  SQLHSTMT first_statement = SQL_NULL_HSTMT;
  SQLHSTMT second_statement = SQL_NULL_HSTMT;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &first_connection));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &second_connection));
  ASSERT_EQ(SQL_SUCCESS,
            SQLConnect(first_connection, test_dsn(), SQL_NTS,
                       nullptr, 0, nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLConnect(second_connection, test_dsn(), SQL_NTS,
                       nullptr, 0, nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_STMT, first_connection,
                           &first_statement));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_STMT, second_connection,
                           &second_statement));
  for (const auto connection : {first_connection, second_connection}) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetConnectAttr(
                  connection, SQL_ATTR_AUTOCOMMIT,
                  reinterpret_cast<SQLPOINTER>(
                      static_cast<std::uintptr_t>(SQL_AUTOCOMMIT_OFF)),
                  0));
  }
  for (const auto statement : {first_statement, second_statement}) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(
                  statement,
                  reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                      "CREATE TEMP TABLE odbcpp_env_txn(value integer)")),
                  SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(
                  statement,
                  reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                      "INSERT INTO odbcpp_env_txn VALUES (1)")),
                  SQL_NTS));
  }

  ASSERT_EQ(SQL_SUCCESS,
            SQLEndTran(SQL_HANDLE_ENV, environment, SQL_ROLLBACK));
  for (const auto statement : {first_statement, second_statement}) {
    EXPECT_EQ(SQL_ERROR,
              SQLExecDirect(
                  statement,
                  reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                      "SELECT count(*) FROM odbcpp_env_txn")),
                  SQL_NTS));
    EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, statement));
  }
  ASSERT_EQ(SQL_SUCCESS,
            SQLEndTran(SQL_HANDLE_ENV, environment, SQL_ROLLBACK));

  for (const auto statement : {first_statement, second_statement}) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(
                  statement,
                  reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                      "CREATE TEMP TABLE odbcpp_env_txn(value integer)")),
                  SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(
                  statement,
                  reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                      "INSERT INTO odbcpp_env_txn VALUES (2)")),
                  SQL_NTS));
  }
  ASSERT_EQ(SQL_SUCCESS,
            SQLEndTran(SQL_HANDLE_ENV, environment, SQL_COMMIT));
  for (const auto statement : {first_statement, second_statement}) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(
                  statement,
                  reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                      "SELECT count(*) FROM odbcpp_env_txn")),
                  SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement));
    SQLINTEGER count = 0;
    SQLLEN indicator = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(statement, 1, SQL_C_SLONG, &count, sizeof(count),
                         &indicator));
    EXPECT_EQ(1, count);
  }

  ASSERT_EQ(SQL_SUCCESS,
            SQLEndTran(SQL_HANDLE_ENV, environment, SQL_ROLLBACK));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, first_statement));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, second_statement));
  EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(first_connection));
  EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(second_connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, first_connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, second_connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

}  // namespace
