#include <gtest/gtest.h>
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include "tests/test_time_helpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <clocale>
#include <ctime>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>

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

TEST_F(BindColIntegrationTest, WideBoundColumnHandlesUnalignedBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'AB'::text", SQL_NTS));

    alignas(SQLWCHAR) std::array<std::byte, 1 + 3 * sizeof(SQLWCHAR)> storage{};
    void* output = storage.data() + 1;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_WCHAR, output, 3 * sizeof(SQLWCHAR), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(2 * static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);

    SQLWCHAR units[3]{};
    std::memcpy(units, output, sizeof(units));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), units[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('B'), units[1]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), units[2]);
}

TEST_F(BindColIntegrationTest, NumericBoundColumnsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 42::integer, 1.5::double precision", SQL_NTS));

    alignas(SQLINTEGER)
        std::array<std::byte, 1 + sizeof(SQLINTEGER)> integer_bytes{};
    alignas(SQLDOUBLE)
        std::array<std::byte, 1 + sizeof(SQLDOUBLE)> double_bytes{};
    SQLLEN integer_length = -1;
    SQLLEN double_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SLONG,
        integer_bytes.data() + 1, sizeof(SQLINTEGER), &integer_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_DOUBLE,
        double_bytes.data() + 1, sizeof(SQLDOUBLE), &double_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)), integer_length);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLDOUBLE)), double_length);

    SQLINTEGER integer = 0;
    SQLDOUBLE floating = 0;
    std::memcpy(&integer, integer_bytes.data() + 1, sizeof(integer));
    std::memcpy(&floating, double_bytes.data() + 1, sizeof(floating));
    EXPECT_EQ(42, integer);
    EXPECT_DOUBLE_EQ(1.5, floating);
}

TEST_F(BindColIntegrationTest, DateTimeBoundColumnsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56', "
                  "TIMESTAMP '2024-02-29 12:34:56'", SQL_NTS));

    alignas(SQL_DATE_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_DATE_STRUCT)> date_bytes{};
    alignas(SQL_TIME_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_TIME_STRUCT)> time_bytes{};
    alignas(SQL_TIMESTAMP_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_TIMESTAMP_STRUCT)> timestamp_bytes{};
    SQLLEN lengths[3]{-1, -1, -1};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_DATE,
        date_bytes.data() + 1, sizeof(SQL_DATE_STRUCT), &lengths[0]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_TIME,
        time_bytes.data() + 1, sizeof(SQL_TIME_STRUCT), &lengths[1]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_TIMESTAMP,
        timestamp_bytes.data() + 1, sizeof(SQL_TIMESTAMP_STRUCT), &lengths[2]));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)), lengths[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIME_STRUCT)), lengths[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT)), lengths[2]);
    SQL_DATE_STRUCT date{};
    SQL_TIME_STRUCT time{};
    SQL_TIMESTAMP_STRUCT timestamp{};
    std::memcpy(&date, date_bytes.data() + 1, sizeof(date));
    std::memcpy(&time, time_bytes.data() + 1, sizeof(time));
    std::memcpy(&timestamp, timestamp_bytes.data() + 1, sizeof(timestamp));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(34, time.minute);
    EXPECT_EQ(56, time.second);
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
}

TEST_F(BindColIntegrationTest, Odbc3TemporalBoundColumns) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56', "
                  "TIMESTAMP '2024-02-29 12:34:56.123456'", SQL_NTS));

    SQL_DATE_STRUCT date{};
    SQL_TIME_STRUCT time{};
    SQL_TIMESTAMP_STRUCT timestamp{};
    SQLLEN lengths[3]{-1, -1, -1};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &lengths[0]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_TYPE_TIME,
        &time, sizeof(time), &lengths[1]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), &lengths[2]));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), lengths[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(time)), lengths[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), lengths[2]);
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(34, time.minute);
    EXPECT_EQ(56, time.second);
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
    EXPECT_EQ(123456000u, timestamp.fraction);
}

TEST_F(BindColIntegrationTest, Odbc3TemporalGetDataAndInvalidValues) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56', "
                  "TIMESTAMP '2024-02-29 12:34:56.123456', "
                  "'2024-02-30'::text, '25:00:00'::text, 'bad'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_DATE_STRUCT date{};
    SQL_TIME_STRUCT time{};
    SQL_TIMESTAMP_STRUCT timestamp{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), length);
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(29, date.day);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_TYPE_TIME,
        &time, sizeof(time), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(time)), length);
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(56, time.second);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), length);
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(123456000u, timestamp.fraction);

    SQLCHAR state[6]{};
    date.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 4, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(73, date.year);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));

    time.hour = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 5, SQL_C_TYPE_TIME,
        &time, sizeof(time), &length));
    EXPECT_EQ(73, time.hour);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));

    timestamp.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 6, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(73, timestamp.year);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
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

TEST_F(BindColIntegrationTest, FailedBoundConversionKeepsSeparateIndicator) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'not-a-date'::text", SQL_NTS));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &descriptor));
    SQL_DATE_STRUCT date{4242, 4, 2};
    SQLLEN octet_length = 91;
    SQLLEN indicator = 92;
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescRec(descriptor, 1,
        SQL_C_TYPE_DATE, 0, sizeof(date), 0, 0,
        &date, &octet_length, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_ROW_DESC, descriptor, 0));

    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4242, date.year);
    EXPECT_EQ(91, octet_length);
    EXPECT_EQ(92, indicator);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), octet_length);
    EXPECT_EQ(0, indicator);

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

TEST_F(BindColIntegrationTest, RowStatusOutputsHandleUnalignedBuffers) {
    alignas(SQLULEN) std::array<std::byte, 1 + sizeof(SQLULEN)> fetched{};
    alignas(SQLUSMALLINT)
    std::array<std::byte, 1 + sizeof(SQLUSMALLINT)> status{};
    auto* fetched_output = reinterpret_cast<SQLULEN*>(fetched.data() + 1);
    auto* status_output = reinterpret_cast<SQLUSMALLINT*>(status.data() + 1);
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROWS_FETCHED_PTR, fetched_output, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROW_STATUS_PTR, status_output, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT value FROM (VALUES (1, 'abcdef'::text), "
            "(2, 'xy'::text), (3, NULL::text)) rows(position, value) "
            "ORDER BY position",
        SQL_NTS));

    char value[4]{};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, value, sizeof(value), nullptr));
    const auto expect_fetch = [&](SQLRETURN expected_result,
                                  SQLULEN expected_count,
                                  SQLUSMALLINT expected_status) {
        fetched.fill(std::byte{0x5a});
        status.fill(std::byte{0x5a});
        EXPECT_EQ(expected_result, SQLFetch(hstmt));
        SQLULEN copied_count = 0;
        SQLUSMALLINT copied_status = 0;
        std::memcpy(&copied_count, fetched.data() + 1,
                    sizeof(copied_count));
        std::memcpy(&copied_status, status.data() + 1,
                    sizeof(copied_status));
        EXPECT_EQ(expected_count, copied_count);
        EXPECT_EQ(expected_status, copied_status);
    };
    expect_fetch(SQL_SUCCESS_WITH_INFO, 1, SQL_ROW_SUCCESS_WITH_INFO);
    EXPECT_STREQ("abc", value);
    expect_fetch(SQL_SUCCESS, 1, SQL_ROW_SUCCESS);
    EXPECT_STREQ("xy", value);
    expect_fetch(SQL_ERROR, 1, SQL_ROW_ERROR);
    expect_fetch(SQL_NO_DATA, 0, SQL_ROW_NOROW);
}

