#include <gtest/gtest.h>
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <thread>
#include <vector>

class PreparedStatementIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        
        // Connect to real database
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

TEST_F(PreparedStatementIntegrationTest, BinaryParameterRoundTripsAsBytea) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    unsigned char input[]{0x00, 0x01, 0x7f, 0xff};
    SQLLEN input_length = sizeof(input);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY, sizeof(input),
        0, input, sizeof(input), &input_length));
    SQLUSMALLINT param_status = SQL_PARAM_UNUSED;
    SQLULEN params_processed = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAM_STATUS_PTR, &param_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &params_processed, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    EXPECT_EQ(1u, params_processed);
    EXPECT_EQ(SQL_PARAM_SUCCESS, param_status);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    unsigned char output[sizeof(input)]{};
    SQLLEN output_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_BINARY, output, sizeof(output), &output_length));
    EXPECT_EQ(sizeof(input), static_cast<std::size_t>(output_length));
    EXPECT_EQ(0, std::memcmp(input, output, sizeof(input)));
}

TEST_F(PreparedStatementIntegrationTest,
       FloatingParametersRetainRoundTripPrecision) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::double precision, ?::real, "
                  "?::double precision, ?::double precision", SQL_NTS));
    SQLDOUBLE precise = 123.45678901234567;
    SQLREAL single = 0.123456789f;
    SQLDOUBLE tiny = 0.0000001;
    SQLDOUBLE negative_zero = -0.0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &precise, sizeof(precise), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_REAL, 0, 0, &single, sizeof(single), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 3, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &tiny, sizeof(tiny), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 4, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &negative_zero,
        sizeof(negative_zero), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLDOUBLE precise_result = 0;
    SQLREAL single_result = 0;
    SQLDOUBLE tiny_result = 0;
    SQLDOUBLE zero_result = 1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_DOUBLE,
        &precise_result, sizeof(precise_result), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_FLOAT,
        &single_result, sizeof(single_result), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_DOUBLE,
        &tiny_result, sizeof(tiny_result), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_DOUBLE,
        &zero_result, sizeof(zero_result), nullptr));
    EXPECT_EQ(precise, precise_result);
    EXPECT_EQ(single, single_result);
    EXPECT_EQ(tiny, tiny_result);
    EXPECT_EQ(0.0, zero_result);
    EXPECT_TRUE(std::signbit(zero_result));
}

TEST_F(PreparedStatementIntegrationTest,
       FloatingSpecialParametersRetainIeeeValues) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::double precision, ?::double precision, "
                  "?::real", SQL_NTS));
    SQLDOUBLE nan = std::numeric_limits<SQLDOUBLE>::quiet_NaN();
    SQLDOUBLE positive = std::numeric_limits<SQLDOUBLE>::infinity();
    SQLREAL negative = -std::numeric_limits<SQLREAL>::infinity();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &nan, sizeof(nan), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &positive, sizeof(positive), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 3, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_REAL, 0, 0, &negative, sizeof(negative), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLDOUBLE nan_result = 0;
    SQLDOUBLE positive_result = 0;
    SQLREAL negative_result = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_DOUBLE,
        &nan_result, sizeof(nan_result), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_DOUBLE,
        &positive_result, sizeof(positive_result), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_FLOAT,
        &negative_result, sizeof(negative_result), nullptr));
    EXPECT_TRUE(std::isnan(nan_result));
    EXPECT_TRUE(std::isinf(positive_result));
    EXPECT_GT(positive_result, 0);
    EXPECT_TRUE(std::isinf(negative_result));
    EXPECT_LT(negative_result, 0);
}

TEST_F(PreparedStatementIntegrationTest,
       FloatingCharacterParametersHonorDeclaredLength) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::text", SQL_NTS));
    SQLDOUBLE precise = 123.456;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_VARCHAR, 6, 0, &precise, sizeof(precise), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22001", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_VARCHAR, 7, 0, &precise, sizeof(precise), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char output[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        output, sizeof(output), nullptr));
    EXPECT_STREQ("123.456", output);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLREAL negative = -123.5f;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_WVARCHAR, 5, 0, &negative,
        sizeof(negative), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22001", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_WVARCHAR, 6, 0, &negative,
        sizeof(negative), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        output, sizeof(output), nullptr));
    EXPECT_STREQ("-123.5", output);
}

TEST_F(PreparedStatementIntegrationTest,
       IntegerCharacterParametersHonorDeclaredLength) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::text", SQL_NTS));
    SQLINTEGER negative = -12345;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_VARCHAR, 5, 0, &negative,
        sizeof(negative), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22001", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_VARCHAR, 6, 0, &negative,
        sizeof(negative), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char output[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        output, sizeof(output), nullptr));
    EXPECT_STREQ("-12345", output);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLBIGINT maximum = std::numeric_limits<SQLBIGINT>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SBIGINT, SQL_WVARCHAR, 18, 0, &maximum,
        sizeof(maximum), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22001", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SBIGINT, SQL_WVARCHAR, 19, 0, &maximum,
        sizeof(maximum), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        output, sizeof(output), nullptr));
    EXPECT_STREQ("9223372036854775807", output);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLSMALLINT small = 123;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SSHORT, SQL_CHAR, 2, 0, &small, sizeof(small), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22001", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SSHORT, SQL_CHAR, 3, 0, &small, sizeof(small), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        output, sizeof(output), nullptr));
    EXPECT_STREQ("123", output);
}

TEST_F(PreparedStatementIntegrationTest,
       FloatingInputToSqlIntegerTruncatesAndChecksRange) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLDOUBLE value = 123.75;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_INTEGER, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER result = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(123, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = -123.75;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(-123, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = -2147483648.0;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(std::numeric_limits<SQLINTEGER>::min(), result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    for (SQLDOUBLE invalid : {2147483648.0,
                              std::numeric_limits<SQLDOUBLE>::infinity()}) {
        value = invalid;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }

    value = 42.0;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(42, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLREAL single = 12.75f;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_INTEGER, 0, 0, &single, sizeof(single), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(12, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    single = 2147483648.0f;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       FloatingInputToSqlSmallintTruncatesAndChecksRange) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::smallint", SQL_NTS));
    SQLDOUBLE value = 123.75;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_SMALLINT, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLSMALLINT result = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SSHORT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(123, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = -32768.0;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SSHORT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(std::numeric_limits<SQLSMALLINT>::min(), result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = 32768.0;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    SQLREAL single = -12.75f;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_SMALLINT, 0, 0, &single,
        sizeof(single), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SSHORT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(-12, result);
}

TEST_F(PreparedStatementIntegrationTest,
       FloatingInputToSqlBigintTruncatesAndChecksRange) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::bigint", SQL_NTS));
    SQLDOUBLE value = 123.75;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_BIGINT, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLBIGINT result = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(123, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    const SQLDOUBLE upper = std::ldexp(1.0, 63);
    value = -upper;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = std::nextafter(upper, 0.0);
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(static_cast<SQLBIGINT>(value), result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    for (SQLDOUBLE invalid : {upper,
                              std::nextafter(-upper, -upper * 2.0)}) {
        value = invalid;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }

    SQLREAL single = -12.75f;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_BIGINT, 0, 0, &single, sizeof(single), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(-12, result);
}

TEST_F(PreparedStatementIntegrationTest,
       FloatingInputToSqlBitValidatesValue) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::boolean", SQL_NTS));
    SQLDOUBLE value = 0.0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_BIT, 0, 0, &value, sizeof(value), nullptr));
    for (const SQLDOUBLE valid : {0.0, 1.0}) {
        value = valid;
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLCHAR result = 9;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(static_cast<SQLCHAR>(valid), result);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }

    struct InvalidValue {
        SQLDOUBLE number;
        const char* state;
    };
    for (const InvalidValue invalid : {
             InvalidValue{0.5, "22001"},
             InvalidValue{-0.5, "22003"},
             InvalidValue{2.0, "22003"},
             InvalidValue{std::numeric_limits<SQLDOUBLE>::infinity(),
                          "22003"},
             InvalidValue{std::numeric_limits<SQLDOUBLE>::quiet_NaN(),
                          "22003"}}) {
        value = invalid.number;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(invalid.state, reinterpret_cast<char*>(state));
    }

    SQLREAL single = 1.0f;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_BIT, 0, 0, &single, sizeof(single), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLCHAR result = 9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(1, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    single = 0.5f;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22001", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       SignedIntegerInputToSqlBitValidatesValue) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::boolean", SQL_NTS));
    SQLINTEGER integer = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_BIT, 0, 0, &integer, sizeof(integer), nullptr));
    const auto expect_bit = [&](SQLCHAR expected) {
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLCHAR result = 9;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(expected, result);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    const auto expect_out_of_range = [&] {
        ASSERT_EQ(SQL_ERROR, SQLExecute(hstmt));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    };

    expect_bit(0);
    integer = 1;
    expect_bit(1);
    integer = -1;
    expect_out_of_range();
    integer = 2;
    expect_out_of_range();
    integer = 0;
    expect_bit(0);

    SQLSMALLINT short_value = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SSHORT, SQL_BIT, 0, 0, &short_value, sizeof(short_value),
        nullptr));
    expect_out_of_range();
    short_value = 1;
    expect_bit(1);

    SQLBIGINT big_value = std::numeric_limits<SQLBIGINT>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SBIGINT, SQL_BIT, 0, 0, &big_value, sizeof(big_value),
        nullptr));
    expect_out_of_range();
    big_value = 0;
    expect_bit(0);
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterInputToSqlBitValidatesNumericLiteral) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::boolean", SQL_NTS));
    struct Case {
        const char* text;
        const char* state;
        SQLCHAR bit;
    };
    for (const Case test : {
             Case{"0", nullptr, 0},
             Case{" +1.0 ", nullptr, 1},
             Case{"-0e10", nullptr, 0},
             Case{"\t0\t", nullptr, 0},
             Case{"0.5", "22001", 0},
             Case{"2e-1", "22001", 0},
             Case{"1.0000000000000000001", "22001", 0},
             Case{"1e-1000", "22001", 0},
             Case{"-1", "22003", 0},
             Case{"2", "22003", 0},
             Case{"1e1000", "22003", 0},
             Case{"true", "22018", 0},
             Case{"1e", "22018", 0},
             Case{"1junk", "22018", 0}}) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_CHAR, SQL_BIT, 0, 0, (SQLPOINTER)test.text, 0, nullptr));
        const SQLRETURN result = SQLExecute(hstmt);
        if (test.state) {
            ASSERT_EQ(SQL_ERROR, result) << test.text;
            SQLCHAR state[6]{};
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ(test.state, reinterpret_cast<char*>(state));
        } else {
            ASSERT_EQ(SQL_SUCCESS, result) << test.text;
            ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
            SQLCHAR bit = 9;
            ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
                &bit, sizeof(bit), nullptr));
            EXPECT_EQ(test.bit, bit);
            ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        }
    }

    SQLWCHAR wide_valid[]{' ', '+', '1', '.', '0', ' ', 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_BIT, 0, 0, wide_valid, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLCHAR bit = 9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
        &bit, sizeof(bit), nullptr));
    EXPECT_EQ(1, bit);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLWCHAR wide_invalid[]{'t', 'r', 'u', 'e', 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_BIT, 0, 0, wide_invalid, 0, nullptr));
    ASSERT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedBigIntInputValidatesIntegerRange) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::bigint", SQL_NTS));
    SQLUBIGINT value = static_cast<SQLUBIGINT>(
        std::numeric_limits<SQLBIGINT>::max());
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_UBIGINT, SQL_BIGINT, 0, 0, &value, sizeof(value), nullptr));
    const auto expect_value = [&](SQLBIGINT expected) {
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLBIGINT result = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(expected, result);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_value(std::numeric_limits<SQLBIGINT>::max());
    value = static_cast<SQLUBIGINT>(
        std::numeric_limits<SQLBIGINT>::max()) + 1;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    value = 0;
    expect_value(0);
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedBigIntInputValidatesNarrowIntegerRanges) {
    struct Target {
        const char* sql;
        SQLSMALLINT type;
        SQLUBIGINT maximum;
    };
    for (const Target target : {
             Target{"SELECT ?::smallint", SQL_SMALLINT,
                    static_cast<SQLUBIGINT>(
                        std::numeric_limits<SQLSMALLINT>::max())},
             Target{"SELECT ?::integer", SQL_INTEGER,
                    static_cast<SQLUBIGINT>(
                        std::numeric_limits<SQLINTEGER>::max())}}) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
            (SQLCHAR*)target.sql, SQL_NTS));
        SQLUBIGINT value = target.maximum;
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_UBIGINT, target.type, 0, 0, &value, sizeof(value), nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLBIGINT result = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(target.maximum, static_cast<SQLUBIGINT>(result));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

        value = target.maximum + 1;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
        ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, hstmt));
        hstmt = nullptr;
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt));
    }
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedBigIntInputValidatesSqlBit) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::boolean", SQL_NTS));
    SQLUBIGINT value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_UBIGINT, SQL_BIT, 0, 0, &value, sizeof(value), nullptr));
    for (SQLUBIGINT valid : {SQLUBIGINT{0}, SQLUBIGINT{1}}) {
        value = valid;
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLCHAR result = 9;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(valid, result);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
    for (SQLUBIGINT invalid : {SQLUBIGINT{2},
                               std::numeric_limits<SQLUBIGINT>::max()}) {
        value = invalid;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    value = 1;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedBigIntInputHonorsCharacterLength) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::text", SQL_NTS));
    SQLUBIGINT value = std::numeric_limits<SQLUBIGINT>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_UBIGINT, SQL_VARCHAR, 20, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLCHAR result[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        result, sizeof(result), nullptr));
    EXPECT_STREQ("18446744073709551615", reinterpret_cast<char*>(result));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_UBIGINT, SQL_VARCHAR, 19, 0, &value, sizeof(value), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22001", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedLongInputValidatesIntegerRange) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLUINTEGER value = static_cast<SQLUINTEGER>(
        std::numeric_limits<SQLINTEGER>::max());
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_ULONG, SQL_INTEGER, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER result = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(std::numeric_limits<SQLINTEGER>::max(), result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = static_cast<SQLUINTEGER>(
        std::numeric_limits<SQLINTEGER>::max()) + 1;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedLongInputPreservesFullWidth) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::bigint", SQL_NTS));
    SQLUINTEGER value = std::numeric_limits<SQLUINTEGER>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_ULONG, SQL_BIGINT, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLBIGINT result = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(static_cast<SQLBIGINT>(value), result);
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedLongInputValidatesSqlBit) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::boolean", SQL_NTS));
    SQLUINTEGER value = 1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_ULONG, SQL_BIT, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLCHAR result = 9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(1, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = 2;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedShortInputValidatesIntegerRange) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::smallint", SQL_NTS));
    SQLUSMALLINT value = static_cast<SQLUSMALLINT>(
        std::numeric_limits<SQLSMALLINT>::max());
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_USHORT, SQL_SMALLINT, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLSMALLINT result = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SSHORT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(std::numeric_limits<SQLSMALLINT>::max(), result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = static_cast<SQLUSMALLINT>(
        std::numeric_limits<SQLSMALLINT>::max()) + 1;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedShortAndTinyIntPreserveFullWidth) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::bigint", SQL_NTS));
    SQLUSMALLINT short_value = std::numeric_limits<SQLUSMALLINT>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_USHORT, SQL_BIGINT, 0, 0, &short_value,
        sizeof(short_value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLBIGINT result = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(65535, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR tiny_value = std::numeric_limits<SQLCHAR>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_UTINYINT, SQL_BIGINT, 0, 0, &tiny_value,
        sizeof(tiny_value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SBIGINT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(255, result);
}

TEST_F(PreparedStatementIntegrationTest,
       UnsignedTinyIntInputValidatesSqlBit) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::boolean", SQL_NTS));
    SQLCHAR value = 1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_UTINYINT, SQL_BIT, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLCHAR result = 9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(1, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    value = 2;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
}

TEST_F(PreparedStatementIntegrationTest,
       SignedTinyIntInputPreservesSignedRange) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::smallint", SQL_NTS));
    SQLSCHAR value = std::numeric_limits<SQLSCHAR>::min();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_STINYINT, SQL_SMALLINT, 0, 0, &value, sizeof(value), nullptr));
    for (SQLSCHAR expected : {std::numeric_limits<SQLSCHAR>::min(),
                              std::numeric_limits<SQLSCHAR>::max()}) {
        value = expected;
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLSMALLINT result = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SSHORT,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(expected, result);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
}

