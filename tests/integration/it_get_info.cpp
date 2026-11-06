#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <cstdint>
#include <algorithm>
#include <chrono>

#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "core/database/database_factory.h"
#endif
#include "tests/test_connection_config.h"
#include "tests/test_dm_expectations.h"
#include "tests/test_product_profile.h"
#include "tests/test_driver_exports.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>


namespace {
// One original per-case window, including reconnects and sibling handles.
// Apply caps only on I/O entry, leaving allocation defaults and deliberate shorter
// timeout values intact. Never restore attributes after a call: its diagnostics belong to the test.
constexpr SQLULEN fixture_timeout_seconds(std::int64_t left, SQLULEN requested = 0) {
  if (left <= 0) return 0;
  // The original fixture window is the bound. Valid cold Redshift statements
  // took 6.84s and 9.56s; an extra five-second clamp cancelled those queries.
  const auto cap = static_cast<SQLULEN>(left);
  return requested == 0 ? cap : std::min(cap, requested);
}
static_assert(fixture_timeout_seconds(40) == 40);
static_assert(fixture_timeout_seconds(40, 60) == 40);
static_assert(fixture_timeout_seconds(4, 2) == 2);
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

#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
std::optional<bool> fixture_redshift_profile(rs::core::database::DatabaseType type) {
  using rs::core::database::DatabaseType;
  if (type == DatabaseType::PostgreSQL) return false;
  if (type == DatabaseType::Redshift) return true;
  return std::nullopt;
}

TEST(GetInfoFixtureProfile, KeepsPostgresAndRedshiftExpectationsDistinct) {
  using rs::core::database::DatabaseType;
  EXPECT_EQ(std::optional<bool>(false), fixture_redshift_profile(DatabaseType::PostgreSQL));
  EXPECT_EQ(std::optional<bool>(true), fixture_redshift_profile(DatabaseType::Redshift));
  EXPECT_FALSE(fixture_redshift_profile(static_cast<DatabaseType>(-1)).has_value());
}
#endif

std::string diagnostic_state(SQLHDBC connection) {
  SQLCHAR state[6]{};
  EXPECT_EQ(SQL_SUCCESS,
            odbcpp::test::diag_rec(SQL_HANDLE_DBC, connection, 1, state, nullptr,
                                   nullptr, 0, nullptr));
  return reinterpret_cast<const char*>(state);
}

class GetInfoIntegrationTest : public ::testing::Test {
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
    // Select independent literal expectations by the existing compiled product,
    // never by learning expected capabilities from the SQLGetInfo output.
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
    const std::optional<bool> profile = odbcpp::test::compiled_redshift_product();
#else
    const auto profile = fixture_redshift_profile(
        rs::core::database::DatabaseFactory::get_compiled_database_type());
#endif
    ASSERT_TRUE(profile.has_value()) << "Unsupported integration fixture product profile";
    redshift_profile_ = *profile;
    // Independent API expectation includes local Need Data cancellation on every
    // carrier; this does not claim server cancellation for Async/Auto transports.
    // Refuse malformed controls before connection.
#ifdef _WIN32
    char* raw_value = nullptr;
    std::size_t value_size = 0;
    const auto read_status = _dupenv_s(
        &raw_value, &value_size, "ODBCPP_TEST_EXPECT_SQLCANCEL");
    const std::unique_ptr<char, decltype(&std::free)> owned_value(
        raw_value, &std::free);
    ASSERT_EQ(0, read_status)
        << "Could not read fixture cancellation expectation";
    const char* value = owned_value.get();
#else
    const char* value = std::getenv("ODBCPP_TEST_EXPECT_SQLCANCEL");
#endif
    if (value) {
      const std::string_view expected(value);
      ASSERT_TRUE(expected == "0" || expected == "1")
          << "ODBCPP_TEST_EXPECT_SQLCANCEL must be 0 or 1";
      expect_sqlcancel_ = expected == "1";
    }
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
  }

  void TearDown() override {
    if (connection_) {
      const auto disconnected = SQLDisconnect(connection_);
      if (disconnected != SQL_SUCCESS) {
        // Failed login or a retired timeout connection is already closed.
        EXPECT_EQ(SQL_ERROR, disconnected);
        EXPECT_EQ("08003", diagnostic_state(connection_));
      }
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection_));
      connection_ = SQL_NULL_HDBC;
    }
    if (environment_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment_));
      environment_ = SQL_NULL_HENV;
    }
  }

  std::string string_info(SQLUSMALLINT type) {
    SQLCHAR value[1024]{};
    SQLSMALLINT length = -1;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetInfo(connection_, type, value, sizeof(value), &length));
    if (length < 0) return {};
    return {reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(length)};
  }

  void execute(std::string_view sql) {
    SQLHSTMT statement = SQL_NULL_HSTMT;
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement));
    struct StatementOwner {
      SQLHSTMT handle;
      ~StatementOwner() { EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, handle)); }
    } owner{statement};
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(statement,
                            reinterpret_cast<SQLCHAR*>(
                                const_cast<char*>(sql.data())),
                            static_cast<SQLINTEGER>(sql.size())))
        << sql << ": " << statement_diagnostic(statement);
  }


  std::string statement_diagnostic(SQLHSTMT statement) {
    SQLCHAR state[6]{}, message[1024]{};
    if (::SQLGetDiagRec(SQL_HANDLE_STMT, statement, 1, state, nullptr,
                       message, sizeof(message), nullptr) != SQL_SUCCESS) {
      return "diagnostic unavailable";
    }
    return std::string(reinterpret_cast<const char*>(state)) + " " +
        reinterpret_cast<const char*>(message);
  }

  void verify_predicate(std::string_view predicate, bool prepared = false,
                        std::string_view from = {}, std::string_view typed_expression = {},
                        SQLSMALLINT expected_type = 0) {
    SCOPED_TRACE(predicate);
    SCOPED_TRACE(prepared);
    SQLHSTMT statement = SQL_NULL_HSTMT;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement));
    struct StatementOwner {
      SQLHSTMT handle;
      ~StatementOwner() { SQLFreeHandle(SQL_HANDLE_STMT, handle); }
    } owner{statement};
    const std::string sql = "SELECT CASE WHEN (" + std::string(predicate) +
        ") THEN 1 ELSE 0 END" +
        (typed_expression.empty() ? "" : ", " + std::string(typed_expression)) +
        (from.empty() ? "" : " FROM " + std::string(from));
    if (prepared) {
      ASSERT_TRUE(fixture_statement_budget(fixture_end_, statement));
      ASSERT_EQ(SQL_SUCCESS, ::SQLPrepare(statement,
          reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.data())),
          static_cast<SQLINTEGER>(sql.size()))) << statement_diagnostic(statement);
      ASSERT_TRUE(fixture_statement_budget(fixture_end_, statement));
      ASSERT_EQ(SQL_SUCCESS, ::SQLExecute(statement)) << statement_diagnostic(statement);
    } else {
      ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement,
          reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.data())),
          static_cast<SQLINTEGER>(sql.size()))) << statement_diagnostic(statement);
    }
    if (!typed_expression.empty()) {
      SQLCHAR name[64]{};
      SQLSMALLINT type = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(statement, 2, name, sizeof(name),
          nullptr, &type, nullptr, nullptr, nullptr));
      EXPECT_EQ(expected_type, type) << typed_expression;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement));
    SQLINTEGER actual = -1;
    SQLLEN length = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(statement, 1, SQL_C_SLONG, &actual, sizeof(actual), &length));
    EXPECT_NE(SQL_NULL_DATA, length);
    EXPECT_EQ(1, actual);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(statement));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(statement));
  }

  void verify_sql_feature(SQLUSMALLINT type, SQLUINTEGER bit, std::string_view predicate) {
    SCOPED_TRACE(type);
    SCOPED_TRACE(bit);
    SCOPED_TRACE(predicate);
    SQLUINTEGER mask = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, type, &mask, sizeof(mask), nullptr));
    // Redshift remains a qualification probe: assert the claim is withheld,
    // then run the same semantic assertion so future acceptance cannot hide
    // behind a zero capability mask. PostgreSQL must advertise the exact bit.
    ASSERT_EQ(redshift_profile_ ? 0u : bit, mask & bit);
    ASSERT_NO_FATAL_FAILURE(verify_predicate(predicate));
  }

  bool expected_support(SQLUSMALLINT id) const {
    return id == SQL_API_SQLCANCEL ? expect_sqlcancel_
                                   : odbcpp::test::expected_support(id) ||
                                         odbcpp::test::dm::manager_reports_function(id);
  }

  bool redshift_profile_{false};
  bool expect_sqlcancel_{true};
  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
};

