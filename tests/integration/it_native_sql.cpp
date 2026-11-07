#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>

#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include "odbc/odbc_api.h"
#endif
#include "tests/test_connection_config.h"
#include "tests/test_dm_expectations.h"
#include "tests/test_product_profile.h"
#include "tests/test_text.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>


namespace {
// One original per-case window, including reconnects and sibling handles.
// Apply caps only on I/O entry, leaving allocation defaults and deliberate shorter
// timeout values intact. Never restore attributes after a call: its diagnostics belong to the test.
constexpr SQLULEN fixture_timeout_seconds(std::int64_t left, SQLULEN requested = 0) {
  if (left <= 0) return 0;
  const auto cap = static_cast<SQLULEN>(left);
  return requested == 0 ? cap : std::min(cap, requested);
}
static_assert(fixture_timeout_seconds(40) == 40);
static_assert(fixture_timeout_seconds(4) == 4);
static_assert(fixture_timeout_seconds(1) == 1);
static_assert(fixture_timeout_seconds(0) == 0);
static_assert(fixture_timeout_seconds(-1) == 0);
static_assert(fixture_timeout_seconds(40, 1) == 1);
static_assert(fixture_timeout_seconds(40, 2) == 2);
static_assert(fixture_timeout_seconds(40, 3) == 3);

SQLULEN fixture_seconds_left(std::chrono::steady_clock::time_point end) {
  const auto seconds = fixture_timeout_seconds(
      std::chrono::duration_cast<std::chrono::seconds>(
          end - std::chrono::steady_clock::now()).count());
  if (seconds == 0) {
    ADD_FAILURE() << "Original forty-second fixture window expired; no new I/O";
  }
  return seconds;
}

bool fixture_statement_budget(std::chrono::steady_clock::time_point end, SQLHSTMT statement) {
  const auto left = fixture_seconds_left(end);
  if (left == 0) return false;
  SQLULEN requested = 0;
  if (SQLGetStmtAttr(statement, SQL_ATTR_QUERY_TIMEOUT, &requested, 0, nullptr) != SQL_SUCCESS) {
    // Preserve the original API's invalid-handle/local-validation assertion.
    return true;
  }
  const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), requested);
  if (seconds == requested) return true;
  if (SQLSetStmtAttr(statement, SQL_ATTR_QUERY_TIMEOUT,
                     reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
    ADD_FAILURE() << "Could not apply finite fixture statement timeout";
    return false;
  }
  return true;
}

bool fixture_connection_budget(std::chrono::steady_clock::time_point end, SQLHDBC connection, bool login) {
  const auto left = fixture_seconds_left(end);
  if (left == 0) return false;
  SQLUINTEGER current = 0;
  if (login) {
    SQLUINTEGER dead = SQL_CD_TRUE;
    if (SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_DEAD, &dead, sizeof(dead), nullptr) == SQL_SUCCESS && dead == SQL_CD_FALSE) {
      // An already-connected connect call is a local 08002 test, not new I/O.
      return true;
    }
    if (odbcpp::test::get_login_timeout(connection, &current) != SQL_SUCCESS) return true;
    const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), current);
    if (SQLSetConnectAttr(connection, SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
      ADD_FAILURE() << "Could not apply finite fixture login timeout";
      return false;
    }
  }
  if (SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_TIMEOUT, &current, sizeof(current), nullptr) != SQL_SUCCESS) return true;
  const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), current);
  if (SQLSetConnectAttr(connection, SQL_ATTR_CONNECTION_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
    ADD_FAILURE() << "Could not apply finite fixture connection timeout";
    return false;
  }
  return true;
}
}  // namespace


namespace {

std::string diagnostic_state(SQLSMALLINT type, SQLHANDLE handle) {
  SQLCHAR state[6]{};
  EXPECT_EQ(SQL_SUCCESS,
            odbcpp::test::diag_rec(type, handle, 1, state, nullptr, nullptr, 0,
                                   nullptr));
  return reinterpret_cast<const char*>(state);
}

class NativeSqlIntegrationTest : public ::testing::Test {
 protected:
    const std::chrono::steady_clock::time_point fixture_end_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(40);

    // Fixture-local forwards cover repetitions, reconnects and sibling handles.
    // The original assertions/arguments below still call the actual public API.
    SQLRETURN SQLConnect(SQLHDBC connection, SQLCHAR* server_name, SQLSMALLINT name_length1,
        SQLCHAR* user_name, SQLSMALLINT name_length2, SQLCHAR* authentication,
        SQLSMALLINT name_length3) {
      return fixture_connection_budget(fixture_end_, connection, true)
          ? ::SQLConnect(connection, server_name, name_length1, user_name, name_length2,
              authentication, name_length3) : SQL_ERROR;
    }
    SQLRETURN SQLExecDirect(SQLHSTMT statement, SQLCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecDirect(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLExecDirectW(SQLHSTMT statement, SQLWCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecDirectW(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLExecute(SQLHSTMT statement) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecute(statement) : SQL_ERROR;
    }
    SQLRETURN SQLFetch(SQLHSTMT statement) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLFetch(statement) : SQL_ERROR;
    }
    SQLRETURN SQLPrepare(SQLHSTMT statement, SQLCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLPrepare(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLPrepareW(SQLHSTMT statement, SQLWCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLPrepareW(statement, statement_text, text_length) : SQL_ERROR;
    }

  std::string connection_diagnostics() {
    std::string result;
    for (SQLINTEGER record = 1; record <= std::numeric_limits<SQLSMALLINT>::max(); ++record) {
      SQLCHAR state[6]{}, message[1024]{};
      SQLINTEGER native = 0;
      const auto rc = ::SQLGetDiagRec(SQL_HANDLE_DBC, connection_,
          static_cast<SQLSMALLINT>(record), state, &native, message, sizeof(message), nullptr);
      if (rc == SQL_NO_DATA) {
        break;
      }
      if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        result += " diagnostic_read_failed=" + std::to_string(rc);
        break;
      }
      message[sizeof(message) - 1] = 0;
      std::string lowered(reinterpret_cast<const char*>(message));
      std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
      });
      const std::string sqlstate(reinterpret_cast<const char*>(state));
      const char* category = "unknown";
      if (lowered.find("too many connections") != std::string::npos ||
          lowered.find("connection limit exceeded") != std::string::npos) {
        category = "connection_limit";
      } else if (sqlstate == "28000") {
        category = "authentication";
      } else if (sqlstate == "HYT00" || sqlstate == "HYT01") {
        category = "timeout";
      } else if (lowered.find("certificate verify failed") != std::string::npos ||
                 lowered.find("certificate verification failed") != std::string::npos) {
        category = "tls_certificate";
      }
      // Startup text may contain a principal or private endpoint. Preserve all
      // record codes and a fixed category, never arbitrary server text in XML.
      result += " [record=" + std::to_string(record) + " state=" + sqlstate +
          " native=" + std::to_string(native) + " category=" + category + "]";
    }
    return result.empty() ? "no diagnostic records" : result;
  }

  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    if (SQLConnect(connection_,
                   odbcpp::test::configured_server_name_data(), SQL_NTS,
                   nullptr, 0, nullptr, 0) != SQL_SUCCESS) {
      GTEST_SKIP() << "Database connection failed: " << connection_diagnostics();
    }
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  }

