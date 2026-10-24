#include <gtest/gtest.h>
#include "tests/test_connection_config.h"
#include <cstdint>
#include <algorithm>
#include <chrono>
#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include "odbc/odbc_api.h"
#include "tests/test_handle_helpers.h"
#endif


namespace {
// One original per-case window, including reconnects and sibling handles.
// Apply caps only on I/O entry, leaving allocation defaults and deliberate shorter
// timeout values intact. Never restore attributes after a call: its diagnostics belong to the test.
constexpr SQLULEN fixture_timeout_seconds(std::int64_t left, SQLULEN requested = 0) {
  if (left <= 0) return 0;
  const auto cap = static_cast<SQLULEN>(std::min<std::int64_t>(left, 5));
  return requested == 0 ? cap : std::min(cap, requested);
}
static_assert(fixture_timeout_seconds(40) == 5);
static_assert(fixture_timeout_seconds(4) == 4);
static_assert(fixture_timeout_seconds(1) == 1);
static_assert(fixture_timeout_seconds(0) == 0);
static_assert(fixture_timeout_seconds(-1) == 0);
static_assert(fixture_timeout_seconds(40, 1) == 1);
static_assert(fixture_timeout_seconds(40, 2) == 2);
static_assert(fixture_timeout_seconds(40, 3) == 3);

SQLULEN fixture_seconds_left(std::chrono::steady_clock::time_point end) {
  const auto seconds = fixture_timeout_seconds(
      std::chrono::duration_cast<std::chrono::seconds>(
          end - std::chrono::steady_clock::now()).count());
  if (seconds == 0) {
    ADD_FAILURE() << "Original forty-second fixture window expired; no new I/O";
  }
  return seconds;
}

bool fixture_statement_budget(std::chrono::steady_clock::time_point end, SQLHSTMT statement) {
  const auto left = fixture_seconds_left(end);
  if (left == 0) return false;
  SQLULEN requested = 0;
  if (SQLGetStmtAttr(statement, SQL_ATTR_QUERY_TIMEOUT, &requested, 0, nullptr) != SQL_SUCCESS) {
    // Preserve the original API's invalid-handle/local-validation assertion.
    return true;
  }
  const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), requested);
  if (seconds == requested) return true;
  if (SQLSetStmtAttr(statement, SQL_ATTR_QUERY_TIMEOUT,
                     reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
    ADD_FAILURE() << "Could not apply finite fixture statement timeout";
    return false;
  }
  return true;
}

bool fixture_connection_budget(std::chrono::steady_clock::time_point end, SQLHDBC connection, bool login) {
  const auto left = fixture_seconds_left(end);
  if (left == 0) return false;
  SQLUINTEGER current = 0;
  if (login) {
    SQLUINTEGER dead = SQL_CD_TRUE;
    if (SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_DEAD, &dead, sizeof(dead), nullptr) == SQL_SUCCESS && dead == SQL_CD_FALSE) {
      // An already-connected connect call is a local 08002 test, not new I/O.
      return true;
    }
    if (SQLGetConnectAttr(connection, SQL_ATTR_LOGIN_TIMEOUT, &current, sizeof(current), nullptr) != SQL_SUCCESS) return true;
    const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), current);
    if (SQLSetConnectAttr(connection, SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
      ADD_FAILURE() << "Could not apply finite fixture login timeout";
      return false;
    }
  }
  if (SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_TIMEOUT, &current, sizeof(current), nullptr) != SQL_SUCCESS) return true;
  const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), current);
  if (SQLSetConnectAttr(connection, SQL_ATTR_CONNECTION_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
    ADD_FAILURE() << "Could not apply finite fixture connection timeout";
    return false;
  }
  return true;
}
}  // namespace


#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
// A statement on a never-connected connection exists only through the test
// registry helper: SQLAllocHandle refuses it, and a Driver Manager raises its
// own 08003 first. These cases stay on the direct layer.
class DiagnosticsIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        hstmt = odbcpp::test::make_statement(hdbc);
    }
    
    void TearDown() override {
        if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        if (hdbc) SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        if (henv) SQLFreeHandle(SQL_HANDLE_ENV, henv);
    }
    
    SQLHENV henv = nullptr;
    SQLHDBC hdbc = nullptr;
    SQLHSTMT hstmt = nullptr;
};