TEST_F(GetInfoIntegrationTest, ReportsStaticStringCapabilities) {
  const std::array<std::pair<SQLUSMALLINT, std::string_view>, 33>
      expected_values{{
          {SQL_DRIVER_NAME, ODBCPP_EXPECT_DRIVER_FILENAME},
          {SQL_DRIVER_VER, ODBCPP_DRIVER_VERSION},
          {SQL_DRIVER_ODBC_VER, "03.80"},
          // The manager answers SQL_ODBC_VER with its own version.
          {SQL_ODBC_VER, odbcpp::test::dm::text(
              odbcpp::test::dm::Deviation::ManagerOdbcVersion, "03.80")},
          {SQL_DBMS_NAME, redshift_profile_ ? "Amazon Redshift" : "PostgreSQL"},
          {SQL_IDENTIFIER_QUOTE_CHAR, "\""},
          {SQL_CATALOG_NAME_SEPARATOR, "."},
          {SQL_CATALOG_TERM, "database"},
          {SQL_SCHEMA_TERM, "schema"},
          {SQL_TABLE_TERM, "table"},
          {SQL_PROCEDURE_TERM, "procedure"},
          {SQL_SEARCH_PATTERN_ESCAPE, "\\"},
          {SQL_CATALOG_NAME, "Y"},
          {SQL_COLUMN_ALIAS, "Y"},
          {SQL_DESCRIBE_PARAMETER, "Y"},
          {SQL_EXPRESSIONS_IN_ORDERBY, "Y"},
          {SQL_INTEGRITY, "Y"},
          {SQL_LIKE_ESCAPE_CLAUSE, "Y"},
          {SQL_MULT_RESULT_SETS, "Y"},
          {SQL_OUTER_JOINS, redshift_profile_ ? "N" : "Y"},
          {SQL_PROCEDURES, "Y"},
          {SQL_ACCESSIBLE_TABLES, "N"},
          {SQL_ACCESSIBLE_PROCEDURES, "N"},
          {SQL_DATA_SOURCE_READ_ONLY, "N"},
          {SQL_MAX_ROW_SIZE_INCLUDES_LONG, "N"},
          {SQL_MULTIPLE_ACTIVE_TXN, "N"},
          {SQL_NEED_LONG_DATA_LEN, "N"},
          {SQL_ORDER_BY_COLUMNS_IN_SELECT, "N"},
          {SQL_ROW_UPDATES, "N"},
          {SQL_COLLATION_SEQ, ""},
          {SQL_KEYWORDS,
           redshift_profile_
               ? "AES128,AES256,ALLOWOVERWRITE,ANALYSE,ANALYZE,ARRAY,AZ64,BACKUP,BINARY,BLANKSASNULL,BYTEDICT,BZIP2,CREDENTIALS,CURRENT_USER_ID,DEFLATE,DEFRAG,DELTA,DELTA32K,DISABLE,DO,EMPTYASNULL,ENABLE,ENCODE,ENCRYPTION,EXPLICIT,FREEZE,GLOBALDICT256,GLOBALDICT64K,GZIP,IGNORE,ILIKE,ISNULL,LIMIT,LOCALTIME,LOCALTIMESTAMP,LUN,LUNS,LZO,LZOP,MINUS,MOSTLY16,MOSTLY32,MOSTLY8,NEW,NOTNULL,NULLS,OFF,OFFLINE,OFFSET,OID,OLD,PARALLEL,PARTITION,PERCENT,PERMISSIONS,PIVOT,PLACING,RAW,READRATIO,RECOVER,REJECTLOG,RESORT,RESPECT,RESTORE,SIMILAR,SYSDATE,SYSTEM,TAG,TDES,TEXT255,TEXT32K,TOP,TRUNCATECOLUMNS,UNNEST,UNPIVOT,VERBOSE,WALLET,WITHOUT"
               : "ANALYSE,ANALYZE,ARRAY,ASYMMETRIC,CURRENT_CATALOG,CURRENT_ROLE,CURRENT_SCHEMA,DO,ILIKE,LATERAL,LIMIT,LOCALTIME,LOCALTIMESTAMP,OFFSET,PLACING,RETURNING,SYMMETRIC,VARIADIC,VERBOSE,WINDOW"},
          {SQL_SPECIAL_CHARACTERS, ""},
          {SQL_XOPEN_CLI_YEAR, "1995"},
      }};
  for (const auto& [type, expected] : expected_values) {
    EXPECT_EQ(expected, string_info(type)) << type;

    SQLWCHAR wide_value[1024]{};
    SQLSMALLINT wide_bytes = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetInfoW(connection_, type, wide_value, sizeof(wide_value),
                          &wide_bytes))
        << type;
    // iODBC 3.52.16 reports its wide SQL_ODBC_VER length in characters.
    const bool length_in_characters =
        type == SQL_ODBC_VER &&
        odbcpp::test::dm::deviates(
            odbcpp::test::dm::Deviation::OdbcVersionWideLengthInCharacters);
    EXPECT_EQ(static_cast<SQLSMALLINT>(
                  expected.size() * (length_in_characters ? 1 : sizeof(SQLWCHAR))),
              wide_bytes)
        << type;
    // unixODBC 2.3.14 answers SQL_XOPEN_CLI_YEAR itself, differently for
    // the wide call.
    const std::string_view wide_expected =
        type == SQL_XOPEN_CLI_YEAR
            ? std::string_view(odbcpp::test::dm::text(
                  odbcpp::test::dm::Deviation::ManagerWideXopenCliYear, expected.data()))
            : expected;
    for (std::size_t index = 0; index < wide_expected.size(); ++index) {
      EXPECT_EQ(static_cast<SQLWCHAR>(wide_expected[index]), wide_value[index])
          << type << " at " << index;
    }
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide_value[expected.size()]) << type;
  }
}