TEST_F(BindColIntegrationTest, LengthIndicatorsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text, 'abcd'::text", SQL_NTS));

    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> null_bound{};
    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> text_bound{};
    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> get_data{};
    auto* null_indicator = reinterpret_cast<SQLLEN*>(null_bound.data() + 1);
    auto* text_indicator = reinterpret_cast<SQLLEN*>(text_bound.data() + 1);
    auto* data_indicator = reinterpret_cast<SQLLEN*>(get_data.data() + 1);
    const auto read_indicator = [](const auto& storage) {
        SQLLEN value = 0;
        std::memcpy(&value, storage.data() + 1, sizeof(value));
        return value;
    };

    get_data.fill(std::byte{0x5a});
    char buffer[8]{};
    const auto previous_output = get_data;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), data_indicator));
    EXPECT_EQ(previous_output, get_data);

    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), null_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 2, SQL_C_CHAR, buffer, sizeof(buffer), text_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NULL_DATA, read_indicator(null_bound));
    EXPECT_EQ(4, read_indicator(text_bound));
    EXPECT_STREQ("abcd", buffer);

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), data_indicator));
    EXPECT_EQ(SQL_NULL_DATA, read_indicator(get_data));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, buffer, sizeof(buffer), data_indicator));
    EXPECT_EQ(4, read_indicator(get_data));
    EXPECT_STREQ("abcd", buffer);
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

TEST_F(BindColIntegrationTest, GetDataHonorsNarrowBufferBoundaries) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT ''::text, 'abc'::text, 'abc'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char value[4] = {'x', 'x', 'x', 'x'};
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, value, 0, &length));
    EXPECT_EQ('x', value[0]);
    EXPECT_EQ(0, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, value, 1, &length));
    EXPECT_EQ('\0', value[0]);
    EXPECT_EQ(0, length);

    value[0] = 'x';
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 2, SQL_C_CHAR, value, 1, &length));
    EXPECT_EQ('\0', value[0]);
    EXPECT_EQ(3, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, value, sizeof(value), &length));
    EXPECT_STREQ("abc", value);
    EXPECT_EQ(3, length);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_CHAR, value, 3, &length));
    EXPECT_STREQ("ab", value);
    EXPECT_EQ(3, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_CHAR, value, 3, &length));
    EXPECT_STREQ("c", value);
    EXPECT_EQ(1, length);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 3, SQL_C_CHAR, value, sizeof(value), &length));
}

TEST_F(BindColIntegrationTest, GetDataHonorsWideBufferBoundaries) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT ''::text, 'A'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLWCHAR value[2]{static_cast<SQLWCHAR>('x'),
                      static_cast<SQLWCHAR>('x')};
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_WCHAR, value, sizeof(SQLWCHAR) - 1, &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), value[0]);
    EXPECT_EQ(0, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_WCHAR, value, sizeof(SQLWCHAR), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>(0), value[0]);
    EXPECT_EQ(0, length);

    value[0] = static_cast<SQLWCHAR>('x');
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 2, SQL_C_WCHAR, value, sizeof(SQLWCHAR), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>(0), value[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_WCHAR, value, sizeof(value), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), value[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), value[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
}

TEST_F(BindColIntegrationTest, BoundCharacterBuffersRequireTerminatorSpace) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT ''::text, ''::text", SQL_NTS));

    char narrow = 'x';
    SQLWCHAR wide = static_cast<SQLWCHAR>('x');
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, &narrow, 0, &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 2, SQL_C_WCHAR, &wide, sizeof(SQLWCHAR) - 1, &wide_length));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_EQ('x', narrow);
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide);
    EXPECT_EQ(0, narrow_length);
    EXPECT_EQ(0, wide_length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, GetDataHonorsBinaryBufferBoundaries) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT decode('', 'hex'), decode('aa', 'hex')",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    unsigned char value = 0x55;
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_BINARY, &value, 0, &length));
    EXPECT_EQ(0x55, value);
    EXPECT_EQ(0, length);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 1, SQL_C_BINARY, &value, 0, &length));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 2, SQL_C_BINARY, &value, 0, &length));
    EXPECT_EQ(0x55, value);
    EXPECT_EQ(1, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_BINARY, &value, 1, &length));
    EXPECT_EQ(0xaa, value);
    EXPECT_EQ(1, length);
}

TEST_F(BindColIntegrationTest,
       GetDataFormatsBinaryAsCompleteHexPairs) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT decode('00ff7f', 'hex'), "
                  "decode('00ff7f', 'hex'), decode('', 'hex')",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLLEN remaining = -1;
    SQLCHAR state[6]{};

    char narrow_tiny[2]{'x', 'x'};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &remaining));
    EXPECT_EQ(0, narrow_tiny[0]);
    EXPECT_EQ(6, remaining);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    SQLLEN expected_remaining = 6;
    for (const auto* expected : {"00", "ff", "7f"}) {
        char chunk[4]{};
        const auto result = SQLGetData(hstmt, 1, SQL_C_CHAR,
            chunk, sizeof(chunk), &remaining);
        EXPECT_EQ(std::string_view(expected) == "7f"
                      ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO, result);
        EXPECT_EQ(expected_remaining, remaining);
        EXPECT_STREQ(expected, chunk);
        expected_remaining -= 2;
    }
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &remaining));

    SQLWCHAR wide_tiny[2]{'x', 'x'};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &remaining));
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide_tiny[0]);
    EXPECT_EQ(static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)), remaining);
    expected_remaining = 6 * sizeof(SQLWCHAR);
    for (const auto* expected : {"00", "ff", "7f"}) {
        SQLWCHAR chunk[4]{};
        const auto result = SQLGetData(hstmt, 2, SQL_C_WCHAR,
            chunk, sizeof(chunk), &remaining);
        EXPECT_EQ(std::string_view(expected) == "7f"
                      ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO, result);
        EXPECT_EQ(expected_remaining, remaining);
        EXPECT_EQ(static_cast<SQLWCHAR>(expected[0]), chunk[0]);
        EXPECT_EQ(static_cast<SQLWCHAR>(expected[1]), chunk[1]);
        EXPECT_EQ(static_cast<SQLWCHAR>(0), chunk[2]);
        expected_remaining -= 2 * sizeof(SQLWCHAR);
    }
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &remaining));

    char empty[2]{'x', 'x'};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_CHAR,
        empty, sizeof(empty), &remaining));
    EXPECT_EQ(0, empty[0]);
    EXPECT_EQ(0, remaining);
}

