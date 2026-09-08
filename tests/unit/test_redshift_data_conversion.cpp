#include <gtest/gtest.h>
#include "odbc/result_types.h"
#include "odbc/text_data_converter.h"

using RedshiftDataConverter = rs::odbc::TextDataConverter;

class RedshiftDataConverterTest : public ::testing::Test {
protected:
    char buffer[256];
    SQLLEN indicator;
};

// Test through public interface only - essential edge cases
TEST_F(RedshiftDataConverterTest, ConvertDataInteger) {
    SQLINTEGER result;
    
    // Valid integer
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("42", SQL_C_SLONG, &result, 0, &indicator));
    EXPECT_EQ(42, result);
    
    // Invalid integer
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("not_a_number", SQL_C_SLONG, &result, 0, &indicator));
    
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "2147483648", SQL_C_SLONG, &result, 0, &indicator));
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "42 trailing", SQL_C_SLONG, &result, 0, &indicator));
}

TEST_F(RedshiftDataConverterTest, ConvertDataBigint) {
    SQLBIGINT result;
    
    // Valid bigint
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("1234567890123", SQL_C_SBIGINT, &result, 0, &indicator));
    EXPECT_EQ(1234567890123LL, result);
    
    // Invalid bigint
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("invalid", SQL_C_SBIGINT, &result, 0, &indicator));
}

TEST_F(RedshiftDataConverterTest, ConvertDataDouble) {
    SQLDOUBLE result;
    
    // Valid double
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("3.14159", SQL_C_DOUBLE, &result, 0, &indicator));
    EXPECT_NEAR(3.14159, result, 0.00001);
    
    // Invalid double
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("not_a_double", SQL_C_DOUBLE, &result, 0, &indicator));
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("3.5x", SQL_C_DOUBLE, &result, 0, &indicator));
}

TEST_F(RedshiftDataConverterTest, ConvertDataSmallintAndFloat) {
    SQLSMALLINT smallint_result = 0;
    SQLREAL float_result = 0;
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "-123", SQL_C_SSHORT, &smallint_result, 0, &indicator));
    EXPECT_EQ(-123, smallint_result);
    EXPECT_EQ(sizeof(SQLSMALLINT), indicator);
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "32768", SQL_C_SSHORT, &smallint_result, 0, &indicator));
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "1.25", SQL_C_FLOAT, &float_result, 0, &indicator));
    EXPECT_FLOAT_EQ(1.25f, float_result);
}

TEST_F(RedshiftDataConverterTest, ConvertDataDate) {
    SQL_DATE_STRUCT result;
    
    // Valid date
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("2025-08-31", SQL_C_DATE, &result, 0, &indicator));
    EXPECT_EQ(2025, result.year);
    EXPECT_EQ(8, result.month);
    EXPECT_EQ(31, result.day);

    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("2025-13-31", SQL_C_DATE, &result, 0, &indicator));
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("2025-02-29", SQL_C_DATE, &result, 0, &indicator));
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("2024-02-29", SQL_C_DATE, &result, 0, &indicator));
    
    // Invalid date
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("invalid-date", SQL_C_DATE, &result, 0, &indicator));
}

TEST_F(RedshiftDataConverterTest, ConvertDataTimestamp) {
    SQL_TIMESTAMP_STRUCT result;
    
    // Valid timestamp
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("2025-08-31 23:45:30", SQL_C_TIMESTAMP, &result, 0, &indicator));
    EXPECT_EQ(2025, result.year);
    EXPECT_EQ(23, result.hour);

    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "2025-08-31 23:45:30.020257+00", SQL_C_TIMESTAMP, &result, 0, &indicator));
    EXPECT_EQ(20257000u, result.fraction);
    
    // Invalid timestamp
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("invalid", SQL_C_TIMESTAMP, &result, 0, &indicator));
}

TEST_F(RedshiftDataConverterTest, ConvertDataTime) {
    SQL_TIME_STRUCT result{};
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "23:45:30.123456+02:00", SQL_C_TIME, &result, 0, &indicator));
    EXPECT_EQ(23, result.hour);
    EXPECT_EQ(45, result.minute);
    EXPECT_EQ(30, result.second);
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "24:00:00", SQL_C_TIME, &result, 0, &indicator));
}

TEST_F(RedshiftDataConverterTest, ConvertDataString) {
    // Normal string
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("Hello", SQL_C_CHAR, buffer, 256, &indicator));
    EXPECT_STREQ("Hello", buffer);
    EXPECT_EQ(5, indicator);
    
    // Truncation
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data("Very long string", SQL_C_CHAR, buffer, 5, &indicator));
    EXPECT_STREQ("Very", buffer);
}

TEST_F(RedshiftDataConverterTest, ConvertDataBoolean) {
    SQLCHAR result;
    
    // Redshift boolean formats
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("t", SQL_C_BIT, &result, 0, &indicator));
    EXPECT_EQ(1, result);
    
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("f", SQL_C_BIT, &result, 0, &indicator));
    EXPECT_EQ(0, result);
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("maybe", SQL_C_BIT, &result, 0, &indicator));
}

TEST(ResultTypesTest, ProvidesMetadataDrivenDefaults) {
    EXPECT_EQ(SQL_C_SSHORT, rs::odbc::ResultTypes::default_c_type(SQL_SMALLINT));
    EXPECT_EQ(SQL_C_SLONG, rs::odbc::ResultTypes::default_c_type(SQL_INTEGER));
    EXPECT_EQ(SQL_C_SBIGINT, rs::odbc::ResultTypes::default_c_type(SQL_BIGINT));
    EXPECT_EQ(SQL_C_FLOAT, rs::odbc::ResultTypes::default_c_type(SQL_REAL));
    EXPECT_EQ(SQL_C_DOUBLE, rs::odbc::ResultTypes::default_c_type(SQL_DOUBLE));
    EXPECT_EQ(SQL_C_DATE, rs::odbc::ResultTypes::default_c_type(SQL_TYPE_DATE));
    EXPECT_EQ(SQL_C_CHAR, rs::odbc::ResultTypes::default_c_type(SQL_NUMERIC));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_INTEGER, SQL_C_SLONG));
    EXPECT_FALSE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_INTEGER, SQL_C_BINARY));
}

TEST_F(RedshiftDataConverterTest, UnsupportedType) {
    SQLINTEGER result;
    
    // Unsupported C type
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("42", 999, &result, 0, &indicator));
}
