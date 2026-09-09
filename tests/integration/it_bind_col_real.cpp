#include <gtest/gtest.h>
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"

#include <chrono>
#include <cstdint>
#include <string>

class BindColIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        
        SQLRETURN ret = SQLConnect(hdbc, (SQLCHAR*)"DSN=RedshiftProd", SQL_NTS, nullptr, 0, nullptr, 0);
        if (ret != SQL_SUCCESS) {
            GTEST_SKIP() << "Database connection failed - skipping integration tests";
        }
        
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    }
    
    void TearDown() override {
        if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        if (hdbc) {
            SQLDisconnect(hdbc);
            SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        }
        if (henv) SQLFreeHandle(SQL_HANDLE_ENV, henv);
    }
    
    SQLHENV henv = nullptr;
    SQLHDBC hdbc = nullptr;
    SQLHSTMT hstmt = nullptr;
};

TEST_F(BindColIntegrationTest, QueryTimeoutUsesTransportDeadline) {
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_QUERY_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(1)), 0));

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT pg_sleep(3)", SQL_NTS));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::seconds(3));

    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HYT00", reinterpret_cast<char*>(sqlstate));
}

// Test basic column binding with different data types
TEST_F(BindColIntegrationTest, BasicColumnBinding) {
    // Execute query with known data types
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Hello' as str_col, 123 as int_col, 45.67 as double_col", SQL_NTS));
    
    // Bind columns to variables
    char str_val[256];
    SQLINTEGER int_val;
    SQLDOUBLE double_val;
    SQLLEN str_len, int_len, double_len;
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, str_val, sizeof(str_val), &str_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_SLONG, &int_val, 0, &int_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_DOUBLE, &double_val, 0, &double_len));
    
    // Fetch should auto-populate bound columns
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // Verify bound data
    EXPECT_STREQ("Hello", str_val);
    EXPECT_EQ(123, int_val);
    EXPECT_DOUBLE_EQ(45.67, double_val);
}

TEST_F(BindColIntegrationTest, ApplicationDescriptorDrivesFetchBinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'descriptor'::text", SQL_NTS));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &descriptor));
    char value[32]{};
    SQLLEN indicator = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_C_CHAR}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_OCTET_LENGTH,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{sizeof(value)}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_DATA_PTR, value, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_INDICATOR_PTR, &indicator, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_OCTET_LENGTH_PTR, &indicator, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_ROW_DESC, descriptor, 0));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_STREQ("descriptor", value);
    EXPECT_EQ(10, indicator);

    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

TEST_F(BindColIntegrationTest,
       RejectsUnsupportedAttachedRowArraysBeforeFetch) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS));
    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &descriptor));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 0, SQL_DESC_ARRAY_SIZE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_ROW_DESC, descriptor, 0));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HYC00", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

// Test column binding with NULL values
TEST_F(BindColIntegrationTest, NullValueBinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT NULL as null_col, 'NotNull' as str_col", SQL_NTS));
    
    char null_val[256];
    char str_val[256];
    SQLLEN null_len, str_len;
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, null_val, sizeof(null_val), &null_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_CHAR, str_val, sizeof(str_val), &str_len));
    
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // NULL column should have SQL_NULL_DATA indicator
    EXPECT_EQ(SQL_NULL_DATA, null_len);
    EXPECT_STREQ("NotNull", str_val);
}

TEST_F(BindColIntegrationTest, BoundNullAndEmptyStringRemainDistinct) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text, ''::text", SQL_NTS));

    char null_value[16] = "unchanged";
    char empty_value[16] = "unchanged";
    SQLLEN null_indicator = 99;
    SQLLEN empty_indicator = 99;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, null_value, sizeof(null_value), &null_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 2, SQL_C_CHAR, empty_value, sizeof(empty_value), &empty_indicator));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NULL_DATA, null_indicator);
    EXPECT_STREQ("unchanged", null_value);
    EXPECT_EQ(0, empty_indicator);
    EXPECT_STREQ("", empty_value);
}

