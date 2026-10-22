// Test-only exported ODBC assertions through the owning driver manager. No DDL.
// Fixture creation/custody and finite resource admission remain Python-owned.
#include "../test_live_package_inputs.h"
#include <gtest/gtest.h>
#include <sql.h>
#include <sqlext.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace inputs = odbcpp::test::live_package_inputs;
constexpr char schema[] = "odbcpp_fixture";
constexpr char parent[] = "pbi_parent_single_20261022_q007";
constexpr char fact[] = "pbi_fact_single_20261022_q007";
using Rows = std::vector<std::vector<std::string>>;
SQLCHAR* bytes(const char* value) {
  return reinterpret_cast<SQLCHAR*>(const_cast<char*>(value));
}
std::string pattern(std::string_view value) {
  std::string result;
  for (const char ch : value) {
    if (ch == '_' || ch == '%' || ch == '\\') {
      result.push_back('\\');
    }
    result.push_back(ch);
  }
  return result;
}

class PowerBiRelationshipNative : public testing::Test {
 protected:
  using Clock = std::chrono::steady_clock;
  Clock::time_point end_{};
  SQLHENV env_ = SQL_NULL_HENV;
  SQLHDBC dbc_ = SQL_NULL_HDBC;
  SQLHSTMT stmt_ = SQL_NULL_HSTMT;
  bool admitted_ = false, connected_ = false;
  std::string parent_oid_, fact_oid_;