TEST_F(PreparedStatementIntegrationTest,
       SignedTinyIntInputValidatesSqlBit) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::boolean", SQL_NTS));
    SQLSCHAR value = 1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_STINYINT, SQL_BIT, 0, 0, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLCHAR result = 9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BIT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(1, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    for (SQLSCHAR invalid : {SQLSCHAR{-1}, SQLSCHAR{2}}) {
        value = invalid;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
}

TEST_F(PreparedStatementIntegrationTest,
       BinaryParameterHonorsDeclaredSqlLength) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    unsigned char input[]{0x00, 0x01, 0x7f, 0xff};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_BINARY, SQL_VARBINARY, sizeof(input), 0,
        input, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    unsigned char output[sizeof(input)]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY,
        output, sizeof(output), nullptr));
    EXPECT_EQ(0, std::memcmp(input, output, sizeof(input)));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR state[6]{};
    for (const SQLULEN short_width : {
             static_cast<SQLULEN>(sizeof(input) - 1),
             static_cast<SQLULEN>(0)}) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_BINARY, SQL_VARBINARY, short_width, 0,
            input, sizeof(input), nullptr));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            continue;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22001", reinterpret_cast<char*>(state));
    }
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterBinaryParameterDecodesHexAndValidatesLength) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN input_length = SQL_NTS;
    const auto expect_bytes = [&](SQLSMALLINT c_type, SQLPOINTER input,
                                  SQLULEN sql_length,
                                  std::initializer_list<unsigned char> bytes) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, SQL_VARBINARY, sql_length, 0,
            input, 0, &input_length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        unsigned char output[8]{};
        SQLLEN output_length = -1;
        const auto get_result = SQLGetData(hstmt, 1, SQL_C_BINARY,
            output, sizeof(output), &output_length);
        EXPECT_EQ(SQL_SUCCESS, get_result);
        if (get_result == SQL_SUCCESS) {
            EXPECT_EQ(static_cast<SQLLEN>(bytes.size()), output_length);
            EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), output));
        }
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    char odd_hex[] = "00fFa";
    expect_bytes(SQL_C_CHAR, odd_hex, 2, {0x00, 0xff});
    char explicit_hex[] = "00117fff";
    input_length = 6;
    expect_bytes(SQL_C_CHAR, explicit_hex, 3, {0x00, 0x11, 0x7f});
    input_length = SQL_NTS;
    SQLWCHAR wide_hex[] = {'0', '0', 'f', 'F', 'a', 0};
    expect_bytes(SQL_C_WCHAR, wide_hex, 2, {0x00, 0xff});
    SQLWCHAR wide_odd_nonhex[] = {'0', '0', 0x00e9, 0};
    expect_bytes(SQL_C_WCHAR, wide_odd_nonhex, 1, {0x00});
    std::vector<SQLWCHAR> wide_odd_supplementary{'0', '0'};
    if constexpr (sizeof(SQLWCHAR) == 2) {
        wide_odd_supplementary.push_back(static_cast<SQLWCHAR>(0xd83d));
        wide_odd_supplementary.push_back(static_cast<SQLWCHAR>(0xde00));
    } else {
        wide_odd_supplementary.push_back(static_cast<SQLWCHAR>(0x1f600));
    }
    wide_odd_supplementary.push_back(0);
    expect_bytes(SQL_C_WCHAR, wide_odd_supplementary.data(), 1, {0x00});

    const auto expect_error = [&](SQLSMALLINT c_type, SQLPOINTER input,
                                  SQLULEN sql_length,
                                  const char* expected_state) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, SQL_VARBINARY, sql_length, 0,
            input, 0, &input_length));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            return;
        }
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected_state, reinterpret_cast<char*>(state));
    };
    char invalid_hex[] = "0G";
    expect_error(SQL_C_CHAR, invalid_hex, 1, "22018");
    char oversized_hex[] = "00117f";
    expect_error(SQL_C_CHAR, oversized_hex, 2, "22001");
    char narrow_nonhex[] = "00\xc3\xa9";
    expect_error(SQL_C_CHAR, narrow_nonhex, 2, "22018");
    SQLWCHAR wide_invalid_pair[] = {'0', 0x00e9, 0};
    expect_error(SQL_C_WCHAR, wide_invalid_pair, 1, "22018");
}

TEST_F(PreparedStatementIntegrationTest,
       ExplicitZeroCharacterParameterLengthIsEmpty) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT octet_length(?::text)", SQL_NTS));
    SQLLEN input_length = 0;
    const auto expect_length = [&](SQLSMALLINT c_type, SQLSMALLINT sql_type,
                                   SQLPOINTER input, SQLINTEGER expected) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, sql_type, 0, 0, input, 0, &input_length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLINTEGER output = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
            &output, sizeof(output), nullptr));
        EXPECT_EQ(expected, output);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    char narrow[] = "secret";
    expect_length(SQL_C_CHAR, SQL_VARCHAR, narrow, 0);
    SQLWCHAR wide[] = {'w', 'i', 'd', 'e', 0};
    expect_length(SQL_C_WCHAR, SQL_WVARCHAR, wide, 0);
    input_length = SQL_NTS;
    expect_length(SQL_C_CHAR, SQL_VARCHAR, narrow, 6);
    expect_length(SQL_C_WCHAR, SQL_WVARCHAR, wide, 4);
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterParameterHonorsDeclaredNarrowSqlByteLength) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN input_length = SQL_NTS;
    SQLCHAR state[6]{};
    char ascii[] = "abc";
    for (const auto sql_type : {SQL_CHAR, SQL_VARCHAR, SQL_LONGVARCHAR}) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1,
            SQL_PARAM_INPUT, SQL_C_CHAR, sql_type, 3, 0,
            ascii, 0, &input_length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        char output[8]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
            output, sizeof(output), nullptr));
        EXPECT_STREQ("abc", output);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1,
            SQL_PARAM_INPUT, SQL_C_CHAR, sql_type, 2, 0,
            ascii, 0, &input_length));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            continue;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22001", reinterpret_cast<char*>(state));
    }

    SQLWCHAR wide[] = {0x00e9, 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_VARCHAR, 2, 0, wide, 0, &input_length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char output[8]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        output, sizeof(output), nullptr));
    EXPECT_STREQ("\xc3\xa9", output);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_VARCHAR, 1, 0, wide, 0, &input_length));
    const auto result = SQLExecute(hstmt);
    EXPECT_EQ(SQL_ERROR, result);
    if (result != SQL_ERROR) {
        SQLCloseCursor(hstmt);
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22001", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterParameterHonorsDeclaredWideSqlCharacterLength) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN input_length = SQL_NTS;
    SQLCHAR state[6]{};
    char utf8[] = "\xc3\xa9x";
    SQLWCHAR wide[] = {0x00e9, 'x', 0};
    const auto expect_state = [&](SQLRETURN result, const char* expected) {
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            return;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(state));
    };
    for (const auto sql_type : {SQL_WCHAR, SQL_WVARCHAR,
                                SQL_WLONGVARCHAR}) {
        for (const auto c_type : {SQL_C_CHAR, SQL_C_WCHAR}) {
            SQLPOINTER input = c_type == SQL_C_CHAR
                ? static_cast<SQLPOINTER>(utf8)
                : static_cast<SQLPOINTER>(wide);
            ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1,
                SQL_PARAM_INPUT, c_type, sql_type, 2, 0,
                input, 0, &input_length));
            ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
            ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
            char output[8]{};
            ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
                output, sizeof(output), nullptr));
            EXPECT_STREQ(utf8, output);
            ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

            ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1,
                SQL_PARAM_INPUT, c_type, sql_type, 1, 0,
                input, 0, &input_length));
            expect_state(SQLExecute(hstmt), "22001");
        }
    }
    char invalid_utf8[] = {'\xff', 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_CHAR, SQL_WVARCHAR, 1, 0,
        invalid_utf8, 0, &input_length));
    expect_state(SQLExecute(hstmt), "22018");
}

