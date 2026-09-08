#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "core/util/deadline.h"

#include <cstdint>

namespace {

class AttributeApisTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
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
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_ENV, environment_, SQL_COMMIT));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_ENV, environment_));
}

TEST_F(AttributeApisTest, ReportsTransactionCapabilities) {
  SQLUSMALLINT small_value = 0;
  SQLSMALLINT length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_TXN_CAPABLE, &small_value, sizeof(small_value), &length));
  EXPECT_EQ(SQL_TC_ALL, small_value);
  EXPECT_EQ(sizeof(SQLUSMALLINT), static_cast<std::size_t>(length));

  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_CURSOR_COMMIT_BEHAVIOR, &small_value,
      sizeof(small_value), nullptr));
  EXPECT_EQ(SQL_CB_PRESERVE, small_value);
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_CURSOR_ROLLBACK_BEHAVIOR, &small_value,
      sizeof(small_value), nullptr));
  EXPECT_EQ(SQL_CB_PRESERVE, small_value);

  SQLUINTEGER integer_value = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_DEFAULT_TXN_ISOLATION, &integer_value,
      sizeof(integer_value), &length));
  EXPECT_EQ(SQL_TXN_READ_COMMITTED, integer_value);
  EXPECT_EQ(sizeof(SQLUINTEGER), static_cast<std::size_t>(length));
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_TXN_ISOLATION_OPTION, &integer_value,
      sizeof(integer_value), nullptr));
  EXPECT_EQ(SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED |
                SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE,
            integer_value);
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

TEST(AttributeDeadlineTest, SaturatesUnlimitedTimeoutWithoutOverflow) {
  EXPECT_EQ(rs::util::Deadline::max(),
            rs::util::make_deadline(std::chrono::milliseconds::max()));
}

} // namespace