TEST_F(BindColIntegrationTest,
       BoundBinaryTextPreservesCompleteHexPairs) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT decode('00ff7f', 'hex'), "
                  "decode('00ff7f', 'hex'), decode('', 'hex'), "
                  "decode('00ff7f', 'hex')",
        SQL_NTS));

    char narrow[4]{'x', 'x', 'x', 'x'};
    SQLWCHAR wide[4]{'x', 'x', 'x', 'x'};
    char empty[2]{'x', 'x'};
    char tiny[2]{'x', 'x'};
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = -1;
    SQLLEN empty_length = -1;
    SQLLEN tiny_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &wide_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_CHAR,
        empty, sizeof(empty), &empty_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 4, SQL_C_CHAR,
        tiny, sizeof(tiny), &tiny_length));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_STREQ("00", narrow);
    EXPECT_EQ(6, narrow_length);
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide[1]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[2]);
    EXPECT_EQ(static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)), wide_length);
    EXPECT_EQ(0, empty[0]);
    EXPECT_EQ(0, empty_length);
    EXPECT_EQ(0, tiny[0]);
    EXPECT_EQ(6, tiny_length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BoundBinaryTextDecodesLegacyByteaOutput) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SET bytea_output = 'escape'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT decode('00ff7f', 'hex'), "
                  "decode('00ff7f', 'hex')", SQL_NTS));

    char narrow[7]{};
    SQLWCHAR wide[7]{};
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &wide_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_STREQ("00ff7f", narrow);
    EXPECT_EQ(6, narrow_length);
    for (std::size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(static_cast<SQLWCHAR>(narrow[i]), wide[i]);
    }
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[6]);
    EXPECT_EQ(static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)), wide_length);
}

TEST_F(BindColIntegrationTest, BitToCharacterGetDataUsesZeroAndOne) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true, false, true, false, true, false",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLLEN length = -1;
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        char value[2]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_CHAR,
            value, sizeof(value), &length));
        EXPECT_STREQ(column == 1 ? "1" : "0", value);
        EXPECT_EQ(1, length);
    }
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        SQLWCHAR value[2]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_WCHAR,
            value, sizeof(value), &length));
        EXPECT_EQ(static_cast<SQLWCHAR>(column == 3 ? '1' : '0'), value[0]);
        EXPECT_EQ(static_cast<SQLWCHAR>(0), value[1]);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
    }

    char narrow_tiny = 'x';
    length = 77;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 5, SQL_C_CHAR,
        &narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny);
    EXPECT_EQ(77, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow_retry[2]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, SQL_C_CHAR,
        narrow_retry, sizeof(narrow_retry), &length));
    EXPECT_STREQ("1", narrow_retry);

    SQLWCHAR wide_tiny = 'x';
    length = 78;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 6, SQL_C_WCHAR,
        &wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny);
    EXPECT_EQ(78, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide_retry[2]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_WCHAR,
        wide_retry, sizeof(wide_retry), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide_retry[0]);
}

TEST_F(BindColIntegrationTest, BitToCharacterBoundColumnsUseZeroAndOne) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true, false", SQL_NTS));
    char narrow[2]{};
    SQLWCHAR wide[2]{};
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &wide_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_STREQ("1", narrow);
    EXPECT_EQ(1, narrow_length);
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), wide_length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true", SQL_NTS));
    char tiny = 'x';
    SQLLEN tiny_length = 79;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        &tiny, sizeof(tiny), &tiny_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', tiny);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT false", SQL_NTS));
    SQLWCHAR wide_tiny = 'x';
    SQLLEN wide_tiny_length = 80;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_WCHAR,
        &wide_tiny, sizeof(wide_tiny), &wide_tiny_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BitToBinaryGetDataReturnsOneByte) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true, false, true", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLLEN length = -1;
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        SQLCHAR value = 0xff;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_BINARY,
            &value, sizeof(value), &length));
        EXPECT_EQ(column == 1 ? 1 : 0, value);
        EXPECT_EQ(1, length);
    }

    SQLCHAR tiny = 0x55;
    length = 81;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_BINARY,
        &tiny, 0, &length));
    EXPECT_EQ(0x55, tiny);
    EXPECT_EQ(81, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_BINARY,
        &tiny, sizeof(tiny), &length));
    EXPECT_EQ(1, tiny);
    EXPECT_EQ(1, length);
}

TEST_F(BindColIntegrationTest, BitToBinaryBoundColumnsReturnOneByte) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT false, true", SQL_NTS));
    SQLCHAR first = 0xff;
    SQLCHAR second = 0xff;
    SQLLEN first_length = -1;
    SQLLEN second_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BINARY,
        &first, sizeof(first), &first_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_BINARY,
        &second, sizeof(second), &second_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0, first);
    EXPECT_EQ(1, second);
    EXPECT_EQ(1, first_length);
    EXPECT_EQ(1, second_length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true", SQL_NTS));
    SQLCHAR tiny = 0x55;
    SQLLEN tiny_length = 82;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BINARY,
        &tiny, 0, &tiny_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(0x55, tiny);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BitToNumericGetDataUsesZeroAndOne) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true, true, true, true, true", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLSMALLINT short_value = -1;
    SQLINTEGER integer = -1;
    SQLBIGINT big = -1;
    SQLREAL real = -1;
    SQLDOUBLE double_value = -1;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SSHORT,
        &short_value, 0, &length));
    EXPECT_EQ(1, short_value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(short_value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_SLONG,
        &integer, 0, &length));
    EXPECT_EQ(1, integer);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_SBIGINT,
        &big, 0, &length));
    EXPECT_EQ(1, big);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(big)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_FLOAT,
        &real, 0, &length));
    EXPECT_FLOAT_EQ(1.0f, real);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(real)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, SQL_C_DOUBLE,
        &double_value, 0, &length));
    EXPECT_DOUBLE_EQ(1.0, double_value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(double_value)), length);
}