TEST_F(PreparedStatementIntegrationTest,
       WideSqlCharacterLengthHandlesSupplementaryInput) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN input_length = SQL_NTS;
    char utf8[] = "\xf0\x9f\x98\x80x";
    std::vector<SQLWCHAR> wide;
    if constexpr (sizeof(SQLWCHAR) == 2) {
        wide = {static_cast<SQLWCHAR>(0xd83d),
                static_cast<SQLWCHAR>(0xde00), 'x', 0};
    } else {
        wide = {static_cast<SQLWCHAR>(0x1f600), 'x', 0};
    }
    for (const auto c_type : {SQL_C_CHAR, SQL_C_WCHAR}) {
        SQLPOINTER input = c_type == SQL_C_CHAR
            ? static_cast<SQLPOINTER>(utf8)
            : static_cast<SQLPOINTER>(wide.data());
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, SQL_WVARCHAR, 2, 0, input, 0, &input_length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        char output[8]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
            output, sizeof(output), nullptr));
        EXPECT_STREQ(utf8, output);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, SQL_WVARCHAR, 1, 0, input, 0, &input_length));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            continue;
        }
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22001", reinterpret_cast<char*>(state));
    }

    std::vector<SQLWCHAR> malformed;
    if constexpr (sizeof(SQLWCHAR) == 2) {
        malformed = {static_cast<SQLWCHAR>(0xd83d), 0};
    } else {
        malformed = {static_cast<SQLWCHAR>(0x110000), 0};
    }
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_WVARCHAR, 2, 0,
        malformed.data(), 0, &input_length));
    ASSERT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest, DateStructParameterRoundTrips) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_DATE_STRUCT input{2024, 2, 29};
    alignas(SQL_DATE_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_DATE_STRUCT)> input_bytes{};
    std::memcpy(input_bytes.data() + 1, &input, sizeof(input));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_DATE, SQL_TYPE_DATE, 10, 0, input_bytes.data() + 1,
        sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    SQLSMALLINT sql_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1, nullptr, 0, nullptr,
        &sql_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_TYPE_DATE, sql_type);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_DATE_STRUCT output{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &output, sizeof(output), &length));
    EXPECT_EQ(2024, output.year);
    EXPECT_EQ(2, output.month);
    EXPECT_EQ(29, output.day);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
}

TEST_F(PreparedStatementIntegrationTest, DefaultDateParameterCTypeRoundTrips) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_DATE_STRUCT input{2024, 12, 31};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DEFAULT, SQL_TYPE_DATE, 10, 0, &input, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_DATE_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &output, sizeof(output), nullptr));
    EXPECT_EQ(2024, output.year);
    EXPECT_EQ(12, output.month);
    EXPECT_EQ(31, output.day);
}

TEST_F(PreparedStatementIntegrationTest,
       DateStructParameterConvertsToTimestampAtMidnight) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_DATE_STRUCT input{2024, 2, 29};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_DATE, SQL_TYPE_TIMESTAMP, 19, 0,
        &input, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &output, sizeof(output), nullptr));
    EXPECT_EQ(2024, output.year);
    EXPECT_EQ(2, output.month);
    EXPECT_EQ(29, output.day);
    EXPECT_EQ(0, output.hour);
    EXPECT_EQ(0, output.minute);
    EXPECT_EQ(0, output.second);
    EXPECT_EQ(0u, output.fraction);
}

TEST_F(PreparedStatementIntegrationTest,
       DateStructParameterRejectsUnsupportedSqlTargetsAtBind) {
    SQL_DATE_STRUCT input{2024, 2, 29};
    SQLCHAR state[6]{};
    for (const SQLSMALLINT sql_type : {SQL_TYPE_TIME, SQL_INTEGER}) {
        EXPECT_EQ(SQL_ERROR, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_TYPE_DATE, sql_type, 10, 0,
            &input, sizeof(input), nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }
}

TEST_F(PreparedStatementIntegrationTest, DateStructParameterRejectsInvalidDate) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_DATE_STRUCT input{2023, 2, 29};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DATE, SQL_TYPE_DATE, 10, 0, &input, sizeof(input), nullptr));
    SQLCHAR state[6]{};
    for (const SQL_DATE_STRUCT invalid : {
             SQL_DATE_STRUCT{2023, 2, 29}, SQL_DATE_STRUCT{2024, 13, 1},
             SQL_DATE_STRUCT{2024, 4, 31}, SQL_DATE_STRUCT{0, 1, 1}}) {
        input = invalid;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
    }
}

TEST_F(PreparedStatementIntegrationTest, NullDateStructParameterStaysNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_DATE, SQL_TYPE_DATE, 10, 0, nullptr, 0, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_DATE_STRUCT output{73, 1, 1};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &output, sizeof(output), &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(73, output.year);
}

TEST_F(PreparedStatementIntegrationTest, TimeStructParameterRoundTrips) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIME_STRUCT input{12, 34, 56};
    alignas(SQL_TIME_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_TIME_STRUCT)> input_bytes{};
    std::memcpy(input_bytes.data() + 1, &input, sizeof(input));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIME, SQL_TYPE_TIME, 8, 0, input_bytes.data() + 1,
        sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    SQLSMALLINT sql_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1, nullptr, 0, nullptr,
        &sql_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_TYPE_TIME, sql_type);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIME_STRUCT output{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIME,
        &output, sizeof(output), &length));
    EXPECT_EQ(12, output.hour);
    EXPECT_EQ(34, output.minute);
    EXPECT_EQ(56, output.second);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
}

TEST_F(PreparedStatementIntegrationTest, DefaultTimeParameterCTypeRoundTrips) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIME_STRUCT input{23, 59, 59};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DEFAULT, SQL_TYPE_TIME, 8, 0, &input, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIME_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIME,
        &output, sizeof(output), nullptr));
    EXPECT_EQ(23, output.hour);
    EXPECT_EQ(59, output.minute);
    EXPECT_EQ(59, output.second);
}

TEST_F(PreparedStatementIntegrationTest,
       TimeStructParameterConvertsToTimestampOnCurrentLocalDate) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIME_STRUCT input{12, 34, 56};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIME, SQL_TYPE_TIMESTAMP, 19, 0,
        &input, sizeof(input), nullptr));

    const auto before_time = std::time(nullptr);
    const auto* before_calendar = std::localtime(&before_time);
    ASSERT_NE(nullptr, before_calendar);
    const std::tm before = *before_calendar;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &output, sizeof(output), nullptr));
    const auto after_time = std::time(nullptr);
    const auto* after_calendar = std::localtime(&after_time);
    ASSERT_NE(nullptr, after_calendar);
    const std::tm after = *after_calendar;
    const auto matches = [&](const std::tm& calendar) {
        return output.year == calendar.tm_year + 1900 &&
            output.month == calendar.tm_mon + 1 &&
            output.day == calendar.tm_mday;
    };
    EXPECT_TRUE(matches(before) || matches(after));
    EXPECT_EQ(12, output.hour);
    EXPECT_EQ(34, output.minute);
    EXPECT_EQ(56, output.second);
    EXPECT_EQ(0u, output.fraction);
}

TEST_F(PreparedStatementIntegrationTest,
       TimeStructToTimestampRejectsInvalidTimeAndPreservesNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIME_STRUCT input{24, 0, 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TIME, SQL_TYPE_TIMESTAMP, 19, 0,
        &input, sizeof(input), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));

    SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TIME, SQL_TYPE_TIMESTAMP, 19, 0,
        &input, sizeof(input), &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT output{73, 1, 1, 0, 0, 0, 0};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &output, sizeof(output), &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(73, output.year);
}

TEST_F(PreparedStatementIntegrationTest, TimeStructParameterRejectsInvalidTime) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIME_STRUCT input{24, 0, 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TIME, SQL_TYPE_TIME, 8, 0, &input, sizeof(input), nullptr));
    SQLCHAR state[6]{};
    for (const SQL_TIME_STRUCT invalid : {
             SQL_TIME_STRUCT{24, 0, 0}, SQL_TIME_STRUCT{12, 60, 0},
             SQL_TIME_STRUCT{12, 0, 62}}) {
        input = invalid;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
    }
}

TEST_F(PreparedStatementIntegrationTest, NullTimeStructParameterStaysNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIME, SQL_TYPE_TIME, 8, 0, nullptr, 0, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIME_STRUCT output{73, 1, 1};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIME,
        &output, sizeof(output), &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(73, output.hour);
}

TEST_F(PreparedStatementIntegrationTest, TimestampStructParameterRoundTrips) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 12, 34, 56, 123456000};
    alignas(SQL_TIMESTAMP_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_TIMESTAMP_STRUCT)> input_bytes{};
    std::memcpy(input_bytes.data() + 1, &input, sizeof(input));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, 6,
        input_bytes.data() + 1, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    SQLSMALLINT sql_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1, nullptr, 0, nullptr,
        &sql_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_TYPE_TIMESTAMP, sql_type);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT output{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &output, sizeof(output), &length));
    EXPECT_EQ(2024, output.year);
    EXPECT_EQ(2, output.month);
    EXPECT_EQ(29, output.day);
    EXPECT_EQ(12, output.hour);
    EXPECT_EQ(34, output.minute);
    EXPECT_EQ(56, output.second);
    EXPECT_EQ(123456000u, output.fraction);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
}

TEST_F(PreparedStatementIntegrationTest,
       DefaultTimestampParameterCTypeRoundTrips) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIMESTAMP_STRUCT input{2024, 12, 31, 23, 59, 59, 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DEFAULT, SQL_TYPE_TIMESTAMP, 26, 6,
        &input, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &output, sizeof(output), nullptr));
    EXPECT_EQ(2024, output.year);
    EXPECT_EQ(12, output.month);
    EXPECT_EQ(31, output.day);
    EXPECT_EQ(23, output.hour);
    EXPECT_EQ(59, output.minute);
    EXPECT_EQ(59, output.second);
    EXPECT_EQ(0u, output.fraction);
}

