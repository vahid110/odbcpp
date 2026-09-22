#include <gtest/gtest.h>
#include "odbc/result_types.h"
#include "odbc/text_data_converter.h"
#include "odbc/unicode.h"
#include "tests/test_time_helpers.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ctime>
#include <cstring>
#include <initializer_list>
#include <limits>

using RedshiftDataConverter = rs::odbc::TextDataConverter;

class RedshiftDataConverterTest : public ::testing::Test {
protected:
    char buffer[256];
    SQLLEN indicator;
};

TEST_F(RedshiftDataConverterTest, NumericStructureIsExactAndLittleEndian) {
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "123.45", SQL_C_NUMERIC, &numeric, 0, &length, &issue, 5, 2));
    EXPECT_EQ(5, numeric.precision);
    EXPECT_EQ(2, numeric.scale);
    EXPECT_EQ(1, numeric.sign);
    EXPECT_EQ(0x39, numeric.val[0]);
    EXPECT_EQ(0x30, numeric.val[1]);
    EXPECT_TRUE(std::all_of(std::begin(numeric.val) + 2,
        std::end(numeric.val), [](SQLCHAR byte) { return byte == 0; }));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "-123.45", SQL_C_NUMERIC, &numeric, 0, &length, &issue, 5, 2));
    EXPECT_EQ(0, numeric.sign);
    EXPECT_EQ(0x39, numeric.val[0]);
    EXPECT_EQ(0x30, numeric.val[1]);
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "-0.00", SQL_C_NUMERIC, &numeric, 0, &length, &issue, 5, 2));
    EXPECT_EQ(1, numeric.sign);
}

TEST_F(RedshiftDataConverterTest, NumericStructureTruncationAndErrors) {
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "12.34", SQL_C_NUMERIC, &numeric, 0, &length, &issue));
    EXPECT_EQ(38, numeric.precision);
    EXPECT_EQ(0, numeric.scale);
    EXPECT_EQ(12, numeric.val[0]);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "12.00", SQL_C_NUMERIC, &numeric, 0, &length, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    for (const auto* invalid : {"100", "999999999999999999999999999999999999999"}) {
        std::memset(&numeric, 0x5a, sizeof(numeric));
        length = 91;
        issue = rs::odbc::ConversionIssue::None;
        const SQLSMALLINT precision = std::strcmp(invalid, "100") == 0 ? 2 : 38;
        EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
            invalid, SQL_C_NUMERIC, &numeric, 0, &length, &issue,
            precision, 0));
        EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);
        EXPECT_EQ(91, length);
        EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
            reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
            [](unsigned char byte) { return byte == 0x5a; }));
    }
    for (const auto* invalid : {"NaN", "1e2", "."}) {
        issue = rs::odbc::ConversionIssue::None;
        EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
            invalid, SQL_C_NUMERIC, &numeric, 0, &length, &issue));
        EXPECT_EQ(rs::odbc::ConversionIssue::InvalidCharacterValue, issue);
        EXPECT_EQ(91, length);
    }
    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "1", SQL_C_NUMERIC, &numeric, 0, &length, &issue, 2, 3));
    EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);
}

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

TEST_F(RedshiftDataConverterTest, BigintLimitsStayExact) {
    SQLBIGINT value = 0;
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "9223372036854775807", SQL_C_SBIGINT, &value, 0, &length,
        &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "+9223372036854775807", SQL_C_SBIGINT, &value, 0, &length,
        &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);

    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "-9223372036854775808", SQL_C_SBIGINT, &value, 0, &length,
        &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "9223372036854775807.9", SQL_C_SBIGINT, &value, 0, &length,
        &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "-9223372036854775808.9", SQL_C_SBIGINT, &value, 0, &length,
        &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    for (const auto* out_of_range : {
             "9223372036854775808", "-9223372036854775809",
             "9223372036854775808.1", "-9223372036854775809.1"}) {
        value = 73;
        length = 74;
        issue = rs::odbc::ConversionIssue::None;
        EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
            out_of_range, SQL_C_SBIGINT, &value, 0, &length, &issue));
        EXPECT_EQ(73, value);
        EXPECT_EQ(74, length);
        EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);
    }
}