  static std::string setting(const char* name) {
    return inputs::fixture_environment(name).value_or(std::string{});
  }
  static bool oid(const std::string& value) {
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return !value.empty() && value.front() != '0' && parsed.ec == std::errc{} &&
        parsed.ptr == value.data() + value.size() && result != 0;
  }
  std::string state(SQLSMALLINT kind, SQLHANDLE handle) {
    std::array<SQLCHAR, 6> code{};
    const auto rc = SQLGetDiagRecA(kind, handle, 1, code.data(), nullptr, nullptr, 0, nullptr);
    return SQL_SUCCEEDED(rc) ? std::string(reinterpret_cast<char*>(code.data()), 5) : "unavailable";
  }
  bool cap() {
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(end_ - Clock::now()).count();
    if (left <= 0) {
      ADD_FAILURE() << "Original relationship case window expired";
      return false;
    }
    const auto seconds = static_cast<std::uintptr_t>(std::min<std::int64_t>(left, 5));
    if (SQLSetConnectAttr(dbc_, SQL_ATTR_CONNECTION_TIMEOUT, reinterpret_cast<SQLPOINTER>(seconds), 0) != SQL_SUCCESS) {
      ADD_FAILURE() << state(SQL_HANDLE_DBC, dbc_);
      return false;
    }
    if (stmt_ && SQLSetStmtAttr(stmt_, SQL_ATTR_QUERY_TIMEOUT, reinterpret_cast<SQLPOINTER>(seconds), 0) != SQL_SUCCESS) {
      ADD_FAILURE() << state(SQL_HANDLE_STMT, stmt_);
      return false;
    }
    return true;
  }
  void direct(const std::string& sql) {
    ASSERT_TRUE(cap());
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirectA(stmt_, bytes(sql.c_str()), SQL_NTS)) << state(SQL_HANDLE_STMT, stmt_);
  }
  bool cell(SQLUSMALLINT column, std::string& value) {
    std::array<char, 256> buffer{};
    SQLLEN length = -1;
    const auto rc = SQLGetData(stmt_, column, SQL_C_CHAR, buffer.data(), static_cast<SQLLEN>(buffer.size()), &length);
    if (rc != SQL_SUCCESS || length < 0 || static_cast<std::size_t>(length) >= buffer.size() || buffer[static_cast<std::size_t>(length)] != '\0') {
      return false;
    }
    value.assign(buffer.data(), static_cast<std::size_t>(length));
    return true;
  }
  void rows(const Rows& expected) {
    ASSERT_FALSE(expected.empty());
    SQLSMALLINT columns = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt_, &columns));
    ASSERT_EQ(expected.front().size(), static_cast<std::size_t>(columns));
    for (const auto& row : expected) {
      ASSERT_TRUE(cap());
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt_)) << state(SQL_HANDLE_STMT, stmt_);
      ASSERT_EQ(static_cast<std::size_t>(columns), row.size());
      for (SQLUSMALLINT col = 1; col <= static_cast<SQLUSMALLINT>(columns); ++col) {
        std::string value;
        ASSERT_TRUE(cell(col, value)) << "Null, truncated or unavailable cell; SQLSTATE=" << state(SQL_HANDLE_STMT, stmt_);
        EXPECT_EQ(row[static_cast<std::size_t>(col - 1)], value);
      }
    }
    ASSERT_TRUE(cap());
    ASSERT_EQ(SQL_NO_DATA, SQLFetch(stmt_)) << "Unexpected additional fixture rows";
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt_));
  }
  void SetUp() override {
    const auto marker = inputs::fixture_environment("ODBCPP_REDSHIFT_POWERBI_RELATIONSHIP");
    if (!marker) {
      GTEST_SKIP() << "Optional single-key relationship fixture not admitted";
    }
    ASSERT_EQ("1", *marker) << "Malformed relationship admission marker";
    admitted_ = true;
    end_ = Clock::now() + std::chrono::seconds{40};
    // The canonical private DSN stager owns TLS, endpoint, limits and password
    // custody. Missing/empty inputs after admission fail, never fall back.
    const auto connection = inputs::fixture_environment("ODBCPP_TEST_DSN");
    ASSERT_TRUE(connection.has_value());
    ASSERT_FALSE(connection->empty());
    ASSERT_LE(connection->size(), std::size_t{16384});
    parent_oid_ = setting("ODBCPP_REDSHIFT_POWERBI_PARENT_OID");
    fact_oid_ = setting("ODBCPP_REDSHIFT_POWERBI_FACT_OID");
    ASSERT_TRUE(oid(parent_oid_) && oid(fact_oid_) && parent_oid_ != fact_oid_);
    const auto driver = setting("ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER");
    std::error_code error;
    const auto driver_path = std::filesystem::path(driver);
    ASSERT_TRUE(driver_path.is_absolute());
    const auto canonical = std::filesystem::canonical(driver_path, error);
    ASSERT_FALSE(error);
    ASSERT_EQ(canonical, driver_path);
    ASSERT_TRUE(std::filesystem::is_regular_file(canonical, error) && !error);
#ifdef ODBCPP_EXPECT_DM_SQLWCHAR_SIZE
    ASSERT_EQ(static_cast<std::size_t>(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE), sizeof(SQLWCHAR));
#endif
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_));
    ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(env_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, env_, &dbc_));
    ASSERT_TRUE(cap());
    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc_, SQL_ATTR_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(std::uintptr_t{5}), 0));
    const auto rc = SQLDriverConnectA(dbc_, nullptr, bytes(connection->c_str()),
        inputs::fixture_smallint_length(connection->size()), nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    connected_ = SQL_SUCCEEDED(rc);
    ASSERT_EQ(SQL_SUCCESS, rc) << state(SQL_HANDLE_DBC, dbc_);
    const auto images = inputs::loaded_image_paths();
    ASSERT_TRUE(images.has_value());
    std::size_t matches = 0;
    for (const auto& path : *images) {
      if (inputs::is_database_driver_image(path)) {
        const auto actual = std::filesystem::canonical(path, error);
        ASSERT_FALSE(error);
        ASSERT_EQ(canonical, actual) << "Mixed or unexpected database driver image";
        ++matches;
      }
    }
    ASSERT_EQ(std::size_t{1}, matches);
    RecordProperty("relationship_loaded_driver_image", canonical.string());
    RecordProperty("relationship_parent_oid", parent_oid_);
    RecordProperty("relationship_fact_oid", fact_oid_);
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &stmt_));
    ASSERT_TRUE(cap());
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt_, SQL_ATTR_METADATA_ID, reinterpret_cast<SQLPOINTER>(SQL_FALSE), 0));
    ASSERT_NO_FATAL_FAILURE(direct("SELECT TRIM(current_user),current_database()"));
    ASSERT_NO_FATAL_FAILURE(rows({{"odbcpp_pilot_test", "odbcpp_pilot"}}));
    ASSERT_NO_FATAL_FAILURE(direct("SELECT c.oid,c.relname,TRIM(u.usename) FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON c.relnamespace=n.oid JOIN pg_user u ON u.usesysid=c.relowner WHERE n.nspname='odbcpp_fixture' AND c.relname IN ('pbi_parent_single_20261022_q007','pbi_fact_single_20261022_q007') AND c.relkind='r' ORDER BY c.relname"));
    ASSERT_NO_FATAL_FAILURE(rows({{fact_oid_, fact, "odbcpp_pilot_admin"}, {parent_oid_, parent, "odbcpp_pilot_admin"}}));
  }
  void TearDown() override {
    if (admitted_) {
      EXPECT_LT(Clock::now(), end_) << "Relationship case exceeded its original window";
    }
    // Never issue repair SQL or retry an unknown dispatch from test cleanup.
    if (stmt_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt_));
      stmt_ = SQL_NULL_HSTMT;
    }
    if (dbc_) {
      if (connected_ && cap()) {
        EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(dbc_)) << state(SQL_HANDLE_DBC, dbc_);
      }
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc_));
      dbc_ = SQL_NULL_HDBC;
    }
    if (env_) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, env_));
      env_ = SQL_NULL_HENV;
    }
  }
};