TEST_F(PreparedStatementIntegrationTest,
       TimestampStructParameterRejectsInvalidFieldsAndPrecisionLoss) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 12, 34, 56, 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, 6,
        &input, sizeof(input), nullptr));
    SQLCHAR state[6]{};
    for (const SQL_TIMESTAMP_STRUCT invalid : {
             SQL_TIMESTAMP_STRUCT{2023, 2, 29, 12, 34, 56, 0},
             SQL_TIMESTAMP_STRUCT{2024, 2, 29, 24, 0, 0, 0},
             SQL_TIMESTAMP_STRUCT{2024, 2, 29, 12, 34, 56, 1000000000u}}) {
        input = invalid;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
    }
    input = SQL_TIMESTAMP_STRUCT{2024, 2, 29, 12, 34, 56, 123456789u};
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22008", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       NullTimestampStructParameterStaysNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, 6,
        nullptr, 0, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT output{73, 1, 1, 0, 0, 0, 0};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &output, sizeof(output), &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(73, output.year);
}

TEST_F(PreparedStatementIntegrationTest,
       TimestampStructToCharacterParameterPreservesNanoseconds) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 12, 34, 56, 0};
    SQLCHAR state[6]{};
    const auto check = [&](SQLUINTEGER fraction, SQLSMALLINT sql_type,
                           SQLULEN width, const char* expected) {
        input.fraction = fraction;
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_TYPE_TIMESTAMP, sql_type, width, 0,
            &input, sizeof(input), nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        char output[40]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
            output, sizeof(output), nullptr));
        EXPECT_STREQ(expected, output);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

        for (const SQLULEN short_width : {
                 width - 1, static_cast<SQLULEN>(0)}) {
            ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1,
                SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP, sql_type,
                short_width, 0, &input, sizeof(input), nullptr));
            const auto result = SQLExecute(hstmt);
            EXPECT_EQ(SQL_ERROR, result);
            if (result != SQL_ERROR) {
                SQLCloseCursor(hstmt);
                continue;
            }
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("22001", reinterpret_cast<char*>(state));
        }
    };
    check(0, SQL_VARCHAR, 19, "2024-02-29 12:34:56");
    check(120000000u, SQL_WVARCHAR, 22, "2024-02-29 12:34:56.12");
    check(123456789u, SQL_VARCHAR, 29,
          "2024-02-29 12:34:56.123456789");
}

TEST_F(PreparedStatementIntegrationTest,
       TimestampStructToDateRequiresZeroTime) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 0, 0, 0, 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_DATE, 10, 0,
        &input, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_DATE_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &output, sizeof(output), nullptr));
    EXPECT_EQ(2024, output.year);
    EXPECT_EQ(2, output.month);
    EXPECT_EQ(29, output.day);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR state[6]{};
    for (const SQL_TIMESTAMP_STRUCT truncated : {
             SQL_TIMESTAMP_STRUCT{2024, 2, 29, 12, 0, 0, 0},
             SQL_TIMESTAMP_STRUCT{2024, 2, 29, 0, 0, 0, 1}}) {
        input = truncated;
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22008", reinterpret_cast<char*>(state));
    }
    input = SQL_TIMESTAMP_STRUCT{2023, 2, 29, 0, 0, 0, 0};
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       TimestampStructToTimeIgnoresDateAndRejectsFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIMESTAMP_STRUCT input{0, 0, 0, 12, 34, 56, 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIME, 8, 0,
        &input, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIME_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIME,
        &output, sizeof(output), nullptr));
    EXPECT_EQ(12, output.hour);
    EXPECT_EQ(34, output.minute);
    EXPECT_EQ(56, output.second);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR state[6]{};
    input.fraction = 1;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22008", reinterpret_cast<char*>(state));

    input.fraction = 0;
    input.hour = 24;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       TimestampParameterHonorsDeclaredFractionalPrecision) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 12, 34, 56, 123000000u};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 23, 3,
        &input, sizeof(input), nullptr));

    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLSMALLINT precision = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(implementation, 1,
        SQL_DESC_PRECISION, &precision, 0, nullptr));
    EXPECT_EQ(3, precision);

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &output, sizeof(output), nullptr));
    EXPECT_EQ(123000000u, output.fraction);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    input.fraction = 123456000u;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22008", reinterpret_cast<char*>(state));

    input.fraction = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 19, 0,
        &input, sizeof(input), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    input.fraction = 1000u;
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22008", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterTimestampParameterHonorsDeclaredFractionalPrecision) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NTS;
    SQLCHAR state[6]{};
    const auto bind = [&](const char* value, SQLSMALLINT precision) {
        return SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_CHAR, SQL_TYPE_TIMESTAMP, 26, precision,
            const_cast<char*>(value), 0, &indicator);
    };
    const auto expect_fraction = [&](const char* value,
                                     SQLSMALLINT precision,
                                     SQLUINTEGER fraction) {
        ASSERT_EQ(SQL_SUCCESS, bind(value, precision));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQL_TIMESTAMP_STRUCT output{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
            &output, sizeof(output), nullptr));
        EXPECT_EQ(fraction, output.fraction);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    const auto expect_error = [&](const char* value,
                                  SQLSMALLINT precision,
                                  const char* expected_state) {
        ASSERT_EQ(SQL_SUCCESS, bind(value, precision));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            return;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected_state, reinterpret_cast<char*>(state));
    };

    expect_fraction("2024-02-29 12:34:56", 0, 0);
    expect_error("2024-02-29 12:34:56.1", 0, "22008");
    expect_fraction("2024-02-29 12:34:56.123000", 3, 123000000u);
    expect_fraction(" 2024-02-29 12:34:56.123000 ", 3, 123000000u);
    expect_error("2024-02-29 12:34:56.123456", 3, "22008");
    expect_fraction("2024-02-29 12:34:56.123456000", 6, 123456000u);
    expect_error("2024-02-29 12:34:56.123456789", 6, "22008");
    expect_error("2024-02-29 12:34:56.1234560001", 6, "22008");
    expect_error("not-a-timestamp", 6, "22018");
    expect_error("2023-02-29 12:34:56", 6, "22018");
    expect_error("25:00:00", 6, "22018");

    std::vector<SQLWCHAR> wide_input;
    const auto bind_wide = [&](const char* value) {
        wide_input.clear();
        for (const char* ch = value; *ch; ++ch) {
            wide_input.push_back(static_cast<SQLWCHAR>(*ch));
        }
        wide_input.push_back(0);
        return SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_WCHAR, SQL_TYPE_TIMESTAMP, 26, 3,
            wide_input.data(), 0, &indicator);
    };
    ASSERT_EQ(SQL_SUCCESS, bind_wide("2024-02-29 12:34:56.123000"));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT wide_output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &wide_output, sizeof(wide_output), nullptr));
    EXPECT_EQ(123000000u, wide_output.fraction);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, bind_wide("2024-02-29 12:34:56.123456"));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22008", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       TimeOnlyCharacterTimestampParameterUsesCurrentLocalDate) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NTS;
    char input[] = " 12:34:56.123 ";
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_CHAR, SQL_TYPE_TIMESTAMP, 23, 3,
        input, 0, &indicator));
    const auto before_time = std::time(nullptr);
    const auto* before_calendar = std::localtime(&before_time);
    ASSERT_NE(nullptr, before_calendar);
    const std::tm before = *before_calendar;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIMESTAMP_STRUCT output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &output, sizeof(output), nullptr));
    const auto after_time = std::time(nullptr);
    const auto* after_calendar = std::localtime(&after_time);
    ASSERT_NE(nullptr, after_calendar);
    const std::tm after = *after_calendar;
    const auto matches = [&](const std::tm& calendar) {
        return output.year == calendar.tm_year + 1900 &&
            output.month == calendar.tm_mon + 1 &&
            output.day == calendar.tm_mday;
    };
    EXPECT_TRUE(matches(before) || matches(after));
    EXPECT_EQ(12, output.hour);
    EXPECT_EQ(34, output.minute);
    EXPECT_EQ(56, output.second);
    EXPECT_EQ(123000000u, output.fraction);
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterDateParameterValidatesAndDiscardsOnlyZeroTime) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NTS;
    SQLCHAR state[6]{};
    const auto bind = [&](SQLSMALLINT c_type, SQLPOINTER input) {
        return SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, SQL_TYPE_DATE, 10, 0, input, 0, &indicator);
    };
    const auto expect_date = [&](const char* input, SQLSMALLINT year,
                                 SQLUSMALLINT month, SQLUSMALLINT day) {
        ASSERT_EQ(SQL_SUCCESS, bind(SQL_C_CHAR, const_cast<char*>(input)));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQL_DATE_STRUCT output{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
            &output, sizeof(output), nullptr));
        EXPECT_EQ(year, output.year);
        EXPECT_EQ(month, output.month);
        EXPECT_EQ(day, output.day);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    const auto expect_error = [&](const char* input,
                                  const char* expected_state) {
        ASSERT_EQ(SQL_SUCCESS, bind(SQL_C_CHAR, const_cast<char*>(input)));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            return;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected_state, reinterpret_cast<char*>(state));
    };

    expect_date(" 2024-02-29 ", 2024, 2, 29);
    expect_date("2024-02-29 00:00:00.000000", 2024, 2, 29);
    expect_error("2024-02-29 00:00:01", "22008");
    expect_error("2024-02-29 00:00:00.000001", "22008");
    expect_error("2023-02-29", "22018");

    SQLWCHAR wide_input[] = {'2', '0', '2', '4', '-', '1', '2', '-', '3', '1', 0};
    ASSERT_EQ(SQL_SUCCESS, bind(SQL_C_WCHAR, wide_input));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_DATE_STRUCT wide_output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &wide_output, sizeof(wide_output), nullptr));
    EXPECT_EQ(2024, wide_output.year);
    EXPECT_EQ(12, wide_output.month);
    EXPECT_EQ(31, wide_output.day);
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterTimeParameterValidatesAndDiscardsOnlyZeroFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NTS;
    SQLCHAR state[6]{};
    const auto bind = [&](SQLSMALLINT c_type, SQLPOINTER input) {
        return SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, SQL_TYPE_TIME, 8, 0, input, 0, &indicator);
    };
    const auto expect_time = [&](const char* input, SQLUSMALLINT hour,
                                 SQLUSMALLINT minute, SQLUSMALLINT second) {
        ASSERT_EQ(SQL_SUCCESS, bind(SQL_C_CHAR, const_cast<char*>(input)));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQL_TIME_STRUCT output{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIME,
            &output, sizeof(output), nullptr));
        EXPECT_EQ(hour, output.hour);
        EXPECT_EQ(minute, output.minute);
        EXPECT_EQ(second, output.second);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    const auto expect_error = [&](const char* input,
                                  const char* expected_state) {
        ASSERT_EQ(SQL_SUCCESS, bind(SQL_C_CHAR, const_cast<char*>(input)));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            return;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected_state, reinterpret_cast<char*>(state));
    };

    expect_time(" 12:34:56 ", 12, 34, 56);
    expect_time("2024-02-29 12:34:56.000000", 12, 34, 56);
    expect_error("2024-02-29 12:34:56.000001", "22008");
    expect_error("25:00:00", "22018");
    expect_error("2023-02-29 12:34:56", "22018");

    SQLWCHAR wide_input[] = {'2', '3', ':', '5', '9', ':', '5', '9', 0};
    ASSERT_EQ(SQL_SUCCESS, bind(SQL_C_WCHAR, wide_input));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_TIME_STRUCT wide_output{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIME,
        &wide_output, sizeof(wide_output), nullptr));
    EXPECT_EQ(23, wide_output.hour);
    EXPECT_EQ(59, wide_output.minute);
    EXPECT_EQ(59, wide_output.second);
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterTemporalParametersRejectTimezoneSuffix) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NTS;
    SQLCHAR state[6]{};
    struct Case {
        SQLSMALLINT sql_type;
        SQLSMALLINT precision;
        SQLULEN column_size;
        const char* input;
    };
    for (const auto& test : std::array<Case, 4>{{
             {SQL_TYPE_DATE, 0, 10, "2024-02-29 00:00:00+02:00"},
             {SQL_TYPE_TIME, 0, 8, "2024-02-29 12:34:56+02:00"},
             {SQL_TYPE_TIMESTAMP, 6, 26, "2024-02-29 12:34:56+02:00"},
             {SQL_TYPE_TIMESTAMP, 6, 26, "12:34:56+02:00"}}}) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_CHAR, test.sql_type, test.column_size, test.precision,
            const_cast<char*>(test.input), 0, &indicator));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            continue;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
    }

    SQLWCHAR wide_input[] = {'1', '2', ':', '3', '4', ':', '5', '6',
                             '-', '0', '2', ':', '0', '0', 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_TYPE_TIME, 8, 0, wide_input, 0, &indicator));
    ASSERT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       CharacterTemporalParametersRejectIsoTSeparator) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLLEN indicator = SQL_NTS;
    SQLCHAR state[6]{};
    for (const auto sql_type : {SQL_TYPE_DATE, SQL_TYPE_TIME,
                                SQL_TYPE_TIMESTAMP}) {
        char input[] = "2024-02-29T12:34:56";
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_CHAR, sql_type, 26,
            sql_type == SQL_TYPE_TIMESTAMP ? 6 : 0, input, 0, &indicator));
        const auto result = SQLExecute(hstmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result != SQL_ERROR) {
            SQLCloseCursor(hstmt);
            continue;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
    }

    SQLWCHAR wide_input[] = {'2', '0', '2', '4', '-', '0', '2', '-', '2', '9',
                             'T', '1', '2', ':', '3', '4', ':', '5', '6', 0};
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_WCHAR, SQL_TYPE_TIMESTAMP, 26, 6,
        wide_input, 0, &indicator));
    ASSERT_EQ(SQL_ERROR, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
}

