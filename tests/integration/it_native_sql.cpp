#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/unicode.h"
#include "tests/test_connection_config.h"

#include <array>
#include <cstring>
#include <string>

namespace {

std::string diagnostic_state(SQLSMALLINT type, SQLHANDLE handle) {
  SQLCHAR state[6]{};
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(type, handle, 1, state, nullptr, nullptr, 0,
                          nullptr));
  return reinterpret_cast<const char*>(state);
}

class NativeSqlIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    if (SQLConnect(connection_,
                   odbcpp::test::configured_connection_string_data(), SQL_NTS,
                   nullptr, 0, nullptr, 0) != SQL_SUCCESS) {
      GTEST_SKIP() << "Database connection failed";
    }
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  }

  void TearDown() override {
    if (statement_) SQLFreeHandle(SQL_HANDLE_STMT, statement_);
    if (connection_) {
      SQLDisconnect(connection_);
      SQLFreeHandle(SQL_HANDLE_DBC, connection_);
    }
    if (environment_) SQLFreeHandle(SQL_HANDLE_ENV, environment_);
  }

  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  SQLHSTMT statement_{SQL_NULL_HSTMT};
};

} // namespace

TEST_F(NativeSqlIntegrationTest, ReturnsTranslatedAnsiSqlAndExactLength) {
  SQLCHAR input[] = "SELECT {fn UCASE('mixed')}, {d '2024-02-29'}";
  constexpr char expected[] = "SELECT UPPER('mixed'), DATE '2024-02-29'";
  SQLINTEGER length = -1;
  ASSERT_EQ(SQL_SUCCESS,
            SQLNativeSql(connection_, input, SQL_NTS, nullptr, -7, &length));
  EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(expected) - 1), length);

  std::array<SQLCHAR, sizeof(expected)> output{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLNativeSql(connection_, input, SQL_NTS, output.data(),
                         static_cast<SQLINTEGER>(output.size()), &length));
  EXPECT_STREQ(expected, reinterpret_cast<const char*>(output.data()));

  auto wide_input = rs::odbc::utf8_to_wide("SELECT {t '12:34:56'} trailing");
  ASSERT_TRUE(wide_input.has_value());
  std::array<SQLWCHAR, 32> wide_output{};
  length = -1;
  ASSERT_EQ(SQL_SUCCESS,
            SQLNativeSqlW(connection_, wide_input->data(), 21,
                          wide_output.data(),
                          static_cast<SQLINTEGER>(wide_output.size()),
                          &length));
  EXPECT_EQ(22, length);
  EXPECT_EQ("SELECT TIME '12:34:56'",
            *rs::odbc::wide_to_utf8(
                std::span<const SQLWCHAR>(wide_output.data(), length)));

  length = -1;
  EXPECT_EQ(SQL_SUCCESS,
            SQLNativeSqlW(connection_, wide_input->data(), 21, nullptr, -9,
                          &length));
  EXPECT_EQ(22, length);
}

TEST_F(NativeSqlIntegrationTest, ReportsAnsiAndWideTruncation) {
  SQLCHAR input[] = "SELECT {fn UCASE('mixed')}";
  std::array<SQLCHAR, 8> output{};
  SQLINTEGER length = -1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLNativeSql(connection_, input, SQL_NTS, output.data(),
                         static_cast<SQLINTEGER>(output.size()), &length));
  EXPECT_STREQ("SELECT ", reinterpret_cast<const char*>(output.data()));
  EXPECT_EQ(21, length);
  EXPECT_EQ("01004", diagnostic_state(SQL_HANDLE_DBC, connection_));

  auto wide_input = rs::odbc::utf8_to_wide(
      "SELECT {fn LCASE('MIXED')}");
  ASSERT_TRUE(wide_input.has_value());
  std::array<SQLWCHAR, 8> wide_output{};
  length = -1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLNativeSqlW(connection_, wide_input->data(),
                          static_cast<SQLINTEGER>(wide_input->size()),
                          wide_output.data(),
                          static_cast<SQLINTEGER>(wide_output.size()),
                          &length));
  EXPECT_EQ(21, length);
  EXPECT_EQ("SELECT ", *rs::odbc::wide_to_utf8(
                           std::span<const SQLWCHAR>(wide_output.data(), 7)));
  EXPECT_EQ("01004", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(NativeSqlIntegrationTest, ZeroLengthOutputReportsTruncation) {
  SQLCHAR input[] = "SELECT 1";
  SQLCHAR output = 99;
  SQLINTEGER length = -1;
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLNativeSql(connection_, input, SQL_NTS, &output, 0, &length));
  EXPECT_EQ(99, output);
  EXPECT_EQ(8, length);
  EXPECT_EQ("01004", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(NativeSqlIntegrationTest, PreservesOutputsOnTranslationErrors) {
  SQLCHAR invalid_date[] = "SELECT {d '2023-02-29'}";
  SQLCHAR output[] = "keep";
  SQLINTEGER length = 91;
  EXPECT_EQ(SQL_ERROR,
            SQLNativeSql(connection_, invalid_date, SQL_NTS, output,
                         sizeof(output), &length));
  EXPECT_STREQ("keep", reinterpret_cast<const char*>(output));
  EXPECT_EQ(91, length);
  EXPECT_EQ("22007", diagnostic_state(SQL_HANDLE_DBC, connection_));

  SQLCHAR malformed[] = "SELECT {fn UCASE('x')";
  EXPECT_EQ(SQL_ERROR,
            SQLNativeSql(connection_, malformed, SQL_NTS, output,
                         sizeof(output), &length));
  EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(NativeSqlIntegrationTest, ExecutesEscapesInDirectAndPreparedSql) {
  SQLCHAR direct[] =
      "SELECT {fn UCASE('mixed')}, {d '2024-02-29'} = DATE '2024-02-29'";
  ASSERT_EQ(SQL_SUCCESS,
            SQLExecDirect(statement_, direct, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  SQLCHAR text[16]{};
  SQLCHAR boolean[4]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 1, SQL_C_CHAR, text, sizeof(text), nullptr));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 2, SQL_C_CHAR, boolean, sizeof(boolean),
                       nullptr));
  EXPECT_STREQ("MIXED", reinterpret_cast<const char*>(text));
  EXPECT_STREQ("t", reinterpret_cast<const char*>(boolean));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(statement_));

  SQLCHAR prepared[] = "SELECT {fn LCASE('PREPARED')}";
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(statement_, prepared, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(statement_));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  std::memset(text, 0, sizeof(text));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 1, SQL_C_CHAR, text, sizeof(text), nullptr));
  EXPECT_STREQ("prepared", reinterpret_cast<const char*>(text));
}

TEST_F(NativeSqlIntegrationTest, NoScanBypassesEscapeTranslation) {
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetStmtAttr(statement_, SQL_ATTR_NOSCAN,
                           reinterpret_cast<SQLPOINTER>(SQL_NOSCAN_ON), 0));
  SQLCHAR escaped[] = "SELECT {fn UCASE('unchanged')}";
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, escaped, SQL_NTS));
  EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, statement_));

  ASSERT_EQ(SQL_SUCCESS,
            SQLSetStmtAttr(statement_, SQL_ATTR_NOSCAN,
                           reinterpret_cast<SQLPOINTER>(SQL_NOSCAN_OFF), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, escaped, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  SQLCHAR output[16]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 1, SQL_C_CHAR, output, sizeof(output),
                       nullptr));
  EXPECT_STREQ("UNCHANGED", reinterpret_cast<const char*>(output));
}
