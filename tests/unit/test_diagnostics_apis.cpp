#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "tests/test_handle_helpers.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>

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

TEST_F(DiagnosticsTest, NumericFieldsHandleUnalignedOutput) {
    ASSERT_EQ(SQL_ERROR, SQLExecDirect(hstmt, nullptr, SQL_NTS));
    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> storage{};
    void* output = storage.data() + 1;

    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagField(
        SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_NUMBER, output, 0, nullptr));
    SQLINTEGER count = 0;
    std::memcpy(&count, output, sizeof(count));
    EXPECT_EQ(1, count);

    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_RETURNCODE,
        output, 0, nullptr));
    SQLRETURN return_code = SQL_SUCCESS;
    std::memcpy(&return_code, output, sizeof(return_code));
    EXPECT_EQ(SQL_ERROR, return_code);

    SQLLEN expected_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagField(
        SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_CURSOR_ROW_COUNT,
        &expected_count, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagField(
        SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_CURSOR_ROW_COUNT,
        output, 0, nullptr));
    SQLLEN actual_count = -2;
    std::memcpy(&actual_count, output, sizeof(actual_count));
    EXPECT_EQ(expected_count, actual_count);

    SQLINTEGER expected_native = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagField(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_NATIVE,
        &expected_native, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_NATIVE,
        output, 0, nullptr));
    SQLINTEGER actual_native = -2;
    std::memcpy(&actual_native, output, sizeof(actual_native));
    EXPECT_EQ(expected_native, actual_native);

    SQLLEN expected_row = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagField(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_ROW_NUMBER,
        &expected_row, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_ROW_NUMBER,
        output, 0, nullptr));
    SQLLEN actual_row = 0;
    std::memcpy(&actual_row, output, sizeof(actual_row));
    EXPECT_EQ(expected_row, actual_row);

    const auto previous_output = storage;
    EXPECT_EQ(SQL_NO_DATA, SQLGetDiagField(
        SQL_HANDLE_STMT, hstmt, 2, SQL_DIAG_NATIVE, output, 0, nullptr));
    EXPECT_EQ(previous_output, storage);
}

TEST_F(DiagnosticsTest, StringFieldLengthHandlesUnalignedOutput) {
    ASSERT_EQ(SQL_ERROR, SQLExecDirect(hstmt, nullptr, SQL_NTS));
    alignas(SQLSMALLINT) std::array<std::byte, 1 + sizeof(SQLSMALLINT)>
        length_storage{};
    auto* length = reinterpret_cast<SQLSMALLINT*>(length_storage.data() + 1);

    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagField(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT,
        nullptr, 0, length));
    SQLSMALLINT required = -1;
    std::memcpy(&required, reinterpret_cast<const std::byte*>(length),
                sizeof(required));
    EXPECT_EQ(21, required);
}

TEST_F(DiagnosticsTest, DiagnosticRecordHandlesUnalignedOutputs) {
    ASSERT_EQ(SQL_ERROR, SQLExecDirect(hstmt, nullptr, SQL_NTS));

    alignas(SQLINTEGER) std::array<std::byte, 1 + sizeof(SQLINTEGER)>
        native_storage{};
    alignas(SQLSMALLINT) std::array<std::byte, 1 + sizeof(SQLSMALLINT)>
        length_storage{};
    auto* native = reinterpret_cast<SQLINTEGER*>(native_storage.data() + 1);
    auto* length = reinterpret_cast<SQLSMALLINT*>(length_storage.data() + 1);

    SQLCHAR state[6]{};
    SQLCHAR message[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, native, message,
        static_cast<SQLSMALLINT>(sizeof(message)), length));
    EXPECT_STREQ("HY009", reinterpret_cast<const char*>(state));
    EXPECT_STREQ("SQL statement is null",
                reinterpret_cast<const char*>(message));
    SQLINTEGER native_value = -1;
    SQLSMALLINT length_value = -1;
    std::memcpy(&native_value, reinterpret_cast<const std::byte*>(native),
                sizeof(native_value));
    std::memcpy(&length_value, reinterpret_cast<const std::byte*>(length),
                sizeof(length_value));
    EXPECT_EQ(0, native_value);
    EXPECT_EQ(21, length_value);

    alignas(SQLWCHAR) std::array<std::byte, 1 + 6 * sizeof(SQLWCHAR)>
        wide_state_storage{};
    alignas(SQLWCHAR) std::array<std::byte, 1 + 32 * sizeof(SQLWCHAR)>
        wide_message_storage{};
    auto* wide_state = reinterpret_cast<SQLWCHAR*>(
        wide_state_storage.data() + 1);
    auto* wide_message = reinterpret_cast<SQLWCHAR*>(
        wide_message_storage.data() + 1);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRecW(
        SQL_HANDLE_STMT, hstmt, 1, wide_state, native, wide_message,
        32, length));
    std::array<SQLWCHAR, 6> decoded_state{};
    std::array<SQLWCHAR, 22> decoded_message{};
    std::memcpy(decoded_state.data(),
                reinterpret_cast<const std::byte*>(wide_state),
                sizeof(decoded_state));
    std::memcpy(decoded_message.data(),
                reinterpret_cast<const std::byte*>(wide_message),
                sizeof(decoded_message));
    constexpr char expected_state[] = "HY009";
    constexpr char expected_message[] = "SQL statement is null";
    for (std::size_t index = 0; index < sizeof(expected_state); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(expected_state[index]),
                  decoded_state[index]);
    }
    for (std::size_t index = 0; index < sizeof(expected_message); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(expected_message[index]),
                  decoded_message[index]);
    }
    std::memcpy(&length_value, reinterpret_cast<const std::byte*>(length),
                sizeof(length_value));
    EXPECT_EQ(21, length_value);

    wide_message_storage.fill(std::byte{0x7f});
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT,
        wide_message, static_cast<SQLSMALLINT>(32 * sizeof(SQLWCHAR)),
        length));
    std::memcpy(decoded_message.data(),
                reinterpret_cast<const std::byte*>(wide_message),
                sizeof(decoded_message));
    for (std::size_t index = 0; index < sizeof(expected_message); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(expected_message[index]),
                  decoded_message[index]);
    }
    std::memcpy(&length_value, reinterpret_cast<const std::byte*>(length),
                sizeof(length_value));
    EXPECT_EQ(21 * sizeof(SQLWCHAR), length_value);

    const auto previous_native = native_storage;
    const auto previous_length = length_storage;
    EXPECT_EQ(SQL_NO_DATA, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 2, nullptr, native, nullptr, 0, length));
    EXPECT_EQ(previous_native, native_storage);
    EXPECT_EQ(previous_length, length_storage);
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
    auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCConnection>(hdbc);
    ASSERT_NE(nullptr, connection);
    connection->set_error("01000", "first diagnostic", 11);
    connection->add_diagnostic("22003", 22, "second diagnostic");

    SQLCHAR state[6]{};
    SQLINTEGER native_error = 0;
    SQLCHAR message[32]{};
    SQLSMALLINT text_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLError(henv, hdbc, nullptr, state, &native_error, message,
                       sizeof(message), &text_length));
    EXPECT_STREQ("01000", reinterpret_cast<const char*>(state));
    EXPECT_EQ(11, native_error);
    EXPECT_STREQ("first diagnostic",
                 reinterpret_cast<const char*>(message));
    EXPECT_EQ(16, text_length);

    SQLWCHAR wide_state[6]{};
    SQLWCHAR wide_message[32]{};
    ASSERT_EQ(SQL_SUCCESS,
              SQLErrorW(henv, hdbc, nullptr, wide_state, &native_error,
                        wide_message,
                        static_cast<SQLSMALLINT>(std::size(wide_message)),
                        &text_length));
    EXPECT_EQ(static_cast<SQLWCHAR>('2'), wide_state[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('2'), wide_state[1]);
    EXPECT_EQ(static_cast<SQLWCHAR>('3'), wide_state[4]);
    EXPECT_EQ(22, native_error);
    EXPECT_EQ(17, text_length);
    EXPECT_EQ(static_cast<SQLWCHAR>('s'), wide_message[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('c'), wide_message[16]);
    EXPECT_EQ(0, wide_message[17]);
    EXPECT_EQ(SQL_NO_DATA,
              SQLError(henv, hdbc, nullptr, state, nullptr, nullptr, 0,
                       nullptr));

    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 1, state, nullptr,
                            message, sizeof(message), nullptr));
    EXPECT_STREQ("01000", reinterpret_cast<const char*>(state));
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 2, state, nullptr,
                            message, sizeof(message), nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<const char*>(state));

    connection->set_error("HY000", "replacement diagnostic", 33);
    ASSERT_EQ(SQL_SUCCESS,
              SQLError(henv, hdbc, nullptr, state, &native_error, message,
                       sizeof(message), nullptr));
    EXPECT_STREQ("HY000", reinterpret_cast<const char*>(state));
    EXPECT_EQ(33, native_error);
}