  void TearDown() override {
    if (statement_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement_));
      statement_ = SQL_NULL_HSTMT;
    }
    if (connection_) {
      const auto disconnected = SQLDisconnect(connection_);
      if (disconnected != SQL_SUCCESS) {
        // Failed login or a deliberately retired timeout/COPY connection is closed.
        EXPECT_EQ(SQL_ERROR, disconnected);
        EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));
      }
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection_));
      connection_ = SQL_NULL_HDBC;
    }
    if (environment_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment_));
      environment_ = SQL_NULL_HENV;
    }
  }

  void expect_scalar(const std::string& sql, const char* expected) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.data())), SQL_NTS))
        << sql << " SQLSTATE=" << diagnostic_state(SQL_HANDLE_STMT, statement_);
    ASSERT_NO_FATAL_FAILURE(expect_fetched_scalar(expected));
  }

  void expect_wide_scalar(const std::string& sql, std::string_view expected) {
    auto input = odbcpp::test::text::utf8_to_wide(sql);
    const auto expected_wide = odbcpp::test::text::utf8_to_wide(expected);
    ASSERT_TRUE(input.has_value());
    ASSERT_TRUE(expected_wide.has_value());
    input->push_back(0);
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirectW(statement_, input->data(), SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
    SQLWCHAR actual[256]{};
    SQLLEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(statement_, 1, SQL_C_WCHAR, actual, sizeof(actual), &length));
    ASSERT_EQ(static_cast<SQLLEN>(expected_wide->size() * sizeof(SQLWCHAR)), length);
    ASSERT_LT(expected_wide->size(), std::size(actual));
    EXPECT_EQ(*expected_wide, std::vector<SQLWCHAR>(actual, actual + expected_wide->size()));
    EXPECT_EQ(0, actual[expected_wide->size()]);
    ASSERT_EQ(SQL_NO_DATA, SQLFetch(statement_));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  }

  void expect_fetched_scalar(const char* expected) {
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
    SQLCHAR actual[256]{};
    SQLLEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(statement_, 1, SQL_C_CHAR, actual, sizeof(actual), &length));
    if (expected) {
      ASSERT_NE(SQL_NULL_DATA, length);
      EXPECT_STREQ(expected, reinterpret_cast<const char*>(actual));
    } else {
      EXPECT_EQ(SQL_NULL_DATA, length);
    }
    ASSERT_EQ(SQL_NO_DATA, SQLFetch(statement_));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  }

  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  SQLHSTMT statement_{SQL_NULL_HSTMT};
};

} // namespace

TEST(NativeSqlFixtureBudgetTest, UsesRemainingWindowAndHonorsExplicitShortTimeouts) {
  EXPECT_EQ(40u, fixture_timeout_seconds(40));
  EXPECT_EQ(9u, fixture_timeout_seconds(9));
  EXPECT_EQ(4u, fixture_timeout_seconds(4));
  EXPECT_EQ(0u, fixture_timeout_seconds(0));
  EXPECT_EQ(0u, fixture_timeout_seconds(-1));
  EXPECT_EQ(2u, fixture_timeout_seconds(40, 2));
  EXPECT_EQ(1u, fixture_timeout_seconds(1, 2));
  EXPECT_EQ(9u, fixture_timeout_seconds(9, 40));
}

TEST_F(NativeSqlIntegrationTest, RejectsEmbeddedNulSqlAndRecovers) {
  std::string sql("SELECT 1\0;SELECT 2", sizeof("SELECT 1\0;SELECT 2") - 1);
  auto* narrow = reinterpret_cast<SQLCHAR*>(sql.data());
  std::vector<SQLWCHAR> wide(sql.begin(), sql.end());
  const auto length = static_cast<SQLINTEGER>(sql.size());
  const auto wide_length = static_cast<SQLINTEGER>(wide.size());

  // The manager passes the driver SQL_NTS instead of the explicit length, so
  // the text before the NUL runs and the rest of the case cannot be reached.
  if (odbcpp::test::dm::stops_case(
          odbcpp::test::dm::Deviation::ExplicitTextLengthReplacedByNts)) {
    EXPECT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, narrow, length));
    return;
  }
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, narrow, length));
  EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLExecDirectW(statement_, wide.data(), wide_length));
  EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLPrepare(statement_, narrow, length));
  EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLPrepareW(statement_, wide.data(), wide_length));
  EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLCHAR valid[] = "SELECT 42";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, valid, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  SQLINTEGER value = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 1, SQL_C_SLONG, &value, sizeof(value),
                       nullptr));
  EXPECT_EQ(42, value);
}

