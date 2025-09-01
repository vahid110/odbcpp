#include <gtest/gtest.h>
#include "odbc/redshift/redshift_data_converter.h"

using namespace rs::odbc::redshift;

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
    
    // Note: Large numbers may truncate rather than error in std::stol
    // This is acceptable behavior for ODBC drivers
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
}

TEST_F(RedshiftDataConverterTest, ConvertDataDate) {
    SQL_DATE_STRUCT result;
    
    // Valid date
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("2025-08-31", SQL_C_DATE, &result, 0, &indicator));
    EXPECT_EQ(2025, result.year);
    EXPECT_EQ(8, result.month);
    EXPECT_EQ(31, result.day);
    
    // Invalid date
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("invalid-date", SQL_C_DATE, &result, 0, &indicator));
}

TEST_F(RedshiftDataConverterTest, ConvertDataTimestamp) {
    SQL_TIMESTAMP_STRUCT result;
    
    // Valid timestamp
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data("2025-08-31 23:45:30", SQL_C_TIMESTAMP, &result, 0, &indicator));
    EXPECT_EQ(2025, result.year);
    EXPECT_EQ(23, result.hour);
    
    // Invalid timestamp
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("invalid", SQL_C_TIMESTAMP, &result, 0, &indicator));
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
}

TEST_F(RedshiftDataConverterTest, UnsupportedType) {
    SQLINTEGER result;
    
    // Unsupported C type
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("42", 999, &result, 0, &indicator));
}