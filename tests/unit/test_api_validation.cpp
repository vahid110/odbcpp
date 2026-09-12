#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "odbc/testing_hooks.h"
#include "tests/test_handle_helpers.h"

#include <chrono>
#include <cstdint>
#include <future>
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

void set_odbc3(SQLHENV environment) {
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
}

class ApiValidationTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
    set_odbc3(environment_);
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

TEST(ApiAllocationValidationTest, ReportsInjectedAllocationFailure) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  set_odbc3(environment);

  SQLHDBC connection = reinterpret_cast<SQLHDBC>(std::uintptr_t{1});
  rs::odbc::testing::fail_next_handle_allocation();
  EXPECT_EQ(SQL_ERROR,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  EXPECT_EQ(SQL_NULL_HDBC, connection);
  EXPECT_EQ("HY001", diagnostic_state(SQL_HANDLE_ENV, environment));

  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
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

TEST(ApiHandleLifetimeTest, EnvironmentCannotBeFreedBeforeItsConnection) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  set_odbc3(environment);
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));

  EXPECT_EQ(SQL_ERROR, SQLFreeHandle(SQL_HANDLE_ENV, environment));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_ENV, environment));
  EXPECT_NE(nullptr,
            rs::odbc::HandleRegistry::instance().get_handle(connection));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiHandleLifetimeTest, ConnectionCannotBeFreedBeforeItsStatement) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  SQLHSTMT statement = SQL_NULL_HSTMT;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  set_odbc3(environment);
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  statement = odbcpp::test::make_statement(connection);
  ASSERT_NE(nullptr, statement);

  EXPECT_EQ(SQL_ERROR, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS,
            SQLSetStmtAttr(statement, SQL_ATTR_MAX_ROWS,
                           reinterpret_cast<SQLPOINTER>(std::uintptr_t{1}),
                           0));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiHandleLifetimeTest, FreeingStatementInvalidatesImplicitDescriptors) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  SQLHSTMT statement = SQL_NULL_HSTMT;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  set_odbc3(environment);
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  statement = odbcpp::test::make_statement(connection);
  ASSERT_NE(nullptr, statement);

  SQLHDESC descriptor = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetStmtAttr(statement, SQL_ATTR_APP_ROW_DESC, &descriptor, 0,
                           nullptr));
  ASSERT_NE(nullptr, descriptor);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));

  SQLSMALLINT count = 0;
  EXPECT_EQ(SQL_INVALID_HANDLE,
            SQLGetDescField(descriptor, 0, SQL_DESC_COUNT, &count, 0,
                            nullptr));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiHandleLifetimeTest, OppositeDescriptorCopiesDoNotDeadlock) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  set_odbc3(environment);
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  const auto first = odbcpp::test::make_descriptor(connection);
  const auto second = odbcpp::test::make_descriptor(connection);
  ASSERT_NE(nullptr, first);
  ASSERT_NE(nullptr, second);

  std::atomic<int> failures{0};
  std::thread forward([&] {
    for (int iteration = 0; iteration < 500; ++iteration) {
      if (SQLCopyDesc(first, second) != SQL_SUCCESS) ++failures;
    }
  });
  std::thread reverse([&] {
    for (int iteration = 0; iteration < 500; ++iteration) {
      if (SQLCopyDesc(second, first) != SQL_SUCCESS) ++failures;
    }
  });
  forward.join();
  reverse.join();

  EXPECT_EQ(0, failures.load());
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, first));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, second));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiHandleLifetimeTest, ParentFreeWaitsForInFlightChildOperation) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  set_odbc3(environment);
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  const auto statement = odbcpp::test::make_statement(connection);
  ASSERT_NE(nullptr, statement);

  std::promise<void> free_started;
  auto started = free_started.get_future();
  std::future<SQLRETURN> free_result;
  {
    auto child_operation =
        rs::odbc::HandleRegistry::instance().lock_handles({statement});
    free_result = std::async(std::launch::async, [&] {
      free_started.set_value();
      return SQLFreeHandle(SQL_HANDLE_DBC, connection);
    });
    started.wait();
    EXPECT_EQ(std::future_status::timeout,
              free_result.wait_for(std::chrono::milliseconds(50)));
  }

  ASSERT_EQ(std::future_status::ready,
            free_result.wait_for(std::chrono::seconds(2)));
  EXPECT_EQ(SQL_ERROR, free_result.get());
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiHandleLifetimeTest, DisconnectRejectsConnectionThatWasNeverOpened) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  set_odbc3(environment);
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));

  EXPECT_EQ(SQL_ERROR, SQLDisconnect(connection));
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiHandleStateTest, RejectsStatementAndDescriptorBeforeConnect) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  set_odbc3(environment);
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));

  SQLHSTMT statement = reinterpret_cast<SQLHSTMT>(std::uintptr_t{1});
  EXPECT_EQ(SQL_ERROR,
            SQLAllocHandle(SQL_HANDLE_STMT, connection, &statement));
  EXPECT_EQ(nullptr, statement);
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection));

  SQLHDESC descriptor = reinterpret_cast<SQLHDESC>(std::uintptr_t{1});
  EXPECT_EQ(SQL_ERROR,
            SQLAllocHandle(SQL_HANDLE_DESC, connection, &descriptor));
  EXPECT_EQ(nullptr, descriptor);
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ApiHandleStateTest, ConnectionRequiresEnvironmentVersion) {
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));

  SQLHDBC connection = reinterpret_cast<SQLHDBC>(std::uintptr_t{1});
  EXPECT_EQ(SQL_ERROR,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  EXPECT_EQ(nullptr, connection);
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_ENV, environment));

  set_odbc3(environment);
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  EXPECT_EQ(SQL_ERROR,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_ENV, environment));

  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
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