TEST_F(RedshiftDataConverterTest, BigintScientificNotationStaysExact) {
    SQLBIGINT value = 0;
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "9.223372036854775807e18", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);

    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "-9.223372036854775808e18", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "9.2233720368547758079e18", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    for (const auto* out_of_range : {
             "9.223372036854775808e18", "-9.223372036854775809e18"}) {
        value = 73;
        length = 74;
        issue = rs::odbc::ConversionIssue::None;
        EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
            out_of_range, SQL_C_SBIGINT, &value, 0, &length, &issue));
        EXPECT_EQ(73, value);
        EXPECT_EQ(74, length);
        EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);
    }

    value = 0;
    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "1.25e2", SQL_C_SBIGINT, &value, 0, &length, &issue));
    EXPECT_EQ(125, value);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "1.25e1", SQL_C_SBIGINT, &value, 0, &length, &issue));
    EXPECT_EQ(12, value);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "0e999999999999999999999", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(0, value);
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "1e-999999999999999999999", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(0, value);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    value = 73;
    length = 74;
    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "1e999999999999999999999", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);

    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "1e+", SQL_C_SBIGINT, &value, 0, &length, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidCharacterValue, issue);
}

TEST_F(RedshiftDataConverterTest, SmallintScientificNotationChecksRange) {
    SQLSMALLINT value = 0;
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "3.2767E+4", SQL_C_SSHORT, &value, 0, &length, &issue));
    EXPECT_EQ(std::numeric_limits<SQLSMALLINT>::max(), value);

    value = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "3.2768E+4", SQL_C_SSHORT, &value, 0, &length, &issue));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);
}

TEST_F(RedshiftDataConverterTest, LeadingWhitespaceKeepsBigintExact) {
    SQLBIGINT value = 0;
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " \t9223372036854775807", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);

    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " \t-9.223372036854775808e18", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        " \t9.2233720368547758079e18", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    value = 73;
    length = 74;
    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        " \t9.223372036854775808e18", SQL_C_SBIGINT, &value, 0,
        &length, &issue));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);

    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        " \t1e2 x", SQL_C_SBIGINT, &value, 0, &length, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidCharacterValue, issue);
}

TEST_F(RedshiftDataConverterTest, CharacterConversionsIgnoreOuterWhitespace) {
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;

    SQLBIGINT integer = 0;
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " \t9223372036854775807 \t", SQL_C_SBIGINT, &integer, 0,
        &length, &issue));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), integer);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer)), length);

    SQLDOUBLE floating = 0;
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " 1.25e2 \t", SQL_C_DOUBLE, &floating, 0, &length, &issue));
    EXPECT_DOUBLE_EQ(125.0, floating);

    SQL_DATE_STRUCT date{};
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " 2024-02-29 ", SQL_C_DATE, &date, 0, &length, &issue));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);

    SQL_TIME_STRUCT time{};
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " 12:34:56 ", SQL_C_TIME, &time, 0, &length, &issue));
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(34, time.minute);
    EXPECT_EQ(56, time.second);

    SQL_TIMESTAMP_STRUCT timestamp{};
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " 2024-02-29 12:34:56 ", SQL_C_TIMESTAMP, &timestamp,
        0, &length, &issue));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(12, timestamp.hour);

    integer = 73;
    length = 74;
    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        " \t ", SQL_C_SBIGINT, &integer, 0, &length, &issue));
    EXPECT_EQ(73, integer);
    EXPECT_EQ(74, length);
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidCharacterValue, issue);
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

TEST_F(RedshiftDataConverterTest, ClassifiesNumericConversionIssues) {
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    SQLSMALLINT result = 0;

    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "32768", SQL_C_SSHORT, &result, 0, &indicator, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "12.75", SQL_C_SSHORT, &result, 0, &indicator, &issue));
    EXPECT_EQ(12, result);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "not-a-number", SQL_C_SSHORT, &result, 0, &indicator, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidCharacterValue, issue);
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

TEST_F(RedshiftDataConverterTest, ClassifiesInvalidDatetimeFormat) {
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    SQL_DATE_STRUCT result{};
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "2025-02-29", SQL_C_DATE, &result, 0, &indicator, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidDatetimeFormat, issue);
}