TEST_F(BindColIntegrationTest, GetDataReportsNullWithoutTouchingBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text, ''::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char null_value[16] = "unchanged";
    char empty_value[16] = "unchanged";
    SQLLEN null_indicator = 99;
    SQLLEN empty_indicator = 99;
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, null_value, sizeof(null_value), &null_indicator));
    EXPECT_EQ(SQL_NULL_DATA, null_indicator);
    EXPECT_STREQ("unchanged", null_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, empty_value, sizeof(empty_value), &empty_indicator));
    EXPECT_EQ(0, empty_indicator);
    EXPECT_STREQ("", empty_value);
}

TEST_F(BindColIntegrationTest, GetDataSupportsAnyColumnOrder) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'first'::text, 'second'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char second[16]{};
    char first[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, second, sizeof(second), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, first, sizeof(first), nullptr));
    EXPECT_STREQ("second", second);
    EXPECT_STREQ("first", first);
}

TEST_F(BindColIntegrationTest, NullWithoutIndicatorReturns22002) {
    SQLUSMALLINT row_status = SQL_ROW_SUCCESS;
    SQLULEN rows_fetched = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROW_STATUS_PTR, &row_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text", SQL_NTS));

    char value[16] = "unchanged";
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, value, sizeof(value), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_STREQ("unchanged", value);
    EXPECT_EQ(1u, rows_fetched);
    EXPECT_EQ(SQL_ROW_ERROR, row_status);

    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22002", reinterpret_cast<char*>(sqlstate));
}

TEST_F(BindColIntegrationTest, ReportsSingleRowFetchStatus) {
    SQLUSMALLINT row_status = SQL_ROW_NOROW;
    SQLULEN rows_fetched = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROW_STATUS_PTR, &row_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT value FROM (VALUES ('abcdef'::text), "
                         "('xy'::text)) rows(value)", SQL_NTS));

    char value[4]{};
    SQLLEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, value, sizeof(value), &length));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_EQ(1u, rows_fetched);
    EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO, row_status);
    EXPECT_STREQ("abc", value);

    EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(1u, rows_fetched);
    EXPECT_EQ(SQL_ROW_SUCCESS, row_status);
    EXPECT_STREQ("xy", value);

    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    EXPECT_EQ(0u, rows_fetched);
    EXPECT_EQ(SQL_ROW_NOROW, row_status);
}

TEST_F(BindColIntegrationTest, GetDataNullWithoutIndicatorReturns22002) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char value[16] = "unchanged";
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_CHAR, value, sizeof(value), nullptr));
    EXPECT_STREQ("unchanged", value);

    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22002", reinterpret_cast<char*>(sqlstate));
}

TEST_F(BindColIntegrationTest, GetDataRetrievesLongTextInChunks) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'abcdefghijklmnopqrstuvwxyz'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    const std::vector<std::string> expected_chunks{
        "abcde", "fghij", "klmno", "pqrst", "uvwxy", "z"};
    const std::vector<SQLLEN> expected_remaining{26, 21, 16, 11, 6, 1};
    std::string assembled;
    for (std::size_t i = 0; i < expected_chunks.size(); ++i) {
        char chunk[6]{};
        SQLLEN remaining = 0;
        const auto result = SQLGetData(
            hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining);
        EXPECT_EQ(i + 1 == expected_chunks.size()
                      ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO,
                  result);
        EXPECT_EQ(expected_remaining[i], remaining);
        EXPECT_EQ(expected_chunks[i], chunk);
        assembled += chunk;

        if (result == SQL_SUCCESS_WITH_INFO) {
            SQLCHAR sqlstate[6]{};
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
                SQL_HANDLE_STMT, hstmt, 1, sqlstate,
                nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("01004", reinterpret_cast<char*>(sqlstate));
        }
    }
    EXPECT_EQ("abcdefghijklmnopqrstuvwxyz", assembled);

    char exhausted[6] = "keep";
    SQLLEN exhausted_length = 99;
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 1, SQL_C_CHAR, exhausted, sizeof(exhausted),
        &exhausted_length));
    EXPECT_STREQ("keep", exhausted);
    EXPECT_EQ(99, exhausted_length);
}