TEST_F(DiagnosticsIntegrationTest, ConnectionFailureDiagnostics) {
    // Try to connect with malformed connection string (should fail at parsing level)
    SQLRETURN ret = SQLConnect(hdbc, (SQLCHAR*)"DSN=NonExistentDSN", SQL_NTS, nullptr, 0, nullptr, 0);
    EXPECT_EQ(ret, SQL_ERROR);
    
    // Check diagnostic record
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLCHAR message[512];
    SQLSMALLINT text_length;
    
    ret = SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 1, sqlstate, &native_error, 
                       message, sizeof(message), &text_length);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_STREQ((char*)sqlstate, "08001"); // Connection failure
    EXPECT_GT(text_length, 0);
    // Message should contain DSN or connection error details
    EXPECT_GT(strlen((char*)message), 0);
}

TEST_F(DiagnosticsIntegrationTest, StatementExecutionWithoutConnection) {
    // Try to execute without connection
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS);
    EXPECT_EQ(ret, SQL_ERROR);
    
    // Check diagnostic record
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLCHAR message[512];
    SQLSMALLINT text_length;
    
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate, &native_error, 
                       message, sizeof(message), &text_length);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_STREQ((char*)sqlstate, "08001"); // Connection failure (not established)
    EXPECT_GT(text_length, 0);
}

TEST_F(DiagnosticsIntegrationTest, InvalidParameterNumber) {
    // Try to bind invalid parameter
    SQLINTEGER param_value = 123;
    SQLRETURN ret = SQLBindParameter(hstmt, 0, SQL_PARAM_INPUT, SQL_C_SLONG, 
                                    SQL_INTEGER, 0, 0, &param_value, 0, nullptr);
    EXPECT_EQ(ret, SQL_ERROR);
    
    // Check diagnostic record
    SQLCHAR sqlstate[6];
    SQLCHAR message[512];
    
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, 
                       message, sizeof(message), nullptr);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_STREQ((char*)sqlstate, "07009"); // Invalid descriptor index
    EXPECT_TRUE(strstr((char*)message, "parameter number") != nullptr);
}

TEST_F(DiagnosticsIntegrationTest, InvalidColumnNumber) {
    // Try to bind invalid column
    char buffer[256];
    SQLLEN indicator;
    SQLRETURN ret = SQLBindCol(hstmt, 0, SQL_C_CHAR, buffer, sizeof(buffer), &indicator);
    EXPECT_EQ(ret, SQL_ERROR);
    
    // Check diagnostic record
    SQLCHAR sqlstate[6];
    SQLCHAR message[512];
    
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, 
                       message, sizeof(message), nullptr);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_STREQ((char*)sqlstate, "07009"); // Invalid descriptor index
    EXPECT_TRUE(strstr((char*)message, "column number") != nullptr);
}

TEST_F(DiagnosticsIntegrationTest, GetDataWithoutExecution) {
    // Try to get data without executing query
    char buffer[256];
    SQLLEN indicator;
    SQLRETURN ret = SQLGetData(hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator);
    EXPECT_EQ(ret, SQL_ERROR);
    
    // Check diagnostic record
    SQLCHAR sqlstate[6];
    SQLCHAR message[512];
    
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, 
                       message, sizeof(message), nullptr);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_STREQ((char*)sqlstate, "24000"); // Invalid cursor state
    EXPECT_TRUE(strstr((char*)message, "No current row") != nullptr);
}

TEST_F(DiagnosticsIntegrationTest, InvalidDiagnosticRecordNumber) {
    // First create an error
    SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS); // Will fail - no connection
    
    // Try to get invalid record number
    SQLCHAR sqlstate[6];
    SQLCHAR message[512];
    
    SQLRETURN ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 0, sqlstate, nullptr, 
                                 message, sizeof(message), nullptr);
    EXPECT_EQ(ret, SQL_ERROR);
    
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 99, sqlstate, nullptr, 
                       message, sizeof(message), nullptr);
    EXPECT_EQ(ret, SQL_NO_DATA);
}

TEST_F(DiagnosticsIntegrationTest, SQLGetDiagFieldHeaderFields) {
    // First create an error
    SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS); // Will fail - no connection
    
    // Get number of diagnostic records
    SQLINTEGER record_count;
    SQLRETURN ret = SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_NUMBER, 
                                   &record_count, 0, nullptr);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_EQ(record_count, 1);
    
    // Get return code
    SQLRETURN return_code;
    ret = SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_RETURNCODE, 
                         &return_code, 0, nullptr);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_EQ(return_code, SQL_ERROR);
}