TEST_F(PowerBiRelationshipNative, ColumnsAndSingleKeyMetadata) {
  const auto schema_pattern = pattern(schema);
  for (const auto* table : {parent, fact}) {
    const auto table_pattern = pattern(table);
    ASSERT_TRUE(cap());
    ASSERT_EQ(SQL_SUCCESS, SQLTablesA(stmt_, bytes("odbcpp_pilot"), SQL_NTS,
        bytes(schema_pattern.c_str()), SQL_NTS, bytes(table_pattern.c_str()), SQL_NTS,
        bytes("TABLE"), SQL_NTS)) << state(SQL_HANDLE_STMT, stmt_);
    SQLSMALLINT table_columns = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt_, &table_columns));
    ASSERT_EQ(5, table_columns);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt_));
    const std::array<std::string, 4> table_identity{"odbcpp_pilot", schema, table, "TABLE"};
    for (SQLUSMALLINT column = 1; column <= 4; ++column) {
      std::string value;
      ASSERT_TRUE(cell(column, value));
      EXPECT_EQ(table_identity[static_cast<std::size_t>(column - 1)], value);
    }
    ASSERT_EQ(SQL_NO_DATA, SQLFetch(stmt_));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt_));
    ASSERT_TRUE(cap());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsA(stmt_, bytes("odbcpp_pilot"), SQL_NTS,
        bytes(schema_pattern.c_str()), SQL_NTS, bytes(table_pattern.c_str()), SQL_NTS, nullptr, 0)) << state(SQL_HANDLE_STMT, stmt_);
    SQLSMALLINT count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt_, &count));
    ASSERT_EQ(18, count);
    const std::vector<std::string> names = std::string_view(table) == parent ? std::vector<std::string>{"parent_id", "parent_label"} :
        std::vector<std::string>{"fact_id", "parent_id", "amount"};
    for (std::size_t i = 0; i < names.size(); ++i) {
      ASSERT_TRUE(cap());
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt_));
      std::string catalog, actual_schema, actual_table, name, type, nullable, ordinal;
      ASSERT_TRUE(cell(1, catalog));
      ASSERT_TRUE(cell(2, actual_schema));
      ASSERT_TRUE(cell(3, actual_table));
      ASSERT_TRUE(cell(4, name));
      ASSERT_TRUE(cell(5, type));
      ASSERT_TRUE(cell(11, nullable));
      ASSERT_TRUE(cell(17, ordinal));
      EXPECT_EQ("odbcpp_pilot", catalog);
      EXPECT_EQ(schema, actual_schema);
      EXPECT_EQ(table, actual_table);
      EXPECT_EQ(names[i], name);
      EXPECT_EQ(std::to_string(names[i] == "parent_label" ? SQL_VARCHAR : SQL_INTEGER), type);
      EXPECT_EQ(std::to_string(SQL_NO_NULLS), nullable);
      EXPECT_EQ(std::to_string(i + 1), ordinal);
    }
    ASSERT_EQ(SQL_NO_DATA, SQLFetch(stmt_));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt_));
    ASSERT_TRUE(cap());
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeysA(stmt_, bytes("odbcpp_pilot"), SQL_NTS,
        bytes(schema), SQL_NTS, bytes(table), SQL_NTS)) << state(SQL_HANDLE_STMT, stmt_);
    const bool is_parent = std::string_view(table) == parent;
    ASSERT_NO_FATAL_FAILURE(rows({{"odbcpp_pilot", schema, table, is_parent ? "parent_id" : "fact_id", "1",
        is_parent ? "pbi_parent_single_q007_pk" : "pbi_fact_single_q007_pk"}}));
  }
  ASSERT_TRUE(cap());
  ASSERT_EQ(SQL_SUCCESS, SQLForeignKeysA(stmt_, bytes("odbcpp_pilot"), SQL_NTS, bytes(schema), SQL_NTS,
      bytes(parent), SQL_NTS, bytes("odbcpp_pilot"), SQL_NTS, bytes(schema), SQL_NTS, bytes(fact), SQL_NTS)) << state(SQL_HANDLE_STMT, stmt_);
  SQLSMALLINT count = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt_, &count));
  ASSERT_EQ(14, count);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt_));
  const std::array<std::string, 9> expected{"odbcpp_pilot", schema, parent, "parent_id", "odbcpp_pilot", schema, fact, "parent_id", "1"};
  for (SQLUSMALLINT col = 1; col <= 9; ++col) {
    std::string value;
    ASSERT_TRUE(cell(col, value));
    EXPECT_EQ(expected[static_cast<std::size_t>(col - 1)], value);
  }
  std::string fk_name, pk_name;
  ASSERT_TRUE(cell(12, fk_name));
  ASSERT_TRUE(cell(13, pk_name));
  EXPECT_EQ("pbi_fact_single_q007_fk", fk_name);
  EXPECT_EQ("pbi_parent_single_q007_pk", pk_name);
  ASSERT_EQ(SQL_NO_DATA, SQLFetch(stmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt_));
}

