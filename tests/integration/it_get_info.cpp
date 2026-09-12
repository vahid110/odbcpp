#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "tests/test_connection_config.h"

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

std::string diagnostic_state(SQLHDBC connection) {
  SQLCHAR state[6]{};
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetDiagRec(SQL_HANDLE_DBC, connection, 1, state, nullptr,
                          nullptr, 0, nullptr));
  return reinterpret_cast<const char*>(state);
}

class GetInfoIntegrationTest : public ::testing::Test {
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
  }

  void TearDown() override {
    if (connection_) {
      SQLDisconnect(connection_);
      SQLFreeHandle(SQL_HANDLE_DBC, connection_);
    }
    if (environment_) SQLFreeHandle(SQL_HANDLE_ENV, environment_);
  }

  std::string string_info(SQLUSMALLINT type) {
    SQLCHAR value[128]{};
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
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(statement,
                            reinterpret_cast<SQLCHAR*>(
                                const_cast<char*>(sql.data())),
                            static_cast<SQLINTEGER>(sql.size())))
        << sql;
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));
  }

  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
};

TEST_F(GetInfoIntegrationTest, ReportsStaticStringCapabilities) {
  constexpr std::array<std::pair<SQLUSMALLINT, std::string_view>, 33>
      expected_values{{
          {SQL_DRIVER_NAME, "ODBCPP Driver"},
          {SQL_DRIVER_VER, "01.00.0000"},
          {SQL_DRIVER_ODBC_VER, "03.80"},
          {SQL_ODBC_VER, "03.80"},
          {SQL_DBMS_NAME, "PostgreSQL"},
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
          {SQL_OUTER_JOINS, "Y"},
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
          {SQL_KEYWORDS, ""},
          {SQL_SPECIAL_CHARACTERS, ""},
          {SQL_XOPEN_CLI_YEAR, "1995"},
      }};
  for (const auto& [type, expected] : expected_values) {
    EXPECT_EQ(expected, string_info(type)) << type;

    SQLWCHAR wide_value[128]{};
    SQLSMALLINT wide_bytes = -1;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetInfoW(connection_, type, wide_value, sizeof(wide_value),
                          &wide_bytes))
        << type;
    EXPECT_EQ(static_cast<SQLSMALLINT>(expected.size() * sizeof(SQLWCHAR)),
              wide_bytes)
        << type;
    for (std::size_t index = 0; index < expected.size(); ++index) {
      EXPECT_EQ(static_cast<SQLWCHAR>(expected[index]), wide_value[index])
          << type << " at " << index;
    }
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide_value[expected.size()]) << type;
  }
}