TEST_F(BindColIntegrationTest, SignedTinyintGetDataChecksRangeAndFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT -128::numeric, 127::numeric, -129::numeric, "
                  "128::numeric, 12.75::numeric, true, false", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLSCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(-128, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(127, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_STINYINT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(12, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(1, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(0, value);
}

TEST_F(BindColIntegrationTest, SignedTinyintBoundColumnHandlesNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 127::numeric UNION ALL SELECT NULL::numeric "
                  "ORDER BY 1 NULLS LAST", SQL_NTS));
    SQLSCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_STINYINT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(127, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(127, value);
    EXPECT_EQ(SQL_NULL_DATA, length);
}

TEST_F(BindColIntegrationTest, SignedTinyintBoundColumnRejectsOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 127::numeric UNION ALL SELECT 128::numeric "
                  "ORDER BY 1", SQL_NTS));
    SQLSCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_STINYINT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(127, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(127, value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, SignedTinyintRejectsTemporalResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-01-01', NULL::date", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLSCHAR value = 44;
    SQLLEN length = 91;
    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_STINYINT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }
    SQL_DATE_STRUCT date{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(2024, date.year);
}

TEST_F(BindColIntegrationTest, UnsignedTinyintGetDataChecksRangeAndFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 0::numeric, 255::numeric, -1::numeric, "
                  "256::numeric, 255.75::numeric, true, false", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(0, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(255, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_UTINYINT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(255, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(1, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(0, value);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'not-a-number'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    value = 44;
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(44, value);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
    char recovered[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        recovered, sizeof(recovered), nullptr));
    EXPECT_STREQ("not-a-number", recovered);
}

TEST_F(BindColIntegrationTest, UnsignedTinyintBoundColumnChecksNullAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (VALUES (1, 255::numeric), "
                  "(2, NULL::numeric), (3, 256::numeric)) AS v(ord, value) "
                  "ORDER BY ord", SQL_NTS));
    SQLCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_UTINYINT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(255, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(255, value);
    EXPECT_EQ(SQL_NULL_DATA, length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(255, value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, UnsignedTinyintRejectsTemporalResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-01-01', NULL::date", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLCHAR value = 44;
    SQLLEN length = 91;
    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_UTINYINT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }
}

TEST_F(BindColIntegrationTest, UnsignedShortGetDataChecksRangeAndFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 0::numeric, 65535::numeric, -1::numeric, "
                  "65536::numeric, 65535.75::numeric, true, false",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLUSMALLINT value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(0, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(65535, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_USHORT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(65535, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(1, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(0, value);
}

TEST_F(BindColIntegrationTest, UnsignedShortBoundColumnChecksNullAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (VALUES (1, 65535::numeric), "
                  "(2, NULL::numeric), (3, 65536::numeric)) AS v(ord, value) "
                  "ORDER BY ord", SQL_NTS));
    SQLUSMALLINT value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_USHORT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(65535, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(65535, value);
    EXPECT_EQ(SQL_NULL_DATA, length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(65535, value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, UnsignedWideTargetsRejectTemporalResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-01-01', NULL::date", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLUBIGINT value = 44;
    SQLLEN length = 91;
    SQLCHAR state[6]{};
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{SQL_C_USHORT, SQL_C_ULONG, SQL_C_UBIGINT}) {
        SCOPED_TRACE(target);
        for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
            EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, target,
                &value, 0, &length));
            EXPECT_EQ(44u, value);
            EXPECT_EQ(91, length);
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
        }
    }
}

TEST_F(BindColIntegrationTest, UnsignedLongGetDataChecksRangeAndFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 0::numeric, 4294967295::numeric, -1::numeric, "
                  "4294967296::numeric, 4294967295.75::numeric, true, false",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLUINTEGER value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(0u, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(4294967295u, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_ULONG,
            &value, 0, &length));
        EXPECT_EQ(44u, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(4294967295u, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(1u, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(0u, value);
}

TEST_F(BindColIntegrationTest, UnsignedLongBoundColumnChecksNullAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (VALUES (1, 4294967295::numeric), "
                  "(2, NULL::numeric), (3, 4294967296::numeric)) "
                  "AS v(ord, value) ORDER BY ord", SQL_NTS));
    SQLUINTEGER value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_ULONG,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(4294967295u, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(4294967295u, value);
    EXPECT_EQ(SQL_NULL_DATA, length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4294967295u, value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, UnsignedBigintGetDataChecksExactLimits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 0::numeric, 9223372036854775808::numeric, "
                  "18446744073709551615::numeric, -1::numeric, "
                  "18446744073709551616::numeric, "
                  "18446744073709551615.75::numeric, true, false, "
                  "'1.8446744073709551615e19'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLUBIGINT value = 44;
    SQLLEN length = 91;
    const auto maximum = std::numeric_limits<SQLUBIGINT>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(0u, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(static_cast<SQLUBIGINT>(1) << 63, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(maximum, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{4, 5}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_UBIGINT,
            &value, 0, &length));
        EXPECT_EQ(44u, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 6, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(maximum, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(1u, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 8, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(0u, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 9, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(maximum, value);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'not-a-number'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    value = 44;
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(44u, value);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
    char recovered[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        recovered, sizeof(recovered), nullptr));
    EXPECT_STREQ("not-a-number", recovered);
}

TEST_F(BindColIntegrationTest, UnsignedBigintBoundColumnChecksNullAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM "
                  "(VALUES (1, 18446744073709551615::numeric), "
                  "(2, NULL::numeric), "
                  "(3, 18446744073709551616::numeric)) AS v(ord, value) "
                  "ORDER BY ord", SQL_NTS));
    SQLUBIGINT value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_UBIGINT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::numeric_limits<SQLUBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::numeric_limits<SQLUBIGINT>::max(), value);
    EXPECT_EQ(SQL_NULL_DATA, length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(std::numeric_limits<SQLUBIGINT>::max(), value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BitToNumericBoundColumnsUseZero) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT false, false, false, false, false", SQL_NTS));
    SQLSMALLINT short_value = -1;
    SQLINTEGER integer = -1;
    SQLBIGINT big = -1;
    SQLREAL real = -1;
    SQLDOUBLE double_value = -1;
    SQLLEN lengths[5]{-1, -1, -1, -1, -1};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SSHORT,
        &short_value, 0, &lengths[0]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_SLONG,
        &integer, 0, &lengths[1]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_SBIGINT,
        &big, 0, &lengths[2]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 4, SQL_C_FLOAT,
        &real, 0, &lengths[3]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 5, SQL_C_DOUBLE,
        &double_value, 0, &lengths[4]));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0, short_value);
    EXPECT_EQ(0, integer);
    EXPECT_EQ(0, big);
    EXPECT_FLOAT_EQ(0.0f, real);
    EXPECT_DOUBLE_EQ(0.0, double_value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(short_value)), lengths[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer)), lengths[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(big)), lengths[2]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(real)), lengths[3]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(double_value)), lengths[4]);
}

TEST_F(BindColIntegrationTest, BitRejectsTemporalResultTargets) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{
             SQL_C_DATE, SQL_C_TYPE_DATE,
             SQL_C_TIME, SQL_C_TYPE_TIME,
             SQL_C_TIMESTAMP, SQL_C_TYPE_TIMESTAMP}) {
        SCOPED_TRACE(target);
        std::array<unsigned char, sizeof(SQL_TIMESTAMP_STRUCT)> output;
        output.fill(0x5a);
        SQLLEN length = 83;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, target,
            output.data(), static_cast<SQLLEN>(output.size()), &length));
        EXPECT_EQ(83, length);
        EXPECT_TRUE(std::all_of(output.begin(), output.end(),
            [](unsigned char byte) { return byte == 0x5a; }));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT false", SQL_NTS));
    SQL_DATE_STRUCT date{};
    date.year = 4242;
    SQLLEN length = 84;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4242, date.year);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BinaryRejectsNonCharacterScalarTargets) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT decode('01', 'hex')", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{
             SQL_C_BIT, SQL_C_SSHORT, SQL_C_SLONG, SQL_C_SBIGINT,
             SQL_C_FLOAT, SQL_C_DOUBLE, SQL_C_TYPE_DATE,
             SQL_C_TYPE_TIME, SQL_C_TYPE_TIMESTAMP}) {
        SCOPED_TRACE(target);
        std::array<unsigned char, sizeof(SQL_TIMESTAMP_STRUCT)> output;
        output.fill(0x5a);
        SQLLEN length = 85;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, target,
            output.data(), static_cast<SQLLEN>(output.size()), &length));
        EXPECT_EQ(85, length);
        EXPECT_TRUE(std::all_of(output.begin(), output.end(),
            [](unsigned char byte) { return byte == 0x5a; }));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT decode('01', 'hex')", SQL_NTS));
    SQLINTEGER number = 4242;
    SQLLEN length = 86;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SLONG,
        &number, sizeof(number), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4242, number);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, NumericRejectsTemporalResultTargets) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 42::integer, 1.5::double precision, "
                  "2.5::numeric", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2, 3}) {
        for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{
                 SQL_C_TYPE_DATE, SQL_C_TYPE_TIME,
                 SQL_C_TYPE_TIMESTAMP}) {
            SCOPED_TRACE(column);
            SCOPED_TRACE(target);
            std::array<unsigned char, sizeof(SQL_TIMESTAMP_STRUCT)> output;
            output.fill(0x5a);
            SQLLEN length = 87;
            EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, target,
                output.data(), static_cast<SQLLEN>(output.size()), &length));
            EXPECT_EQ(87, length);
            EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                [](unsigned char byte) { return byte == 0x5a; }));
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
        }
    }

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 42::integer", SQL_NTS));
    SQL_DATE_STRUCT date{};
    date.year = 4242;
    SQLLEN length = 88;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4242, date.year);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TemporalRejectsNumericResultTargets) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56', "
                  "TIMESTAMP '2024-02-29 12:34:56'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2, 3}) {
        for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{
                 SQL_C_BIT, SQL_C_SSHORT, SQL_C_SLONG, SQL_C_SBIGINT,
                 SQL_C_FLOAT, SQL_C_DOUBLE}) {
            SCOPED_TRACE(column);
            SCOPED_TRACE(target);
            std::array<unsigned char, sizeof(SQLDOUBLE)> output;
            output.fill(0x5a);
            SQLLEN length = 89;
            EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, target,
                output.data(), static_cast<SQLLEN>(output.size()), &length));
            EXPECT_EQ(89, length);
            EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                [](unsigned char byte) { return byte == 0x5a; }));
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
        }
    }

    char valid_date[11]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        valid_date, sizeof(valid_date), nullptr));
    EXPECT_STREQ("2024-02-29", valid_date);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIME '12:34:56'", SQL_NTS));
    SQLDOUBLE bound = 42.0;
    SQLLEN length = 90;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_DOUBLE,
        &bound, sizeof(bound), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_DOUBLE_EQ(42.0, bound);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TemporalCrossConversionsRespectTypeMatrix) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{SQL_C_TIME, SQL_C_TYPE_TIME}) {
        SQL_TIME_STRUCT output{42, 42, 42};
        SQLLEN length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, target,
            &output, sizeof(output), &length));
        EXPECT_EQ(42, output.hour);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{SQL_C_DATE, SQL_C_TYPE_DATE}) {
        SQL_DATE_STRUCT output{4242, 42, 42};
        SQLLEN length = 92;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, target,
            &output, sizeof(output), &length));
        EXPECT_EQ(4242, output.year);
        EXPECT_EQ(92, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }

    SQL_TIMESTAMP_STRUCT timestamp{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), nullptr));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(0, timestamp.hour);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), nullptr));
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29'", SQL_NTS));
    SQL_TIME_STRUCT bound{42, 42, 42};
    SQLLEN length = 93;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_TIME,
        &bound, sizeof(bound), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(42, bound.hour);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, NullValueStillChecksResultConversion) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT NULL::date, NULL::date, DATE '2024-02-29'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLINTEGER number = 42;
    SQLLEN length = 91;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &number, sizeof(number), &length));
    EXPECT_EQ(42, number);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));

    SQL_DATE_STRUCT date{4242, 4, 2};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(4242, date.year);
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_SLONG,
        &number, sizeof(number), &length));
    EXPECT_EQ(42, number);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT NULL::date", SQL_NTS));
    length = 93;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SLONG,
        &number, sizeof(number), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(42, number);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, NumericGetDataDefaultsAndNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric, NULL::numeric", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 91;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_NUMERIC,
        &numeric, 0, &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);
    EXPECT_EQ(38, numeric.precision);
    EXPECT_EQ(0, numeric.scale);
    EXPECT_EQ(1, numeric.sign);
    EXPECT_EQ(12, numeric.val[0]);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_NUMERIC,
        &numeric, 0, &length));
    std::memset(&numeric, 0x5a, sizeof(numeric));
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_NUMERIC,
        &numeric, 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
}

