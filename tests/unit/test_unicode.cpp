#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/unicode.h"

#include <array>
#include <string>

using rs::odbc::sqlwchar_to_utf8;
using rs::odbc::utf8_to_wide;
using rs::odbc::wide_to_utf8;

namespace {

std::string as_utf8(const SQLWCHAR* value, std::size_t length) {
  const auto converted = wide_to_utf8(
      std::span<const SQLWCHAR>(value, length));
  return converted.value_or(std::string{});
}

class UnicodeApiTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  }

  void TearDown() override {
    if (statement_) SQLFreeHandle(SQL_HANDLE_STMT, statement_);
    if (connection_) SQLFreeHandle(SQL_HANDLE_DBC, connection_);
    if (environment_) SQLFreeHandle(SQL_HANDLE_ENV, environment_);
  }

  SQLHENV environment_ = nullptr;
  SQLHDBC connection_ = nullptr;
  SQLHSTMT statement_ = nullptr;
};

} // namespace

TEST(UnicodeConversionTest, RoundTripsBmpAndSupplementaryCharacters) {
  const std::string utf8 =
      "Gr\xc3\xbc\xc3\x9f" "e \xe4\xb8\x96\xe7\x95\x8c "
      "\xf0\x9f\x99\x82";

  const auto wide = utf8_to_wide(utf8);
  ASSERT_TRUE(wide.has_value());
  const auto round_trip = wide_to_utf8(*wide);
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_EQ(utf8, *round_trip);
}

TEST(UnicodeConversionTest, SupportsNullTerminatedAndExplicitLengths) {
  const std::array<SQLWCHAR, 4> input{
      static_cast<SQLWCHAR>('O'), static_cast<SQLWCHAR>('D'),
      static_cast<SQLWCHAR>('B'), 0};

  ASSERT_TRUE(sqlwchar_to_utf8(input.data(), SQL_NTS).has_value());
  EXPECT_EQ("ODB", *sqlwchar_to_utf8(input.data(), SQL_NTS));
  ASSERT_TRUE(sqlwchar_to_utf8(input.data(), 2).has_value());
  EXPECT_EQ("OD", *sqlwchar_to_utf8(input.data(), 2));
  EXPECT_FALSE(sqlwchar_to_utf8(input.data(), -2).has_value());
}

TEST(UnicodeConversionTest, RejectsMalformedUnicodeAndUtf8) {
  const std::array<SQLWCHAR, 1> unpaired_surrogate{
      static_cast<SQLWCHAR>(0xd800)};
  EXPECT_FALSE(wide_to_utf8(unpaired_surrogate).has_value());
  EXPECT_FALSE(utf8_to_wide("\xc0\x80").has_value());
  EXPECT_FALSE(utf8_to_wide("\xed\xa0\x80").has_value());
  EXPECT_FALSE(utf8_to_wide("\xf4\x90\x80\x80").has_value());
}

TEST_F(UnicodeApiTest, ReportsInvalidWideSqlThroughWideDiagnostics) {
  std::array<SQLWCHAR, 1> invalid_sql{static_cast<SQLWCHAR>(0xd800)};
  ASSERT_EQ(SQL_ERROR,
            SQLExecDirectW(statement_, invalid_sql.data(),
                           static_cast<SQLINTEGER>(invalid_sql.size())));

  SQLWCHAR state[6]{};
  SQLWCHAR message[128]{};
  SQLSMALLINT message_length = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagRecW(SQL_HANDLE_STMT, statement_, 1, state, nullptr,
                           message, 128, &message_length));
  EXPECT_EQ("22018", as_utf8(state, 5));
  EXPECT_GT(message_length, 0);
  EXPECT_EQ("Invalid wide-character SQL statement",
            as_utf8(message, static_cast<std::size_t>(message_length)));
}

TEST_F(UnicodeApiTest, WideDiagnosticsReportRequiredLengthAndTruncation) {
  ASSERT_EQ(SQL_ERROR, SQLExecDirectW(statement_, nullptr, 0));

  SQLWCHAR state[6]{};
  SQLWCHAR message[5]{};
  SQLSMALLINT message_length = 0;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLGetDiagRecW(SQL_HANDLE_STMT, statement_, 1, state, nullptr,
                           message, 5, &message_length));
  EXPECT_EQ("HY009", as_utf8(state, 5));
  EXPECT_GT(message_length, 4);
  EXPECT_EQ(static_cast<SQLWCHAR>(0), message[4]);
}

TEST_F(UnicodeApiTest, WideCatalogApisValidateLengthsAndEncoding) {
  auto table = utf8_to_wide("table");
  ASSERT_TRUE(table.has_value());
  ASSERT_EQ(SQL_ERROR,
            SQLTablesW(statement_, nullptr, 0, nullptr, 0, table->data(), -2,
                       nullptr, 0));
  SQLWCHAR state[6]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagRecW(SQL_HANDLE_STMT, statement_, 1, state, nullptr,
                           nullptr, 0, nullptr));
  EXPECT_EQ("HY090", as_utf8(state, 5));

  std::array<SQLWCHAR, 1> invalid{static_cast<SQLWCHAR>(0xd800)};
  ASSERT_EQ(SQL_ERROR,
            SQLColumnsW(statement_, nullptr, 0, nullptr, 0, invalid.data(), 1,
                        nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagRecW(SQL_HANDLE_STMT, statement_, 1, state, nullptr,
                           nullptr, 0, nullptr));
  EXPECT_EQ("22018", as_utf8(state, 5));
}

TEST_F(UnicodeApiTest, WideInformationUsesByteLengths) {
  SQLWCHAR driver_name[32]{};
  SQLSMALLINT name_bytes = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetInfoW(connection_, SQL_DRIVER_NAME, driver_name,
                        sizeof(driver_name), &name_bytes));
  EXPECT_EQ(static_cast<SQLSMALLINT>(13 * sizeof(SQLWCHAR)), name_bytes);
  EXPECT_EQ("ODBCPP Driver", as_utf8(driver_name, 13));
}

TEST_F(UnicodeApiTest, WideDiagnosticFieldAndSqLErrorAreCompatible) {
  ASSERT_EQ(SQL_ERROR, SQLExecDirectW(statement_, nullptr, 0));
  SQLWCHAR message[64]{};
  SQLSMALLINT message_bytes = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetDiagFieldW(SQL_HANDLE_STMT, statement_, 1,
                             SQL_DIAG_MESSAGE_TEXT, message,
                             sizeof(message), &message_bytes));
  EXPECT_EQ("SQL statement is null",
            as_utf8(message, static_cast<std::size_t>(message_bytes) /
                                 sizeof(SQLWCHAR)));

  SQLWCHAR state[6]{};
  SQLSMALLINT message_units = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLErrorW(nullptr, nullptr, statement_, state, nullptr, message,
                      64, &message_units));
  EXPECT_EQ("HY009", as_utf8(state, 5));
  EXPECT_EQ(SQL_NO_DATA,
            SQLErrorW(nullptr, nullptr, statement_, state, nullptr, message,
                      64, &message_units));
}