TEST_F(DiagnosticsTest, SQLErrorSelectsHandlesAndAdvancesOnlyOnSuccess) {
    auto environment = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCEnvironment>(henv);
    auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCConnection>(hdbc);
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, environment);
    ASSERT_NE(nullptr, connection);
    ASSERT_NE(nullptr, statement);
    environment->set_error("01000", "environment");
    connection->set_error("08001", "connection");
    statement->set_error("22003", "statement");

    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLError(henv, hdbc, hstmt, state, nullptr, nullptr, 0,
                       nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<const char*>(state));
    EXPECT_EQ(SQL_SUCCESS,
              SQLError(henv, hdbc, nullptr, state, nullptr, nullptr, 0,
                       nullptr));
    EXPECT_STREQ("08001", reinterpret_cast<const char*>(state));
    EXPECT_EQ(SQL_SUCCESS,
              SQLError(henv, nullptr, nullptr, state, nullptr, nullptr, 0,
                       nullptr));
    EXPECT_STREQ("01000", reinterpret_cast<const char*>(state));

    statement->set_error("HY000", "retry after invalid buffer");
    EXPECT_EQ(SQL_ERROR,
              SQLError(henv, hdbc, hstmt, state, nullptr, nullptr, -1,
                       nullptr));
    EXPECT_EQ(SQL_SUCCESS,
              SQLError(henv, hdbc, hstmt, state, nullptr, nullptr, 0,
                       nullptr));
    EXPECT_STREQ("HY000", reinterpret_cast<const char*>(state));

    statement->set_error("01000", "truncated legacy diagnostic");
    SQLCHAR terminator = 0x7f;
    SQLSMALLINT required = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLError(henv, hdbc, hstmt, state, nullptr, &terminator, 1,
                       &required));
    EXPECT_EQ(0, terminator);
    EXPECT_EQ(27, required);
    EXPECT_EQ(SQL_NO_DATA,
              SQLError(henv, hdbc, hstmt, state, nullptr, nullptr, 0,
                       nullptr));

    EXPECT_EQ(SQL_INVALID_HANDLE,
              SQLError(henv, hdbc,
                       reinterpret_cast<SQLHSTMT>(std::uintptr_t{1}),
                       state, nullptr, nullptr, 0, nullptr));
    EXPECT_EQ(SQL_INVALID_HANDLE,
              SQLError(nullptr, nullptr, nullptr, state, nullptr, nullptr,
                       0, nullptr));
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

TEST_F(DiagnosticsTest, SQLGetDiagRec_ZeroCapacityPreservesAnsiRecordAndCanaries) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("HY000", "alpha", 17);
    statement->add_diagnostic("01000", 18, "omega");

    std::array<SQLCHAR, 3> message{0xa5, 0xa5, 0xa5};
    SQLCHAR state[6]{};
    SQLINTEGER native_error = -1;
    SQLSMALLINT required = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, &native_error,
                            message.data() + 1, 0, &required));
    EXPECT_EQ((std::array<SQLCHAR, 3>{0xa5, 0xa5, 0xa5}), message);
    EXPECT_STREQ("HY000", reinterpret_cast<const char*>(state));
    EXPECT_EQ(17, native_error);
    EXPECT_EQ(5, required);
    EXPECT_EQ(2u, statement->get_diagnostic_count());

    required = -1;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, &native_error,
                            nullptr, 0, &required));
    EXPECT_EQ(5, required);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                            message.data() + 1, 0, nullptr));
    EXPECT_EQ((std::array<SQLCHAR, 3>{0xa5, 0xa5, 0xa5}), message);

    std::array<SQLCHAR, 6> retry{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, &native_error,
                            retry.data(), 6, &required));
    EXPECT_STREQ("alpha", reinterpret_cast<const char*>(retry.data()));
    EXPECT_STREQ("HY000", reinterpret_cast<const char*>(state));
    EXPECT_EQ(17, native_error);
    EXPECT_EQ(5, required);
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 2, state, &native_error,
                            retry.data(), 6, &required));
    EXPECT_STREQ("omega", reinterpret_cast<const char*>(retry.data()));
    EXPECT_STREQ("01000", reinterpret_cast<const char*>(state));
    EXPECT_EQ(18, native_error);
    EXPECT_EQ(SQL_NO_DATA,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 3, nullptr, nullptr,
                            message.data() + 1, 0, nullptr));
    EXPECT_EQ(2u, statement->get_diagnostic_count());
}

TEST_F(DiagnosticsTest, SQLGetDiagRec_WideZeroCapacityAndUnicodeRetryBoundaries) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("22003", "A\xF0\x9F\x98\x80Z", 37);
    const SQLSMALLINT expected_units = sizeof(SQLWCHAR) == 2 ? 4 : 3;
    std::array<SQLWCHAR, 3> message{0x5a5a, 0x5a5a, 0x5a5a};
    SQLWCHAR state[6]{};
    SQLINTEGER native_error = -1;
    // A misaligned SQLSMALLINT output must write exactly its typed width.
    std::array<unsigned char, sizeof(SQLSMALLINT) + 2> length_bytes{};
    length_bytes.fill(0xa5);
    auto* length_ptr = reinterpret_cast<SQLSMALLINT*>(length_bytes.data() + 1);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, state, &native_error,
                             message.data() + 1, 0, length_ptr));
    EXPECT_EQ((std::array<SQLWCHAR, 3>{0x5a5a, 0x5a5a, 0x5a5a}), message);
    SQLSMALLINT required = -1;
    std::memcpy(&required, length_bytes.data() + 1, sizeof(required));
    EXPECT_EQ(expected_units, required);
    EXPECT_EQ(0xa5, length_bytes.front());
    EXPECT_EQ(0xa5, length_bytes.back());
    EXPECT_EQ(37, native_error);
    constexpr char expected_state[] = "22003";
    for (std::size_t index = 0; index < sizeof(expected_state); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(expected_state[index]), state[index]);
    }
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                             nullptr, 0, &required));
    EXPECT_EQ(expected_units, required);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                             message.data() + 1, 0, nullptr));
    EXPECT_EQ((std::array<SQLWCHAR, 3>{0x5a5a, 0x5a5a, 0x5a5a}), message);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                             message.data(), 3, &required));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), message[0]);
    if constexpr (sizeof(SQLWCHAR) == 2) {
        EXPECT_EQ(0, message[1]);  // Never emit a lone high surrogate.
        EXPECT_EQ(0x5a5a, message[2]);
    } else {
        EXPECT_EQ(static_cast<SQLWCHAR>(0x1f600), message[1]);
        EXPECT_EQ(0, message[2]);
    }
    EXPECT_EQ(expected_units, required);
    std::array<SQLWCHAR, 5> retry{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, state, &native_error,
                             retry.data(), expected_units + 1, &required));
    if constexpr (sizeof(SQLWCHAR) == 2) {
        EXPECT_EQ(0xd83d, retry[1]);
        EXPECT_EQ(0xde00, retry[2]);
        EXPECT_EQ(static_cast<SQLWCHAR>('Z'), retry[3]);
        EXPECT_EQ(0, retry[4]);
    } else {
        EXPECT_EQ(static_cast<SQLWCHAR>(0x1f600), retry[1]);
        EXPECT_EQ(static_cast<SQLWCHAR>('Z'), retry[2]);
        EXPECT_EQ(0, retry[3]);
    }
    EXPECT_EQ(expected_units, required);
    EXPECT_EQ(37, native_error);
    EXPECT_EQ(1u, statement->get_diagnostic_count());
}