TEST_F(GetInfoIntegrationTest, ReportsNumericCapabilities) {
  constexpr std::array<std::pair<SQLUSMALLINT, SQLUSMALLINT>, 25>
      small_values{{
          {SQL_ACTIVE_ENVIRONMENTS, 0},
          {SQL_MAX_CONCURRENT_ACTIVITIES, 0},
          {SQL_MAX_DRIVER_CONNECTIONS, 0},
          {SQL_FILE_USAGE, SQL_FILE_NOT_SUPPORTED},
          {SQL_TXN_CAPABLE, SQL_TC_ALL},
          {SQL_CURSOR_COMMIT_BEHAVIOR, SQL_CB_PRESERVE},
          {SQL_CURSOR_ROLLBACK_BEHAVIOR, SQL_CB_PRESERVE},
          {SQL_MAX_IDENTIFIER_LEN, 63},
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
          {SQL_ODBC_SQL_CONFORMANCE, SQL_OSC_CORE},
          {SQL_MAX_COLUMN_NAME_LEN, 63},
          {SQL_MAX_TABLE_NAME_LEN, 63},
          {SQL_MAX_SCHEMA_NAME_LEN, 63},
          {SQL_MAX_CATALOG_NAME_LEN, 63},
          {SQL_MAX_PROCEDURE_NAME_LEN, 63},
          {SQL_MAX_USER_NAME_LEN, 63},
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
      SQL_MAX_COLUMNS_IN_ORDER_BY, SQL_MAX_COLUMNS_IN_SELECT,
      SQL_MAX_COLUMNS_IN_TABLE, SQL_MAX_CURSOR_NAME_LEN,
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

  constexpr std::array<std::pair<SQLUSMALLINT, SQLUINTEGER>, 36>
      integer_values{{
          {SQL_DEFAULT_TXN_ISOLATION, SQL_TXN_READ_COMMITTED},
          {SQL_TXN_ISOLATION_OPTION,
           SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED |
               SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE},
          {SQL_SCROLL_OPTIONS, SQL_SO_FORWARD_ONLY},
          {SQL_GETDATA_EXTENSIONS, SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER},
          {SQL_ASYNC_MODE, SQL_AM_NONE},
          {SQL_BATCH_ROW_COUNT, 0},
          {SQL_BATCH_SUPPORT, 0},
          {SQL_BOOKMARK_PERSISTENCE, 0},
          {SQL_CONVERT_FUNCTIONS, 0},
          {SQL_CURSOR_SENSITIVITY, SQL_UNSPECIFIED},
          {SQL_DYNAMIC_CURSOR_ATTRIBUTES1, 0},
          {SQL_DYNAMIC_CURSOR_ATTRIBUTES2, 0},
          {SQL_KEYSET_CURSOR_ATTRIBUTES1, 0},
          {SQL_KEYSET_CURSOR_ATTRIBUTES2, 0},
          {SQL_MAX_ASYNC_CONCURRENT_STATEMENTS, 0},
          {SQL_POS_OPERATIONS, 0},
          {SQL_POSITIONED_STATEMENTS, 0},
          {SQL_STATIC_CURSOR_ATTRIBUTES1, 0},
          {SQL_STATIC_CURSOR_ATTRIBUTES2, 0},
          {SQL_STATIC_SENSITIVITY, 0},
          {SQL_FETCH_DIRECTION, SQL_FD_FETCH_NEXT},
          {SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1, SQL_CA1_NEXT},
          {SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2,
           SQL_CA2_READ_ONLY_CONCURRENCY},
          {SQL_LOCK_TYPES, SQL_LCK_NO_CHANGE},
          {SQL_PARAM_ARRAY_ROW_COUNTS, SQL_PARC_NO_BATCH},
          {SQL_PARAM_ARRAY_SELECTS, SQL_PAS_NO_SELECT},
          {SQL_SCROLL_CONCURRENCY, SQL_SCCO_READ_ONLY},
          {SQL_DDL_INDEX, SQL_DI_CREATE_INDEX | SQL_DI_DROP_INDEX},
          {SQL_INSERT_STATEMENT,
           SQL_IS_INSERT_LITERALS | SQL_IS_INSERT_SEARCHED |
               SQL_IS_SELECT_INTO},
          {SQL_ODBC_INTERFACE_CONFORMANCE, SQL_OIC_CORE},
          {SQL_SQL_CONFORMANCE, SQL_SC_SQL92_ENTRY},
          {SQL_STANDARD_CLI_CONFORMANCE, SQL_SCC_XOPEN_CLI_VERSION1},
          {SQL_UNION, SQL_U_UNION | SQL_U_UNION_ALL},
          {SQL_DTC_TRANSITION_COST, 0},
          {SQL_CATALOG_USAGE, 0},
          {SQL_SCHEMA_USAGE,
           SQL_SU_DML_STATEMENTS | SQL_SU_PROCEDURE_INVOCATION |
               SQL_SU_TABLE_DEFINITION | SQL_SU_INDEX_DEFINITION |
               SQL_SU_PRIVILEGE_DEFINITION},
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
      SQL_AGGREGATE_FUNCTIONS,
      SQL_ALTER_DOMAIN,
      SQL_ALTER_TABLE,
      SQL_DATETIME_LITERALS,
      SQL_CREATE_ASSERTION,
      SQL_CREATE_CHARACTER_SET,
      SQL_CREATE_COLLATION,
      SQL_CREATE_DOMAIN,
      SQL_CREATE_SCHEMA,
      SQL_CREATE_TABLE,
      SQL_CREATE_TRANSLATION,
      SQL_CREATE_VIEW,
      SQL_CONVERT_BIGINT,
      SQL_CONVERT_BINARY,
      SQL_CONVERT_BIT,
      SQL_CONVERT_CHAR,
      SQL_CONVERT_DATE,
      SQL_CONVERT_DECIMAL,
      SQL_CONVERT_DOUBLE,
      SQL_CONVERT_FLOAT,
      SQL_CONVERT_INTEGER,
      SQL_CONVERT_INTERVAL_DAY_TIME,
      SQL_CONVERT_INTERVAL_YEAR_MONTH,
      SQL_CONVERT_LONGVARBINARY,
      SQL_CONVERT_LONGVARCHAR,
      SQL_CONVERT_NUMERIC,
      SQL_CONVERT_REAL,
      SQL_CONVERT_SMALLINT,
      SQL_CONVERT_TIME,
      SQL_CONVERT_TIMESTAMP,
      SQL_CONVERT_TINYINT,
      SQL_CONVERT_VARBINARY,
      SQL_CONVERT_VARCHAR,
      SQL_CONVERT_WCHAR,
      SQL_CONVERT_WLONGVARCHAR,
      SQL_CONVERT_WVARCHAR,
#ifdef SQL_CONVERT_GUID
      SQL_CONVERT_GUID,
#endif
      SQL_DROP_ASSERTION,
      SQL_DROP_CHARACTER_SET,
      SQL_DROP_COLLATION,
      SQL_DROP_DOMAIN,
      SQL_DROP_SCHEMA,
      SQL_DROP_TABLE,
      SQL_DROP_TRANSLATION,
      SQL_DROP_VIEW,
      SQL_INDEX_KEYWORDS,
      SQL_INFO_SCHEMA_VIEWS,
      SQL_MAX_BINARY_LITERAL_LEN,
      SQL_MAX_CHAR_LITERAL_LEN,
      SQL_MAX_INDEX_SIZE,
      SQL_MAX_ROW_SIZE,
      SQL_MAX_STATEMENT_LEN,
      SQL_NUMERIC_FUNCTIONS,
      SQL_OJ_CAPABILITIES,
      SQL_SQL92_DATETIME_FUNCTIONS,
      SQL_SQL92_FOREIGN_KEY_DELETE_RULE,
      SQL_SQL92_FOREIGN_KEY_UPDATE_RULE,
      SQL_SQL92_GRANT,
      SQL_SQL92_NUMERIC_VALUE_FUNCTIONS,
      SQL_SQL92_PREDICATES,
      SQL_SQL92_RELATIONAL_JOIN_OPERATORS,
      SQL_SQL92_REVOKE,
      SQL_SQL92_ROW_VALUE_CONSTRUCTOR,
      SQL_SQL92_STRING_FUNCTIONS,
      SQL_SQL92_VALUE_EXPRESSIONS,
      SQL_STRING_FUNCTIONS,
      SQL_SUBQUERIES,
      SQL_SYSTEM_FUNCTIONS,
      SQL_TIMEDATE_ADD_INTERVALS,
      SQL_TIMEDATE_DIFF_INTERVALS,
      SQL_TIMEDATE_FUNCTIONS,
  };
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
  EXPECT_STREQ("ODBC", reinterpret_cast<const char*>(driver_name));
  EXPECT_EQ(13, required);
  EXPECT_EQ("01004", diagnostic_state(connection_));

  SQLWCHAR wide_driver_name[5]{};
  SQLSMALLINT wide_required = -1;
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLGetInfoW(connection_, SQL_DRIVER_NAME, wide_driver_name,
                        sizeof(wide_driver_name), &wide_required));
  EXPECT_EQ(static_cast<SQLWCHAR>('O'), wide_driver_name[0]);
  EXPECT_EQ(static_cast<SQLWCHAR>('C'), wide_driver_name[3]);
  EXPECT_EQ(static_cast<SQLWCHAR>(0), wide_driver_name[4]);
  EXPECT_EQ(static_cast<SQLSMALLINT>(13 * sizeof(SQLWCHAR)), wide_required);
  EXPECT_EQ("01004", diagnostic_state(connection_));

  required = -1;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetInfo(connection_, SQL_DRIVER_NAME, nullptr, 0, &required));
  EXPECT_EQ(13, required);
  wide_required = -1;
  EXPECT_EQ(SQL_SUCCESS,
            SQLGetInfoW(connection_, SQL_DRIVER_NAME, nullptr, 0,
                        &wide_required));
  EXPECT_EQ(static_cast<SQLSMALLINT>(13 * sizeof(SQLWCHAR)), wide_required);

  SQLSMALLINT untouched_length = 41;
  EXPECT_EQ(SQL_ERROR,
            SQLGetInfo(connection_, SQL_TXN_CAPABLE, nullptr, 0,
                       &untouched_length));
  EXPECT_EQ(41, untouched_length);
  EXPECT_EQ("HY009", diagnostic_state(connection_));

  SQLWCHAR misaligned[2]{};
  EXPECT_EQ(SQL_ERROR,
            SQLGetInfoW(connection_, SQL_DRIVER_NAME, misaligned,
                        static_cast<SQLSMALLINT>(sizeof(SQLWCHAR) + 1),
                        nullptr));
  EXPECT_EQ("HY090", diagnostic_state(connection_));
}

TEST_F(GetInfoIntegrationTest, AdvertisedPostgresqlSyntaxExecutes) {
  execute("CREATE TEMP TABLE odbcpp_get_info_capabilities "
          "(id integer PRIMARY KEY, value integer)");
  execute("INSERT INTO odbcpp_get_info_capabilities VALUES (1, 10), (2, 20)");
  execute("CREATE INDEX odbcpp_get_info_capabilities_value_idx "
          "ON odbcpp_get_info_capabilities (value)");
  execute("DROP INDEX odbcpp_get_info_capabilities_value_idx");
  execute("SELECT count(*) AS total FROM odbcpp_get_info_capabilities "
          "GROUP BY value ORDER BY value + 1");
  execute("SELECT left_side.id FROM odbcpp_get_info_capabilities left_side "
          "LEFT OUTER JOIN odbcpp_get_info_capabilities right_side "
          "ON left_side.id = right_side.id");
  execute("SELECT 1 UNION ALL SELECT 2");
  execute("SELECT 'a_b' LIKE 'a\\_b' ESCAPE '\\'");
  execute("SELECT 1 AS id INTO TEMP TABLE odbcpp_get_info_select_into");
}

}  // namespace