TEST_F(RedshiftDataConverterTest, TimestampToDateReportsLostTime) {
    SQL_DATE_STRUCT date{};
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;

    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "2024-02-29 00:00:00.000000", SQL_C_DATE, &date, 0,
        &length, &issue));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), length);
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    for (const char* input : {
             "2024-02-29 12:00:00", "2024-02-29 00:00:01",
             "2024-02-29 00:00:00.0000000001"}) {
        issue = rs::odbc::ConversionIssue::None;
        ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
            input, SQL_C_DATE, &date, 0, &length, &issue));
        EXPECT_EQ(29, date.day);
        EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);
    }

    for (const char* input : {
             "2024-02-30 12:00:00", "2024-02-29 24:00:00",
             "2024-02-29 12:00:00 junk"}) {
        date.year = 73;
        length = 74;
        issue = rs::odbc::ConversionIssue::None;
        EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
            input, SQL_C_DATE, &date, 0, &length, &issue));
        EXPECT_EQ(73, date.year);
        EXPECT_EQ(74, length);
        EXPECT_EQ(rs::odbc::ConversionIssue::InvalidDatetimeFormat, issue);
    }
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

TEST_F(RedshiftDataConverterTest, DateToTimestampZeroesTimeFields) {
    SQL_TIMESTAMP_STRUCT timestamp{};
    timestamp.hour = 73;
    timestamp.minute = 74;
    timestamp.second = 75;
    timestamp.fraction = 76;
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;

    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " 2024-02-29 ", SQL_C_TIMESTAMP, &timestamp, 0,
        &length, &issue));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(0, timestamp.hour);
    EXPECT_EQ(0, timestamp.minute);
    EXPECT_EQ(0, timestamp.second);
    EXPECT_EQ(0u, timestamp.fraction);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), length);
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    timestamp.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "2024-02-30", SQL_C_TIMESTAMP, &timestamp, 0,
        &length, &issue));
    EXPECT_EQ(73, timestamp.year);
    EXPECT_EQ(74, length);
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidDatetimeFormat, issue);
}

TEST_F(RedshiftDataConverterTest, TimeToTimestampUsesCurrentLocalDate) {
    const auto before = odbcpp::test::local_calendar(std::time(nullptr));
    ASSERT_TRUE(before.has_value());

    SQL_TIMESTAMP_STRUCT timestamp{};
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        " 12:34:56 ", SQL_C_TIMESTAMP, &timestamp, 0,
        &length, &issue));

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
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    timestamp.hour = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "25:00:00", SQL_C_TIMESTAMP, &timestamp, 0,
        &length, &issue));
    EXPECT_EQ(73, timestamp.hour);
    EXPECT_EQ(74, length);
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidDatetimeFormat, issue);
}

TEST_F(RedshiftDataConverterTest, ConvertDataTime) {
    SQL_TIME_STRUCT result{};
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "23:45:30.123456+02:00", SQL_C_TIME, &result, 0,
        &indicator, &issue));
    EXPECT_EQ(23, result.hour);
    EXPECT_EQ(45, result.minute);
    EXPECT_EQ(30, result.second);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "24:00:00", SQL_C_TIME, &result, 0, &indicator));
}

TEST_F(RedshiftDataConverterTest, TemporalFractionTruncationIsReported) {
    SQLLEN length = -1;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    SQL_TIME_STRUCT time{};

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "12:34:56.123456", SQL_C_TIME, &time, 0, &length, &issue));
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(time)), length);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "2024-02-29 12:34:56.123456", SQL_C_TIME, &time, 0,
        &length, &issue));
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "12:34:56.0000000001", SQL_C_TIME, &time, 0,
        &length, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "12:34:56.000000", SQL_C_TIME, &time, 0, &length, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    SQL_TIMESTAMP_STRUCT timestamp{};
    issue = rs::odbc::ConversionIssue::None;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "2024-02-29 12:34:56.1234567891", SQL_C_TIMESTAMP,
        &timestamp, 0, &length, &issue));
    EXPECT_EQ(123456789u, timestamp.fraction);
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);

    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "2024-02-29 12:34:56.1234567890", SQL_C_TIMESTAMP,
        &timestamp, 0, &length, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::None, issue);

    time.hour = 73;
    length = 74;
    issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "2024-02-30 12:34:56.123456", SQL_C_TIME, &time, 0,
        &length, &issue));
    EXPECT_EQ(73, time.hour);
    EXPECT_EQ(74, length);
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidDatetimeFormat, issue);
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