TEST_F(BindColIntegrationTest, GetDataRetrievesWideTextInWholeCodePoints) {
    const std::string expected = "A\xf0\x9f\x99\x82" "BC";
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 'A\xf0\x9f\x99\x82" "BC'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    std::string assembled;
    SQLRETURN result = SQL_SUCCESS_WITH_INFO;
    for (int call = 0; call < 4 && result == SQL_SUCCESS_WITH_INFO; ++call) {
        SQLWCHAR chunk[3]{};
        SQLLEN remaining_bytes = 0;
        result = SQLGetData(hstmt, 1, SQL_C_WCHAR, chunk, sizeof(chunk),
                            &remaining_bytes);
        ASSERT_TRUE(result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO);
        std::size_t chunk_units = 0;
        while (chunk_units < 2 && chunk[chunk_units] != 0) ++chunk_units;
        const auto converted = rs::odbc::wide_to_utf8(
            std::span<const SQLWCHAR>(chunk, chunk_units));
        ASSERT_TRUE(converted.has_value());
        assembled += *converted;
        EXPECT_GT(remaining_bytes, 0);
    }
    EXPECT_EQ(SQL_SUCCESS, result);
    EXPECT_EQ(expected, assembled);
}

TEST_F(BindColIntegrationTest, GetDataOffsetsResetForEachFetchedRow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT value FROM (VALUES (1, 'abcdefgh'::text), "
                  "(2, 'ijklmnop'::text)) AS rows(id, value) ORDER BY id",
        SQL_NTS));

    char chunk[5]{};
    SQLLEN remaining = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining));
    EXPECT_STREQ("abcd", chunk);
    EXPECT_EQ(8, remaining);

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining));
    EXPECT_STREQ("ijkl", chunk);
    EXPECT_EQ(8, remaining);
}

// Test mixed binding (some bound, some unbound)
TEST_F(BindColIntegrationTest, MixedBinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Bound' as col1, 'Unbound' as col2, 999 as col3", SQL_NTS));
    
    char bound_val[256];
    SQLINTEGER int_val;
    SQLLEN bound_len, int_len;
    
    // Bind only columns 1 and 3, leave column 2 unbound
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, bound_val, sizeof(bound_val), &bound_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_SLONG, &int_val, 0, &int_len));
    
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // Bound columns should be populated
    EXPECT_STREQ("Bound", bound_val);
    EXPECT_EQ(999, int_val);
    
    // Unbound column should still be accessible via SQLGetData
    char unbound_val[256];
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_CHAR, unbound_val, sizeof(unbound_val), nullptr));
    EXPECT_STREQ("Unbound", unbound_val);
}

// Test column binding with multiple rows
TEST_F(BindColIntegrationTest, MultipleRowBinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT generate_series(1,3) as num, 'Row' || generate_series(1,3) as text", SQL_NTS));
    
    SQLINTEGER num_val;
    char text_val[256];
    SQLLEN num_len, text_len;
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SLONG, &num_val, 0, &num_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_CHAR, text_val, sizeof(text_val), &text_len));
    
    // Fetch multiple rows
    std::vector<std::pair<int, std::string>> results;
    
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        results.emplace_back(num_val, std::string(text_val));
    }
    
    // Verify all rows were fetched with bound data
    ASSERT_EQ(3, results.size());
    EXPECT_EQ(1, results[0].first);
    EXPECT_EQ("Row1", results[0].second);
    EXPECT_EQ(2, results[1].first);
    EXPECT_EQ("Row2", results[1].second);
    EXPECT_EQ(3, results[2].first);
    EXPECT_EQ("Row3", results[2].second);
}

// Test column binding with data type conversions
TEST_F(BindColIntegrationTest, DataTypeConversions) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 123 as int_as_str, '456' as str_as_int, 78.9 as double_as_int", SQL_NTS));
    
    char int_as_str[256];
    SQLINTEGER str_as_int;
    SQLINTEGER double_as_int;
    SQLLEN len1, len2, len3;
    
    // Bind with type conversions
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, int_as_str, sizeof(int_as_str), &len1));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_SLONG, &str_as_int, 0, &len2));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_SLONG, &double_as_int, 0, &len3));
    
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // Verify conversions
    EXPECT_STREQ("123", int_as_str);
    EXPECT_EQ(456, str_as_int);
    EXPECT_EQ(78, double_as_int);  // Truncated
}