TEST_F(NativeSqlIntegrationTest, CopyOutReportsUnsupportedWithoutHanging) {
  SQLCHAR sql[] = "COPY (SELECT 1) TO STDOUT";
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, sql, SQL_NTS));
  // Redshift rejects PostgreSQL COPY STDIN/STDOUT syntax before entering
  // COPY wire mode; PostgreSQL enters that unsupported mode and is closed.
  // https://docs.aws.amazon.com/redshift/latest/dg/r_COPY.html
  const bool redshift = odbcpp::test::compiled_redshift_product();
  EXPECT_EQ(redshift ? "42000" : "HYC00",
            diagnostic_state(SQL_HANDLE_STMT, statement_));
  SQLUINTEGER dead = SQL_CD_FALSE;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection_, SQL_ATTR_CONNECTION_DEAD, &dead,
                              sizeof(dead), nullptr));
  EXPECT_EQ(redshift ? SQL_CD_FALSE : SQL_CD_TRUE, dead);
  if (redshift) {
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT 42", "42"));
  }
}

TEST_F(NativeSqlIntegrationTest, CopyInReportsUnsupportedBeforeTimeout) {
  SQLCHAR create[] = "CREATE TEMP TABLE odbcpp_copy_input_probe (value integer)";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, create, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));

  SQLCHAR sql[] = "COPY odbcpp_copy_input_probe FROM STDIN";
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, sql, SQL_NTS));
  // Redshift rejects PostgreSQL COPY STDIN/STDOUT syntax before entering
  // COPY wire mode; PostgreSQL enters that unsupported mode and is closed.
  // https://docs.aws.amazon.com/redshift/latest/dg/r_COPY.html
  const bool redshift = odbcpp::test::compiled_redshift_product();
  EXPECT_EQ(redshift ? "42000" : "HYC00",
            diagnostic_state(SQL_HANDLE_STMT, statement_));
  SQLUINTEGER dead = SQL_CD_FALSE;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection_, SQL_ATTR_CONNECTION_DEAD, &dead,
                              sizeof(dead), nullptr));
  EXPECT_EQ(redshift ? SQL_CD_FALSE : SQL_CD_TRUE, dead);
  if (redshift) {
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT 42", "42"));
  }
}

TEST_F(NativeSqlIntegrationTest, BinaryCursorResultsAreNotReadAsText) {
  SQLCHAR begin[] = "BEGIN";
  SQLCHAR declare[] =
      "DECLARE odbcpp_binary_probe BINARY CURSOR FOR "
      "SELECT 42::int4 AS value";
  SQLCHAR fetch[] = "FETCH ALL FROM odbcpp_binary_probe";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, begin, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, declare, SQL_NTS));
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, fetch, SQL_NTS));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  SQLUINTEGER dead = SQL_CD_FALSE;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetConnectAttr(connection_, SQL_ATTR_CONNECTION_DEAD, &dead,
                              sizeof(dead), nullptr));
  EXPECT_EQ(SQL_CD_FALSE, dead);

  SQLCHAR recovery[] = "SELECT 43";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, recovery, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  SQLINTEGER value = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 1, SQL_C_SLONG, &value, sizeof(value),
                       nullptr));
  EXPECT_EQ(43, value);
}

TEST_F(NativeSqlIntegrationTest, NativeSqlRejectsEmbeddedNulWithoutChangingOutputs) {
  std::string sql("SELECT 1\0;SELECT 2", sizeof("SELECT 1\0;SELECT 2") - 1);
  std::vector<SQLWCHAR> wide(sql.begin(), sql.end());
  SQLCHAR output[] = "keep";
  SQLWCHAR wide_output[] = {'k', 'e', 'e', 'p', 0};
  SQLINTEGER length = 91;

  // The manager passes the driver SQL_NTS instead of the explicit length, so
  // the driver translates "SELECT 1" and truncates it into the 5-byte output.
  if (odbcpp::test::dm::stops_case(
          odbcpp::test::dm::Deviation::ExplicitTextLengthReplacedByNts)) {
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              SQLNativeSql(connection_, reinterpret_cast<SQLCHAR*>(sql.data()),
                           static_cast<SQLINTEGER>(sql.size()), output,
                           static_cast<SQLINTEGER>(sizeof(output)), &length));
    EXPECT_STREQ("SELE", reinterpret_cast<const char*>(output));
    EXPECT_EQ(8, length);
    return;
  }
  EXPECT_EQ(SQL_ERROR,
            SQLNativeSql(connection_, reinterpret_cast<SQLCHAR*>(sql.data()),
                         static_cast<SQLINTEGER>(sql.size()), output,
                         static_cast<SQLINTEGER>(sizeof(output)), &length));
  EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_STREQ("keep", reinterpret_cast<const char*>(output));
  EXPECT_EQ(91, length);

  EXPECT_EQ(SQL_ERROR,
            SQLNativeSqlW(connection_, wide.data(),
                          static_cast<SQLINTEGER>(wide.size()), wide_output,
                          static_cast<SQLINTEGER>(std::size(wide_output)),
                          &length));
  EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ('k', wide_output[0]);
  EXPECT_EQ('e', wide_output[1]);
  EXPECT_EQ('e', wide_output[2]);
  EXPECT_EQ('p', wide_output[3]);
  EXPECT_EQ(0, wide_output[4]);
  EXPECT_EQ(91, length);
}