TEST_F(GetInfoIntegrationTest, ReportsNumericCapabilities) {
  const std::array<std::pair<SQLUSMALLINT, SQLUSMALLINT>, 27>
      small_values{{
          {SQL_ACTIVE_ENVIRONMENTS, 0},
          {SQL_MAX_CONCURRENT_ACTIVITIES, 0},
          {SQL_MAX_DRIVER_CONNECTIONS, 0},
          {SQL_FILE_USAGE, SQL_FILE_NOT_SUPPORTED},
          {SQL_TXN_CAPABLE, SQL_TC_ALL},
          {SQL_CURSOR_COMMIT_BEHAVIOR, SQL_CB_PRESERVE},
          {SQL_CURSOR_ROLLBACK_BEHAVIOR, SQL_CB_PRESERVE},
          {SQL_MAX_IDENTIFIER_LEN, static_cast<SQLUSMALLINT>(redshift_profile_ ? 127 : 63)},
          {SQL_IDENTIFIER_CASE, SQL_IC_LOWER},
          {SQL_QUOTED_IDENTIFIER_CASE, SQL_IC_SENSITIVE},
          {SQL_CATALOG_LOCATION, SQL_CL_START},
          {SQL_NULL_COLLATION, SQL_NC_HIGH},
          {SQL_NON_NULLABLE_COLUMNS, SQL_NNC_NON_NULL},
          {SQL_CONCAT_NULL_BEHAVIOR, SQL_CB_NULL},
          {SQL_CORRELATION_NAME, SQL_CN_ANY},
          {SQL_GROUP_BY, SQL_GB_NO_RELATION},
          {SQL_ODBC_API_CONFORMANCE, SQL_OAC_LEVEL1},
          {SQL_ODBC_SAG_CLI_CONFORMANCE, SQL_OSCC_COMPLIANT},
          {SQL_ODBC_SQL_CONFORMANCE, static_cast<SQLUSMALLINT>(
              redshift_profile_ ? SQL_OSC_MINIMUM : SQL_OSC_CORE)},
          {SQL_MAX_COLUMN_NAME_LEN, static_cast<SQLUSMALLINT>(redshift_profile_ ? 127 : 63)},
          {SQL_MAX_TABLE_NAME_LEN, static_cast<SQLUSMALLINT>(redshift_profile_ ? 127 : 63)},
          {SQL_MAX_SCHEMA_NAME_LEN, static_cast<SQLUSMALLINT>(redshift_profile_ ? 127 : 63)},
          {SQL_MAX_CATALOG_NAME_LEN, static_cast<SQLUSMALLINT>(redshift_profile_ ? 127 : 63)},
          {SQL_MAX_PROCEDURE_NAME_LEN, static_cast<SQLUSMALLINT>(redshift_profile_ ? 127 : 63)},
          {SQL_MAX_USER_NAME_LEN, static_cast<SQLUSMALLINT>(redshift_profile_ ? 127 : 63)},
          {SQL_MAX_COLUMNS_IN_TABLE, 1600},
          {SQL_MAX_COLUMNS_IN_SELECT, 1600},
      }};
  for (const auto& [type, expected] : small_values) {
    SQLUSMALLINT value = std::numeric_limits<SQLUSMALLINT>::max();
    SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetInfo(connection_, type, &value, sizeof(value), &length))
        << type;
    EXPECT_EQ(expected, value) << type;
    EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(value)), length) << type;
  }

  constexpr SQLUSMALLINT zero_small_values[]{
      SQL_MAX_COLUMNS_IN_GROUP_BY, SQL_MAX_COLUMNS_IN_INDEX,
      SQL_MAX_COLUMNS_IN_ORDER_BY, SQL_MAX_CURSOR_NAME_LEN,
      SQL_MAX_TABLES_IN_SELECT};
  for (const auto type : zero_small_values) {
    SQLUSMALLINT value = std::numeric_limits<SQLUSMALLINT>::max();
    SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetInfo(connection_, type, &value, sizeof(value), &length))
        << type;
    EXPECT_EQ(0, value) << type;
    EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(value)), length) << type;
  }

  // Redshift's database isolation mode is not an observed standard session
  // default. Keep its truthful refusal separate from capabilities that succeed.
  for (const bool wide : {false, true}) {
    SCOPED_TRACE(wide);
    SQLUINTEGER value = 0xaabbccdd;
    SQLSMALLINT length = 79;
    const auto result = wide
        ? SQLGetInfoW(connection_, SQL_DEFAULT_TXN_ISOLATION,
                      &value, sizeof(value), &length)
        : SQLGetInfo(connection_, SQL_DEFAULT_TXN_ISOLATION,
                     &value, sizeof(value), &length);
    if (redshift_profile_) {
      ASSERT_EQ(SQL_ERROR, result);
      EXPECT_EQ("HY000", diagnostic_state(connection_));
      EXPECT_EQ(0xaabbccddu, value);
      EXPECT_EQ(79, length);
    } else {
      ASSERT_EQ(SQL_SUCCESS, result);
      EXPECT_EQ(SQL_TXN_READ_COMMITTED, value);
      EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(value)), length);
    }
  }

  const std::array<std::pair<SQLUSMALLINT, SQLUINTEGER>, 44>
      integer_values{{
          {SQL_TXN_ISOLATION_OPTION,
           redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(
               SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED |
               SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE)},
          {SQL_SCROLL_OPTIONS, SQL_SO_FORWARD_ONLY | SQL_SO_STATIC},
          {SQL_GETDATA_EXTENSIONS, SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BLOCK},
          {SQL_ASYNC_MODE, SQL_AM_NONE},
          {SQL_BATCH_ROW_COUNT, 0},
          {SQL_BATCH_SUPPORT, 0},
          {SQL_BOOKMARK_PERSISTENCE, SQL_BP_TRANSACTION},
          {SQL_CONVERT_FUNCTIONS, redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(SQL_FN_CVT_CONVERT)},
          {SQL_CURSOR_SENSITIVITY, SQL_UNSPECIFIED},
          {SQL_DYNAMIC_CURSOR_ATTRIBUTES1, 0},
          {SQL_DYNAMIC_CURSOR_ATTRIBUTES2, 0},
          {SQL_KEYSET_CURSOR_ATTRIBUTES1, 0},
          {SQL_KEYSET_CURSOR_ATTRIBUTES2, 0},
          {SQL_MAX_ASYNC_CONCURRENT_STATEMENTS, 0},
          {SQL_POS_OPERATIONS, SQL_POS_POSITION},
          {SQL_POSITIONED_STATEMENTS, 0},
          {SQL_STATIC_CURSOR_ATTRIBUTES1, SQL_CA1_NEXT | SQL_CA1_ABSOLUTE | SQL_CA1_RELATIVE | SQL_CA1_POS_POSITION | SQL_CA1_BOOKMARK | SQL_CA1_BULK_FETCH_BY_BOOKMARK},
          {SQL_STATIC_CURSOR_ATTRIBUTES2, SQL_CA2_READ_ONLY_CONCURRENCY},
          {SQL_STATIC_SENSITIVITY, 0},
          {SQL_FETCH_DIRECTION, SQL_FD_FETCH_NEXT | SQL_FD_FETCH_FIRST | SQL_FD_FETCH_LAST |
           SQL_FD_FETCH_PREV | SQL_FD_FETCH_ABSOLUTE | SQL_FD_FETCH_RELATIVE | SQL_FD_FETCH_BOOKMARK},
          {SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, SQL_CA1_NEXT},
          {SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2,
           SQL_CA2_READ_ONLY_CONCURRENCY},
          {SQL_LOCK_TYPES, SQL_LCK_NO_CHANGE},
          {SQL_PARAM_ARRAY_ROW_COUNTS, SQL_PARC_BATCH},
          {SQL_PARAM_ARRAY_SELECTS, SQL_PAS_BATCH},
          {SQL_SCROLL_CONCURRENCY, SQL_SCCO_READ_ONLY},
          {SQL_DDL_INDEX, redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(SQL_DI_CREATE_INDEX | SQL_DI_DROP_INDEX)},
          {SQL_INSERT_STATEMENT,
           SQL_IS_INSERT_LITERALS | SQL_IS_INSERT_SEARCHED |
               SQL_IS_SELECT_INTO},
          {SQL_ODBC_INTERFACE_CONFORMANCE, SQL_OIC_CORE},
          {SQL_SQL_CONFORMANCE, redshift_profile_ ? 0u :
              static_cast<SQLUINTEGER>(SQL_SC_SQL92_ENTRY)},
          {SQL_STANDARD_CLI_CONFORMANCE, SQL_SCC_XOPEN_CLI_VERSION1},
          {SQL_UNION, SQL_U_UNION | SQL_U_UNION_ALL},
          {SQL_DTC_TRANSITION_COST, 0},
          {SQL_CATALOG_USAGE, 0},
          {SQL_SCHEMA_USAGE,
           SQL_SU_DML_STATEMENTS | SQL_SU_PROCEDURE_INVOCATION |
               SQL_SU_TABLE_DEFINITION | (redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(SQL_SU_INDEX_DEFINITION)) |
               SQL_SU_PRIVILEGE_DEFINITION},
          {SQL_CREATE_TABLE,
           static_cast<SQLUINTEGER>(
               SQL_CT_CREATE_TABLE | SQL_CT_LOCAL_TEMPORARY |
               SQL_CT_COLUMN_CONSTRAINT | SQL_CT_COLUMN_DEFAULT |
               SQL_CT_TABLE_CONSTRAINT | SQL_CT_CONSTRAINT_NAME_DEFINITION |
               (redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(
                   SQL_CT_COMMIT_PRESERVE | SQL_CT_COMMIT_DELETE |
                   SQL_CT_COLUMN_COLLATION)))},
          {SQL_DROP_TABLE,
           static_cast<SQLUINTEGER>(SQL_DT_DROP_TABLE | SQL_DT_CASCADE | SQL_DT_RESTRICT)},
          {SQL_CREATE_VIEW,
           static_cast<SQLUINTEGER>(
               SQL_CV_CREATE_VIEW |
               (redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(SQL_CV_CHECK_OPTION)))},
          {SQL_DROP_VIEW,
           static_cast<SQLUINTEGER>(SQL_DV_DROP_VIEW | SQL_DV_CASCADE | SQL_DV_RESTRICT)},
          {SQL_ALTER_TABLE,
           static_cast<SQLUINTEGER>(
               SQL_AT_ADD_COLUMN_SINGLE | SQL_AT_ADD_COLUMN_DEFAULT |
               SQL_AT_SET_COLUMN_DEFAULT | SQL_AT_DROP_COLUMN_DEFAULT |
               SQL_AT_DROP_COLUMN_CASCADE | SQL_AT_DROP_COLUMN_RESTRICT |
               SQL_AT_ADD_TABLE_CONSTRAINT |
               SQL_AT_DROP_TABLE_CONSTRAINT_CASCADE |
               SQL_AT_DROP_TABLE_CONSTRAINT_RESTRICT |
               SQL_AT_CONSTRAINT_NAME_DEFINITION |
               (redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(SQL_AT_ADD_COLUMN_COLLATION)))},
          {SQL_CREATE_SCHEMA,
           static_cast<SQLUINTEGER>(SQL_CS_CREATE_SCHEMA | SQL_CS_AUTHORIZATION)},
          {SQL_DROP_SCHEMA,
           static_cast<SQLUINTEGER>(SQL_DS_DROP_SCHEMA | SQL_DS_CASCADE | SQL_DS_RESTRICT)},
          {SQL_INDEX_KEYWORDS,
           redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(SQL_IK_ASC | SQL_IK_DESC)},
          {SQL_INFO_SCHEMA_VIEWS,
           redshift_profile_
               ? static_cast<SQLUINTEGER>(
                     SQL_ISV_COLUMNS | SQL_ISV_SCHEMATA | SQL_ISV_TABLES | SQL_ISV_VIEWS)
               : static_cast<SQLUINTEGER>(
                     SQL_ISV_CHARACTER_SETS | SQL_ISV_CHECK_CONSTRAINTS |
                     SQL_ISV_COLLATIONS | SQL_ISV_COLUMN_DOMAIN_USAGE |
                     SQL_ISV_COLUMN_PRIVILEGES | SQL_ISV_COLUMNS |
                     SQL_ISV_CONSTRAINT_COLUMN_USAGE |
                     SQL_ISV_CONSTRAINT_TABLE_USAGE | SQL_ISV_DOMAIN_CONSTRAINTS |
                     SQL_ISV_DOMAINS | SQL_ISV_KEY_COLUMN_USAGE |
                     SQL_ISV_REFERENTIAL_CONSTRAINTS | SQL_ISV_SCHEMATA |
                     SQL_ISV_SQL_LANGUAGES | SQL_ISV_TABLE_CONSTRAINTS |
                     SQL_ISV_TABLE_PRIVILEGES | SQL_ISV_TABLES |
                     SQL_ISV_USAGE_PRIVILEGES | SQL_ISV_VIEW_COLUMN_USAGE |
                     SQL_ISV_VIEW_TABLE_USAGE | SQL_ISV_VIEWS)},
      }};
  for (const auto& [type, expected] : integer_values) {
    SQLUINTEGER value = std::numeric_limits<SQLUINTEGER>::max();
    SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetInfo(connection_, type, &value, sizeof(value), &length))
        << type;
    EXPECT_EQ(expected, value) << type;
    EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(value)), length) << type;
  }


  constexpr SQLUSMALLINT zero_integer_values[]{
      SQL_ALTER_DOMAIN,
      SQL_CREATE_ASSERTION,
      SQL_CREATE_CHARACTER_SET,
      SQL_CREATE_COLLATION,
      SQL_CREATE_DOMAIN,
      SQL_CREATE_TRANSLATION,
      SQL_CONVERT_INTERVAL_DAY_TIME,
      SQL_CONVERT_INTERVAL_YEAR_MONTH,
      SQL_CONVERT_TINYINT,
      SQL_DROP_ASSERTION,
      SQL_DROP_CHARACTER_SET,
      SQL_DROP_COLLATION,
      SQL_DROP_DOMAIN,
      SQL_DROP_TRANSLATION,
      SQL_MAX_BINARY_LITERAL_LEN,
      SQL_MAX_CHAR_LITERAL_LEN,
      SQL_MAX_INDEX_SIZE,
      SQL_MAX_ROW_SIZE,
      SQL_MAX_STATEMENT_LEN,
      SQL_SQL92_FOREIGN_KEY_DELETE_RULE,
      SQL_SQL92_FOREIGN_KEY_UPDATE_RULE,
      SQL_SQL92_GRANT,
      SQL_SQL92_REVOKE,
  };

  // #144: supported conversions and interval escapes replace the old zero claims.
  constexpr SQLUINTEGER characters = SQL_CVT_CHAR | SQL_CVT_VARCHAR | SQL_CVT_LONGVARCHAR |
      SQL_CVT_WCHAR | SQL_CVT_WVARCHAR | SQL_CVT_WLONGVARCHAR;
  constexpr SQLUINTEGER numeric = SQL_CVT_SMALLINT | SQL_CVT_INTEGER | SQL_CVT_BIGINT |
      SQL_CVT_REAL | SQL_CVT_FLOAT | SQL_CVT_DOUBLE | SQL_CVT_NUMERIC | SQL_CVT_DECIMAL;
  constexpr SQLUINTEGER binary = SQL_CVT_BINARY | SQL_CVT_VARBINARY | SQL_CVT_LONGVARBINARY;
  constexpr SQLUINTEGER add = SQL_FN_TSI_SECOND | SQL_FN_TSI_MINUTE | SQL_FN_TSI_HOUR |
      SQL_FN_TSI_DAY | SQL_FN_TSI_WEEK | SQL_FN_TSI_MONTH | SQL_FN_TSI_QUARTER | SQL_FN_TSI_YEAR;
    SQLUINTEGER character_targets = characters | numeric | binary | SQL_CVT_DATE | SQL_CVT_TIME | SQL_CVT_TIMESTAMP;
    if (!redshift_profile_) {
      character_targets |= SQL_CVT_BIT;
#ifdef SQL_CVT_GUID
      character_targets |= SQL_CVT_GUID;
#endif
    }
    // Redshift reports none of these until its live acceptance passes (#144):
    // the translations exist, but no Redshift statement behind them has run.
    const auto pg_only = [&](SQLUINTEGER value) -> SQLUINTEGER { return redshift_profile_ ? 0u : value; };
    const std::pair<SQLUSMALLINT, SQLUINTEGER> cases[]{
        {SQL_CONVERT_FUNCTIONS, pg_only(SQL_FN_CVT_CONVERT)},
        {SQL_CONVERT_SMALLINT, pg_only(numeric | characters)}, {SQL_CONVERT_INTEGER, pg_only(numeric | characters)},
        {SQL_CONVERT_BIGINT, pg_only(numeric | characters)}, {SQL_CONVERT_REAL, pg_only(numeric | characters)},
        {SQL_CONVERT_FLOAT, pg_only(numeric | characters)}, {SQL_CONVERT_DOUBLE, pg_only(numeric | characters)},
        {SQL_CONVERT_NUMERIC, pg_only(numeric | characters)}, {SQL_CONVERT_DECIMAL, pg_only(numeric | characters)},
        {SQL_CONVERT_BIT, pg_only(SQL_CVT_BIT | (redshift_profile_ ? 0u : characters))},
        {SQL_CONVERT_CHAR, pg_only(character_targets)}, {SQL_CONVERT_VARCHAR, pg_only(character_targets)},
        {SQL_CONVERT_LONGVARCHAR, pg_only(character_targets)}, {SQL_CONVERT_WCHAR, pg_only(character_targets)},
        {SQL_CONVERT_WVARCHAR, pg_only(character_targets)}, {SQL_CONVERT_WLONGVARCHAR, pg_only(character_targets)},
        {SQL_CONVERT_BINARY, pg_only(binary | characters)}, {SQL_CONVERT_VARBINARY, pg_only(binary | characters)},
        {SQL_CONVERT_LONGVARBINARY, pg_only(binary | characters)},
        {SQL_CONVERT_DATE, pg_only(SQL_CVT_DATE | SQL_CVT_TIMESTAMP | characters)},
        {SQL_CONVERT_TIME, pg_only(SQL_CVT_TIME | characters)},
        {SQL_CONVERT_TIMESTAMP, pg_only(SQL_CVT_TIMESTAMP | SQL_CVT_DATE | SQL_CVT_TIME | characters)},
        {SQL_CONVERT_TINYINT, pg_only(0)}, {SQL_CONVERT_INTERVAL_YEAR_MONTH, pg_only(0)}, {SQL_CONVERT_INTERVAL_DAY_TIME, pg_only(0)},
        {SQL_TIMEDATE_ADD_INTERVALS, pg_only(add)}, {SQL_TIMEDATE_DIFF_INTERVALS, pg_only(add & ~SQL_FN_TSI_WEEK)},
        {SQL_TIMEDATE_FUNCTIONS, pg_only(SQL_FN_TD_CURDATE | SQL_FN_TD_CURTIME | SQL_FN_TD_NOW |
            SQL_FN_TD_TIMESTAMPADD | SQL_FN_TD_TIMESTAMPDIFF)},
#if defined(SQL_CONVERT_GUID) && defined(SQL_CVT_GUID)
        {SQL_CONVERT_GUID, pg_only(redshift_profile_ ? 0u : SQL_CVT_GUID | characters)},
#endif
    };
  for (const auto& [type, expected] : cases) {
    SQLUINTEGER value = 0xfefefefe;
    SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, type, &value, sizeof(value), &length)) << type;
    EXPECT_EQ(expected, value) << type;
    EXPECT_EQ(sizeof(value), static_cast<std::size_t>(length));
  }
  for (const auto type : zero_integer_values) {
    SQLUINTEGER value = std::numeric_limits<SQLUINTEGER>::max();
    SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetInfo(connection_, type, &value, sizeof(value), &length))
        << type;
    EXPECT_EQ(0u, value) << type;
    EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(value)), length) << type;
  }

