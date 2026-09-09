#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "tests/test_handle_helpers.h"
#include <cstdint>
#include <cstring>

class DiagnosticsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv));
        ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0));
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc));
        hstmt = odbcpp::test::make_statement(hdbc);
        ASSERT_NE(nullptr, hstmt);
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

TEST_F(DiagnosticsTest, SQLGetDiagRec_MultipleRecords) {
    // Force an error to generate diagnostic records
    SQLRETURN ret = SQLConnect(hdbc, (SQLCHAR*)"invalid_dsn", SQL_NTS, nullptr, 0, nullptr, 0);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test SQLGetDiagRec for first record
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLCHAR message[256];
    SQLSMALLINT text_length;
    
    ret = SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 1, sqlstate, &native_error, message, sizeof(message), &text_length);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("08001", (char*)sqlstate);  // Connection failure
    EXPECT_GT(text_length, 0);
    
    // Test no second record exists
    ret = SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 2, sqlstate, &native_error, message, sizeof(message), &text_length);
    EXPECT_EQ(SQL_NO_DATA, ret);
}

TEST_F(DiagnosticsTest, SQLGetDiagField_HeaderFields) {
    // Force an error
    SQLConnect(hdbc, (SQLCHAR*)"invalid_dsn", SQL_NTS, nullptr, 0, nullptr, 0);
    
    // Test SQL_DIAG_NUMBER (number of diagnostic records)
    SQLINTEGER diag_count;
    SQLRETURN ret = SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 0, SQL_DIAG_NUMBER, &diag_count, 0, nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ(1, diag_count);
    
    // Test SQL_DIAG_RETURNCODE
    SQLRETURN return_code;
    ret = SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 0, SQL_DIAG_RETURNCODE, &return_code, 0, nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ(SQL_ERROR, return_code);
}

TEST_F(DiagnosticsTest, SQLDiagReturnCodeTracksTheGeneratingCall) {
    EXPECT_EQ(SQL_ERROR,
              SQLExecDirect(hstmt, nullptr, SQL_NTS));

    SQLRETURN return_code = SQL_SUCCESS;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
                              SQL_DIAG_RETURNCODE, &return_code, 0, nullptr));
    EXPECT_EQ(SQL_ERROR, return_code);

    ASSERT_EQ(SQL_SUCCESS,
              SQLSetStmtAttr(hstmt, SQL_ATTR_MAX_ROWS,
                             reinterpret_cast<SQLPOINTER>(
                                 static_cast<std::uintptr_t>(1)),
                             0));
    return_code = SQL_ERROR;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
                              SQL_DIAG_RETURNCODE, &return_code, 0, nullptr));
    EXPECT_EQ(SQL_SUCCESS, return_code);
}

TEST_F(DiagnosticsTest, SQLGetDiagField_RecordFields) {
    // Force an error
    SQLConnect(hdbc, (SQLCHAR*)"invalid_dsn", SQL_NTS, nullptr, 0, nullptr, 0);
    
    // Test SQL_DIAG_SQLSTATE
    SQLCHAR sqlstate[6];
    SQLSMALLINT string_length;
    SQLRETURN ret = SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 1, SQL_DIAG_SQLSTATE, sqlstate, sizeof(sqlstate), &string_length);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("08001", (char*)sqlstate);
    EXPECT_EQ(5, string_length);
    
    // Test SQL_DIAG_NATIVE
    SQLINTEGER native_error;
    ret = SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 1, SQL_DIAG_NATIVE, &native_error, 0, nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    
    // Test SQL_DIAG_MESSAGE_TEXT
    SQLCHAR message[256];
    ret = SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 1, SQL_DIAG_MESSAGE_TEXT, message, sizeof(message), &string_length);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_GT(string_length, 0);
}