TEST_F(DiagnosticsTest, SQLGetDiagRec_InvalidUtf8WideReadPreservesAnsiAndOtherRecords) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    const std::string malformed("\xF0\x28\x8C\x28", 4);
    statement->set_error("22018", malformed, 71);
    statement->add_diagnostic("01000", 72, "second diagnostic");

    std::array<SQLWCHAR, 5> wide_message{};
    wide_message.fill(0x5a5a);
    SQLWCHAR wide_state[6]{};
    SQLINTEGER native_error = -1;
    SQLSMALLINT required = -1;
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, wide_state,
                             &native_error, wide_message.data(), 5, &required));
    EXPECT_EQ((std::array<SQLWCHAR, 5>{0x5a5a, 0x5a5a, 0x5a5a, 0x5a5a, 0x5a5a}),
              wide_message);
    EXPECT_EQ(-1, required);
    EXPECT_EQ(71, native_error);
    EXPECT_EQ(2u, statement->get_diagnostic_count());
    constexpr char expected_state[] = "22018";
    for (std::size_t index = 0; index < sizeof(expected_state); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(expected_state[index]), wide_state[index]);
    }
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                             nullptr, 0, &required));
    EXPECT_EQ(2u, statement->get_diagnostic_count());

    SQLCHAR state[6]{};
    std::array<SQLCHAR, 5> ansi_message{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, &native_error,
                            ansi_message.data(), 5, &required));
    EXPECT_EQ(0, std::memcmp(ansi_message.data(), malformed.data(), 4));
    EXPECT_EQ(0, ansi_message[4]);
    EXPECT_STREQ("22018", reinterpret_cast<const char*>(state));
    EXPECT_EQ(71, native_error);
    EXPECT_EQ(4, required);

    std::array<SQLWCHAR, 18> second_message{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 2, nullptr, &native_error,
                             second_message.data(), 18, &required));
    EXPECT_EQ(72, native_error);
    EXPECT_EQ(17, required);
    constexpr char second_expected[] = "second diagnostic";
    for (std::size_t index = 0; index < sizeof(second_expected); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(second_expected[index]), second_message[index]);
    }
    EXPECT_EQ(SQL_ERROR,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                             wide_message.data(), 0, nullptr));
    EXPECT_EQ(2u, statement->get_diagnostic_count());

    statement->set_error("HY000", "valid recovery", 73);
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRecW(SQL_HANDLE_STMT, hstmt, 1, wide_state, &native_error,
                             second_message.data(), 18, &required));
    EXPECT_EQ(73, native_error);
    EXPECT_EQ(14, required);
    constexpr char recovery_expected[] = "valid recovery";
    for (std::size_t index = 0; index < sizeof(recovery_expected); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(recovery_expected[index]), second_message[index]);
    }
    constexpr char recovery_state[] = "HY000";
    for (std::size_t index = 0; index < sizeof(recovery_state); ++index) {
        EXPECT_EQ(static_cast<SQLWCHAR>(recovery_state[index]), wide_state[index]);
    }
    EXPECT_EQ(1u, statement->get_diagnostic_count());
}


TEST_F(DiagnosticsTest, SQLGetDiagField_RecordStringsZeroCapacityAndRetry) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("HY000", "alpha", 17);
    statement->add_diagnostic("01000", 18, "omega");
    struct StringField {
        SQLSMALLINT identifier;
        const char* expected;
    };
    const std::array<StringField, 6> fields{{
        {SQL_DIAG_SQLSTATE, "HY000"},
        {SQL_DIAG_MESSAGE_TEXT, "alpha"},
        {SQL_DIAG_CLASS_ORIGIN, "ISO 9075"},
        {SQL_DIAG_SUBCLASS_ORIGIN, "ISO 9075"},
        {SQL_DIAG_CONNECTION_NAME, ""},
        {SQL_DIAG_SERVER_NAME, ""},
    }};
    for (const auto& field : fields) {
        SCOPED_TRACE(field.identifier);
        const auto expected_length = static_cast<SQLSMALLINT>(
            std::strlen(field.expected));
        std::array<SQLCHAR, 32> output{};
        output.fill(0xa5);
        const auto untouched = output;
        alignas(SQLSMALLINT) std::array<std::byte,
            sizeof(SQLSMALLINT) + 2> length_bytes{};
        length_bytes.fill(std::byte{0x7b});
        auto* length = reinterpret_cast<SQLSMALLINT*>(length_bytes.data() + 1);
        EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagField(
            SQL_HANDLE_STMT, hstmt, 1, field.identifier,
            output.data() + 1, 0, length));
        EXPECT_EQ(untouched, output);
        SQLSMALLINT actual_length = -1;
        std::memcpy(&actual_length, length_bytes.data() + 1,
                    sizeof(actual_length));
        EXPECT_EQ(expected_length, actual_length);
        EXPECT_EQ(std::byte{0x7b}, length_bytes.front());
        EXPECT_EQ(std::byte{0x7b}, length_bytes.back());
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
            SQL_HANDLE_STMT, hstmt, 1, field.identifier,
            nullptr, 0, length));
        std::memcpy(&actual_length, length_bytes.data() + 1,
                    sizeof(actual_length));
        EXPECT_EQ(expected_length, actual_length);
        EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagField(
            SQL_HANDLE_STMT, hstmt, 1, field.identifier,
            output.data() + 1, 0, nullptr));
        EXPECT_EQ(untouched, output);
        EXPECT_EQ(expected_length == 0 ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO,
                  SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 1, field.identifier,
                                  output.data() + 1, 1, length));
        EXPECT_EQ(0, output[1]);
        EXPECT_EQ(0xa5, output[0]);
        EXPECT_EQ(0xa5, output[2]);
        if (expected_length > 0) {
            EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagField(
                SQL_HANDLE_STMT, hstmt, 1, field.identifier,
                output.data() + 1, expected_length, length));
            EXPECT_EQ(0, output[expected_length]);
        }
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
            SQL_HANDLE_STMT, hstmt, 1, field.identifier,
            output.data() + 1, expected_length + 1, length));
        EXPECT_STREQ(field.expected,
                     reinterpret_cast<const char*>(output.data() + 1));
        EXPECT_EQ(0xa5, output[0]);
        EXPECT_EQ(0xa5, output[expected_length + 2]);
        std::array<SQLWCHAR, 32> wide{};
        wide.fill(static_cast<SQLWCHAR>(0x7b7b));
        const auto wide_untouched = wide;
        EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagFieldW(
            SQL_HANDLE_STMT, hstmt, 1, field.identifier,
            wide.data() + 1, 0, length));
        EXPECT_EQ(wide_untouched, wide);
        std::memcpy(&actual_length, length_bytes.data() + 1,
                    sizeof(actual_length));
        EXPECT_EQ(expected_length * sizeof(SQLWCHAR), actual_length);
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
            SQL_HANDLE_STMT, hstmt, 1, field.identifier,
            nullptr, 0, length));
        std::memcpy(&actual_length, length_bytes.data() + 1,
                    sizeof(actual_length));
        EXPECT_EQ(expected_length * sizeof(SQLWCHAR), actual_length);
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
            SQL_HANDLE_STMT, hstmt, 1, field.identifier,
            wide.data() + 1,
            static_cast<SQLSMALLINT>((expected_length + 1) * sizeof(SQLWCHAR)),
            length));
        for (SQLSMALLINT index = 0; index < expected_length; ++index) {
            EXPECT_EQ(static_cast<SQLWCHAR>(field.expected[index]), wide[index + 1]);
        }
        EXPECT_EQ(0, wide[expected_length + 1]);
        EXPECT_EQ(static_cast<SQLWCHAR>(0x7b7b), wide.front());
        EXPECT_EQ(static_cast<SQLWCHAR>(0x7b7b), wide[expected_length + 2]);
        EXPECT_EQ(std::byte{0x7b}, length_bytes.front());
        EXPECT_EQ(std::byte{0x7b}, length_bytes.back());
    }
    ASSERT_EQ(2u, statement->get_diagnostic_count());
    const auto first = statement->get_diagnostic_record(1);
    const auto second = statement->get_diagnostic_record(2);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ("HY000", first->sqlstate);
    EXPECT_EQ(17, first->native_error);
    EXPECT_EQ("alpha", first->message_text);
    EXPECT_EQ("01000", second->sqlstate);
    EXPECT_EQ(18, second->native_error);
    EXPECT_EQ("omega", second->message_text);
}


