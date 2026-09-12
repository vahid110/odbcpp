#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "tests/test_handle_helpers.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>

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

TEST_F(DiagnosticsTest, SQLGetDiagField_CompleteStatementFieldFamilies) {
    ASSERT_EQ(SQL_ERROR,
              SQLExecDirect(hstmt, reinterpret_cast<SQLCHAR*>(
                                       const_cast<char*>("select 1")),
                            SQL_NTS));

    SQLCHAR dynamic_function[32]{};
    SQLSMALLINT string_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
                              SQL_DIAG_DYNAMIC_FUNCTION, dynamic_function,
                              sizeof(dynamic_function), &string_length));
    EXPECT_STREQ("SELECT CURSOR", reinterpret_cast<char*>(dynamic_function));
    EXPECT_EQ(13, string_length);

    SQLINTEGER dynamic_code = -1;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
                              SQL_DIAG_DYNAMIC_FUNCTION_CODE, &dynamic_code,
                              0, nullptr));
    EXPECT_EQ(SQL_DIAG_SELECT_CURSOR, dynamic_code);

    SQLLEN row_count = -1;
    SQLLEN cursor_row_count = -1;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
                              SQL_DIAG_ROW_COUNT, &row_count, 0, nullptr));
    EXPECT_EQ(0, row_count);
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
                              SQL_DIAG_CURSOR_ROW_COUNT, &cursor_row_count,
                              0, nullptr));
    EXPECT_EQ(0, cursor_row_count);

    SQLLEN row_number = 0;
    SQLINTEGER column_number = 0;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 1,
                              SQL_DIAG_ROW_NUMBER, &row_number, 0, nullptr));
    EXPECT_EQ(SQL_NO_ROW_NUMBER, row_number);
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 1,
                              SQL_DIAG_COLUMN_NUMBER, &column_number, 0,
                              nullptr));
    EXPECT_EQ(SQL_NO_COLUMN_NUMBER, column_number);

    SQLWCHAR wide_dynamic_function[32]{};
    string_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagFieldW(SQL_HANDLE_STMT, hstmt, 0,
                               SQL_DIAG_DYNAMIC_FUNCTION,
                               wide_dynamic_function,
                               sizeof(wide_dynamic_function),
                               &string_length));
    EXPECT_EQ(13 * static_cast<SQLSMALLINT>(sizeof(SQLWCHAR)), string_length);
    EXPECT_EQ(static_cast<SQLWCHAR>('S'), wide_dynamic_function[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('R'), wide_dynamic_function[12]);
    EXPECT_EQ(0, wide_dynamic_function[13]);
}

TEST_F(DiagnosticsTest, StatementOnlyFieldsRejectOtherHandleTypes) {
    SQLLEN value = 0;
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 0,
                              SQL_DIAG_ROW_COUNT, &value, 0, nullptr));
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 0,
                              SQL_DIAG_CURSOR_ROW_COUNT, &value, 0,
                              nullptr));

    ASSERT_EQ(SQL_ERROR,
              SQLDriverConnect(
                  hdbc, nullptr,
                  reinterpret_cast<SQLCHAR*>(const_cast<char*>("DSN=x")),
                  SQL_NTS, nullptr, 0, nullptr,
                  static_cast<SQLUSMALLINT>(999)));
    SQLCHAR origin[16]{};
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 1,
                              SQL_DIAG_CLASS_ORIGIN, origin,
                              sizeof(origin), nullptr));
    EXPECT_STREQ("ISO 9075", reinterpret_cast<char*>(origin));
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagField(SQL_HANDLE_DBC, hdbc, 1,
                              SQL_DIAG_SUBCLASS_ORIGIN, origin,
                              sizeof(origin), nullptr));
    EXPECT_STREQ("ODBC 3.0", reinterpret_cast<char*>(origin));
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
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 1,
                              SQL_DIAG_MESSAGE_TEXT, nullptr, -1, nullptr));

    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, nullptr,
                            nullptr, 0, nullptr));
    EXPECT_STREQ("08001", reinterpret_cast<const char*>(state));
}