TEST_F(PreparedStatementIntegrationTest,
       TemporalParameterRejectsUnsupportedFractionalPrecision) {
    SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 12, 34, 56, 0};
    SQLCHAR state[6]{};
    for (const SQLSMALLINT sql_type : {SQL_TYPE_TIME, SQL_TYPE_TIMESTAMP}) {
        for (const SQLSMALLINT precision : {SQLSMALLINT{-1}, SQLSMALLINT{7}}) {
            EXPECT_EQ(SQL_ERROR, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
                SQL_C_TYPE_TIMESTAMP, sql_type, 26, precision,
                &input, sizeof(input), nullptr));
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("HY104", reinterpret_cast<char*>(state));
        }
    }
}

TEST_F(PreparedStatementIntegrationTest,
       TimeAndTimestampStructsRejectUnsupportedSqlTargetsAtBind) {
    SQL_TIME_STRUCT time{12, 34, 56};
    SQL_TIMESTAMP_STRUCT timestamp{2024, 2, 29, 12, 34, 56, 0};
    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLSMALLINT bound_type = 0;
    SQLCHAR state[6]{};

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIME, SQL_TYPE_TIME, 8, 0,
        &time, sizeof(time), nullptr));
    for (const SQLSMALLINT sql_type : {SQL_TYPE_DATE, SQL_INTEGER}) {
        EXPECT_EQ(SQL_ERROR, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_TYPE_TIME, sql_type, 10, 0,
            &time, sizeof(time), nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(implementation, 1,
            SQL_DESC_CONCISE_TYPE, &bound_type, 0, nullptr));
        EXPECT_EQ(SQL_TYPE_TIME, bound_type);
    }

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 19, 0,
        &timestamp, sizeof(timestamp), nullptr));
    for (const SQLSMALLINT sql_type : {SQL_INTEGER, SQL_VARBINARY}) {
        EXPECT_EQ(SQL_ERROR, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_TYPE_TIMESTAMP, sql_type, 19, 0,
            &timestamp, sizeof(timestamp), nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(implementation, 1,
            SQL_DESC_CONCISE_TYPE, &bound_type, 0, nullptr));
        EXPECT_EQ(SQL_TYPE_TIMESTAMP, bound_type);
    }
}

TEST_F(PreparedStatementIntegrationTest,
       InvalidTemporalStructsUseCharacterTargetDiagnostic) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_DATE_STRUCT date{2023, 2, 29};
    SQL_TIME_STRUCT time{24, 0, 0};
    SQL_TIMESTAMP_STRUCT timestamp{2024, 2, 29, 24, 0, 0, 0};
    SQLCHAR state[6]{};
    const auto expect_invalid = [&](SQLSMALLINT c_type, SQLPOINTER input,
                                    SQLLEN size, SQLULEN column_size) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, SQL_VARCHAR, column_size, 0, input, size, nullptr));
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22008", reinterpret_cast<char*>(state));
    };
    expect_invalid(SQL_C_TYPE_DATE, &date, sizeof(date), 10);
    expect_invalid(SQL_C_TYPE_TIME, &time, sizeof(time), 8);
    expect_invalid(SQL_C_TYPE_TIMESTAMP, &timestamp, sizeof(timestamp), 26);
}

TEST_F(PreparedStatementIntegrationTest,
       DateAndTimeStructCharacterTargetsHonorDeclaredLength) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQL_DATE_STRUCT date{2024, 2, 29};
    SQL_TIME_STRUCT time{12, 34, 56};
    SQLCHAR state[6]{};
    const auto check = [&](SQLSMALLINT c_type, SQLSMALLINT sql_type,
                           SQLPOINTER input, SQLLEN size, SQLULEN width,
                           const char* expected) {
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            c_type, sql_type, width, 0, input, size, nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLCHAR output[32]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
            output, sizeof(output), nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(output));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

        for (const SQLULEN short_width : {
                 width - 1, static_cast<SQLULEN>(0)}) {
            ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1,
                SQL_PARAM_INPUT, c_type, sql_type, short_width, 0,
                input, size, nullptr));
            const auto result = SQLExecute(hstmt);
            EXPECT_EQ(SQL_ERROR, result);
            if (result != SQL_ERROR) {
                SQLCloseCursor(hstmt);
                continue;
            }
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("22001", reinterpret_cast<char*>(state));
        }
    };
    check(SQL_C_TYPE_DATE, SQL_VARCHAR, &date, sizeof(date), 10,
          "2024-02-29");
    check(SQL_C_TYPE_TIME, SQL_WVARCHAR, &time, sizeof(time), 8,
          "12:34:56");
}

TEST_F(PreparedStatementIntegrationTest,
       QuotedIdentifierBackslashDoesNotHideParameter) {
    char sql[] = R"(SELECT 1 AS "slash\", ?::integer AS value)";
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, reinterpret_cast<SQLCHAR*>(sql), SQL_NTS));
    SQLSMALLINT parameter_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(1, parameter_count);

    SQLINTEGER input = 42;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SLONG, &output, sizeof(output), nullptr));
    EXPECT_EQ(input, output);
}

TEST_F(PreparedStatementIntegrationTest,
       OrdinaryStringBackslashDoesNotHideParameter) {
    char sql[] = R"(SELECT 'slash\' AS literal, ?::integer AS value)";
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, reinterpret_cast<SQLCHAR*>(sql), SQL_NTS));
    SQLSMALLINT parameter_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(1, parameter_count);

    SQLINTEGER input = 42;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SLONG, &output, sizeof(output), nullptr));
    EXPECT_EQ(input, output);
}

TEST_F(PreparedStatementIntegrationTest,
       DollarSignsInIdentifierDoNotHideParameter) {
    char sql[] = "SELECT 1 AS foo$tag$bar, ?::integer AS value";
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, reinterpret_cast<SQLCHAR*>(sql), SQL_NTS));
    SQLSMALLINT parameter_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(1, parameter_count);

    SQLINTEGER input = 42;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SLONG, &output, sizeof(output), nullptr));
    EXPECT_EQ(input, output);
}

TEST_F(PreparedStatementIntegrationTest,
       CarriageReturnEndsCommentBeforeParameter) {
    char sql[] = "SELECT 1 -- ignored ?\r, ?::integer AS value";
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, reinterpret_cast<SQLCHAR*>(sql), SQL_NTS));
    SQLSMALLINT parameter_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(1, parameter_count);

    SQLINTEGER input = 42;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SLONG, &output, sizeof(output), nullptr));
    EXPECT_EQ(input, output);
}

TEST_F(PreparedStatementIntegrationTest,
       ParameterStatusOutputsHandleUnalignedBuffers) {
    alignas(SQLULEN) std::array<std::byte, 1 + sizeof(SQLULEN)> processed{};
    alignas(SQLUSMALLINT)
    std::array<std::byte, 1 + sizeof(SQLUSMALLINT)> status{};
    auto* processed_output = reinterpret_cast<SQLULEN*>(processed.data() + 1);
    auto* status_output = reinterpret_cast<SQLUSMALLINT*>(status.data() + 1);
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, processed_output, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAM_STATUS_PTR, status_output, 0));

    SQLINTEGER input = 7;
    const auto run = [&](const char* query, SQLRETURN expected_result,
                         SQLUSMALLINT expected_status) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
            hstmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(query)),
            SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
            hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
            0, 0, &input, 0, nullptr));
        processed.fill(std::byte{0x5a});
        status.fill(std::byte{0x5a});
        EXPECT_EQ(expected_result, SQLExecute(hstmt));
        SQLULEN copied_processed = 0;
        SQLUSMALLINT copied_status = 0;
        std::memcpy(&copied_processed, processed.data() + 1,
                    sizeof(copied_processed));
        std::memcpy(&copied_status, status.data() + 1,
                    sizeof(copied_status));
        EXPECT_EQ(1u, copied_processed);
        EXPECT_EQ(expected_status, copied_status);
    };
    run("SELECT ?::integer", SQL_SUCCESS, SQL_PARAM_SUCCESS);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    input = 0;
    run("SELECT 1 / ?::integer", SQL_ERROR, SQL_PARAM_ERROR);
}