TEST_F(NativeSqlIntegrationTest, ReturnsTranslatedAnsiSqlAndExactLength) {
  SQLCHAR input[] = "SELECT {fn UCASE('mixed')}, {d '2024-02-29'}";
  constexpr char expected[] = "SELECT UPPER('mixed'), DATE '2024-02-29'";
  SQLINTEGER length = -1;
  // The ODBC reference raises HY090 for a negative BufferLength only with a
  // non-null output; the manager also rejects it for a NULL output.
  const SQLRETURN negative_result =
      SQLNativeSql(connection_, input, SQL_NTS, nullptr, -7, &length);
  if (odbcpp::test::dm::deviates(
          odbcpp::test::dm::Deviation::NegativeBufferLengthRejectedWithNullOutput)) {
    EXPECT_EQ(SQL_ERROR, negative_result);
    EXPECT_EQ(odbcpp::test::dm::text(
                  odbcpp::test::dm::Deviation::NegativeBufferLengthRejectedWithNullOutput,
                  "HY090"),
              diagnostic_state(SQL_HANDLE_DBC, connection_));
  } else {
    ASSERT_EQ(SQL_SUCCESS, negative_result);
    EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(expected) - 1), length);
  }

  std::array<SQLCHAR, sizeof(expected)> output{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLNativeSql(connection_, input, SQL_NTS, output.data(),
                         static_cast<SQLINTEGER>(output.size()), &length));
  EXPECT_STREQ(expected, reinterpret_cast<const char*>(output.data()));

  auto wide_input = odbcpp::test::text::utf8_to_wide("SELECT {t '12:34:56'} trailing");
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
            *odbcpp::test::text::wide_to_utf8(
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

  auto wide_input = odbcpp::test::text::utf8_to_wide(
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
  EXPECT_EQ("SELECT ", *odbcpp::test::text::wide_to_utf8(
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

TEST_F(NativeSqlIntegrationTest, LengthOutputsHandleUnalignedBuffers) {
  SQLCHAR input[] = "SELECT {fn UCASE('x')}";
  alignas(SQLINTEGER)
  std::array<std::byte, 1 + sizeof(SQLINTEGER)> storage{};
  auto* length = reinterpret_cast<SQLINTEGER*>(storage.data() + 1);
  const auto read_length = [&] {
    SQLINTEGER copied = 0;
    std::memcpy(&copied, storage.data() + 1, sizeof(copied));
    return copied;
  };

  SQLINTEGER expected = -1;
  // The manager gives the driver a 0-length scratch buffer for the NULL
  // output, so the driver correctly reports truncation.
  const auto null_output_result = odbcpp::test::dm::value(
      odbcpp::test::dm::Deviation::NullOutputBufferSubstituted,
      SQLRETURN{SQL_SUCCESS}, SQLRETURN{SQL_SUCCESS_WITH_INFO});
  ASSERT_EQ(null_output_result, SQLNativeSql(
      connection_, input, SQL_NTS, nullptr, 0, &expected));
  ASSERT_EQ(null_output_result, SQLNativeSql(
      connection_, input, SQL_NTS, nullptr, 0, length));
  EXPECT_EQ(expected, read_length());

  auto wide_input = odbcpp::test::text::utf8_to_wide("SELECT {fn UCASE('x')}");
  ASSERT_TRUE(wide_input.has_value());
  SQLWCHAR wide_output = static_cast<SQLWCHAR>('k');
  expected = -1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLNativeSqlW(
      connection_, wide_input->data(),
      static_cast<SQLINTEGER>(wide_input->size()), &wide_output, 0,
      &expected));
  storage.fill(std::byte{0x5a});
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLNativeSqlW(
      connection_, wide_input->data(),
      static_cast<SQLINTEGER>(wide_input->size()), &wide_output, 0,
      length));
  EXPECT_EQ(expected, read_length());
  EXPECT_EQ(static_cast<SQLWCHAR>('k'), wide_output);

  SQLCHAR invalid[] = "SELECT {d '2023-02-29'}";
  const auto previous_output = storage;
  EXPECT_EQ(SQL_ERROR, SQLNativeSql(
      connection_, invalid, SQL_NTS, nullptr, 0, length));
  EXPECT_EQ(previous_output, storage);
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

  SQLCHAR invalid_time[] = "SELECT {t '-1:00:00'}";
  EXPECT_EQ(SQL_ERROR,
            SQLNativeSql(connection_, invalid_time, SQL_NTS, output,
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

TEST_F(NativeSqlIntegrationTest, RejectsNegativeTimeInDirectAndPreparedSql) {
  SQLCHAR invalid[] = "SELECT {t '12:-1:00'}";
  EXPECT_EQ(SQL_ERROR, SQLExecDirect(statement_, invalid, SQL_NTS));
  EXPECT_EQ("22007", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLPrepare(statement_, invalid, SQL_NTS));
  EXPECT_EQ("22007", diagnostic_state(SQL_HANDLE_STMT, statement_));

  SQLCHAR valid[] = "SELECT {t '12:34:56'}";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, valid, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
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
  EXPECT_STREQ("1", reinterpret_cast<const char*>(boolean));
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

TEST_F(NativeSqlIntegrationTest, ConvertNumericEscapesPreserveValuesAndNulls) {
  for (const auto* target : {"SQL_SMALLINT", "SQL_INTEGER", "SQL_BIGINT", "SQL_REAL",
                           "SQL_FLOAT", "SQL_DOUBLE", "SQL_NUMERIC", "SQL_DECIMAL"}) {
    SCOPED_TRACE(target);
    const auto expression = "{fn CONVERT('42'," + std::string(target) + ")}";
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT CASE WHEN " + expression + " = 42 THEN 'ok' ELSE 'bad' END", "ok"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(NULL," + std::string(target) + ")}", nullptr));
  }
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT CASE WHEN {fn CONVERT('1.25',SQL_DECIMAL)} = 1.25 THEN 'ok' ELSE 'bad' END", "ok"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT CASE WHEN {fn CONVERT(TRUE,SQL_BIT)} THEN 'ok' ELSE 'bad' END", "ok"));
}

TEST_F(NativeSqlIntegrationTest, ConvertCharacterBinaryAndTemporalEscapes) {
  for (const auto* target : {"SQL_CHAR", "SQL_VARCHAR", "SQL_LONGVARCHAR", "SQL_WCHAR", "SQL_WVARCHAR", "SQL_WLONGVARCHAR"}) {
    ASSERT_NO_FATAL_FAILURE(expect_wide_scalar("SELECT {fn CONVERT('Grüße'," + std::string(target) + ")}", "Grüße"));
  }
  for (const auto* target : {"SQL_BINARY", "SQL_VARBINARY", "SQL_LONGVARBINARY"}) {
    const auto expression = "{fn CONVERT('ab'," + std::string(target) + ")}";
    const auto query = odbcpp::test::compiled_redshift_product()
        ? "SELECT FROM_VARBYTE(" + expression + ", 'hex')"
        : "SELECT encode(" + expression + ", 'hex')";
    ASSERT_NO_FATAL_FAILURE(expect_scalar(query, "6162"));
  }
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(42,SQL_VARCHAR)}", "42"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(DATE '2024-02-29',SQL_TIMESTAMP)}", "2024-02-29 00:00:00"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(TIMESTAMP '2024-02-29 12:34:56',SQL_DATE)}", "2024-02-29"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(TIMESTAMP '2024-02-29 12:34:56',SQL_TIME)}", "12:34:56"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT('2024-02-29',SQL_DATE)}", "2024-02-29"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT('12:34:56',SQL_TIME)}", "12:34:56"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT('2024-02-29 12:34:56',SQL_TIMESTAMP)}", "2024-02-29 12:34:56"));
  if (!odbcpp::test::compiled_redshift_product()) {
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(TRUE,SQL_VARCHAR)}", "true"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT('12345678-1234-1234-1234-123456789abc',SQL_GUID)}",
        "12345678-1234-1234-1234-123456789abc"));
  }
}

