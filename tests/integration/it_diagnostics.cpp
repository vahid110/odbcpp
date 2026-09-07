#include <gtest/gtest.h>
#include "odbc/odbc_api.h"

class DiagnosticsIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
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
    EXPECT_STREQ((char*)sqlstate, "HY000"); // General error
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
    EXPECT_STREQ((char*)sqlstate, "HY000"); // General error
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
    EXPECT_STREQ((char*)sqlstate, "HY000"); // General error
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