TEST_F(ApiValidationTest, SuccessfulCallClearsPreviousDiagnostics) {
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, nullptr, SQL_NTS));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_SUCCESS,
            SQLSetStmtAttr(statement_, SQL_ATTR_MAX_ROWS,
                           reinterpret_cast<SQLPOINTER>(std::uintptr_t{10}),
                           0));
  EXPECT_EQ(SQL_NO_DATA,
            SQLGetDiagRec(SQL_HANDLE_STMT, statement_, 1, nullptr, nullptr,
                          nullptr, 0, nullptr));
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

  SQLWCHAR wide_dsn[]{'e', 'x', 'a', 'm', 'p', 'l', 'e', 0};
  EXPECT_EQ(SQL_ERROR,
            SQLConnectW(connection_, wide_dsn, -2, nullptr, 0, nullptr, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(ApiValidationTest, AnsiAndWideOutputLengthsHaveMatchingValidation) {
  SQLCHAR narrow[16]{};
  SQLWCHAR wide[16]{};

  EXPECT_EQ(SQL_ERROR,
            SQLDescribeCol(statement_, 1, narrow, -1, nullptr, nullptr,
                           nullptr, nullptr, nullptr));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR,
            SQLDescribeColW(statement_, 1, wide, -1, nullptr, nullptr,
                            nullptr, nullptr, nullptr));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR,
            SQLColAttribute(statement_, 1, SQL_DESC_NAME, narrow, -1,
                            nullptr, nullptr));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR,
            SQLColAttributeW(statement_, 1, SQL_DESC_NAME, wide, -1,
                             nullptr, nullptr));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR,
            SQLGetInfo(connection_, SQL_ODBC_VER, narrow, -1, nullptr));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR,
            SQLGetInfoW(connection_, SQL_ODBC_VER, wide, -1, nullptr));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_DBC, connection_));

  SQLUSMALLINT transaction_capability = 123;
  EXPECT_EQ(SQL_ERROR,
            SQLGetInfo(connection_, SQL_TXN_CAPABLE,
                       &transaction_capability, -1, nullptr));
  EXPECT_EQ(123, transaction_capability);
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR,
            SQLGetInfoW(connection_, SQL_TXN_CAPABLE,
                        &transaction_capability, -1, nullptr));
  EXPECT_EQ(123, transaction_capability);
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(ApiValidationTest, CatalogApisShareAnsiAndWideLengthValidation) {
  auto table = reinterpret_cast<SQLCHAR*>(const_cast<char*>("table"));
  EXPECT_EQ(SQL_ERROR,
            SQLTables(statement_, nullptr, 0, nullptr, 0, table, -2,
                      nullptr, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLWCHAR wide_table[]{'t', 'a', 'b', 'l', 'e', 0};
  EXPECT_EQ(SQL_ERROR,
            SQLTablesW(statement_, nullptr, 0, nullptr, 0, wide_table, -2,
                       nullptr, 0));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(ApiValidationTest, UsesSpecificDiagnosticsForInvalidStatementInputs) {
  SQLSMALLINT parameter_count = 91;
  EXPECT_EQ(SQL_ERROR, SQLNumParams(statement_, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLNumParams(statement_, &parameter_count));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(91, parameter_count);

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

  SQLSMALLINT data_type = 71;
  SQLULEN parameter_size = 72;
  SQLSMALLINT decimal_digits = 73;
  SQLSMALLINT nullable = 74;
  EXPECT_EQ(SQL_ERROR,
            SQLDescribeParam(statement_, 0, &data_type, nullptr, nullptr,
                             nullptr));
  EXPECT_EQ("07009", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR,
            SQLDescribeParam(statement_, 1, &data_type, &parameter_size,
                             &decimal_digits, &nullable));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(71, data_type);
  EXPECT_EQ(72u, parameter_size);
  EXPECT_EQ(73, decimal_digits);
  EXPECT_EQ(74, nullable);

  SQLLEN column_attribute = 81;
  EXPECT_EQ(SQL_ERROR,
            SQLColAttribute(statement_, 1, SQL_DESC_TYPE, nullptr, 0,
                            nullptr, &column_attribute));
  EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(81, column_attribute);
  EXPECT_EQ(SQL_ERROR,
            SQLColAttribute(statement_, 1, 9999, nullptr, 0, nullptr,
                            &column_attribute));
  EXPECT_EQ("HY091", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR,
            SQLColAttribute(statement_, 1, SQL_DESC_BASE_TABLE_NAME,
                            buffer, sizeof(buffer), nullptr, nullptr));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLWCHAR wide_buffer[8]{};
  EXPECT_EQ(SQL_ERROR,
            SQLColAttribute(statement_, 1, SQL_DESC_BASE_TABLE_NAME,
                            buffer, -1, nullptr, nullptr));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR,
            SQLColAttributeW(statement_, 1, SQL_DESC_BASE_TABLE_NAME,
                             wide_buffer, -1, nullptr, nullptr));
  EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

} // namespace