TEST_F(BindColIntegrationTest, NumericArdScaleAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric, 12345.67::numeric", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_C_NUMERIC)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 92;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_ARD_TYPE,
        &numeric, 0, &length));
    EXPECT_EQ(5, numeric.precision);
    EXPECT_EQ(2, numeric.scale);
    EXPECT_EQ(0xd2, numeric.val[0]);
    EXPECT_EQ(0x04, numeric.val[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_C_NUMERIC)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    std::memset(&numeric, 0x5a, sizeof(numeric));
    length = 93;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_ARD_TYPE,
        &numeric, 0, &length));
    EXPECT_EQ(93, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BoundNumericUsesArdScaleAndPreservesNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric UNION ALL SELECT NULL::numeric",
        SQL_NTS));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 92;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_NUMERIC,
        &numeric, sizeof(numeric), &length));
    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    // Descriptor metadata changes unbind the data pointer; restore it last.
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_DATA_PTR,
        &numeric, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0xd2, numeric.val[0]);
    EXPECT_EQ(0x04, numeric.val[1]);
    EXPECT_EQ(5, numeric.precision);
    EXPECT_EQ(2, numeric.scale);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);
    std::memset(&numeric, 0x5a, sizeof(numeric));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
}

TEST_F(BindColIntegrationTest, BoundNumericOverflowPreservesOutput) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 12345.67::numeric", SQL_NTS));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 92;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_NUMERIC,
        &numeric, sizeof(numeric), &length));
    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_DATA_PTR,
        &numeric, 0));
    std::memset(&numeric, 0x5a, sizeof(numeric));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(92, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, CharacterToBinaryReturnsRawUtf8Bytes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'AéZ'::text, ''::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR first[2]{0x55, 0x55};
    SQLCHAR second[2]{0x55, 0x55};
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_BINARY,
        first, sizeof(first), &length));
    EXPECT_EQ(4, length);
    EXPECT_EQ(0x41, first[0]);
    EXPECT_EQ(0xc3, first[1]);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY,
        second, sizeof(second), &length));
    EXPECT_EQ(2, length);
    EXPECT_EQ(0xa9, second[0]);
    EXPECT_EQ(0x5a, second[1]);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_BINARY,
        second, sizeof(second), &length));

    SQLCHAR empty = 0x55;
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_BINARY,
        &empty, 0, &length));
    EXPECT_EQ(0x55, empty);
    EXPECT_EQ(0, length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'AéZ'::text, 'AéZ'::text", SQL_NTS));
    SQLCHAR full[4]{};
    SQLCHAR truncated[2]{0x55, 0x55};
    SQLLEN full_length = -1;
    SQLLEN truncated_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BINARY,
        full, sizeof(full), &full_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_BINARY,
        truncated, sizeof(truncated), &truncated_length));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_EQ(4, full_length);
    EXPECT_EQ(4, truncated_length);
    EXPECT_EQ(0x41, full[0]);
    EXPECT_EQ(0xc3, full[1]);
    EXPECT_EQ(0xa9, full[2]);
    EXPECT_EQ(0x5a, full[3]);
    EXPECT_EQ(0x41, truncated[0]);
    EXPECT_EQ(0xc3, truncated[1]);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, IntegerTextNeedsWholeValueBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1234::integer, 1234::integer", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 89;
    char narrow_tiny[4]{'x', 'x', 'x', 'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(89, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[5]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("1234", narrow);
    EXPECT_EQ(4, length);

    SQLWCHAR wide_tiny[4]{'x', 'x', 'x', 'x'};
    length = 90;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(90, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[5]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('4'), wide[3]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[4]);
    EXPECT_EQ(static_cast<SQLLEN>(4 * sizeof(SQLWCHAR)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1234::integer, 1234::integer", SQL_NTS));
    char bound_narrow[5]{};
    SQLWCHAR bound_wide[4]{'x', 'x', 'x', 'x'};
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_narrow, sizeof(bound_narrow), &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_WCHAR,
        bound_wide, sizeof(bound_wide), &wide_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_STREQ("1234", bound_narrow);
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), bound_wide[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT -1234::integer", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char signed_tiny[5]{'x', 'x', 'x', 'x', 'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        signed_tiny, sizeof(signed_tiny), &length));
    EXPECT_EQ('x', signed_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char signed_full[6]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        signed_full, sizeof(signed_full), &length));
    EXPECT_STREQ("-1234", signed_full);
}

TEST_F(BindColIntegrationTest, DecimalTextCanTruncateOnlyFractionalDigits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1234.56::numeric, 1234.56::numeric", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 93;
    char narrow_tiny[4]{'x', 'x', 'x', 'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[5]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("1234", narrow);
    EXPECT_EQ(7, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    char fractional[2]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        fractional, sizeof(fractional), &length));
    EXPECT_STREQ(".", fractional);
    EXPECT_EQ(3, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("56", narrow);
    EXPECT_EQ(2, length);

    SQLWCHAR wide_tiny[4]{'x', 'x', 'x', 'x'};
    length = 94;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(94, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[5]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('4'), wide[3]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[4]);
    EXPECT_EQ(static_cast<SQLLEN>(7 * sizeof(SQLWCHAR)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    SQLWCHAR fractional_wide[2]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        fractional_wide, sizeof(fractional_wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('.'), fractional_wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), fractional_wide[1]);
    EXPECT_EQ(static_cast<SQLLEN>(3 * sizeof(SQLWCHAR)), length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('5'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('6'), wide[1]);
    EXPECT_EQ(static_cast<SQLLEN>(2 * sizeof(SQLWCHAR)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT -1234.56::numeric", SQL_NTS));
    char bound_tiny[5]{'x', 'x', 'x', 'x', 'x'};
    SQLLEN bound_length = 95;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, ApproximateNumericTextPreservesMagnitude) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1234.56::double precision, "
                  "1.23e+30::double precision, 1.23e-30::double precision",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 91;
    char tiny[4]{'x', 'x', 'x', 'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        tiny, sizeof(tiny), &length));
    EXPECT_EQ('x', tiny[0]);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char whole[5]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        whole, sizeof(whole), &length));
    EXPECT_STREQ("1234", whole);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    char fraction[8]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        fraction, sizeof(fraction), &length));
    EXPECT_STREQ(".56", fraction);

    char exponent_tiny[5]{'x', 'x', 'x', 'x', 'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_CHAR,
        exponent_tiny, sizeof(exponent_tiny), &length));
    EXPECT_EQ('x', exponent_tiny[0]);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char exponent[32]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_CHAR,
        exponent, sizeof(exponent), &length));
    EXPECT_STREQ("1.23e+30", exponent);

    SQLWCHAR exponent_wide_tiny[5]{'x', 'x', 'x', 'x', 'x'};
    length = 93;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_WCHAR,
        exponent_wide_tiny, sizeof(exponent_wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), exponent_wide_tiny[0]);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR exponent_wide[32]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_WCHAR,
        exponent_wide, sizeof(exponent_wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), exponent_wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('e'), exponent_wide[4]);
    EXPECT_EQ(static_cast<SQLWCHAR>('-'), exponent_wide[5]);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1.23e+30::double precision", SQL_NTS));
    char bound_tiny[6]{'x', 'x', 'x', 'x', 'x', 'x'};
    SQLLEN bound_length = 94;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, DateTextNeedsCompleteBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', DATE '2024-02-29'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 91;
    char narrow_tiny[10]{'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[11]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("2024-02-29", narrow);
    EXPECT_EQ(10, length);

    SQLWCHAR wide_tiny[10]{'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[11]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('2'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('9'), wide[9]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[10]);
    EXPECT_EQ(static_cast<SQLLEN>(10 * sizeof(SQLWCHAR)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29'", SQL_NTS));
    char bound_tiny[10]{'x'};
    SQLLEN bound_length = 93;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TimeTextKeepsSecondsAndZone) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIME '12:34:56.78', TIME '12:34:56.78', "
                  "TIMETZ '12:34:56+02'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 91;
    char narrow_tiny[8]{'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[9]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("12:34:56", narrow);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    char fraction[4]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        fraction, sizeof(fraction), &length));
    EXPECT_STREQ(".78", fraction);

    SQLWCHAR wide_tiny[8]{'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[9]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('6'), wide[7]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[8]);
    EXPECT_EQ(static_cast<SQLLEN>(11 * sizeof(SQLWCHAR)), length);

    char zone_tiny[9]{'x'};
    length = 93;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_CHAR,
        zone_tiny, sizeof(zone_tiny), &length));
    EXPECT_EQ('x', zone_tiny[0]);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char zone[20]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_CHAR,
        zone, sizeof(zone), &length));
    EXPECT_STREQ("12:34:56+02", zone);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIME '12:34:56'", SQL_NTS));
    char bound_tiny[8]{'x'};
    SQLLEN bound_length = 94;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TimestampTextKeepsSecondsAndZone) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SET TIME ZONE 'UTC'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIMESTAMP '2024-02-29 12:34:56.78', "
                  "TIMESTAMP '2024-02-29 12:34:56.78', "
                  "TIMESTAMPTZ '2024-02-29 12:34:56.78+00'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 91;
    char narrow_tiny[19]{'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[20]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("2024-02-29 12:34:56", narrow);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    char fraction[4]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        fraction, sizeof(fraction), &length));
    EXPECT_STREQ(".78", fraction);

    SQLWCHAR wide_tiny[19]{'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[20]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('2'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('6'), wide[18]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[19]);
    EXPECT_EQ(static_cast<SQLLEN>(22 * sizeof(SQLWCHAR)), length);

    char zone_tiny[20]{'x'};
    length = 93;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_CHAR,
        zone_tiny, sizeof(zone_tiny), &length));
    EXPECT_EQ('x', zone_tiny[0]);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char zone[40]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_CHAR,
        zone, sizeof(zone), &length));
    EXPECT_STREQ("2024-02-29 12:34:56.78+00", zone);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIMESTAMP '2024-02-29 12:34:56'", SQL_NTS));
    char bound_tiny[19]{'x'};
    SQLLEN bound_length = 94;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, FailedGetDataDoesNotDiscardPartialOffset) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'abcdef'::text, 'other'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char chunk[4]{};
    SQLLEN length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &length));
    EXPECT_STREQ("abc", chunk);

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 2, SQL_C_CHAR, nullptr, 0, &length));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY009", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &length));
    EXPECT_STREQ("def", chunk);
    EXPECT_EQ(3, length);
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

