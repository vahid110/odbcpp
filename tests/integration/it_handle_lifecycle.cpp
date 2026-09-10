#include <gtest/gtest.h>

#include "odbc/odbc_api.h"

#include <cstdint>
#include <string>

namespace {

std::string diagnostic_state(SQLSMALLINT handle_type, SQLHANDLE handle) {
  SQLCHAR state[6]{};
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(handle_type, handle, 1, state, nullptr, nullptr, 0,
                          nullptr));
  return reinterpret_cast<const char*>(state);
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
    const auto result = SQLConnect(
        connection_, reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                         "DSN=RedshiftProd")),
        SQL_NTS, nullptr, 0, nullptr, 0);
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
            SQLConnect(connection_,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                           "DSN=RedshiftProd")),
                       SQL_NTS, nullptr, 0, nullptr, 0));
  EXPECT_EQ("08002", diagnostic_state(SQL_HANDLE_DBC, connection_));
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
            SQLConnect(connection,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                           "DSN=RedshiftProd")),
                       SQL_NTS, nullptr, 0, nullptr, 0));

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

}  // namespace