// Exercise every numeric source/target family, not only character-to-number.
// Literal 42 is exactly representable in every supported numeric type.
TEST_F(NativeSqlIntegrationTest, ConvertNumericSourceTargetMatrix) {
  for (const auto* source : {"SMALLINT", "INTEGER", "BIGINT", "REAL", "DOUBLE PRECISION", "DECIMAL(10,2)"}) {
    for (const auto* target : {"SQL_SMALLINT", "SQL_INTEGER", "SQL_BIGINT", "SQL_REAL", "SQL_FLOAT",
                             "SQL_DOUBLE", "SQL_NUMERIC", "SQL_DECIMAL"}) {
      SCOPED_TRACE(std::string(source) + " -> " + target);
      const auto prefix = "SELECT CASE WHEN {fn CONVERT(CAST(";
      const auto suffix = " AS " + std::string(source) + ")," + target + ")}";
      ASSERT_NO_FATAL_FAILURE(expect_scalar(prefix + std::string("42") + suffix + " = 42 THEN 'ok' ELSE 'bad' END", "ok"));
      ASSERT_NO_FATAL_FAILURE(expect_scalar(prefix + std::string("NULL") + suffix + " IS NULL THEN 'ok' ELSE 'bad' END", "ok"));
    }
    const auto converted = "{fn CONVERT(CAST(42 AS " + std::string(source) + "),SQL_VARCHAR)}";
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT CASE WHEN CAST(" + converted + " AS DECIMAL(10,2)) = 42 THEN 'ok' ELSE 'bad' END", "ok"));
  }
}

TEST_F(NativeSqlIntegrationTest, ConvertBinaryAndTemporalIdentityAndCharacterPairs) {
  const bool redshift = odbcpp::test::compiled_redshift_product();
  const auto binary = redshift ? "CAST('ab' AS VARBYTE)" : "CAST('ab' AS BYTEA)";
  for (const auto* target : {"SQL_BINARY", "SQL_VARBINARY", "SQL_LONGVARBINARY"}) {
    SCOPED_TRACE(target);
    const auto expression = "{fn CONVERT(" + std::string(binary) + "," + target + ")}";
    const auto query = redshift ? "SELECT FROM_VARBYTE(" + expression + ", 'hex')"
                               : "SELECT encode(" + expression + ", 'hex')";
    ASSERT_NO_FATAL_FAILURE(expect_scalar(query, "6162"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(NULL," + std::string(target) + ")}", nullptr));
  }
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(" + std::string(binary) + ",SQL_VARCHAR)}",
      redshift ? "ab" : "\\x6162"));
  const std::array<std::array<const char*, 3>, 3> temporal{{
      {{"DATE '2024-02-29'", "SQL_DATE", "2024-02-29"}},
      {{"TIME '12:34:56'", "SQL_TIME", "12:34:56"}},
      {{"TIMESTAMP '2024-02-29 12:34:56'", "SQL_TIMESTAMP", "2024-02-29 12:34:56"}}}};
  for (const auto& [value, target, expected] : temporal) {
    SCOPED_TRACE(target);
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(" + std::string(value) + "," + target + ")}", expected));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(" + std::string(value) + ",SQL_VARCHAR)}", expected));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(NULL," + std::string(target) + ")}", nullptr));
  }
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn CONVERT(NULL,SQL_BIT)}", nullptr));
}

TEST_F(NativeSqlIntegrationTest, TimestampAddEverySupportedUnitAndCalendarEdges) {
  const std::array<std::pair<const char*, const char*>, 8> cases{{
      {"SECOND", "2024-01-01 00:00:01"}, {"MINUTE", "2024-01-01 00:01:00"},
      {"HOUR", "2024-01-01 01:00:00"}, {"DAY", "2024-01-02 00:00:00"},
      {"WEEK", "2024-01-08 00:00:00"}, {"MONTH", "2024-02-01 00:00:00"},
      {"QUARTER", "2024-04-01 00:00:00"}, {"YEAR", "2025-01-01 00:00:00"}}};
  for (const auto& [unit, expected] : cases) {
    SCOPED_TRACE(unit);
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_" + std::string(unit) +
        ",1,{ts '2024-01-01 00:00:00'})}", expected));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_" + std::string(unit) +
        ",-1,TIMESTAMP '" + expected + "')}", "2024-01-01 00:00:00"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_" + std::string(unit) +
        ",NULL,TIMESTAMP '2024-01-01')}", nullptr));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_" + std::string(unit) + ",1,NULL)}", nullptr));
  }
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_MONTH,1,{d '2024-01-31'})}", "2024-02-29 00:00:00"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_YEAR,1,{d '2024-02-29'})}", "2025-02-28 00:00:00"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_YEAR,-1,{ts '2024-02-29 12:34:56'})}", "2023-02-28 12:34:56"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_YEAR,4,{d '2024-02-29'})}", "2028-02-29 00:00:00"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_MONTH,12,{d '2024-02-29'})}", "2025-02-28 00:00:00"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_DAY,-1,{d '2024-01-01'})}", "2023-12-31 00:00:00"));
}

