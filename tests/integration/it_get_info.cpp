#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "tests/test_connection_config.h"

#include <array>
#include <cstring>
#include <limits>
#include <string>
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

  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
};

TEST_F(GetInfoIntegrationTest, ReportsStaticStringCapabilities) {
  EXPECT_EQ("ODBCPP Driver", string_info(SQL_DRIVER_NAME));
  EXPECT_EQ("01.00.0000", string_info(SQL_DRIVER_VER));
  EXPECT_EQ("03.80", string_info(SQL_DRIVER_ODBC_VER));
  EXPECT_EQ("PostgreSQL", string_info(SQL_DBMS_NAME));
  EXPECT_EQ("\"", string_info(SQL_IDENTIFIER_QUOTE_CHAR));
  EXPECT_EQ("database", string_info(SQL_CATALOG_TERM));
  EXPECT_EQ("schema", string_info(SQL_SCHEMA_TERM));
  EXPECT_EQ("Y", string_info(SQL_CATALOG_NAME));
  EXPECT_EQ("N", string_info(SQL_ACCESSIBLE_TABLES));
  EXPECT_EQ("N", string_info(SQL_ACCESSIBLE_PROCEDURES));
  EXPECT_EQ("Y", string_info(SQL_DESCRIBE_PARAMETER));
  EXPECT_EQ("Y", string_info(SQL_PROCEDURES));
  EXPECT_EQ("N", string_info(SQL_ROW_UPDATES));

  SQLWCHAR driver_name[32]{};
  SQLSMALLINT name_bytes = -1;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetInfoW(connection_, SQL_DRIVER_NAME, driver_name,
                        sizeof(driver_name), &name_bytes));
  EXPECT_EQ(static_cast<SQLSMALLINT>(13 * sizeof(SQLWCHAR)), name_bytes);
  EXPECT_EQ(static_cast<SQLWCHAR>('O'), driver_name[0]);
}

TEST_F(GetInfoIntegrationTest, ReportsNumericCapabilities) {
  constexpr std::array<std::pair<SQLUSMALLINT, SQLUSMALLINT>, 13>
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

  constexpr std::array<std::pair<SQLUSMALLINT, SQLUINTEGER>, 27>
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
}

}  // namespace