#ifdef SQL_ASYNC_DBC_FUNCTIONS
  for (const auto type : {
           static_cast<SQLUSMALLINT>(SQL_ASYNC_DBC_FUNCTIONS),
           static_cast<SQLUSMALLINT>(SQL_ASYNC_NOTIFICATION),
           static_cast<SQLUSMALLINT>(SQL_DRIVER_AWARE_POOLING_SUPPORTED)}) {
    SQLUINTEGER value = std::numeric_limits<SQLUINTEGER>::max();
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetInfo(connection_, type, &value, sizeof(value), nullptr))
        << type;
    EXPECT_EQ(0u, value) << type;
  }
#endif
}

TEST_F(GetInfoIntegrationTest, NumericOutputNeedNotBeAligned) {
  alignas(SQLUINTEGER)
      std::array<std::byte, 1 + sizeof(SQLUINTEGER)> storage{};
  void* output = storage.data() + 1;
  SQLSMALLINT length = -1;

  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_TXN_CAPABLE, output, sizeof(SQLUSMALLINT), &length));
  EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(SQLUSMALLINT)), length);
  SQLUSMALLINT small_value = 0;
  std::memcpy(&small_value, output, sizeof(small_value));
  EXPECT_EQ(SQL_TC_ALL, small_value);

  length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfoW(
      connection_, SQL_GETDATA_EXTENSIONS, output,
      sizeof(SQLUINTEGER), &length));
  EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(SQLUINTEGER)), length);
  SQLUINTEGER integer = 0;
  std::memcpy(&integer, output, sizeof(integer));
  EXPECT_EQ(static_cast<SQLUINTEGER>(
                SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BLOCK), integer);

  if (redshift_profile_) {
    const auto previous_default_output = storage;
    length = 73;
    ASSERT_EQ(SQL_ERROR, SQLGetInfoW(
        connection_, SQL_DEFAULT_TXN_ISOLATION, output,
        sizeof(SQLUINTEGER), &length));
    EXPECT_EQ("HY000", diagnostic_state(connection_));
    EXPECT_EQ(previous_default_output, storage);
    EXPECT_EQ(73, length);
  }

  const auto previous_output = storage;
  length = 17;
  EXPECT_EQ(SQL_ERROR, SQLGetInfo(
      connection_, 0xffff, output, sizeof(SQLUINTEGER), &length));
  EXPECT_EQ(previous_output, storage);
  EXPECT_EQ(17, length);
}

TEST_F(GetInfoIntegrationTest, NumericOutputLengthNeedNotBeAligned) {
  alignas(SQLSMALLINT)
      std::array<std::byte, 1 + sizeof(SQLSMALLINT)> length_storage{};
  auto* length = reinterpret_cast<SQLSMALLINT*>(length_storage.data() + 1);

  SQLUSMALLINT small_value = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_TXN_CAPABLE, &small_value,
      static_cast<SQLSMALLINT>(sizeof(small_value)), length));
  SQLSMALLINT required = -1;
  std::memcpy(&required, reinterpret_cast<const std::byte*>(length),
              sizeof(required));
  EXPECT_EQ(SQL_TC_ALL, small_value);
  EXPECT_EQ(sizeof(small_value), required);

  SQLUINTEGER integer = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfoW(
      connection_, SQL_GETDATA_EXTENSIONS, &integer,
      static_cast<SQLSMALLINT>(sizeof(integer)), length));
  std::memcpy(&required, reinterpret_cast<const std::byte*>(length),
              sizeof(required));
  EXPECT_EQ(static_cast<SQLUINTEGER>(
                SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER | SQL_GD_BLOCK), integer);
  EXPECT_EQ(sizeof(integer), required);

  if (redshift_profile_) {
    const auto previous_default_length = length_storage;
    integer = 0xaabbccdd;
    ASSERT_EQ(SQL_ERROR, SQLGetInfoW(
        connection_, SQL_DEFAULT_TXN_ISOLATION, &integer,
        static_cast<SQLSMALLINT>(sizeof(integer)), length));
    EXPECT_EQ("HY000", diagnostic_state(connection_));
    EXPECT_EQ(0xaabbccddu, integer);
    EXPECT_EQ(previous_default_length, length_storage);
  }

  const auto previous_length = length_storage;
  EXPECT_EQ(SQL_ERROR, SQLGetInfo(
      connection_, 0xffff, &integer,
      static_cast<SQLSMALLINT>(sizeof(integer)), length));
  EXPECT_EQ(previous_length, length_storage);
}

TEST_F(GetInfoIntegrationTest, ReportsErrorsAndTruncationPrecisely) {
  SQLUINTEGER value = 0;
  EXPECT_EQ(SQL_ERROR,
            SQLGetInfo(connection_, 0xffff, &value, sizeof(value), nullptr));
  EXPECT_EQ("HY096", diagnostic_state(connection_));
  EXPECT_EQ(SQL_ERROR,
            SQLGetInfoW(connection_, 0xffff, &value, sizeof(value), nullptr));
  EXPECT_EQ("HY096", diagnostic_state(connection_));

  SQLCHAR driver_name[5]{};
  SQLSMALLINT required = -1;
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLGetInfo(connection_, SQL_DRIVER_NAME, driver_name,
                       sizeof(driver_name), &required));
  EXPECT_EQ(0, std::memcmp(driver_name, ODBCPP_EXPECT_DRIVER_FILENAME, 4));
  EXPECT_EQ(0, driver_name[4]);
  // iODBC reports copied characters; the Windows manager preserves the
  // driver's required wide byte count when converting this truncated result.
  const auto ansi_required = odbcpp::test::dm::value(
      odbcpp::test::dm::Deviation::AnsiGetInfoLengthInWideBytes,
      sizeof(ODBCPP_EXPECT_DRIVER_FILENAME) - 1,
      (sizeof(ODBCPP_EXPECT_DRIVER_FILENAME) - 1) * sizeof(SQLWCHAR));
  EXPECT_EQ(odbcpp::test::dm::value(odbcpp::test::dm::Deviation::AnsiGetInfoStringLengths,
                                    ansi_required,
                                    sizeof(driver_name) - 1),
            static_cast<std::size_t>(required));
  EXPECT_EQ("01004", diagnostic_state(connection_));

  SQLWCHAR wide_driver_name[5]{};
  SQLSMALLINT wide_required = -1;
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLGetInfoW(connection_, SQL_DRIVER_NAME, wide_driver_name,
                        sizeof(wide_driver_name), &wide_required));
  EXPECT_EQ(static_cast<SQLWCHAR>(ODBCPP_EXPECT_DRIVER_FILENAME[0]), wide_driver_name[0]);
  EXPECT_EQ(static_cast<SQLWCHAR>(ODBCPP_EXPECT_DRIVER_FILENAME[3]), wide_driver_name[3]);
  EXPECT_EQ(static_cast<SQLWCHAR>(0), wide_driver_name[4]);
  EXPECT_EQ(static_cast<SQLSMALLINT>((sizeof(ODBCPP_EXPECT_DRIVER_FILENAME)-1) * sizeof(SQLWCHAR)), wide_required);
  EXPECT_EQ("01004", diagnostic_state(connection_));

  required = -1;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetInfo(connection_, SQL_DRIVER_NAME, nullptr, 0, &required));
  // iODBC 3.52.16 hands the ANSI caller the driver's wide byte count.
  EXPECT_EQ(odbcpp::test::dm::value(odbcpp::test::dm::Deviation::AnsiGetInfoStringLengths,
                                    ansi_required,
                                    (sizeof(ODBCPP_EXPECT_DRIVER_FILENAME) - 1) * sizeof(SQLWCHAR)),
            static_cast<std::size_t>(required));
  wide_required = -1;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetInfoW(connection_, SQL_DRIVER_NAME, nullptr, 0,
                        &wide_required));
  EXPECT_EQ(static_cast<SQLSMALLINT>((sizeof(ODBCPP_EXPECT_DRIVER_FILENAME)-1) * sizeof(SQLWCHAR)), wide_required);

  SQLSMALLINT numeric_length = 41;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetInfo(connection_, SQL_TXN_CAPABLE, nullptr, 0,
                       &numeric_length));
  EXPECT_EQ(static_cast<SQLSMALLINT>(sizeof(SQLUSMALLINT)), numeric_length);
  SQLINTEGER diagnostic_count = -1;
  EXPECT_EQ(SQL_SUCCESS, SQLGetDiagField(
      SQL_HANDLE_DBC, connection_, 0, SQL_DIAG_NUMBER,
      &diagnostic_count, 0, nullptr));
  EXPECT_EQ(0, diagnostic_count);

  SQLWCHAR misaligned[2]{};
  EXPECT_EQ(SQL_ERROR,
            SQLGetInfoW(connection_, SQL_DRIVER_NAME, misaligned,
                        static_cast<SQLSMALLINT>(sizeof(SQLWCHAR) + 1),
                        nullptr));
  EXPECT_EQ("HY090", diagnostic_state(connection_));
}