TEST_F(PreparedStatementIntegrationTest,
       ParameterIndicatorsMayUseUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::text, ?::bytea, ?::integer, ?::text",
        SQL_NTS));

    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> text_length{};
    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> binary_length{};
    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> null_length{};
    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> wide_length{};
    auto* text_indicator = reinterpret_cast<SQLLEN*>(text_length.data() + 1);
    auto* binary_indicator = reinterpret_cast<SQLLEN*>(
        binary_length.data() + 1);
    auto* null_indicator = reinterpret_cast<SQLLEN*>(null_length.data() + 1);
    auto* wide_indicator = reinterpret_cast<SQLLEN*>(wide_length.data() + 1);
    const SQLLEN text_bytes = 3;
    const SQLLEN binary_bytes = 2;
    const SQLLEN null_value = SQL_NULL_DATA;
    auto wide = rs::odbc::utf8_to_wide("Hi");
    ASSERT_TRUE(wide.has_value());
    const SQLLEN wide_bytes = static_cast<SQLLEN>(
        wide->size() * sizeof(SQLWCHAR));
    wide->push_back(0);
    std::memcpy(text_length.data() + 1, &text_bytes, sizeof(text_bytes));
    std::memcpy(binary_length.data() + 1, &binary_bytes,
                sizeof(binary_bytes));
    std::memcpy(null_length.data() + 1, &null_value, sizeof(null_value));
    std::memcpy(wide_length.data() + 1, &wide_bytes, sizeof(wide_bytes));

    char text[] = "abc";
    unsigned char binary[]{0x00, 0xff};
    SQLINTEGER ignored = 7;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        3, 0, text, sizeof(text), text_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 2, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY,
        sizeof(binary), 0, binary, sizeof(binary), binary_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 3, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &ignored, 0, null_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 4, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        2, 0, wide->data(),
        static_cast<SQLLEN>(wide->size() * sizeof(SQLWCHAR)),
        wide_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char text_output[8]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, text_output, sizeof(text_output), nullptr));
    EXPECT_STREQ("abc", text_output);
    unsigned char binary_output[sizeof(binary)]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_BINARY, binary_output,
        sizeof(binary_output), nullptr));
    EXPECT_EQ(0, std::memcmp(binary, binary_output, sizeof(binary)));
    SQLLEN returned_null = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_SLONG, &ignored, sizeof(ignored), &returned_null));
    EXPECT_EQ(SQL_NULL_DATA, returned_null);
    char wide_output[8]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, wide_output, sizeof(wide_output), nullptr));
    EXPECT_STREQ("Hi", wide_output);
}

TEST_F(PreparedStatementIntegrationTest,
       NumericParametersMayUseUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::integer, ?::real, ?::double precision",
        SQL_NTS));

    alignas(SQLDOUBLE)
        std::array<std::byte, 1 + sizeof(SQLDOUBLE)> integer_bytes{};
    alignas(SQLDOUBLE)
        std::array<std::byte, 1 + sizeof(SQLDOUBLE)> real_bytes{};
    alignas(SQLDOUBLE)
        std::array<std::byte, 1 + sizeof(SQLDOUBLE)> double_bytes{};
    const SQLINTEGER integer = 42;
    const SQLREAL real = 1.5f;
    const SQLDOUBLE floating = 2.25;
    std::memcpy(integer_bytes.data() + 1, &integer, sizeof(integer));
    std::memcpy(real_bytes.data() + 1, &real, sizeof(real));
    std::memcpy(double_bytes.data() + 1, &floating, sizeof(floating));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_INTEGER, 0, 0, integer_bytes.data() + 1,
        sizeof(integer), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT,
        SQL_C_FLOAT, SQL_REAL, 0, 0, real_bytes.data() + 1,
        sizeof(real), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 3, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, double_bytes.data() + 1,
        sizeof(floating), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLINTEGER returned_integer = 0;
    SQLREAL returned_real = 0;
    SQLDOUBLE returned_double = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &returned_integer, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_FLOAT,
        &returned_real, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_DOUBLE,
        &returned_double, 0, nullptr));
    EXPECT_EQ(integer, returned_integer);
    EXPECT_FLOAT_EQ(real, returned_real);
    EXPECT_DOUBLE_EQ(floating, returned_double);
}

TEST_F(PreparedStatementIntegrationTest,
       WideParameterMayUseUnalignedBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::text", SQL_NTS));
    const std::string expected = "\xf0\x9f\x9a\x80 prepared";
    auto wide = rs::odbc::utf8_to_wide(expected);
    ASSERT_TRUE(wide.has_value());
    wide->push_back(0);
    alignas(SQLWCHAR)
        std::array<std::byte, 1 + 16 * sizeof(SQLWCHAR)> storage{};
    ASSERT_LE(wide->size() * sizeof(SQLWCHAR), storage.size() - 1);
    std::memcpy(storage.data() + 1, wide->data(),
                wide->size() * sizeof(SQLWCHAR));

    SQLLEN input_length = SQL_NTS;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR,
        wide->size() - 1, 0, storage.data() + 1,
        static_cast<SQLLEN>(wide->size() * sizeof(SQLWCHAR)),
        &input_length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char output[64]{};
    SQLLEN output_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, output, sizeof(output), &output_length));
    EXPECT_EQ(static_cast<SQLLEN>(expected.size()), output_length);
    EXPECT_EQ(expected, output);
}

TEST_F(PreparedStatementIntegrationTest,
       ApplicationDescriptorsDrivePreparedExecution) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer + 1", SQL_NTS));

    SQLHDESC application = SQL_NULL_HDESC;
    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &application));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLINTEGER input = 41;
    const auto number = [](SQLLEN value) {
        return reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(value));
    };
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        application, 1, SQL_DESC_CONCISE_TYPE,
        number(SQL_C_SLONG), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        application, 1, SQL_DESC_DATA_PTR, &input, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_CONCISE_TYPE,
        number(SQL_INTEGER), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_PARAMETER_TYPE,
        number(SQL_PARAM_INPUT), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_PARAM_DESC, application, 0));

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &output, 0, nullptr));
    EXPECT_EQ(42, output);

    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, application));
}

TEST_F(PreparedStatementIntegrationTest, PreparePreservesParameterBindings) {
    SQLINTEGER input = 7;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &output, 0, nullptr));
    EXPECT_EQ(input, output);
}

TEST_F(PreparedStatementIntegrationTest,
       RejectsUnsupportedAttachedParameterArraysBeforeExecution) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLINTEGER input = 7;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &descriptor));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 0, SQL_DESC_ARRAY_SIZE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_PARAM_DESC, descriptor, 0));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));

    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HYC00", reinterpret_cast<char*>(state));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

TEST_F(PreparedStatementIntegrationTest, ReportsPreparedParameterCount) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?, '?'::text, ? /* ? */, $$?$$, ?", SQL_NTS));
    SQLSMALLINT parameter_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(3, parameter_count);

    alignas(SQLSMALLINT) std::array<std::byte, 1 + sizeof(SQLSMALLINT)> storage{};
    auto* unaligned_count = reinterpret_cast<SQLSMALLINT*>(
        storage.data() + 1);
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, unaligned_count));
    SQLSMALLINT copied_count = 0;
    std::memcpy(&copied_count, storage.data() + 1, sizeof(copied_count));
    EXPECT_EQ(parameter_count, copied_count);

    SQLHSTMT unprepared = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_STMT, hdbc, &unprepared));
    const auto previous_output = storage;
    EXPECT_EQ(SQL_ERROR, SQLNumParams(unprepared, unaligned_count));
    EXPECT_EQ(previous_output, storage);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, unprepared));
}

TEST_F(PreparedStatementIntegrationTest,
       ParameterMetadataFollowsStatementState) {
    const auto expect_state = [this](const char* expected) {
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0,
            nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(state));
    };

    SQLSMALLINT parameter_count = 61;
    EXPECT_EQ(SQL_ERROR, SQLNumParams(hstmt, &parameter_count));
    expect_state("HY010");
    EXPECT_EQ(61, parameter_count);

    SQLSMALLINT data_type = 62;
    SQLULEN parameter_size = 63;
    SQLSMALLINT decimal_digits = 64;
    SQLSMALLINT nullable = 65;
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(
        hstmt, 0, &data_type, &parameter_size, &decimal_digits,
        &nullable));
    expect_state("07009");
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(
        hstmt, 1, &data_type, &parameter_size, &decimal_digits,
        &nullable));
    expect_state("HY010");
    EXPECT_EQ(62, data_type);
    EXPECT_EQ(63u, parameter_size);
    EXPECT_EQ(64, decimal_digits);
    EXPECT_EQ(65, nullable);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ? FROM", SQL_NTS));
    parameter_count = 66;
    EXPECT_EQ(SQL_ERROR, SQLNumParams(hstmt, &parameter_count));
    expect_state("42000");
    EXPECT_EQ(66, parameter_count);
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(
        hstmt, 1, &data_type, &parameter_size, &decimal_digits,
        &nullable));
    expect_state("42000");
    EXPECT_EQ(62, data_type);
    EXPECT_EQ(63u, parameter_size);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLINTEGER extra_value = 99;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &extra_value, 0, nullptr));
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(
        hstmt, 2, &data_type, &parameter_size, &decimal_digits,
        &nullable));
    expect_state("07009");
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(1, parameter_count);

    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLSMALLINT descriptor_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 0, SQL_DESC_COUNT, &descriptor_count, 0,
        nullptr));
    EXPECT_EQ(1, descriptor_count);

    SQLINTEGER input = 41;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &data_type, &parameter_size, &decimal_digits,
        &nullable));
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(10u, parameter_size);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    data_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &data_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, data_type);

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS));
    parameter_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(0, parameter_count);
    descriptor_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 0, SQL_DESC_COUNT, &descriptor_count, 0,
        nullptr));
    EXPECT_EQ(0, descriptor_count);
    data_type = 67;
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(
        hstmt, 1, &data_type, nullptr, nullptr, nullptr));
    expect_state("07009");
    EXPECT_EQ(67, data_type);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    parameter_count = 68;
    EXPECT_EQ(SQL_ERROR, SQLNumParams(hstmt, &parameter_count));
    expect_state("HY010");
    EXPECT_EQ(68, parameter_count);
}

TEST_F(PreparedStatementIntegrationTest,
       DescribesInferredParameterTypesBeforeExecution) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt,
        (SQLCHAR*)"SELECT ?::boolean, ?::smallint, ?::integer, "
                  "?::bigint, ?::real, ?::double precision, ?::date, "
                  "?::time, ?::timestamp, ?::bytea, ?::text",
        SQL_NTS));

    SQLSMALLINT first_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &first_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_BIT, first_type);

    SQLSMALLINT parameter_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(11, parameter_count);

    struct ExpectedParameter {
        SQLSMALLINT data_type;
        SQLULEN size;
        SQLSMALLINT decimal_digits;
    };
    const ExpectedParameter expected[] = {
        {SQL_BIT, 1, 0},
        {SQL_SMALLINT, 5, 0},
        {SQL_INTEGER, 10, 0},
        {SQL_BIGINT, 19, 0},
        {SQL_REAL, 7, 6},
        {SQL_DOUBLE, 15, 15},
        {SQL_TYPE_DATE, 10, 0},
        {SQL_TYPE_TIME, 15, 6},
        {SQL_TYPE_TIMESTAMP, 26, 6},
        {SQL_VARBINARY, 0, 0},
        {SQL_VARCHAR, 0, 0},
    };
    for (SQLUSMALLINT index = 0; index < parameter_count; ++index) {
        SQLSMALLINT data_type = 0;
        SQLULEN parameter_size = 0;
        SQLSMALLINT decimal_digits = -1;
        SQLSMALLINT nullable = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
            hstmt, index + 1, &data_type, &parameter_size,
            &decimal_digits, &nullable));
        EXPECT_EQ(expected[index].data_type, data_type) << index;
        EXPECT_EQ(expected[index].size, parameter_size) << index;
        EXPECT_EQ(expected[index].decimal_digits, decimal_digits) << index;
        EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable) << index;
    }

    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLSMALLINT descriptor_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 0, SQL_DESC_COUNT, &descriptor_count, 0,
        nullptr));
    EXPECT_EQ(parameter_count, descriptor_count);
    SQLSMALLINT descriptor_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 7, SQL_DESC_TYPE, &descriptor_type, 0,
        nullptr));
    EXPECT_EQ(SQL_DATETIME, descriptor_type);
}