TEST_F(DiagnosticsIntegrationTest, SQLGetDiagFieldRecordFields) {
    // First create an error
    SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS); // Will fail - no connection
    
    // Get SQLSTATE
    char sqlstate[6];
    SQLSMALLINT length;
    SQLRETURN ret = SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_SQLSTATE, 
                                   sqlstate, sizeof(sqlstate), &length);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_EQ(length, 5);
    EXPECT_STREQ(sqlstate, "08001");
    
    // Get message text
    char message[512];
    ret = SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT, 
                         message, sizeof(message), &length);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_GT(length, 0);
    EXPECT_TRUE(strstr(message, "Connection") != nullptr);
}
#endif

class DiagnosticsConnectedIntegrationTest : public ::testing::Test {
 protected:
    const std::chrono::steady_clock::time_point fixture_end_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(40);

    // Fixture-local forwards cover repetitions, reconnects and sibling handles.
    // The original assertions/arguments below still call the actual public API.
    SQLRETURN SQLConnect(SQLHDBC connection, SQLCHAR* server_name, SQLSMALLINT name_length1,
        SQLCHAR* user_name, SQLSMALLINT name_length2, SQLCHAR* authentication,
        SQLSMALLINT name_length3) {
      return fixture_connection_budget(fixture_end_, connection, true)
          ? ::SQLConnect(connection, server_name, name_length1, user_name, name_length2,
              authentication, name_length3) : SQL_ERROR;
    }
    SQLRETURN SQLExecDirect(SQLHSTMT statement, SQLCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecDirect(statement, statement_text, text_length) : SQL_ERROR;
    }

};

TEST_F(DiagnosticsConnectedIntegrationTest, StatementHeaderReflectsExecution) {
    SQLHENV environment = SQL_NULL_HENV;
    SQLHDBC connection = SQL_NULL_HDBC;
    SQLHSTMT statement = SQL_NULL_HSTMT;
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
    ASSERT_EQ(SQL_SUCCESS,
              SQLConnect(connection,
                         odbcpp::test::configured_server_name_data(),
                         SQL_NTS, nullptr, 0, nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_STMT, connection, &statement));

    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(statement,
                            reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                                "CREATE TEMP TABLE odbcpp_diag_matrix "
                                "(value integer)")),
                            SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(statement,
                            reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                                "INSERT INTO odbcpp_diag_matrix VALUES "
                                "(1), (2), (3)")),
                            SQL_NTS));

    SQLLEN row_count = -1;
    SQLINTEGER dynamic_code = -1;
    SQLCHAR dynamic_function[32]{};
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, statement, 0,
                              SQL_DIAG_ROW_COUNT, &row_count, 0, nullptr));
    EXPECT_EQ(3, row_count);
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, statement, 0,
                              SQL_DIAG_DYNAMIC_FUNCTION_CODE, &dynamic_code,
                              0, nullptr));
    EXPECT_EQ(SQL_DIAG_INSERT, dynamic_code);
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, statement, 0,
                              SQL_DIAG_DYNAMIC_FUNCTION, dynamic_function,
                              sizeof(dynamic_function), nullptr));
    EXPECT_STREQ("INSERT", reinterpret_cast<char*>(dynamic_function));

    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(statement,
                            reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                                "SELECT value FROM odbcpp_diag_matrix "
                                "ORDER BY value")),
                            SQL_NTS));
    SQLLEN cursor_row_count = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, statement, 0,
                              SQL_DIAG_CURSOR_ROW_COUNT, &cursor_row_count,
                              0, nullptr));
    EXPECT_EQ(3, cursor_row_count);

    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
TEST_F(DiagnosticsIntegrationTest, SQLErrorCompatibility) {
    // First create an error
    SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS); // Will fail - no connection
    
    // Use ODBC 2.x SQLError function
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLCHAR message[512];
    SQLSMALLINT text_length;
    
    SQLRETURN ret = SQLError(henv, hdbc, hstmt, sqlstate, &native_error, 
                            message, sizeof(message), &text_length);
    EXPECT_EQ(ret, SQL_SUCCESS);
    EXPECT_STREQ((char*)sqlstate, "08001");
    EXPECT_GT(text_length, 0);
    
    // SQLError should clear diagnostics after retrieving
    ret = SQLError(henv, hdbc, hstmt, sqlstate, &native_error, 
                  message, sizeof(message), &text_length);
    EXPECT_EQ(ret, SQL_NO_DATA);
}
#endif