TEST_F(GetInfoIntegrationTest, AdvertisedPostgresqlSyntaxExecutes) {
  ASSERT_NO_FATAL_FAILURE(execute(
      "CREATE TEMP TABLE odbcpp_get_info_capabilities "
      "(id integer PRIMARY KEY, value integer)"));
  ASSERT_NO_FATAL_FAILURE(execute(
      "INSERT INTO odbcpp_get_info_capabilities VALUES (1, 10), (2, 20)"));
  SQLUINTEGER index_support = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_DDL_INDEX,
      &index_support, sizeof(index_support), nullptr));
  // Redshift has no indexes; retain the actual create/drop proof for PostgreSQL.
  // https://docs.aws.amazon.com/redshift/latest/dg/c_unsupported-postgresql-features.html
  ASSERT_EQ(redshift_profile_ ? 0u : static_cast<SQLUINTEGER>(SQL_DI_CREATE_INDEX | SQL_DI_DROP_INDEX),
      index_support);
  if (!redshift_profile_) {
    ASSERT_NO_FATAL_FAILURE(execute(
        "CREATE INDEX odbcpp_get_info_capabilities_value_idx "
        "ON odbcpp_get_info_capabilities (value)"));
    ASSERT_NO_FATAL_FAILURE(execute(
        "DROP INDEX odbcpp_get_info_capabilities_value_idx"));
  } else {
    SQLHSTMT statement = SQL_NULL_HSTMT;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement));
    struct StatementOwner {
      SQLHSTMT handle;
      ~StatementOwner() { SQLFreeHandle(SQL_HANDLE_STMT, handle); }
    } owner{statement};
    SQLCHAR unsupported[] = "CREATE INDEX odbcpp_get_info_capabilities_value_idx "
                            "ON odbcpp_get_info_capabilities (value)";
    ASSERT_EQ(SQL_ERROR, SQLExecDirect(statement, unsupported, SQL_NTS));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, odbcpp::test::diag_rec(SQL_HANDLE_STMT, statement, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("42000", reinterpret_cast<const char*>(state));
    ASSERT_NO_FATAL_FAILURE(verify_predicate("1=1"));
  }
  ASSERT_NO_FATAL_FAILURE(execute(
      "SELECT count(*) AS total FROM odbcpp_get_info_capabilities "
      "GROUP BY value ORDER BY value + 1"));
  ASSERT_NO_FATAL_FAILURE(execute(
      "SELECT left_side.id FROM odbcpp_get_info_capabilities left_side "
      "LEFT OUTER JOIN odbcpp_get_info_capabilities right_side "
      "ON left_side.id = right_side.id"));
  ASSERT_NO_FATAL_FAILURE(execute("SELECT 1 UNION ALL SELECT 2"));
  // Use a portable explicit escape character and fetch both the positive and
  // negative result. Preserve the existing PostgreSQL backslash-literal probe.
  ASSERT_NO_FATAL_FAILURE(verify_predicate(
      "'a_b' LIKE 'a!_b' ESCAPE '!' AND NOT ('axb' LIKE 'a!_b' ESCAPE '!')"));
  if (!redshift_profile_) {
    ASSERT_NO_FATAL_FAILURE(execute("SELECT 'a_b' LIKE 'a\\_b' ESCAPE '\\'"));
  }
  ASSERT_NO_FATAL_FAILURE(execute(
      "SELECT 1 AS id INTO TEMP TABLE odbcpp_get_info_select_into"));
}

TEST_F(GetInfoIntegrationTest, FunctionSupportMatchesDriverExports) {
  SQLUSMALLINT odbc3[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetFunctions(connection_, SQL_API_ODBC3_ALL_FUNCTIONS, odbc3));
  for (SQLUSMALLINT id = 0; id < 4000; ++id) {
    EXPECT_EQ(expected_support(id),
              SQL_FUNC_EXISTS(odbc3, id) != 0)
        << id;
  }

  SQLUSMALLINT odbc2[100]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetFunctions(connection_, SQL_API_ALL_FUNCTIONS, odbc2));
  for (SQLUSMALLINT id = 0; id < 100; ++id) {
    // This manager omits SQLBulkOperations and SQLAllocHandleStd from the
    // ODBC 2 bitmap, while its ODBC 3 bitmap reports both.
    const bool expected = (id == SQL_API_SQLBULKOPERATIONS || id == 73) &&
        odbcpp::test::dm::deviates(
            odbcpp::test::dm::Deviation::Odbc2FunctionBitmapOmitsBulkAndAllocHandleStd)
        ? false : expected_support(id);
    EXPECT_EQ(expected ? SQL_TRUE : SQL_FALSE, odbc2[id])
        << id;
  }

  for (const auto& function : odbcpp::test::advertised_functions) {
    const bool expected = expected_support(function.id);
    // Poison with the opposite value so the unsupported single-call result
    // must actually write SQL_FALSE, rather than inherit a zero initialization.
    SQLUSMALLINT supported = expected ? SQL_FALSE : SQL_TRUE;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetFunctions(connection_, function.id, &supported))
        << function.name;
    EXPECT_EQ(expected ? SQL_TRUE : SQL_FALSE, supported) << function.name;
  }

  SQLUSMALLINT supported = SQL_TRUE;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetFunctions(connection_, SQL_API_SQLBROWSECONNECT,
                            &supported));
  EXPECT_EQ(SQL_FALSE, supported);
  EXPECT_EQ(SQL_ERROR,
            SQLGetFunctions(connection_, static_cast<SQLUSMALLINT>(0xffff),
                            &supported));
  EXPECT_EQ(SQL_FALSE, supported);
  EXPECT_EQ("HY095", diagnostic_state(connection_));
  // The manager accepts a NULL output without calling the driver. iODBC
  // 3.52.16 leaves the previous call's diagnostic; unixODBC 2.3.14 clears it.
  const bool null_output_accepted = odbcpp::test::dm::deviates(
      odbcpp::test::dm::Deviation::GetFunctionsAcceptsNullOutput);
  EXPECT_EQ(null_output_accepted ? SQL_SUCCESS : SQL_ERROR,
            SQLGetFunctions(connection_, SQL_API_SQLCONNECT, nullptr));
  if (!null_output_accepted) {
    EXPECT_EQ("HY009", diagnostic_state(connection_));
  } else {
    const std::string left = odbcpp::test::dm::text(
        odbcpp::test::dm::Deviation::GetFunctionsAcceptsNullOutput, "");
    if (left.empty()) {
      SQLCHAR state[6]{};
      EXPECT_EQ(SQL_NO_DATA, odbcpp::test::diag_rec(SQL_HANDLE_DBC, connection_, 1, state,
                                                    nullptr, nullptr, 0, nullptr));
    } else {
      EXPECT_EQ(left, diagnostic_state(connection_));
    }
  }
}

TEST_F(GetInfoIntegrationTest, FunctionSupportHandlesUnalignedOutput) {
  alignas(SQLUSMALLINT)
      std::array<std::byte, 1 + sizeof(SQLUSMALLINT)> single_storage{};
  auto* single = reinterpret_cast<SQLUSMALLINT*>(single_storage.data() + 1);
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetFunctions(connection_, SQL_API_SQLCONNECT, single));
  SQLUSMALLINT supported = SQL_FALSE;
  std::memcpy(&supported, reinterpret_cast<const std::byte*>(single),
              sizeof(supported));
  EXPECT_EQ(SQL_TRUE, supported);

  alignas(SQLUSMALLINT) std::array<
      std::byte, 1 + sizeof(SQLUSMALLINT) * SQL_API_ODBC3_ALL_FUNCTIONS_SIZE>
      odbc3_storage{};
  auto* odbc3 = reinterpret_cast<SQLUSMALLINT*>(odbc3_storage.data() + 1);
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetFunctions(connection_, SQL_API_ODBC3_ALL_FUNCTIONS, odbc3));
  std::array<SQLUSMALLINT, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE> odbc3_words{};
  std::memcpy(odbc3_words.data(),
              reinterpret_cast<const std::byte*>(odbc3),
              sizeof(odbc3_words));
  EXPECT_NE(0, odbc3_words[SQL_API_SQLCONNECT >> 4] &
                   (1u << (SQL_API_SQLCONNECT & 0x000f)));

  alignas(SQLUSMALLINT)
      std::array<std::byte, 1 + 100 * sizeof(SQLUSMALLINT)> odbc2_storage{};
  auto* odbc2 = reinterpret_cast<SQLUSMALLINT*>(odbc2_storage.data() + 1);
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetFunctions(connection_, SQL_API_ALL_FUNCTIONS, odbc2));
  std::array<SQLUSMALLINT, 100> odbc2_values{};
  std::memcpy(odbc2_values.data(),
              reinterpret_cast<const std::byte*>(odbc2),
              sizeof(odbc2_values));
  EXPECT_EQ(SQL_TRUE, odbc2_values[SQL_API_SQLCONNECT]);
  EXPECT_EQ(SQL_FALSE, odbc2_values[SQL_API_SQLBROWSECONNECT]);

  const auto previous_single = single_storage;
  EXPECT_EQ(SQL_ERROR, SQLGetFunctions(
      connection_, static_cast<SQLUSMALLINT>(0xffff), single));
  EXPECT_EQ(previous_single, single_storage);
}