TEST_F(DiagnosticsTest, SQLGetDiagField_HeaderStringBoundariesPreserveMetadata) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("HY000", "original", 37);
    statement->add_diagnostic("01000", 38, "second");
    statement->set_last_return_code(SQL_ERROR);
    for (const auto* text : {"SELECT CURSOR", ""}) {
        SCOPED_TRACE(text);
        statement->set_statement_diagnostic_header(
            29, 17, text, SQL_DIAG_SELECT_CURSOR);
        const auto units = static_cast<SQLSMALLINT>(std::strlen(text));
        const auto bytes = static_cast<SQLSMALLINT>(units * sizeof(SQLWCHAR));
        SQLSMALLINT required = -1;
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
            SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_DYNAMIC_FUNCTION,
            nullptr, 0, &required));
        EXPECT_EQ(units, required);
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
            SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_DYNAMIC_FUNCTION,
            nullptr, 0, &required));
        EXPECT_EQ(bytes, required);
        for (SQLSMALLINT capacity : {SQLSMALLINT{0}, SQLSMALLINT{1},
                                     units, static_cast<SQLSMALLINT>(units + 1)}) {
            SCOPED_TRACE(capacity);
            std::array<SQLCHAR, 32> output{};
            output.fill(0xa5);
            const auto untouched = output;
            const auto expected = capacity <= units
                ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
            EXPECT_EQ(expected, SQLGetDiagField(
                SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_DYNAMIC_FUNCTION,
                output.data() + 1, capacity, &required));
            EXPECT_EQ(units, required);
            if (capacity == 0) {
                EXPECT_EQ(untouched, output);
            } else {
                EXPECT_EQ(0, output[std::min(capacity - 1, int(units)) + 1]);
                EXPECT_EQ(0xa5, output[capacity + 1]);
            }
            EXPECT_EQ(0xa5, output.front());
            if (capacity > units) {
                EXPECT_STREQ(text, reinterpret_cast<const char*>(output.data() + 1));
            }
            std::array<SQLWCHAR, 32> wide_output{};
            wide_output.fill(static_cast<SQLWCHAR>(0x7b7b));
            const auto wide_untouched = wide_output;
            EXPECT_EQ(expected, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_DYNAMIC_FUNCTION,
                wide_output.data() + 1,
                static_cast<SQLSMALLINT>(capacity * sizeof(SQLWCHAR)), &required));
            EXPECT_EQ(bytes, required);
            if (capacity == 0) {
                EXPECT_EQ(wide_untouched, wide_output);
            } else {
                EXPECT_EQ(0, wide_output[std::min(capacity - 1, int(units)) + 1]);
                EXPECT_EQ(static_cast<SQLWCHAR>(0x7b7b), wide_output[capacity + 1]);
            }
            EXPECT_EQ(static_cast<SQLWCHAR>(0x7b7b), wide_output.front());
            if (capacity > units) {
                for (SQLSMALLINT index = 0; index < units; ++index) {
                    EXPECT_EQ(static_cast<SQLWCHAR>(text[index]), wide_output[index + 1]);
                }
            }
            SQLINTEGER code = -1;
            SQLINTEGER count = -1;
            SQLRETURN return_code = SQL_SUCCESS;
            SQLLEN row_count = -1;
            SQLLEN cursor_count = -1;
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
                SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_DYNAMIC_FUNCTION_CODE,
                &code, 0, nullptr));
            EXPECT_EQ(SQL_DIAG_SELECT_CURSOR, code);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_NUMBER, &count, 0, nullptr));
            EXPECT_EQ(2, count);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_RETURNCODE,
                &return_code, 0, nullptr));
            EXPECT_EQ(SQL_ERROR, return_code);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
                SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_ROW_COUNT,
                &row_count, 0, nullptr));
            EXPECT_EQ(17, row_count);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_CURSOR_ROW_COUNT,
                &cursor_count, 0, nullptr));
            EXPECT_EQ(29, cursor_count);
        }
        const auto header = statement->get_diagnostic_header();
        EXPECT_EQ(text, header.dynamic_function);
        EXPECT_EQ(29, header.cursor_row_count);
        EXPECT_EQ(17, header.row_count);
        EXPECT_EQ(SQL_DIAG_SELECT_CURSOR, header.dynamic_function_code);
        ASSERT_EQ(2u, statement->get_diagnostic_count());
        const auto record = statement->get_diagnostic_record(1);
        ASSERT_TRUE(record.has_value());
        EXPECT_EQ("HY000", record->sqlstate);
        EXPECT_EQ(37, record->native_error);
        EXPECT_EQ("original", record->message_text);
    }
}

TEST_F(DiagnosticsTest, SQLGetDiagField_WideByteCapacityAndScalarBoundaries) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("22003", "A\xF0\x9F\x98\x80Z", 53);
    statement->add_diagnostic("01000", 54, "second");
    constexpr auto units = sizeof(SQLWCHAR) == 2 ? 4 : 3;
    constexpr auto bytes = static_cast<SQLSMALLINT>(units * sizeof(SQLWCHAR));
    alignas(SQLWCHAR) std::array<std::byte, 2 + 6 * sizeof(SQLWCHAR)> storage{};
    storage.fill(std::byte{0x7b});
    auto* output = reinterpret_cast<SQLWCHAR*>(storage.data() + 1);
    const auto untouched = storage;
    alignas(SQLSMALLINT) std::array<std::byte, sizeof(SQLSMALLINT) + 2> lengths{};
    lengths.fill(std::byte{0x5a});
    auto* length = reinterpret_cast<SQLSMALLINT*>(lengths.data() + 1);
    SQLSMALLINT required = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT, nullptr, 0, length));
    std::memcpy(&required, lengths.data() + 1, sizeof(required));
    EXPECT_EQ(bytes, required);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT, output, 0, length));
    EXPECT_EQ(untouched, storage);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT, output, 0, nullptr));
    EXPECT_EQ(untouched, storage);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT, output,
        static_cast<SQLSMALLINT>(3 * sizeof(SQLWCHAR)), length));
    std::array<SQLWCHAR, 6> decoded{};
    std::memcpy(decoded.data(), storage.data() + 1, sizeof(decoded));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), decoded[0]);
    if constexpr (sizeof(SQLWCHAR) == 2) {
        EXPECT_EQ(0, decoded[1]);
        EXPECT_EQ(static_cast<SQLWCHAR>(0x7b7b), decoded[2]);
    } else {
        EXPECT_EQ(static_cast<SQLWCHAR>(0x1f600), decoded[1]);
        EXPECT_EQ(0, decoded[2]);
    }
    std::memcpy(&required, lengths.data() + 1, sizeof(required));
    EXPECT_EQ(bytes, required);
    EXPECT_EQ(std::byte{0x7b}, storage[1 + 3 * sizeof(SQLWCHAR)]);
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT, output,
        static_cast<SQLSMALLINT>((units + 1) * sizeof(SQLWCHAR)), length));
    std::memcpy(decoded.data(), storage.data() + 1, sizeof(decoded));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), decoded[0]);
    if constexpr (sizeof(SQLWCHAR) == 2) {
        EXPECT_EQ(static_cast<SQLWCHAR>(0xd83d), decoded[1]);
        EXPECT_EQ(static_cast<SQLWCHAR>(0xde00), decoded[2]);
    } else {
        EXPECT_EQ(static_cast<SQLWCHAR>(0x1f600), decoded[1]);
    }
    EXPECT_EQ(static_cast<SQLWCHAR>('Z'), decoded[units - 1]);
    EXPECT_EQ(0, decoded[units]);
    EXPECT_EQ(std::byte{0x7b}, storage.front());
    EXPECT_EQ(std::byte{0x7b}, storage[1 + (units + 1) * sizeof(SQLWCHAR)]);
    EXPECT_EQ(std::byte{0x7b}, storage.back());
    EXPECT_EQ(std::byte{0x5a}, lengths.front());
    EXPECT_EQ(std::byte{0x5a}, lengths.back());
    ASSERT_EQ(2u, statement->get_diagnostic_count());
    SQLCHAR state[6]{};
    SQLINTEGER native = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, &native, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<const char*>(state));
    EXPECT_EQ(53, native);
}