TEST_F(PowerBiRelationshipNative, RowsFilterAndReset) {
  ASSERT_NO_FATAL_FAILURE(direct("SELECT parent_id,parent_label FROM odbcpp_fixture.pbi_parent_single_20261022_q007 ORDER BY parent_id"));
  ASSERT_NO_FATAL_FAILURE(rows({{"1", "North"}, {"2", "South"}}));
  ASSERT_NO_FATAL_FAILURE(direct("SELECT fact_id,parent_id,amount FROM odbcpp_fixture.pbi_fact_single_20261022_q007 ORDER BY fact_id"));
  ASSERT_NO_FATAL_FAILURE(rows({{"101", "1", "10"}, {"102", "1", "20"}, {"103", "2", "7"}, {"104", "2", "13"}, {"105", "2", "5"}}));
  const std::string aggregate = "SELECT COUNT(*),SUM(f.amount) FROM odbcpp_fixture.pbi_fact_single_20261022_q007 f JOIN odbcpp_fixture.pbi_parent_single_20261022_q007 p ON f.parent_id=p.parent_id";
  ASSERT_NO_FATAL_FAILURE(direct(aggregate));
  ASSERT_NO_FATAL_FAILURE(rows({{"5", "55"}}));
  ASSERT_NO_FATAL_FAILURE(direct(aggregate + " WHERE p.parent_label='North'"));
  ASSERT_NO_FATAL_FAILURE(rows({{"2", "30"}}));
  ASSERT_NO_FATAL_FAILURE(direct(aggregate + " WHERE p.parent_label='South'"));
  ASSERT_NO_FATAL_FAILURE(rows({{"3", "25"}}));
  ASSERT_NO_FATAL_FAILURE(direct(aggregate));
  ASSERT_NO_FATAL_FAILURE(rows({{"5", "55"}}));
}
}  // namespace