TEST_F(GetInfoIntegrationTest, IdentifierProfileMatchesQuotedBoundaryNames) {
  SQLUSMALLINT limit = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_MAX_IDENTIFIER_LEN,
      &limit, sizeof(limit), nullptr));
  // Product limits are independent literals, not learned from driver output.
  // https://docs.aws.amazon.com/redshift/latest/dg/r_names.html
  const SQLUSMALLINT expected_limit = redshift_profile_ ? 127 : 63;
  ASSERT_EQ(expected_limit, limit);
  const std::string name = "MiXeD_" + std::string(expected_limit - 6, 'x');
  const auto quote = string_info(SQL_IDENTIFIER_QUOTE_CHAR);
  ASSERT_EQ("\"", quote);
  execute("CREATE TEMP TABLE " + quote + name + quote + " (UnQuoted integer, \"MiXeD\" integer)");
  execute("INSERT INTO " + quote + name + quote + " (unquoted, \"MiXeD\") VALUES (1, 2)");
  ASSERT_NO_FATAL_FAILURE(verify_predicate("EXISTS (SELECT 1 FROM " + quote + name + quote +
      " WHERE unquoted = 1 AND \"MiXeD\" = 2)"));
  execute("DROP TABLE " + quote + name + quote);
}

TEST_F(GetInfoIntegrationTest, NullAndSetOperationClaimsMatchServerSemantics) {
  SQLUSMALLINT null_order = 0, concat = 0;
  SQLUINTEGER unions = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_NULL_COLLATION,
      &null_order, sizeof(null_order), nullptr));
  ASSERT_EQ(SQL_NC_HIGH, null_order);
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_CONCAT_NULL_BEHAVIOR,
      &concat, sizeof(concat), nullptr));
  ASSERT_EQ(SQL_CB_NULL, concat);
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_UNION,
      &unions, sizeof(unions), nullptr));
  ASSERT_EQ(static_cast<SQLUINTEGER>(SQL_U_UNION | SQL_U_UNION_ALL), unions);
  // Ordinary SELECT assertions work on both products; Redshift has neither
  // PostgreSQL anonymous DO blocks nor VALUES used as constant tables.
  // NULLs rank high on Redshift too:
  // https://docs.aws.amazon.com/redshift/latest/dg/r_ORDER_BY_clause.html
  const char* predicates[]{
      "('value' || CAST(NULL AS VARCHAR(10))) IS NULL",
      "(SELECT v FROM (SELECT CAST(NULL AS INTEGER) AS v UNION ALL SELECT 7) t ORDER BY v DESC LIMIT 1) IS NULL",
      "(SELECT v FROM (SELECT CAST(NULL AS INTEGER) AS v UNION ALL SELECT 7) t ORDER BY v ASC LIMIT 1)=7",
      "(SELECT count(*) FROM (SELECT CAST(NULL AS INTEGER) UNION SELECT CAST(NULL AS INTEGER)) t)=1",
      "(SELECT count(*) FROM (SELECT CAST(NULL AS INTEGER) UNION ALL SELECT CAST(NULL AS INTEGER)) t)=2",
  };
  for (const auto* predicate : predicates) {
    EXPECT_NO_FATAL_FAILURE(verify_predicate(predicate));
  }
}

}  // namespace

TEST_F(GetInfoIntegrationTest, ExactSqlCapabilityMasksForConnectedProduct) {
    const auto pg_only = [&](SQLUINTEGER value) { return redshift_profile_ ? 0u : value; };
    const SQLUINTEGER outer = SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL |
        SQL_OJ_NESTED | SQL_OJ_NOT_ORDERED | SQL_OJ_INNER;
    const SQLUINTEGER predicates = SQL_SP_EXISTS | SQL_SP_ISNOTNULL | SQL_SP_ISNULL |
        SQL_SP_LIKE | SQL_SP_IN | SQL_SP_BETWEEN | SQL_SP_COMPARISON;
    const std::pair<SQLUSMALLINT, SQLUINTEGER> expected[]{
        {SQL_OJ_CAPABILITIES, pg_only(outer)},
        {SQL_AGGREGATE_FUNCTIONS, pg_only(SQL_AF_AVG | SQL_AF_COUNT | SQL_AF_MAX |
            SQL_AF_MIN | SQL_AF_SUM | SQL_AF_DISTINCT | SQL_AF_ALL)},
        {SQL_STRING_FUNCTIONS, pg_only(SQL_FN_STR_UCASE | SQL_FN_STR_LCASE)},
        {SQL_NUMERIC_FUNCTIONS, 0},
        {SQL_TIMEDATE_FUNCTIONS, pg_only(SQL_FN_TD_CURDATE | SQL_FN_TD_CURTIME | SQL_FN_TD_NOW |
            SQL_FN_TD_TIMESTAMPADD | SQL_FN_TD_TIMESTAMPDIFF)},
        {SQL_SYSTEM_FUNCTIONS, pg_only(SQL_FN_SYS_IFNULL)},
        {SQL_DATETIME_LITERALS, pg_only(SQL_DL_SQL92_DATE | SQL_DL_SQL92_TIME | SQL_DL_SQL92_TIMESTAMP)},
        {SQL_SUBQUERIES, pg_only(SQL_SQ_COMPARISON | SQL_SQ_EXISTS | SQL_SQ_IN |
            (redshift_profile_ ? 0 : SQL_SQ_QUANTIFIED | SQL_SQ_CORRELATED_SUBQUERIES))},
        {SQL_SQL92_PREDICATES, pg_only(predicates |
            (redshift_profile_ ? 0 : SQL_SP_OVERLAPS | SQL_SP_QUANTIFIED_COMPARISON))},
        {SQL_SQL92_VALUE_EXPRESSIONS, pg_only(SQL_SVE_CASE | SQL_SVE_CAST | SQL_SVE_COALESCE | SQL_SVE_NULLIF)},
        {SQL_SQL92_ROW_VALUE_CONSTRUCTOR, pg_only(redshift_profile_ ? 0 :
            SQL_SRVC_VALUE_EXPRESSION | SQL_SRVC_NULL | SQL_SRVC_ROW_SUBQUERY)},
        {SQL_SQL92_STRING_FUNCTIONS, pg_only(SQL_SSF_LOWER | SQL_SSF_UPPER | SQL_SSF_SUBSTRING |
            SQL_SSF_TRIM_BOTH | SQL_SSF_TRIM_LEADING | SQL_SSF_TRIM_TRAILING)},
        {SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, pg_only(SQL_SNVF_CHAR_LENGTH | SQL_SNVF_CHARACTER_LENGTH |
            SQL_SNVF_EXTRACT | SQL_SNVF_OCTET_LENGTH | SQL_SNVF_POSITION |
            (redshift_profile_ ? 0 : SQL_SNVF_BIT_LENGTH))},
        {SQL_SQL92_DATETIME_FUNCTIONS, pg_only(SQL_SDF_CURRENT_DATE |
            (redshift_profile_ ? 0 : SQL_SDF_CURRENT_TIME | SQL_SDF_CURRENT_TIMESTAMP))},
        {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, pg_only(SQL_SRJO_CROSS_JOIN | SQL_SRJO_FULL_OUTER_JOIN |
            SQL_SRJO_INNER_JOIN | SQL_SRJO_LEFT_OUTER_JOIN | SQL_SRJO_NATURAL_JOIN |
            SQL_SRJO_RIGHT_OUTER_JOIN)},
        {SQL_BATCH_SUPPORT, 0},
        {SQL_BATCH_ROW_COUNT, 0},
    };

  RecordProperty("server_version", string_info(SQL_DBMS_VER));
  for (const auto& [type, expected_mask] : expected) {
    SCOPED_TRACE(type);
    SQLUINTEGER actual = 0xffffffff;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, type, &actual, sizeof(actual), nullptr));
    EXPECT_EQ(expected_mask, actual);
  }
  EXPECT_EQ(redshift_profile_ ? "N" : "Y", string_info(SQL_OUTER_JOINS));
  EXPECT_EQ("Y", string_info(SQL_MULT_RESULT_SETS));
}

TEST_F(GetInfoIntegrationTest, Sql92PredicateAndSubqueryClaimsExecute) {
  RecordProperty("server_version", string_info(SQL_DBMS_VER));
  struct Case { SQLUSMALLINT type; SQLUINTEGER bit; const char* predicate; };
  const Case common[]{
      {SQL_SUBQUERIES, SQL_SQ_COMPARISON, "1 = (SELECT 1)"},
      {SQL_SUBQUERIES, SQL_SQ_EXISTS, "EXISTS (SELECT 1)"},
      {SQL_SUBQUERIES, SQL_SQ_IN, "1 IN (SELECT 1)"},
      {SQL_SQL92_PREDICATES, SQL_SP_EXISTS, "EXISTS (SELECT 1)"},
      {SQL_SQL92_PREDICATES, SQL_SP_ISNOTNULL, "1 IS NOT NULL"},
      {SQL_SQL92_PREDICATES, SQL_SP_ISNULL, "CAST(NULL AS INTEGER) IS NULL"},
      {SQL_SQL92_PREDICATES, SQL_SP_LIKE, "'cat' LIKE 'c_t' AND 'cat' LIKE 'c%'"},
      {SQL_SQL92_PREDICATES, SQL_SP_IN, "1 IN (1,2) AND 3 NOT IN (1,2)"},
      {SQL_SQL92_PREDICATES, SQL_SP_BETWEEN, "2 BETWEEN 1 AND 3"},
      {SQL_SQL92_PREDICATES, SQL_SP_COMPARISON, "1=1 AND 1<>2 AND 1<2 AND 2>1 AND 1<=1 AND 2>=2"},
  };
  for (const auto& item : common) {
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(item.type, item.bit, item.predicate));
  }
  if (!redshift_profile_) {
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SUBQUERIES, SQL_SQ_QUANTIFIED,
        "2 > ALL (SELECT 1) AND 1 = ANY (SELECT 1) AND 1 = SOME (SELECT 1)"));
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SUBQUERIES, SQL_SQ_CORRELATED_SUBQUERIES,
        "EXISTS (SELECT 1 FROM (SELECT 1 AS k) a WHERE EXISTS (SELECT 1 WHERE a.k=1))"));
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SQL92_PREDICATES, SQL_SP_QUANTIFIED_COMPARISON,
        "2 > ALL (SELECT 1) AND 1 = ANY (SELECT 1) AND 1 = SOME (SELECT 1)"));
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SQL92_PREDICATES, SQL_SP_OVERLAPS,
        "(DATE '2024-01-01',DATE '2024-01-03') OVERLAPS (DATE '2024-01-02',DATE '2024-01-04')"));
  }
}