TEST_F(PreparedStatementIntegrationTest,
       PreservesBoundParameterPrecisionAndClearsStaleIpdRecords) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::numeric, ?::varchar", SQL_NTS));
    char amount[]{"12.34"};
    char label[]{"prepared"};
    SQLLEN amount_length = SQL_NTS;
    SQLLEN label_length = SQL_NTS;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_NUMERIC,
        8, 2, amount, sizeof(amount), &amount_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        12, 0, label, sizeof(label), &label_length));

    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLSMALLINT precision = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 1, SQL_DESC_PRECISION, &precision, 0, nullptr));
    EXPECT_EQ(8, precision);

    SQLSMALLINT data_type = 0;
    SQLULEN parameter_size = 0;
    SQLSMALLINT decimal_digits = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &data_type, &parameter_size, &decimal_digits,
        nullptr));
    EXPECT_EQ(SQL_NUMERIC, data_type);
    EXPECT_EQ(8u, parameter_size);
    EXPECT_EQ(2, decimal_digits);
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 2, &data_type, &parameter_size, &decimal_digits,
        nullptr));
    EXPECT_EQ(SQL_VARCHAR, data_type);
    EXPECT_EQ(12u, parameter_size);

    const auto number = [](SQLLEN value) {
        return reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(value));
    };
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_PRECISION, number(9), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_SCALE, number(3), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &data_type, &parameter_size, &decimal_digits,
        nullptr));
    EXPECT_EQ(SQL_NUMERIC, data_type);
    EXPECT_EQ(9u, parameter_size);
    EXPECT_EQ(3, decimal_digits);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS));
    SQLSMALLINT parameter_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(0, parameter_count);
    SQLSMALLINT descriptor_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 0, SQL_DESC_COUNT, &descriptor_count, 0,
        nullptr));
    EXPECT_EQ(0, descriptor_count);

    data_type = 71;
    parameter_size = 72;
    decimal_digits = 73;
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(
        hstmt, 1, &data_type, &parameter_size, &decimal_digits,
        nullptr));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07009", reinterpret_cast<char*>(state));
    EXPECT_EQ(71, data_type);
    EXPECT_EQ(72u, parameter_size);
    EXPECT_EQ(73, decimal_digits);
}

TEST_F(PreparedStatementIntegrationTest,
       FloatingParameterUsesIpdPrecisionAndRejectsOversizedPrecision) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::real, ?::double precision, ?::double precision",
        SQL_NTS));
    SQLDOUBLE values[]{1.5, 2.5, 3.5};
    const SQLSMALLINT sql_types[]{SQL_REAL, SQL_DOUBLE, SQL_FLOAT};
    const SQLSMALLINT precisions[]{7, 15, 15};
    for (SQLUSMALLINT parameter = 1; parameter <= 3; ++parameter) {
        const auto index = parameter - 1;
        EXPECT_EQ(SQL_ERROR, SQLBindParameter(hstmt, parameter,
            SQL_PARAM_INPUT, SQL_C_DOUBLE, sql_types[index],
            static_cast<SQLULEN>(std::numeric_limits<SQLSMALLINT>::max()) + 1,
            0, &values[index], sizeof(values[index]), nullptr));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("HY104", reinterpret_cast<char*>(state));
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, parameter,
            SQL_PARAM_INPUT, SQL_C_DOUBLE, sql_types[index],
            precisions[index], 0, &values[index], sizeof(values[index]),
            nullptr));
    }

    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_IMP_PARAM_DESC,
        &implementation, 0, nullptr));
    for (SQLSMALLINT parameter = 1; parameter <= 3; ++parameter) {
        SQLSMALLINT precision = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(implementation, parameter,
            SQL_DESC_PRECISION, &precision, 0, nullptr));
        EXPECT_EQ(precisions[parameter - 1], precision) << parameter;
    }

    const auto number = [](SQLLEN value) {
        return reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(value));
    };
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(implementation, 2,
        SQL_DESC_PRECISION, number(14), 0));
    SQLSMALLINT data_type = 0;
    SQLULEN parameter_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(hstmt, 2,
        &data_type, &parameter_size, nullptr, nullptr));
    EXPECT_EQ(SQL_DOUBLE, data_type);
    EXPECT_EQ(14u, parameter_size);

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    for (SQLUSMALLINT column = 1; column <= 3; ++column) {
        SQLDOUBLE result = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_DOUBLE,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(values[column - 1], result);
    }
}

TEST_F(PreparedStatementIntegrationTest,
       RealParameterUsesFloat32ServerTypeAndRecoversFromOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLDOUBLE value = std::numeric_limits<SQLDOUBLE>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_REAL, 7, 0, &value, sizeof(value), nullptr));

    SQLSMALLINT data_type = 0;
    SQLULEN parameter_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(hstmt, 1,
        &data_type, &parameter_size, nullptr, nullptr));
    EXPECT_EQ(SQL_REAL, data_type);
    EXPECT_EQ(7u, parameter_size);

    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    value = 1.5;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLREAL result = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_FLOAT,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(1.5f, result);
}

TEST_F(PreparedStatementIntegrationTest,
       SmallintParameterUsesInt16ServerTypeAndRecoversFromOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?", SQL_NTS));
    SQLINTEGER value = std::numeric_limits<SQLSMALLINT>::max() + 1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_SMALLINT, 5, 0, &value, sizeof(value), nullptr));

    SQLSMALLINT data_type = 0;
    SQLULEN parameter_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(hstmt, 1,
        &data_type, &parameter_size, nullptr, nullptr));
    EXPECT_EQ(SQL_SMALLINT, data_type);
    EXPECT_EQ(5u, parameter_size);

    const auto expect_overflow = [&] {
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    };
    expect_overflow();
    value = std::numeric_limits<SQLSMALLINT>::min() - 1;
    expect_overflow();

    for (const SQLSMALLINT boundary : {
             std::numeric_limits<SQLSMALLINT>::min(),
             std::numeric_limits<SQLSMALLINT>::max()}) {
        value = boundary;
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLSMALLINT result = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SSHORT,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(boundary, result);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
}

TEST_F(PreparedStatementIntegrationTest,
       DefaultTinyintParameterReadsSingleSignedByte) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLSCHAR value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_DEFAULT, SQL_TINYINT, 3, 0, &value, 0, nullptr));
    SQLSMALLINT server_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(hstmt, 1,
        &server_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, server_type);
    for (const SQLSCHAR boundary : {
             static_cast<SQLSCHAR>(0),
             std::numeric_limits<SQLSCHAR>::min(),
             std::numeric_limits<SQLSCHAR>::max()}) {
        value = boundary;
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        SQLINTEGER result = 999;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
            &result, sizeof(result), nullptr));
        EXPECT_EQ(boundary, result);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
}

TEST_F(PreparedStatementIntegrationTest,
       DescriptorDefaultTinyintSurvivesServerTypePromotion) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLHDESC application = SQL_NULL_HDESC;
    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DESC, hdbc,
        &application));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_IMP_PARAM_DESC,
        &implementation, 0, nullptr));
    const auto number = [](SQLLEN field) {
        return reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(field));
    };
    SQLSCHAR value = std::numeric_limits<SQLSCHAR>::min();
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(application, 1,
        SQL_DESC_CONCISE_TYPE, number(SQL_C_DEFAULT), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(application, 1,
        SQL_DESC_DATA_PTR, &value, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(implementation, 1,
        SQL_DESC_CONCISE_TYPE, number(SQL_TINYINT), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_APP_PARAM_DESC,
        application, 0));

    SQLSMALLINT server_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(hstmt, 1,
        &server_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, server_type);
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER result = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(value, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLSCHAR replacement = std::numeric_limits<SQLSCHAR>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(application, 1,
        SQL_DESC_DATA_PTR, &replacement, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(replacement, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLSMALLINT wider = 1234;
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(implementation, 1,
        SQL_DESC_CONCISE_TYPE, number(SQL_SMALLINT), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(application, 1,
        SQL_DESC_DATA_PTR, &wider, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(hstmt, 1,
        &server_type, nullptr, nullptr, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(wider, result);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_APP_PARAM_DESC,
        SQL_NULL_HDESC, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, application));
}

TEST_F(PreparedStatementIntegrationTest,
       NumericParameterRejectsUnrepresentablePrecision) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::numeric", SQL_NTS));
    SQLINTEGER value = 42;
    EXPECT_EQ(SQL_ERROR, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_NUMERIC,
        static_cast<SQLULEN>(std::numeric_limits<SQLSMALLINT>::max()) + 1,
        0, &value, sizeof(value), nullptr));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY104", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_NUMERIC, 2, 0,
        &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER result = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(value, result);
}

TEST_F(PreparedStatementIntegrationTest,
       DecimalParameterUsesIpdPrecisionAndScale) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT ?::numeric", SQL_NTS));
    SQLINTEGER value = 42;
    EXPECT_EQ(SQL_ERROR, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_DECIMAL,
        static_cast<SQLULEN>(std::numeric_limits<SQLSMALLINT>::max()) + 1,
        2, &value, sizeof(value), nullptr));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY104", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
        SQL_C_SLONG, SQL_DECIMAL, 5, 2,
        &value, sizeof(value), nullptr));
    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_IMP_PARAM_DESC,
        &implementation, 0, nullptr));
    SQLSMALLINT precision = -1;
    SQLSMALLINT scale = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(implementation, 1,
        SQL_DESC_PRECISION, &precision, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(implementation, 1,
        SQL_DESC_SCALE, &scale, 0, nullptr));
    EXPECT_EQ(5, precision);
    EXPECT_EQ(2, scale);
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER result = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &result, sizeof(result), nullptr));
    EXPECT_EQ(value, result);
}

TEST_F(PreparedStatementIntegrationTest,
       ReplacesPreparedStatementsAndProtectsOpenCursors) {
    SQLCHAR state[6]{};
    const auto expect_state = [&](SQLRETURN result, const char* expected) {
        EXPECT_EQ(SQL_ERROR, result);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(state));
    };

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 11", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    expect_state(SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 12", SQL_NTS), "24000");
    expect_state(SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 13", SQL_NTS), "24000");
    expect_state(SQLExecute(hstmt), "24000");

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 21", SQL_NTS));
    SQLSMALLINT parameter_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(0, parameter_count);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    expect_state(SQLExecute(hstmt), "HY010");
}

TEST_F(PreparedStatementIntegrationTest,
       MissingParameterFailsAndExtraBindingIsIgnored) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLUSMALLINT param_status = SQL_PARAM_UNUSED;
    SQLULEN params_processed = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAM_STATUS_PTR, &param_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &params_processed, 0));

    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ(1u, params_processed);
    EXPECT_EQ(SQL_PARAM_ERROR, param_status);
    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07009", reinterpret_cast<char*>(sqlstate));

    SQLINTEGER ignored = 99;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &ignored, 0, nullptr));
    SQLINTEGER used = 41;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &used, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &output, 0, nullptr));
    EXPECT_EQ(used, output);
}