TEST_F(RedshiftDataConverterTest, CharacterBuffersMayHaveNoTerminatorSpace) {
    char narrow = 'x';
    indicator = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "", SQL_C_CHAR, &narrow, 0, &indicator));
    EXPECT_EQ('x', narrow);
    EXPECT_EQ(0, indicator);

    SQLWCHAR wide = static_cast<SQLWCHAR>('x');
    indicator = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "", SQL_C_WCHAR, &wide, sizeof(SQLWCHAR) - 1, &indicator));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide);
    EXPECT_EQ(0, indicator);

    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "", SQL_C_WCHAR, &wide, sizeof(SQLWCHAR), &indicator));
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide);
}

TEST_F(RedshiftDataConverterTest, ConvertDataWideString) {
    const std::string utf8 =
        "Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x99\x82";
    SQLWCHAR wide_buffer[32]{};
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        utf8, SQL_C_WCHAR, wide_buffer, sizeof(wide_buffer), &indicator));
    EXPECT_EQ(static_cast<SQLLEN>(
                  rs::odbc::utf8_to_wide(utf8)->size() * sizeof(SQLWCHAR)),
              indicator);
    const auto converted = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            wide_buffer, static_cast<std::size_t>(indicator) /
                             sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted.has_value());
    EXPECT_EQ(utf8, *converted);

    SQLWCHAR truncated[3]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        utf8, SQL_C_WCHAR, truncated, sizeof(truncated), &indicator));
    EXPECT_EQ(static_cast<SQLWCHAR>(0), truncated[2]);
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "\xc0\x80", SQL_C_WCHAR, wide_buffer, sizeof(wide_buffer),
        &indicator));
}

TEST_F(RedshiftDataConverterTest, WideOutputNeedNotBeAligned) {
    alignas(SQLWCHAR) std::array<std::byte, 1 + 3 * sizeof(SQLWCHAR)> storage{};
    void* output = storage.data() + 1;
    SQLLEN length = -1;

    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "AB", SQL_C_WCHAR, output, 3 * sizeof(SQLWCHAR), &length));
    EXPECT_EQ(2 * static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);

    SQLWCHAR units[3]{};
    std::memcpy(units, output, sizeof(units));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), units[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('B'), units[1]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), units[2]);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "AB", SQL_C_WCHAR, output, 2 * sizeof(SQLWCHAR), &length));
    EXPECT_EQ(2 * static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
    std::memcpy(units, output, 2 * sizeof(SQLWCHAR));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), units[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), units[1]);
}

TEST_F(RedshiftDataConverterTest, NumericOutputNeedNotBeAligned) {
    alignas(SQLDOUBLE) std::array<std::byte, 1 + sizeof(SQLDOUBLE)> storage{};
    void* output = storage.data() + 1;
    SQLLEN length = -1;

    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "42", SQL_C_SLONG, output, 0, &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)), length);
    SQLINTEGER integer = 0;
    std::memcpy(&integer, output, sizeof(integer));
    EXPECT_EQ(42, integer);

    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "1.5", SQL_C_DOUBLE, output, 0, &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLDOUBLE)), length);
    SQLDOUBLE floating = 0;
    std::memcpy(&floating, output, sizeof(floating));
    EXPECT_DOUBLE_EQ(1.5, floating);

    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "2.5", SQL_C_SLONG, output, 0, &length, &issue));
    EXPECT_EQ(rs::odbc::ConversionIssue::FractionalTruncation, issue);
    std::memcpy(&integer, output, sizeof(integer));
    EXPECT_EQ(2, integer);

    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "invalid", SQL_C_SLONG, output, 0, &length));
    std::memcpy(&integer, output, sizeof(integer));
    EXPECT_EQ(2, integer);
}

TEST_F(RedshiftDataConverterTest, DateTimeOutputNeedNotBeAligned) {
    alignas(SQL_TIMESTAMP_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_TIMESTAMP_STRUCT)> storage{};
    void* output = storage.data() + 1;
    SQLLEN length = -1;

    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "2024-02-29", SQL_C_DATE, output, 0, &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)), length);
    SQL_DATE_STRUCT date{};
    std::memcpy(&date, output, sizeof(date));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);

    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "12:34:56", SQL_C_TIME, output, 0, &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIME_STRUCT)), length);
    SQL_TIME_STRUCT time{};
    std::memcpy(&time, output, sizeof(time));
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(34, time.minute);
    EXPECT_EQ(56, time.second);

    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "2024-02-29 12:34:56.123456", SQL_C_TIMESTAMP,
        output, 0, &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT)), length);
    SQL_TIMESTAMP_STRUCT timestamp{};
    std::memcpy(&timestamp, output, sizeof(timestamp));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
    EXPECT_EQ(123456000u, timestamp.fraction);

    const auto previous_output = storage;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "2024-02-30 12:34:56", SQL_C_TIMESTAMP, output, 0, &length));
    EXPECT_EQ(previous_output, storage);
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