TEST_F(GetInfoIntegrationTest, Sql92ValueAndStringClaimsExecute) {
  RecordProperty("server_version", string_info(SQL_DBMS_VER));
  struct Case { SQLUSMALLINT type; SQLUINTEGER bit; const char* predicate; };
  const Case common[]{
      {SQL_SQL92_VALUE_EXPRESSIONS, SQL_SVE_CASE, "CASE WHEN 1=1 THEN 7 ELSE 8 END = 7"},
      {SQL_SQL92_VALUE_EXPRESSIONS, SQL_SVE_CAST, "CAST('7' AS INTEGER)=7"},
      {SQL_SQL92_VALUE_EXPRESSIONS, SQL_SVE_COALESCE, "COALESCE(CAST(NULL AS INTEGER),7)=7"},
      {SQL_SQL92_VALUE_EXPRESSIONS, SQL_SVE_NULLIF, "NULLIF(7,7) IS NULL AND NULLIF(7,8)=7"},
      {SQL_SQL92_STRING_FUNCTIONS, SQL_SSF_LOWER, "LOWER('Cat')='cat'"},
      {SQL_SQL92_STRING_FUNCTIONS, SQL_SSF_UPPER, "UPPER('Cat')='CAT'"},
      {SQL_SQL92_STRING_FUNCTIONS, SQL_SSF_SUBSTRING, "SUBSTRING('cat' FROM 2 FOR 2)='at'"},
      {SQL_SQL92_STRING_FUNCTIONS, SQL_SSF_TRIM_BOTH, "TRIM(BOTH 'x' FROM 'xcatx')='cat'"},
      {SQL_SQL92_STRING_FUNCTIONS, SQL_SSF_TRIM_LEADING, "TRIM(LEADING 'x' FROM 'xcat')='cat'"},
      {SQL_SQL92_STRING_FUNCTIONS, SQL_SSF_TRIM_TRAILING, "TRIM(TRAILING 'x' FROM 'catx')='cat'"},
  };
  for (const auto& item : common) {
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(item.type, item.bit, item.predicate));
  }
  if (!redshift_profile_) {
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SQL92_ROW_VALUE_CONSTRUCTOR,
        SQL_SRVC_VALUE_EXPRESSION, "(1,2)=(1,2)"));
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SQL92_ROW_VALUE_CONSTRUCTOR,
        SQL_SRVC_NULL, "ROW(NULL,NULL) IS NULL"));
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SQL92_ROW_VALUE_CONSTRUCTOR,
        SQL_SRVC_ROW_SUBQUERY, "(1,2)=(SELECT 1,2)"));
  }
}

TEST_F(GetInfoIntegrationTest, Sql92NumericDatetimeAndLiteralClaimsExecute) {
  RecordProperty("server_version", string_info(SQL_DBMS_VER));
  struct Case { SQLUSMALLINT type; SQLUINTEGER bit; const char* predicate; };
  const Case common[]{
      {SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, SQL_SNVF_CHAR_LENGTH, "CHAR_LENGTH('cat')=3"},
      {SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, SQL_SNVF_CHARACTER_LENGTH, "CHARACTER_LENGTH('cat')=3"},
      {SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, SQL_SNVF_EXTRACT, "EXTRACT(YEAR FROM DATE '2024-02-29')=2024"},
      {SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, SQL_SNVF_OCTET_LENGTH, "OCTET_LENGTH('cat')=3"},
      {SQL_SQL92_NUMERIC_VALUE_FUNCTIONS, SQL_SNVF_POSITION, "POSITION('at' IN 'cat')=2"},
      {SQL_SQL92_DATETIME_FUNCTIONS, SQL_SDF_CURRENT_DATE, "CURRENT_DATE IS NOT NULL"},
      {SQL_DATETIME_LITERALS, SQL_DL_SQL92_DATE, "{d '2024-02-29'} = DATE '2024-02-29'"},
      {SQL_DATETIME_LITERALS, SQL_DL_SQL92_TIME, "{t '12:34:56'} = TIME '12:34:56'"},
      {SQL_DATETIME_LITERALS, SQL_DL_SQL92_TIMESTAMP, "{ts '2024-02-29 12:34:56'} = TIMESTAMP '2024-02-29 12:34:56'"},
  };
  for (const auto& item : common) {
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(item.type, item.bit, item.predicate));
  }
  if (!redshift_profile_) {
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SQL92_NUMERIC_VALUE_FUNCTIONS,
        SQL_SNVF_BIT_LENGTH, "BIT_LENGTH('cat')=24"));
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SQL92_DATETIME_FUNCTIONS,
        SQL_SDF_CURRENT_TIME, "CURRENT_TIME IS NOT NULL"));
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_SQL92_DATETIME_FUNCTIONS,
        SQL_SDF_CURRENT_TIMESTAMP, "CURRENT_TIMESTAMP IS NOT NULL"));
  }
}

TEST_F(GetInfoIntegrationTest, Sql92JoinAndOuterEscapeClaimsExecute) {
  RecordProperty("server_version", string_info(SQL_DBMS_VER));
  struct Case { SQLUSMALLINT type; SQLUINTEGER bit; const char* predicate; };
  const Case cases[]{
      {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, SQL_SRJO_CROSS_JOIN,
          "(SELECT COUNT(*) FROM (SELECT 1 AS x) a CROSS JOIN (SELECT 2 AS y) b)=1"},
      {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, SQL_SRJO_INNER_JOIN,
          "(SELECT COUNT(*) FROM (SELECT 1 AS x) a INNER JOIN (SELECT 1 AS y) b ON a.x=b.y)=1"},
      {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, SQL_SRJO_NATURAL_JOIN,
          "(SELECT COUNT(*) FROM (SELECT 1 AS x) a NATURAL JOIN (SELECT 1 AS x) b)=1"},
      {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, SQL_SRJO_LEFT_OUTER_JOIN,
          "(SELECT COUNT(*) FROM (SELECT 1 AS x) a LEFT JOIN (SELECT 2 AS y) b ON a.x=b.y WHERE b.y IS NULL)=1"},
      {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, SQL_SRJO_RIGHT_OUTER_JOIN,
          "(SELECT COUNT(*) FROM (SELECT 1 AS x) a RIGHT JOIN (SELECT 2 AS y) b ON a.x=b.y WHERE a.x IS NULL)=1"},
      {SQL_SQL92_RELATIONAL_JOIN_OPERATORS, SQL_SRJO_FULL_OUTER_JOIN,
          "(SELECT COUNT(*) FROM (SELECT 1 AS x) a FULL JOIN (SELECT 2 AS y) b ON a.x=b.y)=2"},
      {SQL_OJ_CAPABILITIES, SQL_OJ_LEFT,
          "(SELECT COUNT(*) FROM {oj (SELECT 1 AS x) a LEFT OUTER JOIN (SELECT 2 AS y) b ON a.x=b.y})=1"},
      {SQL_OJ_CAPABILITIES, SQL_OJ_LEFT,
          "(SELECT COUNT(*) FROM {oj (SELECT 1 AS x) a LEFT JOIN (SELECT 2 AS y) b ON a.x=b.y})=1"},
      {SQL_OJ_CAPABILITIES, SQL_OJ_RIGHT,
          "(SELECT COUNT(*) FROM {oj (SELECT 1 AS x) a RIGHT OUTER JOIN (SELECT 2 AS y) b ON a.x=b.y})=1"},
      {SQL_OJ_CAPABILITIES, SQL_OJ_FULL,
          "(SELECT COUNT(*) FROM {oj (SELECT 1 AS x) a FULL OUTER JOIN (SELECT 2 AS y) b ON a.x=b.y})=2"},
      {SQL_OJ_CAPABILITIES, SQL_OJ_NOT_ORDERED,
          "(SELECT COUNT(*) FROM {oj (SELECT 1 AS x) a LEFT OUTER JOIN (SELECT 2 AS y) b ON b.y=a.x})=1"},
      {SQL_OJ_CAPABILITIES, SQL_OJ_NESTED,
          "(SELECT COUNT(*) FROM {oj (SELECT 1 AS x) a LEFT OUTER JOIN ((SELECT 2 AS y) b LEFT OUTER JOIN (SELECT 3 AS z) c ON b.y=c.z) ON a.x=b.y})=1"},
      {SQL_OJ_CAPABILITIES, SQL_OJ_INNER,
          "(SELECT COUNT(*) FROM {oj (SELECT 1 AS x) a LEFT OUTER JOIN ((SELECT 2 AS y) b INNER JOIN (SELECT 2 AS z) c ON b.y=c.z) ON a.x=b.y})=1"},
  };
  for (const auto& item : cases) {
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(item.type, item.bit, item.predicate));
  }
}

TEST_F(GetInfoIntegrationTest, AggregateClaimsExecuteWithDistinctAndAll) {
  RecordProperty("server_version", string_info(SQL_DBMS_VER));
  const std::pair<SQLUINTEGER, const char*> cases[]{
      {SQL_AF_AVG, "(SELECT AVG(x) FROM (SELECT 1 AS x UNION ALL SELECT 1 UNION ALL SELECT 4) v)=2"},
      {SQL_AF_COUNT, "(SELECT COUNT(x) FROM (SELECT 1 AS x UNION ALL SELECT NULL) v)=1"},
      {SQL_AF_MAX, "(SELECT MAX(x) FROM (SELECT 1 AS x UNION ALL SELECT 4) v)=4"},
      {SQL_AF_MIN, "(SELECT MIN(x) FROM (SELECT 1 AS x UNION ALL SELECT 4) v)=1"},
      {SQL_AF_SUM, "(SELECT SUM(x) FROM (SELECT 1 AS x UNION ALL SELECT 4) v)=5"},
      {SQL_AF_DISTINCT, "(SELECT COUNT(DISTINCT x) FROM (SELECT 1 AS x UNION ALL SELECT 1 UNION ALL SELECT 4) v)=2"},
      {SQL_AF_ALL, "(SELECT SUM(ALL x) FROM (SELECT 1 AS x UNION ALL SELECT 1 UNION ALL SELECT 4) v)=6"},
  };
  for (const auto& [bit, predicate] : cases) {
    ASSERT_NO_FATAL_FAILURE(verify_sql_feature(SQL_AGGREGATE_FUNCTIONS, bit, predicate));
  }
}