TEST_F(BindColIntegrationTest, MetadataDrivenDefaultConversions) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 42::smallint, 1234567890123::bigint, "
                  "1.25::real, 3.5::double precision, true, "
                  "DATE '2024-02-29', TIME '23:45:30', "
                  "TIMESTAMP '2024-02-29 12:34:56.123456'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLSMALLINT small_value = 0;
    SQLBIGINT big_value = 0;
    SQLREAL real_value = 0;
    SQLDOUBLE double_value = 0;
    SQLCHAR bool_value = 0;
    SQL_DATE_STRUCT date_value{};
    SQL_TIME_STRUCT time_value{};
    SQL_TIMESTAMP_STRUCT timestamp_value{};
    SQLLEN indicator = 0;

    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_DEFAULT,
        &small_value, 0, &indicator));
    EXPECT_EQ(42, small_value);
    EXPECT_EQ(sizeof(SQLSMALLINT), indicator);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_DEFAULT,
        &big_value, 0, &indicator));
    EXPECT_EQ(1234567890123LL, big_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_DEFAULT,
        &real_value, 0, &indicator));
    EXPECT_FLOAT_EQ(1.25f, real_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_DEFAULT,
        &double_value, 0, &indicator));
    EXPECT_DOUBLE_EQ(3.5, double_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, SQL_C_DEFAULT,
        &bool_value, 0, &indicator));
    EXPECT_EQ(1, bool_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_DEFAULT,
        &date_value, 0, &indicator));
    EXPECT_EQ(2024, date_value.year);
    EXPECT_EQ(2, date_value.month);
    EXPECT_EQ(29, date_value.day);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_DEFAULT,
        &time_value, 0, &indicator));
    EXPECT_EQ(23, time_value.hour);
    EXPECT_EQ(45, time_value.minute);
    EXPECT_EQ(30, time_value.second);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 8, SQL_C_DEFAULT,
        &timestamp_value, 0, &indicator));
    EXPECT_EQ(2024, timestamp_value.year);
    EXPECT_EQ(12, timestamp_value.hour);
    EXPECT_EQ(123456000u, timestamp_value.fraction);
}

TEST_F(BindColIntegrationTest, ByteaUsesBinaryDefaultAndSupportsChunks) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT decode('00017fff', 'hex')", SQL_NTS));

    SQLSMALLINT sql_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &sql_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_VARBINARY, sql_type);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    unsigned char first[2]{};
    unsigned char second[2]{};
    SQLLEN indicator = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_DEFAULT, first, sizeof(first), &indicator));
    EXPECT_EQ(4, indicator);
    EXPECT_EQ(0x00, first[0]);
    EXPECT_EQ(0x01, first[1]);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_DEFAULT, second, sizeof(second), &indicator));
    EXPECT_EQ(2, indicator);
    EXPECT_EQ(0x7f, second[0]);
    EXPECT_EQ(0xff, second[1]);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 1, SQL_C_DEFAULT, second, sizeof(second), &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT decode('10203040', 'hex')", SQL_NTS));
    unsigned char bound[4]{};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_DEFAULT, bound, sizeof(bound), &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(4, indicator);
    EXPECT_EQ(0x10, bound[0]);
    EXPECT_EQ(0x20, bound[1]);
    EXPECT_EQ(0x30, bound[2]);
    EXPECT_EQ(0x40, bound[3]);
}

TEST_F(BindColIntegrationTest, BoundColumnsResolveSqlCDefault) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 42::integer, DATE '2024-02-29'", SQL_NTS));

    SQLINTEGER integer_value = 0;
    SQL_DATE_STRUCT date_value{};
    SQLLEN integer_indicator = 0;
    SQLLEN date_indicator = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_DEFAULT,
        &integer_value, 0, &integer_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_DEFAULT,
        &date_value, 0, &date_indicator));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(42, integer_value);
    EXPECT_EQ(sizeof(SQLINTEGER), integer_indicator);
    EXPECT_EQ(2024, date_value.year);
    EXPECT_EQ(2, date_value.month);
    EXPECT_EQ(29, date_value.day);
    EXPECT_EQ(sizeof(SQL_DATE_STRUCT), date_indicator);
}