TEST_F(RedshiftDataConverterTest, NumericTextToBitUsesExactRange) {
    struct Case {
        const char* text;
        SQLRETURN result;
        SQLCHAR bit;
        rs::odbc::ConversionIssue issue;
    };
    const std::array<Case, 7> accepted{{
        {"0", SQL_SUCCESS, 0, rs::odbc::ConversionIssue::None},
        {"1", SQL_SUCCESS, 1, rs::odbc::ConversionIssue::None},
        {"-0.000", SQL_SUCCESS, 0, rs::odbc::ConversionIssue::None},
        {"0.5", SQL_SUCCESS_WITH_INFO, 0,
         rs::odbc::ConversionIssue::FractionalTruncation},
        {"1.5", SQL_SUCCESS_WITH_INFO, 1,
         rs::odbc::ConversionIssue::FractionalTruncation},
        {"1.999999999999999999999", SQL_SUCCESS_WITH_INFO, 1,
         rs::odbc::ConversionIssue::FractionalTruncation},
        {" 1e-100 ", SQL_SUCCESS_WITH_INFO, 0,
         rs::odbc::ConversionIssue::FractionalTruncation},
    }};
    SQLCHAR bit = 73;
    SQLLEN length = 74;
    rs::odbc::ConversionIssue issue = rs::odbc::ConversionIssue::None;
    for (const auto& test : accepted) {
        SCOPED_TRACE(test.text);
        ASSERT_EQ(test.result, RedshiftDataConverter::convert_data(
            test.text, SQL_C_BIT, &bit, 0, &length, &issue));
        EXPECT_EQ(test.bit, bit);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(bit)), length);
        EXPECT_EQ(test.issue, issue);
    }

    for (const char* text : {"-0.5", "-1e-100", "2", "1e2"}) {
        SCOPED_TRACE(text);
        bit = 73;
        length = 74;
        issue = rs::odbc::ConversionIssue::None;
        EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
            text, SQL_C_BIT, &bit, 0, &length, &issue));
        EXPECT_EQ(73, bit);
        EXPECT_EQ(74, length);
        EXPECT_EQ(rs::odbc::ConversionIssue::NumericValueOutOfRange, issue);
    }

    bit = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data(
        "1.5x", SQL_C_BIT, &bit, 0, &length, &issue));
    EXPECT_EQ(73, bit);
    EXPECT_EQ(74, length);
    EXPECT_EQ(rs::odbc::ConversionIssue::InvalidCharacterValue, issue);
}

TEST_F(RedshiftDataConverterTest, ConvertsPostgresqlByteaText) {
    unsigned char binary[4]{};
    EXPECT_EQ(SQL_SUCCESS, RedshiftDataConverter::convert_data(
        "\\x00017fFF", SQL_C_BINARY, binary, sizeof(binary), &indicator));
    EXPECT_EQ(4, indicator);
    EXPECT_EQ(0x00, binary[0]);
    EXPECT_EQ(0x01, binary[1]);
    EXPECT_EQ(0x7f, binary[2]);
    EXPECT_EQ(0xff, binary[3]);

    unsigned char truncated[2]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, RedshiftDataConverter::convert_data(
        "\\x00017fff", SQL_C_BINARY, truncated, sizeof(truncated), &indicator));
    EXPECT_EQ(4, indicator);
    EXPECT_EQ(0x00, truncated[0]);
    EXPECT_EQ(0x01, truncated[1]);

    const auto escaped = RedshiftDataConverter::decode_binary("A\\\\B\\000");
    ASSERT_TRUE(escaped.has_value());
    ASSERT_EQ(4u, escaped->size());
    EXPECT_EQ(std::byte{'A'}, (*escaped)[0]);
    EXPECT_EQ(std::byte{'\\'}, (*escaped)[1]);
    EXPECT_EQ(std::byte{'B'}, (*escaped)[2]);
    EXPECT_EQ(std::byte{0}, (*escaped)[3]);
    EXPECT_FALSE(RedshiftDataConverter::decode_binary("\\x123").has_value());

    const std::byte source[]{
        std::byte{0}, std::byte{1}, std::byte{0x7f}, std::byte{0xff}};
    EXPECT_EQ("\\x00017fff", RedshiftDataConverter::encode_binary(source));
}