TEST_F(NativeSqlIntegrationTest, TimestampDiffCountsSignedBoundariesRatherThanRoundedDurations) {
  for (const auto* unit : {"SECOND", "MINUTE", "HOUR", "DAY", "MONTH", "QUARTER", "YEAR"}) {
    SCOPED_TRACE(unit);
    const auto prefix = "SELECT {fn TIMESTAMPDIFF(SQL_TSI_" + std::string(unit) + ",";
    ASSERT_NO_FATAL_FAILURE(expect_scalar(prefix + "{ts '2023-12-31 23:59:59.999999'},{ts '2024-01-01 00:00:00'})}", "1"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar(prefix + "{ts '2024-01-01 00:00:00'},{ts '2023-12-31 23:59:59.999999'})}", "-1"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar(prefix + "NULL,TIMESTAMP '2024-01-01')}", nullptr));
    ASSERT_NO_FATAL_FAILURE(expect_scalar(prefix + "TIMESTAMP '2024-01-01',NULL)}", nullptr));
  }
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_SECOND,{ts '1969-12-31 23:59:59.1'},{ts '1969-12-31 23:59:59.9'})}", "0"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_SECOND,{ts '1969-12-31 23:59:59.9'},{ts '1970-01-01 00:00:00.1'})}", "1"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_MONTH,{d '2024-01-31'},{d '2024-02-01'})}", "1"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_YEAR,{d '2024-01-01'},{d '2024-12-31'})}", "0"));
}

TEST_F(NativeSqlIntegrationTest, TimestampEscapesNormalizeTimeDateAndNullOperands) {
  ASSERT_NO_FATAL_FAILURE(expect_scalar(
      "SELECT CASE WHEN {fn TIMESTAMPADD(SQL_TSI_DAY,1,TIME '12:34:56')} = "
      "CAST(CAST(CURRENT_DATE AS VARCHAR(10)) || ' 12:34:56' AS TIMESTAMP) + INTERVAL '1 day' THEN 'ok' ELSE 'bad' END", "ok"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_DAY,TIME '01:00:00',TIME '23:00:00')}", "0"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_HOUR,TIME '01:00:00',TIME '23:00:00')}", "22"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_HOUR,{d '2024-01-01'},{ts '2024-01-01 02:30:00'})}", "2"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_DAY,NULL,{d '2024-01-01'})}", nullptr));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_DAY,1,NULL)}", nullptr));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_DAY,NULL,{d '2024-01-01'})}", nullptr));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_MONTH,{d '2024-01-01'},NULL)}", nullptr));
}

TEST_F(NativeSqlIntegrationTest, TimestampEscapesUseWallClockFieldsAcrossDst) {
  SQLCHAR timezone[] = "SET TIMEZONE TO 'America/New_York'";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, timezone, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  // ODBC timestamp operands carry local fields, not a timezone. Do not turn
  // this into elapsed-instant arithmetic across the missing/repeated hour.
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_HOUR,1,TIMESTAMP '2024-03-10 01:30:00')}", "2024-03-10 02:30:00"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_HOUR,TIMESTAMP '2024-03-10 01:30:00',TIMESTAMP '2024-03-10 03:30:00')}", "2"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_HOUR,TIMESTAMP '2024-11-03 00:30:00',TIMESTAMP '2024-11-03 02:30:00')}", "2"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPADD(SQL_TSI_DAY,1,TIMESTAMP '2024-03-09 12:00:00')}", "2024-03-10 12:00:00"));
}

TEST_F(NativeSqlIntegrationTest, TimestampAddPreparedTypedTimePreservesParameterOrder) {
  SQLCHAR sql[] = "SELECT CASE WHEN {fn TIMESTAMPADD(SQL_TSI_DAY,?,?)} = "
      "CAST(CAST(CURRENT_DATE AS VARCHAR(10)) || ' 12:34:56' AS TIMESTAMP) + INTERVAL '2 days' THEN 'ok' ELSE 'bad' END";
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(statement_, sql, SQL_NTS));
  SQLINTEGER days = 2;
  SQL_TIME_STRUCT time{12, 34, 56};
  SQLLEN count_length = sizeof(days), time_length = sizeof(time);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(statement_, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
      0, 0, &days, sizeof(days), &count_length));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(statement_, 2, SQL_PARAM_INPUT, SQL_C_TYPE_TIME, SQL_TYPE_TIME,
      0, 0, &time, sizeof(time), &time_length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(statement_));
  ASSERT_NO_FATAL_FAILURE(expect_fetched_scalar("ok"));
}

TEST_F(NativeSqlIntegrationTest, TimestampCalendarDifferencePreservesWindowScopeAndEraBoundary) {
  for (const auto* unit : {"MONTH", "QUARTER", "YEAR"}) {
    SCOPED_TRACE(unit);
    // DELTA is reserved on Redshift; keep the window-expression assertion portable.
    const auto query = "SELECT interval_count FROM (SELECT ts, {fn TIMESTAMPDIFF(SQL_TSI_" + std::string(unit) +
        ",LAG(ts) OVER (ORDER BY ts),ts)} AS interval_count FROM (SELECT TIMESTAMP '2023-12-31' AS ts "
        "UNION ALL SELECT TIMESTAMP '2024-01-01') AS events) AS differences WHERE interval_count IS NOT NULL";
    ASSERT_NO_FATAL_FAILURE(expect_scalar(query, "1"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_" + std::string(unit) +
        ",MIN(ts),MAX(ts))} FROM (SELECT TIMESTAMP '2023-12-31' AS ts "
        "UNION ALL SELECT TIMESTAMP '2024-01-01') AS events", "1"));
    if (!odbcpp::test::compiled_redshift_product()) {
      ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_" + std::string(unit) +
          ",TIMESTAMP '0001-12-31 BC',TIMESTAMP '0001-01-01 AD')}", "1"));
      ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_" + std::string(unit) +
          ",TIMESTAMP '0001-01-01 AD',TIMESTAMP '0001-12-31 BC')}", "-1"));
    }
  }
}

