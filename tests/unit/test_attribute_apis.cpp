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
  SQLULEN value = 0;
  SQLINTEGER length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, &value, sizeof(value), &length));
  EXPECT_EQ(30u, value);
  EXPECT_EQ(sizeof(SQLULEN), static_cast<std::size_t>(length));

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, integer_value(7), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, &value, sizeof(value), nullptr));
  EXPECT_EQ(7u, value);
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
  SQLULEN value = 0;
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

TEST(AttributeDeadlineTest, SaturatesUnlimitedTimeoutWithoutOverflow) {
  EXPECT_EQ(rs::util::Deadline::max(),
            rs::util::make_deadline(std::chrono::milliseconds::max()));
}

} // namespace