TEST(ResultTypesTest, ProvidesMetadataDrivenDefaults) {
    EXPECT_EQ(SQL_C_STINYINT,
              rs::odbc::ResultTypes::default_c_type(SQL_TINYINT));
    EXPECT_EQ(SQL_C_SSHORT, rs::odbc::ResultTypes::default_c_type(SQL_SMALLINT));
    EXPECT_EQ(SQL_C_SLONG, rs::odbc::ResultTypes::default_c_type(SQL_INTEGER));
    EXPECT_EQ(SQL_C_SBIGINT, rs::odbc::ResultTypes::default_c_type(SQL_BIGINT));
    EXPECT_EQ(SQL_C_FLOAT, rs::odbc::ResultTypes::default_c_type(SQL_REAL));
    EXPECT_EQ(SQL_C_DOUBLE, rs::odbc::ResultTypes::default_c_type(SQL_DOUBLE));
    EXPECT_EQ(SQL_C_DATE, rs::odbc::ResultTypes::default_c_type(SQL_TYPE_DATE));
    EXPECT_EQ(SQL_C_BINARY,
              rs::odbc::ResultTypes::default_c_type(SQL_VARBINARY));
    for (const SQLSMALLINT sql_type : std::initializer_list<SQLSMALLINT>{SQL_WCHAR, SQL_WVARCHAR,
                                SQL_WLONGVARCHAR}) {
        EXPECT_EQ(SQL_C_WCHAR,
                  rs::odbc::ResultTypes::default_c_type(sql_type));
    }
    EXPECT_EQ(SQL_C_CHAR, rs::odbc::ResultTypes::default_c_type(SQL_NUMERIC));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_INTEGER, SQL_C_SLONG));
    EXPECT_FALSE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_INTEGER, SQL_C_BINARY));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_VARBINARY, SQL_C_BINARY));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_NUMERIC, SQL_C_NUMERIC));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_INTEGER, SQL_C_NUMERIC));
    EXPECT_FALSE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_VARCHAR, SQL_C_NUMERIC));
    EXPECT_FALSE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_DOUBLE, SQL_C_NUMERIC));
    for (SQLSMALLINT temporal : std::initializer_list<SQLSMALLINT>{
             SQL_TYPE_DATE, SQL_TYPE_TIME, SQL_TYPE_TIMESTAMP}) {
        for (SQLSMALLINT numeric : std::initializer_list<SQLSMALLINT>{
                 SQL_C_BIT, SQL_C_SSHORT, SQL_C_SLONG, SQL_C_SBIGINT,
                 SQL_C_FLOAT, SQL_C_DOUBLE}) {
            EXPECT_FALSE(rs::odbc::ResultTypes::is_conversion_supported(
                temporal, numeric));
        }
        EXPECT_TRUE(rs::odbc::ResultTypes::is_conversion_supported(
            temporal, SQL_C_CHAR));
    }
    EXPECT_FALSE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_TYPE_DATE, SQL_C_TYPE_TIME));
    EXPECT_FALSE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_TYPE_TIME, SQL_C_TYPE_DATE));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_TYPE_DATE, SQL_C_TYPE_TIMESTAMP));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_conversion_supported(
        SQL_TYPE_TIME, SQL_C_TYPE_TIMESTAMP));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_c_type(
        SQL_C_TYPE_DATE));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_c_type(
        SQL_C_DATE));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_sql_type(
        SQL_TYPE_DATE));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_c_type(
        SQL_C_TYPE_TIME));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_c_type(
        SQL_C_TIME));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_sql_type(
        SQL_TYPE_TIME));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_c_type(
        SQL_C_TYPE_TIMESTAMP));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_c_type(
        SQL_C_TIMESTAMP));
    EXPECT_TRUE(rs::odbc::ResultTypes::is_supported_parameter_sql_type(
        SQL_TYPE_TIMESTAMP));
}

TEST_F(RedshiftDataConverterTest, UnsupportedType) {
    SQLINTEGER result;
    
    // Unsupported C type
    EXPECT_EQ(SQL_ERROR, RedshiftDataConverter::convert_data("42", 999, &result, 0, &indicator));
}