TEST_F(BindColIntegrationTest, RequestsUtf8ClientEncoding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT current_setting('client_encoding'), chr(233)::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char encoding[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, encoding, sizeof(encoding), nullptr));
    EXPECT_STREQ("UTF8", encoding);

    SQLWCHAR wide[2]{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_WCHAR, wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>(0x00e9), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
}

TEST_F(BindColIntegrationTest, RequestsIsoDateStyle) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT current_setting('DateStyle'), DATE '2024-02-29', "
            "TIMESTAMP '2024-02-29 12:34:56'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char date_style[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, date_style, sizeof(date_style), nullptr));
    EXPECT_TRUE(std::string_view(date_style).starts_with("ISO"));

    SQL_DATE_STRUCT date{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_DATE, &date, sizeof(date), nullptr));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);

    SQL_TIMESTAMP_STRUCT timestamp{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp),
        nullptr));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
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

TEST_F(BindColIntegrationTest, SwitchingColumnsInvalidatesPartialOffset) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'abcdefgh'::text, 'other'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char chunk[5]{};
    SQLLEN remaining = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining));
    EXPECT_STREQ("abcd", chunk);
    EXPECT_EQ(8, remaining);

    char other[8]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, other, sizeof(other), &remaining));
    EXPECT_STREQ("other", other);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining));
    EXPECT_STREQ("abcd", chunk);
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
    
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    
    // Verify conversions
    EXPECT_STREQ("123", int_as_str);
    EXPECT_EQ(456, str_as_int);
    EXPECT_EQ(78, double_as_int);
}