TEST_F(DiagnosticsTest, SQLGetDiagField_InvalidWideTextPreservesHeaderAndRecords) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    const std::string malformed("\xF0\x28\x8C\x28", 4);
    statement->set_error("22018", malformed, 71);
    statement->add_diagnostic("01000", 72, "valid second");
    statement->set_statement_diagnostic_header(
        31, 19, malformed, SQL_DIAG_SELECT_CURSOR);
    statement->set_last_return_code(SQL_ERROR);
    for (SQLSMALLINT record : {SQLSMALLINT{0}, SQLSMALLINT{1}}) {
        const auto identifier = static_cast<SQLSMALLINT>(record == 0
            ? SQL_DIAG_DYNAMIC_FUNCTION : SQL_DIAG_MESSAGE_TEXT);
        SCOPED_TRACE(record);
        for (SQLSMALLINT capacity : {SQLSMALLINT{0}, SQLSMALLINT{32}}) {
            std::array<SQLWCHAR, 32> output{};
            output.fill(static_cast<SQLWCHAR>(0x7b7b));
            const auto untouched = output;
            SQLSMALLINT length = -73;
            EXPECT_EQ(SQL_ERROR, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, record, identifier,
                output.data(), capacity, &length));
            EXPECT_EQ(untouched, output);
            EXPECT_EQ(-73, length);
            EXPECT_EQ(SQL_ERROR, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, record, identifier,
                nullptr, 0, &length));
            EXPECT_EQ(-73, length);
            ASSERT_EQ(2u, statement->get_diagnostic_count());
            const auto first = statement->get_diagnostic_record(1);
            const auto second = statement->get_diagnostic_record(2);
            ASSERT_TRUE(first.has_value());
            ASSERT_TRUE(second.has_value());
            EXPECT_EQ("22018", first->sqlstate);
            EXPECT_EQ(71, first->native_error);
            EXPECT_EQ(malformed, first->message_text);
            EXPECT_EQ("01000", second->sqlstate);
            EXPECT_EQ(72, second->native_error);
            EXPECT_EQ("valid second", second->message_text);
            const auto header = statement->get_diagnostic_header();
            EXPECT_EQ(malformed, header.dynamic_function);
            EXPECT_EQ(31, header.cursor_row_count);
            EXPECT_EQ(19, header.row_count);
            EXPECT_EQ(SQL_DIAG_SELECT_CURSOR, header.dynamic_function_code);
            EXPECT_EQ(SQL_ERROR, statement->get_last_return_code());
            std::array<SQLCHAR, 8> ansi{};
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
                SQL_HANDLE_STMT, hstmt, record, identifier,
                ansi.data(), static_cast<SQLSMALLINT>(ansi.size()), &length));
            EXPECT_EQ(4, length);
            EXPECT_EQ(0, std::memcmp(ansi.data(), malformed.data(), malformed.size()));
            EXPECT_EQ(0, ansi[4]);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(
                SQL_HANDLE_STMT, hstmt, 1, nullptr, nullptr,
                ansi.data(), static_cast<SQLSMALLINT>(ansi.size()), &length));
            EXPECT_EQ(4, length);
            EXPECT_EQ(0, std::memcmp(ansi.data(), malformed.data(), malformed.size()));
            output.fill(0);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, 2, SQL_DIAG_MESSAGE_TEXT,
                output.data(), static_cast<SQLSMALLINT>(sizeof(output)), &length));
            EXPECT_EQ(12 * sizeof(SQLWCHAR), length);
            constexpr char valid[] = "valid second";
            for (std::size_t index = 0; index < sizeof(valid); ++index) {
                EXPECT_EQ(static_cast<SQLWCHAR>(valid[index]), output[index]);
            }
        }
    }
    // A deliberate fixture header update permits recovery without changing records.
    statement->set_statement_diagnostic_header(
        31, 19, "SELECT CURSOR", SQL_DIAG_SELECT_CURSOR);
    std::array<SQLWCHAR, 32> recovered{};
    SQLSMALLINT length = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 0, SQL_DIAG_DYNAMIC_FUNCTION,
        recovered.data(), static_cast<SQLSMALLINT>(sizeof(recovered)), &length));
    EXPECT_EQ(13 * sizeof(SQLWCHAR), length);
    EXPECT_EQ(static_cast<SQLWCHAR>('S'), recovered[0]);
    EXPECT_EQ(0, recovered[13]);
    EXPECT_EQ(SQL_ERROR, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT,
        nullptr, 0, &length));
    EXPECT_EQ(13 * sizeof(SQLWCHAR), length);
    EXPECT_EQ(2u, statement->get_diagnostic_count());
    const auto first = statement->get_diagnostic_record(1);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(malformed, first->message_text);
    EXPECT_EQ("SELECT CURSOR", statement->get_diagnostic_header().dynamic_function);
}