TEST_F(BindColIntegrationTest, GetDataValidatesAndResolvesTargetTypes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 42::integer, 43::integer, 44::integer",
        SQL_NTS));
    char bound[16]{};
    SQLLEN bound_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, bound, sizeof(bound), &bound_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char value[16]{};
    SQLLEN value_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_ARD_TYPE, value, sizeof(value), &value_length));
    EXPECT_STREQ("42", value);
    EXPECT_EQ(2, value_length);

    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 2, 12345, value, sizeof(value), &value_length));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY003", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 3, SQL_C_BINARY, value, sizeof(value), &value_length));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, ConversionFailuresUseSpecificSqlstates) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'not-an-integer'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLINTEGER integer_value = 0;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &integer_value, 0, nullptr));
    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(sqlstate));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'binary-not-supported'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    unsigned char binary_value[32]{};
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_BINARY, binary_value, sizeof(binary_value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(sqlstate));
}

// Test rebinding columns
TEST_F(BindColIntegrationTest, ColumnRebinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Test' as col1, 789 as col2", SQL_NTS));
    
    // First binding
    char str_val1[256];
    SQLLEN len1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, str_val1, sizeof(str_val1), &len1));
    
    // Rebind same column to different buffer
    char str_val2[256];
    SQLLEN len2;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, str_val2, sizeof(str_val2), &len2));
    
    SQLINTEGER int_val;
    SQLLEN int_len;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_SLONG, &int_val, 0, &int_len));
    
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // Second buffer should be populated (rebinding worked)
    EXPECT_STREQ("Test", str_val2);
    EXPECT_EQ(789, int_val);
}

// Test error conditions
TEST_F(BindColIntegrationTest, ErrorConditions) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Test' as col1", SQL_NTS));
    
    char buffer[256];
    SQLLEN len;
    
    // Invalid column number
    EXPECT_EQ(SQL_ERROR, SQLBindCol(hstmt, 0, SQL_C_CHAR, buffer, sizeof(buffer), &len));
    EXPECT_EQ(SQL_ERROR, SQLBindCol(hstmt, 999, SQL_C_CHAR, buffer, sizeof(buffer), &len));

    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_ERROR, SQLBindCol(
        hstmt, 1, 12345, buffer, sizeof(buffer), &len));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY003", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, buffer, -1, &len));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY090", reinterpret_cast<char*>(state));
    
    // Valid binding should still work
    EXPECT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &len));
}

TEST_F(BindColIntegrationTest, NegativeTests) {
    // Test SQLBindCol with invalid handle
    char buffer[256];
    SQLLEN len;
    SQLRETURN ret = SQLBindCol(nullptr, 1, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
    
    // Test SQLBindCol with invalid column number
    ret = SQLBindCol(hstmt, 0, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Verify diagnostic is set
    SQLCHAR sqlstate[6], message[256];
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("07009", (char*)sqlstate);
    EXPECT_TRUE(strstr((char*)message, "column number") != nullptr);
    
    // Test SQLFetch without execution
    SQLHSTMT new_stmt;
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &new_stmt);
    ret = SQLFetch(new_stmt);
    EXPECT_EQ(SQL_ERROR, ret);
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, new_stmt, 1, sqlstate,
                        nullptr, message, sizeof(message), nullptr);
    ASSERT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("HY010", reinterpret_cast<char*>(sqlstate));
    SQLFreeHandle(SQL_HANDLE_STMT, new_stmt);
    
    // Test SQLGetData without execution
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &new_stmt);
    ret = SQLGetData(new_stmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Verify diagnostic
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, new_stmt, 1, sqlstate, nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("24000", (char*)sqlstate);
    SQLFreeHandle(SQL_HANDLE_STMT, new_stmt);
    
    // Test SQLGetData with invalid column
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    ret = SQLGetData(hstmt, 0, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
    
    ret = SQLGetData(hstmt, 999, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
}