TEST_F(DiagnosticsTest, SQLError_ODBC2Compatibility) {
    // Force an error
    SQLConnect(hdbc, (SQLCHAR*)"invalid_dsn", SQL_NTS, nullptr, 0, nullptr, 0);
    
    // Test SQLError (ODBC 2.x compatibility)
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLCHAR message[256];
    SQLSMALLINT text_length;
    
    SQLRETURN ret = SQLError(henv, hdbc, nullptr, sqlstate, &native_error, message, sizeof(message), &text_length);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("08001", (char*)sqlstate);
    
    // SQLError should clear diagnostics after retrieval
    ret = SQLError(henv, hdbc, nullptr, sqlstate, &native_error, message, sizeof(message), &text_length);
    EXPECT_EQ(SQL_NO_DATA, ret);
}

TEST_F(DiagnosticsTest, SQLGetDiagRec_InvalidParameters) {
    // Test invalid record number
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLCHAR message[256];
    SQLSMALLINT text_length;
    
    SQLRETURN ret = SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 0, sqlstate, &native_error, message, sizeof(message), &text_length);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test invalid handle
    ret = SQLGetDiagRec(SQL_HANDLE_DBC, nullptr, 1, sqlstate, &native_error, message, sizeof(message), &text_length);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
}

TEST_F(DiagnosticsTest, DiagnosticRetrievalErrorsPreserveExistingRecords) {
    ASSERT_EQ(SQL_ERROR,
              SQLExecDirect(hstmt, reinterpret_cast<SQLCHAR*>(
                                       const_cast<char*>("SELECT 1")),
                            SQL_NTS));

    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 0, state, nullptr,
                            nullptr, 0, nullptr));
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0, -1, nullptr, 0,
                              nullptr));
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, nullptr,
                            nullptr, 0, nullptr));
    EXPECT_STREQ("08001", reinterpret_cast<const char*>(state));

    SQLWCHAR wide_state[6]{};
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 0, wide_state, nullptr,
                             nullptr, 0, nullptr));
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagFieldW(SQL_HANDLE_STMT, hstmt, 0, -1, nullptr, 0,
                               nullptr));
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, wide_state, nullptr,
                             nullptr, 0, nullptr));
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide_state[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('8'), wide_state[1]);
}

TEST_F(DiagnosticsTest, DiagnosticFunctionsValidateTypeRecordAndBuffer) {
    ASSERT_EQ(SQL_ERROR,
              SQLExecDirect(hstmt, reinterpret_cast<SQLCHAR*>(
                                       const_cast<char*>("SELECT 1")),
                            SQL_NTS));

    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_INVALID_HANDLE,
              SQLGetDiagRec(SQL_HANDLE_DBC, hstmt, 1, state, nullptr,
                            nullptr, 0, nullptr));
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, nullptr,
                            nullptr, -1, nullptr));
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, -1,
                              SQL_DIAG_MESSAGE_TEXT, nullptr, 0, nullptr));

    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, nullptr,
                            nullptr, 0, nullptr));
    EXPECT_STREQ("08001", reinterpret_cast<const char*>(state));
}

TEST_F(DiagnosticsTest, SQLGetDiagRec_MessageTruncation) {
    // Force an error with a long message
    SQLConnect(hdbc, (SQLCHAR*)"invalid_dsn_with_very_long_name_that_should_cause_truncation", SQL_NTS, nullptr, 0, nullptr, 0);
    
    // Test with small buffer to trigger truncation
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLCHAR message[10];  // Very small buffer
    SQLSMALLINT text_length;
    
    SQLRETURN ret = SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 1, sqlstate, &native_error, message, sizeof(message), &text_length);
    
    // Should return SQL_SUCCESS_WITH_INFO for truncation
    if (text_length >= static_cast<SQLSMALLINT>(sizeof(message))) {
        EXPECT_EQ(SQL_SUCCESS_WITH_INFO, ret);
    } else {
        EXPECT_EQ(SQL_SUCCESS, ret);
    }
    
    EXPECT_GT(text_length, 0);
    EXPECT_EQ('\0', message[sizeof(message) - 1]);  // Null terminated
}