TEST_F(DiagnosticsTest, SQLGetDiagField_ErrorAndNumericControlsAreNondestructive) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("HY000", "original", 91);
    statement->add_diagnostic("01000", 92, "second");
    statement->set_statement_diagnostic_header(
        29, 17, "SELECT CURSOR", SQL_DIAG_SELECT_CURSOR);
    statement->set_last_return_code(SQL_ERROR);
    for (const auto api : {&SQLGetDiagField, &SQLGetDiagFieldW}) {
        std::array<SQLWCHAR, 16> output{};
        output.fill(static_cast<SQLWCHAR>(0x7b7b));
        const auto untouched = output;
        SQLSMALLINT length = -11;
        EXPECT_EQ(SQL_INVALID_HANDLE, api(
            SQL_HANDLE_DBC, hstmt, 1, SQL_DIAG_MESSAGE_TEXT,
            output.data(), 0, &length));
        EXPECT_EQ(SQL_INVALID_HANDLE, api(
            SQL_HANDLE_STMT, nullptr, 1, SQL_DIAG_MESSAGE_TEXT,
            output.data(), 0, &length));
        EXPECT_EQ(SQL_ERROR, api(
            SQL_HANDLE_STMT, hstmt, 1, -12345,
            output.data(), 0, &length));
        EXPECT_EQ(SQL_NO_DATA, api(
            SQL_HANDLE_STMT, hstmt, 3, SQL_DIAG_MESSAGE_TEXT,
            output.data(), 0, &length));
        EXPECT_EQ(SQL_ERROR, api(
            SQL_HANDLE_STMT, hstmt, -1, SQL_DIAG_MESSAGE_TEXT,
            output.data(), 0, &length));
        EXPECT_EQ(SQL_ERROR, api(
            SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_MESSAGE_TEXT,
            output.data(), -1, &length));
        EXPECT_EQ(SQL_ERROR, api(
            SQL_HANDLE_DBC, hdbc, 0, SQL_DIAG_DYNAMIC_FUNCTION,
            output.data(), 0, &length));
        EXPECT_EQ(untouched, output);
        EXPECT_EQ(-11, length);
        // This generic lambda tests exact typed publication, not a new production seam.
        const auto check_numeric = [&](SQLSMALLINT record,
                                       SQLSMALLINT identifier, auto expected) {
            using Value = decltype(expected);
            alignas(Value) std::array<std::byte, sizeof(Value) + 2> storage{};
            storage.fill(std::byte{0x5a});
            EXPECT_EQ(SQL_SUCCESS, api(
                SQL_HANDLE_STMT, hstmt, record, identifier,
                storage.data() + 1, 0, &length));
            Value actual{};
            std::memcpy(&actual, storage.data() + 1, sizeof(actual));
            EXPECT_EQ(expected, actual);
            EXPECT_EQ(std::byte{0x5a}, storage.front());
            EXPECT_EQ(std::byte{0x5a}, storage.back());
            EXPECT_EQ(-11, length);
        };
        check_numeric(0, SQL_DIAG_NUMBER, SQLINTEGER{2});
        check_numeric(0, SQL_DIAG_RETURNCODE, SQLRETURN{SQL_ERROR});
        check_numeric(0, SQL_DIAG_DYNAMIC_FUNCTION_CODE,
                      SQLINTEGER{SQL_DIAG_SELECT_CURSOR});
        check_numeric(0, SQL_DIAG_ROW_COUNT, SQLLEN{17});
        check_numeric(0, SQL_DIAG_CURSOR_ROW_COUNT, SQLLEN{29});
        check_numeric(1, SQL_DIAG_NATIVE, SQLINTEGER{91});
        check_numeric(1, SQL_DIAG_ROW_NUMBER, SQLLEN{SQL_NO_ROW_NUMBER});
        check_numeric(1, SQL_DIAG_COLUMN_NUMBER,
                      SQLINTEGER{SQL_NO_COLUMN_NUMBER});
    }
    for (SQLSMALLINT record : {SQLSMALLINT{0}, SQLSMALLINT{1}}) {
        const auto identifier = static_cast<SQLSMALLINT>(record == 0
            ? SQL_DIAG_DYNAMIC_FUNCTION : SQL_DIAG_MESSAGE_TEXT);
        std::array<SQLWCHAR, 16> output{};
        output.fill(static_cast<SQLWCHAR>(0x7b7b));
        const auto untouched = output;
        SQLSMALLINT length = -11;
        EXPECT_EQ(SQL_ERROR, SQLGetDiagFieldW(
            SQL_HANDLE_STMT, hstmt, record, identifier,
            output.data(), static_cast<SQLSMALLINT>(sizeof(SQLWCHAR) - 1), &length));
        EXPECT_EQ(untouched, output);
        EXPECT_EQ(-11, length);
        ASSERT_EQ(2u, statement->get_diagnostic_count());
        const auto first = statement->get_diagnostic_record(1);
        const auto second = statement->get_diagnostic_record(2);
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        EXPECT_EQ("HY000", first->sqlstate);
        EXPECT_EQ(91, first->native_error);
        EXPECT_EQ("original", first->message_text);
        EXPECT_EQ("01000", second->sqlstate);
        EXPECT_EQ(92, second->native_error);
        EXPECT_EQ("second", second->message_text);
        const auto header = statement->get_diagnostic_header();
        EXPECT_EQ("SELECT CURSOR", header.dynamic_function);
        EXPECT_EQ(29, header.cursor_row_count);
        EXPECT_EQ(17, header.row_count);
        EXPECT_EQ(SQL_DIAG_SELECT_CURSOR, header.dynamic_function_code);
        EXPECT_EQ(SQL_ERROR, statement->get_last_return_code());
    }
}


TEST_F(DiagnosticsTest, SQLGetDiagField_HeaderNumericsIgnoreRecordNumberAndCapacity) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("HY000", "original", 41);
    statement->add_diagnostic("01000", 42, "second");
    statement->set_statement_diagnostic_header(
        29, 17, "SELECT CURSOR", SQL_DIAG_SELECT_CURSOR);
    statement->set_last_return_code(SQL_ERROR);
    const std::array<SQLSMALLINT, 6> indices{
        std::numeric_limits<SQLSMALLINT>::min(), -1, 0, 1, 7,
        std::numeric_limits<SQLSMALLINT>::max()};
    const std::array<SQLSMALLINT, 4> capacities{
        std::numeric_limits<SQLSMALLINT>::min(), -1, 0, 1};
    for (const auto api : {&SQLGetDiagField, &SQLGetDiagFieldW}) {
        for (const auto record : indices) {
            SCOPED_TRACE(record);
            for (const auto capacity : capacities) {
                SCOPED_TRACE(capacity);
                const auto check = [&](SQLSMALLINT identifier, auto expected) {
                    using Value = decltype(expected);
                    alignas(Value) std::array<std::byte, sizeof(Value) + 2> output{};
                    output.fill(std::byte{0x5a});
                    SQLSMALLINT length = -73;
                    EXPECT_EQ(SQL_SUCCESS, api(
                        SQL_HANDLE_STMT, hstmt, record, identifier,
                        output.data() + 1, capacity, &length));
                    Value actual{};
                    std::memcpy(&actual, output.data() + 1, sizeof(actual));
                    EXPECT_EQ(expected, actual);
                    EXPECT_EQ(std::byte{0x5a}, output.front());
                    EXPECT_EQ(std::byte{0x5a}, output.back());
                    EXPECT_EQ(-73, length);
                };
                check(SQL_DIAG_NUMBER, SQLINTEGER{2});
                check(SQL_DIAG_RETURNCODE, SQLRETURN{SQL_ERROR});
                check(SQL_DIAG_DYNAMIC_FUNCTION_CODE,
                      SQLINTEGER{SQL_DIAG_SELECT_CURSOR});
                check(SQL_DIAG_ROW_COUNT, SQLLEN{17});
                check(SQL_DIAG_CURSOR_ROW_COUNT, SQLLEN{29});
            }
        }
    }
    EXPECT_EQ(2u, statement->get_diagnostic_count());
    EXPECT_EQ(SQL_ERROR, statement->get_last_return_code());
    const auto header = statement->get_diagnostic_header();
    EXPECT_EQ("SELECT CURSOR", header.dynamic_function);
    EXPECT_EQ(29, header.cursor_row_count);
    EXPECT_EQ(17, header.row_count);
    EXPECT_EQ(SQL_DIAG_SELECT_CURSOR, header.dynamic_function_code);
    statement->clear_diagnostics();
    for (const auto api : {&SQLGetDiagField, &SQLGetDiagFieldW}) {
        for (const auto record : indices) {
            SQLINTEGER count = -1;
            SQLSMALLINT length = -73;
            EXPECT_EQ(SQL_SUCCESS, api(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_NUMBER,
                &count, -1, &length));
            EXPECT_EQ(0, count);
            EXPECT_EQ(-73, length);
            EXPECT_EQ(SQL_SUCCESS, api(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_NUMBER,
                nullptr, -1, &length));
            EXPECT_EQ(-73, length);
        }
    }
}