TEST_F(BindColIntegrationTest, BigintTextConversionAtExactLimits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT '9223372036854775807'::text, "
            "'-9223372036854775808'::text, "
            "'9223372036854775807.9'::text, "
            "'9223372036854775808'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLBIGINT value = 0;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    value = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BigintScientificTextConversionAtExactLimits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT '9.223372036854775807e18'::text, "
            "'-9.223372036854775808e18'::text, "
            "'9.2233720368547758079e18'::text, "
            "'9.223372036854775808e18'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLBIGINT value = 0;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    value = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, FloatingConversionRejectsUnderflowToZero) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT '1e-50'::text, '-1e-50'::text, "
            "'1e-400'::text, '-1e-400'::text, "
            "'1e-30'::text, '1e-300'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    for (const SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        SQLREAL value = 7.0f;
        SQLLEN length = 8;
        EXPECT_EQ(SQL_ERROR, SQLGetData(
            hstmt, column, SQL_C_FLOAT, &value, sizeof(value), &length));
        EXPECT_EQ(7.0f, value);
        EXPECT_EQ(8, length);
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    for (const SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        SQLDOUBLE value = 7.0;
        SQLLEN length = 8;
        EXPECT_EQ(SQL_ERROR, SQLGetData(
            hstmt, column, SQL_C_DOUBLE, &value, sizeof(value), &length));
        EXPECT_EQ(7.0, value);
        EXPECT_EQ(8, length);
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }

    SQLREAL small_float = 0;
    SQLDOUBLE small_double = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_FLOAT, &small_float, sizeof(small_float), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 6, SQL_C_DOUBLE, &small_double, sizeof(small_double), nullptr));
    EXPECT_GT(small_float, 0);
    EXPECT_GT(small_double, 0);
}

TEST_F(BindColIntegrationTest, FloatingSpecialValuesPreserveIeeeResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'NaN'::double precision, "
                  "'Infinity'::double precision, "
                  "'-Infinity'::real", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLDOUBLE nan_value = 7.0;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_DOUBLE,
        &nan_value, sizeof(nan_value), &length));
    EXPECT_TRUE(std::isnan(nan_value));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(nan_value)), length);

    SQLDOUBLE positive = 7.0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_DOUBLE,
        &positive, sizeof(positive), &length));
    EXPECT_TRUE(std::isinf(positive));
    EXPECT_GT(positive, 0);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(positive)), length);

    SQLREAL negative = 7.0f;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_FLOAT,
        &negative, sizeof(negative), &length));
    EXPECT_TRUE(std::isinf(negative));
    EXPECT_LT(negative, 0);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(negative)), length);

    SQLINTEGER integer = 73;
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_SLONG,
        &integer, sizeof(integer), &length));
    EXPECT_EQ(73, integer);
    EXPECT_EQ(92, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'Infinity'::double precision", SQL_NTS));
    SQLREAL bound = 0;
    SQLLEN bound_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_FLOAT,
        &bound, sizeof(bound), &bound_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_TRUE(std::isinf(bound));
    EXPECT_GT(bound, 0);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(bound)), bound_length);
}