TEST_F(DiagnosticsTest, SQLGetDiagRec_ExactAnsiAndWideBufferBoundaries) {
    ASSERT_EQ(SQL_ERROR, SQLExecDirect(hstmt, nullptr, SQL_NTS));
    constexpr char expected[] = "SQL statement is null";
    constexpr auto expected_length =
        static_cast<SQLSMALLINT>(sizeof(expected) - 1);

    SQLCHAR state[6]{};
    SQLINTEGER native_error = -1;
    SQLSMALLINT required = -1;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state,
                            &native_error, nullptr, 0, &required));
    EXPECT_STREQ("HY009", reinterpret_cast<const char*>(state));
    EXPECT_EQ(0, native_error);
    EXPECT_EQ(expected_length, required);

    std::array<SQLCHAR, 1> terminator_only{0x7f};
    required = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                            terminator_only.data(),
                            static_cast<SQLSMALLINT>(terminator_only.size()),
                            &required));
    EXPECT_EQ(0, terminator_only[0]);
    EXPECT_EQ(expected_length, required);

    std::array<SQLCHAR, sizeof(expected) - 1> short_message{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                            short_message.data(),
                            static_cast<SQLSMALLINT>(short_message.size()),
                            &required));
    EXPECT_EQ(0, short_message.back());
    EXPECT_EQ(0, std::memcmp(short_message.data(), expected,
                             short_message.size() - 1));

    std::array<SQLCHAR, sizeof(expected)> exact_message{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                            exact_message.data(),
                            static_cast<SQLSMALLINT>(exact_message.size()),
                            &required));
    EXPECT_STREQ(expected,
                 reinterpret_cast<const char*>(exact_message.data()));

    std::array<SQLWCHAR, 1> wide_terminator_only{
        static_cast<SQLWCHAR>(0x7f)};
    required = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRecW(
                  SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                  wide_terminator_only.data(),
                  static_cast<SQLSMALLINT>(wide_terminator_only.size()),
                  &required));
    EXPECT_EQ(0, wide_terminator_only[0]);
    EXPECT_EQ(expected_length, required);

    std::array<SQLWCHAR, sizeof(expected) - 1> wide_short_message{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRecW(
                  SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                  wide_short_message.data(),
                  static_cast<SQLSMALLINT>(wide_short_message.size()),
                  &required));
    EXPECT_EQ(0, wide_short_message.back());
    for (std::size_t index = 0;
         index + 1 < wide_short_message.size(); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(expected[index]),
                  wide_short_message[index]);
    }

    std::array<SQLWCHAR, sizeof(expected)> wide_exact_message{};
    SQLWCHAR wide_state[6]{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRecW(
                  SQL_HANDLE_STMT, hstmt, 1, wide_state, nullptr,
                  wide_exact_message.data(),
                  static_cast<SQLSMALLINT>(wide_exact_message.size()),
                  &required));
    constexpr char expected_state[] = "HY009";
    for (std::size_t index = 0; index < sizeof(expected_state) - 1; ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(expected_state[index]),
                  wide_state[index]);
    }
    EXPECT_EQ(0, wide_state[5]);
    EXPECT_EQ(expected_length, required);
    for (std::size_t index = 0; index < sizeof(expected); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(expected[index]),
                  wide_exact_message[index]);
    }

    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                            nullptr, 0, nullptr));
}

TEST_F(DiagnosticsTest, SQLGetDiagField_TruncatesAnsiAndWideStrings) {
    ASSERT_EQ(SQL_ERROR,
              SQLExecDirect(hstmt, reinterpret_cast<SQLCHAR*>(
                                       const_cast<char*>("SELECT 1")),
                            SQL_NTS));

    SQLCHAR message[5]{};
    SQLSMALLINT required = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 1,
                              SQL_DIAG_MESSAGE_TEXT, message,
                              sizeof(message), &required));
    EXPECT_GT(required, static_cast<SQLSMALLINT>(sizeof(message) - 1));
    EXPECT_EQ(0, message[sizeof(message) - 1]);

    SQLWCHAR wide_message[5]{};
    required = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagFieldW(SQL_HANDLE_STMT, hstmt, 1,
                               SQL_DIAG_MESSAGE_TEXT, wide_message,
                               sizeof(wide_message), &required));
    EXPECT_GT(required,
              static_cast<SQLSMALLINT>((std::size(wide_message) - 1) *
                                       sizeof(SQLWCHAR)));
    EXPECT_EQ(0, wide_message[std::size(wide_message) - 1]);
}