TEST_F(DiagnosticsTest, SQLGetDiagField_HeaderStringUsesIdentifierForEveryRecordNumber) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("HY000", "original", 51);
    statement->add_diagnostic("01000", 52, "second");
    statement->set_last_return_code(SQL_ERROR);
    const std::array<SQLSMALLINT, 6> indices{
        std::numeric_limits<SQLSMALLINT>::min(), -1, 0, 1, 7,
        std::numeric_limits<SQLSMALLINT>::max()};
    for (const auto* text : {"SELECT CURSOR", ""}) {
        statement->set_statement_diagnostic_header(
            29, 17, text, SQL_DIAG_SELECT_CURSOR);
        const auto units = static_cast<SQLSMALLINT>(std::strlen(text));
        for (const auto record : indices) {
            SCOPED_TRACE(record);
            std::array<SQLCHAR, 32> output{};
            output.fill(0xa5);
            const auto untouched = output;
            SQLSMALLINT length = -73;
            EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagField(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
                output.data() + 1, 0, &length));
            EXPECT_EQ(units, length);
            EXPECT_EQ(untouched, output);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
                nullptr, 0, &length));
            EXPECT_EQ(units, length);
            EXPECT_EQ(units == 0 ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO,
                      SQLGetDiagField(SQL_HANDLE_STMT, hstmt, record,
                                      SQL_DIAG_DYNAMIC_FUNCTION,
                                      output.data() + 1, 1, &length));
            EXPECT_EQ(0, output[1]);
            EXPECT_EQ(0xa5, output[2]);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
                output.data() + 1, units + 1, &length));
            EXPECT_STREQ(text, reinterpret_cast<const char*>(output.data() + 1));
            EXPECT_EQ(0xa5, output.front());
            EXPECT_EQ(0xa5, output[units + 2]);
            std::array<SQLWCHAR, 32> wide{};
            wide.fill(static_cast<SQLWCHAR>(0x7b7b));
            const auto wide_untouched = wide;
            EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
                wide.data() + 1, 0, &length));
            EXPECT_EQ(units * sizeof(SQLWCHAR), length);
            EXPECT_EQ(wide_untouched, wide);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
                nullptr, 0, &length));
            EXPECT_EQ(units * sizeof(SQLWCHAR), length);
            EXPECT_EQ(units == 0 ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO,
                      SQLGetDiagFieldW(SQL_HANDLE_STMT, hstmt, record,
                                       SQL_DIAG_DYNAMIC_FUNCTION,
                                       wide.data() + 1, sizeof(SQLWCHAR), &length));
            EXPECT_EQ(0, wide[1]);
            EXPECT_EQ(static_cast<SQLWCHAR>(0x7b7b), wide[2]);
            EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
                wide.data() + 1,
                static_cast<SQLSMALLINT>((units + 1) * sizeof(SQLWCHAR)), &length));
            for (SQLSMALLINT index = 0; index < units; ++index) {
                EXPECT_EQ(static_cast<SQLWCHAR>(text[index]), wide[index + 1]);
            }
            EXPECT_EQ(0, wide[units + 1]);
            EXPECT_EQ(static_cast<SQLWCHAR>(0x7b7b), wide.front());
            EXPECT_EQ(static_cast<SQLWCHAR>(0x7b7b), wide[units + 2]);
            for (const auto api : {&SQLGetDiagField, &SQLGetDiagFieldW}) {
                const auto before = wide;
                length = -73;
                EXPECT_EQ(SQL_ERROR, api(
                    SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
                    wide.data(), -1, &length));
                EXPECT_EQ(-73, length);
                EXPECT_EQ(before, wide);
            }
            const auto header = statement->get_diagnostic_header();
            EXPECT_EQ(text, header.dynamic_function);
            EXPECT_EQ(29, header.cursor_row_count);
            EXPECT_EQ(17, header.row_count);
            EXPECT_EQ(SQL_DIAG_SELECT_CURSOR, header.dynamic_function_code);
            EXPECT_EQ(SQL_ERROR, statement->get_last_return_code());
            EXPECT_EQ(2u, statement->get_diagnostic_count());
        }
    }
    const std::string malformed("\xF0\x28\x8C\x28", 4);
    statement->set_statement_diagnostic_header(
        29, 17, malformed, SQL_DIAG_SELECT_CURSOR);
    for (SQLSMALLINT record : {SQLSMALLINT{-1}, SQLSMALLINT{7}}) {
        std::array<SQLWCHAR, 8> output{};
        output.fill(static_cast<SQLWCHAR>(0x7b7b));
        const auto untouched = output;
        SQLSMALLINT length = -73;
        EXPECT_EQ(SQL_ERROR, SQLGetDiagFieldW(
            SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
            output.data(), sizeof(output), &length));
        EXPECT_EQ(untouched, output);
        EXPECT_EQ(-73, length);
        std::array<SQLCHAR, 8> ansi{};
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
            SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_DYNAMIC_FUNCTION,
            ansi.data(), ansi.size(), &length));
        EXPECT_EQ(4, length);
        EXPECT_EQ(0, std::memcmp(ansi.data(), malformed.data(), malformed.size()));
        EXPECT_EQ(malformed, statement->get_diagnostic_header().dynamic_function);
        ASSERT_EQ(2u, statement->get_diagnostic_count());
        const auto first = statement->get_diagnostic_record(1);
        ASSERT_TRUE(first.has_value());
        EXPECT_EQ("HY000", first->sqlstate);
        EXPECT_EQ(51, first->native_error);
        EXPECT_EQ("original", first->message_text);
    }
}

TEST_F(DiagnosticsTest, SQLGetDiagField_RecordNumericsIgnoreCapacityAndKeepTypedPublication) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("HY000", "original", 71);
    statement->add_attributed_diagnostic("22003", "second", 23, 7, 72);
    statement->set_statement_diagnostic_header(
        29, 17, "SELECT CURSOR", SQL_DIAG_SELECT_CURSOR);
    statement->set_last_return_code(SQL_ERROR);
    const std::array<SQLSMALLINT, 6> capacities{
        std::numeric_limits<SQLSMALLINT>::min(), SQL_NTS, -1, 0, 1,
        std::numeric_limits<SQLSMALLINT>::max()};
    for (const auto api : {&SQLGetDiagField, &SQLGetDiagFieldW}) {
        for (const auto capacity : capacities) {
            SCOPED_TRACE(capacity);
            const auto check = [&](SQLSMALLINT record, SQLSMALLINT identifier,
                                   auto expected) {
                using Value = decltype(expected);
                alignas(Value) std::array<std::byte, sizeof(Value) + 2> output{};
                output.fill(std::byte{0x5a});
                SQLSMALLINT length = -73;
                EXPECT_EQ(SQL_SUCCESS, api(
                    SQL_HANDLE_STMT, hstmt, record, identifier,
                    output.data() + 1, capacity, &length));
                Value actual{};
                std::memcpy(&actual, output.data() + 1, sizeof(actual));
                EXPECT_EQ(expected, actual);
                EXPECT_EQ(std::byte{0x5a}, output.front());
                EXPECT_EQ(std::byte{0x5a}, output.back());
                EXPECT_EQ(-73, length);
                EXPECT_EQ(SQL_SUCCESS, api(
                    SQL_HANDLE_STMT, hstmt, record, identifier,
                    nullptr, capacity, &length));
                EXPECT_EQ(-73, length);
            };
            check(1, SQL_DIAG_NATIVE, SQLINTEGER{71});
            check(1, SQL_DIAG_ROW_NUMBER, SQLLEN{SQL_NO_ROW_NUMBER});
            check(1, SQL_DIAG_COLUMN_NUMBER, SQLINTEGER{SQL_NO_COLUMN_NUMBER});
            check(2, SQL_DIAG_NATIVE, SQLINTEGER{72});
            check(2, SQL_DIAG_ROW_NUMBER, SQLLEN{23});
            check(2, SQL_DIAG_COLUMN_NUMBER, SQLINTEGER{7});
        }
        for (SQLSMALLINT record : {SQLSMALLINT{-1}, SQLSMALLINT{0},
                                   SQLSMALLINT{3},
                                   std::numeric_limits<SQLSMALLINT>::max()}) {
            SQLINTEGER output = -19;
            SQLSMALLINT length = -73;
            EXPECT_EQ(record > 0 ? SQL_NO_DATA : SQL_ERROR, api(
                SQL_HANDLE_STMT, hstmt, record, SQL_DIAG_NATIVE,
                &output, -1, &length));
            EXPECT_EQ(-19, output);
            EXPECT_EQ(-73, length);
        }
    }
    ASSERT_EQ(2u, statement->get_diagnostic_count());
    SQLCHAR state[6]{};
    SQLINTEGER native = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, &native, nullptr, 0, nullptr));
    EXPECT_STREQ("HY000", reinterpret_cast<const char*>(state));
    EXPECT_EQ(71, native);
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 2, state, &native, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<const char*>(state));
    EXPECT_EQ(72, native);
    EXPECT_EQ(29, statement->get_diagnostic_header().cursor_row_count);
    EXPECT_EQ(17, statement->get_diagnostic_header().row_count);
    EXPECT_EQ(SQL_ERROR, statement->get_last_return_code());
}