TEST_F(GetInfoIntegrationTest, DirectSequencesReturnEachResultWithoutClaimingPreparedBatches) {
  RecordProperty("server_version", string_info(SQL_DBMS_VER));
  SQLUINTEGER support = 0, counts = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_BATCH_SUPPORT, &support, sizeof(support), nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, SQL_BATCH_ROW_COUNT, &counts, sizeof(counts), nullptr));
  ASSERT_EQ(0u, support);
  ASSERT_EQ(0u, counts);
  EXPECT_EQ("Y", string_info(SQL_MULT_RESULT_SETS));
  SQLHSTMT statement = SQL_NULL_HSTMT;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement));
  struct StatementOwner {
    SQLHSTMT handle;
    ~StatementOwner() { SQLFreeHandle(SQL_HANDLE_STMT, handle); }
  } owner{statement};
  // This proves only the simple-query path. It must not authorize explicit
  // batch masks: prepared/parameterized batches are still unsupported.
  // Temporary objects are private to this connection and disappear at fixture disconnect.
  std::string create = "CREATE TEMP TABLE odbcpp_capability_batch (value INTEGER)";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement, reinterpret_cast<SQLCHAR*>(create.data()), SQL_NTS));
  std::string batch = "SELECT 7; INSERT INTO odbcpp_capability_batch VALUES (1); "
      "UPDATE odbcpp_capability_batch SET value=2; SELECT value FROM odbcpp_capability_batch";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement, reinterpret_cast<SQLCHAR*>(batch.data()), SQL_NTS));
  for (int index = 0; index != 4; ++index) {
    SCOPED_TRACE(index);
    SQLSMALLINT columns = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(statement, &columns));
    if (index == 0 || index == 3) {
      EXPECT_EQ(1, columns);
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(statement));
      SQLINTEGER value = -1;
      SQLLEN length = SQL_NULL_DATA;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(statement, 1, SQL_C_SLONG, &value, sizeof(value), &length));
      EXPECT_EQ(index == 0 ? 7 : 2, value);
      EXPECT_NE(SQL_NULL_DATA, length);
      EXPECT_EQ(SQL_NO_DATA, SQLFetch(statement));
    } else {
      EXPECT_EQ(0, columns);
      SQLLEN affected = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLRowCount(statement, &affected));
      EXPECT_EQ(1, affected);
    }
    ASSERT_EQ(index == 3 ? SQL_NO_DATA : SQL_SUCCESS, SQLMoreResults(statement));
  }
  std::string drop = "DROP TABLE odbcpp_capability_batch";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(statement, reinterpret_cast<SQLCHAR*>(drop.data()), SQL_NTS));
}


TEST_F(GetInfoIntegrationTest, ScalarEscapesExecuteOnTableValuesDirectAndPrepared) {
  RecordProperty("server_version", string_info(SQL_DBMS_VER));
  // A real temporary table exercises compute-node expressions, rather than
  // accepting a leader-only constant SELECT as qualification. Disconnect also
  // removes this session-owned table if a fatal assertion interrupts the case.
  ASSERT_NO_FATAL_FAILURE(execute("CREATE TEMP TABLE odbcpp_scalar_capabilities "
      "(text_value VARCHAR(32), padded_value VARCHAR(32), number_value DOUBLE PRECISION, "
      "date_value DATE, timestamp_value TIMESTAMP)"));
  ASSERT_NO_FATAL_FAILURE(execute("INSERT INTO odbcpp_scalar_capabilities VALUES "
      "('MiXeD', 'cat   ', 100, DATE '2024-02-29', TIMESTAMP '2024-02-29 23:59:59.875')"));
  struct Case {
    SQLUSMALLINT type;
    SQLUINTEGER bit;
    const char* predicate;
    const char* typed_expression = "";
    SQLSMALLINT expected_type = 0;
  };
  // Clock expectations use another server clock API, not the escape rewrite.
  // SYSDATE is transaction-start time; clock_timestamp() is wall time. This
  // autocommit fixture permits at most its existing 40-second statement window
  // of skew. Date and time comparisons tolerate crossing midnight, and the
  // uncast result column must independently report the expected ODBC type.
  // AWS: redshift/latest/dg/r_SYSDATE.html; PostgreSQL: functions-datetime.html.
  const Case common[]{
      {SQL_STRING_FUNCTIONS, SQL_FN_STR_UCASE,
          "{fn UCASE(text_value)}='MIXED' AND {fn UCASE(CAST(NULL AS VARCHAR(32)))} IS NULL"},
      {SQL_STRING_FUNCTIONS, SQL_FN_STR_LCASE,
          "{fn LCASE(text_value)}='mixed' AND {fn LCASE(CAST(NULL AS VARCHAR(32)))} IS NULL"},
      {SQL_SYSTEM_FUNCTIONS, SQL_FN_SYS_IFNULL,
          "{fn IFNULL(CAST(NULL AS INTEGER),7)}=7 AND {fn IFNULL(9,7)}=9"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_CURDATE,
          redshift_profile_
              ? "{fn CURDATE()} BETWEEN CAST(DATEADD(second,-40,SYSDATE) AS DATE) "
                "AND CAST(DATEADD(second,40,SYSDATE) AS DATE)"
              : "{fn CURDATE()} BETWEEN CAST(clock_timestamp()-INTERVAL '40 seconds' AS DATE) "
                "AND CAST(clock_timestamp()+INTERVAL '40 seconds' AS DATE)",
          "{fn CURDATE()}", SQL_TYPE_DATE},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_CURTIME,
          redshift_profile_
              ? "LEAST(ABS(DATEDIFF(second,{fn CURTIME()},CAST(SYSDATE AS TIME))),"
                "86400-ABS(DATEDIFF(second,{fn CURTIME()},CAST(SYSDATE AS TIME))))<=40"
              : "LEAST(ABS(EXTRACT(EPOCH FROM (CAST({fn CURTIME()} AS TIME)-CAST(clock_timestamp() AS TIME)))),"
                "86400-ABS(EXTRACT(EPOCH FROM (CAST({fn CURTIME()} AS TIME)-CAST(clock_timestamp() AS TIME)))))<=40",
          "{fn CURTIME()}", SQL_TYPE_TIME},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_NOW,
          redshift_profile_
              ? "ABS(DATEDIFF(second,{fn NOW()},SYSDATE))<=40"
              : "ABS(EXTRACT(EPOCH FROM ({fn NOW()}-clock_timestamp())))<=40",
          "{fn NOW()}", SQL_TYPE_TIMESTAMP},
  };
  const Case redshift_extensions[]{
      {SQL_STRING_FUNCTIONS, SQL_FN_STR_LENGTH,
          "{fn LENGTH(padded_value)}=3 AND {fn LENGTH(CAST(NULL AS VARCHAR(32)))} IS NULL"},
      {SQL_STRING_FUNCTIONS, SQL_FN_STR_LOCATE_2,
          "{fn LOCATE('Xe',text_value)}=3 AND {fn LOCATE('z',text_value)}=0 "
          "AND {fn LOCATE(CAST(NULL AS VARCHAR(32)),text_value)} IS NULL"},
      {SQL_NUMERIC_FUNCTIONS, SQL_FN_NUM_LOG,
          "ABS({fn LOG(number_value)}-4.605170185988092)<0.000000000001 "
          "AND {fn LOG(CAST(NULL AS DOUBLE PRECISION))} IS NULL"},
      {SQL_NUMERIC_FUNCTIONS, SQL_FN_NUM_LOG10,
          "ABS({fn LOG10(number_value)}-2)<0.000000000001 "
          "AND {fn LOG10(CAST(NULL AS DOUBLE PRECISION))} IS NULL"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_YEAR, "{fn YEAR(date_value)}=2024 AND {fn YEAR(CAST(NULL AS DATE))} IS NULL"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_MONTH, "{fn MONTH(date_value)}=2 AND {fn MONTH(CAST(NULL AS DATE))} IS NULL"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_DAYOFMONTH, "{fn DAYOFMONTH(date_value)}=29 AND {fn DAYOFMONTH(CAST(NULL AS DATE))} IS NULL"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_DAYOFWEEK, "{fn DAYOFWEEK(date_value)}=5 AND {fn DAYOFWEEK(CAST(NULL AS DATE))} IS NULL"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_DAYOFYEAR, "{fn DAYOFYEAR(date_value)}=60 AND {fn DAYOFYEAR(CAST(NULL AS DATE))} IS NULL"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_HOUR, "{fn HOUR(timestamp_value)}=23 AND {fn HOUR(CAST(NULL AS TIMESTAMP))} IS NULL"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_MINUTE, "{fn MINUTE(timestamp_value)}=59 AND {fn MINUTE(CAST(NULL AS TIMESTAMP))} IS NULL"},
      {SQL_TIMEDATE_FUNCTIONS, SQL_FN_TD_SECOND, "{fn SECOND(timestamp_value)}=59 AND {fn SECOND(CAST(NULL AS TIMESTAMP))} IS NULL"},
  };
  const auto verify = [&](const Case& item) {
    SCOPED_TRACE(item.bit);
    SQLUINTEGER mask = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(connection_, item.type, &mask, sizeof(mask), nullptr));
    EXPECT_EQ(redshift_profile_ ? 0u : item.bit, mask & item.bit);
    for (const bool prepared : {false, true}) {
      EXPECT_NO_FATAL_FAILURE(verify_predicate(item.predicate, prepared, "odbcpp_scalar_capabilities",
          item.typed_expression, item.expected_type));
    }
  };
  for (const auto& item : common) {
    EXPECT_NO_FATAL_FAILURE(verify(item));
  }
  if (redshift_profile_) {
    for (const auto& item : redshift_extensions) {
      EXPECT_NO_FATAL_FAILURE(verify(item));
    }
  }
  ASSERT_NO_FATAL_FAILURE(execute("DROP TABLE odbcpp_scalar_capabilities"));
}

#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
TEST_F(GetInfoIntegrationTest, ExecuteAssertionFailureReleasesItsStatement) {
  ::testing::TestPartResultArray failures;
  {
    ::testing::ScopedFakeTestPartResultReporter capture(
        ::testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD,
        &failures);
    execute("SELECT FROM");
  }
  ASSERT_EQ(1, failures.size());
  ASSERT_TRUE(failures.GetTestPartResult(0).fatally_failed());
  // Successful disconnect also unregisters children, so inspect ownership
  // before disconnect. On a timed-out connection SQLDisconnect instead returns
  // 08003 and cannot hide a helper leak. This is a direct-carrier fixture check,
  // not a claim about Driver Manager-owned handles.
  EXPECT_FALSE(rs::odbc::HandleRegistry::instance().has_children(connection_));
}
#endif
