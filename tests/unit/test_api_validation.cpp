#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"

#include <cstdint>
#include <string>
#include <thread>

namespace {

std::string diagnostic_state(SQLSMALLINT handle_type, SQLHANDLE handle) {
  SQLCHAR state[6]{};
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(handle_type, handle, 1, state, nullptr, nullptr, 0,
                          nullptr));
  return reinterpret_cast<const char*>(state);
}

class ApiValidationTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
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

  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  SQLHSTMT statement_{SQL_NULL_HSTMT};
};

TEST(ApiAllocationValidationTest, RejectsNullOutputPointer) {
  EXPECT_EQ(SQL_ERROR,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, nullptr));

  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  EXPECT_EQ(SQL_ERROR,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_ENV, environment));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiAllocationValidationTest, RejectsInvalidHandleType) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));

  SQLHANDLE output = reinterpret_cast<SQLHANDLE>(std::uintptr_t{1});
  EXPECT_EQ(SQL_ERROR, SQLAllocHandle(999, environment, &output));
  EXPECT_EQ(SQL_NULL_HANDLE, output);
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_ENV, environment));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiAllocationValidationTest, LookupPinsHandleAcrossConcurrentFree) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));

  auto pinned = rs::odbc::HandleRegistry::instance().get_handle_as<
      rs::odbc::ODBCEnvironment>(environment);
  ASSERT_NE(nullptr, pinned);

  SQLRETURN free_result = SQL_ERROR;
  std::thread freer([&] {
    free_result = SQLFreeHandle(SQL_HANDLE_ENV, environment);
  });
  freer.join();

  ASSERT_EQ(SQL_SUCCESS, free_result);
  EXPECT_EQ(nullptr,
            rs::odbc::HandleRegistry::instance().get_handle(environment));
  EXPECT_EQ(rs::odbc::HandleType::Environment, pinned->get_type());
}

TEST_F(ApiValidationTest, RejectsInvalidAnsiStatementText) {
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, nullptr, SQL_NTS));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, statement_));

  auto statement = reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 1"));
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, statement, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, statement, -2));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR, SQLPrepare(statement_, nullptr, SQL_NTS));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLPrepare(statement_, statement, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLPrepare(statement_, statement, -2));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(ApiValidationTest, RejectsInvalidWideStatementText) {
  SQLWCHAR statement[]{'S', 'E', 'L', 'E', 'C', 'T', ' ', '1', 0};

  EXPECT_EQ(SQL_ERROR, SQLExecDirectW(statement_, statement, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLExecDirectW(statement_, statement, -2));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR, SQLPrepareW(statement_, statement, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLPrepareW(statement_, statement, -2));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(ApiValidationTest, RejectsInvalidConnectionStringLengths) {
  auto dsn = reinterpret_cast<SQLCHAR*>(const_cast<char*>("example"));
  EXPECT_EQ(SQL_ERROR,
            SQLConnect(connection_, dsn, -2, nullptr, 0, nullptr, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_ERROR,
            SQLConnect(connection_, dsn, SQL_NTS, nullptr, -2, nullptr, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_ERROR,
            SQLConnect(connection_, dsn, SQL_NTS, nullptr, 0, nullptr, -2));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(ApiValidationTest, UsesSpecificDiagnosticsForInvalidStatementInputs) {
  SQLSMALLINT column_count = 0;
  EXPECT_EQ(SQL_ERROR, SQLNumResultCols(statement_, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLNumResultCols(statement_, &column_count));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLLEN row_count = 0;
  EXPECT_EQ(SQL_ERROR, SQLRowCount(statement_, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLRowCount(statement_, &row_count));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLCHAR buffer[8]{};
  SQLLEN indicator = 0;
  EXPECT_EQ(SQL_ERROR,
            SQLGetData(statement_, 1, SQL_C_CHAR, buffer, sizeof(buffer),
                       &indicator));
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR,
            SQLBindCol(statement_, 0, SQL_C_CHAR, buffer, sizeof(buffer),
                       &indicator));
  EXPECT_EQ("07009", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR,
            SQLBindParameter(statement_, 0, SQL_PARAM_INPUT, SQL_C_CHAR,
                             SQL_VARCHAR, 8, 0, buffer, sizeof(buffer),
                             &indicator));
  EXPECT_EQ("07009", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLSMALLINT data_type = 0;
  EXPECT_EQ(SQL_ERROR,
            SQLDescribeParam(statement_, 0, &data_type, nullptr, nullptr,
                             nullptr));
  EXPECT_EQ("07009", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

} // namespace