TEST_F(NativeSqlIntegrationTest, TimestampCalendarDifferenceRetainsOuterColumnScope) {
  for (const auto* unit : {"MONTH", "QUARTER", "YEAR"}) {
    SCOPED_TRACE(unit);
    const auto query = "SELECT {fn TIMESTAMPDIFF(SQL_TSI_" + std::string(unit) +
        ", value, TIMESTAMP '2025-01-01')} FROM (SELECT TIMESTAMP '2024-01-01' AS value) AS source";
    ASSERT_NO_FATAL_FAILURE(expect_scalar(query, std::strcmp(unit, "MONTH") == 0 ? "12" :
        (std::strcmp(unit, "QUARTER") == 0 ? "4" : "1")));
  }
}

TEST_F(NativeSqlIntegrationTest, TimestampNormalizationPreservesNativeDateStyle) {
  if (odbcpp::test::compiled_redshift_product()) {
    GTEST_SKIP() << "PostgreSQL-only DateStyle matrix; Redshift uses its native timestamp parser";
  }
  for (const auto* style : {"ISO, MDY", "SQL, DMY", "German, DMY", "Postgres, MDY"}) {
    SCOPED_TRACE(style);
    auto setting = "SET DateStyle TO '" + std::string(style) + "'";
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, reinterpret_cast<SQLCHAR*>(setting.data()), SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT CASE WHEN {fn TIMESTAMPADD(SQL_TSI_DAY,1,DATE '2024-02-28')} = "
        "TIMESTAMP '2024-02-29 00:00:00' THEN 'ok' ELSE 'bad' END", "ok"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT CASE WHEN {fn TIMESTAMPADD(SQL_TSI_HOUR,1,TIME '12:34:56')} = "
        "CURRENT_DATE + TIME '13:34:56' THEN 'ok' ELSE 'bad' END", "ok"));
    ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_MONTH,"
        "TIMESTAMP '2024-01-31 01:02:03', TIMESTAMP '2024-02-01 04:05:06')}", "1"));
  }
}

TEST_F(NativeSqlIntegrationTest, TimestampMonthDifferenceEvaluatesVolatileOperandOnce) {
  if (odbcpp::test::compiled_redshift_product()) {
    GTEST_SKIP() << "PostgreSQL-only volatile-operand proof: Redshift uses native DATEDIFF and has no sequences";
  }
  SQLCHAR create[] = "CREATE TEMP SEQUENCE odbcpp_escape_evaluation";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, create, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT {fn TIMESTAMPDIFF(SQL_TSI_MONTH,"
      "TIMESTAMP '2024-01-01' + nextval('odbcpp_escape_evaluation') * INTERVAL '1 month',TIMESTAMP '2025-01-01')}", "11"));
  ASSERT_NO_FATAL_FAILURE(expect_scalar("SELECT last_value FROM odbcpp_escape_evaluation", "1"));
  SQLCHAR drop[] = "DROP SEQUENCE odbcpp_escape_evaluation";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, drop, SQL_NTS));
}

TEST_F(NativeSqlIntegrationTest, LikeEscapeAcceptsUnicodeAndQuote) {
  SQLCHAR unicode[] =
      "SELECT 'a%b' LIKE 'a\xC3\xA9%b' {escape '\xC3\xA9'}";
  constexpr char expected[] =
      "SELECT 'a%b' LIKE 'a\xC3\xA9%b' ESCAPE '\xC3\xA9'";
  auto ansi_input = odbcpp::test::text::utf8_to_ansi_input(
      reinterpret_cast<const char*>(unicode));
  auto ansi_expected = odbcpp::test::text::utf8_to_ansi_input(expected);
  ASSERT_TRUE(ansi_input.has_value());
  ASSERT_TRUE(ansi_expected.has_value());
  auto* const ansi_text = reinterpret_cast<SQLCHAR*>(ansi_input->data());
  SQLCHAR output[sizeof(expected)]{};
  // Byte-wise widening makes this ANSI escape two characters. Only its
  // execution depends on that translation; wide and ASCII checks still apply.
  const bool ansi_widened = odbcpp::test::dm::deviates(
      odbcpp::test::dm::Deviation::AnsiTextWidenedPerByte);
  if (ansi_widened) {
    RecordProperty("dm_ansi_execution_blocked_by", "AnsiTextWidenedPerByte");
    EXPECT_EQ(SQL_ERROR, SQLNativeSql(connection_, ansi_text, SQL_NTS, output,
                                      sizeof(output), nullptr));
    EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_DBC, connection_));
  } else {
    ASSERT_EQ(SQL_SUCCESS,
              SQLNativeSql(connection_, ansi_text, SQL_NTS, output,
                           sizeof(output), nullptr));
    EXPECT_STREQ(ansi_expected->c_str(), reinterpret_cast<const char*>(output));
  }

  auto wide_input = odbcpp::test::text::utf8_to_wide(
      reinterpret_cast<const char*>(unicode));
  ASSERT_TRUE(wide_input.has_value());
  std::array<SQLWCHAR, sizeof(expected)> wide_output{};
  SQLINTEGER wide_length = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLNativeSqlW(connection_, wide_input->data(),
                          static_cast<SQLINTEGER>(wide_input->size()),
                          wide_output.data(),
                          static_cast<SQLINTEGER>(wide_output.size()),
                          &wide_length));
  ASSERT_GE(wide_length, 0);
  ASSERT_LT(static_cast<std::size_t>(wide_length), wide_output.size());
  const auto native_wide = odbcpp::test::text::wide_to_utf8(
      std::span<const SQLWCHAR>(wide_output.data(), wide_length));
  ASSERT_TRUE(native_wide.has_value());
  EXPECT_EQ(expected, *native_wide);

  SQLCHAR matched[4]{};
  if (!ansi_widened) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, ansi_text, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(statement_, 1, SQL_C_CHAR, matched, sizeof(matched),
                         nullptr));
    EXPECT_STREQ("1", reinterpret_cast<const char*>(matched));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(statement_));
  }

  SQLCHAR quoted[] = "SELECT 'a%b' LIKE 'a''%b' {escape ''''}";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, quoted, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 1, SQL_C_CHAR, matched, sizeof(matched),
                       nullptr));
  EXPECT_STREQ("1", reinterpret_cast<const char*>(matched));
}