TEST_F(BindColIntegrationTest, BoundFloatingUnderflowPreservesOutput) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT '1e-50'::text", SQL_NTS));
    SQLREAL value = 7.0f;
    SQLLEN length = 8;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_FLOAT, &value, sizeof(value), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(7.0f, value);
    EXPECT_EQ(8, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, FloatingConversionIgnoresProcessLocale) {
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string original = current ? current : "C";
    const char* german_locale = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
#ifdef _WIN32
    if (!german_locale) {
        german_locale = std::setlocale(LC_NUMERIC, "German_Germany.1252");
    }
#endif
    if (!german_locale) GTEST_SKIP() << "German numeric locale unavailable";
    struct LocaleRestore {
        std::string name;
        ~LocaleRestore() { std::setlocale(LC_NUMERIC, name.c_str()); }
    } restore{original};
    if (std::localeconv()->decimal_point[0] != ',') {
        GTEST_SKIP() << "Selected locale does not use a decimal comma";
    }

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT 1.5::numeric, '1.5e2'::text, '1,5'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLREAL real_value = 0;
    SQLDOUBLE double_value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_FLOAT, &real_value, sizeof(real_value), nullptr));
    EXPECT_FLOAT_EQ(1.5f, real_value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_DOUBLE, &double_value, sizeof(double_value), nullptr));
    EXPECT_DOUBLE_EQ(150.0, double_value);

    double_value = 73.0;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 3, SQL_C_DOUBLE, &double_value, sizeof(double_value), nullptr));
    EXPECT_DOUBLE_EQ(73.0, double_value);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BigintLeadingWhitespaceConversionAtLimits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT ' \t9223372036854775807'::text, "
            "' \t-9.223372036854775808e18'::text, "
            "' \t9.2233720368547758079e18'::text, "
            "' \t9.223372036854775808e18'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLBIGINT value = 0;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    value = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, CharacterConversionsIgnoreOuterWhitespace) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT ' 9223372036854775807 '::text, "
            "' 1.25e2 '::text, ' 2024-02-29 '::text, "
            "' 12:34:56 '::text, ' 2024-02-29 12:34:56 '::text, "
            "' 12:34:56 x'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLLEN length = -1;
    SQLBIGINT integer = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SBIGINT, &integer, sizeof(integer), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), integer);

    SQLDOUBLE floating = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_DOUBLE, &floating, sizeof(floating), &length));
    EXPECT_DOUBLE_EQ(125.0, floating);

    SQL_DATE_STRUCT date{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_DATE, &date, sizeof(date), &length));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);

    SQL_TIME_STRUCT time{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_TIME, &time, sizeof(time), &length));
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(34, time.minute);
    EXPECT_EQ(56, time.second);

    SQL_TIMESTAMP_STRUCT timestamp{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(12, timestamp.hour);

    time.hour = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 6, SQL_C_TIME, &time, sizeof(time), &length));
    EXPECT_EQ(73, time.hour);
    EXPECT_EQ(74, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TemporalFractionTruncationHasDiagnostic) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT TIME '12:34:56.123456', TIME '12:34:56', "
            "TIMESTAMP '2024-02-29 12:34:56.123456', "
            "'2024-02-29 12:34:56.1234567891'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_TIME_STRUCT time{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_TIME, &time, sizeof(time), &length));
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(time)), length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_TIME, &time, sizeof(time), &length));
    EXPECT_EQ(12, time.hour);

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_TIME, &time, sizeof(time), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    SQL_TIMESTAMP_STRUCT timestamp{};
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 4, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(123456789u, timestamp.fraction);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TimestampToDateReportsLostTime) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT TIMESTAMP '2024-02-29 00:00:00', "
            "TIMESTAMP '2024-02-29 12:34:56', "
            "'2024-02-29 00:00:00.0000000001'::text, "
            "'2024-02-30 12:00:00'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_DATE_STRUCT date{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_DATE, &date, sizeof(date), &length));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), length);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{2, 3}) {
        ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
            hstmt, column, SQL_C_DATE, &date, sizeof(date), &length));
        EXPECT_EQ(29, date.day);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    }

    date.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_DATE, &date, sizeof(date), &length));
    EXPECT_EQ(73, date.year);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, DateToTimestampZeroesTimeFields) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT DATE '2024-02-29', '2024-02-30'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_TIMESTAMP_STRUCT timestamp{};
    timestamp.hour = 73;
    timestamp.minute = 74;
    timestamp.second = 75;
    timestamp.fraction = 76;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(0, timestamp.hour);
    EXPECT_EQ(0, timestamp.minute);
    EXPECT_EQ(0, timestamp.second);
    EXPECT_EQ(0u, timestamp.fraction);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), length);

    timestamp.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 2, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(73, timestamp.year);
    EXPECT_EQ(74, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TimeToTimestampUsesCurrentLocalDate) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT TIME '12:34:56', '25:00:00'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    const auto before = odbcpp::test::local_calendar(std::time(nullptr));
    ASSERT_TRUE(before.has_value());

    SQL_TIMESTAMP_STRUCT timestamp{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));

    const auto after = odbcpp::test::local_calendar(std::time(nullptr));
    ASSERT_TRUE(after.has_value());
    const auto matches = [&](const std::tm& calendar) {
        return timestamp.year == calendar.tm_year + 1900 &&
            timestamp.month == calendar.tm_mon + 1 &&
            timestamp.day == calendar.tm_mday;
    };
    EXPECT_TRUE(matches(*before) || matches(*after));
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
    EXPECT_EQ(0u, timestamp.fraction);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), length);

    timestamp.hour = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 2, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(73, timestamp.hour);
    EXPECT_EQ(74, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, NumericTextToBitDiagnostics) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT '0.5'::text, '1.999999999999999999999'::text, true",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR bit = 73;
    SQLLEN length = -1;
    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
            hstmt, column, SQL_C_BIT, &bit, sizeof(bit), &length));
        EXPECT_EQ(column == 1 ? 0 : 1, bit);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(bit)), length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_BIT, &bit, sizeof(bit), &length));
    EXPECT_EQ(1, bit);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    struct Failure {
        const char* query;
        const char* state;
    };
    for (const Failure& failure : {
             Failure{"SELECT '-0.5'::text", "22003"},
             Failure{"SELECT '2'::text", "22003"},
             Failure{"SELECT '1.5x'::text", "22018"}}) {
        SCOPED_TRACE(failure.query);
        std::string query(failure.query);
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
            hstmt, reinterpret_cast<SQLCHAR*>(query.data()), SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        bit = 73;
        length = 74;
        EXPECT_EQ(SQL_ERROR, SQLGetData(
            hstmt, 1, SQL_C_BIT, &bit, sizeof(bit), &length));
        EXPECT_EQ(73, bit);
        EXPECT_EQ(74, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(failure.state, reinterpret_cast<char*>(state));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
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
        hstmt, (SQLCHAR*)"SELECT decode('01', 'hex')", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER binary_to_number = 42;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &binary_to_number,
        sizeof(binary_to_number), nullptr));
    EXPECT_EQ(42, binary_to_number);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(sqlstate));
}

TEST_F(BindColIntegrationTest, ConversionDiagnosticsDistinguishFailureKinds) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 32768::integer, 12.75::numeric, "
                  "'not-a-number'::text, '2025-02-29'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLSMALLINT number = 0;
    SQLINTEGER integer = 0;
    SQL_DATE_STRUCT date{};
    SQLCHAR state[6]{};

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_SSHORT, &number, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 2, SQL_C_SSHORT, &number, 0, nullptr));
    EXPECT_EQ(12, number);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 3, SQL_C_SLONG, &integer, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_DATE, &date, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
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