TEST_F(PreparedStatementIntegrationTest,
       BindingValidationUsesSpecificDiagnostics) {
    SQLINTEGER value = 7;
    SQLCHAR state[6]{};
    const auto expect_state = [&](SQLRETURN result, const char* expected) {
        EXPECT_EQ(SQL_ERROR, result);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(state));
    };

    expect_state(SQLBindParameter(
        hstmt, 1, 12345, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &value, 0, nullptr), "HY105");
    expect_state(SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, 12345, SQL_INTEGER,
        0, 0, &value, 0, nullptr), "HY003");
    expect_state(SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, 12345,
        0, 0, &value, 0, nullptr), "HY004");
    expect_state(SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        0, 0, &value, -1, nullptr), "HY090");
    expect_state(SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        0, 0, nullptr, 0, nullptr), "HY009");

    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &value, 0, nullptr));
}

TEST_F(PreparedStatementIntegrationTest, DefaultCTypeUsesSqlTypeMapping) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::smallint", SQL_NTS));
    SQLSMALLINT input = 123;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT, SQL_SMALLINT,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &output, 0, nullptr));
    EXPECT_EQ(input, output);
}

TEST_F(PreparedStatementIntegrationTest, AutocommitOffSupportsCommitAndRollback) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_tx_test(value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
        hdbc, SQL_ATTR_AUTOCOMMIT,
        reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(SQL_AUTOCOMMIT_OFF)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
        hdbc, SQL_ATTR_TXN_ISOLATION,
        reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(SQL_TXN_REPEATABLE_READ)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SHOW transaction_isolation", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char isolation[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, isolation, sizeof(isolation), nullptr));
    EXPECT_STREQ("repeatable read", isolation);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_tx_test VALUES (1)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT count(*) FROM odbcpp_tx_test", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &count, 0, nullptr));
    EXPECT_EQ(0, count);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_tx_test VALUES (2)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_COMMIT));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT count(*) FROM odbcpp_tx_test", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &count, 0, nullptr));
    EXPECT_EQ(1, count);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
        hdbc, SQL_ATTR_AUTOCOMMIT,
        reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(SQL_AUTOCOMMIT_ON)), 0));
    SQLUINTEGER autocommit = SQL_AUTOCOMMIT_OFF;
    ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
        hdbc, SQL_ATTR_AUTOCOMMIT, &autocommit, sizeof(autocommit), nullptr));
    EXPECT_EQ(SQL_AUTOCOMMIT_ON, autocommit);
}

// Test all data type combinations with real database
class PreparedStatementRealDataTest : public PreparedStatementIntegrationTest,
                                     public ::testing::WithParamInterface<std::tuple<SQLSMALLINT, std::string, std::string>> {
};

TEST_P(PreparedStatementRealDataTest, RealDatabaseExecution) {
    auto [c_type, test_value, expected_result] = GetParam();
    
    // Prepare statement
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? as test_value", SQL_NTS));
    
    // Bind parameter based on type
    void* param_buffer = nullptr;
    SQLLEN buffer_length = 0;
    
    std::string str_value;
    SQLINTEGER int_value;
    SQLBIGINT bigint_value;
    SQLDOUBLE double_value;
    
    switch (c_type) {
        case SQL_C_CHAR:
            str_value = test_value;
            param_buffer = (void*)str_value.c_str();
            buffer_length = str_value.length();
            break;
        case SQL_C_SLONG:
            int_value = std::stoi(test_value);
            param_buffer = &int_value;
            buffer_length = sizeof(SQLINTEGER);
            break;
        case SQL_C_SBIGINT:
            bigint_value = std::stoll(test_value);
            param_buffer = &bigint_value;
            buffer_length = sizeof(SQLBIGINT);
            break;
        case SQL_C_DOUBLE:
            double_value = std::stod(test_value);
            param_buffer = &double_value;
            buffer_length = sizeof(SQLDOUBLE);
            break;
    }
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, c_type, SQL_VARCHAR, 0, 0, param_buffer, buffer_length, nullptr));
    
    // Execute
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    
    // Fetch and verify result
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    char result[256];
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
    
    EXPECT_STREQ(expected_result.c_str(), result);
}

INSTANTIATE_TEST_SUITE_P(
    RealDataTypes,
    PreparedStatementRealDataTest,
    ::testing::Values(
        std::make_tuple(SQL_C_CHAR, "Hello Database", "Hello Database"),
        std::make_tuple(SQL_C_SLONG, "12345", "12345"),
        std::make_tuple(SQL_C_SBIGINT, "9876543210", "9876543210"),
        std::make_tuple(SQL_C_DOUBLE, "123.456", "123.456"),
        std::make_tuple(SQL_C_CHAR, "", ""),
        std::make_tuple(SQL_C_SLONG, "0", "0"),
        std::make_tuple(SQL_C_SLONG, "-999", "-999")
    )
);

// Test complex queries with multiple parameters
TEST_F(PreparedStatementIntegrationTest, ComplexMultiParameterQuery) {
    // Test arithmetic operations
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? + ? as sum, ? * ? as product, ? as text", SQL_NTS));
    
    SQLINTEGER a = 10, b = 20, c = 3, d = 7;
    std::string text = "Complex Query";
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &a, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &b, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 3, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &c, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 4, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &d, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (void*)text.c_str(), text.length(), nullptr));
    
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    char sum[32], product[32], result_text[256];
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, sum, sizeof(sum), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_CHAR, product, sizeof(product), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_CHAR, result_text, sizeof(result_text), nullptr));
    
    EXPECT_STREQ("30", sum);      // 10 + 20
    EXPECT_STREQ("21", product);  // 3 * 7
    EXPECT_STREQ("Complex Query", result_text);

    SQLSMALLINT parameter_type = 0;
    SQLULEN parameter_size = 0;
    SQLSMALLINT decimal_digits = -1;
    SQLSMALLINT nullable = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLDescribeParam(hstmt, 1, &parameter_type, &parameter_size,
                               &decimal_digits, &nullable));
    EXPECT_EQ(SQL_INTEGER, parameter_type);
    EXPECT_EQ(10u, parameter_size);
    EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);

    ASSERT_EQ(SQL_SUCCESS,
              SQLDescribeParam(hstmt, 5, &parameter_type, &parameter_size,
                               &decimal_digits, &nullable));
    EXPECT_EQ(SQL_VARCHAR, parameter_type);
}

// Test prepared statement reuse
TEST_F(PreparedStatementIntegrationTest, PreparedStatementReuse) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? * 2 as doubled", SQL_NTS));
    
    // Execute with different values
    std::vector<int> test_values = {1, 5, 10};
    
    for (int value : test_values) {
        SQLINTEGER param = value;
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &param, 0, nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        
        char result[32];
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
        
        int expected = value * 2;
        EXPECT_EQ(expected, std::stoi(result));
        
        // Create new statement for next iteration
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? * 2 as doubled", SQL_NTS));
    }
}

// Test string escaping and SQL injection prevention
TEST_F(PreparedStatementIntegrationTest, SqlInjectionPrevention) {
    // Test various potentially malicious inputs
    std::vector<std::string> malicious_inputs = {
        "'; DROP TABLE users; --",
        "' OR '1'='1",
        "Robert'); DROP TABLE students; --"
    };
    
    for (const auto& input : malicious_inputs) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? as user_input", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (void*)input.c_str(), input.length(), nullptr));
        SQLRETURN exec_result = SQLExecute(hstmt);
        
        if (exec_result == SQL_SUCCESS) {
            // If execution succeeds, verify safe handling
            SQLRETURN fetch_result = SQLFetch(hstmt);
            if (fetch_result == SQL_SUCCESS) {
                char result[512];
                ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
                
                // Should return the exact input as a string (properly escaped)
                EXPECT_STREQ(input.c_str(), result);
            }
            // SQL_NO_DATA is also acceptable (no rows returned)
        }
        // Execution failure is also acceptable for malicious input (good security)
        
        // Reset statement
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    }
}

// Test edge cases with real database
TEST_F(PreparedStatementIntegrationTest, EdgeCasesReal) {
    // Very large number
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? as large_number", SQL_NTS));
    SQLBIGINT large_num = 9223372036854775807LL; // Max BIGINT
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &large_num, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    char result[32];
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
    EXPECT_STREQ("9223372036854775807", result);
}

TEST_F(PreparedStatementIntegrationTest, NullParameterUsesProtocolNull) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?::text IS NULL", SQL_NTS));
    SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS,
              SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
                               SQL_VARCHAR, 0, 0, nullptr, 0, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char result[8]{};
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
    EXPECT_STREQ("1", result);
}

TEST_F(PreparedStatementIntegrationTest, ErrorLeavesConnectionSynchronized) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    const char invalid[] = "not-an-integer";
    ASSERT_EQ(SQL_SUCCESS,
              SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
                               SQL_VARCHAR, 0, 0, (SQLPOINTER)invalid,
                               sizeof(invalid) - 1, nullptr));
    SQLUSMALLINT param_status = SQL_PARAM_UNUSED;
    SQLULEN params_processed = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAM_STATUS_PTR, &param_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &params_processed, 0));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ(1u, params_processed);
    EXPECT_EQ(SQL_PARAM_ERROR, param_status);

    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    hstmt = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt));
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 42", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char result[8]{};
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
    EXPECT_STREQ("42", result);
}

TEST_F(PreparedStatementIntegrationTest, NegativeTests) {
    // Test SQLPrepare without connection
    SQLHSTMT disconnected_stmt = reinterpret_cast<SQLHSTMT>(
        static_cast<std::uintptr_t>(1));
    SQLHDBC disconnected_conn;
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &disconnected_conn);
    SQLRETURN ret = SQLAllocHandle(
        SQL_HANDLE_STMT, disconnected_conn, &disconnected_stmt);
    EXPECT_EQ(SQL_ERROR, ret);
    EXPECT_EQ(nullptr, disconnected_stmt);
    
    // Verify diagnostic
    SQLCHAR sqlstate[6], message[256];
    ret = SQLGetDiagRec(SQL_HANDLE_DBC, disconnected_conn, 1, sqlstate,
                        nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("08003", (char*)sqlstate);
    
    SQLFreeHandle(SQL_HANDLE_DBC, disconnected_conn);
    
    // Test SQLExecute without prepare
    ret = SQLExecute(hstmt);
    EXPECT_EQ(SQL_ERROR, ret);
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate,
                        nullptr, message, sizeof(message), nullptr);
    ASSERT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("HY010", reinterpret_cast<char*>(sqlstate));
    
    // Test SQLBindParameter with invalid parameter number
    SQLINTEGER param = 123;
    ret = SQLBindParameter(hstmt, 0, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &param, 0, nullptr);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test SQLDescribeParam without prepare
    SQLSMALLINT data_type;
    SQLULEN param_size;
    ret = SQLDescribeParam(hstmt, 1, &data_type, &param_size, nullptr, nullptr);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test with invalid handles
    ret = SQLPrepare(nullptr, (SQLCHAR*)"SELECT 1", SQL_NTS);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
    
    ret = SQLExecute(nullptr);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
    
    ret = SQLBindParameter(nullptr, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &param, 0, nullptr);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
}