TEST_F(NativeSqlIntegrationTest, KeepsEscapesInsideNestedComments) {
  SQLCHAR input[] =
      "SELECT 1 /* outer /* inner */ {fn UCASE(ignored)} */, "
      "{fn UCASE('ok')}";
  constexpr char expected[] =
      "SELECT 1 /* outer /* inner */ {fn UCASE(ignored)} */, UPPER('ok')";
  SQLCHAR output[sizeof(expected)]{};
  SQLINTEGER length = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLNativeSql(connection_, input, SQL_NTS, output, sizeof(output),
                         &length));
  EXPECT_STREQ(expected, reinterpret_cast<const char*>(output));
  EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(expected) - 1), length);

  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, input, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  SQLINTEGER number = 0;
  SQLCHAR text[8]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 1, SQL_C_SLONG, &number, sizeof(number),
                       nullptr));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 2, SQL_C_CHAR, text, sizeof(text), nullptr));
  EXPECT_EQ(1, number);
  EXPECT_STREQ("OK", reinterpret_cast<const char*>(text));
}

TEST_F(NativeSqlIntegrationTest, KeepsEscapesInsideBackslashQuotedStrings) {
  SQLCHAR input[] =
      "SELECT E'it\\'s {fn UCASE(ignored)}', {fn UCASE('ok')}";
  constexpr char expected[] =
      "SELECT E'it\\'s {fn UCASE(ignored)}', UPPER('ok')";
  SQLCHAR output[sizeof(expected)]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLNativeSql(connection_, input, SQL_NTS, output, sizeof(output),
                         nullptr));
  EXPECT_STREQ(expected, reinterpret_cast<const char*>(output));

  // E-prefixed strings exercise the translator above. Redshift execution uses
  // its ordinary quoted spelling with the identical value and escape payload.
  SQLCHAR redshift_input[] =
      "SELECT 'it''s {fn UCASE(ignored)}', {fn UCASE('ok')}";
  const bool redshift = odbcpp::test::compiled_redshift_product();
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_,
      redshift ? redshift_input : input, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  SQLCHAR first[64]{};
  SQLCHAR second[8]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 1, SQL_C_CHAR, first, sizeof(first),
                       nullptr));
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 2, SQL_C_CHAR, second, sizeof(second),
                       nullptr));
  EXPECT_STREQ("it's {fn UCASE(ignored)}",
               reinterpret_cast<const char*>(first));
  EXPECT_STREQ("OK", reinterpret_cast<const char*>(second));
}

TEST_F(NativeSqlIntegrationTest, DollarSignsInIdentifiersDoNotHideEscapes) {
  SQLCHAR input[] =
      "SELECT 1 AS foo$tag$bar, {fn UCASE('ok')}, 2 AS baz$tag$qux";
  constexpr char expected[] =
      "SELECT 1 AS foo$tag$bar, UPPER('ok'), 2 AS baz$tag$qux";
  SQLCHAR output[sizeof(expected)]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLNativeSql(connection_, input, SQL_NTS, output, sizeof(output),
                         nullptr));
  EXPECT_STREQ(expected, reinterpret_cast<const char*>(output));

  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement_, input, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
  SQLCHAR value[8]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(statement_, 2, SQL_C_CHAR, value, sizeof(value),
                       nullptr));
  EXPECT_STREQ("OK", reinterpret_cast<const char*>(value));
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

TEST_F(NativeSqlIntegrationTest, BackendTranslationErrorsPreservePreparedSqlAndRecover) {
  SQLCHAR valid[] = "SELECT {fn IFNULL(NULL, 42)}";
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(statement_, valid, SQL_NTS));
  for (const auto& [input, state] : {
      std::pair{"SELECT {d '2023-02-29'}", "22007"},
      std::pair{"SELECT {fn UCASE('ok')", "42000"},
      std::pair{"{?= call answer()}", "HYC00"}}) {
    for (const bool wide : {false, true}) {
      SCOPED_TRACE(input);
      SCOPED_TRACE(wide);
      std::string narrow(input);
      auto unicode = odbcpp::test::text::utf8_to_wide(input);
      ASSERT_TRUE(unicode);
      // Translation fails before replacing prepared text or executing SQL.
      EXPECT_EQ(SQL_ERROR, wide
          ? SQLPrepareW(statement_, unicode->data(),
                        static_cast<SQLINTEGER>(unicode->size()))
          : SQLPrepare(statement_, reinterpret_cast<SQLCHAR*>(narrow.data()), SQL_NTS));
      EXPECT_EQ(state, diagnostic_state(SQL_HANDLE_STMT, statement_));
      EXPECT_EQ(SQL_ERROR, wide
          ? SQLExecDirectW(statement_, unicode->data(),
                           static_cast<SQLINTEGER>(unicode->size()))
          : SQLExecDirect(statement_, reinterpret_cast<SQLCHAR*>(narrow.data()), SQL_NTS));
      EXPECT_EQ(state, diagnostic_state(SQL_HANDLE_STMT, statement_));
      // The manager's state table returns the statement to "allocated" after
      // the failed prepare, so it refuses SQLExecute without calling the driver.
      if (odbcpp::test::dm::stops_case(
              odbcpp::test::dm::Deviation::StateTableAfterFailedCall)) {
        EXPECT_EQ(SQL_ERROR, SQLExecute(statement_));
        EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, statement_));
        return;
      }
      ASSERT_EQ(SQL_SUCCESS, SQLExecute(statement_));
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement_));
      SQLINTEGER value = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(statement_, 1, SQL_C_SLONG, &value,
                                       sizeof(value), nullptr));
      EXPECT_EQ(42, value);
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(statement_));
    }
  }
}