TEST_F(DiagnosticsTest, SQLGetDiagField_FieldDomainsPreserveStringAndRetrievalControls) {
    auto statement = rs::odbc::HandleRegistry::instance().get_handle_as<
        rs::odbc::ODBCStatement>(hstmt);
    ASSERT_NE(nullptr, statement);
    statement->set_error("HY000", "original", 81);
    statement->add_diagnostic("01000", 82, "second");
    statement->set_statement_diagnostic_header(
        29, 17, "SELECT CURSOR", SQL_DIAG_SELECT_CURSOR);
    statement->set_last_return_code(SQL_ERROR);
    const std::array<SQLSMALLINT, 6> strings{
        SQL_DIAG_SQLSTATE, SQL_DIAG_MESSAGE_TEXT, SQL_DIAG_CLASS_ORIGIN,
        SQL_DIAG_SUBCLASS_ORIGIN, SQL_DIAG_CONNECTION_NAME, SQL_DIAG_SERVER_NAME};
    for (const auto api : {&SQLGetDiagField, &SQLGetDiagFieldW}) {
        std::array<SQLWCHAR, 16> output{};
        output.fill(static_cast<SQLWCHAR>(0x7b7b));
        const auto untouched = output;
        SQLSMALLINT length = -73;
        for (const auto identifier : strings) {
            SCOPED_TRACE(identifier);
            EXPECT_EQ(SQL_ERROR, api(
                SQL_HANDLE_STMT, hstmt, 1, identifier,
                output.data(), -1, &length));
            EXPECT_EQ(SQL_ERROR, api(
                SQL_HANDLE_STMT, hstmt, 1, identifier, nullptr, -1, &length));
            for (SQLSMALLINT record : {SQLSMALLINT{-1}, SQLSMALLINT{0}}) {
                EXPECT_EQ(SQL_ERROR, api(
                    SQL_HANDLE_STMT, hstmt, record, identifier,
                    output.data(), 0, &length));
            }
            EXPECT_EQ(SQL_NO_DATA, api(
                SQL_HANDLE_STMT, hstmt, 3, identifier,
                output.data(), 0, &length));
            // Preserve the existing negative-string-length error precedence.
            EXPECT_EQ(SQL_ERROR, api(
                SQL_HANDLE_STMT, hstmt, 3, identifier,
                output.data(), -1, &length));
            EXPECT_EQ(untouched, output);
            EXPECT_EQ(-73, length);
        }
        EXPECT_EQ(SQL_INVALID_HANDLE, api(
            SQL_HANDLE_DBC, hstmt, -1, SQL_DIAG_NUMBER,
            output.data(), -1, &length));
        EXPECT_EQ(SQL_INVALID_HANDLE, api(
            SQL_HANDLE_STMT, nullptr, -1, SQL_DIAG_NUMBER,
            output.data(), -1, &length));
        EXPECT_EQ(SQL_ERROR, api(
            SQL_HANDLE_STMT, hstmt, 0, -12345,
            output.data(), 0, &length));
        for (SQLSMALLINT record : {SQLSMALLINT{-1}, SQLSMALLINT{7}}) {
            for (SQLSMALLINT identifier : {SQLSMALLINT{SQL_DIAG_ROW_COUNT},
                                           SQLSMALLINT{SQL_DIAG_CURSOR_ROW_COUNT},
                                           SQLSMALLINT{SQL_DIAG_DYNAMIC_FUNCTION},
                                           SQLSMALLINT{SQL_DIAG_DYNAMIC_FUNCTION_CODE}}) {
                EXPECT_EQ(SQL_ERROR, api(
                    SQL_HANDLE_DBC, hdbc, record, identifier,
                    output.data(), -1, &length));
            }
        }
        EXPECT_EQ(untouched, output);
        EXPECT_EQ(-73, length);
        SQLINTEGER native = -1;
        EXPECT_EQ(SQL_SUCCESS, api(
            SQL_HANDLE_STMT, hstmt, 1, SQL_DIAG_NATIVE, &native, -1, &length));
        EXPECT_EQ(81, native);
        EXPECT_EQ(-73, length);
    }
    ASSERT_EQ(2u, statement->get_diagnostic_count());
    const auto first = statement->get_diagnostic_record(1);
    const auto second = statement->get_diagnostic_record(2);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ("HY000", first->sqlstate);
    EXPECT_EQ(81, first->native_error);
    EXPECT_EQ("original", first->message_text);
    EXPECT_EQ("01000", second->sqlstate);
    EXPECT_EQ(82, second->native_error);
    EXPECT_EQ("second", second->message_text);
    EXPECT_EQ(29, statement->get_diagnostic_header().cursor_row_count);
    EXPECT_EQ(17, statement->get_diagnostic_header().row_count);
    EXPECT_EQ(SQL_ERROR, statement->get_last_return_code());
    statement->clear_diagnostics();
    statement->set_statement_diagnostic_header(
        29, 17, "SELECT CURSOR", SQL_DIAG_SELECT_CURSOR);
    SQLCHAR text[32]{};
    SQLWCHAR wide[32]{};
    SQLSMALLINT length = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
        SQL_HANDLE_STMT, hstmt, 7, SQL_DIAG_DYNAMIC_FUNCTION,
        text, sizeof(text), &length));
    EXPECT_STREQ("SELECT CURSOR", reinterpret_cast<const char*>(text));
    EXPECT_EQ(13, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagFieldW(
        SQL_HANDLE_STMT, hstmt, -1, SQL_DIAG_DYNAMIC_FUNCTION,
        wide, sizeof(wide), &length));
    EXPECT_EQ(13 * sizeof(SQLWCHAR), length);
    EXPECT_EQ(static_cast<SQLWCHAR>('S'), wide[0]);
    EXPECT_EQ(0, wide[13]);
    EXPECT_EQ(0u, statement->get_diagnostic_count());
    struct SupportedHandle {
        SQLSMALLINT type;
        SQLHANDLE value;
    };
    const std::array<SupportedHandle, 2> handles{{
        {SQL_HANDLE_ENV, henv}, {SQL_HANDLE_DBC, hdbc},
    }};
    for (const auto& handle : handles) {
        auto object = rs::odbc::HandleRegistry::instance().get_handle(handle.value);
        ASSERT_NE(nullptr, object);
        object->clear_diagnostics();
        object->set_last_return_code(SQL_SUCCESS);
        for (const auto api : {&SQLGetDiagField, &SQLGetDiagFieldW}) {
            SQLINTEGER count = -1;
            SQLRETURN return_code = SQL_ERROR;
            EXPECT_EQ(SQL_SUCCESS, api(
                handle.type, handle.value, -1, SQL_DIAG_NUMBER,
                &count, -1, nullptr));
            EXPECT_EQ(0, count);
            EXPECT_EQ(SQL_SUCCESS, api(
                handle.type, handle.value, 7, SQL_DIAG_RETURNCODE,
                &return_code, -1, nullptr));
            EXPECT_EQ(SQL_SUCCESS, return_code);
            EXPECT_EQ(0u, object->get_diagnostic_count());
            EXPECT_EQ(SQL_SUCCESS, object->get_last_return_code());
        }
    }
}
