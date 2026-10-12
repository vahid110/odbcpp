#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/connection_string.h"
#include "odbcpp/database/backend_provider.h"
#include "core/database/postgres/pg_database_connection.h"
#include "core/database/postgres/redshift_primary_key_contract.h"
#include "core/database/postgres/redshift_foreign_key_contract.h"
#include "odbc/resource_limits.h"
#include <charconv>
#include <vector>
#include <array>
#include <limits>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace {
// Closed local validation reasons only. Return static allowlist storage, never
// the incoming native diagnostic or a substring of it.
constexpr std::string_view safe_schema_refusal_label(std::string_view message) {
  constexpr std::array<std::string_view, 19> allowed{
      "Invalid Redshift schema metadata: lookup-structure",
      "Invalid Redshift schema metadata: lookup-columns",
      "Invalid Redshift schema metadata: lookup-name",
      "Invalid Redshift schema metadata: lookup-type",
      "Invalid Redshift schema metadata: lookup-rows",
      "Invalid Redshift schema metadata: lookup-completion",
      "Invalid Redshift schema metadata: lookup-identifier",
      "Invalid Redshift schema metadata: lookup-snapshot",
      "Invalid Redshift schema metadata: show-database",
      "Invalid Redshift schema metadata: show-structure",
      "Invalid Redshift schema metadata: show-columns",
      "Invalid Redshift schema metadata: show-rows",
      "Invalid Redshift schema metadata: show-completion",
      "Invalid Redshift schema metadata: show-layout",
      "Invalid Redshift schema metadata: show-type",
      "Invalid Redshift schema metadata: show-null-identity",
      "Invalid Redshift schema metadata: show-foreign-database",
      "Invalid Redshift schema metadata: show-identifier",
      "Invalid Redshift schema metadata: show-duplicate"};
  constexpr std::string_view prefix = "Invalid Redshift schema metadata: ";
  for (const auto candidate : allowed)
    if (message == candidate) return candidate.substr(prefix.size());
  return "unknown";
}
static_assert(safe_schema_refusal_label("Invalid Redshift schema metadata: lookup-completion")=="lookup-completion");
static_assert(safe_schema_refusal_label("synthetic-secret-marker")=="unknown");
static_assert(safe_schema_refusal_label("Invalid Redshift schema metadata: lookup-completion synthetic-secret-marker")=="unknown");
constexpr char schema_diagnostic_embedded_nul[]="Invalid Redshift schema metadata: lookup-completion\0synthetic-secret-marker";
static_assert(safe_schema_refusal_label(std::string_view(schema_diagnostic_embedded_nul,
    sizeof(schema_diagnostic_embedded_nul)-1))=="unknown");
constexpr auto schema_diagnostic_oversize=[] {
  std::array<char,4096> bytes{};bytes.fill('x');return bytes;
}();
static_assert(safe_schema_refusal_label(std::string_view(schema_diagnostic_oversize.data(),
    schema_diagnostic_oversize.size()))=="unknown");

}  // namespace

class RedshiftRealTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char* configured = std::getenv("ODBCPP_REDSHIFT_TEST_CONNECTION");
    ASSERT_NE(configured, nullptr)
        << "ODBCPP_REDSHIFT_TEST_CONNECTION is required; the real Redshift "
           "pilot must fail rather than skip when its endpoint is absent";
    connection_string_ = configured;
    ASSERT_FALSE(connection_string_.empty())
        << "ODBCPP_REDSHIFT_TEST_CONNECTION must not be empty";
    const auto parameters = rs::odbc::ConnectionString::parse(connection_string_);
    const auto ssl = parameters.find("SSL");
    ASSERT_NE(ssl, parameters.end())
        << "The real Redshift pilot requires explicit verified TLS (SSL=1)";
    std::string ssl_value = ssl->second;
    std::transform(ssl_value.begin(), ssl_value.end(), ssl_value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    ASSERT_TRUE(ssl_value == "1" || ssl_value == "true" || ssl_value == "yes" ||
                ssl_value == "on")
        << "The real Redshift pilot requires SSL=1";
    
    // Allocate handles
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv_), SQL_SUCCESS);
    ASSERT_EQ(SQLSetEnvAttr(henv_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<void*>(SQL_OV_ODBC3), 0), SQL_SUCCESS);
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC, henv_, &hdbc_), SQL_SUCCESS);
    ASSERT_EQ(SQLSetConnectAttr(hdbc_, SQL_ATTR_LOGIN_TIMEOUT,
                              reinterpret_cast<void*>(std::uintptr_t{15}), 0),
              SQL_SUCCESS);
    ASSERT_EQ(SQLSetConnectAttr(hdbc_, SQL_ATTR_CONNECTION_TIMEOUT,
                              reinterpret_cast<void*>(std::uintptr_t{15}), 0),
              SQL_SUCCESS);
  }
  
  void TearDown() override {
    if (hstmt_) {
      EXPECT_EQ(SQLFreeHandle(SQL_HANDLE_STMT, hstmt_), SQL_SUCCESS);
    }
    if (hdbc_) {
      if (connected_) {
        EXPECT_EQ(SQLDisconnect(hdbc_), SQL_SUCCESS);
      }
      EXPECT_EQ(SQLFreeHandle(SQL_HANDLE_DBC, hdbc_), SQL_SUCCESS);
    }
    if (henv_) {
      EXPECT_EQ(SQLFreeHandle(SQL_HANDLE_ENV, henv_), SQL_SUCCESS);
    }
  }
  
  bool connect() {
    SQLRETURN ret = SQLDriverConnect(
        hdbc_, nullptr, reinterpret_cast<SQLCHAR*>(connection_string_.data()),
        SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
      connected_ = true;
      if (SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &hstmt_) != SQL_SUCCESS)
        return false;
      return SQLSetStmtAttr(hstmt_, SQL_ATTR_QUERY_TIMEOUT,
                            reinterpret_cast<void*>(std::uintptr_t{15}), 0)
             == SQL_SUCCESS;
    }
    return false;
  }
  
  std::string get_error(SQLSMALLINT handle_type, SQLHANDLE handle) {
    SQLCHAR sqlstate[6]{};
    const auto result = SQLGetDiagRec(handle_type, handle, 1, sqlstate,
                                      nullptr, nullptr, 0, nullptr);
    if (result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO)
      return std::string(reinterpret_cast<char*>(sqlstate));
    return "diagnostic unavailable";
  }

  std::string schema_catalog_diagnostic() {
    std::array<SQLCHAR,6> state{};
    std::array<SQLCHAR,256> message{};
    SQLSMALLINT length=-1;
    const auto result=SQLGetDiagRec(SQL_HANDLE_STMT,hstmt_,1,state.data(),nullptr,
        message.data(),static_cast<SQLSMALLINT>(message.size()),&length);
    std::string_view label="unknown";
    if(result==SQL_SUCCESS&&length>=0&&static_cast<std::size_t>(length)<message.size())
      label=safe_schema_refusal_label(std::string_view(
          reinterpret_cast<const char*>(message.data()),static_cast<std::size_t>(length)));
    const bool valid_state=(result==SQL_SUCCESS||result==SQL_SUCCESS_WITH_INFO)&&state[5]==0&&
        std::all_of(state.begin(),state.begin()+5,[](unsigned char c){
          return (c>='A'&&c<='Z')||(c>='0'&&c<='9');
        });
    return "schema_refusal="+std::string(label)+" state="+
        (valid_state?std::string(reinterpret_cast<const char*>(state.data()),5):"unknown");
  }

  // Used only for the fixed pilot metadata query. The bounded pilot runner
  // keeps assertion/XML output private and exports case counts, never this text.
  std::string metadata_diagnostic() {
    SQLCHAR state[6]{};
    SQLCHAR message[1024]{};
    SQLINTEGER native = 0;
    SQLSMALLINT length = 0;
    const auto result = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt_, 1, state,
                                     &native, message, sizeof(message), &length);
    if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO)
      return "metadata diagnostic unavailable";
    return std::string(reinterpret_cast<char*>(state)) + ": " +
           std::string(reinterpret_cast<char*>(message));
  }

  struct CatalogField { const char* name; SQLSMALLINT type; };
  void expect_catalog_fields(std::span<const CatalogField> fields) {
    SQLSMALLINT count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt_, &count));
    ASSERT_EQ(fields.size(), static_cast<std::size_t>(count));
    for (SQLUSMALLINT column = 1; column <= fields.size(); ++column) {
      SQLCHAR name[128]{}; SQLSMALLINT type = 0, digits = 0, nullable = 0;
      SQLULEN size = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt_, column, name, sizeof(name),
          nullptr, &type, &size, &digits, &nullable));
      std::string actual(reinterpret_cast<char*>(name));
      std::transform(actual.begin(), actual.end(), actual.begin(),
          [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      EXPECT_EQ(fields[column - 1].name, actual);
      EXPECT_EQ(fields[column - 1].type, type);
      RecordProperty(actual + "_column_size", std::to_string(size));
      RecordProperty(actual + "_decimal_digits", std::to_string(digits));
      RecordProperty(actual + "_nullable", std::to_string(nullable));
    }
  }

  void expect_empty_catalog(std::span<const CatalogField> fields) {
    expect_catalog_fields(fields);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  }

  bool catalog_succeeded(SQLRETURN result) {
    if (result == SQL_SUCCESS) return true;
    const auto diagnostic = metadata_diagnostic();
    // Recover once on the same connection; never retry the failed catalog.
    EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_CLOSE));
    const auto recovered = SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
        const_cast<char*>("SELECT 1")), SQL_NTS);
    EXPECT_EQ(SQL_SUCCESS, recovered);
    if (recovered == SQL_SUCCESS) {
      EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
      SQLINTEGER value = 0; SQLLEN length = 0;
      EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_SLONG, &value,
          sizeof(value), &length));
      EXPECT_EQ(1, value);
      EXPECT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
    }
    ADD_FAILURE() << "Catalog failed before qualification: " << diagnostic;
    return false;
  }

  void expect_catalog_integer(SQLUSMALLINT column, SQLINTEGER expected) {
    SQLINTEGER value = -9876; SQLLEN length = -9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_SLONG, &value,
        sizeof(value), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    EXPECT_EQ(expected, value);
  }

  void expect_catalog_null(SQLUSMALLINT column) {
    std::array<char, 16> value;
    value.fill('!'); const auto before = value; SQLLEN length = -9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_CHAR, value.data(),
        static_cast<SQLLEN>(value.size()), &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(before, value);
  }

  void expect_catalog_text(SQLUSMALLINT column, const char* expected) {
    char value[128]{}; SQLLEN length = -9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_CHAR, value,
        sizeof(value), &length));
    EXPECT_EQ(static_cast<SQLLEN>(std::strlen(expected)), length);
    EXPECT_STREQ(expected, value);
  }

  bool connected_ = false;

  SQLHENV henv_ = nullptr;
  SQLHDBC hdbc_ = nullptr;
  SQLHSTMT hstmt_ = nullptr;
  
  void modern_primary_key_contract(bool use_executor, bool invalid_names = false, bool legacy = false);
  void odbc_primary_key_mode_contract(const char* explicit_mode);
  void odbc_primary_key_edge_contract(const char* mode, bool quoted);
  std::string connection_string_;
};

TEST_F(RedshiftRealTest, ConnectionTest) {
  ASSERT_TRUE(connect()) << "Failed to connect: " << get_error(SQL_HANDLE_DBC, hdbc_);
}

TEST_F(RedshiftRealTest, VersionQuery) {
  ASSERT_TRUE(connect());
  
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT version()")), 
                               SQL_NTS);
  ASSERT_EQ(ret, SQL_SUCCESS) << "Query failed: " << get_error(SQL_HANDLE_STMT, hstmt_);
  
  ret = SQLFetch(hstmt_);
  ASSERT_EQ(ret, SQL_SUCCESS) << "Fetch failed: " << get_error(SQL_HANDLE_STMT, hstmt_);
  
  char version[512];
  SQLLEN indicator;
  ret = SQLGetData(hstmt_, 1, SQL_C_CHAR, version, sizeof(version), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS) << "GetData failed: " << get_error(SQL_HANDLE_STMT, hstmt_);
  
  std::string server_identity(version);
  std::transform(server_identity.begin(), server_identity.end(),
                 server_identity.begin(), [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  EXPECT_NE(server_identity.find("redshift"), std::string::npos)
      << "Connected endpoint is not Amazon Redshift";
}

TEST_F(RedshiftRealTest, CurrentUserQuery) {
  ASSERT_TRUE(connect());
  
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT current_user")), 
                               SQL_NTS);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  ret = SQLFetch(hstmt_);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  char current_user[256];
  SQLLEN indicator;
  ret = SQLGetData(hstmt_, 1, SQL_C_CHAR, current_user, sizeof(current_user), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  EXPECT_STRNE(current_user, "");

}

TEST_F(RedshiftRealTest, MultipleRowQuery) {
  ASSERT_TRUE(connect());
  
  auto query = const_cast<char*>(
      "SELECT num FROM (SELECT 1 AS num UNION SELECT 2 UNION SELECT 3 "
      "UNION SELECT 4 UNION SELECT 5) AS rows ORDER BY num");
  SQLRETURN ret = SQLExecDirect(hstmt_,
                               reinterpret_cast<SQLCHAR*>(query),
                               SQL_NTS);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  int row_count = 0;
  while ((ret = SQLFetch(hstmt_)) == SQL_SUCCESS) {
    row_count++;
    
    char num_str[32];
    SQLLEN indicator;
    ret = SQLGetData(hstmt_, 1, SQL_C_CHAR, num_str, sizeof(num_str), &indicator);
    ASSERT_EQ(ret, SQL_SUCCESS);
    
    int num = std::stoi(num_str);
    EXPECT_EQ(num, row_count);
  }
  
  EXPECT_EQ(ret, SQL_NO_DATA);
  EXPECT_EQ(row_count, 5);
}

TEST_F(RedshiftRealTest, PreparedScalarAndNull) {
  ASSERT_TRUE(connect());

  auto query = reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT ?::integer AS scalar_value, "
                        "NULL::varchar AS null_value"));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt_, query, SQL_NTS));
  SQLINTEGER input = 42;
  SQLLEN input_length = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
                             SQL_INTEGER, 0, 0, &input, 0, &input_length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));

  SQLINTEGER output = 0;
  SQLLEN indicator = 0;
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(hstmt_, 1, SQL_C_SLONG, &output, sizeof(output),
                       &indicator));
  EXPECT_EQ(42, output);
  char null_value[8]{};
  ASSERT_EQ(SQL_SUCCESS,
            SQLGetData(hstmt_, 2, SQL_C_CHAR, null_value,
                       sizeof(null_value), &indicator));
  EXPECT_EQ(SQL_NULL_DATA, indicator);
}

// Prepared proof inventory only: these cases are not admitted by a paid runner.
TEST_F(RedshiftRealTest, RowVersionEmptyDescriptorContract) {
  const char* schema = std::getenv("ODBCPP_REDSHIFT_TEST_SCHEMA");
  const char* table = std::getenv("ODBCPP_REDSHIFT_TEST_TABLE");
  ASSERT_NE(nullptr, schema); ASSERT_NE(nullptr, table);
  ASSERT_NE('\0', *schema); ASSERT_NE('\0', *table);
  ASSERT_TRUE(connect());
  constexpr std::array<CatalogField, 8> fields{{
      {"scope", SQL_SMALLINT}, {"column_name", SQL_VARCHAR},
      {"data_type", SQL_SMALLINT}, {"type_name", SQL_VARCHAR},
      {"column_size", SQL_INTEGER}, {"buffer_length", SQL_INTEGER},
      {"decimal_digits", SQL_SMALLINT}, {"pseudo_column", SQL_SMALLINT}}};
  constexpr std::array<SQLUSMALLINT, 3> scopes{
      SQL_SCOPE_CURROW, SQL_SCOPE_TRANSACTION, SQL_SCOPE_SESSION};
  constexpr std::array<SQLUSMALLINT, 2> nullability{SQL_NO_NULLS, SQL_NULLABLE};
  for (const auto scope : scopes) {
    for (const auto nullable : nullability) {
      SCOPED_TRACE(scope);
      SCOPED_TRACE(nullable);
      ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(hstmt_, SQL_ROWVER, nullptr, 0,
          reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
          reinterpret_cast<SQLCHAR*>(const_cast<char*>(table)), SQL_NTS,
          scope, nullable)) << metadata_diagnostic();
      expect_empty_catalog(fields);
      ASSERT_FALSE(HasFatalFailure());
    }
  }
}

TEST_F(RedshiftRealTest, StatisticsQuickEmptyDescriptorContract) {
  const char* schema = std::getenv("ODBCPP_REDSHIFT_TEST_SCHEMA");
  const char* table = std::getenv("ODBCPP_REDSHIFT_TEST_TABLE");
  ASSERT_NE(nullptr, schema); ASSERT_NE(nullptr, table);
  ASSERT_NE('\0', *schema); ASSERT_NE('\0', *table);
  ASSERT_TRUE(connect());
  constexpr std::array<CatalogField, 13> fields{{
      {"table_cat", SQL_VARCHAR}, {"table_schem", SQL_VARCHAR},
      {"table_name", SQL_VARCHAR}, {"non_unique", SQL_SMALLINT},
      {"index_qualifier", SQL_VARCHAR}, {"index_name", SQL_VARCHAR},
      {"type", SQL_SMALLINT}, {"ordinal_position", SQL_SMALLINT},
      {"column_name", SQL_VARCHAR}, {"asc_or_desc", SQL_VARCHAR},
      {"cardinality", SQL_INTEGER}, {"pages", SQL_INTEGER},
      {"filter_condition", SQL_VARCHAR}}};
  constexpr std::array<SQLUSMALLINT, 2> uniqueness{SQL_INDEX_ALL, SQL_INDEX_UNIQUE};
  for (const auto unique : uniqueness) {
    SCOPED_TRACE(unique);
    ASSERT_EQ(SQL_SUCCESS, SQLStatistics(hstmt_, nullptr, 0,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(table)), SQL_NTS,
        unique, SQL_QUICK)) << metadata_diagnostic();
    expect_empty_catalog(fields);
    ASSERT_FALSE(HasFatalFailure());
  }
}

// Requires the separately reviewed catalog_contracts.sql fixture and principal
// visibility. Modern descriptor expectations are intentionally not relaxed to
// accept inherited PostgreSQL metadata; these cases can expose current gaps.
TEST_F(RedshiftRealTest, CompositePrimaryKeyCatalogContract) {
  ASSERT_TRUE(connect());
  constexpr char schema[] = "odbcpp_fixture";
  constexpr char parent[] = "m2_catalog_parent_20261003_c01";
  constexpr std::array<CatalogField, 6> fields{{
      {"table_cat", SQL_VARCHAR}, {"table_schem", SQL_VARCHAR},
      {"table_name", SQL_VARCHAR}, {"column_name", SQL_VARCHAR},
      {"key_seq", SQL_SMALLINT}, {"pk_name", SQL_VARCHAR}}};
  ASSERT_TRUE(catalog_succeeded(SQLPrimaryKeys(hstmt_, nullptr, 0,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(parent)), SQL_NTS)));
  expect_catalog_fields(fields);
  ASSERT_FALSE(HasFatalFailure());
  constexpr std::array<const char*, 2> keys{"key_b", "key_a"};
  for (SQLSMALLINT sequence = 1; sequence <= 2; ++sequence) {
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
    char name[128]{}; SQLLEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_CHAR, name, sizeof(name), &length));
    EXPECT_STREQ(schema, name);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 3, SQL_C_CHAR, name, sizeof(name), &length));
    EXPECT_STREQ(parent, name);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 4, SQL_C_CHAR, name, sizeof(name), &length));
    EXPECT_STREQ(keys[static_cast<std::size_t>(sequence - 1)], name);
    SQLSMALLINT actual = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 5, SQL_C_SSHORT, &actual, sizeof(actual), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(actual)), length);
    EXPECT_EQ(sequence, actual);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 6, SQL_C_CHAR, name, sizeof(name), &length));
    EXPECT_GT(length, 0); // Server-generated constraint name, not a fixed guess.
  }
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
}

// Future ODBC option-to-execution proof only; registration does not admit SQL.
// The fixed fixture declares (key_b,key_a), reversing physical column order.
// Each ODBC operation has its existing 15s timeout; the future reviewed runner
// must impose its outer bound. No shared SDK deadline across ODBC calls claimed.
void RedshiftRealTest::odbc_primary_key_mode_contract(const char* explicit_mode) {
  const auto base = rs::odbc::ConnectionString::parse(connection_string_);
  ASSERT_FALSE(base.contains("REDSHIFTCATALOGMODE"))
      << "Base configuration must omit the catalog mode to avoid duplicate-option ambiguity";
  for (const auto* field : {"SERVER", "PORT", "DATABASE", "UID", "PWD", "SSLCAFILE"}) {
    ASSERT_TRUE(base.contains(field));
    ASSERT_TRUE(!base.at(field).empty());
    ASSERT_TRUE(base.at(field).find('\0') == std::string::npos);
  }
  // Bind the separately reviewed ordinary-user fixture without printing private
  // endpoint, principal, CA path or credential-bearing connection settings.
  ASSERT_TRUE(base.at("DATABASE") == "odbcpp_pilot");
  ASSERT_TRUE(base.at("UID") == "odbcpp_pilot_test");
  if (!explicit_mode) {
    const auto resolved = rs::odbc::ConnectionString::resolve(
        connection_string_, std::string(rs::core::database::configured_backend_provider().identity().driver_name));
    ASSERT_FALSE(resolved.effective_parameters.contains("REDSHIFTCATALOGMODE"))
        << "Default-mode qualification requires absence of driver and DSN overrides";
  }
  if (explicit_mode) {
    ASSERT_TRUE(std::string_view(explicit_mode) == "SHOW" ||
                std::string_view(explicit_mode) == "LEGACY");
    connection_string_ += ";RedshiftCatalogMode=";
    connection_string_ += explicit_mode;
    connection_string_ += ';';
  }
  ASSERT_TRUE(connect()); // Fixture teardown owns partially allocated handles.
  constexpr char database[] = "odbcpp_pilot";
  constexpr char schema[] = "odbcpp_fixture";
  constexpr char parent[] = "m2_catalog_parent_20261003_c01";
  constexpr std::array<CatalogField, 6> fields{{
      {"table_cat", SQL_VARCHAR}, {"table_schem", SQL_VARCHAR},
      {"table_name", SQL_VARCHAR}, {"column_name", SQL_VARCHAR},
      {"key_seq", SQL_SMALLINT}, {"pk_name", SQL_VARCHAR}}};
  // On an original catalog failure, catalog_succeeded preserves that failure
  // and executes one SELECT1 recovery only. It never retries another mode.
  ASSERT_TRUE(catalog_succeeded(SQLPrimaryKeys(hstmt_,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(database)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(parent)), SQL_NTS)));
  expect_catalog_fields(fields);
  ASSERT_FALSE(HasFatalFailure());
  constexpr std::array<const char*, 2> keys{"key_b", "key_a"};
  std::string observed_constraint;
  for (std::size_t row = 0; row < keys.size(); ++row) {
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
    const std::array<const char*, 4> expected{database, schema, parent, keys[row]};
    for (SQLUSMALLINT column = 1; column <= 4; ++column) {
      std::array<char, 1024> value{}; SQLLEN indicator = SQL_NULL_DATA;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_CHAR,
          value.data(), static_cast<SQLLEN>(value.size()), &indicator));
      ASSERT_NE(SQL_NULL_DATA, indicator);
      EXPECT_EQ(static_cast<SQLLEN>(std::strlen(expected[column - 1])), indicator);
      EXPECT_STREQ(expected[column - 1], value.data());
    }
    SQLSMALLINT sequence = 0; SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 5, SQL_C_SSHORT,
        &sequence, sizeof(sequence), &indicator));
    ASSERT_NE(SQL_NULL_DATA, indicator);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(sequence)), indicator);
    EXPECT_EQ(static_cast<SQLSMALLINT>(row + 1), sequence);
    // The server-generated constraint name is observed, never guessed. The
    // buffer is a local test bound, not an assertion of native maximum width.
    std::array<char, 1024> constraint{}; indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 6, SQL_C_CHAR, constraint.data(),
        static_cast<SQLLEN>(constraint.size()), &indicator));
    ASSERT_NE(SQL_NULL_DATA, indicator);
    ASSERT_GT(indicator, 0);
    ASSERT_LT(static_cast<std::size_t>(indicator), constraint.size());
    EXPECT_EQ(static_cast<std::size_t>(indicator), std::strlen(constraint.data()));
    if (row == 0) observed_constraint.assign(constraint.data(), static_cast<std::size_t>(indicator));
    else EXPECT_EQ(observed_constraint, std::string(constraint.data(), static_cast<std::size_t>(indicator)));
  }
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
}

TEST_F(RedshiftRealTest, OdbcPrimaryKeyExplicitShowOptionContract) {
  odbc_primary_key_mode_contract("SHOW");
}

TEST_F(RedshiftRealTest, OdbcPrimaryKeyExplicitLegacyOptionContract) {
  odbc_primary_key_mode_contract("LEGACY");
}

TEST_F(RedshiftRealTest, OdbcPrimaryKeyDefaultShowOptionContract) {
  odbc_primary_key_mode_contract(nullptr);
}

// Future wider-fixture scope only: no DDL, grants or automatic admission.
void RedshiftRealTest::odbc_primary_key_edge_contract(const char* mode, bool quoted) {
  const auto base = rs::odbc::ConnectionString::parse(connection_string_);
  ASSERT_FALSE(base.contains("REDSHIFTCATALOGMODE"));
  for (const auto* field : {"SERVER", "PORT", "DATABASE", "UID", "PWD", "SSLCAFILE"}) {
    ASSERT_TRUE(base.contains(field));
    ASSERT_TRUE(!base.at(field).empty());
    ASSERT_TRUE(base.at(field).find('\0') == std::string::npos);
  }
  ASSERT_TRUE(base.at("DATABASE") == "odbcpp_pilot");
  ASSERT_TRUE(base.at("UID") == "odbcpp_pilot_test");
  ASSERT_TRUE(std::string_view(mode) == "SHOW" || std::string_view(mode) == "LEGACY");
  connection_string_ += ";RedshiftCatalogMode=";
  connection_string_ += mode; connection_string_ += ';';
  ASSERT_TRUE(connect());
  constexpr char database[] = "odbcpp_pilot";
  constexpr char schema[] = "odbcpp_fixture";
  // Stored identifier bytes, not SQL-delimited/escaped strings. No interpolation.
  constexpr char quoted_table[] = "m2_pk_quote_20261004_c01.a\"b%'_";
  constexpr char no_key_table[] = "m2_pk_none_20261004_c01";
  constexpr char constraint[] = "m2_pk_quote_constraint_20261004_c01.a\"b%_";
  const char* table = quoted ? quoted_table : no_key_table;
  constexpr std::array<CatalogField, 6> fields{{
      {"table_cat", SQL_VARCHAR}, {"table_schem", SQL_VARCHAR},
      {"table_name", SQL_VARCHAR}, {"column_name", SQL_VARCHAR},
      {"key_seq", SQL_SMALLINT}, {"pk_name", SQL_VARCHAR}}};
  // Existing helper retains the original failure and makes one SELECT1 recovery,
  // never catalog replay, mode fallback or fresh-connection retry.
  ASSERT_TRUE(catalog_succeeded(SQLPrimaryKeys(hstmt_,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(database)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(table)), SQL_NTS)));
  expect_catalog_fields(fields);
  ASSERT_FALSE(HasFatalFailure());
  if (quoted) {
    constexpr std::array<const char*, 2> columns{"key\"b", "key.a"};
    for (std::size_t row = 0; row < columns.size(); ++row) {
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
      const std::array<const char*, 6> expected{
          database, schema, quoted_table, columns[row], nullptr, constraint};
      for (SQLUSMALLINT column = 1; column <= 6; ++column) {
        SQLLEN indicator = SQL_NULL_DATA;
        if (column == 5) {
          SQLSMALLINT sequence = -1;
          ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_SSHORT,
              &sequence, sizeof(sequence), &indicator));
          ASSERT_NE(SQL_NULL_DATA, indicator);
          EXPECT_EQ(static_cast<SQLLEN>(sizeof(sequence)), indicator);
          EXPECT_EQ(static_cast<SQLSMALLINT>(row + 1), sequence);
        } else {
          std::array<char, 1024> value{};
          ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_CHAR, value.data(),
              static_cast<SQLLEN>(value.size()), &indicator));
          ASSERT_NE(SQL_NULL_DATA, indicator);
          EXPECT_EQ(static_cast<SQLLEN>(std::strlen(expected[column - 1])), indicator);
          EXPECT_STREQ(expected[column - 1], value.data());
        }
      }
    }
  }
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  // One success-path recovery statement, with scalar and typed-NULL output.
  // Per-operation15s timeout remains; future runner supplies its outer bound.
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT 1, CAST(NULL AS INTEGER)")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  SQLINTEGER scalar = -1; SQLLEN indicator = -9;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_SLONG,
      &scalar, sizeof(scalar), &indicator));
  EXPECT_EQ(1, scalar); EXPECT_EQ(static_cast<SQLLEN>(sizeof(scalar)), indicator);
  SQLINTEGER null_sentinel = 73; indicator = -9;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_SLONG,
      &null_sentinel, sizeof(null_sentinel), &indicator));
  EXPECT_EQ(SQL_NULL_DATA, indicator); EXPECT_EQ(73, null_sentinel);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
}

TEST_F(RedshiftRealTest, OdbcPrimaryKeyNoKeyShowContract) {
  odbc_primary_key_edge_contract("SHOW", false);
}
TEST_F(RedshiftRealTest, OdbcPrimaryKeyNoKeyLegacyContract) {
  odbc_primary_key_edge_contract("LEGACY", false);
}
TEST_F(RedshiftRealTest, OdbcPrimaryKeyQuotedShowContract) {
  odbc_primary_key_edge_contract("SHOW", true);
}
TEST_F(RedshiftRealTest, OdbcPrimaryKeyQuotedLegacyContract) {
  odbc_primary_key_edge_contract("LEGACY", true);
}

// Future LEGACY missing-object proof only. The separately reviewed runner must
// confirm this exact name absent before and after the case; never create/drop it.
// A collision or concurrent appearance invalidates proof. No SHOW outcome claim.
TEST_F(RedshiftRealTest, OdbcPrimaryKeyMissingLegacyContract) {
  const auto base = rs::odbc::ConnectionString::parse(connection_string_);
  ASSERT_FALSE(base.contains("REDSHIFTCATALOGMODE"));
  for (const auto* field : {"SERVER", "PORT", "DATABASE", "UID", "PWD", "SSLCAFILE"}) {
    ASSERT_TRUE(base.contains(field));
    ASSERT_TRUE(!base.at(field).empty());
    ASSERT_TRUE(base.at(field).find('\0') == std::string::npos);
  }
  ASSERT_TRUE(base.at("DATABASE") == "odbcpp_pilot");
  ASSERT_TRUE(base.at("UID") == "odbcpp_pilot_test");
  connection_string_ += ";RedshiftCatalogMode=LEGACY;";
  ASSERT_TRUE(connect()); // Existing fixture owns cleanup after early assertions.
  constexpr char database[] = "odbcpp_pilot";
  constexpr char schema[] = "odbcpp_fixture";
  constexpr char absent[] = "m2_pk_absent_20261004_c01";
  constexpr std::array<CatalogField, 6> fields{{
      {"table_cat", SQL_VARCHAR}, {"table_schem", SQL_VARCHAR},
      {"table_name", SQL_VARCHAR}, {"column_name", SQL_VARCHAR},
      {"key_seq", SQL_SMALLINT}, {"pk_name", SQL_VARCHAR}}};
  // Source-grounded empty expectation, native-unqualified until new admission.
  // Original failure stays failed: existing helper preserves its diagnostic and
  // attempts one SELECT1 recovery, never catalog replay or another mode.
  ASSERT_TRUE(catalog_succeeded(SQLPrimaryKeys(hstmt_,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(database)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(absent)), SQL_NTS)));
  expect_catalog_fields(fields);
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  // Existing15s per-operation bounds; no shared ODBC absolute deadline claim.
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT 1, CAST(NULL AS INTEGER)")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  SQLINTEGER scalar = -1; SQLLEN indicator = -9;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_SLONG,
      &scalar, sizeof(scalar), &indicator));
  EXPECT_EQ(1, scalar);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(scalar)), indicator);
  SQLINTEGER null_sentinel = 73; indicator = -9;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_SLONG,
      &null_sentinel, sizeof(null_sentinel), &indicator));
  EXPECT_EQ(SQL_NULL_DATA, indicator); EXPECT_EQ(73, null_sentinel);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, hstmt_));
  hstmt_ = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(hdbc_));
  connected_ = false;
}

// FUTURE proof only: separately admitted fixture/endpoint, never auto-enabled.
// Dispatch one fixed SHOW through the real production Redshift session. Existing
// parameter type-resolution reads may occur under the SAME absolute deadline;
// their admission requires separate source review. No resolver override, catalog
// fallback, probe, capability SQL, retry or inherited SQLPrimaryKeys execution.
// Pinned upstream56d35297f9bee0cc31c0148581c87ca455639a39:
// rsMetadataAPIHelper.cpp:186; rsMetadataServerProxyHelper.cpp:806-810;
// rsutil.c:16303-16325 (Unspecified OID0 alternative, not default VARCHAR1043).
void RedshiftRealTest::modern_primary_key_contract(bool use_executor, bool invalid_names, bool legacy) {
  using namespace rs::core::database;
  using namespace rs::core::database::postgres;
  const auto fields = rs::odbc::ConnectionString::parse(connection_string_);
  for (const auto* key : {"SERVER", "PORT", "DATABASE", "UID", "PWD", "SSLCAFILE"}) {
    ASSERT_TRUE(fields.contains(key)) << "Required explicit connection field missing";
    ASSERT_FALSE(fields.at(key).empty()) << "Required explicit connection field empty";
    ASSERT_TRUE(fields.at(key).find('\0') == std::string::npos)
        << "Required connection field contains NUL";
  }
  // Identity was independently verified by the reviewed runner, not discovered
  // with additional SQL here. Boolean assertions avoid dumping private settings.
  ASSERT_TRUE(fields.at("DATABASE") == "odbcpp_pilot") << "Unexpected pilot database";
  ASSERT_TRUE(fields.at("UID") == "odbcpp_pilot_test") << "Unexpected pilot principal";
  const auto& port_text = fields.at("PORT");
  ASSERT_LE(port_text.size(), 5u);
  ASSERT_TRUE(port_text.find_first_not_of("0123456789") == std::string::npos);
  unsigned port = 0;
  const auto parsed_port = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
  ASSERT_TRUE(parsed_port.ec == std::errc{} && parsed_port.ptr == port_text.data() + port_text.size());
  ASSERT_TRUE(port > 0 && port <= 65535);
  ConnectionSettings settings;
  settings.host = fields.at("SERVER");
  settings.port = static_cast<std::uint16_t>(port);
  settings.database = fields.at("DATABASE");
  settings.user = fields.at("UID");
  settings.password = fields.at("PWD");
  settings.use_ssl = true;
  settings.ssl_ca_file = fields.at("SSLCAFILE");
  settings.timeout = std::chrono::seconds{15};
  if (legacy) settings.redshift_catalog_mode = RedshiftCatalogMode::Legacy;
  PgDatabaseConnection session(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  auto connected = session.connect(settings);
  ASSERT_TRUE(connected) << (connected ? "" : connected.backend_error().safe_summary());

  if (!legacy) {
  // Authenticated ParameterStatus only. Entire bounded unsigned value required.
  const auto capability = session.get_parameter("show_discovery");
  ASSERT_FALSE(capability.empty()) << "Authenticated SHOW capability missing";
  ASSERT_LE(capability.size(), 10u) << "SHOW capability exceeds bounded representation";
  ASSERT_TRUE(capability.find_first_not_of("0123456789") == std::string::npos)
      << "Malformed SHOW capability";
  std::uint32_t version = 0;
  const auto parsed = std::from_chars(capability.data(), capability.data() + capability.size(), version);
  ASSERT_TRUE(parsed.ec == std::errc{} && parsed.ptr == capability.data() + capability.size())
      << "Malformed SHOW capability";
  ASSERT_GE(version, 4u) << "Modern SHOW discovery unavailable";
  } else {
    ASSERT_TRUE(use_executor);
  }
  auto plan = redshift_primary_key_plan("odbcpp_pilot", "odbcpp_fixture", "m2_catalog_parent_20261003_c01");
  ASSERT_TRUE(plan);
  const std::vector<QueryParameter> parameters{
      {plan->database, QueryParameterType::Unspecified},
      {plan->schema, QueryParameterType::Unspecified},
      {plan->table, QueryParameterType::Unspecified}};
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{15});
  if (invalid_names) {
    ASSERT_TRUE(use_executor);
    auto* executor = session.catalog_execution();
    ASSERT_NE(nullptr, executor);
    const SessionSnapshot expected{SessionState::Idle, SessionDisposition::Reusable};
    ASSERT_EQ(SessionState::Idle, session.session_state());
    std::vector<PrimaryKeysCatalogRequest> invalid{
        {std::nullopt, plan->schema, plan->table},
        {plan->database, std::nullopt, plan->table}};
    for (const auto& bad : {std::string{}, std::string("x\0suffix", 8),
                            std::string("\xff", 1)}) {
      invalid.push_back({bad, plan->schema, plan->table});
      invalid.push_back({plan->database, bad, plan->table});
      invalid.push_back({plan->database, plan->schema, bad});
    }
    ASSERT_EQ(11u, invalid.size());
    for (const auto& input : invalid) {
      auto rejected = executor->execute_catalog(input, deadline);
      ASSERT_FALSE(rejected);
      EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),
                rejected.error());
      EXPECT_EQ(BackendErrorClass::InvalidInput, rejected.backend_error().error_class);
      EXPECT_EQ(BackendOperation::ExecuteCatalog, rejected.backend_error().operation);
      EXPECT_FALSE(rejected.backend_error().native_state);
      EXPECT_FALSE(rejected.backend_error().native_code);
      EXPECT_EQ(expected, rejected.session_snapshot());
      EXPECT_EQ(SessionState::Idle, session.session_state());
    }
    // Real-session recovery is qualified by the one valid catalog execution below. Absence
    // of SQL for local rejections is established separately by spy unit tests.
  }
  // Keep the previously qualified direct exchange and the future backend facet
  // as distinct GoogleTests with identical fixed owning-output assertions.
  auto result = [&]() -> BackendResult<QueryResult> {
    if (use_executor) {
      auto* executor = session.catalog_execution();
      if (!executor) return {rs::util::DbErrorCode::UnsupportedFeature,
                            "Redshift catalog execution facet missing"};
      return executor->execute_catalog(PrimaryKeysCatalogRequest{
          plan->database, plan->schema, plan->table}, deadline);
    }
    return normalize_redshift_primary_keys(*plan, session.execute_prepared(
        "SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;", parameters, deadline));
  }();
  // Trusted private XML retains the original server diagnostic for this fixed
  // statement. Connection settings and credential values are never printed.
  ASSERT_TRUE(result) << (result ? "" : result.backend_error().message);
  session.disconnect(); // Assertions below use the independent owning snapshot.
  ASSERT_EQ(6u, result->columns.size());
  constexpr std::array<const char*, 6> names{
      "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(names[i], result->columns[i].name);
    ASSERT_TRUE(result->columns[i].normalized_type);
    EXPECT_TRUE(result->columns[i].normalized_type->known);
    EXPECT_EQ(i == 4 ? ScalarType::SmallInt : ScalarType::VarChar,
        result->columns[i].normalized_type->type);
  }
  ASSERT_EQ(2u, result->rows.size());
  constexpr std::array<const char*, 2> keys{"key_b", "key_a"};
  std::string observed_pk_name;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    const auto& row = result->rows[i];
    ASSERT_EQ(6u, row.size());
    for (const auto& cell : row) ASSERT_TRUE(cell.has_value());
    EXPECT_EQ(plan->database, *row[0]); EXPECT_EQ(plan->schema, *row[1]);
    EXPECT_EQ(plan->table, *row[2]); EXPECT_EQ(keys[i], *row[3]);
    EXPECT_EQ(std::to_string(i + 1), *row[4]);
    ASSERT_FALSE(row[5]->empty()); // Fixture name is generated, never guessed.
    if (i == 0) observed_pk_name = *row[5];
    else EXPECT_EQ(observed_pk_name, *row[5]);
  }
  EXPECT_TRUE(result->cell_errors.empty());
  EXPECT_TRUE(result->additional_results.empty());
  EXPECT_FALSE(result->error);
}

TEST_F(RedshiftRealTest, ModernPrimaryKeyShowUnspecifiedContract) {
  modern_primary_key_contract(false);
}

TEST_F(RedshiftRealTest, ModernPrimaryKeyExecutionContract) {
  modern_primary_key_contract(true);
}

TEST_F(RedshiftRealTest, ModernPrimaryKeyExecutionInvalidExactNamesPreserveSession) {
  modern_primary_key_contract(true, true);
}

TEST_F(RedshiftRealTest, LegacyPrimaryKeyExecutionContract) {
  modern_primary_key_contract(true, false, true);
}

TEST_F(RedshiftRealTest, LegacyPrimaryKeyExecutionInvalidExactNamesPreserveSession) {
  modern_primary_key_contract(true, true, true);
}

TEST_F(RedshiftRealTest, CompositeForeignKeyCatalogContract) {
  ASSERT_TRUE(connect());
  constexpr char schema[] = "odbcpp_fixture";
  constexpr char parent[] = "m2_catalog_parent_20261003_c01";
  constexpr char child[] = "m2_catalog_child_20261003_c01";
  constexpr std::array<CatalogField, 14> fields{{
      {"pktable_cat", SQL_VARCHAR}, {"pktable_schem", SQL_VARCHAR},
      {"pktable_name", SQL_VARCHAR}, {"pkcolumn_name", SQL_VARCHAR},
      {"fktable_cat", SQL_VARCHAR}, {"fktable_schem", SQL_VARCHAR},
      {"fktable_name", SQL_VARCHAR}, {"fkcolumn_name", SQL_VARCHAR},
      {"key_seq", SQL_SMALLINT}, {"update_rule", SQL_SMALLINT},
      {"delete_rule", SQL_SMALLINT}, {"fk_name", SQL_VARCHAR},
      {"pk_name", SQL_VARCHAR}, {"deferrability", SQL_SMALLINT}}};
  constexpr std::array<const char*, 2> pk{"key_b", "key_a"};
  constexpr std::array<const char*, 2> fk{"ref_b", "ref_a"};
  // Imported, exported, and both-table requests each execute exactly once.
  for (int direction = 0; direction < 3; ++direction) {
    SCOPED_TRACE(direction);
    auto* pk_schema = direction == 0 ? nullptr : reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema));
    auto* pk_table = direction == 0 ? nullptr : reinterpret_cast<SQLCHAR*>(const_cast<char*>(parent));
    auto* fk_schema = direction == 1 ? nullptr : reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema));
    auto* fk_table = direction == 1 ? nullptr : reinterpret_cast<SQLCHAR*>(const_cast<char*>(child));
    ASSERT_TRUE(catalog_succeeded(SQLForeignKeys(hstmt_, nullptr, 0,
        pk_schema, static_cast<SQLSMALLINT>(pk_schema ? SQL_NTS : 0),
        pk_table, static_cast<SQLSMALLINT>(pk_table ? SQL_NTS : 0),
        nullptr, 0, fk_schema, static_cast<SQLSMALLINT>(fk_schema ? SQL_NTS : 0),
        fk_table, static_cast<SQLSMALLINT>(fk_table ? SQL_NTS : 0))));
    expect_catalog_fields(fields);
    ASSERT_FALSE(HasFatalFailure());
    std::string fk_constraint, pk_constraint;
    for (SQLSMALLINT sequence = 1; sequence <= 2; ++sequence) {
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
      char name[128]{}; SQLLEN length = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_CHAR, name, sizeof(name), &length));
      EXPECT_STREQ(schema, name);
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 3, SQL_C_CHAR, name, sizeof(name), &length));
      EXPECT_STREQ(parent, name);
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 4, SQL_C_CHAR, name, sizeof(name), &length));
      EXPECT_STREQ(pk[static_cast<std::size_t>(sequence - 1)], name);
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 6, SQL_C_CHAR, name, sizeof(name), &length));
      EXPECT_STREQ(schema, name);
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 7, SQL_C_CHAR, name, sizeof(name), &length));
      EXPECT_STREQ(child, name);
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 8, SQL_C_CHAR, name, sizeof(name), &length));
      EXPECT_STREQ(fk[static_cast<std::size_t>(sequence - 1)], name);
      SQLSMALLINT value = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 9, SQL_C_SSHORT, &value, sizeof(value), &length));
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
      EXPECT_EQ(sequence, value);
      for (const SQLUSMALLINT column : std::array<SQLUSMALLINT, 2>{10, 11}) {
        value = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_SSHORT, &value, sizeof(value), &length));
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
        EXPECT_EQ(SQL_NO_ACTION, value);
      }
      for (const SQLUSMALLINT column : std::array<SQLUSMALLINT, 2>{12, 13}) {
        name[0] = '\0';
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_CHAR, name, sizeof(name), &length));
        EXPECT_GT(length, 0);
        auto& observed = column == 12 ? fk_constraint : pk_constraint;
        if (sequence == 1) observed = name;
        else EXPECT_EQ(observed, name);
      }
      value = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 14, SQL_C_SSHORT, &value, sizeof(value), &length));
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
      EXPECT_EQ(SQL_NOT_DEFERRABLE, value);
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  }
}

// Pinned modern SHOW expectations; inherited routine SQL is unqualified and
// may fail these future cases. The fixture procedure is never invoked.
TEST_F(RedshiftRealTest, ProcedureCatalogContract) {
  ASSERT_TRUE(connect());
  constexpr char schema[] = "odbcpp_fixture";
  constexpr char procedure[] = "sp_m2_catalog_modes_20261003_c01";
  constexpr std::array<CatalogField, 8> fields{{
      {"procedure_cat", SQL_VARCHAR}, {"procedure_schem", SQL_VARCHAR},
      {"procedure_name", SQL_VARCHAR}, {"num_input_params", SQL_VARCHAR},
      {"num_output_params", SQL_VARCHAR}, {"num_result_sets", SQL_VARCHAR},
      {"remarks", SQL_VARCHAR}, {"procedure_type", SQL_SMALLINT}}};
  ASSERT_TRUE(catalog_succeeded(SQLProcedures(hstmt_, nullptr, 0,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(procedure)), SQL_NTS)));
  expect_catalog_fields(fields);
  ASSERT_FALSE(HasFatalFailure());
  int matched = 0; bool exhausted = false;
  for (int row = 0; row < 64; ++row) {
    const auto fetched = SQLFetch(hstmt_);
    if (fetched == SQL_NO_DATA) { exhausted = true; break; }
    ASSERT_EQ(SQL_SUCCESS, fetched);
    char actual_schema[128]{}, actual_name[128]{}; SQLLEN identity_length = -9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_CHAR, actual_schema,
        sizeof(actual_schema), &identity_length));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 3, SQL_C_CHAR, actual_name,
        sizeof(actual_name), &identity_length));
    if (std::strcmp(actual_schema, schema) || std::strcmp(actual_name, procedure)) continue;
    ++matched;
    for (const SQLUSMALLINT column : std::array<SQLUSMALLINT, 3>{4, 5, 6})
      expect_catalog_null(column);
    // Upstream supplies "" with length1 for this field; capture rather than
    // inventing a zero-length/NULL disposition before real-client evidence.
    char remarks[128]{}; SQLLEN length = -9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 7, SQL_C_CHAR, remarks, sizeof(remarks), &length));
    RecordProperty("procedure_remarks_indicator", std::to_string(length));
    std::string raw_remarks;
    constexpr char hex[] = "0123456789abcdef";
    for (SQLLEN byte = 0; byte < std::min<SQLLEN>(length, 127); ++byte) {
      const auto value = static_cast<unsigned char>(remarks[static_cast<std::size_t>(byte)]);
      raw_remarks.push_back(hex[value >> 4]); raw_remarks.push_back(hex[value & 15]);
    }
    RecordProperty("procedure_remarks_hex", raw_remarks);
    expect_catalog_integer(8, SQL_PT_PROCEDURE);
    ASSERT_FALSE(HasFatalFailure());
  }
  EXPECT_TRUE(exhausted) << "Fixture discovery exceeded the finite64-row inventory";
  EXPECT_EQ(1, matched);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
}

TEST_F(RedshiftRealTest, ProcedureParameterCatalogContract) {
  ASSERT_TRUE(connect());
  constexpr char schema[] = "odbcpp_fixture";
  constexpr char procedure[] = "sp_m2_catalog_modes_20261003_c01";
  constexpr std::array<CatalogField, 19> fields{{
      {"procedure_cat", SQL_VARCHAR}, {"procedure_schem", SQL_VARCHAR},
      {"procedure_name", SQL_VARCHAR}, {"column_name", SQL_VARCHAR},
      {"column_type", SQL_SMALLINT}, {"data_type", SQL_INTEGER},
      {"type_name", SQL_VARCHAR}, {"column_size", SQL_INTEGER},
      {"buffer_length", SQL_INTEGER}, {"decimal_digits", SQL_SMALLINT},
      {"num_prec_radix", SQL_SMALLINT}, {"nullable", SQL_SMALLINT},
      {"remarks", SQL_VARCHAR}, {"column_def", SQL_VARCHAR},
      {"sql_data_type", SQL_INTEGER}, {"sql_datetime_sub", SQL_INTEGER},
      {"char_octet_length", SQL_INTEGER}, {"ordinal_position", SQL_INTEGER},
      {"is_nullable", SQL_VARCHAR}}};
  ASSERT_TRUE(catalog_succeeded(SQLProcedureColumns(hstmt_, nullptr, 0,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(procedure)), SQL_NTS, nullptr, 0)));
  expect_catalog_fields(fields);
  ASSERT_FALSE(HasFatalFailure());
  // Index rows by copied SHOW ordinal; upstream doesn't promise a final sort.
  std::array<bool, 2> seen{}; int matched = 0; bool exhausted = false;
  for (int row = 0; row < 64; ++row) {
    const auto fetched = SQLFetch(hstmt_);
    if (fetched == SQL_NO_DATA) { exhausted = true; break; }
    ASSERT_EQ(SQL_SUCCESS, fetched);
    char actual_schema[128]{}, actual_name[128]{}; SQLLEN identity_length = -9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_CHAR, actual_schema,
        sizeof(actual_schema), &identity_length));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 3, SQL_C_CHAR, actual_name,
        sizeof(actual_name), &identity_length));
    if (std::strcmp(actual_schema, schema) || std::strcmp(actual_name, procedure)) continue;
    ++matched;
    char name[128]{}; SQLLEN length = -9;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 4, SQL_C_CHAR, name, sizeof(name), &length));
    const bool input = std::strcmp(name, "p_input") == 0;
    ASSERT_TRUE(input || std::strcmp(name, "p_result") == 0);
    EXPECT_EQ(static_cast<SQLLEN>(std::strlen(name)), length);
    expect_catalog_integer(5, input ? SQL_PARAM_INPUT : SQL_PARAM_INPUT_OUTPUT);
    expect_catalog_integer(6, SQL_INTEGER);
    expect_catalog_text(7, "int4");
    expect_catalog_integer(8, 10);
    expect_catalog_integer(9, 4);
    expect_catalog_null(10);
    expect_catalog_integer(11, 10);
    expect_catalog_integer(12, SQL_NULLABLE_UNKNOWN);
    expect_catalog_text(13, "");
    expect_catalog_null(14);
    expect_catalog_integer(15, SQL_INTEGER);
    expect_catalog_null(16);
    expect_catalog_null(17);
    const auto index = input ? 0u : 1u;
    expect_catalog_integer(18, static_cast<SQLINTEGER>(index + 1));
    expect_catalog_text(19, "");
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_FALSE(seen[index]); seen[index] = true;
  }
  EXPECT_TRUE(seen[0]); EXPECT_TRUE(seen[1]);
  EXPECT_TRUE(exhausted) << "Fixture discovery exceeded the finite64-row inventory";
  EXPECT_EQ(2, matched); // INOUT once, no synthesized return for this fixture.
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
}

TEST_F(RedshiftRealTest, ConfiguredFixtureMetadata) {
  const char* schema = std::getenv("ODBCPP_REDSHIFT_TEST_SCHEMA");
  const char* table = std::getenv("ODBCPP_REDSHIFT_TEST_TABLE");
  ASSERT_NE(schema, nullptr)
      << "ODBCPP_REDSHIFT_TEST_SCHEMA is required for pilot metadata evidence";
  ASSERT_NE(table, nullptr)
      << "ODBCPP_REDSHIFT_TEST_TABLE is required for pilot metadata evidence";
  ASSERT_NE(*schema, '\0');
  ASSERT_NE(*table, '\0');
  ASSERT_TRUE(connect());

  ASSERT_EQ(SQL_SUCCESS,
            SQLTables(hstmt_, nullptr, 0,
                      reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)),
                      SQL_NTS,
                      reinterpret_cast<SQLCHAR*>(const_cast<char*>(table)),
                      SQL_NTS, nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_))
      << "Configured Redshift fixture table was not discovered";
  char discovered_schema[256]{};
  char discovered_table[256]{};
  SQLLEN length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_CHAR, discovered_schema,
                                   sizeof(discovered_schema), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 3, SQL_C_CHAR, discovered_table,
                                   sizeof(discovered_table), &length));
  EXPECT_STREQ(schema, discovered_schema);
  EXPECT_STREQ(table, discovered_table);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));

  ASSERT_EQ(SQL_SUCCESS,
            SQLColumns(hstmt_, nullptr, 0,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)),
                       SQL_NTS,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>(table)),
                       SQL_NTS, nullptr, 0)) << metadata_diagnostic();
  SQLSMALLINT result_columns = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt_, &result_columns));
  ASSERT_EQ(18, result_columns);
  int ordinal = 0;
  for (const auto* expected_column : {"id", "value"}) {
    ++ordinal;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_))
        << "Configured Redshift fixture exposed incomplete column metadata";
    char column[256]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 4, SQL_C_CHAR, column,
                                     sizeof(column), &length));
    EXPECT_STREQ(expected_column, column);
    const auto number = [&](SQLUSMALLINT index, SQLINTEGER expected) {
      SQLINTEGER value = 0;
      SQLLEN indicator = 0;
      EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, index, SQL_C_LONG, &value,
                                       sizeof(value), &indicator));
      EXPECT_NE(SQL_NULL_DATA, indicator);
      EXPECT_EQ(expected, value);
    };
    number(5, ordinal == 1 ? SQL_INTEGER : SQL_VARCHAR);
    number(7, ordinal == 1 ? 10 : 32);
    number(8, ordinal == 1 ? 4 : 32);
    number(11, SQL_NULLABLE);
    if (ordinal == 2) number(16, 32);
    number(17, ordinal);
  }
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  // An explicitly empty pattern is distinct from an omitted column filter.
  const char empty[] = "";
  ASSERT_EQ(SQL_SUCCESS,
            SQLColumns(hstmt_, nullptr, 0,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>(table)), SQL_NTS,
                       reinterpret_cast<SQLCHAR*>(const_cast<char*>(empty)), SQL_NTS))
      << metadata_diagnostic();
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  // Unescaped v%lue and v_lue match value; escaped wildcard literals must not.
  for (const auto* escaped : {"v\\%lue", "v\\_lue"}) {
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLColumns(hstmt_, nullptr, 0,
                         reinterpret_cast<SQLCHAR*>(const_cast<char*>(schema)), SQL_NTS,
                         reinterpret_cast<SQLCHAR*>(const_cast<char*>(table)), SQL_NTS,
                         reinterpret_cast<SQLCHAR*>(const_cast<char*>(escaped)), SQL_NTS))
        << metadata_diagnostic();
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  }

}

TEST_F(RedshiftRealTest, ErrorHandling) {
  ASSERT_TRUE(connect());
  
  // Execute invalid SQL
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT * FROM nonexistent_table_12345")), 
                               SQL_NTS);
  EXPECT_EQ(ret, SQL_ERROR);
  
  std::string error = get_error(SQL_HANDLE_STMT, hstmt_);
  EXPECT_FALSE(error.empty());
  // ODBC maps the native undefined-table state to table/view not found.
  EXPECT_EQ(error, "42S02");

  ret = SQLExecDirect(
      hstmt_, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 1")),
      SQL_NTS);
  ASSERT_EQ(SQL_SUCCESS, ret) << "Connection did not recover after invalid SQL: "
                              << get_error(SQL_HANDLE_STMT, hstmt_);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
}

TEST_F(RedshiftRealTest, DataTypes) {
  ASSERT_TRUE(connect());
  
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                                 "SELECT 'text_value' as text_col, "
                                 "42 as int_col, "
                                 "3.14159 as float_col, "
                                 "NOW() as timestamp_col")), 
                               SQL_NTS);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  ret = SQLFetch(hstmt_);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  // Test string column
  char text_val[256];
  SQLLEN indicator;
  ret = SQLGetData(hstmt_, 1, SQL_C_CHAR, text_val, sizeof(text_val), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS);
  EXPECT_STREQ(text_val, "text_value");
  
  // Test integer column
  char int_val[32];
  ret = SQLGetData(hstmt_, 2, SQL_C_CHAR, int_val, sizeof(int_val), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS);
  EXPECT_STREQ(int_val, "42");
  
  // Test float column
  char float_val[32];
  ret = SQLGetData(hstmt_, 3, SQL_C_CHAR, float_val, sizeof(float_val), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS);
  EXPECT_TRUE(strstr(float_val, "3.14") != nullptr);
}

TEST_F(RedshiftRealTest, NegativeTests) {
  // Test connection with invalid credentials
  SQLHDBC bad_conn;
  ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC, henv_, &bad_conn), SQL_SUCCESS);
  
  SQLRETURN ret = SQLConnect(bad_conn, (SQLCHAR*)"SERVER=invalid.host;DATABASE=invalid;UID=invalid;PWD=invalid", SQL_NTS, nullptr, 0, nullptr, 0);
  EXPECT_EQ(SQL_ERROR, ret);
  
  // Verify diagnostic is set
  SQLCHAR sqlstate[6], message[256];
  ret = SQLGetDiagRec(SQL_HANDLE_DBC, bad_conn, 1, sqlstate, nullptr, message, sizeof(message), nullptr);
  EXPECT_EQ(SQL_SUCCESS, ret);
  EXPECT_STREQ("08001", (char*)sqlstate);
  
  SQLFreeHandle(SQL_HANDLE_DBC, bad_conn);
  
  // Test operations without connection
  SQLHSTMT disconnected_stmt = reinterpret_cast<SQLHSTMT>(
      static_cast<std::uintptr_t>(1));
  SQLHDBC disconnected_conn;
  ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC, henv_, &disconnected_conn), SQL_SUCCESS);
  EXPECT_EQ(SQL_ERROR, SQLAllocHandle(
      SQL_HANDLE_STMT, disconnected_conn, &disconnected_stmt));
  EXPECT_EQ(nullptr, disconnected_stmt);
  
  // Verify diagnostic
  ret = SQLGetDiagRec(SQL_HANDLE_DBC, disconnected_conn, 1, sqlstate,
                      nullptr, message, sizeof(message), nullptr);
  EXPECT_EQ(SQL_SUCCESS, ret);
  EXPECT_STREQ("08003", (char*)sqlstate);
  
  SQLFreeHandle(SQL_HANDLE_DBC, disconnected_conn);
  
  // Test with invalid handles
  ret = SQLExecDirect(nullptr, (SQLCHAR*)"SELECT 1", SQL_NTS);
  EXPECT_EQ(SQL_INVALID_HANDLE, ret);
  
  ret = SQLFetch(nullptr);
  EXPECT_EQ(SQL_INVALID_HANDLE, ret);
  
  char buffer[256];
  SQLLEN len;
  ret = SQLGetData(nullptr, 1, SQL_C_CHAR, buffer, sizeof(buffer), &len);
  EXPECT_EQ(SQL_INVALID_HANDLE, ret);
}

// Externally issued temporary credentials only; no native IAM provider/refresh.
TEST_F(RedshiftRealTest, IAMPrincipalScalar) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC, hdbc_);
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SET statement_timeout TO 15000")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SHOW statement_timeout")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  char timeout[32]{};
  SQLLEN length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_CHAR, timeout,
                                   sizeof(timeout), &length));
  EXPECT_STREQ("15000", timeout);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT current_user, 7::integer, NULL::integer")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  char principal[128]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_CHAR, principal,
                                   sizeof(principal), &length));
  EXPECT_STREQ("IAMR:odbcpp-redshift-test-runtime", principal);
  SQLINTEGER value = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_LONG, &value,
                                   sizeof(value), &length));
  EXPECT_EQ(7, value);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 3, SQL_C_LONG, &value,
                                   sizeof(value), &length));
  EXPECT_EQ(SQL_NULL_DATA, length);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, hstmt_));
  hstmt_ = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(hdbc_));
  connected_ = false;
}

TEST_F(RedshiftRealTest, IAMInvalidPassword) {
  const char* invalid = std::getenv("ODBCPP_REDSHIFT_IAM_INVALID_CONNECTION");
  ASSERT_NE(nullptr, invalid);
  const auto valid_fields = rs::odbc::ConnectionString::parse(connection_string_);
  const auto invalid_fields = rs::odbc::ConnectionString::parse(invalid);
  for (const auto* key : {"SERVER", "PORT", "DATABASE", "UID", "SSL", "SSLCAFILE"}) {
    ASSERT_TRUE(valid_fields.count(key) != 0 && invalid_fields.count(key) != 0);
    // Boolean comparison keeps connection values out of assertion output.
    ASSERT_TRUE(valid_fields.at(key) == invalid_fields.at(key));
  }
  ASSERT_TRUE(valid_fields.at("UID") == "IAMR:odbcpp-redshift-test-runtime");
  ASSERT_TRUE(invalid_fields.count("PWD") != 0 && valid_fields.count("PWD") != 0);
  ASSERT_TRUE(invalid_fields.at("PWD") != valid_fields.at("PWD"));
  connection_string_ = invalid;
  EXPECT_FALSE(connect());
  EXPECT_EQ("28000", get_error(SQL_HANDLE_DBC, hdbc_));
}

// Future ordinary-password proof only. The reviewed runner supplies both
// configurations and expected principal; registration does not admit live SQL.
TEST_F(RedshiftRealTest, PasswordRejectedThenFreshValidConnection) {
  const char* invalid = std::getenv("ODBCPP_REDSHIFT_AUTH_INVALID_CONNECTION");
  const char* expected = std::getenv("ODBCPP_REDSHIFT_AUTH_EXPECTED_USER");
  ASSERT_NE(nullptr, invalid);
  ASSERT_NE(nullptr, expected);
  const std::string invalid_connection{invalid};
  const std::string expected_user{expected};
  ASSERT_FALSE(expected_user.empty());
  // Fixed output-buffer resource bound, not an asserted AWS username limit.
  ASSERT_TRUE(expected_user.size() < 1024);
  const auto valid_fields = rs::odbc::ConnectionString::parse(connection_string_);
  const auto invalid_fields = rs::odbc::ConnectionString::parse(invalid_connection);
  for (const auto* key : {"SERVER", "PORT", "DATABASE", "UID", "SSL", "SSLCAFILE", "PWD"}) {
    ASSERT_TRUE(valid_fields.contains(key) && invalid_fields.contains(key));
    ASSERT_TRUE(!valid_fields.at(key).empty() && !invalid_fields.at(key).empty());
    ASSERT_TRUE(valid_fields.at(key).find('\0') == std::string::npos &&
                invalid_fields.at(key).find('\0') == std::string::npos);
    if (std::string_view{key} != "PWD") {
      ASSERT_TRUE(valid_fields.at(key) == invalid_fields.at(key));
    }
  }
  ASSERT_TRUE(valid_fields.at("PWD") != invalid_fields.at("PWD"));
  auto valid_policy = valid_fields;
  auto invalid_policy = invalid_fields;
  valid_policy.erase("PWD"); invalid_policy.erase("PWD");
  ASSERT_TRUE(valid_policy == invalid_policy); // Only the password may differ.

  const auto rejected = SQLDriverConnect(hdbc_, nullptr,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(invalid_connection.data())),
      SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
  // Preserve teardown even if the deliberately wrong password unexpectedly works.
  connected_ = rejected == SQL_SUCCESS || rejected == SQL_SUCCESS_WITH_INFO;
  ASSERT_EQ(SQL_ERROR, rejected);
  ASSERT_EQ("28000", get_error(SQL_HANDLE_DBC, hdbc_));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, hdbc_));
  hdbc_ = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, henv_, &hdbc_));
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(hdbc_, SQL_ATTR_LOGIN_TIMEOUT,
      reinterpret_cast<void*>(std::uintptr_t{15}), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(hdbc_, SQL_ATTR_CONNECTION_TIMEOUT,
      reinterpret_cast<void*>(std::uintptr_t{15}), 0));
  ASSERT_TRUE(connect()); // Fixture applies the bounded query timeout.

  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT CAST(current_user AS VARCHAR(1024)), 7::integer, NULL::integer")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  std::array<char, 1024> principal{};
  SQLLEN length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_CHAR, principal.data(),
      static_cast<SQLLEN>(principal.size()), &length));
  ASSERT_TRUE(length == static_cast<SQLLEN>(expected_user.size()));
  ASSERT_TRUE(std::string_view(principal.data(), expected_user.size()) == expected_user);
  SQLINTEGER value = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_LONG, &value, sizeof(value), &length));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
  EXPECT_EQ(7, value);
  value = 83; length = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 3, SQL_C_LONG, &value, sizeof(value), &length));
  EXPECT_EQ(SQL_NULL_DATA, length);
  EXPECT_EQ(83, value);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, hstmt_));
  hstmt_ = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(hdbc_));
  connected_ = false;
}


// These finite type cases are not part of the admitted pilot inventory merely
// because they compile. Live qualification requires a separately reviewed batch.
class RedshiftNumericResultBoundaryRealTest : public RedshiftRealTest {
protected:
  void SetUp() override {
    const char* marker = std::getenv("ODBCPP_REDSHIFT_NUMERIC_RESULT_ADMISSION");
    if (marker == nullptr) {
      GTEST_SKIP() << "Numeric result boundary scope is not admitted";
    }
    ASSERT_TRUE(std::string_view(marker) == "numeric-result-boundary-v1")
        << "Invalid numeric result boundary scope marker";
    // FIRST gate, before configuration, handles or connection work.
    RedshiftRealTest::SetUp();
  }
};

TEST_F(RedshiftNumericResultBoundaryRealTest, IntegerBoundariesAndNarrowing) {
  ASSERT_TRUE(connect());
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT CAST('-9223372036854775808' AS BIGINT), "
                        "CAST('9223372036854775807' AS BIGINT), "
                        "CAST(32768 AS INTEGER)")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  const SQLBIGINT expected[]{std::numeric_limits<SQLBIGINT>::min(),
                             std::numeric_limits<SQLBIGINT>::max()};
  for (SQLUSMALLINT column = 1; column <= 2; ++column) {
    SQLBIGINT value{}; SQLLEN length{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, SQL_C_SBIGINT,
        &value, sizeof(value), &length));
    EXPECT_EQ(expected[column - 1], value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
  }
  if (HasFailure()) return;
  SQLSMALLINT sentinel = 17; SQLLEN length = 93;
  EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt_, 3, SQL_C_SSHORT,
      &sentinel, sizeof(sentinel), &length));
  EXPECT_EQ("22003", get_error(SQL_HANDLE_STMT, hstmt_));
  EXPECT_EQ(17, sentinel);
  EXPECT_EQ(93, length);
  if (HasFailure()) return;
  // A failed narrowing conversion must not consume the current cell.
  SQLINTEGER widened = -1;
  SQLLEN widened_length = 93;
  const auto retry = SQLGetData(hstmt_, 3, SQL_C_SLONG, &widened,
                               sizeof(widened), &widened_length);
  SCOPED_TRACE("column=3 target=C_SLONG SQLSTATE=" +
               get_error(SQL_HANDLE_STMT, hstmt_));
  ASSERT_EQ(SQL_SUCCESS, retry);
  EXPECT_EQ(32768, widened);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(widened)), widened_length);
  if (HasFailure()) return;
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  if (HasFailure()) return;
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_))
      << get_error(SQL_HANDLE_STMT, hstmt_);

  // The same connection and statement remain usable after local 22003.
  SQLCHAR recovery_query[] = "SELECT 1";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, recovery_query, SQL_NTS))
      << get_error(SQL_HANDLE_STMT, hstmt_);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_))
      << get_error(SQL_HANDLE_STMT, hstmt_);
  SQLINTEGER recovered = -1;
  SQLLEN recovered_length = 93;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_SLONG, &recovered,
                                  sizeof(recovered), &recovered_length))
      << get_error(SQL_HANDLE_STMT, hstmt_);
  EXPECT_EQ(1, recovered);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(recovered)), recovered_length);
  if (HasFailure()) return;
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_))
      << get_error(SQL_HANDLE_STMT, hstmt_);
}

TEST_F(RedshiftNumericResultBoundaryRealTest, ExactDecimalAndNull) {
  ASSERT_TRUE(connect());
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT CAST('123.45' AS DECIMAL(5,2)), "
                        "CAST('-123.45' AS DECIMAL(5,2)), "
                        "CAST('99999999999999999999999999999999999999' AS DECIMAL(38,0)), "
                        "CAST(NULL AS DECIMAL(5,2))")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  SQLHDESC ard = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt_, SQL_ATTR_APP_ROW_DESC,
      &ard, 0, nullptr));
  const std::array<unsigned char, 16> maximum_magnitude{
      255, 255, 255, 255, 63, 34, 138, 9, 122, 196, 134, 90, 168, 76, 59, 75};
  for (SQLSMALLINT column = 1; column <= 4; ++column) {
    const std::uintptr_t precision = column == 3 ? 38 : 5;
    const std::uintptr_t scale = column == 3 ? 0 : 2;
    SQLCHAR name[64]{}; SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt_, static_cast<SQLUSMALLINT>(column),
        name, sizeof(name), nullptr, &type, &size, &digits, &nullable));
    EXPECT_EQ(SQL_NUMERIC, type);
    EXPECT_EQ(precision, size);
    EXPECT_EQ(scale, digits);
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, column, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_C_NUMERIC}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, column, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(precision), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, column, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(scale), 0));
    SQL_NUMERIC_STRUCT value;
    std::memset(&value, 0x5a, sizeof(value));
    SQLLEN length = 93;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, static_cast<SQLUSMALLINT>(column),
        SQL_ARD_TYPE, &value, sizeof(value), &length));
    if (column == 4) {
      EXPECT_EQ(SQL_NULL_DATA, length);
      EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&value),
          reinterpret_cast<const unsigned char*>(&value) + sizeof(value),
          [](unsigned char byte) { return byte == 0x5a; }));
      continue;
    }
    EXPECT_EQ(precision, value.precision);
    EXPECT_EQ(scale, value.scale);
    EXPECT_EQ(column == 2 ? 0 : 1, value.sign);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    std::array<unsigned char, 16> magnitude{};
    if (column == 3) magnitude = maximum_magnitude;
    else { magnitude[0] = 0x39; magnitude[1] = 0x30; }
    EXPECT_TRUE(std::equal(magnitude.begin(), magnitude.end(), value.val));
  }
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
}

TEST_F(RedshiftRealTest, UnicodeRoundTrip) {
  ASSERT_TRUE(connect());
  const std::string expected = "Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x99\x82";
  // Escapes above are UTF-8 bytes, independent of compiler source encoding.
  std::vector<SQLWCHAR> wide{'G', 'r', 0xfc, 0xdf, 'e', ' '};
  if constexpr (sizeof(SQLWCHAR) == 2) {
    wide.push_back(0xd83d); wide.push_back(0xde42);
  } else {
    static_assert(sizeof(SQLWCHAR) == 2 || sizeof(SQLWCHAR) == 4);
    wide.push_back(static_cast<SQLWCHAR>(0x1f642));
  }
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT CAST(? AS VARCHAR(64)), CAST(? AS VARCHAR(64))")), SQL_NTS));
  SQLLEN input_length = static_cast<SQLLEN>(expected.size());
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 64, 0, const_cast<char*>(expected.data()),
      static_cast<SQLLEN>(expected.size()), &input_length));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 2, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 64, 0, const_cast<char*>(expected.data()),
      static_cast<SQLLEN>(expected.size()), &input_length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  char narrow[64]{}; SQLLEN length{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_CHAR, narrow, sizeof(narrow), &length));
  EXPECT_EQ(expected, narrow);
  EXPECT_EQ(12, length);
  SQLWCHAR output[32]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_WCHAR, output, sizeof(output), &length));
  EXPECT_EQ(static_cast<SQLLEN>(wide.size() * sizeof(SQLWCHAR)), length);
  EXPECT_TRUE(std::equal(wide.begin(), wide.end(), output));
  EXPECT_EQ(SQLWCHAR{}, output[wide.size()]);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
}

TEST_F(RedshiftRealTest, TemporalExactAndNull) {
  ASSERT_TRUE(connect());
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT CAST('2024-02-29' AS DATE), "
                        "CAST('2024-02-29 12:34:56.123456' AS TIMESTAMP), "
                        "CAST('12:34:56.123456' AS TIME), CAST(NULL AS TIMESTAMP)")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  SQLLEN length{}; SQL_DATE_STRUCT date{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_TYPE_DATE, &date, sizeof(date), &length));
  EXPECT_EQ(2024, date.year); EXPECT_EQ(2, date.month); EXPECT_EQ(29, date.day);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), length);
  SQL_TIMESTAMP_STRUCT timestamp{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_TYPE_TIMESTAMP,
      &timestamp, sizeof(timestamp), &length));
  EXPECT_EQ(2024, timestamp.year); EXPECT_EQ(2, timestamp.month); EXPECT_EQ(29, timestamp.day);
  EXPECT_EQ(12, timestamp.hour); EXPECT_EQ(34, timestamp.minute); EXPECT_EQ(56, timestamp.second);
  EXPECT_EQ(123456000u, timestamp.fraction);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), length);
  SQL_TIME_STRUCT time{};
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt_, 3, SQL_C_TYPE_TIME,
      &time, sizeof(time), &length));
  EXPECT_EQ("01S07", get_error(SQL_HANDLE_STMT, hstmt_));
  EXPECT_EQ(12, time.hour); EXPECT_EQ(34, time.minute); EXPECT_EQ(56, time.second);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(time)), length);
  std::memset(&timestamp, 0x5a, sizeof(timestamp));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 4, SQL_C_TYPE_TIMESTAMP,
      &timestamp, sizeof(timestamp), &length));
  EXPECT_EQ(SQL_NULL_DATA, length);
  EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&timestamp),
      reinterpret_cast<const unsigned char*>(&timestamp) + sizeof(timestamp),
      [](unsigned char byte) { return byte == 0x5a; }));
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
}

TEST_F(RedshiftRealTest, TypedNullAndOutputPreservation) {
  ASSERT_TRUE(connect());
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(
      const_cast<char*>("SELECT CAST(NULL AS INTEGER), CAST(NULL AS DECIMAL(5,2)), "
                        "CAST(NULL AS VARCHAR(8)), CAST(NULL AS TIMESTAMP), "
                        "CAST('' AS VARCHAR(8))")), SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  const SQLSMALLINT targets[]{SQL_C_SLONG, SQL_C_NUMERIC, SQL_C_CHAR, SQL_C_TYPE_TIMESTAMP};
  for (SQLUSMALLINT column = 1; column <= 4; ++column) {
    std::array<unsigned char, 64> output;
    output.fill(0x5a); SQLLEN length = 93;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, column, targets[column - 1],
        output.data(), static_cast<SQLLEN>(output.size()), &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(),
        [](unsigned char byte) { return byte == 0x5a; }));
  }
  char empty[8]; std::memset(empty, 0x5a, sizeof(empty)); SQLLEN length = 93;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 5, SQL_C_CHAR, empty, sizeof(empty), &length));
  EXPECT_EQ(0, length); EXPECT_EQ('\0', empty[0]);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
}


class RedshiftCatalogVarbyteRealTest : public RedshiftRealTest {
protected:
  void SetUp() override {
    const char* admission=std::getenv("ODBCPP_REDSHIFT_CATALOG_VARBYTE_ADMISSION");
    if(admission==nullptr) { GTEST_SKIP() << "Catalog/VARBYTE no-DDL scope is not admitted"; }
    ASSERT_TRUE(std::string_view(admission)=="catalog-varbyte-no-ddl-v1")
        << "Invalid catalog/VARBYTE no-DDL scope marker";
    // Scope selection only. The future owner-reviewed runner supplies authority.
    RedshiftRealTest::SetUp();
  }
};

// Future fixed three-case inventory: ConnectionTest plus these two cases.
// No existing launcher/profile admits them; compiling this source grants no SQL.
// Static DDL advertisement and generic result-family labels are not native maxima.
TEST_F(RedshiftCatalogVarbyteRealTest, ContemporaryDdlTypeInfoPolicyContract) {
  ASSERT_TRUE(connect());
  SQLCHAR identity_sql[]="SELECT current_database(),TRIM(current_user)";
  ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,identity_sql,SQL_NTS));
  SQLSMALLINT identity_columns=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&identity_columns));ASSERT_EQ(2,identity_columns);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  for(SQLUSMALLINT column=1;column<=2;++column) {
    const char* expected=column==1?"odbcpp_pilot":"odbcpp_pilot_test";
    std::array<char,64> actual{};SQLLEN length=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,actual.data(),actual.size(),&length));
    ASSERT_GE(length,0);ASSERT_LT(length,static_cast<SQLLEN>(actual.size()));
    ASSERT_TRUE(static_cast<std::size_t>(length)==std::strlen(expected));
    ASSERT_TRUE(std::memcmp(actual.data(),expected,static_cast<std::size_t>(length)+1)==0);
  }
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  const CatalogField fields[]{
      {"type_name",SQL_VARCHAR},{"data_type",SQL_SMALLINT},{"column_size",SQL_INTEGER},
      {"literal_prefix",SQL_VARCHAR},{"literal_suffix",SQL_VARCHAR},{"create_params",SQL_VARCHAR},
      {"nullable",SQL_SMALLINT},{"case_sensitive",SQL_SMALLINT},{"searchable",SQL_SMALLINT},
      {"unsigned_attribute",SQL_SMALLINT},{"fixed_prec_scale",SQL_SMALLINT},{"auto_unique_value",SQL_SMALLINT},
      {"local_type_name",SQL_VARCHAR},{"minimum_scale",SQL_SMALLINT},{"maximum_scale",SQL_SMALLINT},
      {"sql_data_type",SQL_SMALLINT},{"sql_datetime_sub",SQL_SMALLINT},{"num_prec_radix",SQL_INTEGER},
      {"interval_precision",SQL_SMALLINT}};
  struct Expected {SQLSMALLINT type;const char* name;SQLINTEGER size;bool quoted,sensitive;};
  const Expected cases[]{{SQL_CHAR,"char",4096,true,true},{SQL_VARCHAR,"varchar",65535,true,true},
                         {SQL_LONGVARBINARY,"varbyte",16777216,false,false}};
  for(const auto& item:cases) {
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLGetTypeInfo(hstmt_,item.type));expect_catalog_fields(fields);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));expect_catalog_text(1,item.name);ASSERT_FALSE(HasFailure());
    expect_catalog_integer(2,item.type);expect_catalog_integer(3,item.size);ASSERT_FALSE(HasFailure());
    if(item.quoted) { expect_catalog_text(4,"'");expect_catalog_text(5,"'"); }
    else { expect_catalog_null(4);expect_catalog_null(5); }
    ASSERT_FALSE(HasFailure());expect_catalog_text(6,"length");expect_catalog_integer(7,SQL_NULLABLE);
    expect_catalog_integer(8,item.sensitive?SQL_TRUE:SQL_FALSE);
    expect_catalog_integer(9,item.quoted?SQL_SEARCHABLE:SQL_PRED_BASIC);ASSERT_FALSE(HasFailure());
    for(const SQLUSMALLINT field:{SQLUSMALLINT{10},SQLUSMALLINT{12},SQLUSMALLINT{13},SQLUSMALLINT{14},SQLUSMALLINT{15},SQLUSMALLINT{17},SQLUSMALLINT{18},SQLUSMALLINT{19}}) {
      expect_catalog_null(field);ASSERT_FALSE(HasFailure());
    }
    expect_catalog_integer(11,SQL_FALSE);expect_catalog_integer(16,item.type);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  }
  ASSERT_FALSE(HasFailure());
  std::array<char,32> owned{};SQLLEN length=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetTypeInfo(hstmt_,SQL_VARCHAR));ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_CHAR,owned.data(),owned.size(),&length));ASSERT_EQ(7,length);
  const auto snapshot=owned;ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  for(const SQLSMALLINT removed:{SQLSMALLINT{SQL_BINARY},SQLSMALLINT{SQL_VARBINARY},SQLSMALLINT{SQL_LONGVARCHAR}}) {
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLGetTypeInfo(hstmt_,removed));expect_catalog_fields(fields);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));EXPECT_EQ(snapshot,owned);
  }
  RecordProperty("type_info_policy","contemporary_static_ddl_not_native_maximum");
}

TEST_F(RedshiftCatalogVarbyteRealTest, VarbyteConstantDirectPreparedResultContract) {
  ASSERT_TRUE(connect());
  SQLCHAR identity_sql[]="SELECT current_database(),TRIM(current_user)";
  ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,identity_sql,SQL_NTS));
  SQLSMALLINT identity_columns=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&identity_columns));ASSERT_EQ(2,identity_columns);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  for(SQLUSMALLINT column=1;column<=2;++column) {
    const char* expected=column==1?"odbcpp_pilot":"odbcpp_pilot_test";
    std::array<char,64> actual{};SQLLEN length=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,actual.data(),actual.size(),&length));
    ASSERT_GE(length,0);ASSERT_LT(length,static_cast<SQLLEN>(actual.size()));
    ASSERT_TRUE(static_cast<std::size_t>(length)==std::strlen(expected));
    ASSERT_TRUE(std::memcmp(actual.data(),expected,static_cast<std::size_t>(length)+1)==0);
  }
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  SQLCHAR query[]="SELECT FROM_HEX('0041ff') AS b, FROM_HEX('0041ff') AS copy_b, CAST(NULL AS VARBYTE) AS null_b";
  const std::array<unsigned char,3> expected{0x00,0x41,0xff};
  const char* aliases[]{"b","copy_b","null_b"};
  auto metadata=[&] {
    SQLSMALLINT count=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(3,count);
    SQLHDESC ird=nullptr;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_IMP_ROW_DESC,&ird,0,nullptr));ASSERT_NE(nullptr,ird);
    for(SQLUSMALLINT column=1;column<=3;++column) {
      char alias[16]{};SQLSMALLINT alias_length=-1,type=-1,scale=-1,nullable=-1;SQLULEN size=99;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,reinterpret_cast<SQLCHAR*>(alias),sizeof(alias),&alias_length,&type,&size,&scale,&nullable));
      ASSERT_EQ(std::strlen(aliases[column-1]),static_cast<std::size_t>(alias_length));EXPECT_EQ(0,std::memcmp(alias,aliases[column-1],static_cast<std::size_t>(alias_length)+1));
      EXPECT_EQ(SQL_LONGVARBINARY,type);EXPECT_EQ(0u,size);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
      char name[16]{};SQLSMALLINT name_length=-1;ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(hstmt_,column,SQL_DESC_TYPE_NAME,name,sizeof(name),&name_length,nullptr));
      ASSERT_EQ(7,name_length);EXPECT_EQ(0,std::memcmp(name,"varbyte",8));
      SQLINTEGER descriptor_length=-1;ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,column,SQL_DESC_TYPE_NAME,name,sizeof(name),&descriptor_length));ASSERT_EQ(7,descriptor_length);EXPECT_EQ(0,std::memcmp(name,"varbyte",8));
      SQLULEN descriptor_size=99;ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,column,SQL_DESC_LENGTH,&descriptor_size,0,nullptr));EXPECT_EQ(0u,descriptor_size);
      for(const SQLUSMALLINT field:{SQLUSMALLINT{SQL_DESC_OCTET_LENGTH},SQLUSMALLINT{SQL_DESC_DISPLAY_SIZE}}) {
        SQLLEN value=99;ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(hstmt_,column,field,nullptr,0,nullptr,&value));EXPECT_EQ(SQL_NO_TOTAL,value);
        ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,column,field,&value,0,nullptr));EXPECT_EQ(SQL_NO_TOTAL,value);
      }
    }
  };
  ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,query,SQL_NTS));metadata();ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  std::array<unsigned char,4> chunk{0x5a,0x5a,0x5a,0x5a};SQLLEN length=-1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLGetData(hstmt_,1,SQL_C_BINARY,chunk.data(),2,&length));ASSERT_EQ("01004",get_error(SQL_HANDLE_STMT,hstmt_));
  EXPECT_EQ(3,length);EXPECT_EQ(0x00,chunk[0]);EXPECT_EQ(0x41,chunk[1]);EXPECT_EQ(0x5a,chunk[2]);EXPECT_EQ(0x5a,chunk[3]);
  chunk.fill(0x5a);ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_BINARY,chunk.data(),2,&length));EXPECT_EQ(1,length);EXPECT_EQ(0xff,chunk[0]);EXPECT_EQ(0x5a,chunk[1]);EXPECT_EQ(0x5a,chunk[2]);EXPECT_EQ(0x5a,chunk[3]);
  ASSERT_EQ(SQL_NO_DATA,SQLGetData(hstmt_,1,SQL_C_BINARY,chunk.data(),2,&length));
  std::array<unsigned char,4> owned{0x5a,0x5a,0x5a,0x5a};ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,2,SQL_C_BINARY,owned.data(),3,&length));
  EXPECT_EQ(3,length);EXPECT_EQ(0,std::memcmp(owned.data(),expected.data(),expected.size()));EXPECT_EQ(0x5a,owned[3]);const auto snapshot=owned;
  chunk.fill(0x5a);ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,3,SQL_C_BINARY,chunk.data(),3,&length));EXPECT_EQ(SQL_NULL_DATA,length);EXPECT_TRUE(std::all_of(chunk.begin(),chunk.end(),[](auto byte){return byte==0x5a;}));
  metadata();ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,query,SQL_NTS));
  SQLULEN fetched=99;SQLUSMALLINT row_status=SQL_ROW_NOROW;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,&fetched,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,&row_status,0));
  std::array<unsigned char,4> first{},copy{},null_buffer{};SQLLEN first_length=-1,copy_length=-1,null_length=-1;
  for(const bool truncate:{true,false}) {
    ASSERT_FALSE(HasFailure());
    first.fill(0x5a);copy.fill(0x5a);null_buffer.fill(0x5a);fetched=99;row_status=SQL_ROW_NOROW;
    ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_BINARY,first.data(),truncate?2:3,&first_length));
    ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,2,SQL_C_BINARY,copy.data(),3,&copy_length));ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,3,SQL_C_BINARY,null_buffer.data(),3,&null_length));
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_));metadata();ASSERT_FALSE(HasFailure());
    ASSERT_EQ(truncate?SQL_SUCCESS_WITH_INFO:SQL_SUCCESS,SQLFetch(hstmt_));ASSERT_EQ(1u,fetched);ASSERT_EQ(truncate?SQL_ROW_SUCCESS_WITH_INFO:SQL_ROW_SUCCESS,row_status);
    if(truncate) { ASSERT_EQ("01004",get_error(SQL_HANDLE_STMT,hstmt_)); }
    EXPECT_EQ(3,first_length);EXPECT_EQ(3,copy_length);EXPECT_EQ(SQL_NULL_DATA,null_length);
    EXPECT_EQ(0,std::memcmp(first.data(),expected.data(),truncate?2:3));EXPECT_EQ(0x5a,first[truncate?2:3]);EXPECT_EQ(0x5a,first[3]);
    EXPECT_EQ(0,std::memcmp(copy.data(),expected.data(),expected.size()));EXPECT_EQ(0x5a,copy[3]);EXPECT_TRUE(std::all_of(null_buffer.begin(),null_buffer.end(),[](auto byte){return byte==0x5a;}));EXPECT_EQ(snapshot,owned);
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
    ASSERT_FALSE(HasFailure());
  }
  RecordProperty("result_type_name_policy","varbyte_generic_family_fallback_not_server_native_name");
  RecordProperty("descriptor_size_policy","unknown_zero_octet_display_no_total_not_actual_cell_length");
}

// Prospective owning text-parameter checkpoint, separate from raw C_BINARY
// VARBYTE input support and from the already qualified constant-result scope.
class RedshiftVarbyteTextParameterRealTest : public RedshiftRealTest {
protected:
  void SetUp() override {
    const char* admission=std::getenv("ODBCPP_REDSHIFT_VARBYTE_TEXT_PARAMETER_ADMISSION");
    if(admission==nullptr) { GTEST_SKIP() << "Prepared hex-text VARBYTE scope is not admitted"; }
    ASSERT_TRUE(std::string_view(admission)=="varbyte-hex-text-parameter-v1")
        << "Invalid prepared hex-text VARBYTE scope marker";
    // FIRST gate, before base configuration, handles or connection work.
    RedshiftRealTest::SetUp();
  }
};

TEST_F(RedshiftVarbyteTextParameterRealTest, PreparedHexTextToVarbyteBytes) {
  ASSERT_TRUE(connect());
  SQLCHAR identity_sql[]="SELECT current_database(),TRIM(current_user)";
  ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,identity_sql,SQL_NTS));
  SQLSMALLINT count=0;
  ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(2,count);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  for(SQLUSMALLINT column=1;column<=2;++column) {
    const char* expected=column==1?"odbcpp_pilot":"odbcpp_pilot_test";
    std::array<char,64> actual{};SQLLEN length=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,actual.data(),actual.size(),&length));
    ASSERT_GE(length,0);ASSERT_LT(length,static_cast<SQLLEN>(actual.size()));
    ASSERT_TRUE(static_cast<std::size_t>(length)==std::strlen(expected));
    ASSERT_TRUE(std::memcmp(actual.data(),expected,static_cast<std::size_t>(length)+1)==0);
  }
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  SQLCHAR query[]="SELECT FROM_HEX(?) AS b";
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,query,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
  SQLSMALLINT parameters=0;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&parameters));ASSERT_EQ(1,parameters);
  const std::array<unsigned char,3> expected{0x00,0x41,0xff};
  std::array<unsigned char,5> owned{};
  for(unsigned trial=0;trial<3;++trial) {
    ASSERT_FALSE(HasFailure());
    SCOPED_TRACE(trial);
    std::array<char,6> input{'0','0','4','1','f','f'};
    SQLLEN input_length=6;
    if(trial!=0) { input.fill('!');input_length=trial==1?0:SQL_NULL_DATA; }
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_CHAR,
        SQL_VARCHAR,6,0,input.data(),input.size(),&input_length));
    ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
    input.fill('!'); // An owning parameter/result must outlive caller mutation.
    ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(1,count);
    char name[8]{};SQLSMALLINT name_length=-1,type=-1,scale=-1,nullable=-1;SQLULEN size=99;
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,1,reinterpret_cast<SQLCHAR*>(name),sizeof(name),
        &name_length,&type,&size,&scale,&nullable));
    EXPECT_EQ(1,name_length);EXPECT_EQ(0,std::memcmp(name,"b",2));
    EXPECT_EQ(SQL_LONGVARBINARY,type);EXPECT_EQ(0u,size);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
    // Selected unknown-size result-family policy, not native parameter provenance.
    for(const SQLUSMALLINT field:{SQLUSMALLINT{SQL_DESC_OCTET_LENGTH},SQLUSMALLINT{SQL_DESC_DISPLAY_SIZE}}) {
      SQLLEN value=99;
      ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(hstmt_,1,field,nullptr,0,nullptr,&value));
      EXPECT_EQ(SQL_NO_TOTAL,value);
    }
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    std::array<unsigned char,5> output{0x5a,0x5a,0x5a,0x5a,0x5a};SQLLEN length=99;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_BINARY,output.data(),3,&length));
    if(trial==0) {
      EXPECT_EQ(3,length);EXPECT_EQ(0,std::memcmp(output.data(),expected.data(),expected.size()));
      EXPECT_EQ(0x5a,output[3]);EXPECT_EQ(0x5a,output[4]);owned=output;
    } else {
      EXPECT_EQ(trial==1?0:SQL_NULL_DATA,length);
      EXPECT_TRUE(std::all_of(output.begin(),output.end(),[](auto byte){return byte==0x5a;}));
      EXPECT_EQ(0,std::memcmp(owned.data(),expected.data(),expected.size()));
      EXPECT_EQ(0x5a,owned[3]);EXPECT_EQ(0x5a,owned[4]);
    }
    ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));
    ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_)); // Exactly one result, no extras.
    // MoreResults already closes the result; SQL_CLOSE is the idempotent close.
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
  }
  ASSERT_FALSE(HasFailure());
  SQLCHAR recovery[]="SELECT 1";
  ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,recovery,SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(1,count);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  SQLINTEGER scalar=-99;SQLLEN scalar_length=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&scalar,sizeof(scalar),&scalar_length));
  EXPECT_EQ(1,scalar);EXPECT_EQ(static_cast<SQLLEN>(sizeof(scalar)),scalar_length);
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
}

// Representative discovery -> metadata -> prepared application workflow.
// SHOW/LEGACY currently selects PRIMARY KEYS only: SQLTables still uses
// information_schema for tables, SVV_REDSHIFT_SCHEMAS for schema enumeration,
// and SVV_COLUMNS for columns. These cases do not claim modern
// SHOW TABLES/COLUMNS routing or grant fixture/setup authority.
class RedshiftMetadataWorkflowRealTest : public RedshiftRealTest {
protected:
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_METADATA_WORKFLOW_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Metadata workflow scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="metadata-prepared-workflow-v1");
    RedshiftRealTest::SetUp();
  }
  static std::string quoted(const std::string& identifier) {
    std::string output="\"";
    for(char c:identifier) { if(c=='\"') { output+='\"'; } output+=c; }
    return output+'\"';
  }
  enum class DescriptorScope { Tables, AllSchemas, ShowSchemas, Columns };
  struct DescriptorSnapshot {
    std::string name;
    SQLSMALLINT type,digits,nullable;
    SQLULEN size;
    bool operator==(const DescriptorSnapshot&) const = default;
  };
  std::vector<DescriptorSnapshot> catalog_descriptors_;
  void descriptors(std::span<const CatalogField> fields,
                   DescriptorScope scope=DescriptorScope::Tables) {
    SQLSMALLINT count=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));
    ASSERT_EQ(fields.size(),static_cast<std::size_t>(count));
    catalog_descriptors_.clear();
    for(SQLUSMALLINT column=1;column<=fields.size();++column) {
      SCOPED_TRACE(fields[column-1].name); // Closed field label, never returned data.
      std::array<SQLCHAR,64> name{};SQLSMALLINT length=-1,type=-1,digits=-1,nullable=-1;SQLULEN size=99;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name.data(),name.size(),&length,&type,&size,&digits,&nullable));
      ASSERT_GE(length,0);ASSERT_LT(length,static_cast<SQLSMALLINT>(name.size()));
      std::string actual(reinterpret_cast<const char*>(name.data()),static_cast<std::size_t>(length));
      if(scope==DescriptorScope::ShowSchemas) {
        const std::array<std::string_view,5> names{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","TABLE_TYPE","REMARKS"};
        ASSERT_LE(column,names.size());EXPECT_EQ(names[column-1],actual);
      }
      std::transform(actual.begin(),actual.end(),actual.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
      EXPECT_TRUE(actual==fields[column-1].name);EXPECT_EQ(fields[column-1].type,type);
      EXPECT_EQ(0,digits);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
      if(type==SQL_SMALLINT) { EXPECT_EQ(5U,size); }
      else if(type==SQL_INTEGER) { EXPECT_EQ(10U,size); }
      else if(type==SQL_VARCHAR) {
        if(scope==DescriptorScope::ShowSchemas) {
          // Backend preserves SHOW's schema capacity; synthetic NULL fields
          // have unknown capacity0. No legacy SVV typmod is inherited.
          if(column==2) { EXPECT_LE(size,65535U); }
          else { EXPECT_EQ(0U,size); }
        } else if(scope==DescriptorScope::AllSchemas) {
          // Selected schema enumeration: SVV schema_name retains VARCHAR(128).
          const std::array<SQLULEN,5> widths{65535,128,65535,65535,65535};
          ASSERT_LE(column,widths.size());EXPECT_EQ(widths[column-1],size);
        } else if(scope==DescriptorScope::Tables) {
          // This selected database expression is12; TABLE_TYPE's CASE is15.
          // Catalog header capacity is separate from a user column's size.
          const std::array<SQLULEN,5> widths{12,65535,65535,15,65535};
          ASSERT_LE(column,widths.size());EXPECT_EQ(widths[column-1],size);
        } else {
          // SVV_COLUMNS text has no prescribed projected typmod here. Keep
          // its declared bounded capacity (0 means unknown), not guessed65535.
          EXPECT_LE(size,65535U);
        }
      } else { ADD_FAILURE() << "Unexpected catalog descriptor type"; }
      catalog_descriptors_.push_back({std::move(actual),type,digits,nullable,size});
    }
  }
  void text_capacity(SQLUSMALLINT column,std::string_view value) {
    if(catalog_descriptors_.empty()) { return; }
    ASSERT_GE(column,1);ASSERT_LE(column,catalog_descriptors_.size());
    const auto& descriptor=catalog_descriptors_[column-1];
    EXPECT_EQ(SQL_VARCHAR,descriptor.type);
    // Selected identity/fixture metadata strings are ASCII, so byte length
    // also measures characters. Opaque non-ASCII remarks keep their own bound.
    EXPECT_TRUE(std::all_of(value.begin(),value.end(),[](unsigned char c){return c<128;}));
    if(descriptor.size!=0) { EXPECT_LE(value.size(),descriptor.size); }
  }
  std::string text(SQLUSMALLINT column) {
    std::array<unsigned char,1026> buffer{};buffer.fill(0x5a);SQLLEN length=-1;
    const auto result=SQLGetData(hstmt_,column,SQL_C_CHAR,buffer.data(),1025,&length);
    EXPECT_EQ(SQL_SUCCESS,result);EXPECT_GE(length,0);EXPECT_LT(length,1025);
    if(result!=SQL_SUCCESS||length<0||length>=1025) { return {}; }
    EXPECT_EQ(0,buffer[static_cast<std::size_t>(length)]);EXPECT_EQ(0x5a,buffer[1025]);
    std::string value(reinterpret_cast<const char*>(buffer.data()),static_cast<std::size_t>(length));
    text_capacity(column,value);
    return value;
  }
  void number(SQLUSMALLINT column,SQLINTEGER expected) {
    struct {SQLINTEGER before{17},value{-99},after{83};} output;
    SQLLEN length=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_SLONG,&output.value,sizeof(output.value),&length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(output.value)),length);EXPECT_EQ(expected,output.value);
    EXPECT_EQ(17,output.before);EXPECT_EQ(83,output.after);
  }
  void null(SQLUSMALLINT column) {
    std::array<unsigned char,16> buffer{};buffer.fill(0x5a);SQLLEN length=99;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,buffer.data(),buffer.size(),&length));
    EXPECT_EQ(SQL_NULL_DATA,length);
    EXPECT_TRUE(std::all_of(buffer.begin(),buffer.end(),[](auto byte){return byte==0x5a;}));
  }
  void remarks(SQLUSMALLINT column) {
    // SVV remarks are opaque optional bounded text; no unproved NULL/empty rule.
    std::array<unsigned char,1026> buffer{};buffer.fill(0x5a);SQLLEN length=99;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,buffer.data(),1025,&length));
    if(length==SQL_NULL_DATA) {
      EXPECT_TRUE(std::all_of(buffer.begin(),buffer.end(),[](auto byte){return byte==0x5a;}));
    } else {
      ASSERT_GE(length,0);ASSERT_LT(length,1025);
      EXPECT_EQ(0,buffer[static_cast<std::size_t>(length)]);EXPECT_EQ(0x5a,buffer[1025]);
      ASSERT_GE(column,1);ASSERT_LE(column,catalog_descriptors_.size());
      const auto& descriptor=catalog_descriptors_[column-1];
      if(descriptor.size!=0&&std::all_of(buffer.begin(),buffer.begin()+length,[](auto c){return c<128;})) {
        EXPECT_LE(static_cast<SQLULEN>(length),descriptor.size);
      }
    }
  }
  void workflow(const char* mode) {
    ASSERT_TRUE(std::string_view(mode)=="SHOW"||std::string_view(mode)=="LEGACY");
    const auto base=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_FALSE(base.contains("REDSHIFTCATALOGMODE"));ASSERT_FALSE(base.contains("DSN"));
    for(const char* key:{"SERVER","PORT","DATABASE","UID","PWD","SSLCAFILE"}) {
      ASSERT_TRUE(base.contains(key));ASSERT_TRUE(!base.at(key).empty());ASSERT_TRUE(base.at(key).find('\0')==std::string::npos);
    }
    ASSERT_TRUE(base.at("DATABASE")=="odbcpp_pilot");ASSERT_TRUE(base.at("UID")=="odbcpp_pilot_test");ASSERT_TRUE(base.at("PORT")=="5439");
    const char* schema_env=std::getenv("ODBCPP_REDSHIFT_TEST_SCHEMA");
    const char* table_env=std::getenv("ODBCPP_REDSHIFT_TEST_TABLE");
    ASSERT_NE(nullptr,schema_env);ASSERT_NE(nullptr,table_env);
    std::string schema,table;
    for(const auto& pair:{std::pair{schema_env,&schema},std::pair{table_env,&table}}) {
      std::size_t count=0;while(count<=128&&pair.first[count]!='\0') { ++count; }
      ASSERT_GT(count,0U);ASSERT_LE(count,128U);
      for(std::size_t i=0;i<count;++i) {
        const unsigned char c=static_cast<unsigned char>(pair.first[i]);
        ASSERT_TRUE((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_');
      }
      pair.second->assign(pair.first,count);
    }
    ASSERT_TRUE(schema=="odbcpp_fixture");
    connection_string_+=";RedshiftCatalogMode=";connection_string_+=mode;connection_string_+=';';
    ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
    SQLCHAR identity[]="SELECT current_database(),TRIM(current_user)";
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,identity,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    SQLSMALLINT count=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(2,count);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));EXPECT_TRUE(text(1)=="odbcpp_pilot");EXPECT_TRUE(text(2)=="odbcpp_pilot_test");
    ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
    const CatalogField tables[]{{"table_cat",SQL_VARCHAR},{"table_schem",SQL_VARCHAR},{"table_name",SQL_VARCHAR},{"table_type",SQL_VARCHAR},{"remarks",SQL_VARCHAR}};
    SQLCHAR empty[]="";SQLCHAR all_schemas[]=SQL_ALL_SCHEMAS;
    ASSERT_EQ(SQL_SUCCESS,SQLTables(hstmt_,empty,SQL_NTS,all_schemas,SQL_NTS,empty,SQL_NTS,nullptr,0)) << schema_catalog_diagnostic();
    descriptors(tables,std::string_view(mode)=="SHOW" ? DescriptorScope::ShowSchemas : DescriptorScope::AllSchemas);ASSERT_FALSE(HasFailure());
    unsigned selected_schemas=0;bool exhausted=false;
    for(unsigned row=0;row<=64;++row) {
      const auto result=SQLFetch(hstmt_);if(result==SQL_NO_DATA) { exhausted=true;break; }
      ASSERT_EQ(SQL_SUCCESS,result);ASSERT_LT(row,64U);
      null(1);const auto discovered=text(2);null(3);null(4);null(5);ASSERT_FALSE(HasFailure());
      if(discovered==schema) { ++selected_schemas; }
    }
    ASSERT_TRUE(exhausted);ASSERT_EQ(1U,selected_schemas);ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
    auto bytes=[](const std::string& value){return reinterpret_cast<SQLCHAR*>(const_cast<char*>(value.c_str()));};
    SQLCHAR database[]="odbcpp_pilot";SQLCHAR table_type[]="TABLE";
    ASSERT_EQ(SQL_SUCCESS,SQLTables(hstmt_,database,SQL_NTS,bytes(schema),SQL_NTS,bytes(table),SQL_NTS,table_type,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    descriptors(tables);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    EXPECT_TRUE(text(1)=="odbcpp_pilot");const auto discovered_schema=text(2);const auto discovered_table=text(3);
    EXPECT_TRUE(discovered_schema==schema);EXPECT_TRUE(discovered_table==table);EXPECT_TRUE(text(4)=="TABLE");null(5);
    ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLTables(hstmt_,database,SQL_NTS,bytes(schema),SQL_NTS,empty,SQL_NTS,table_type,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    descriptors(tables);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
    const CatalogField columns[]{{"table_cat",SQL_VARCHAR},{"table_schem",SQL_VARCHAR},{"table_name",SQL_VARCHAR},{"column_name",SQL_VARCHAR},
      {"data_type",SQL_SMALLINT},{"type_name",SQL_VARCHAR},{"column_size",SQL_INTEGER},{"buffer_length",SQL_INTEGER},
      {"decimal_digits",SQL_SMALLINT},{"num_prec_radix",SQL_SMALLINT},{"nullable",SQL_SMALLINT},{"remarks",SQL_VARCHAR},
      {"column_def",SQL_VARCHAR},{"sql_data_type",SQL_SMALLINT},{"sql_datetime_sub",SQL_SMALLINT},{"char_octet_length",SQL_INTEGER},
      {"ordinal_position",SQL_INTEGER},{"is_nullable",SQL_VARCHAR}};
    std::array<std::string,2> owned_columns;
    const auto column_row=[&](bool value_column) {
      EXPECT_TRUE(text(1)=="odbcpp_pilot");EXPECT_TRUE(text(2)==schema);EXPECT_TRUE(text(3)==table);
      auto name=text(4);EXPECT_TRUE(name==(value_column?"value":"id"));
      if(!value_column) { owned_columns[0]=name; } else { owned_columns[1]=name; }
      number(5,value_column?SQL_VARCHAR:SQL_INTEGER);EXPECT_TRUE(text(6)==(value_column?"character varying":"integer"));
      number(7,value_column?32:10);number(8,value_column?32:4);
      if(value_column) { null(9);null(10); } else { number(9,0);number(10,10); }
      number(11,SQL_NULLABLE);remarks(12);null(13);number(14,value_column?SQL_VARCHAR:SQL_INTEGER);null(15);
      if(value_column) { number(16,32); } else { null(16); }
      number(17,value_column?2:1);EXPECT_TRUE(text(18)=="YES");
    };
    ASSERT_EQ(SQL_SUCCESS,SQLColumns(hstmt_,database,SQL_NTS,bytes(schema),SQL_NTS,bytes(table),SQL_NTS,nullptr,0)) << get_error(SQL_HANDLE_STMT,hstmt_);
    descriptors(columns,DescriptorScope::Columns);ASSERT_FALSE(HasFailure());
    const auto owned_column_descriptors=catalog_descriptors_;
    for(bool value_column:{false,true}) { ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));column_row(value_column);ASSERT_FALSE(HasFailure()); }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
    for(const char* pattern:{"","v\\%lue","v\\_lue","v%lue","v_lue"}) {
      ASSERT_FALSE(HasFailure());
      ASSERT_EQ(SQL_SUCCESS,SQLColumns(hstmt_,database,SQL_NTS,bytes(schema),SQL_NTS,bytes(table),SQL_NTS,
          reinterpret_cast<SQLCHAR*>(const_cast<char*>(pattern)),SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
      descriptors(columns,DescriptorScope::Columns);ASSERT_FALSE(HasFailure());
      EXPECT_EQ(owned_column_descriptors,catalog_descriptors_);ASSERT_FALSE(HasFailure());
      if(std::string_view(pattern)=="v%lue"||std::string_view(pattern)=="v_lue") {
        ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));column_row(true);ASSERT_FALSE(HasFailure());
      }
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
    }
    const auto owned_last_column_descriptors=catalog_descriptors_;
    catalog_descriptors_.clear(); // Later application results have their own descriptors.
    // Query targets only the exact selected/discovered identifiers; quote before
    // construction. Mode does not change this information_schema/SVV workflow.
    const auto sql="WITH odbcpp_input AS (SELECT CAST(? AS INTEGER) AS min_id, CAST(? AS VARCHAR(32)) AS marker) "
      "SELECT fixture.id,fixture.value,odbcpp_input.marker,CAST(CASE WHEN fixture.id=2 THEN NULL ELSE fixture.value END AS VARCHAR(32)) AS maybe_value "
      "FROM "+quoted(discovered_schema)+"."+quoted(discovered_table)+" AS fixture CROSS JOIN odbcpp_input "
      "WHERE fixture.id>=odbcpp_input.min_id ORDER BY fixture.id";
    ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    SQLSMALLINT parameter_count=0;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&parameter_count));ASSERT_EQ(2,parameter_count);
    static_assert(sizeof(SQLWCHAR)==2||sizeof(SQLWCHAR)==4);
    const std::array<SQLWCHAR,8> expected_marker{'G','r',0xfc,0xdf,'e',' ',0x03a9,0};
    struct OwnedRow {SQLINTEGER id;std::string value,maybe_value;std::array<SQLWCHAR,12> marker;bool marker_null,maybe_null;};
    std::vector<OwnedRow> snapshots;
    for(bool null_marker:{false,true}) {
      ASSERT_FALSE(HasFailure());
      SQLINTEGER minimum=1;SQLLEN minimum_length=sizeof(minimum);
      auto marker=expected_marker;SQLLEN marker_length=7*static_cast<SQLLEN>(sizeof(SQLWCHAR));
      if(null_marker) { marker.fill('!');marker_length=SQL_NULL_DATA; }
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&minimum,sizeof(minimum),&minimum_length));
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,2,SQL_PARAM_INPUT,SQL_C_WCHAR,SQL_VARCHAR,32,0,marker.data(),sizeof(marker),&marker_length));
      struct {SQLINTEGER before{17},id{-99},after{83};} bound;
      SQLLEN id_length=-1;ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_SLONG,&bound.id,sizeof(bound.id),&id_length));
      ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
      minimum=99;marker.fill('!'); // Owned inputs/results survive caller mutation.
      ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(4,count);
      const char* aliases[]{"id","value","marker","maybe_value"};
      for(SQLUSMALLINT column=1;column<=4;++column) {
        std::array<SQLCHAR,32> name{};SQLSMALLINT name_length=-1,type=-1,scale=-1,nullable=-1;SQLULEN size=99;
        ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name.data(),name.size(),&name_length,&type,&size,&scale,&nullable));
        ASSERT_GE(name_length,0);ASSERT_LT(name_length,32);
        EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(name.data()),static_cast<std::size_t>(name_length))==aliases[column-1]);
        EXPECT_EQ(column==1?SQL_INTEGER:SQL_VARCHAR,type);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
        if(column==1) { EXPECT_EQ(10U,size); } else { EXPECT_EQ(32U,size); }
      }
      ASSERT_FALSE(HasFailure());
      for(SQLINTEGER row=1;row<=2;++row) {
        ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));EXPECT_EQ(row,bound.id);EXPECT_EQ(17,bound.before);EXPECT_EQ(83,bound.after);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(bound.id)),id_length);
        auto value=text(2);EXPECT_TRUE(value==(row==1?"one":"two"));
        std::array<SQLWCHAR,12> output{};output.fill(0x5a);SQLLEN output_length=99;
        ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,3,SQL_C_WCHAR,output.data(),10*sizeof(SQLWCHAR),&output_length));
        if(null_marker) {
          EXPECT_EQ(SQL_NULL_DATA,output_length);EXPECT_TRUE(std::all_of(output.begin(),output.end(),[](auto unit){return unit==0x5a;}));
        } else {
          EXPECT_EQ(7*static_cast<SQLLEN>(sizeof(SQLWCHAR)),output_length);
          EXPECT_TRUE(std::equal(expected_marker.begin(),expected_marker.end(),output.begin()));
          EXPECT_TRUE(std::all_of(output.begin()+8,output.end(),[](auto unit){return unit==0x5a;}));
        }
        std::string maybe;
        if(row==1) { maybe=text(4);EXPECT_TRUE(maybe=="one"); } else { null(4); }
        snapshots.push_back({bound.id,std::move(value),std::move(maybe),output,null_marker,row==2});
        ASSERT_FALSE(HasFailure());
      }
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
    }
    ASSERT_FALSE(HasFailure());
    SQLCHAR recovery[]="SELECT 1";ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,recovery,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));number(1,1);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;
    ASSERT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));connected_=false;
    EXPECT_EQ(owned_column_descriptors,owned_last_column_descriptors);
    ASSERT_EQ(18U,owned_column_descriptors.size());
    EXPECT_TRUE(owned_columns[0]=="id");EXPECT_TRUE(owned_columns[1]=="value");ASSERT_EQ(4U,snapshots.size());
    for(std::size_t index=0;index<snapshots.size();++index) {
      const auto& row=snapshots[index];const bool second=index%2==1;
      EXPECT_EQ(second?2:1,row.id);EXPECT_TRUE(row.value==(second?"two":"one"));
      EXPECT_EQ(second,row.maybe_null);EXPECT_TRUE(row.maybe_value==(second?"":"one"));EXPECT_EQ(index>=2,row.marker_null);
      if(index<2) { EXPECT_TRUE(std::equal(expected_marker.begin(),expected_marker.end(),row.marker.begin())); }
      else { EXPECT_TRUE(std::all_of(row.marker.begin(),row.marker.end(),[](auto unit){return unit==0x5a;})); }
    }
  }
};

TEST_F(RedshiftMetadataWorkflowRealTest, MetadataToPreparedQueryShowConfigured) { workflow("SHOW"); }
TEST_F(RedshiftMetadataWorkflowRealTest, MetadataToPreparedQueryLegacyConfigured) { workflow("LEGACY"); }


// Prepared exact-decimal input is a separately admitted no-DDL scope; direct
// numeric result tests above do not establish this input/owning-result path.
class RedshiftDecimalParameterRealTest : public RedshiftRealTest {
protected:
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_DECIMAL_PARAMETER_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Prepared decimal scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="decimal-struct-parameter-v1")
        << "Invalid prepared decimal scope marker";
    // FIRST gate, before configuration, handles or connection work.
    RedshiftRealTest::SetUp();
  }
};

TEST_F(RedshiftDecimalParameterRealTest, PreparedExactDecimalAndNull) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
  SQLCHAR query[]="SELECT CAST(? AS DECIMAL(5,2)) AS amount";
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,query,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
  SQLSMALLINT parameters=0;
  ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&parameters));ASSERT_EQ(1,parameters);
  SQLHDESC apd=SQL_NULL_HDESC,ard=SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_APP_PARAM_DESC,&apd,0,nullptr));
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_APP_ROW_DESC,&ard,0,nullptr));
  const std::array<unsigned char,16> magnitude{0x39,0x30}; // 12345, independently literal.
  std::array<SQL_NUMERIC_STRUCT,2> owned{};
  const auto trials=[&]() {
    for(unsigned trial=0;trial<3;++trial) {
      SCOPED_TRACE(trial);
      SQL_NUMERIC_STRUCT input{};
      input.precision=5;input.scale=2;input.sign=trial==1?0:1;
      std::copy(magnitude.begin(),magnitude.end(),input.val);
      SQLLEN input_length=sizeof(input);
      if(trial==2) { std::memset(&input,0xff,sizeof(input));input_length=SQL_NULL_DATA; }
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_NUMERIC,
          SQL_NUMERIC,5,2,&input,sizeof(input),&input_length));
      ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(apd,1,SQL_DESC_PRECISION,
          reinterpret_cast<SQLPOINTER>(std::uintptr_t{5}),0));
      ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(apd,1,SQL_DESC_SCALE,
          reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}),0));
      // Metadata edits invalidate DATA_PTR; restore the actual bound input.
      ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(apd,1,SQL_DESC_DATA_PTR,&input,0));
      ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
      std::memset(&input,0xa5,sizeof(input)); // Result must own the executed input.
      SQLSMALLINT count=0;
      ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(1,count);
      std::array<SQLCHAR,16> name{};
      SQLSMALLINT name_length=-1,type=-1,digits=-1,nullable=-1;SQLULEN size=99;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,1,name.data(),name.size(),&name_length,
          &type,&size,&digits,&nullable));
      EXPECT_EQ(6,name_length);EXPECT_EQ(0,std::memcmp(name.data(),"amount",7));
      EXPECT_EQ(SQL_NUMERIC,type);EXPECT_EQ(5U,size);EXPECT_EQ(2,digits);
      // Expression nullability is not asserted as a native guarantee.
      struct GuardedNumeric {
        std::array<unsigned char,8> before;
        SQL_NUMERIC_STRUCT value;
        std::array<unsigned char,8> after;
      } output{};
      output.before.fill(0x5a);output.after.fill(0x5a);
      std::memset(&output.value,0x5a,sizeof(output.value));SQLLEN length=93;
      ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_NUMERIC,&output.value,sizeof(output.value),&length));
      ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard,1,SQL_DESC_PRECISION,
          reinterpret_cast<SQLPOINTER>(std::uintptr_t{5}),0));
      ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard,1,SQL_DESC_SCALE,
          reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}),0));
      ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard,1,SQL_DESC_DATA_PTR,&output.value,0));
      if(HasFailure()) { return; }
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
      EXPECT_TRUE(std::all_of(output.before.begin(),output.before.end(),[](auto byte){return byte==0x5a;}));
      EXPECT_TRUE(std::all_of(output.after.begin(),output.after.end(),[](auto byte){return byte==0x5a;}));
      if(trial==2) {
        EXPECT_EQ(SQL_NULL_DATA,length);
        EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&output.value),
            reinterpret_cast<const unsigned char*>(&output.value)+sizeof(output.value),
            [](auto byte){return byte==0x5a;}));
      } else {
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(output.value)),length);
        EXPECT_EQ(5,output.value.precision);EXPECT_EQ(2,output.value.scale);
        EXPECT_EQ(trial==1?0:1,output.value.sign);
        EXPECT_TRUE(std::equal(magnitude.begin(),magnitude.end(),output.value.val));
        owned[trial]=output.value;
      }
      if(HasFailure()) { return; }
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
    }
  };
  trials(); // An assertion failure is retained; never replay a failed execution.
  // Exactly one recovery execution, even after an earlier trial assertion.
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
  SQLCHAR recovery[]="SELECT 1";
  ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,recovery,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
  SQLSMALLINT count=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(1,count);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));SQLINTEGER scalar=-99;SQLLEN scalar_length=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&scalar,sizeof(scalar),&scalar_length));
  EXPECT_EQ(1,scalar);EXPECT_EQ(static_cast<SQLLEN>(sizeof(scalar)),scalar_length);
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  if(!HasFailure()) {
    EXPECT_EQ(1,owned[0].sign);EXPECT_EQ(0,owned[1].sign);
    for(const auto& value:owned) {
      EXPECT_EQ(5,value.precision);EXPECT_EQ(2,value.scale);
      EXPECT_TRUE(std::equal(magnitude.begin(),magnitude.end(),value.val));
    }
  }
}


class RedshiftTemporalParameterRealTest : public RedshiftRealTest {
protected:
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_TEMPORAL_PARAMETER_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Prepared temporal scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="temporal-struct-parameter-v1")
        << "Invalid prepared temporal scope marker";
    // FIRST gate, before configuration, handles or connection work.
    RedshiftRealTest::SetUp();
  }
};

TEST_F(RedshiftTemporalParameterRealTest, PreparedDateTimeTimestampAndNull) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
  SQLCHAR query[]="SELECT CAST(? AS DATE) AS calendar_day, CAST(? AS TIME) AS clock_time, CAST(? AS TIMESTAMP) AS stamp";
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,query,SQL_NTS));
  SQLSMALLINT parameter_count=0;
  ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&parameter_count));ASSERT_EQ(3,parameter_count);
  struct GuardedDate { std::array<unsigned char,8> before; SQL_DATE_STRUCT value; std::array<unsigned char,8> after; } date_out{};
  struct GuardedTime { std::array<unsigned char,8> before; SQL_TIME_STRUCT value; std::array<unsigned char,8> after; } time_out{};
  struct GuardedStamp { std::array<unsigned char,8> before; SQL_TIMESTAMP_STRUCT value; std::array<unsigned char,8> after; } stamp_out{};
  // All bound storage outlives failure recovery and RESET_PARAMS/UNBIND.
  SQL_DATE_STRUCT date_in{};SQL_TIME_STRUCT time_in{};SQL_TIMESTAMP_STRUCT stamp_in{};
  SQLLEN date_input_length=0,time_input_length=0,stamp_input_length=0;
  SQLLEN date_length=0,time_length=0,stamp_length=0;
  std::array<SQL_DATE_STRUCT,2> owned_dates{};
  std::array<SQL_TIME_STRUCT,2> owned_times{};
  std::array<SQL_TIMESTAMP_STRUCT,2> owned_stamps{};
  struct Descriptor {
    std::string name; SQLSMALLINT type; SQLULEN size; SQLSMALLINT digits; SQLSMALLINT nullable;
  };
  std::array<Descriptor,3> owned_descriptors{};
  const auto poison_ok=[](const auto& value) {
    const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
    return std::all_of(bytes,bytes+sizeof(value),[](auto byte){return byte==0x5a;});
  };
  const auto guard_ok=[](const auto& value) {
    return std::all_of(value.before.begin(),value.before.end(),[](auto byte){return byte==0x5a;}) &&
        std::all_of(value.after.begin(),value.after.end(),[](auto byte){return byte==0x5a;});
  };
  const auto trials=[&]() {
    for(unsigned trial=0;trial<3;++trial) {
      SCOPED_TRACE(trial);
      const SQL_DATE_STRUCT expected_date{static_cast<SQLSMALLINT>(trial==0?2024:2000),2,29};
      const SQL_TIME_STRUCT expected_time{static_cast<SQLUSMALLINT>(trial==0?12:0),
          static_cast<SQLUSMALLINT>(trial==0?34:0),static_cast<SQLUSMALLINT>(trial==0?56:0)};
      const SQL_TIMESTAMP_STRUCT expected_stamp{expected_date.year,2,29,
          expected_time.hour,expected_time.minute,expected_time.second,trial==0?123456000u:1000u};
      date_in=expected_date;time_in=expected_time;stamp_in=expected_stamp;
      date_input_length=sizeof(date_in);time_input_length=sizeof(time_in);stamp_input_length=sizeof(stamp_in);
      if(trial==2) {
        std::memset(&date_in,0xff,sizeof(date_in));std::memset(&time_in,0xff,sizeof(time_in));
        std::memset(&stamp_in,0xff,sizeof(stamp_in));
        date_input_length=SQL_NULL_DATA;time_input_length=SQL_NULL_DATA;stamp_input_length=SQL_NULL_DATA;
      }
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,
          10,0,&date_in,sizeof(date_in),&date_input_length));
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,2,SQL_PARAM_INPUT,SQL_C_TYPE_TIME,SQL_TYPE_TIME,
          8,0,&time_in,sizeof(time_in),&time_input_length));
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,3,SQL_PARAM_INPUT,SQL_C_TYPE_TIMESTAMP,SQL_TYPE_TIMESTAMP,
          26,6,&stamp_in,sizeof(stamp_in),&stamp_input_length));
      ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
      std::memset(&date_in,0xa5,sizeof(date_in));std::memset(&time_in,0xa5,sizeof(time_in));
      std::memset(&stamp_in,0xa5,sizeof(stamp_in));
      SQLSMALLINT count=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(3,count);
      const char* names[]{"calendar_day","clock_time","stamp"};
      const SQLSMALLINT types[]{SQL_TYPE_DATE,SQL_TYPE_TIME,SQL_TYPE_TIMESTAMP};
      // Whole-second C_TIME input does not alter the server's default TIME(6) result type.
      const SQLULEN sizes[]{10,15,26};const SQLSMALLINT scales[]{0,6,6};
      const SQLSMALLINT codes[]{SQL_CODE_DATE,SQL_CODE_TIME,SQL_CODE_TIMESTAMP};
      SQLHDESC result_descriptor=SQL_NULL_HDESC;
      ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_IMP_ROW_DESC,&result_descriptor,
          sizeof(result_descriptor),nullptr)) << get_error(SQL_HANDLE_STMT,hstmt_);
      for(SQLUSMALLINT column=1;column<=3;++column) {
        SCOPED_TRACE(column);
        std::array<SQLCHAR,32> name{};SQLSMALLINT name_length=-1,type=-1,digits=-1,nullable=-1;SQLULEN size=99;
        ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name.data(),name.size(),&name_length,
            &type,&size,&digits,&nullable));
        ASSERT_GE(name_length,0);ASSERT_LT(static_cast<std::size_t>(name_length),name.size());
        Descriptor observed{std::string(reinterpret_cast<const char*>(name.data()),static_cast<std::size_t>(name_length)),type,size,digits,nullable};
        EXPECT_EQ(names[column-1],observed.name);EXPECT_EQ(types[column-1],type);
        EXPECT_EQ(sizes[column-1],size);EXPECT_EQ(scales[column-1],digits);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
        for(const auto& field:{std::pair{SQL_DESC_CONCISE_TYPE,types[column-1]},
            std::pair{SQL_DESC_TYPE,SQLSMALLINT(SQL_DATETIME)},
            std::pair{SQL_DESC_SCALE,scales[column-1]}}) {
          SCOPED_TRACE(field.first);
          SQLLEN actual=-1;ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(hstmt_,column,field.first,nullptr,0,nullptr,&actual))
              << get_error(SQL_HANDLE_STMT,hstmt_);
          EXPECT_EQ(field.second,actual);
        }
        // The datetime subcode is a descriptor field, not a SQLColAttribute identifier.
        SQLSMALLINT datetime_code=-1;
        ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(result_descriptor,static_cast<SQLSMALLINT>(column),
            SQL_DESC_DATETIME_INTERVAL_CODE,&datetime_code,sizeof(datetime_code),nullptr))
            << get_error(SQL_HANDLE_DESC,result_descriptor);
        EXPECT_EQ(codes[column-1],datetime_code);
        SQLLEN display=-1;ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(hstmt_,column,SQL_DESC_DISPLAY_SIZE,nullptr,0,nullptr,&display))
            << get_error(SQL_HANDLE_STMT,hstmt_);
        EXPECT_EQ(static_cast<SQLLEN>(sizes[column-1]),display);
        if(trial==0) { owned_descriptors[column-1]=observed; }
        else {
          const auto& previous=owned_descriptors[column-1];
          EXPECT_EQ(previous.name,observed.name);EXPECT_EQ(previous.type,type);EXPECT_EQ(previous.size,size);
          EXPECT_EQ(previous.digits,digits);EXPECT_EQ(previous.nullable,nullable);
        }
      }
      date_out.before.fill(0x5a);date_out.after.fill(0x5a);std::memset(&date_out.value,0x5a,sizeof(date_out.value));
      time_out.before.fill(0x5a);time_out.after.fill(0x5a);std::memset(&time_out.value,0x5a,sizeof(time_out.value));
      stamp_out.before.fill(0x5a);stamp_out.after.fill(0x5a);std::memset(&stamp_out.value,0x5a,sizeof(stamp_out.value));
      date_length=93;time_length=94;stamp_length=95;
      ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_TYPE_DATE,&date_out.value,sizeof(date_out.value),&date_length));
      ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,2,SQL_C_TYPE_TIME,&time_out.value,sizeof(time_out.value),&time_length));
      ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,3,SQL_C_TYPE_TIMESTAMP,&stamp_out.value,sizeof(stamp_out.value),&stamp_length));
      if(HasFailure()) { return; }
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
      EXPECT_TRUE(guard_ok(date_out));EXPECT_TRUE(guard_ok(time_out));EXPECT_TRUE(guard_ok(stamp_out));
      if(trial==2) {
        EXPECT_EQ(SQL_NULL_DATA,date_length);EXPECT_EQ(SQL_NULL_DATA,time_length);EXPECT_EQ(SQL_NULL_DATA,stamp_length);
        EXPECT_TRUE(poison_ok(date_out.value));EXPECT_TRUE(poison_ok(time_out.value));EXPECT_TRUE(poison_ok(stamp_out.value));
      } else {
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(date_out.value)),date_length);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(time_out.value)),time_length);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(stamp_out.value)),stamp_length);
        EXPECT_EQ(expected_date.year,date_out.value.year);EXPECT_EQ(2,date_out.value.month);EXPECT_EQ(29,date_out.value.day);
        EXPECT_EQ(expected_time.hour,time_out.value.hour);EXPECT_EQ(expected_time.minute,time_out.value.minute);EXPECT_EQ(expected_time.second,time_out.value.second);
        EXPECT_EQ(expected_stamp.year,stamp_out.value.year);EXPECT_EQ(2,stamp_out.value.month);EXPECT_EQ(29,stamp_out.value.day);
        EXPECT_EQ(expected_stamp.hour,stamp_out.value.hour);EXPECT_EQ(expected_stamp.minute,stamp_out.value.minute);
        EXPECT_EQ(expected_stamp.second,stamp_out.value.second);EXPECT_EQ(expected_stamp.fraction,stamp_out.value.fraction);
        owned_dates[trial]=date_out.value;owned_times[trial]=time_out.value;owned_stamps[trial]=stamp_out.value;
      }
      if(HasFailure()) { return; }
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
    }
  };
  trials(); // Preserve any original failure; never replay its parameter execution.
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
  SQLCHAR recovery[]="SELECT 1";ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,recovery,SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));SQLINTEGER scalar=-99;SQLLEN scalar_length=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&scalar,sizeof(scalar),&scalar_length));
  EXPECT_EQ(1,scalar);EXPECT_EQ(static_cast<SQLLEN>(sizeof(scalar)),scalar_length);
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  if(!HasFailure()) {
    for(unsigned index=0;index<2;++index) {
      EXPECT_EQ(index==0?2024:2000,owned_dates[index].year);EXPECT_EQ(2,owned_dates[index].month);EXPECT_EQ(29,owned_dates[index].day);
      EXPECT_EQ(index==0?12:0,owned_times[index].hour);EXPECT_EQ(index==0?34:0,owned_times[index].minute);EXPECT_EQ(index==0?56:0,owned_times[index].second);
      EXPECT_EQ(owned_dates[index].year,owned_stamps[index].year);EXPECT_EQ(2,owned_stamps[index].month);EXPECT_EQ(29,owned_stamps[index].day);
      EXPECT_EQ(owned_times[index].hour,owned_stamps[index].hour);EXPECT_EQ(owned_times[index].minute,owned_stamps[index].minute);
      EXPECT_EQ(owned_times[index].second,owned_stamps[index].second);EXPECT_EQ(index==0?123456000u:1000u,owned_stamps[index].fraction);
    }
    EXPECT_EQ("calendar_day",owned_descriptors[0].name);EXPECT_EQ("clock_time",owned_descriptors[1].name);EXPECT_EQ("stamp",owned_descriptors[2].name);
  }
}


class RedshiftUnicodeChunkRealTest : public RedshiftRealTest {
protected:
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_UNICODE_CHUNK_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Unicode chunk scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="unicode-chunk-result-v1") << "Invalid Unicode chunk scope marker";
    // FIRST gate, before configuration, handles or connection work.
    RedshiftRealTest::SetUp();
  }
};

TEST_F(RedshiftUnicodeChunkRealTest, PreparedWideChunksBoundTruncationAndNull) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
  SQLCHAR query[]="SELECT CAST(? AS VARCHAR(64)) AS bound_text, CAST(? AS VARCHAR(64)) AS chunk_text";
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,query,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
  SQLSMALLINT parameters=0;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&parameters));ASSERT_EQ(2,parameters);
  const std::string positive="A\xe2\x82\xac\xf0\x9f\x98\x80" "B";
  std::vector<SQLWCHAR> expected{'A',0x20ac};
  if constexpr(sizeof(SQLWCHAR)==2) { expected.push_back(0xd83d);expected.push_back(0xde00); }
  else { static_assert(sizeof(SQLWCHAR)==2 || sizeof(SQLWCHAR)==4);expected.push_back(static_cast<SQLWCHAR>(0x1f600)); }
  expected.push_back('B');
  struct GuardedWide { std::array<unsigned char,8> before;std::array<SQLWCHAR,4> value;std::array<unsigned char,8> after; } bound{},chunk{};
  // These buffers, strings and indicators survive a fatal trial return and recovery cleanup.
  std::string first,second;SQLLEN first_length=0,second_length=0,bound_length=0,chunk_length=0;
  std::vector<SQLWCHAR> owned_chunks,owned_bound;
  struct Descriptor { std::string name;SQLSMALLINT type;SQLULEN size;SQLSMALLINT scale;SQLSMALLINT nullable; };
  std::array<Descriptor,2> owned_descriptors{};
  const SQLWCHAR poison=static_cast<SQLWCHAR>(0x5a);
  const auto reset=[&](auto& buffer) { buffer.before.fill(0x5a);buffer.after.fill(0x5a);buffer.value.fill(poison); };
  const auto guards=[](const auto& buffer) {
    return std::all_of(buffer.before.begin(),buffer.before.end(),[](auto byte){return byte==0x5a;}) &&
        std::all_of(buffer.after.begin(),buffer.after.end(),[](auto byte){return byte==0x5a;});
  };
  const auto read_chunk=[&](SQLLEN units,SQLRETURN wanted,SQLLEN remaining,std::initializer_list<SQLWCHAR> payload) {
    SCOPED_TRACE(units); // Numeric capacity only, no connection/server message.
    reset(chunk);chunk_length=97;
    const auto actual=SQLGetData(hstmt_,2,SQL_C_WCHAR,chunk.value.data(),units*sizeof(SQLWCHAR),&chunk_length);
    EXPECT_EQ(wanted,actual) << get_error(SQL_HANDLE_STMT,hstmt_);
    if(actual!=wanted) { return false; }
    EXPECT_EQ(remaining,chunk_length);EXPECT_TRUE(guards(chunk));
    if(wanted==SQL_SUCCESS_WITH_INFO) { EXPECT_EQ("01004",get_error(SQL_HANDLE_STMT,hstmt_)); }
    std::size_t index=0;
    for(auto value:payload) { EXPECT_EQ(value,chunk.value[index++]); }
    EXPECT_EQ(SQLWCHAR{},chunk.value[index++]);
    for(;index<chunk.value.size();++index) { EXPECT_EQ(poison,chunk.value[index]); }
    if(!HasFailure()) { owned_chunks.insert(owned_chunks.end(),payload.begin(),payload.end()); }
    return !HasFailure();
  };
  const auto trials=[&]() {
    for(unsigned trial=0;trial<3;++trial) {
      SCOPED_TRACE(trial);
      first=trial==0?positive:trial==1?"":std::string("poison\xff",7);second=first;
      first_length=trial==2?SQL_NULL_DATA:static_cast<SQLLEN>(first.size());second_length=first_length;
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,64,0,
          first.data(),first.size(),&first_length));
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,2,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,64,0,
          second.data(),second.size(),&second_length));
      ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
      first.assign("changed");second.assign("changed"); // Fetch must reflect owning executed parameters.
      SQLSMALLINT count=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(2,count);
      for(SQLUSMALLINT column=1;column<=2;++column) {
        SCOPED_TRACE(column);std::array<SQLCHAR,32> name{};SQLSMALLINT length=-1,type=-1,scale=-1,nullable=-1;SQLULEN size=99;
        ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name.data(),name.size(),&length,&type,&size,&scale,&nullable))
            << get_error(SQL_HANDLE_STMT,hstmt_);
        ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length),name.size());
        Descriptor observed{std::string(reinterpret_cast<const char*>(name.data()),static_cast<std::size_t>(length)),type,size,scale,nullable};
        EXPECT_EQ(column==1?"bound_text":"chunk_text",observed.name);EXPECT_EQ(SQL_VARCHAR,type);
        EXPECT_EQ(64u,size);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
        for(const auto& field:{std::pair{SQL_DESC_CONCISE_TYPE,SQLLEN(SQL_VARCHAR)},std::pair{SQL_DESC_LENGTH,SQLLEN(64)},std::pair{SQL_DESC_SCALE,SQLLEN(0)}}) {
          SCOPED_TRACE(field.first);SQLLEN value=-1;
          ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(hstmt_,column,field.first,nullptr,0,nullptr,&value)) << get_error(SQL_HANDLE_STMT,hstmt_);
          EXPECT_EQ(field.second,value);
        }
        if(trial==0) { owned_descriptors[column-1]=observed; }
        else {
          const auto& previous=owned_descriptors[column-1];EXPECT_EQ(previous.name,observed.name);
          EXPECT_EQ(previous.type,type);EXPECT_EQ(previous.size,size);EXPECT_EQ(previous.scale,scale);EXPECT_EQ(previous.nullable,nullable);
        }
      }
      reset(bound);bound_length=93;
      ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_WCHAR,bound.value.data(),2*sizeof(SQLWCHAR),&bound_length));
      if(HasFailure()) { return; }
      ASSERT_EQ(trial==0?SQL_SUCCESS_WITH_INFO:SQL_SUCCESS,SQLFetch(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
      EXPECT_TRUE(guards(bound));
      if(trial==0) {
        EXPECT_EQ("01004",get_error(SQL_HANDLE_STMT,hstmt_));
        EXPECT_EQ(static_cast<SQLLEN>(expected.size()*sizeof(SQLWCHAR)),bound_length);
        EXPECT_EQ(SQLWCHAR('A'),bound.value[0]);EXPECT_EQ(SQLWCHAR{},bound.value[1]);
        EXPECT_EQ(poison,bound.value[2]);EXPECT_EQ(poison,bound.value[3]);owned_bound={bound.value[0]};
        owned_chunks.clear();const auto full=static_cast<SQLLEN>(expected.size()*sizeof(SQLWCHAR));
        ASSERT_TRUE(read_chunk(2,SQL_SUCCESS_WITH_INFO,full,{'A'}));
        ASSERT_TRUE(read_chunk(2,SQL_SUCCESS_WITH_INFO,full-sizeof(SQLWCHAR),{0x20ac}));
        const SQLLEN remaining=full-2*sizeof(SQLWCHAR);
        ASSERT_TRUE(read_chunk(1,SQL_SUCCESS_WITH_INFO,remaining,{}));
        if constexpr(sizeof(SQLWCHAR)==2) {
          ASSERT_TRUE(read_chunk(2,SQL_SUCCESS_WITH_INFO,remaining,{}));
          ASSERT_TRUE(read_chunk(3,SQL_SUCCESS_WITH_INFO,remaining,{0xd83d,0xde00}));
        } else { ASSERT_TRUE(read_chunk(2,SQL_SUCCESS_WITH_INFO,remaining,{static_cast<SQLWCHAR>(0x1f600)})); }
        ASSERT_TRUE(read_chunk(2,SQL_SUCCESS,sizeof(SQLWCHAR),{'B'}));EXPECT_EQ(expected,owned_chunks);
      } else if(trial==1) {
        EXPECT_EQ(0,bound_length);EXPECT_EQ(SQLWCHAR{},bound.value[0]);
        for(std::size_t index=1;index<bound.value.size();++index) { EXPECT_EQ(poison,bound.value[index]); }
        // Empty retrieval must not replace the positive owning reconstruction.
        ASSERT_TRUE(read_chunk(2,SQL_SUCCESS,0,{}));EXPECT_EQ(expected,owned_chunks);
      } else {
        EXPECT_EQ(SQL_NULL_DATA,bound_length);
        EXPECT_TRUE(std::all_of(bound.value.begin(),bound.value.end(),[&](auto unit){return unit==poison;}));
        reset(chunk);chunk_length=97;
        ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,2,SQL_C_WCHAR,chunk.value.data(),2*sizeof(SQLWCHAR),&chunk_length));
        EXPECT_EQ(SQL_NULL_DATA,chunk_length);EXPECT_TRUE(guards(chunk));
        EXPECT_TRUE(std::all_of(chunk.value.begin(),chunk.value.end(),[&](auto unit){return unit==poison;}));
      }
      if(HasFailure()) { return; }
      reset(chunk);const auto finished_length=chunk_length;
      ASSERT_EQ(SQL_NO_DATA,SQLGetData(hstmt_,2,SQL_C_WCHAR,chunk.value.data(),2*sizeof(SQLWCHAR),&chunk_length));
      EXPECT_EQ(finished_length,chunk_length);EXPECT_TRUE(guards(chunk));
      EXPECT_TRUE(std::all_of(chunk.value.begin(),chunk.value.end(),[&](auto unit){return unit==poison;}));
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
    }
  };
  trials(); // At most three applications; retain any original failure, no replay.
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
  SQLCHAR recovery[]="SELECT 1";ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,recovery,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));SQLINTEGER scalar=-99;SQLLEN length=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&scalar,sizeof(scalar),&length));
  EXPECT_EQ(1,scalar);EXPECT_EQ(static_cast<SQLLEN>(sizeof(scalar)),length);
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  if(!HasFailure()) {
    EXPECT_EQ(expected,owned_chunks);ASSERT_EQ(1u,owned_bound.size());EXPECT_EQ(SQLWCHAR('A'),owned_bound[0]);
    EXPECT_EQ("bound_text",owned_descriptors[0].name);EXPECT_EQ("chunk_text",owned_descriptors[1].name);
  }
}


class RedshiftBooleanFloatingRealTest : public RedshiftRealTest {
protected:
  // Members survive a fatal test-body return until base TearDown frees handles.
  struct BitCell { std::array<unsigned char, 8> before; SQLCHAR value; std::array<unsigned char, 8> after; } bit_out_{};
  struct RealCell { std::array<unsigned char, 8> before; SQLREAL value; std::array<unsigned char, 8> after; } real_out_{};
  struct DoubleCell { std::array<unsigned char, 8> before; SQLDOUBLE value; std::array<unsigned char, 8> after; } double_out_{};
  SQLCHAR bit_in_ = 0;
  SQLREAL real_in_ = 0;
  SQLDOUBLE double_in_ = 0;
  SQLLEN bit_input_length_ = 0, real_input_length_ = 0, double_input_length_ = 0;
  SQLLEN bit_length_ = 0, real_length_ = 0, double_length_ = 0;
  SQLULEN processed_ = 99;
  SQLUSMALLINT parameter_status_ = SQL_PARAM_UNUSED;

  void SetUp() override {
    const char* marker = std::getenv("ODBCPP_REDSHIFT_BOOLEAN_FLOATING_ADMISSION");
    if (marker == nullptr) {
      GTEST_SKIP() << "Boolean/floating scope is not admitted";
    }
    ASSERT_TRUE(std::string_view(marker) == "boolean-floating-parameter-v1")
        << "Invalid boolean/floating scope marker";
    // FIRST gate, before configuration, handles or connection work.
    RedshiftRealTest::SetUp();
  }
};

TEST_F(RedshiftBooleanFloatingRealTest, PreparedBooleanRealDoubleAndNull) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC, hdbc_);
  SQLCHAR query[] = "SELECT CAST(? AS BOOLEAN) AS flag_value, CAST(? AS REAL) AS real_value, CAST(? AS DOUBLE PRECISION) AS double_value";
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt_, query, SQL_NTS))
      << get_error(SQL_HANDLE_STMT, hstmt_);
  SQLSMALLINT parameter_count = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt_, &parameter_count));
  ASSERT_EQ(3, parameter_count);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed_, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_PARAM_STATUS_PTR, &parameter_status_, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT,
      1, 0, &bit_in_, sizeof(bit_in_), &bit_input_length_));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 2, SQL_PARAM_INPUT, SQL_C_FLOAT, SQL_REAL,
      7, 0, &real_in_, sizeof(real_in_), &real_input_length_));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 3, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE,
      15, 0, &double_in_, sizeof(double_in_), &double_input_length_));

  struct Descriptor {
    std::string name;
    SQLSMALLINT type = 0, nullable = 0;
    SQLULEN size = 0;
    SQLLEN octets = 0;
  };
  std::array<Descriptor, 3> owned_descriptors{};
  bool have_descriptors = false;
  const auto describe = [&] {
    SQLSMALLINT count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt_, &count));
    ASSERT_EQ(3, count);
    const char* names[]{"flag_value", "real_value", "double_value"};
    const SQLSMALLINT types[]{SQL_BIT, SQL_REAL, SQL_DOUBLE};
    const SQLULEN sizes[]{1, 7, 15};
    const SQLLEN octets[]{1, 4, 8};
    for (SQLUSMALLINT column = 1; column <= 3; ++column) {
      SCOPED_TRACE("column=" + std::to_string(column));
      SQLCHAR name[32]{};
      SQLSMALLINT name_length = 0, type = 0, digits = 0, nullable = 0;
      SQLULEN size = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt_, column, name, sizeof(name),
          &name_length, &type, &size, &digits, &nullable)) << get_error(SQL_HANDLE_STMT, hstmt_);
      ASSERT_GE(name_length, 0);
      ASSERT_LT(static_cast<std::size_t>(name_length), sizeof(name));
      const std::string owned_name(reinterpret_cast<const char*>(name), static_cast<std::size_t>(name_length));
      EXPECT_EQ(names[column - 1], owned_name);
      EXPECT_EQ(types[column - 1], type);
      EXPECT_EQ(sizes[column - 1], size);
      // Native type recognition is not expression nullability; RowDescription
      // is published by column_info_for with SQL_NULLABLE_UNKNOWN.
      EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);
      // Approximate decimal scale is not a normative floating-point oracle.
      SQLLEN concise = -1, bytes = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(hstmt_, column, SQL_DESC_CONCISE_TYPE,
          nullptr, 0, nullptr, &concise)) << get_error(SQL_HANDLE_STMT, hstmt_);
      ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(hstmt_, column, SQL_DESC_OCTET_LENGTH,
          nullptr, 0, nullptr, &bytes)) << get_error(SQL_HANDLE_STMT, hstmt_);
      EXPECT_EQ(types[column - 1], concise);
      EXPECT_EQ(octets[column - 1], bytes);
      if (column > 1) {
        SQLLEN radix = -1, precision = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(hstmt_, column, SQL_DESC_NUM_PREC_RADIX,
            nullptr, 0, nullptr, &radix)) << get_error(SQL_HANDLE_STMT, hstmt_);
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(hstmt_, column, SQL_DESC_PRECISION,
            nullptr, 0, nullptr, &precision)) << get_error(SQL_HANDLE_STMT, hstmt_);
        EXPECT_EQ(2, radix);
        EXPECT_EQ(column == 2 ? 24 : 53, precision);
      }
      const Descriptor current{owned_name, type, nullable, size, bytes};
      if (have_descriptors) {
        const auto& previous = owned_descriptors[column - 1];
        EXPECT_EQ(previous.name, current.name);
        EXPECT_EQ(previous.type, current.type);
        EXPECT_EQ(previous.nullable, current.nullable);
        EXPECT_EQ(previous.size, current.size);
        EXPECT_EQ(previous.octets, current.octets);
      } else {
        owned_descriptors[column - 1] = current;
      }
    }
    have_descriptors = true;
  };
  const auto bind_outputs = [&] {
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt_, 1, SQL_C_BIT, &bit_out_.value,
        sizeof(bit_out_.value), &bit_length_));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt_, 2, SQL_C_FLOAT, &real_out_.value,
        sizeof(real_out_.value), &real_length_));
    // Column3 remains unbound and is retrieved after bound columns1/2.
  };
  const auto poison_outputs = [&] {
    std::memset(&bit_out_, 0x5a, sizeof(bit_out_));
    std::memset(&real_out_, 0x5a, sizeof(real_out_));
    std::memset(&double_out_, 0x5a, sizeof(double_out_));
    bit_length_ = real_length_ = double_length_ = 93;
  };
  const auto poisoned = [](const void* value, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(value);
    return std::all_of(bytes, bytes + size, [](unsigned char byte) { return byte == 0x5a; });
  };
  struct Values { SQLCHAR bit; SQLREAL real; SQLDOUBLE number; };
  std::array<Values, 2> owned_values{};
  const auto fetch_row = [&](bool nulls, SQLCHAR bit, SQLREAL real, SQLDOUBLE number) {
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_)) << get_error(SQL_HANDLE_STMT, hstmt_);
    if (nulls) {
      EXPECT_EQ(SQL_NULL_DATA, bit_length_);
      EXPECT_EQ(SQL_NULL_DATA, real_length_);
      EXPECT_TRUE(poisoned(&bit_out_.value, sizeof(bit_out_.value)));
      EXPECT_TRUE(poisoned(&real_out_.value, sizeof(real_out_.value)));
    } else {
      EXPECT_EQ(bit, bit_out_.value);
      EXPECT_EQ(real, real_out_.value);
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(bit_out_.value)), bit_length_);
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(real_out_.value)), real_length_);
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 3, SQL_C_DOUBLE, &double_out_.value,
        sizeof(double_out_.value), &double_length_)) << get_error(SQL_HANDLE_STMT, hstmt_);
    if (nulls) {
      EXPECT_EQ(SQL_NULL_DATA, double_length_);
      EXPECT_TRUE(poisoned(&double_out_.value, sizeof(double_out_.value)));
    } else {
      EXPECT_EQ(number, double_out_.value);
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(double_out_.value)), double_length_);
    }
    EXPECT_TRUE(poisoned(bit_out_.before.data(), bit_out_.before.size()));
    EXPECT_TRUE(poisoned(bit_out_.after.data(), bit_out_.after.size()));
    EXPECT_TRUE(poisoned(real_out_.before.data(), real_out_.before.size()));
    EXPECT_TRUE(poisoned(real_out_.after.data(), real_out_.after.size()));
    EXPECT_TRUE(poisoned(double_out_.before.data(), double_out_.before.size()));
    EXPECT_TRUE(poisoned(double_out_.after.data(), double_out_.after.size()));
    std::array<unsigned char, sizeof(double_out_)> previous{};
    std::memcpy(previous.data(), &double_out_, sizeof(double_out_));
    double_length_ = 93;
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt_, 3, SQL_C_DOUBLE, &double_out_.value,
        sizeof(double_out_.value), &double_length_));
    EXPECT_EQ(93, double_length_);
    EXPECT_EQ(0, std::memcmp(previous.data(), &double_out_, sizeof(double_out_)));
  };
  unsigned application_attempts = 0;
  bind_outputs();
  if (HasFailure()) return;
  for (unsigned trial = 0; trial != 3; ++trial) {
    SCOPED_TRACE("trial=" + std::to_string(trial));
    const bool nulls = trial == 2;
    const SQLCHAR expected_bit = trial == 0 ? 1 : 0;
    const SQLREAL expected_real = trial == 0 ? SQLREAL{1.25} : SQLREAL{-2.5};
    const SQLDOUBLE expected_double = trial == 0 ? SQLDOUBLE{-2.5} : SQLDOUBLE{1.25};
    bit_in_ = nulls ? 2 : expected_bit;
    real_in_ = nulls ? SQLREAL{17} : expected_real;
    double_in_ = nulls ? SQLDOUBLE{23} : expected_double;
    bit_input_length_ = nulls ? SQL_NULL_DATA : static_cast<SQLLEN>(sizeof(bit_in_));
    real_input_length_ = nulls ? SQL_NULL_DATA : static_cast<SQLLEN>(sizeof(real_in_));
    double_input_length_ = nulls ? SQL_NULL_DATA : static_cast<SQLLEN>(sizeof(double_in_));
    processed_ = 99; parameter_status_ = SQL_PARAM_UNUSED;
    poison_outputs();
    ++application_attempts;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT, hstmt_);
    EXPECT_EQ(1u, processed_); EXPECT_EQ(SQL_PARAM_SUCCESS, parameter_status_);
    // Execution owns its values: caller storage/indicators may now change.
    bit_in_ = 2; real_in_ = 17; double_in_ = 23;
    bit_input_length_ = real_input_length_ = double_input_length_ = SQL_NULL_DATA;
    describe();
    if (HasFailure()) return;
    fetch_row(nulls, expected_bit, expected_real, expected_double);
    if (HasFailure()) return;
    if (!nulls) owned_values[trial] = {bit_out_.value, real_out_.value, double_out_.value};
    ASSERT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  }
  bit_in_ = 2; real_in_ = 1.25; double_in_ = -2.5;
  bit_input_length_ = sizeof(bit_in_); real_input_length_ = sizeof(real_in_);
  double_input_length_ = sizeof(double_in_);
  processed_ = 99; parameter_status_ = SQL_PARAM_UNUSED;
  ++application_attempts;
  const auto refused = SQLExecute(hstmt_);
  {
    SCOPED_TRACE("parameter=1 SQLSTATE=" + get_error(SQL_HANDLE_STMT, hstmt_));
    EXPECT_EQ(SQL_ERROR, refused);
    EXPECT_EQ("22003", get_error(SQL_HANDLE_STMT, hstmt_));
    EXPECT_EQ(SQL_PARAM_ERROR, parameter_status_); EXPECT_EQ(1u, processed_);
    EXPECT_EQ(2, bit_in_); EXPECT_EQ(static_cast<SQLLEN>(sizeof(bit_in_)), bit_input_length_);
    EXPECT_EQ(SQLREAL{1.25}, real_in_); EXPECT_EQ(SQLDOUBLE{-2.5}, double_in_);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(real_in_)), real_input_length_);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(double_in_)), double_input_length_);
  }
  if (HasFailure()) return;
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_RESET_PARAMS));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_PARAMS_PROCESSED_PTR, nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_PARAM_STATUS_PTR, nullptr, 0));
  bind_outputs();
  if (HasFailure()) return;
  poison_outputs();
  SQLCHAR recovery[] = "SELECT CAST(FALSE AS BOOLEAN) AS flag_value, CAST(1.25 AS REAL) AS real_value, CAST(-2.5 AS DOUBLE PRECISION) AS double_value";
  ++application_attempts;
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, recovery, SQL_NTS)) << get_error(SQL_HANDLE_STMT, hstmt_);
  describe();
  if (HasFailure()) return;
  fetch_row(false, 0, 1.25, -2.5);
  if (HasFailure()) return;
  ASSERT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_UNBIND));
  poison_outputs();
  EXPECT_EQ(5u, application_attempts);
  EXPECT_EQ(1, owned_values[0].bit); EXPECT_EQ(SQLREAL{1.25}, owned_values[0].real); EXPECT_EQ(SQLDOUBLE{-2.5}, owned_values[0].number);
  EXPECT_EQ(0, owned_values[1].bit); EXPECT_EQ(SQLREAL{-2.5}, owned_values[1].real); EXPECT_EQ(SQLDOUBLE{1.25}, owned_values[1].number);
  EXPECT_EQ("flag_value", owned_descriptors[0].name);
  EXPECT_EQ("real_value", owned_descriptors[1].name);
  EXPECT_EQ("double_value", owned_descriptors[2].name);
}


#include <optional>

class RedshiftMultirowFetchRealTest : public RedshiftRealTest {
protected:
  // Bound members survive fatal body returns through inherited handle TearDown.
  struct IntCell { std::array<unsigned char, 8> before; SQLINTEGER value; std::array<unsigned char, 8> after; } ordinal_{};
  struct ShortCell { std::array<unsigned char, 8> before; SQLSMALLINT value; std::array<unsigned char, 8> after; } narrowed_{};
  struct TextCell { std::array<unsigned char, 8> before; char value[8]; std::array<unsigned char, 8> after; } text_{};
  SQLLEN ordinal_length_ = 93, narrowed_length_ = 93, text_length_ = 93;
  SQLULEN fetched_ = 99;
  SQLUSMALLINT row_status_ = SQL_ROW_NOROW;

  void SetUp() override {
    const char* marker = std::getenv("ODBCPP_REDSHIFT_MULTIROW_FETCH_ADMISSION");
    if (marker == nullptr) {
      GTEST_SKIP() << "Multirow fetch scope is not admitted";
    }
    ASSERT_TRUE(std::string_view(marker) == "multirow-fetch-v1")
        << "Invalid multirow fetch scope marker";
    // FIRST gate, before configuration, handles or connection work.
    RedshiftRealTest::SetUp();
  }
};

TEST_F(RedshiftMultirowFetchRealTest, OrderedRowsStatusNullAndRecovery) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC, hdbc_);
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_ARRAY_SIZE,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}), 0));
  SQLULEN array_size = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt_, SQL_ATTR_ROW_ARRAY_SIZE,
      &array_size, sizeof(array_size), nullptr));
  EXPECT_EQ(2u, array_size);
  if (HasFailure()) return;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_ARRAY_SIZE,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{1}), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_ROWS_FETCHED_PTR, &fetched_, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_STATUS_PTR, &row_status_, 0));

  SQLCHAR query[] =
      "SELECT CAST(row_no AS INTEGER) AS row_no, "
      "CAST(narrowed_value AS INTEGER) AS narrowed_value, "
      "CAST(row_text AS VARCHAR(8)) AS row_text FROM ("
      "SELECT CAST(1 AS INTEGER) AS row_no, CAST(1 AS INTEGER) AS narrowed_value, "
      "CAST('one' AS VARCHAR(8)) AS row_text "
      "UNION ALL SELECT 2, 2, CAST(NULL AS VARCHAR(8)) "
      "UNION ALL SELECT 3, 32768, CAST('err' AS VARCHAR(8)) "
      "UNION ALL SELECT 4, 4, CAST(NULL AS VARCHAR(8)) "
      "UNION ALL SELECT 5, 5, CAST('five' AS VARCHAR(8))"
      ") AS fixed_rows ORDER BY row_no";
  const auto bind_columns = [&] {
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt_, 1, SQL_C_SLONG, &ordinal_.value,
        sizeof(ordinal_.value), &ordinal_length_));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt_, 2, SQL_C_SSHORT, &narrowed_.value,
        sizeof(narrowed_.value), &narrowed_length_));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt_, 3, SQL_C_CHAR, text_.value,
        sizeof(text_.value), &text_length_));
  };
  const auto poison = [&] {
    std::memset(&ordinal_, 0x5a, sizeof(ordinal_));
    std::memset(&narrowed_, 0x5a, sizeof(narrowed_));
    std::memset(&text_, 0x5a, sizeof(text_));
    ordinal_.value = -77; narrowed_.value = -77;
    ordinal_length_ = narrowed_length_ = text_length_ = 93;
    fetched_ = 99; row_status_ = SQL_ROW_NOROW;
  };
  const auto poisoned = [](const void* bytes, std::size_t size) {
    const auto* begin = static_cast<const unsigned char*>(bytes);
    return std::all_of(begin, begin + size, [](unsigned char byte) { return byte == 0x5a; });
  };
  const auto guards = [&] {
    EXPECT_TRUE(poisoned(ordinal_.before.data(), ordinal_.before.size()));
    EXPECT_TRUE(poisoned(ordinal_.after.data(), ordinal_.after.size()));
    EXPECT_TRUE(poisoned(narrowed_.before.data(), narrowed_.before.size()));
    EXPECT_TRUE(poisoned(narrowed_.after.data(), narrowed_.after.size()));
    EXPECT_TRUE(poisoned(text_.before.data(), text_.before.size()));
    EXPECT_TRUE(poisoned(text_.after.data(), text_.after.size()));
  };
  struct Descriptor { std::string name; SQLSMALLINT type; SQLULEN size; SQLSMALLINT nullable; };
  std::array<Descriptor, 3> descriptors{};
  bool have_descriptors = false;
  const auto describe = [&] {
    SQLSMALLINT count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt_, &count)); ASSERT_EQ(3, count);
    const char* names[]{"row_no", "narrowed_value", "row_text"};
    const SQLSMALLINT types[]{SQL_INTEGER, SQL_INTEGER, SQL_VARCHAR};
    const SQLULEN sizes[]{10, 10, 8};
    for (SQLUSMALLINT column = 1; column <= 3; ++column) {
      SCOPED_TRACE("column=" + std::to_string(column));
      SQLCHAR name[32]{}; SQLSMALLINT length = 0, type = 0, digits = 0, nullable = 0;
      SQLULEN size = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt_, column, name, sizeof(name),
          &length, &type, &size, &digits, &nullable)) << get_error(SQL_HANDLE_STMT, hstmt_);
      ASSERT_GE(length, 0); ASSERT_LT(static_cast<std::size_t>(length), sizeof(name));
      const std::string owned_name(reinterpret_cast<const char*>(name), static_cast<std::size_t>(length));
      EXPECT_EQ(names[column - 1], owned_name); EXPECT_EQ(types[column - 1], type);
      EXPECT_EQ(sizes[column - 1], size); EXPECT_EQ(0, digits);
      EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable); // Recognition is not nullability.
      if (have_descriptors) {
        const auto& previous = descriptors[column - 1];
        EXPECT_EQ(previous.name, owned_name); EXPECT_EQ(previous.type, type);
        EXPECT_EQ(previous.size, size); EXPECT_EQ(previous.nullable, nullable);
      } else {
        descriptors[column - 1] = {owned_name, type, size, nullable};
      }
    }
    have_descriptors = true;
  };
  struct OwnedRow {
    SQLINTEGER ordinal = 0;
    SQLUSMALLINT status = SQL_ROW_NOROW;
    std::optional<SQLSMALLINT> narrowed;
    std::optional<std::string> text;
  };
  std::array<OwnedRow, 5> owned_rows{};
  const auto fetch_row = [&](unsigned row, bool save) {
    SCOPED_TRACE("row=" + std::to_string(row));
    poison();
    const auto result = row % 2 == 0 ? SQLFetchScroll(hstmt_, SQL_FETCH_NEXT, 0) : SQLFetch(hstmt_);
    if (row == 3) {
      SCOPED_TRACE("column=2 SQLSTATE=" + get_error(SQL_HANDLE_STMT, hstmt_));
      EXPECT_EQ(SQL_ERROR, result); EXPECT_EQ("22003", get_error(SQL_HANDLE_STMT, hstmt_));
      // Existing scalar policy consumes the failing row; it is not whole-row atomic.
      EXPECT_EQ(1u, fetched_); EXPECT_EQ(SQL_ROW_ERROR, row_status_);
      EXPECT_EQ(3, ordinal_.value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(ordinal_.value)), ordinal_length_);
      EXPECT_EQ(-77, narrowed_.value); EXPECT_EQ(93, narrowed_length_);
      EXPECT_TRUE(poisoned(text_.value, sizeof(text_.value))); EXPECT_EQ(93, text_length_);
      if (save) owned_rows[row - 1] = {ordinal_.value, row_status_, std::nullopt, std::nullopt};
    } else {
      ASSERT_EQ(SQL_SUCCESS, result) << get_error(SQL_HANDLE_STMT, hstmt_);
      EXPECT_EQ(1u, fetched_); EXPECT_EQ(SQL_ROW_SUCCESS, row_status_);
      EXPECT_EQ(static_cast<SQLINTEGER>(row), ordinal_.value);
      EXPECT_EQ(static_cast<SQLSMALLINT>(row), narrowed_.value);
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(ordinal_.value)), ordinal_length_);
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(narrowed_.value)), narrowed_length_);
      std::optional<std::string> text;
      if (row == 2 || row == 4) {
        EXPECT_EQ(SQL_NULL_DATA, text_length_);
        EXPECT_TRUE(poisoned(text_.value, sizeof(text_.value)));
      } else {
        const std::string expected = row == 1 ? "one" : "five";
        EXPECT_EQ(static_cast<SQLLEN>(expected.size()), text_length_);
        ASSERT_GE(text_length_, 0); ASSERT_LT(static_cast<std::size_t>(text_length_), sizeof(text_.value));
        EXPECT_EQ('\0', text_.value[static_cast<std::size_t>(text_length_)]);
        text = std::string(text_.value, static_cast<std::size_t>(text_length_));
        EXPECT_EQ(expected, *text);
      }
      if (save) owned_rows[row - 1] = {ordinal_.value, row_status_, narrowed_.value, std::move(text)};
    }
    guards();
  };
  const auto end = [&] {
    std::array<unsigned char, sizeof(ordinal_)> old_ordinal{};
    std::array<unsigned char, sizeof(narrowed_)> old_narrowed{};
    std::array<unsigned char, sizeof(text_)> old_text{};
    std::memcpy(old_ordinal.data(), &ordinal_, sizeof(ordinal_));
    std::memcpy(old_narrowed.data(), &narrowed_, sizeof(narrowed_));
    std::memcpy(old_text.data(), &text_, sizeof(text_));
    const auto old_ordinal_length = ordinal_length_, old_narrowed_length = narrowed_length_, old_text_length = text_length_;
    fetched_ = 99; row_status_ = SQL_ROW_SUCCESS;
    ASSERT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
    EXPECT_EQ(0u, fetched_); EXPECT_EQ(SQL_ROW_NOROW, row_status_);
    EXPECT_EQ(0, std::memcmp(old_ordinal.data(), &ordinal_, sizeof(ordinal_)));
    EXPECT_EQ(0, std::memcmp(old_narrowed.data(), &narrowed_, sizeof(narrowed_)));
    EXPECT_EQ(0, std::memcmp(old_text.data(), &text_, sizeof(text_)));
    EXPECT_EQ(old_ordinal_length, ordinal_length_); EXPECT_EQ(old_narrowed_length, narrowed_length_);
    EXPECT_EQ(old_text_length, text_length_); guards();
  };
  bind_columns();
  if (HasFailure()) return;
  unsigned application_attempts = 0;
  ++application_attempts;
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, query, SQL_NTS)) << get_error(SQL_HANDLE_STMT, hstmt_);
  describe();
  if (HasFailure()) return;
  for (unsigned row = 1; row <= 5; ++row) {
    fetch_row(row, true);
    if (HasFailure()) return;
  }
  end();
  if (HasFailure()) return;
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));

  ++application_attempts;
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, query, SQL_NTS)) << get_error(SQL_HANDLE_STMT, hstmt_);
  describe();
  if (HasFailure()) return;
  for (unsigned row = 1; row <= 2; ++row) {
    fetch_row(row, false);
    if (HasFailure()) return;
  }
  // This discards a buffered result, not remote cancellation.
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_RESET_PARAMS));
  poison();
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt_, 1, SQL_C_SLONG, &ordinal_.value,
      sizeof(ordinal_.value), &ordinal_length_));
  SQLCHAR recovery[] = "SELECT 1 AS row_no";
  ++application_attempts;
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, recovery, SQL_NTS)) << get_error(SQL_HANDLE_STMT, hstmt_);
  SQLSMALLINT count = 0; ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt_, &count)); ASSERT_EQ(1, count);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  EXPECT_EQ(1, ordinal_.value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(ordinal_.value)), ordinal_length_);
  EXPECT_EQ(1u, fetched_); EXPECT_EQ(SQL_ROW_SUCCESS, row_status_); guards();
  if (HasFailure()) return;
  end();
  if (HasFailure()) return;
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_STATUS_PTR, nullptr, 0));
  poison();
  EXPECT_EQ(3u, application_attempts);
  for (unsigned row = 1; row <= 5; ++row) {
    const auto& saved = owned_rows[row - 1];
    EXPECT_EQ(static_cast<SQLINTEGER>(row), saved.ordinal);
    EXPECT_EQ(row == 3 ? SQL_ROW_ERROR : SQL_ROW_SUCCESS, saved.status);
    if (row == 3) {
      EXPECT_FALSE(saved.narrowed); EXPECT_FALSE(saved.text);
    } else {
      ASSERT_TRUE(saved.narrowed); EXPECT_EQ(static_cast<SQLSMALLINT>(row), *saved.narrowed);
      if (row == 2 || row == 4) {
        EXPECT_FALSE(saved.text);
      } else {
        ASSERT_TRUE(saved.text); EXPECT_EQ(row == 1 ? "one" : "five", *saved.text);
      }
    }
  }
  EXPECT_EQ("row_no", descriptors[0].name); EXPECT_EQ("narrowed_value", descriptors[1].name);
  EXPECT_EQ("row_text", descriptors[2].name);
}


class RedshiftUnicodeAliasMetadataRealTest : public RedshiftRealTest {
protected:
  void SetUp() override {
    const char* marker = std::getenv("ODBCPP_REDSHIFT_UNICODE_ALIAS_METADATA_ADMISSION");
    if (marker == nullptr) {
      GTEST_SKIP() << "Unicode alias metadata no-DDL scope is not admitted";
    }
    ASSERT_TRUE(std::string_view(marker) == "unicode-alias-metadata-v1")
        << "Invalid Unicode alias metadata scope marker";
    // FIRST gate, before configuration, handles or connection work.
    RedshiftRealTest::SetUp();
  }
};

TEST_F(RedshiftUnicodeAliasMetadataRealTest, DirectAndPreparedUnicodeAliasNamesAndRecovery) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC, hdbc_);
  // Independent UTF-8 bytes: U+00E9, U+8868 and U+1F600. These are SELECT
  // result aliases, not catalog schema/table/column object-name evidence.
  const std::string expected = "\xc3\xa9\xe8\xa1\xa8\xf0\x9f\x98\x80";
  std::vector<SQLWCHAR> expected_wide{0x00e9, 0x8868};
  if constexpr (sizeof(SQLWCHAR) == 2) {
    expected_wide.push_back(0xd83d); expected_wide.push_back(0xde00);
  } else {
    static_assert(sizeof(SQLWCHAR) == 2 || sizeof(SQLWCHAR) == 4);
    expected_wide.push_back(static_cast<SQLWCHAR>(0x1f600));
  }
  std::array<std::string, 2> owned_names;
  std::array<std::vector<SQLWCHAR>, 2> owned_wide_names;
  const auto trials = [&]() {
    for (const bool prepared : {false, true}) {
      SCOPED_TRACE(prepared);
      std::string query = "SELECT CAST(7 AS INTEGER) AS \""
          "\xc3\xa9\xe8\xa1\xa8\xf0\x9f\x98\x80"
          "\", CAST('ok' AS VARCHAR(8)) AS neighbor";
      if (prepared) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt_, reinterpret_cast<SQLCHAR*>(query.data()), SQL_NTS))
            << get_error(SQL_HANDLE_STMT, hstmt_);
        SQLSMALLINT parameters = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt_, &parameters)); EXPECT_EQ(0, parameters);
        if (HasFailure()) { return; }
        std::fill(query.begin(), query.end(), 'x');
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT, hstmt_);
      } else {
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(query.data()), SQL_NTS))
            << get_error(SQL_HANDLE_STMT, hstmt_);
        std::fill(query.begin(), query.end(), 'x');
      }
      SQLSMALLINT count = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt_, &count)); ASSERT_EQ(2, count);
      std::array<SQLCHAR, 64> narrow; narrow.fill(0x5a);
      std::array<SQLWCHAR, 32> wide; wide.fill(SQLWCHAR{0x5a});
      SQLSMALLINT bytes = -1, units = -1, type = -1, wide_type = -1;
      SQLSMALLINT scale = -1, wide_scale = -1, nullable = -1, wide_nullable = -1;
      SQLULEN size = 99, wide_size = 99;
      ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt_, 1, narrow.data(),
          static_cast<SQLSMALLINT>(narrow.size()), &bytes, &type, &size, &scale, &nullable));
      EXPECT_EQ(9, bytes); EXPECT_EQ(SQL_INTEGER, type); EXPECT_EQ(10u, size); EXPECT_EQ(0, scale);
      EXPECT_EQ(0, std::memcmp(narrow.data(), expected.data(), expected.size()));
      EXPECT_EQ(0, narrow[expected.size()]); EXPECT_EQ(0x5a, narrow[expected.size() + 1]);
      ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(hstmt_, 1, wide.data(),
          static_cast<SQLSMALLINT>(wide.size()), &units, &wide_type, &wide_size, &wide_scale, &wide_nullable));
      EXPECT_EQ(static_cast<SQLSMALLINT>(expected_wide.size()), units);
      EXPECT_TRUE(std::equal(expected_wide.begin(), expected_wide.end(), wide.begin()));
      EXPECT_EQ(SQLWCHAR{0}, wide[expected_wide.size()]);
      EXPECT_EQ(SQLWCHAR{0x5a}, wide[expected_wide.size() + 1]);
      EXPECT_EQ(type, wide_type); EXPECT_EQ(size, wide_size);
      EXPECT_EQ(scale, wide_scale); EXPECT_EQ(nullable, wide_nullable);
      // DescribeColW counts units; ColAttributeW capacity and result count bytes.
      narrow.fill(0x5a); bytes = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(hstmt_, 1, SQL_DESC_NAME, narrow.data(),
          static_cast<SQLSMALLINT>(narrow.size()), &bytes, nullptr));
      EXPECT_EQ(9, bytes); EXPECT_EQ(0, std::memcmp(narrow.data(), expected.data(), expected.size()));
      EXPECT_EQ(0, narrow[expected.size()]); EXPECT_EQ(0x5a, narrow[expected.size() + 1]);
      wide.fill(SQLWCHAR{0x5a}); bytes = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLColAttributeW(hstmt_, 1, SQL_DESC_NAME, wide.data(),
          static_cast<SQLSMALLINT>(sizeof(wide)), &bytes, nullptr));
      EXPECT_EQ(static_cast<SQLSMALLINT>(expected_wide.size() * sizeof(SQLWCHAR)), bytes);
      EXPECT_TRUE(std::equal(expected_wide.begin(), expected_wide.end(), wide.begin()));
      EXPECT_EQ(SQLWCHAR{0}, wide[expected_wide.size()]);
      EXPECT_EQ(SQLWCHAR{0x5a}, wide[expected_wide.size() + 1]);
      wide.fill(SQLWCHAR{0x5a}); units = -1;
      ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLDescribeColW(hstmt_, 1, wide.data(), 2,
          &units, nullptr, nullptr, nullptr, nullptr));
      EXPECT_EQ("01004", get_error(SQL_HANDLE_STMT, hstmt_));
      EXPECT_EQ(static_cast<SQLSMALLINT>(expected_wide.size()), units);
      EXPECT_EQ(SQLWCHAR{0x00e9}, wide[0]); EXPECT_EQ(SQLWCHAR{0}, wide[1]);
      EXPECT_EQ(SQLWCHAR{0x5a}, wide[2]);
      // Re-read the full name after truncation; copy only an asserted bounded buffer.
      wide.fill(SQLWCHAR{0x5a});
      ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(hstmt_, 1, wide.data(),
          static_cast<SQLSMALLINT>(wide.size()), &units, nullptr, nullptr, nullptr, nullptr));
      ASSERT_EQ(static_cast<SQLSMALLINT>(expected_wide.size()), units);
      const auto trial = static_cast<std::size_t>(prepared ? 1 : 0);
      owned_names[trial].assign(reinterpret_cast<const char*>(narrow.data()), expected.size());
      owned_wide_names[trial].assign(wide.begin(), wide.begin() + units);
      narrow.fill(0x5a); wide.fill(SQLWCHAR{0x5a});
      std::array<SQLCHAR, 16> neighbor{};
      ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt_, 2, neighbor.data(),
          static_cast<SQLSMALLINT>(neighbor.size()), &bytes, &type, nullptr, nullptr, nullptr));
      EXPECT_EQ(8, bytes); EXPECT_EQ(0, std::memcmp(neighbor.data(), "neighbor", 9));
      EXPECT_EQ(SQL_VARCHAR, type);
      if (HasFailure()) { return; }
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
      SQLINTEGER value = -99; SQLLEN length = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_SLONG, &value, sizeof(value), &length));
      EXPECT_EQ(7, value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
      char text[8]; std::fill(std::begin(text), std::end(text), '\x5a'); length = -1;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 2, SQL_C_CHAR, text, sizeof(text), &length));
      EXPECT_EQ(2, length); EXPECT_EQ(0, std::memcmp(text, "ok", 3)); EXPECT_EQ('\x5a', text[3]);
      ASSERT_EQ(SQL_NO_DATA, SQLFetch(hstmt_)); ASSERT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt_));
      ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_CLOSE));
    }
  };
  trials(); // Preserve failure and never replay a failed trial.
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_RESET_PARAMS));
  SQLCHAR recovery[] = "SELECT 1";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt_, recovery, SQL_NTS)) << get_error(SQL_HANDLE_STMT, hstmt_);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt_));
  SQLINTEGER value = -99; SQLLEN length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt_, 1, SQL_C_SLONG, &value, sizeof(value), &length));
  EXPECT_EQ(1, value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
  ASSERT_EQ(SQL_NO_DATA, SQLFetch(hstmt_)); ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt_));
  if (!HasFailure()) {
    for (std::size_t trial = 0; trial < owned_names.size(); ++trial) {
      EXPECT_EQ(expected, owned_names[trial]); EXPECT_EQ(expected_wide, owned_wide_names[trial]);
    }
  }
}


// Future Unicode catalog-object scope: reuse only. No DDL or fixture authority
// follows from this marker; the controller must separately own the exact objects.
class RedshiftUnicodeCatalogRealTest : public RedshiftRealTest {
protected:
  const std::string schema_ = "odbcpp_u_\xc3\xa9\xe8\xa1\xa8";
  const std::string table_ = "t\"\xc3\xa9%_\xe8\xa1\xa8";
  const std::string id_ = "i\"\xc3\xa9\xe8\xa1\xa8";
  const std::string value_ = "v%_\xc3\xa9\xe8\xa1\xa8";
  const std::string payload_ = "Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x98\x80";
  const std::string schema_pattern_ = "odbcpp\\_u\\_\xc3\xa9\xe8\xa1\xa8";
  const std::string table_pattern_ = "t\"\xc3\xa9\\%\\_\xe8\xa1\xa8";
  const std::string value_pattern_ = "v\\%\\_\xc3\xa9\xe8\xa1\xa8";
  // SQLBindParameter borrows these through statement release in TearDown, even
  // when recovery CLOSE fails before RESET_PARAMS.
  std::vector<SQLWCHAR> bound_input_;
  SQLLEN bound_input_length_ = 0;
  enum class Name { Schema, Table, Id, Value, Payload, SchemaPattern, TablePattern, ValuePattern, TableWildcard, ValueWildcard, Missing };
  // Independent code-point oracle; never decode the UTF-8 expectations above.
  static std::vector<SQLWCHAR> wide(Name name) {
    std::vector<SQLWCHAR> result;
    switch (name) {
      case Name::Schema: result={'o','d','b','c','p','p','_','u','_',0x00e9,0x8868}; break;
      case Name::Table: result={'t','"',0x00e9,'%','_',0x8868}; break;
      case Name::Id: result={'i','"',0x00e9,0x8868}; break;
      case Name::Value: result={'v','%','_',0x00e9,0x8868}; break;
      case Name::Payload: result={'G','r',0x00fc,0x00df,'e',' '}; break;
      case Name::SchemaPattern: result={'o','d','b','c','p','p','\\','_','u','\\','_',0x00e9,0x8868}; break;
      case Name::TablePattern: result={'t','"',0x00e9,'\\','%','\\','_',0x8868}; break;
      case Name::ValuePattern: result={'v','\\','%','\\','_',0x00e9,0x8868}; break;
      case Name::TableWildcard: result={'t','"',0x00e9,'%',0x8868}; break;
      case Name::ValueWildcard: result={'v','%',0x00e9,0x8868}; break;
      case Name::Missing: return {'n','o','_','s','u','c','h','_','o','b','j','e','c','t'};
    }
    // Object-name expectations are BMP-only in this successor. Supplementary
    // characters remain explicit value/parameter oracles, not object-name proof.
    if(name==Name::Payload) {
      if constexpr (sizeof(SQLWCHAR)==2) { result.push_back(0xd83d); result.push_back(0xde00); }
      else { static_assert(sizeof(SQLWCHAR)==2 || sizeof(SQLWCHAR)==4); result.push_back(static_cast<SQLWCHAR>(0x1f600)); }
    }
    return result;
  }
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_UNICODE_CATALOG_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Unicode catalog fixture-reuse scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="unicode-catalog-bmp-fixture-reuse-v1") << "Invalid Unicode catalog scope marker";
    // FIRST gate, before endpoint/configuration/handles/connection work.
    RedshiftRealTest::SetUp();
    if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(settings.contains("DATABASE")); ASSERT_TRUE(settings.contains("UID"));
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");
    ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test");
    ASSERT_FALSE(settings.contains("DSN"));
  }
  static std::string quote(std::string_view name) {
    std::string result="\"";
    for(const char ch:name) { if(ch=='"') { result+='"'; } result+=ch; }
    return result+'"';
  }
  std::string qualified() const { return quote(schema_)+"."+quote(table_); }
  void close_cursor() { ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE)); }
  void recovery() {
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
    SQLCHAR sql[]="SELECT 1";
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,sql,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); number(1,1);
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close_cursor();
  }
  void number(SQLUSMALLINT column,SQLINTEGER expected) {
    struct {SQLINTEGER before{17},value{-99},after{83};} output;
    SQLLEN length=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_SLONG,&output.value,sizeof(output.value),&length));
    EXPECT_EQ(expected,output.value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(output.value)),length);
    EXPECT_EQ(17,output.before); EXPECT_EQ(83,output.after);
  }
  void null_text(SQLUSMALLINT column,bool use_wide) {
    std::array<SQLWCHAR,64> output; output.fill(SQLWCHAR{0x5a}); SQLLEN length=99;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,use_wide?SQL_C_WCHAR:SQL_C_CHAR,
        output.data(),static_cast<SQLLEN>(sizeof(output)),&length));
    EXPECT_EQ(SQL_NULL_DATA,length);
    EXPECT_TRUE(std::all_of(output.begin(),output.end(),[](SQLWCHAR unit){return unit==SQLWCHAR{0x5a};}));
  }
  void text(SQLUSMALLINT column,std::string_view expected,const std::vector<SQLWCHAR>& literal,
      bool use_wide,std::string* owned=nullptr,std::vector<SQLWCHAR>* owned_wide=nullptr) {
    SQLLEN length=-1;
    if(use_wide) {
      std::array<SQLWCHAR,128> output; output.fill(SQLWCHAR{0x5a});
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_WCHAR,output.data(),sizeof(output),&length));
      ASSERT_EQ(static_cast<SQLLEN>(literal.size()*sizeof(SQLWCHAR)),length);
      ASSERT_LT(literal.size()+1,output.size());
      EXPECT_TRUE(std::equal(literal.begin(),literal.end(),output.begin()));
      EXPECT_EQ(SQLWCHAR{0},output[literal.size()]); EXPECT_EQ(SQLWCHAR{0x5a},output[literal.size()+1]);
      if(owned_wide!=nullptr) { owned_wide->assign(output.begin(),output.begin()+static_cast<std::ptrdiff_t>(literal.size())); }
    } else {
      std::array<char,256> output; output.fill('\x5a');
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,output.data(),sizeof(output),&length));
      ASSERT_EQ(static_cast<SQLLEN>(expected.size()),length); ASSERT_LT(expected.size()+1,output.size());
      EXPECT_TRUE(std::string_view(output.data(),expected.size())==expected);
      EXPECT_EQ('\0',output[expected.size()]); EXPECT_EQ('\x5a',output[expected.size()+1]);
      if(owned!=nullptr) { owned->assign(output.data(),static_cast<std::size_t>(length)); }
    }
  }
  struct Descriptor {std::string name;SQLSMALLINT type;SQLULEN size;SQLSMALLINT scale;bool operator==(const Descriptor&)const=default;};
  std::vector<Descriptor> descriptors(bool columns) {
    const CatalogField table_fields[]{{"table_cat",SQL_VARCHAR},{"table_schem",SQL_VARCHAR},{"table_name",SQL_VARCHAR},{"table_type",SQL_VARCHAR},{"remarks",SQL_VARCHAR}};
    const CatalogField column_fields[]{{"table_cat",SQL_VARCHAR},{"table_schem",SQL_VARCHAR},{"table_name",SQL_VARCHAR},{"column_name",SQL_VARCHAR},
      {"data_type",SQL_SMALLINT},{"type_name",SQL_VARCHAR},{"column_size",SQL_INTEGER},{"buffer_length",SQL_INTEGER},{"decimal_digits",SQL_SMALLINT},
      {"num_prec_radix",SQL_SMALLINT},{"nullable",SQL_SMALLINT},{"remarks",SQL_VARCHAR},{"column_def",SQL_VARCHAR},{"sql_data_type",SQL_SMALLINT},
      {"sql_datetime_sub",SQL_SMALLINT},{"char_octet_length",SQL_INTEGER},{"ordinal_position",SQL_INTEGER},{"is_nullable",SQL_VARCHAR}};
    const std::span<const CatalogField> fields=columns?std::span<const CatalogField>(column_fields):std::span<const CatalogField>(table_fields);
    SQLSMALLINT count=-1; const auto status=SQLNumResultCols(hstmt_,&count);
    EXPECT_EQ(SQL_SUCCESS,status); EXPECT_EQ(fields.size(),static_cast<std::size_t>(count));
    if(status!=SQL_SUCCESS || count<0 || static_cast<std::size_t>(count)!=fields.size()) { return {}; }
    std::vector<Descriptor> owned;
    for(SQLUSMALLINT column=1;column<=fields.size();++column) {
      std::array<SQLCHAR,64> name;name.fill(0x5a);SQLSMALLINT length=-1,type=-1,scale=-1,nullable=-1;SQLULEN size=99;
      const auto described=SQLDescribeCol(hstmt_,column,name.data(),static_cast<SQLSMALLINT>(name.size()),&length,&type,&size,&scale,&nullable);
      EXPECT_EQ(SQL_SUCCESS,described);EXPECT_GE(length,0);EXPECT_LT(length,static_cast<SQLSMALLINT>(name.size()));
      if(described!=SQL_SUCCESS || length<0 || static_cast<std::size_t>(length)>=name.size()) { return {}; }
      std::string actual(reinterpret_cast<const char*>(name.data()),static_cast<std::size_t>(length));
      std::transform(actual.begin(),actual.end(),actual.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});
      EXPECT_TRUE(actual==fields[column-1].name);EXPECT_EQ(fields[column-1].type,type);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
      if(type==SQL_SMALLINT) { EXPECT_EQ(5u,size); }
      else if(type==SQL_INTEGER) { EXPECT_EQ(10u,size); }
      else { EXPECT_LE(size,65535u); }
      EXPECT_EQ(0,name[static_cast<std::size_t>(length)]);
      owned.push_back({std::move(actual),type,size,scale});
    }
    return owned;
  }
  SQLRETURN tables(bool use_wide,std::string pattern,Name wide_pattern) {
    std::string schema=schema_pattern_;SQLCHAR database[]="odbcpp_pilot",types[]="TABLE";
    auto ws=wide(Name::SchemaPattern),wt=wide(wide_pattern);ws.push_back(0);wt.push_back(0);
    SQLWCHAR wd[]{'o','d','b','c','p','p','_','p','i','l','o','t',0},wtypes[]{'T','A','B','L','E',0};
    const auto result=use_wide?SQLTablesW(hstmt_,wd,SQL_NTS,ws.data(),SQL_NTS,wt.data(),SQL_NTS,wtypes,SQL_NTS)
        :SQLTables(hstmt_,database,SQL_NTS,reinterpret_cast<SQLCHAR*>(schema.data()),SQL_NTS,reinterpret_cast<SQLCHAR*>(pattern.data()),SQL_NTS,types,SQL_NTS);
    std::fill(schema.begin(),schema.end(),'!');std::fill(pattern.begin(),pattern.end(),'!');ws.assign(ws.size(),'!');wt.assign(wt.size(),'!');
    return result;
  }
  SQLRETURN columns(bool use_wide,const std::optional<std::string>& pattern=std::nullopt,Name wide_pattern=Name::ValuePattern) {
    std::string schema=schema_pattern_,table=table_pattern_;auto column=pattern;
    SQLCHAR database[]="odbcpp_pilot";auto ws=wide(Name::SchemaPattern),wt=wide(Name::TablePattern),wc=wide(wide_pattern);
    ws.push_back(0);wt.push_back(0);wc.push_back(0);SQLWCHAR wd[]{'o','d','b','c','p','p','_','p','i','l','o','t',0};
    const auto result=use_wide?SQLColumnsW(hstmt_,wd,SQL_NTS,ws.data(),SQL_NTS,wt.data(),SQL_NTS,column?wc.data():nullptr,column?SQL_NTS:0)
        :SQLColumns(hstmt_,database,SQL_NTS,reinterpret_cast<SQLCHAR*>(schema.data()),SQL_NTS,reinterpret_cast<SQLCHAR*>(table.data()),SQL_NTS,
            column?reinterpret_cast<SQLCHAR*>(column->data()):nullptr,column?SQL_NTS:0);
    std::fill(schema.begin(),schema.end(),'!');std::fill(table.begin(),table.end(),'!');
    if(column) { std::fill(column->begin(),column->end(),'!'); }ws.assign(ws.size(),'!');wt.assign(wt.size(),'!');wc.assign(wc.size(),'!');
    return result;
  }
  void table_row(bool use_wide,std::string* discovered_schema=nullptr,std::string* discovered_table=nullptr) {
    text(1,"odbcpp_pilot",{'o','d','b','c','p','p','_','p','i','l','o','t'},use_wide);
    text(2,schema_,wide(Name::Schema),use_wide,discovered_schema);
    text(3,table_,wide(Name::Table),use_wide,discovered_table);
    text(4,"TABLE",{'T','A','B','L','E'},use_wide);null_text(5,use_wide);
  }
  void column_row(bool value_column,bool use_wide,std::string* discovered=nullptr) {
    text(1,"odbcpp_pilot",{'o','d','b','c','p','p','_','p','i','l','o','t'},use_wide);
    text(2,schema_,wide(Name::Schema),use_wide);text(3,table_,wide(Name::Table),use_wide);
    text(4,value_column?value_:id_,wide(value_column?Name::Value:Name::Id),use_wide,discovered);
    number(5,value_column?SQL_VARCHAR:SQL_INTEGER);
    text(6,value_column?"character varying":"integer",value_column?std::vector<SQLWCHAR>{'c','h','a','r','a','c','t','e','r',' ','v','a','r','y','i','n','g'}:std::vector<SQLWCHAR>{'i','n','t','e','g','e','r'},use_wide);
    number(7,value_column?32:10);number(8,value_column?32:4);
    if(value_column) { null_text(9,use_wide);null_text(10,use_wide); } else { number(9,0);number(10,10); }
    number(11,SQL_NULLABLE);null_text(12,use_wide);null_text(13,use_wide);number(14,value_column?SQL_VARCHAR:SQL_INTEGER);null_text(15,use_wide);
    if(value_column) { number(16,32); } else { null_text(16,use_wide); }
    number(17,value_column?2:1);text(18,"YES",{'Y','E','S'},use_wide);
  }
};

TEST_F(RedshiftUnicodeCatalogRealTest, FixedQuotedObjectsAndUnicodeNullRows) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
  std::string owned_value;std::vector<SQLWCHAR> owned_wide;
  const auto trial=[&]() {
    auto sql="SELECT "+quote(id_)+","+quote(value_)+","+quote(value_)+" FROM "+qualified()+" ORDER BY "+quote(id_);
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    std::fill(sql.begin(),sql.end(),'!');SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(3,count);
    for(SQLUSMALLINT column=1;column<=3;++column) {
      std::array<SQLCHAR,64> name;name.fill(0x5a);SQLSMALLINT length=-1,type=-1,scale=-1;SQLULEN size=99;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name.data(),static_cast<SQLSMALLINT>(name.size()),&length,&type,&size,&scale,nullptr));
      const auto& expected=column==1?id_:value_;ASSERT_EQ(static_cast<SQLSMALLINT>(expected.size()),length);
      EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(name.data()),expected.size())==expected);
      EXPECT_EQ(0,name[expected.size()]);EXPECT_EQ(0x5a,name[expected.size()+1]);
      EXPECT_EQ(column==1?SQL_INTEGER:SQL_VARCHAR,type);EXPECT_EQ(column==1?10u:32u,size);EXPECT_EQ(0,scale);
      std::array<SQLWCHAR,64> wname;wname.fill(SQLWCHAR{0x5a});auto literal=wide(column==1?Name::Id:Name::Value);
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeColW(hstmt_,column,wname.data(),static_cast<SQLSMALLINT>(wname.size()),&length,nullptr,nullptr,nullptr,nullptr));
      EXPECT_EQ(static_cast<SQLSMALLINT>(literal.size()),length);EXPECT_TRUE(std::equal(literal.begin(),literal.end(),wname.begin()));
      EXPECT_EQ(SQLWCHAR{0},wname[literal.size()]);EXPECT_EQ(SQLWCHAR{0x5a},wname[literal.size()+1]);
    }
    if(HasFailure()) { return; }
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));number(1,1);text(2,payload_,wide(Name::Payload),false,&owned_value);text(3,payload_,wide(Name::Payload),true,nullptr,&owned_wide);
    ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));number(1,2);null_text(2,false);null_text(3,true);
    ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();
  };
  trial();recovery();
  if(!HasFailure()) { EXPECT_TRUE(owned_value==payload_);EXPECT_EQ(wide(Name::Payload),owned_wide); }
}

TEST_F(RedshiftUnicodeCatalogRealTest, AnsiWideCatalogIdentitiesTypesAndOrdinals) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
  std::vector<Descriptor> owned_tables,owned_columns;
  const auto trials=[&]() {
    for(bool use_wide:{false,true}) {
      SCOPED_TRACE(use_wide);
      ASSERT_EQ(SQL_SUCCESS,tables(use_wide,table_pattern_,Name::TablePattern));
      const auto td=descriptors(false);ASSERT_EQ(5u,td.size());ASSERT_FALSE(HasFailure());
      if(use_wide) { EXPECT_EQ(owned_tables,td); } else { owned_tables=td; }
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));table_row(use_wide);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();
      ASSERT_EQ(SQL_SUCCESS,columns(use_wide));const auto cd=descriptors(true);ASSERT_EQ(18u,cd.size());ASSERT_FALSE(HasFailure());
      if(use_wide) { EXPECT_EQ(owned_columns,cd); } else { owned_columns=cd; }
      for(bool value_column:{false,true}) { ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));column_row(value_column,use_wide);ASSERT_FALSE(HasFailure()); }
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();
    }
  };
  trials();recovery();
  if(!HasFailure()) { EXPECT_EQ(5u,owned_tables.size());EXPECT_EQ(18u,owned_columns.size()); }
}

TEST_F(RedshiftUnicodeCatalogRealTest, EscapedWildcardsAndNoMatchKeepUnicodeIdentity) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
  const auto trials=[&]() {
    struct TablePattern {bool use_wide;std::string pattern;Name literal;bool matches;};
    const TablePattern table_patterns[]{{false,table_pattern_,Name::TablePattern,true},
      {true,"t\"\xc3\xa9%\xe8\xa1\xa8",Name::TableWildcard,true},
      {false,"no_such_object",Name::Missing,false}};
    for(const auto& pattern:table_patterns) {
      ASSERT_EQ(SQL_SUCCESS,tables(pattern.use_wide,pattern.pattern,pattern.literal));ASSERT_EQ(5u,descriptors(false).size());ASSERT_FALSE(HasFailure());
      if(pattern.matches) { ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));table_row(pattern.use_wide);ASSERT_FALSE(HasFailure()); }
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();
    }
    const TablePattern column_patterns[]{{false,value_pattern_,Name::ValuePattern,true},
      {true,"v%\xc3\xa9\xe8\xa1\xa8",Name::ValueWildcard,true},
      {true,"no_such_object",Name::Missing,false}};
    for(const auto& pattern:column_patterns) {
      ASSERT_EQ(SQL_SUCCESS,columns(pattern.use_wide,pattern.pattern,pattern.literal));ASSERT_EQ(18u,descriptors(true).size());ASSERT_FALSE(HasFailure());
      if(pattern.matches) { ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));column_row(true,pattern.use_wide);ASSERT_FALSE(HasFailure()); }
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();
    }
  };
  trials();recovery();
}

TEST_F(RedshiftUnicodeCatalogRealTest, DiscoveredQuotedIdentifiersPrepareUnicodeNullAndRecover) {
  ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
  std::string discovered_schema,discovered_table;std::array<std::string,2> discovered_columns;
  std::array<std::string,2> owned_values;std::array<std::vector<SQLWCHAR>,2> owned_markers;
  const auto trials=[&]() {
    ASSERT_EQ(SQL_SUCCESS,tables(false,table_pattern_,Name::TablePattern));ASSERT_EQ(5u,descriptors(false).size());
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));table_row(false,&discovered_schema,&discovered_table);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();
    ASSERT_EQ(SQL_SUCCESS,columns(false));ASSERT_EQ(18u,descriptors(true).size());
    for(std::size_t column=0;column<2;++column) { ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));column_row(column==1,false,&discovered_columns[column]);ASSERT_FALSE(HasFailure()); }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();
    ASSERT_TRUE(discovered_schema==schema_);ASSERT_TRUE(discovered_table==table_);ASSERT_TRUE(discovered_columns[0]==id_);ASSERT_TRUE(discovered_columns[1]==value_);
    auto sql="SELECT "+quote(discovered_columns[0])+","+quote(discovered_columns[1])+",CAST(? AS VARCHAR(32)) AS marker FROM "+
      quote(discovered_schema)+"."+quote(discovered_table)+" ORDER BY "+quote(discovered_columns[0]);
    ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS));
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&count));ASSERT_EQ(1,count);std::fill(sql.begin(),sql.end(),'!');
    for(std::size_t trial=0;trial<2;++trial) {
      bound_input_=wide(Name::Payload);bound_input_length_=static_cast<SQLLEN>(bound_input_.size()*sizeof(SQLWCHAR));bound_input_.push_back(0);
      if(trial==1) { bound_input_.assign(bound_input_.size(),SQLWCHAR{0x5a});bound_input_length_=SQL_NULL_DATA; }
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_WCHAR,SQL_VARCHAR,32,0,bound_input_.data(),static_cast<SQLLEN>(bound_input_.size()*sizeof(SQLWCHAR)),&bound_input_length_));
      ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_));std::fill(bound_input_.begin(),bound_input_.end(),SQLWCHAR{0x5a});
      ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(3,count);
      for(SQLINTEGER row=1;row<=2;++row) {
        ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));number(1,row);
        if(row==1) { text(2,payload_,wide(Name::Payload),false,&owned_values[trial]); }
        else { null_text(2,false); }
        if(trial==0) { text(3,payload_,wide(Name::Payload),true,nullptr,&owned_markers[static_cast<std::size_t>(row-1)]); }
        else { null_text(3,true); }
        ASSERT_FALSE(HasFailure());
      }
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));close_cursor();
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
    }
  };
  trials();recovery();
  if(!HasFailure()) {
    EXPECT_TRUE(discovered_schema==schema_);EXPECT_TRUE(discovered_table==table_);EXPECT_TRUE(discovered_columns[0]==id_);EXPECT_TRUE(discovered_columns[1]==value_);
    for(const auto& value:owned_values) { EXPECT_TRUE(value==payload_); }
    for(const auto& marker:owned_markers) { EXPECT_EQ(wide(Name::Payload),marker); }
  }
}


// Operational native scenarios are separately admitted, fixture-free and no-DDL.
// Happy native queries do not prove deadline interruption/cancellation/streaming.
class RedshiftOperationalRealTest : public RedshiftRealTest {
protected:
  rs::util::Deadline window_end_{};
  bool admitted_ = false;
  bool manual_mode_ = false;
  bool rollback_attempted_ = false, rollback_completed_ = false;
  struct IntCell { std::array<unsigned char,8> before; SQLINTEGER value; std::array<unsigned char,8> after; } ordinal_{};
  struct BigIntCell { std::array<unsigned char,8> before; SQLBIGINT value; std::array<unsigned char,8> after; } total_{};
  struct TextCell { std::array<unsigned char,8> before; std::array<char,33> value; std::array<unsigned char,8> after; } text_{};
  SQLLEN ordinal_length_ = 93, text_length_ = 93, total_length_ = 93;
  SQLULEN fetched_ = 99;
  SQLUSMALLINT row_status_ = SQL_ROW_NOROW;

  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_OPERATIONAL_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Operational recovery scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="operational-recovery-v1") << "Invalid operational scope marker";
    // FIRST gate, before clock/configuration/handles/connection work.
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};
    admitted_=true;
    RedshiftRealTest::SetUp();
    if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(settings.contains("DATABASE"));ASSERT_TRUE(settings.contains("UID"));
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");
    ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test");ASSERT_FALSE(settings.contains("DSN"));
  }
  bool cap(rs::util::Deadline cutoff,bool statement) {
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(cutoff-rs::util::Clock::now()).count();
    if(remaining<=0) { ADD_FAILURE() << "Operational finite window expired; no new operation admitted";return false; }
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5));
    if(SQLSetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_DBC,hdbc_);return false;
    }
    if(statement&&SQLSetStmtAttr(hstmt_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_STMT,hstmt_);return false;
    }
    return true;
  }
  void connect_bounded() {
    ASSERT_TRUE(cap(window_end_,false));
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();
    ASSERT_GT(remaining,0);
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5));
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0));
    ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);
    ASSERT_TRUE(cap(window_end_,true));
  }
  void read_scalar(SQLINTEGER expected,SQLINTEGER* owned=nullptr) {
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(1,count);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    struct {SQLINTEGER before{17},value{-99},after{83};} output;
    SQLLEN length=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&output.value,sizeof(output.value),&length));
    EXPECT_EQ(expected,output.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(output.value)),length);
    EXPECT_EQ(17,output.before);EXPECT_EQ(83,output.after);
    if(owned!=nullptr) { *owned=output.value; }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));
  }
  void scalar(const char* sql,SQLINTEGER expected,SQLINTEGER* owned=nullptr) {
    ASSERT_TRUE(cap(window_end_,true));
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    read_scalar(expected,owned);
  }
  void expect_autocommit(SQLUINTEGER expected) {
    SQLUINTEGER actual=99;
    ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_AUTOCOMMIT,&actual,sizeof(actual),nullptr));EXPECT_EQ(expected,actual);
  }
  void manual_off() {
    ASSERT_TRUE(cap(window_end_,false));
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_AUTOCOMMIT,reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_AUTOCOMMIT_OFF}),0));
    manual_mode_=true;rollback_attempted_=false;rollback_completed_=false;expect_autocommit(SQL_AUTOCOMMIT_OFF);
  }
  void finish_manual(SQLSMALLINT action) {
    ASSERT_TRUE(cap(window_end_,false));
    if(action==SQL_ROLLBACK) { rollback_attempted_=true; }
    ASSERT_EQ(SQL_SUCCESS,SQLEndTran(SQL_HANDLE_DBC,hdbc_,action)) << get_error(SQL_HANDLE_DBC,hdbc_);
    if(action==SQL_ROLLBACK) { rollback_completed_=true; }
  }
  void autocommit_on() {
    ASSERT_TRUE(cap(window_end_,false));
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_AUTOCOMMIT,reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_AUTOCOMMIT_ON}),0)) << get_error(SQL_HANDLE_DBC,hdbc_);
    manual_mode_=false;expect_autocommit(SQL_AUTOCOMMIT_ON);
  }
  void TearDown() override {
    const auto cleanup_entry=rs::util::Clock::now();
    if(admitted_) {
      EXPECT_TRUE(cleanup_entry<window_end_) << "Operational case completed after its original finite window";
    }
    // One explicit bounded rollback attempt, never destructor SQL or a retry.
    // Freeze cleanup once and never extend the original per-case window.
    const auto cleanup_end=std::min(window_end_,cleanup_entry+std::chrono::seconds{5});
    bool safe_disconnect=!manual_mode_||rollback_completed_;
    if(manual_mode_&&connected_&&!rollback_attempted_) {
      if(cap(cleanup_end,false)) {
        rollback_attempted_=true;
        const auto result=SQLEndTran(SQL_HANDLE_DBC,hdbc_,SQL_ROLLBACK);
        EXPECT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_DBC,hdbc_);
        safe_disconnect=result==SQL_SUCCESS;
        if(safe_disconnect) { manual_mode_=false; }
      }
    }
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_&&safe_disconnect) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
      if(connected_&&!safe_disconnect) {
        ADD_FAILURE() << "Rollback cleanup unresolved; disconnect not asserted; outer principal cleanup required";
      }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
  }
};

TEST_F(RedshiftOperationalRealTest, DirectPreparedScalarsAndExplicitFreshConnection) {
  connect_bounded();ASSERT_FALSE(HasFailure());
  SQLINTEGER first=-99,second=-99;
  scalar("SELECT CAST(1 AS INTEGER)",1,&first);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;
  ASSERT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));connected_=false;
  connect_bounded();ASSERT_FALSE(HasFailure());
  ASSERT_TRUE(cap(window_end_,true));SQLCHAR query[]="SELECT CAST(2 AS INTEGER)";
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,query,SQL_NTS));
  SQLSMALLINT parameters=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&parameters));ASSERT_EQ(0,parameters);
  std::fill(std::begin(query),std::end(query),'!');
  ASSERT_TRUE(cap(window_end_,true));ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
  read_scalar(2,&second);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
  EXPECT_EQ(1,first);EXPECT_EQ(2,second);
  // Native happy execution is not native deadline/timeout or automatic reconnect proof.
}

TEST_F(RedshiftOperationalRealTest, AutocommitOffCommitAndOnTransitionRecover) {
  connect_bounded();ASSERT_FALSE(HasFailure());manual_off();ASSERT_FALSE(HasFailure());
  scalar("SELECT CAST(1 AS INTEGER)",1);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(SQL_ERROR,SQLDisconnect(hdbc_));EXPECT_EQ("25000",get_error(SQL_HANDLE_DBC,hdbc_));
  ASSERT_FALSE(HasFailure());
  finish_manual(SQL_COMMIT);ASSERT_FALSE(HasFailure());expect_autocommit(SQL_AUTOCOMMIT_OFF);ASSERT_FALSE(HasFailure());
  scalar("SELECT CAST(2 AS INTEGER)",2);ASSERT_FALSE(HasFailure());
  autocommit_on();ASSERT_FALSE(HasFailure());
  scalar("SELECT CAST(3 AS INTEGER)",3);ASSERT_FALSE(HasFailure());
}

TEST_F(RedshiftOperationalRealTest, FailedTransactionRollsBackResetsAndRecovers) {
  connect_bounded();ASSERT_FALSE(HasFailure());manual_off();ASSERT_FALSE(HasFailure());
  ASSERT_TRUE(cap(window_end_,true));SQLCHAR failing[]="SELECT CAST(1 AS INTEGER)/CAST(0 AS INTEGER)";
  EXPECT_EQ(SQL_ERROR,SQLExecDirect(hstmt_,failing,SQL_NTS));EXPECT_EQ("22012",get_error(SQL_HANDLE_STMT,hstmt_));
  ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));
  ASSERT_TRUE(cap(window_end_,true));SQLCHAR blocked[]="SELECT CAST(1 AS INTEGER)";
  EXPECT_EQ(SQL_ERROR,SQLExecDirect(hstmt_,blocked,SQL_NTS));EXPECT_EQ("25P02",get_error(SQL_HANDLE_STMT,hstmt_));
  ASSERT_FALSE(HasFailure());finish_manual(SQL_ROLLBACK);ASSERT_FALSE(HasFailure());
  autocommit_on();ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
  scalar("SELECT CAST(1 AS INTEGER)",1);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;
  ASSERT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));connected_=false;
}

TEST_F(RedshiftOperationalRealTest, BufferedThousandRowsEarlyCloseAndSameStatementRecovery) {
  connect_bounded();ASSERT_FALSE(HasFailure());
  SQLCHAR query[]=
    "WITH digit AS (SELECT 0 AS d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 "
    "UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9), "
    "numbered AS (SELECT h.d*100+t.d*10+u.d+1 AS n FROM digit h CROSS JOIN digit t CROSS JOIN digit u) "
    "SELECT CAST(n AS INTEGER) AS row_no, CAST(CASE WHEN n%5=0 THEN NULL ELSE 'bounded-row' END AS VARCHAR(32)) AS payload, "
    "COUNT(*) OVER() AS total_rows "
    "FROM numbered ORDER BY n";
  ASSERT_TRUE(cap(window_end_,true));ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,query,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
  std::fill(std::begin(query),std::end(query),'!');
  // This driver's SQLRowCount was observed as zero for this SELECT; it does
  // not measure result cardinality. The window count proves all 1,000 rows
  // without another SQL statement or relying on a raw server completion tag.
  SQLLEN count=-1;ASSERT_EQ(SQL_SUCCESS,SQLRowCount(hstmt_,&count));EXPECT_EQ(0,count);
  SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(3,columns);
  const char* names[]{"row_no","payload","total_rows"};const SQLSMALLINT types[]{SQL_INTEGER,SQL_VARCHAR,SQL_BIGINT};const SQLULEN sizes[]{10,32,19};
  std::array<std::string,3> owned_names;
  for(SQLUSMALLINT column=1;column<=3;++column) {
    std::array<SQLCHAR,32> name;name.fill(0x5a);SQLSMALLINT length=-1,type=-1,scale=-1;SQLULEN size=99;
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name.data(),static_cast<SQLSMALLINT>(name.size()),&length,&type,&size,&scale,nullptr));
    ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length)+1,name.size());
    owned_names[column-1].assign(reinterpret_cast<const char*>(name.data()),static_cast<std::size_t>(length));
    EXPECT_EQ(names[column-1],owned_names[column-1]);EXPECT_EQ(types[column-1],type);EXPECT_EQ(sizes[column-1],size);EXPECT_EQ(0,scale);
    EXPECT_EQ(0,name[static_cast<std::size_t>(length)]);EXPECT_EQ(0x5a,name[static_cast<std::size_t>(length)+1]);
  }
  ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_ARRAY_SIZE,reinterpret_cast<SQLPOINTER>(std::uintptr_t{1}),0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,&fetched_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,&row_status_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_SLONG,&ordinal_.value,sizeof(ordinal_.value),&ordinal_length_));
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,2,SQL_C_CHAR,text_.value.data(),static_cast<SQLLEN>(text_.value.size()),&text_length_));
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,3,SQL_C_SBIGINT,&total_.value,sizeof(total_.value),&total_length_));
  const std::array<SQLINTEGER,17> literal_ids{1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17};
  const std::array<bool,17> literal_nulls{false,false,false,false,true,false,false,false,false,true,false,false,false,false,true,false,false};
  struct OwnedRow {SQLINTEGER id;std::optional<std::string> value;SQLBIGINT total;};std::array<OwnedRow,17> owned;
  for(std::size_t row=0;row<literal_ids.size();++row) {
    ordinal_.before.fill(0x5a);ordinal_.after.fill(0x5a);ordinal_.value=-99;ordinal_length_=93;
    total_.before.fill(0x5a);total_.after.fill(0x5a);total_.value=-99;total_length_=93;
    text_.before.fill(0x5a);text_.after.fill(0x5a);text_.value.fill('\x5a');text_length_=93;fetched_=99;row_status_=SQL_ROW_NOROW;
    ASSERT_TRUE(cap(window_end_,true));ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    EXPECT_EQ(1u,fetched_);EXPECT_EQ(SQL_ROW_SUCCESS,row_status_);EXPECT_EQ(literal_ids[row],ordinal_.value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(ordinal_.value)),ordinal_length_);
    EXPECT_EQ(1000,total_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(total_.value)),total_length_);
    for(const auto* guard:{&ordinal_.before,&ordinal_.after,&text_.before,&text_.after,&total_.before,&total_.after}) {
      EXPECT_TRUE(std::all_of(guard->begin(),guard->end(),[](unsigned char byte){return byte==0x5a;}));
    }
    owned[row].id=ordinal_.value;
    owned[row].total=total_.value;
    if(literal_nulls[row]) {
      EXPECT_EQ(SQL_NULL_DATA,text_length_);EXPECT_TRUE(std::all_of(text_.value.begin(),text_.value.end(),[](char ch){return ch=='\x5a';}));
      owned[row].value.reset();
    } else {
      ASSERT_EQ(11,text_length_);EXPECT_EQ(0,std::memcmp(text_.value.data(),"bounded-row",12));EXPECT_EQ('\x5a',text_.value[12]);
      owned[row].value=std::string(text_.value.data(),11);
    }
    ASSERT_FALSE(HasFailure());
  }
  ASSERT_EQ(SQL_SUCCESS,SQLRowCount(hstmt_,&count));EXPECT_EQ(0,count);
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,nullptr,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,nullptr,0));
  std::memset(&ordinal_,0x5a,sizeof(ordinal_));std::memset(&text_,0x5a,sizeof(text_));std::memset(&total_,0x5a,sizeof(total_));
  scalar("SELECT CAST(1 AS INTEGER)",1);ASSERT_FALSE(HasFailure());
  EXPECT_EQ("row_no",owned_names[0]);EXPECT_EQ("payload",owned_names[1]);EXPECT_EQ("total_rows",owned_names[2]);
  for(std::size_t row=0;row<owned.size();++row) {
    EXPECT_EQ(literal_ids[row],owned[row].id);EXPECT_EQ(literal_nulls[row],!owned[row].value.has_value());
    EXPECT_EQ(1000,owned[row].total);
    if(!literal_nulls[row]) { ASSERT_TRUE(owned[row].value);EXPECT_EQ("bounded-row",*owned[row].value); }
  }
  // Only17 fetched rows plus server window-count1000: no streaming/peak-memory proof.
}

// These cases qualify the existing buffered facade, not streaming or cancellation.
class RedshiftBufferedLifecycleRealTest : public RedshiftRealTest {
protected:
  rs::util::Deadline window_end_{};
  bool admitted_ = false;
  SQLHSTMT second_ = nullptr;
  struct IntegerCell { std::array<unsigned char,8> before; SQLINTEGER value; std::array<unsigned char,8> after; } integer_{};
  struct TextCell { std::array<unsigned char,8> before; std::array<char,64> value; std::array<unsigned char,8> after; } text_{};
  std::array<char,64> parameter_{};
  SQLINTEGER integer_parameter_ = 73;
  SQLLEN parameter_length_ = 0, output_length_ = 97;
  SQLULEN rows_fetched_ = 99, parameters_processed_ = 99;
  SQLUSMALLINT row_status_ = SQL_ROW_NOROW, parameter_status_ = SQL_PARAM_UNUSED;

  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_BUFFERED_LIFECYCLE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Buffered lifecycle scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="operational-buffered-lifecycle-v1") << "Invalid buffered lifecycle scope marker";
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();
    if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(settings.contains("DATABASE"));ASSERT_TRUE(settings.contains("UID"));
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test");ASSERT_FALSE(settings.contains("DSN"));
  }
  bool cap(rs::util::Deadline cutoff,SQLHSTMT statement=nullptr) {
    const auto left=std::chrono::duration_cast<std::chrono::seconds>(cutoff-rs::util::Clock::now()).count();
    if(left<=0) { ADD_FAILURE() << "Buffered lifecycle finite window expired";return false; }
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(left,5));
    if(SQLSetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_DBC,hdbc_);return false;
    }
    if(statement&&SQLSetStmtAttr(statement,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_STMT,statement);return false;
    }
    return true;
  }
  void connect_bounded() {
    ASSERT_TRUE(cap(window_end_));
    const auto left=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(left,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(left,5))),0));
    ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);ASSERT_TRUE(cap(window_end_,hstmt_));
    SQLUINTEGER mode=99;ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_AUTOCOMMIT,&mode,sizeof(mode),nullptr));ASSERT_EQ(SQL_AUTOCOMMIT_ON,mode);
  }
  void direct(SQLHSTMT statement,const char* sql) {
    ASSERT_TRUE(cap(window_end_,statement));
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(statement,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS)) << get_error(SQL_HANDLE_STMT,statement);
  }
  void prepare(SQLHSTMT statement,const char* sql) {
    ASSERT_TRUE(cap(window_end_,statement));
    ASSERT_EQ(SQL_SUCCESS,SQLPrepare(statement,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS)) << get_error(SQL_HANDLE_STMT,statement);
  }
  void execute(SQLHSTMT statement) {
    ASSERT_TRUE(cap(window_end_,statement));ASSERT_EQ(SQL_SUCCESS,SQLExecute(statement)) << get_error(SQL_HANDLE_STMT,statement);
  }
  void metadata(SQLHSTMT statement,SQLUSMALLINT column,const char* name,SQLSMALLINT type,SQLULEN width,std::string* owned=nullptr) {
    struct {std::array<unsigned char,8> before;std::array<SQLCHAR,64> value;std::array<unsigned char,8> after;} output;
    output.before.fill(0x5a);output.after.fill(0x5a);output.value.fill(0x5a);
    SQLSMALLINT length=-1,actual_type=-1,scale=-1,nullable=-1;SQLULEN size=99;
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(statement,column,output.value.data(),static_cast<SQLSMALLINT>(output.value.size()),&length,&actual_type,&size,&scale,&nullable));
    ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length),output.value.size());
    EXPECT_TRUE(std::string(reinterpret_cast<char*>(output.value.data()),static_cast<std::size_t>(length))==name);
    EXPECT_EQ(type,actual_type);EXPECT_EQ(width,size);EXPECT_EQ(0,scale);EXPECT_EQ(0,output.value[static_cast<std::size_t>(length)]);
    for(const auto value:output.before) { EXPECT_EQ(0x5a,value); }
    for(const auto value:output.after) { EXPECT_EQ(0x5a,value); }
    if(owned) { *owned=std::string(reinterpret_cast<char*>(output.value.data()),static_cast<std::size_t>(length)); }
  }
  void integer(SQLHSTMT statement,SQLUSMALLINT column,SQLINTEGER expected,SQLINTEGER* owned=nullptr) {
    integer_.before.fill(0x5a);integer_.after.fill(0x5a);integer_.value=-97;output_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(statement,column,SQL_C_SLONG,&integer_.value,sizeof(integer_.value),&output_length_));
    EXPECT_EQ(expected,integer_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer_.value)),output_length_);
    for(const auto value:integer_.before) { EXPECT_EQ(0x5a,value); }
    for(const auto value:integer_.after) { EXPECT_EQ(0x5a,value); }
    if(owned) { *owned=integer_.value; }
  }
  void text(SQLHSTMT statement,SQLUSMALLINT column,const char* expected,bool nulls,std::string* owned=nullptr) {
    text_.before.fill(0x5a);text_.after.fill(0x5a);text_.value.fill('\x5a');output_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(statement,column,SQL_C_CHAR,text_.value.data(),static_cast<SQLLEN>(text_.value.size()),&output_length_));
    if(nulls) {
      EXPECT_EQ(SQL_NULL_DATA,output_length_);for(const auto value:text_.value) { EXPECT_EQ('\x5a',value); }
    } else {
      const auto length=std::strlen(expected);ASSERT_LT(length,text_.value.size());EXPECT_EQ(static_cast<SQLLEN>(length),output_length_);
      EXPECT_EQ(0,std::memcmp(expected,text_.value.data(),length+1));if(owned) { *owned=std::string(text_.value.data(),length); }
    }
    for(const auto value:text_.before) { EXPECT_EQ(0x5a,value); }
    for(const auto value:text_.after) { EXPECT_EQ(0x5a,value); }
  }
  void scalar(SQLHSTMT statement,SQLINTEGER expected,SQLINTEGER* owned=nullptr) {
    SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(statement,&columns));ASSERT_EQ(1,columns);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(statement));integer(statement,1,expected,owned);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(statement));
  }
  void reset(SQLHSTMT statement) {
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(statement,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(statement,SQL_UNBIND));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(statement,SQL_RESET_PARAMS));
  }
  void TearDown() override {
    const auto entry=rs::util::Clock::now();
    if(admitted_) { EXPECT_TRUE(entry<window_end_) << "Buffered lifecycle case completed after original finite window"; }
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    // Release both statements while every borrowed member still exists.
    if(second_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,second_));second_=nullptr; }
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_&&cap(cleanup_end)) {
        const auto result=SQLDisconnect(hdbc_);
        if(result==SQL_ERROR&&get_error(SQL_HANDLE_DBC,hdbc_)=="25000") {
          // Known active-transaction refusal only; at most one bounded rollback.
          ADD_FAILURE() << "Unexpected transaction at buffered lifecycle cleanup";
          if(cap(cleanup_end)) {
            const auto rollback=SQLEndTran(SQL_HANDLE_DBC,hdbc_,SQL_ROLLBACK);EXPECT_EQ(SQL_SUCCESS,rollback) << get_error(SQL_HANDLE_DBC,hdbc_);
            if(rollback==SQL_SUCCESS&&cap(cleanup_end)) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
          }
        } else { EXPECT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_DBC,hdbc_); }
      }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
  }
};

TEST_F(RedshiftBufferedLifecycleRealTest, InterleavedStatementsKeepOwningBufferedResults) {
  connect_bounded();ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&second_));
  direct(hstmt_,"SELECT CAST(1 AS INTEGER) AS a,CAST('alpha' AS VARCHAR(32)) AS text_value UNION ALL SELECT CAST(2 AS INTEGER),CAST(NULL AS VARCHAR(32)) ORDER BY a");ASSERT_FALSE(HasFailure());
  SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(2,columns);
  metadata(hstmt_,1,"a",SQL_INTEGER,10);metadata(hstmt_,2,"text_value",SQL_VARCHAR,32);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));SQLINTEGER saved_a=-1;std::string saved_text;
  integer(hstmt_,1,1,&saved_a);text(hstmt_,2,"alpha",false,&saved_text);ASSERT_FALSE(HasFailure());
  prepare(second_,"SELECT CAST(200 AS INTEGER) AS b");ASSERT_FALSE(HasFailure());execute(second_);ASSERT_FALSE(HasFailure());
  std::string saved_name;metadata(second_,1,"b",SQL_INTEGER,10,&saved_name);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));integer(hstmt_,1,2);text(hstmt_,2,"",true);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));
  reset(hstmt_);ASSERT_FALSE(HasFailure());direct(hstmt_,"SELECT CAST(7 AS INTEGER) AS replacement");ASSERT_FALSE(HasFailure());scalar(hstmt_,7);ASSERT_FALSE(HasFailure());reset(hstmt_);ASSERT_FALSE(HasFailure());
  // Reexecuting A must not replace B's separately owned buffered result/IRD.
  metadata(second_,1,"b",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());SQLINTEGER saved_b=-1;scalar(second_,200,&saved_b);ASSERT_FALSE(HasFailure());reset(second_);ASSERT_FALSE(HasFailure());
  integer_.value=-99;text_.value.fill('\x5a');EXPECT_EQ(1,saved_a);EXPECT_EQ(200,saved_b);EXPECT_TRUE(saved_text=="alpha");EXPECT_TRUE(saved_name=="b");
}

TEST_F(RedshiftBufferedLifecycleRealTest, MaxRowsAndForwardOnlyRefusalsPreservePosition) {
  connect_bounded();ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}),0));
  SQLULEN setting=99;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,&setting,sizeof(setting),nullptr));ASSERT_EQ(2u,setting);
  direct(hstmt_,"SELECT id,text_value,COUNT(*) OVER() AS total_rows FROM (SELECT CAST(1 AS INTEGER) AS id,CAST('one' AS VARCHAR(32)) AS text_value UNION ALL SELECT CAST(2 AS INTEGER),CAST(NULL AS VARCHAR(32)) UNION ALL SELECT CAST(3 AS INTEGER),CAST('three' AS VARCHAR(32))) AS bounded_rows ORDER BY id");ASSERT_FALSE(HasFailure());
  SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(3,columns);
  metadata(hstmt_,1,"id",SQL_INTEGER,10);metadata(hstmt_,2,"text_value",SQL_VARCHAR,32);metadata(hstmt_,3,"total_rows",SQL_BIGINT,19);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,&rows_fetched_,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,&row_status_,0));
  SQLLEN count=-1;ASSERT_EQ(SQL_SUCCESS,SQLRowCount(hstmt_,&count));EXPECT_EQ(0,count);
  for(SQLINTEGER row=1;row<=2;++row) {
    rows_fetched_=99;row_status_=SQL_ROW_NOROW;ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_NEXT,0));
    EXPECT_EQ(1u,rows_fetched_);EXPECT_EQ(SQL_ROW_SUCCESS,row_status_);integer(hstmt_,1,row);text(hstmt_,2,"one",row==2);integer(hstmt_,3,3);ASSERT_FALSE(HasFailure());
    if(row==1) {
      for(const auto orientation:{SQL_FETCH_FIRST,SQL_FETCH_ABSOLUTE}) {
        EXPECT_EQ(SQL_ERROR,SQLFetchScroll(hstmt_,static_cast<SQLSMALLINT>(orientation),1));EXPECT_TRUE(get_error(SQL_HANDLE_STMT,hstmt_)=="HYC00");
      }
      EXPECT_EQ(SQL_ERROR,SQLFetchScroll(hstmt_,static_cast<SQLSMALLINT>(999),0));EXPECT_TRUE(get_error(SQL_HANDLE_STMT,hstmt_)=="HY106");
      ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_ARRAY_SIZE,reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}),0));
      setting=99;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_ROW_ARRAY_SIZE,&setting,sizeof(setting),nullptr));EXPECT_EQ(2u,setting);
      // Restore the existing scalar buffers and next-row advancement contract.
      ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_ARRAY_SIZE,reinterpret_cast<SQLPOINTER>(std::uintptr_t{1}),0));
    }
  }
  rows_fetched_=99;row_status_=SQL_ROW_SUCCESS;ASSERT_EQ(SQL_NO_DATA,SQLFetchScroll(hstmt_,SQL_FETCH_NEXT,0));EXPECT_EQ(0u,rows_fetched_);EXPECT_EQ(SQL_ROW_NOROW,row_status_);
  ASSERT_EQ(SQL_SUCCESS,SQLRowCount(hstmt_,&count));EXPECT_EQ(0,count);reset(hstmt_);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,nullptr,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,nullptr,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,nullptr,0));direct(hstmt_,"SELECT CAST(1 AS INTEGER)");ASSERT_FALSE(HasFailure());scalar(hstmt_,1);ASSERT_FALSE(HasFailure());reset(hstmt_);ASSERT_FALSE(HasFailure());
  // Window count3 proves logical cardinality; MAX_ROWS2 is only local truncation.
}

TEST_F(RedshiftBufferedLifecycleRealTest, ParameterResetRebindAndCrossShapeReprepare) {
  connect_bounded();ASSERT_FALSE(HasFailure());prepare(hstmt_,"SELECT CAST(? AS VARCHAR(32)) AS v");ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,&parameters_processed_,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_STATUS_PTR,&parameter_status_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,32,0,parameter_.data(),static_cast<SQLLEN>(parameter_.size()),&parameter_length_));
  const char* const values[]={("Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x98\x80"),"","changed"};std::string saved;
  for(unsigned row=0;row<3;++row) {
    parameter_.fill('\x5a');const auto length=std::strlen(values[row]);ASSERT_LT(length,parameter_.size());std::memcpy(parameter_.data(),values[row],length+1);
    parameter_length_=row==1?SQL_NULL_DATA:static_cast<SQLLEN>(length);parameters_processed_=99;parameter_status_=SQL_PARAM_UNUSED;
    execute(hstmt_);ASSERT_FALSE(HasFailure());EXPECT_EQ(1u,parameters_processed_);EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status_);
    // The borrowed pointer remains valid, while the executed material is owned.
    parameter_.fill('\x5a');parameter_length_=97;metadata(hstmt_,1,"v",SQL_VARCHAR,32);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));text(hstmt_,1,values[row],row==1,row==0?&saved:nullptr);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  }
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));ASSERT_TRUE(cap(window_end_,hstmt_));parameters_processed_=99;parameter_status_=SQL_PARAM_UNUSED;
  EXPECT_EQ(SQL_ERROR,SQLExecute(hstmt_));EXPECT_TRUE(get_error(SQL_HANDLE_STMT,hstmt_)=="07009");EXPECT_EQ(SQL_PARAM_ERROR,parameter_status_);EXPECT_EQ(1u,parameters_processed_);
  // Native07009 does not measure traffic: source/offline counters own zero-dispatch proof.
  parameter_[0]='x';parameter_[1]=0;parameter_length_=1;
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,32,0,parameter_.data(),static_cast<SQLLEN>(parameter_.size()),&parameter_length_));
  reset(hstmt_);ASSERT_FALSE(HasFailure());prepare(hstmt_,"SELECT CAST(? AS INTEGER) AS i");ASSERT_FALSE(HasFailure());
  parameter_length_=sizeof(integer_parameter_);ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&integer_parameter_,sizeof(integer_parameter_),&parameter_length_));
  execute(hstmt_);ASSERT_FALSE(HasFailure());integer_parameter_=-99;parameter_length_=97;metadata(hstmt_,1,"i",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());
  scalar(hstmt_,73);ASSERT_FALSE(HasFailure());reset(hstmt_);ASSERT_FALSE(HasFailure());EXPECT_TRUE(saved==values[0]);
}

TEST_F(RedshiftBufferedLifecycleRealTest, CompoundResultsAndDeferredErrorRecover) {
  connect_bounded();ASSERT_FALSE(HasFailure());
  direct(hstmt_,"SELECT CAST(1 AS INTEGER) AS first_value; SELECT CAST(2 AS INTEGER) AS second_value");ASSERT_FALSE(HasFailure());
  std::string saved_name;metadata(hstmt_,1,"first_value",SQL_INTEGER,10,&saved_name);ASSERT_FALSE(HasFailure());SQLINTEGER saved_first=-1;scalar(hstmt_,1,&saved_first);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLMoreResults(hstmt_));metadata(hstmt_,1,"second_value",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());scalar(hstmt_,2);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));reset(hstmt_);ASSERT_FALSE(HasFailure());
  direct(hstmt_,"SELECT CAST(3 AS INTEGER) AS retained; SELECT CAST(1 AS INTEGER)/CAST(0 AS INTEGER)");ASSERT_FALSE(HasFailure());
  metadata(hstmt_,1,"retained",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());SQLINTEGER retained=-1;scalar(hstmt_,3,&retained);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(SQL_ERROR,SQLMoreResults(hstmt_));EXPECT_TRUE(get_error(SQL_HANDLE_STMT,hstmt_)=="22012");ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));reset(hstmt_);ASSERT_FALSE(HasFailure());
  direct(hstmt_,"SELECT CAST(1 AS INTEGER)");ASSERT_FALSE(HasFailure());scalar(hstmt_,1);ASSERT_FALSE(HasFailure());reset(hstmt_);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(1,saved_first);EXPECT_EQ(3,retained);EXPECT_TRUE(saved_name=="first_value");
  // A compound-query server rejection is a failure, never split-query fallback.
}


// Fixed common-type boundaries exercise the existing buffered facade only.
class RedshiftCommonTypeBoundariesRealTest : public RedshiftRealTest {
protected:
  rs::util::Deadline window_end_{};
  bool admitted_ = false;
  struct IntegerCell { std::array<unsigned char,8> before; SQLINTEGER value; std::array<unsigned char,8> after; } integer_{};
  struct TextCell { std::array<unsigned char,8> before; std::array<char,64> value; std::array<unsigned char,8> after; } text_{};
  SQLLEN parameter_length_ = 0, output_length_ = 97;
  SQLULEN rows_fetched_ = 99, parameters_processed_ = 99;
  SQLUSMALLINT row_status_ = SQL_ROW_NOROW, parameter_status_ = SQL_PARAM_UNUSED;

  SQLBIGINT bigint_input_ = 0;
  SQLUBIGINT unsigned_input_ = 0;
  SQL_NUMERIC_STRUCT numeric_input_{};
  struct BigintCell { std::array<unsigned char,8> before;SQLBIGINT value;std::array<unsigned char,8> after; } bigint_{};
  struct NumericCell { std::array<unsigned char,8> before;SQL_NUMERIC_STRUCT value;std::array<unsigned char,8> after; } numeric_{};
  struct DateCell { std::array<unsigned char,8> before;SQL_DATE_STRUCT value;std::array<unsigned char,8> after; } date_{};
  struct TimeCell { std::array<unsigned char,8> before;SQL_TIME_STRUCT value;std::array<unsigned char,8> after; } time_{};
  struct StampCell { std::array<unsigned char,8> before;SQL_TIMESTAMP_STRUCT value;std::array<unsigned char,8> after; } stamp_{};
  SQLLEN stamp_length_ = 97;
  SQLHDESC apd_ = SQL_NULL_HDESC,ard_ = SQL_NULL_HDESC;
  static constexpr std::array<SQLCHAR,16> maximum_magnitude_{255,255,255,255,63,34,138,9,122,196,134,90,168,76,59,75};
  static constexpr std::array<SQLCHAR,16> overflow_magnitude_{0,0,0,0,64,34,138,9,122,196,134,90,168,76,59,75};
  template<class Cell> void poison(Cell& cell) {
    cell.before.fill(0x5a);cell.after.fill(0x5a);std::memset(&cell.value,0x5a,sizeof(cell.value));
  }
  template<class Cell> void guards(const Cell& cell) {
    for(const auto value:cell.before) { EXPECT_EQ(0x5a,value); }
    for(const auto value:cell.after) { EXPECT_EQ(0x5a,value); }
  }
  template<class Value> bool poisoned(const Value& value) {
    const auto* bytes=reinterpret_cast<const unsigned char*>(&value);
    return std::all_of(bytes,bytes+sizeof(value),[](unsigned char byte){return byte==0x5a;});
  }
  void bind_parameter_status() {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,&parameters_processed_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_STATUS_PTR,&parameter_status_,0));
  }
  void numeric_ard(SQLSMALLINT precision,SQLSMALLINT scale,bool bound) {
    ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_APP_ROW_DESC,&ard_,0,nullptr));
    if(bound) { ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_NUMERIC,&numeric_.value,sizeof(numeric_.value),&output_length_)); }
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,1,SQL_DESC_CONCISE_TYPE,reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(SQL_C_NUMERIC)),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,1,SQL_DESC_PRECISION,reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(precision)),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,1,SQL_DESC_SCALE,reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(scale)),0));
    // Descriptor edits invalidate DATA_PTR; restore the intended binding explicitly.
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,1,SQL_DESC_DATA_PTR,bound?static_cast<SQLPOINTER>(&numeric_.value):nullptr,0));
  }
  void numeric_input_binding() {
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_NUMERIC,SQL_NUMERIC,38,0,&numeric_input_,sizeof(numeric_input_),&parameter_length_));
    ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_APP_PARAM_DESC,&apd_,0,nullptr));
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(apd_,1,SQL_DESC_PRECISION,reinterpret_cast<SQLPOINTER>(std::intptr_t{38}),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(apd_,1,SQL_DESC_SCALE,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(apd_,1,SQL_DESC_DATA_PTR,&numeric_input_,0));
  }
  void clear_status() {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_STATUS_PTR,nullptr,0));
  }
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_COMMON_TYPE_BOUNDARIES_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Common-type boundaries scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="common-type-boundaries-v1") << "Invalid common-type boundaries scope marker";
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();
    if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(settings.contains("DATABASE"));ASSERT_TRUE(settings.contains("UID"));
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test");ASSERT_FALSE(settings.contains("DSN"));
  }
  bool cap(rs::util::Deadline cutoff,SQLHSTMT statement=nullptr) {
    const auto left=std::chrono::duration_cast<std::chrono::seconds>(cutoff-rs::util::Clock::now()).count();
    if(left<=0) { ADD_FAILURE() << "Common-type boundaries finite window expired";return false; }
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(left,5));
    if(SQLSetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_DBC,hdbc_);return false;
    }
    if(statement&&SQLSetStmtAttr(statement,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_STMT,statement);return false;
    }
    return true;
  }
  void connect_bounded() {
    ASSERT_TRUE(cap(window_end_));
    const auto left=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(left,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(left,5))),0));
    ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);ASSERT_TRUE(cap(window_end_,hstmt_));
    SQLUINTEGER mode=99;ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_AUTOCOMMIT,&mode,sizeof(mode),nullptr));ASSERT_EQ(SQL_AUTOCOMMIT_ON,mode);
  }
  void direct(SQLHSTMT statement,const char* sql) {
    ASSERT_TRUE(cap(window_end_,statement));
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(statement,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS)) << get_error(SQL_HANDLE_STMT,statement);
  }
  void prepare(SQLHSTMT statement,const char* sql) {
    ASSERT_TRUE(cap(window_end_,statement));
    ASSERT_EQ(SQL_SUCCESS,SQLPrepare(statement,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS)) << get_error(SQL_HANDLE_STMT,statement);
  }
  void execute(SQLHSTMT statement) {
    ASSERT_TRUE(cap(window_end_,statement));ASSERT_EQ(SQL_SUCCESS,SQLExecute(statement)) << get_error(SQL_HANDLE_STMT,statement);
  }
  void metadata(SQLHSTMT statement,SQLUSMALLINT column,const char* name,SQLSMALLINT type,SQLULEN width,SQLSMALLINT expected_scale=0,std::string* owned=nullptr) {
    struct {std::array<unsigned char,8> before;std::array<SQLCHAR,64> value;std::array<unsigned char,8> after;} output;
    output.before.fill(0x5a);output.after.fill(0x5a);output.value.fill(0x5a);
    SQLSMALLINT length=-1,actual_type=-1,scale=-1,nullable=-1;SQLULEN size=99;
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(statement,column,output.value.data(),static_cast<SQLSMALLINT>(output.value.size()),&length,&actual_type,&size,&scale,&nullable));
    ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length),output.value.size());
    EXPECT_TRUE(std::string(reinterpret_cast<char*>(output.value.data()),static_cast<std::size_t>(length))==name);
    EXPECT_EQ(type,actual_type);EXPECT_EQ(width,size);EXPECT_EQ(expected_scale,scale);EXPECT_EQ(0,output.value[static_cast<std::size_t>(length)]);
    for(const auto value:output.before) { EXPECT_EQ(0x5a,value); }
    for(const auto value:output.after) { EXPECT_EQ(0x5a,value); }
    if(owned) { *owned=std::string(reinterpret_cast<char*>(output.value.data()),static_cast<std::size_t>(length)); }
  }
  void integer(SQLHSTMT statement,SQLUSMALLINT column,SQLINTEGER expected,SQLINTEGER* owned=nullptr) {
    integer_.before.fill(0x5a);integer_.after.fill(0x5a);integer_.value=-97;output_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(statement,column,SQL_C_SLONG,&integer_.value,sizeof(integer_.value),&output_length_));
    EXPECT_EQ(expected,integer_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer_.value)),output_length_);
    for(const auto value:integer_.before) { EXPECT_EQ(0x5a,value); }
    for(const auto value:integer_.after) { EXPECT_EQ(0x5a,value); }
    if(owned) { *owned=integer_.value; }
  }
  void text(SQLHSTMT statement,SQLUSMALLINT column,const char* expected,bool nulls,std::string* owned=nullptr) {
    text_.before.fill(0x5a);text_.after.fill(0x5a);text_.value.fill('\x5a');output_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(statement,column,SQL_C_CHAR,text_.value.data(),static_cast<SQLLEN>(text_.value.size()),&output_length_));
    if(nulls) {
      EXPECT_EQ(SQL_NULL_DATA,output_length_);for(const auto value:text_.value) { EXPECT_EQ('\x5a',value); }
    } else {
      const auto length=std::strlen(expected);ASSERT_LT(length,text_.value.size());EXPECT_EQ(static_cast<SQLLEN>(length),output_length_);
      EXPECT_EQ(0,std::memcmp(expected,text_.value.data(),length+1));if(owned) { *owned=std::string(text_.value.data(),length); }
    }
    for(const auto value:text_.before) { EXPECT_EQ(0x5a,value); }
    for(const auto value:text_.after) { EXPECT_EQ(0x5a,value); }
  }
  void scalar(SQLHSTMT statement,SQLINTEGER expected,SQLINTEGER* owned=nullptr) {
    SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(statement,&columns));ASSERT_EQ(1,columns);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(statement));integer(statement,1,expected,owned);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(statement));
  }
  void reset(SQLHSTMT statement) {
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(statement,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(statement,SQL_UNBIND));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(statement,SQL_RESET_PARAMS));
  }
  void TearDown() override {
    const auto entry=rs::util::Clock::now();
    if(admitted_) { EXPECT_TRUE(entry<window_end_) << "Common-type boundaries case completed after original finite window"; }
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    // Release the statement while every borrowed member still exists.
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_&&cap(cleanup_end)) {
        const auto result=SQLDisconnect(hdbc_);
        if(result==SQL_ERROR&&get_error(SQL_HANDLE_DBC,hdbc_)=="25000") {
          // Known active-transaction refusal only; at most one bounded rollback.
          ADD_FAILURE() << "Unexpected transaction at common-type boundaries cleanup";
          if(cap(cleanup_end)) {
            const auto rollback=SQLEndTran(SQL_HANDLE_DBC,hdbc_,SQL_ROLLBACK);EXPECT_EQ(SQL_SUCCESS,rollback) << get_error(SQL_HANDLE_DBC,hdbc_);
            if(rollback==SQL_SUCCESS&&cap(cleanup_end)) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
          }
        } else { EXPECT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_DBC,hdbc_); }
      }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
  }
};

TEST_F(RedshiftCommonTypeBoundariesRealTest, PreparedBigintExtremesNullOverflowAndRecovery) {
  connect_bounded();ASSERT_FALSE(HasFailure());
  prepare(hstmt_,"SELECT CAST(? AS BIGINT) AS integer_value");ASSERT_FALSE(HasFailure());bind_parameter_status();ASSERT_FALSE(HasFailure());
  const std::array<SQLBIGINT,2> expected{SQLBIGINT{-9223372036854775807LL}-1,SQLBIGINT{9223372036854775807LL}};
  std::array<SQLBIGINT,2> owned{};std::string owned_name;
  for(std::size_t trial=0;trial<3;++trial) {
    bigint_input_=trial<2?expected[trial]:SQLBIGINT{-73};parameter_length_=trial<2?static_cast<SQLLEN>(sizeof(bigint_input_)):SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SBIGINT,SQL_BIGINT,19,0,&bigint_input_,sizeof(bigint_input_),&parameter_length_));
    parameters_processed_=99;parameter_status_=SQL_PARAM_UNUSED;execute(hstmt_);ASSERT_FALSE(HasFailure());
    EXPECT_EQ(static_cast<SQLULEN>(1),parameters_processed_);EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status_);
    bigint_input_=73;parameter_length_=97; // Execution owns the parameter before caller mutation.
    SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(1,columns);
    metadata(hstmt_,1,"integer_value",SQL_BIGINT,19,0,&owned_name);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));poison(bigint_);output_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SBIGINT,&bigint_.value,sizeof(bigint_.value),&output_length_));guards(bigint_);
    if(trial<2) { EXPECT_EQ(expected[trial],bigint_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(bigint_.value)),output_length_);owned[trial]=bigint_.value; }
    else { EXPECT_EQ(SQL_NULL_DATA,output_length_);EXPECT_TRUE(poisoned(bigint_.value)); }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));
  }
  unsigned_input_=SQLUBIGINT{0x8000000000000000ULL};parameter_length_=static_cast<SQLLEN>(sizeof(unsigned_input_));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_UBIGINT,SQL_BIGINT,19,0,&unsigned_input_,sizeof(unsigned_input_),&parameter_length_));
  parameters_processed_=99;parameter_status_=SQL_PARAM_UNUSED;ASSERT_TRUE(cap(window_end_,hstmt_));
  // This is the checked pre-dispatch unsigned-to-signed refusal, not another SELECT.
  ASSERT_EQ(SQL_ERROR,SQLExecute(hstmt_));EXPECT_EQ("22003",get_error(SQL_HANDLE_STMT,hstmt_));
  EXPECT_EQ(SQLUBIGINT{0x8000000000000000ULL},unsigned_input_);EXPECT_EQ(static_cast<SQLLEN>(sizeof(unsigned_input_)),parameter_length_);EXPECT_EQ(static_cast<SQLULEN>(1),parameters_processed_);EXPECT_EQ(SQL_PARAM_ERROR,parameter_status_);
  bigint_input_=1;parameter_length_=static_cast<SQLLEN>(sizeof(bigint_input_));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SBIGINT,SQL_BIGINT,19,0,&bigint_input_,sizeof(bigint_input_),&parameter_length_));
  execute(hstmt_);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));poison(bigint_);output_length_=97;
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SBIGINT,&bigint_.value,sizeof(bigint_.value),&output_length_));EXPECT_EQ(SQLBIGINT{1},bigint_.value);guards(bigint_);
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));reset(hstmt_);clear_status();ASSERT_FALSE(HasFailure());
  EXPECT_EQ("integer_value",owned_name);EXPECT_EQ(SQLBIGINT{-9223372036854775807LL}-1,owned[0]);EXPECT_EQ(SQLBIGINT{9223372036854775807LL},owned[1]);
}

TEST_F(RedshiftCommonTypeBoundariesRealTest, PreparedDecimal38ExactNullOverflowAndRecovery) {
  connect_bounded();ASSERT_FALSE(HasFailure());prepare(hstmt_,"SELECT CAST(? AS DECIMAL(38,0)) AS amount");ASSERT_FALSE(HasFailure());
  bind_parameter_status();numeric_ard(38,0,true);ASSERT_FALSE(HasFailure());std::array<SQL_NUMERIC_STRUCT,2> owned{};std::string owned_name;
  const std::array<const char*,2> decimal_text{"99999999999999999999999999999999999999","-99999999999999999999999999999999999999"};
  for(std::size_t trial=0;trial<3;++trial) {
    numeric_input_={};numeric_input_.precision=38;numeric_input_.scale=0;numeric_input_.sign=trial==1?SQLCHAR{0}:SQLCHAR{1};
    std::copy(maximum_magnitude_.begin(),maximum_magnitude_.end(),numeric_input_.val);
    parameter_length_=trial<2?static_cast<SQLLEN>(sizeof(numeric_input_)):SQL_NULL_DATA;
    if(trial==2) { std::memset(&numeric_input_,0xff,sizeof(numeric_input_)); }
    numeric_input_binding();ASSERT_FALSE(HasFailure());parameters_processed_=99;parameter_status_=SQL_PARAM_UNUSED;execute(hstmt_);ASSERT_FALSE(HasFailure());
    EXPECT_EQ(static_cast<SQLULEN>(1),parameters_processed_);EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status_);
    std::memset(&numeric_input_,0x5a,sizeof(numeric_input_));parameter_length_=97;
    SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(1,columns);
    metadata(hstmt_,1,"amount",SQL_NUMERIC,38,0,&owned_name);ASSERT_FALSE(HasFailure());poison(numeric_);output_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));guards(numeric_);
    if(trial<2) {
      EXPECT_EQ(38,numeric_.value.precision);EXPECT_EQ(0,numeric_.value.scale);EXPECT_EQ(trial==1?0:1,numeric_.value.sign);
      EXPECT_EQ(0,std::memcmp(maximum_magnitude_.data(),numeric_.value.val,maximum_magnitude_.size()));
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric_.value)),output_length_);owned[trial]=numeric_.value;
      // The bound numeric result and this independent decimal string describe the same row.
      text(hstmt_,1,decimal_text[trial],false);ASSERT_FALSE(HasFailure());
    } else {
      EXPECT_EQ(SQL_NULL_DATA,output_length_);EXPECT_TRUE(poisoned(numeric_.value));
      poison(numeric_);output_length_=97;ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_ARD_TYPE,&numeric_.value,sizeof(numeric_.value),&output_length_));
      EXPECT_EQ(SQL_NULL_DATA,output_length_);EXPECT_TRUE(poisoned(numeric_.value));guards(numeric_);
    }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));
  }
  numeric_input_={};numeric_input_.precision=38;numeric_input_.scale=0;numeric_input_.sign=1;
  std::copy(overflow_magnitude_.begin(),overflow_magnitude_.end(),numeric_input_.val);parameter_length_=static_cast<SQLLEN>(sizeof(numeric_input_));
  numeric_input_binding();ASSERT_FALSE(HasFailure());parameters_processed_=99;parameter_status_=SQL_PARAM_UNUSED;ASSERT_TRUE(cap(window_end_,hstmt_));
  ASSERT_EQ(SQL_ERROR,SQLExecute(hstmt_));EXPECT_EQ("22003",get_error(SQL_HANDLE_STMT,hstmt_));
  EXPECT_EQ(0,std::memcmp(overflow_magnitude_.data(),numeric_input_.val,overflow_magnitude_.size()));EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric_input_)),parameter_length_);EXPECT_EQ(static_cast<SQLULEN>(1),parameters_processed_);EXPECT_EQ(SQL_PARAM_ERROR,parameter_status_);
  numeric_input_={};numeric_input_.precision=38;numeric_input_.sign=1;numeric_input_binding();ASSERT_FALSE(HasFailure());execute(hstmt_);ASSERT_FALSE(HasFailure());
  poison(numeric_);output_length_=97;ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));poison(numeric_);output_length_=97;
  // Explicit C_NUMERIC would use its default profile; SQL_ARD_TYPE selects this ARD's 38/0.
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_ARD_TYPE,&numeric_.value,sizeof(numeric_.value),&output_length_));
  EXPECT_EQ(38,numeric_.value.precision);EXPECT_EQ(0,numeric_.value.scale);EXPECT_EQ(1,numeric_.value.sign);
  for(const auto byte:numeric_.value.val) { EXPECT_EQ(0,byte); } guards(numeric_);
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));reset(hstmt_);clear_status();ASSERT_FALSE(HasFailure());
  EXPECT_EQ("amount",owned_name);EXPECT_EQ(1,owned[0].sign);EXPECT_EQ(0,owned[1].sign);
  EXPECT_EQ(0,std::memcmp(maximum_magnitude_.data(),owned[0].val,maximum_magnitude_.size()));EXPECT_EQ(0,std::memcmp(maximum_magnitude_.data(),owned[1].val,maximum_magnitude_.size()));
}

TEST_F(RedshiftCommonTypeBoundariesRealTest, TimestampDateTimeProjectionWarningsNullAndRecovery) {
  connect_bounded();ASSERT_FALSE(HasFailure());
  constexpr const char* sql="SELECT stamp,CAST(42 AS INTEGER) AS neighbor FROM (SELECT CAST('2000-02-29 12:34:56.123456' AS TIMESTAMP) AS stamp,1 AS row_order UNION ALL SELECT CAST('2000-02-29 00:00:00.000000' AS TIMESTAMP),2 UNION ALL SELECT CAST(NULL AS TIMESTAMP),3) AS projection_rows ORDER BY row_order";
  std::array<SQL_TIMESTAMP_STRUCT,2> owned{};
  for(std::size_t pass=0;pass<2;++pass) {
    if(pass==0) { direct(hstmt_,sql); } else { prepare(hstmt_,sql);ASSERT_FALSE(HasFailure());execute(hstmt_); }
    ASSERT_FALSE(HasFailure());SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(2,columns);metadata(hstmt_,1,"stamp",SQL_TYPE_TIMESTAMP,26,6);metadata(hstmt_,2,"neighbor",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,&rows_fetched_,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,&row_status_,0));
    if(pass==0) { ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_TYPE_DATE,&date_.value,sizeof(date_.value),&output_length_)); }
    else { ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_TYPE_TIME,&time_.value,sizeof(time_.value),&output_length_)); }
    for(std::size_t row=0;row<3;++row) {
      poison(date_);poison(time_);output_length_=97;rows_fetched_=99;row_status_=SQL_ROW_NOROW;
      const auto result=SQLFetch(hstmt_);ASSERT_EQ(row==0?SQL_SUCCESS_WITH_INFO:SQL_SUCCESS,result);
      if(row==0) { EXPECT_EQ("01S07",get_error(SQL_HANDLE_STMT,hstmt_)); }
      EXPECT_EQ(static_cast<SQLULEN>(1),rows_fetched_);EXPECT_EQ(row==0?SQL_ROW_SUCCESS_WITH_INFO:SQL_ROW_SUCCESS,row_status_);
      if(row==2) { EXPECT_EQ(SQL_NULL_DATA,output_length_);EXPECT_TRUE(pass==0?poisoned(date_.value):poisoned(time_.value)); }
      else if(pass==0) { EXPECT_EQ(2000,date_.value.year);EXPECT_EQ(2,date_.value.month);EXPECT_EQ(29,date_.value.day);EXPECT_EQ(static_cast<SQLLEN>(sizeof(date_.value)),output_length_); }
      else { EXPECT_EQ(row==0?12:0,time_.value.hour);EXPECT_EQ(row==0?34:0,time_.value.minute);EXPECT_EQ(row==0?56:0,time_.value.second);EXPECT_EQ(static_cast<SQLLEN>(sizeof(time_.value)),output_length_); }
      guards(date_);guards(time_);poison(stamp_);stamp_length_=97;
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_TYPE_TIMESTAMP,&stamp_.value,sizeof(stamp_.value),&stamp_length_));guards(stamp_);
      if(row==2) { EXPECT_EQ(SQL_NULL_DATA,stamp_length_);EXPECT_TRUE(poisoned(stamp_.value)); }
      else {
        EXPECT_EQ(2000,stamp_.value.year);EXPECT_EQ(2,stamp_.value.month);EXPECT_EQ(29,stamp_.value.day);
        EXPECT_EQ(row==0?12:0,stamp_.value.hour);EXPECT_EQ(row==0?34:0,stamp_.value.minute);EXPECT_EQ(row==0?56:0,stamp_.value.second);
        EXPECT_EQ(row==0?SQLUINTEGER{123456000}:SQLUINTEGER{0},stamp_.value.fraction);EXPECT_EQ(static_cast<SQLLEN>(sizeof(stamp_.value)),stamp_length_);owned[row]=stamp_.value;
      }
      integer(hstmt_,2,42);ASSERT_FALSE(HasFailure());
    }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));EXPECT_EQ(static_cast<SQLULEN>(0),rows_fetched_);EXPECT_EQ(SQL_ROW_NOROW,row_status_);
    reset(hstmt_);clear_status();ASSERT_FALSE(HasFailure());
  }
  direct(hstmt_,"SELECT 1 AS recovery");ASSERT_FALSE(HasFailure());scalar(hstmt_,1);reset(hstmt_);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(SQLUINTEGER{123456000},owned[0].fraction);EXPECT_EQ(SQLUINTEGER{0},owned[1].fraction);
}

TEST_F(RedshiftCommonTypeBoundariesRealTest, NumericRescaleOverflowTextRetryNullAndRecovery) {
  connect_bounded();ASSERT_FALSE(HasFailure());
  direct(hstmt_,"SELECT amount,CAST(42 AS INTEGER) AS neighbor FROM (SELECT CAST('123.456789' AS DECIMAL(10,6)) AS amount,1 AS row_order UNION ALL SELECT CAST('1234.567890' AS DECIMAL(10,6)),2 UNION ALL SELECT CAST(NULL AS DECIMAL(10,6)),3) AS numeric_rows ORDER BY row_order");ASSERT_FALSE(HasFailure());
  SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(2,columns);
  metadata(hstmt_,1,"amount",SQL_NUMERIC,10,6);metadata(hstmt_,2,"neighbor",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());
  numeric_ard(5,2,true);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,&rows_fetched_,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,&row_status_,0));
  poison(numeric_);output_length_=73;ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLFetch(hstmt_));EXPECT_EQ("01S07",get_error(SQL_HANDLE_STMT,hstmt_));
  EXPECT_EQ(static_cast<SQLULEN>(1),rows_fetched_);EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO,row_status_);EXPECT_EQ(5,numeric_.value.precision);EXPECT_EQ(2,numeric_.value.scale);EXPECT_EQ(1,numeric_.value.sign);
  const std::array<SQLCHAR,16> truncated{0x39,0x30,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  EXPECT_EQ(0,std::memcmp(truncated.data(),numeric_.value.val,truncated.size()));EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric_.value)),output_length_);guards(numeric_);const auto owned=numeric_.value;
  integer(hstmt_,2,42);ASSERT_FALSE(HasFailure());poison(numeric_);output_length_=73;rows_fetched_=99;row_status_=SQL_ROW_NOROW;
  ASSERT_EQ(SQL_ERROR,SQLFetch(hstmt_));EXPECT_EQ("22003",get_error(SQL_HANDLE_STMT,hstmt_));EXPECT_EQ(static_cast<SQLULEN>(1),rows_fetched_);EXPECT_EQ(SQL_ROW_ERROR,row_status_);
  EXPECT_EQ(SQLLEN{73},output_length_);EXPECT_TRUE(poisoned(numeric_.value));guards(numeric_);
  // Failed bound conversion leaves the row positioned; retrieve the cell and its unbound neighbor explicitly.
  text(hstmt_,1,"1234.567890",false);ASSERT_FALSE(HasFailure());integer(hstmt_,2,42);ASSERT_FALSE(HasFailure());
  poison(numeric_);output_length_=73;ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));EXPECT_EQ(SQL_ROW_SUCCESS,row_status_);EXPECT_EQ(SQL_NULL_DATA,output_length_);EXPECT_TRUE(poisoned(numeric_.value));guards(numeric_);
  poison(numeric_);ASSERT_EQ(SQL_ERROR,SQLGetData(hstmt_,1,SQL_ARD_TYPE,&numeric_.value,sizeof(numeric_.value),nullptr));EXPECT_EQ("22002",get_error(SQL_HANDLE_STMT,hstmt_));EXPECT_TRUE(poisoned(numeric_.value));
  output_length_=73;ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_ARD_TYPE,&numeric_.value,sizeof(numeric_.value),&output_length_));EXPECT_EQ(SQL_NULL_DATA,output_length_);EXPECT_TRUE(poisoned(numeric_.value));guards(numeric_);
  integer(hstmt_,2,42);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));EXPECT_EQ(static_cast<SQLULEN>(0),rows_fetched_);EXPECT_EQ(SQL_ROW_NOROW,row_status_);
  reset(hstmt_);clear_status();ASSERT_FALSE(HasFailure());direct(hstmt_,"SELECT 1 AS recovery");ASSERT_FALSE(HasFailure());scalar(hstmt_,1);reset(hstmt_);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(5,owned.precision);EXPECT_EQ(2,owned.scale);EXPECT_EQ(0,std::memcmp(truncated.data(),owned.val,truncated.size()));
}

// Fixed materialized exchanges only: application chunking is not network streaming.
class RedshiftMaterializedEnvelopeRealTest : public RedshiftRealTest {
protected:
  rs::util::Deadline window_end_{};
  bool admitted_ = false;
  std::string original_connection_, query_;
  struct IntegerCell { std::array<unsigned char,8> before; SQLINTEGER value; std::array<unsigned char,8> after; } ordinal_{};
  struct TotalCell { std::array<unsigned char,8> before; SQLBIGINT value; std::array<unsigned char,8> after; } total_{};
  struct TextCell { std::array<unsigned char,8> before; std::array<char,4097> value; std::array<unsigned char,8> after; } text_{};
  struct ChunkCell { std::array<unsigned char,8> before; std::array<char,1025> value; std::array<unsigned char,8> after; } chunk_{};
  struct NameCell { std::array<unsigned char,8> before; std::array<SQLCHAR,64> value; std::array<unsigned char,8> after; } name_{};
  std::array<char,17> parameter_{};
  SQLLEN parameter_length_ = 16, ordinal_length_ = 97, total_length_ = 97, text_length_ = 97;
  SQLULEN fetched_ = 99, processed_ = 99;
  SQLUSMALLINT row_status_ = SQL_ROW_NOROW, parameter_status_ = SQL_PARAM_UNUSED;
  std::size_t connections_ = 0, execution_attempts_ = 0, prepares_ = 0;
  std::size_t successful_exchanges_ = 0, limit_refusals_ = 0, local_refusals_ = 0;
  struct OwnedRow { SQLINTEGER ordinal; SQLBIGINT total; std::optional<std::string> payload; };

  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_MATERIALIZED_ENVELOPE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Materialized envelope scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="materialized-envelope-v1") << "Invalid materialized envelope scope marker";
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();
    if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(settings.contains("DATABASE"));ASSERT_TRUE(settings.contains("UID"));
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test");ASSERT_FALSE(settings.contains("DSN"));
    for(const char* key:{"MAXROWS","MAXCELLS","MAXRESPONSEBYTES","MAXRESPONSEMESSAGES"}) {
      ASSERT_FALSE(settings.contains(key)) << "Conflicting materialized envelope resource option";
    }
    original_connection_=connection_string_;
  }
  bool within() {
    if(rs::util::Clock::now()>=window_end_) { ADD_FAILURE() << "Materialized envelope original finite window expired";return false; }
    return true;
  }
  bool cap(rs::util::Deadline cutoff,bool statement,bool prepared_payload=false) {
    const auto left=std::chrono::duration_cast<std::chrono::seconds>(cutoff-rs::util::Clock::now()).count();
    if(left<=0) { ADD_FAILURE() << "Materialized envelope finite operation window expired";return false; }
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(left,prepared_payload?15:5));
    if(SQLSetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_DBC,hdbc_);return false;
    }
    if(statement&&SQLSetStmtAttr(hstmt_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_STMT,hstmt_);return false;
    }
    return true;
  }
  void connect_limits(std::size_t rows,std::size_t bytes) {
    ASSERT_TRUE(rows==999||rows==1000);ASSERT_TRUE(bytes==262144||bytes==16777216);
    connection_string_=original_connection_;
    if(connection_string_.back()!=';') { connection_string_+=';'; }
    // Append only fixed nonsecret policy keys; preserve ordinary verified-TLS credentials.
    connection_string_+="MAXROWS="+std::to_string(rows)+";MAXCELLS=3000;MAXRESPONSEBYTES="+
        std::to_string(bytes)+";MAXRESPONSEMESSAGES=4096;";
    ASSERT_TRUE(cap(window_end_,false));
    const auto left=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(left,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(left,5))),0));
    ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);++connections_;
    ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(within());
    SQLUINTEGER mode=99;ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_AUTOCOMMIT,&mode,sizeof(mode),nullptr));ASSERT_EQ(SQL_AUTOCOMMIT_ON,mode);
    SQLUINTEGER dead=SQL_CD_TRUE;ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_DEAD,&dead,0,nullptr));ASSERT_EQ(SQL_CD_FALSE,dead);
  }
  static std::string numbered_query(std::string_view payload) {
    return std::string("WITH digit AS (SELECT 0 AS d UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4 "
        "UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9), "
        "numbered AS (SELECT h.d*100+t.d*10+u.d+1 AS n FROM digit h CROSS JOIN digit t CROSS JOIN digit u) "
        "SELECT CAST(n AS INTEGER) AS row_no, ")+std::string(payload)+
        " AS payload, COUNT(*) OVER() AS total_rows FROM numbered ORDER BY n";
  }
  static std::string four_kib_query() {
    return numbered_query("CAST(CASE WHEN n%5=0 THEN NULL ELSE REPEAT('0123456789abcdef',256) END AS VARCHAR(4096))");
  }
  static std::string eight_kib_prepared_query() {
    return numbered_query("CAST(CASE WHEN n%5=0 THEN NULL ELSE REPEAT(CAST(? AS VARCHAR(16)),512) END AS VARCHAR(8192))");
  }
  static std::string small_query() {
    return numbered_query("CAST(CASE WHEN n%5=0 THEN NULL ELSE 'bounded-row' END AS VARCHAR(32))");
  }
  static std::string literal_payload(std::size_t bytes) {
    // Independent expected bytes; no observed length or driver conversion feeds this oracle.
    constexpr std::string_view pattern="0123456789abcdef";
    std::string value;value.reserve(bytes);
    for(std::size_t offset=0;offset<bytes;++offset) { value.push_back(pattern[offset%16]); }
    return value;
  }
  template<class Cell> void poison(Cell& cell) {
    cell.before.fill(0x5a);cell.after.fill(0x5a);
  }
  template<class Cell> void guards(const Cell& cell) {
    EXPECT_TRUE(std::all_of(cell.before.begin(),cell.before.end(),[](unsigned char value){return value==0x5a;}));
    EXPECT_TRUE(std::all_of(cell.after.begin(),cell.after.end(),[](unsigned char value){return value==0x5a;}));
  }
  void direct(const char* sql) {
    ASSERT_TRUE(cap(window_end_,true));++execution_attempts_;
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    ++successful_exchanges_;ASSERT_TRUE(within());
  }
  void execute() {
    // Both 8KiB materializations share the unchanged original 40s case window.
    ASSERT_TRUE(cap(window_end_,true,true));++execution_attempts_;
    ASSERT_EQ(SQL_SUCCESS,SQLExecute(hstmt_)) << get_error(SQL_HANDLE_STMT,hstmt_);
    ++successful_exchanges_;ASSERT_TRUE(within());
    EXPECT_EQ(static_cast<SQLULEN>(1),processed_);EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status_);
  }
  void row_status() {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,&fetched_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,&row_status_,0));
  }
  void metadata(SQLULEN payload_width,std::array<std::string,3>& owned) {
    SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(3,columns);
    const char* names[]{"row_no","payload","total_rows"};
    const SQLSMALLINT types[]{SQL_INTEGER,SQL_VARCHAR,SQL_BIGINT};
    const SQLULEN widths[]{static_cast<SQLULEN>(10),payload_width,static_cast<SQLULEN>(19)};
    for(SQLUSMALLINT column=1;column<=3;++column) {
      poison(name_);name_.value.fill(0x5a);
      SQLSMALLINT length=-1,type=-1,scale=-1;SQLULEN width=99;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name_.value.data(),static_cast<SQLSMALLINT>(name_.value.size()),&length,&type,&width,&scale,nullptr));
      ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length)+1,name_.value.size());
      owned[column-1].assign(reinterpret_cast<const char*>(name_.value.data()),static_cast<std::size_t>(length));
      EXPECT_TRUE(owned[column-1]==names[column-1]);EXPECT_EQ(types[column-1],type);EXPECT_EQ(widths[column-1],width);EXPECT_EQ(0,scale);
      EXPECT_EQ(0,name_.value[static_cast<std::size_t>(length)]);EXPECT_EQ(0x5a,name_.value[static_cast<std::size_t>(length)+1]);guards(name_);
      ASSERT_FALSE(HasFailure());
    }
  }
  void fetch_row(SQLINTEGER expected,OwnedRow& owned,const std::string& expected_payload,bool chunks) {
    ASSERT_TRUE(within());fetched_=99;row_status_=SQL_ROW_NOROW;
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));EXPECT_EQ(static_cast<SQLULEN>(1),fetched_);EXPECT_EQ(SQL_ROW_SUCCESS,row_status_);
    poison(ordinal_);ordinal_.value=-97;ordinal_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&ordinal_.value,sizeof(ordinal_.value),&ordinal_length_));
    EXPECT_EQ(expected,ordinal_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(ordinal_.value)),ordinal_length_);guards(ordinal_);ASSERT_FALSE(HasFailure());
    owned.ordinal=ordinal_.value;
    if(expected%5==0) {
      poison(chunk_);chunk_.value.fill('\x5a');text_length_=97;
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,2,SQL_C_CHAR,chunk_.value.data(),static_cast<SQLLEN>(chunk_.value.size()),&text_length_));
      EXPECT_EQ(SQL_NULL_DATA,text_length_);EXPECT_TRUE(std::all_of(chunk_.value.begin(),chunk_.value.end(),[](char value){return value=='\x5a';}));guards(chunk_);
      owned.payload.reset();
    } else if(chunks) {
      const std::array<SQLLEN,8> remaining{8192,7168,6144,5120,4096,3072,2048,1024};
      std::string snapshot;snapshot.reserve(8192);
      ASSERT_EQ(8192u,expected_payload.size());
      for(std::size_t part=0;part<remaining.size();++part) {
        ASSERT_TRUE(within());poison(chunk_);chunk_.value.fill('\x5a');text_length_=97;
        const auto result=SQLGetData(hstmt_,2,SQL_C_CHAR,chunk_.value.data(),static_cast<SQLLEN>(chunk_.value.size()),&text_length_);
        ASSERT_EQ(part+1==remaining.size()?SQL_SUCCESS:SQL_SUCCESS_WITH_INFO,result) << get_error(SQL_HANDLE_STMT,hstmt_);
        if(part+1!=remaining.size()) { EXPECT_EQ("01004",get_error(SQL_HANDLE_STMT,hstmt_)); }
        EXPECT_EQ(remaining[part],text_length_);EXPECT_EQ(0,chunk_.value[1024]);guards(chunk_);
        EXPECT_TRUE(std::equal(chunk_.value.begin(),chunk_.value.begin()+1024,expected_payload.begin()+static_cast<std::ptrdiff_t>(part*1024)));
        ASSERT_FALSE(HasFailure());snapshot.append(chunk_.value.data(),1024);
      }
      ASSERT_EQ(SQL_NO_DATA,SQLGetData(hstmt_,2,SQL_C_CHAR,chunk_.value.data(),static_cast<SQLLEN>(chunk_.value.size()),&text_length_));
      owned.payload=std::move(snapshot);
    } else {
      ASSERT_EQ(4096u,expected_payload.size());poison(text_);text_.value.fill('\x5a');text_length_=97;
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,2,SQL_C_CHAR,text_.value.data(),static_cast<SQLLEN>(text_.value.size()),&text_length_));
      EXPECT_EQ(4096,text_length_);EXPECT_EQ(0,text_.value[4096]);guards(text_);
      EXPECT_TRUE(std::equal(text_.value.begin(),text_.value.begin()+4096,expected_payload.begin()));ASSERT_FALSE(HasFailure());
      owned.payload=std::string(text_.value.data(),4096);
    }
    ASSERT_FALSE(HasFailure());poison(total_);total_.value=-97;total_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,3,SQL_C_SBIGINT,&total_.value,sizeof(total_.value),&total_length_));
    EXPECT_EQ(SQLBIGINT{1000},total_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(total_.value)),total_length_);guards(total_);ASSERT_FALSE(HasFailure());
    owned.total=total_.value;
  }
  void fetch_all(std::vector<OwnedRow>& owned,const std::string& expected_payload,bool chunks) {
    row_status();ASSERT_FALSE(HasFailure());owned.reserve(1000);
    for(SQLINTEGER row=1;row<=1000;++row) {
      OwnedRow snapshot{};fetch_row(row,snapshot,expected_payload,chunks);ASSERT_FALSE(HasFailure());owned.push_back(std::move(snapshot));
    }
    ASSERT_TRUE(within());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));EXPECT_EQ(static_cast<SQLULEN>(0),fetched_);EXPECT_EQ(SQL_ROW_NOROW,row_status_);
    std::size_t nulls=0,bytes=0;
    for(const auto& row:owned) { if(row.payload) { bytes+=row.payload->size(); } else { ++nulls; } }
    EXPECT_EQ(1000u,owned.size());EXPECT_EQ(200u,nulls);EXPECT_EQ(800u*expected_payload.size(),bytes);
  }
  void reset(bool parameters) {
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
    if(parameters) { ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS)); }
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,nullptr,0));
    if(parameters) {
      ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,nullptr,0));
      ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_STATUS_PTR,nullptr,0));
    }
  }
  void scalar_recovery() {
    direct("SELECT CAST(1 AS INTEGER)");ASSERT_FALSE(HasFailure());
    SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(1,columns);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));poison(ordinal_);ordinal_.value=-97;ordinal_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&ordinal_.value,sizeof(ordinal_.value),&ordinal_length_));
    EXPECT_EQ(1,ordinal_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(ordinal_.value)),ordinal_length_);guards(ordinal_);
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));reset(true);ASSERT_FALSE(HasFailure());
  }
  void recheck(const std::vector<OwnedRow>& owned,const std::string& expected_payload,const std::array<std::string,3>& names) {
    ASSERT_EQ(1000u,owned.size());
    EXPECT_TRUE(names[0]=="row_no");EXPECT_TRUE(names[1]=="payload");EXPECT_TRUE(names[2]=="total_rows");
    for(std::size_t index=0;index<owned.size();++index) {
      ASSERT_TRUE(within());EXPECT_EQ(static_cast<SQLINTEGER>(index+1),owned[index].ordinal);EXPECT_EQ(SQLBIGINT{1000},owned[index].total);
      if((index+1)%5==0) { EXPECT_FALSE(owned[index].payload); }
      else { ASSERT_TRUE(owned[index].payload);EXPECT_TRUE(*owned[index].payload==expected_payload); }
      ASSERT_FALSE(HasFailure());
    }
  }
  void refuse_and_reconnect(const std::string& sql) {
    row_status();ASSERT_FALSE(HasFailure());
    poison(ordinal_);ordinal_.value=-97;ordinal_length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_SLONG,&ordinal_.value,sizeof(ordinal_.value),&ordinal_length_));
    ASSERT_TRUE(cap(window_end_,true));++execution_attempts_;
    ASSERT_EQ(SQL_ERROR,SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),SQL_NTS));
    ASSERT_EQ("HY000",get_error(SQL_HANDLE_STMT,hstmt_));++limit_refusals_;ASSERT_TRUE(within());
    SQLUINTEGER dead=SQL_CD_FALSE;ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_DEAD,&dead,0,nullptr));ASSERT_EQ(SQL_CD_TRUE,dead);
    // Failed unprepared execute never publishes a result. These API states are
    // source-supported; the separately selected offline bridge must qualify them.
    SQLSMALLINT columns=73;ASSERT_EQ(SQL_ERROR,SQLNumResultCols(hstmt_,&columns));EXPECT_EQ("HY010",get_error(SQL_HANDLE_STMT,hstmt_));EXPECT_EQ(73,columns);
    ASSERT_EQ(SQL_ERROR,SQLFetch(hstmt_));EXPECT_EQ("HY010",get_error(SQL_HANDLE_STMT,hstmt_));
    EXPECT_EQ(-97,ordinal_.value);EXPECT_EQ(97,ordinal_length_);EXPECT_EQ(static_cast<SQLULEN>(99),fetched_);EXPECT_EQ(SQL_ROW_NOROW,row_status_);guards(ordinal_);
    ASSERT_FALSE(HasFailure());ASSERT_TRUE(within());++execution_attempts_;
    SQLCHAR local[]="SELECT CAST(1 AS INTEGER)";
    ASSERT_EQ(SQL_ERROR,SQLExecDirect(hstmt_,local,SQL_NTS));EXPECT_EQ("08S01",get_error(SQL_HANDLE_STMT,hstmt_));++local_refusals_;
    // No automatic reconnect: zero wire dispatch is an independent offline oracle.
    dead=SQL_CD_FALSE;ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_DEAD,&dead,0,nullptr));ASSERT_EQ(SQL_CD_TRUE,dead);
    ASSERT_FALSE(HasFailure());reset(true);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;
    ASSERT_TRUE(cap(window_end_,false));ASSERT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));connected_=false;
    ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_DBC,henv_,&hdbc_));
    connect_limits(1000,16777216);ASSERT_FALSE(HasFailure());scalar_recovery();ASSERT_FALSE(HasFailure());
  }
  void TearDown() override {
    const auto entry=rs::util::Clock::now();
    if(admitted_) { EXPECT_TRUE(entry<window_end_) << "Materialized envelope case completed after original finite window"; }
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    // Every bound value/indicator/status remains a member until handles are freed.
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_&&cap(cleanup_end,false)) {
        const auto result=SQLDisconnect(hdbc_);
        if(result==SQL_ERROR&&get_error(SQL_HANDLE_DBC,hdbc_)=="25000") {
          ADD_FAILURE() << "Unexpected transaction at materialized envelope cleanup";
          if(cap(cleanup_end,false)) {
            const auto rollback=SQLEndTran(SQL_HANDLE_DBC,hdbc_,SQL_ROLLBACK);EXPECT_EQ(SQL_SUCCESS,rollback) << get_error(SQL_HANDLE_DBC,hdbc_);
            if(rollback==SQL_SUCCESS&&cap(cleanup_end,false)) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
          }
        } else { EXPECT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_DBC,hdbc_); }
      }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
  }
};

TEST_F(RedshiftMaterializedEnvelopeRealTest, DirectThousandFourKiBExactCountsAndOwningRecovery) {
  connect_limits(1000,16777216);ASSERT_FALSE(HasFailure());
  query_=four_kib_query();direct(query_.c_str());ASSERT_FALSE(HasFailure());std::fill(query_.begin(),query_.end(),'!');
  std::array<std::string,3> names;metadata(static_cast<SQLULEN>(4096),names);ASSERT_FALSE(HasFailure());
  const auto expected=literal_payload(4096);std::vector<OwnedRow> owned;
  fetch_all(owned,expected,false);ASSERT_FALSE(HasFailure());reset(true);ASSERT_FALSE(HasFailure());
  text_.value.fill('!');name_.value.fill(0x5a);scalar_recovery();ASSERT_FALSE(HasFailure());
  recheck(owned,expected,names);ASSERT_FALSE(HasFailure());
  EXPECT_EQ(1u,connections_);EXPECT_EQ(2u,execution_attempts_);EXPECT_EQ(2u,successful_exchanges_);EXPECT_EQ(0u,prepares_);
}

TEST_F(RedshiftMaterializedEnvelopeRealTest, PreparedEightKiBChunksEarlyCloseAndOwningReuse) {
  connect_limits(1000,16777216);ASSERT_FALSE(HasFailure());query_=eight_kib_prepared_query();
  ASSERT_TRUE(cap(window_end_,true));++prepares_;
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(hstmt_,reinterpret_cast<SQLCHAR*>(query_.data()),SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
  std::fill(query_.begin(),query_.end(),'!');std::memcpy(parameter_.data(),"0123456789abcdef",17);
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_STATUS_PTR,&parameter_status_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,static_cast<SQLULEN>(16),0,
      parameter_.data(),static_cast<SQLLEN>(parameter_.size()),&parameter_length_));
  execute();ASSERT_FALSE(HasFailure());
  std::array<std::string,3> names;metadata(static_cast<SQLULEN>(8192),names);ASSERT_FALSE(HasFailure());
  const auto expected=literal_payload(8192);std::vector<OwnedRow> owned;
  fetch_all(owned,expected,true);ASSERT_FALSE(HasFailure());reset(false);ASSERT_FALSE(HasFailure());
  processed_=99;parameter_status_=SQL_PARAM_UNUSED;execute();ASSERT_FALSE(HasFailure());parameter_.fill('!');
  row_status();ASSERT_FALSE(HasFailure());std::array<OwnedRow,17> early;
  for(std::size_t index=0;index<early.size();++index) {
    fetch_row(static_cast<SQLINTEGER>(index+1),early[index],expected,true);ASSERT_FALSE(HasFailure());
  }
  // CLOSE discards the remaining buffered rows; no server-cursor/streaming claim.
  reset(true);ASSERT_FALSE(HasFailure());chunk_.value.fill('!');name_.value.fill(0x5a);
  scalar_recovery();ASSERT_FALSE(HasFailure());recheck(owned,expected,names);ASSERT_FALSE(HasFailure());
  for(std::size_t index=0;index<early.size();++index) {
    EXPECT_EQ(static_cast<SQLINTEGER>(index+1),early[index].ordinal);EXPECT_EQ(SQLBIGINT{1000},early[index].total);
    if((index+1)%5==0) { EXPECT_FALSE(early[index].payload); }
    else { ASSERT_TRUE(early[index].payload);EXPECT_TRUE(*early[index].payload==expected); }
  }
  EXPECT_EQ(1u,connections_);EXPECT_EQ(3u,execution_attempts_);EXPECT_EQ(3u,successful_exchanges_);EXPECT_EQ(1u,prepares_);
}

TEST_F(RedshiftMaterializedEnvelopeRealTest, DecodedRowLimitRefusesWithoutPartialRowsThenFreshConnectRecovers) {
  connect_limits(999,16777216);ASSERT_FALSE(HasFailure());
  query_=small_query();refuse_and_reconnect(query_);ASSERT_FALSE(HasFailure());std::fill(query_.begin(),query_.end(),'!');
  EXPECT_EQ(2u,connections_);EXPECT_EQ(3u,execution_attempts_);EXPECT_EQ(1u,successful_exchanges_);
  EXPECT_EQ(1u,limit_refusals_);EXPECT_EQ(1u,local_refusals_);EXPECT_EQ(0u,prepares_);
}

TEST_F(RedshiftMaterializedEnvelopeRealTest, WireByteCapRefusesLargerMaterializationThenFreshConnectRecovers) {
  connect_limits(1000,262144);ASSERT_FALSE(HasFailure());
  query_=four_kib_query();refuse_and_reconnect(query_);ASSERT_FALSE(HasFailure());std::fill(query_.begin(),query_.end(),'!');
  EXPECT_EQ(2u,connections_);EXPECT_EQ(3u,execution_attempts_);EXPECT_EQ(1u,successful_exchanges_);
  EXPECT_EQ(1u,limit_refusals_);EXPECT_EQ(1u,local_refusals_);EXPECT_EQ(0u,prepares_);
  // A byte cap retires an unread exchange. Remote terminal-work absence is an
  // independent finite-controller cleanup obligation, not SQLCancel evidence.
}

// Modern per-schema SHOW route only. Existing objects are reused, never created.
class RedshiftShowTablesRealTest : public RedshiftRealTest {
protected:
  rs::util::Deadline window_end_{};
  bool admitted_ = false;
  std::string schema_,table_,catalog_input_,schema_input_,table_input_,types_input_;
  std::vector<SQLWCHAR> wide_catalog_,wide_schema_,wide_table_,wide_types_;
  struct NarrowCell {std::array<unsigned char,8> before;std::array<char,65537> value;std::array<unsigned char,8> after;} narrow_{};
  struct WideCell {std::array<unsigned char,8> before;std::array<SQLWCHAR,65537> value;std::array<unsigned char,8> after;} wide_{};
  struct NameCell {std::array<unsigned char,8> before;std::array<SQLCHAR,64> value;std::array<unsigned char,8> after;} name_{};
  struct IntegerCell {SQLINTEGER before{17},value{-99},after{83};} integer_{};
  SQLLEN length_ = 97;
  std::size_t catalog_calls_ = 0, scalar_calls_ = 0;
  struct Descriptor {std::string name;SQLSMALLINT type,scale,nullable;SQLULEN capacity;bool operator==(const Descriptor&)const=default;};
  struct OwnedRow {std::array<std::optional<std::string>,5> fields;std::array<std::vector<SQLWCHAR>,5> wide_fields;bool wide_remarks_null=true;};

  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_SHOW_TABLES_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "SHOW tables scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="show-tables-v1") << "Invalid SHOW tables scope marker";
    ASSERT_EQ(2u,sizeof(SQLWCHAR)) << "SHOW tables native scope requires SQLWCHAR width2";
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    for(const char* key:{"SERVER","PORT","DATABASE","UID","PWD","SSLCAFILE"}) {
      ASSERT_TRUE(settings.contains(key));ASSERT_FALSE(settings.at(key).empty());
    }
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test");
    ASSERT_TRUE(settings.at("PORT")=="5439");ASSERT_FALSE(settings.contains("DSN"));ASSERT_FALSE(settings.contains("REDSHIFTCATALOGMODE"));
    const char* schema=std::getenv("ODBCPP_REDSHIFT_TEST_SCHEMA");const char* table=std::getenv("ODBCPP_REDSHIFT_TEST_TABLE");
    ASSERT_NE(nullptr,schema);ASSERT_NE(nullptr,table);
    for(const auto& pair:{std::pair{schema,&schema_},std::pair{table,&table_}}) {
      std::size_t count=0;while(count<=128&&pair.first[count]!='\0') { ++count; }
      ASSERT_GT(count,0u);ASSERT_LE(count,128u);
      for(std::size_t index=0;index<count;++index) {
        const auto ch=static_cast<unsigned char>(pair.first[index]);
        ASSERT_TRUE((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_');
      }
      pair.second->assign(pair.first,count);
    }
    ASSERT_TRUE(schema_=="odbcpp_fixture");ASSERT_TRUE(table_=="pilot_rows");
    if(connection_string_.back()!=';') { connection_string_+=';'; }
    connection_string_+="REDSHIFTCATALOGMODE=SHOW;";
  }
  static std::string literal_pattern(std::string_view identifier) {
    std::string pattern;
    for(const char ch:identifier) { if(ch=='%'||ch=='_'||ch=='\\') { pattern+='\\'; }pattern+=ch; }
    return pattern;
  }
  // The selected existing fixture names are ASCII. These literal code units
  // cover A/W conversion without pretending this is Unicode-object admission.
  static std::vector<SQLWCHAR> ascii_units(std::string_view input) {
    std::vector<SQLWCHAR> output;output.reserve(input.size()+1);
    for(const unsigned char ch:input) { output.push_back(static_cast<SQLWCHAR>(ch)); }
    output.push_back(0);return output;
  }
  bool cap(rs::util::Deadline cutoff,bool statement) {
    const auto left=std::chrono::duration_cast<std::chrono::seconds>(cutoff-rs::util::Clock::now()).count();
    if(left<=0) { ADD_FAILURE() << "SHOW tables original finite window expired";return false; }
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(left,5));
    if(SQLSetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_DBC,hdbc_);return false;
    }
    if(statement&&SQLSetStmtAttr(hstmt_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      ADD_FAILURE() << get_error(SQL_HANDLE_STMT,hstmt_);return false;
    }
    return true;
  }
  void connect_bounded() {
    ASSERT_TRUE(cap(window_end_,false));
    const auto left=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(left,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(left,5))),0));
    ASSERT_TRUE(connect()) << get_error(SQL_HANDLE_DBC,hdbc_);ASSERT_TRUE(cap(window_end_,true));
    SQLUINTEGER mode=99;ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_AUTOCOMMIT,&mode,sizeof(mode),nullptr));ASSERT_EQ(SQL_AUTOCOMMIT_ON,mode);
  }
  void tables(std::string pattern,std::string types,bool use_wide) {
    catalog_input_=literal_pattern("odbcpp_pilot");schema_input_=literal_pattern(schema_);table_input_=std::move(pattern);types_input_=std::move(types);
    ASSERT_TRUE(cap(window_end_,true));++catalog_calls_;
    SQLRETURN result=SQL_ERROR;
    if(use_wide) {
      wide_catalog_=ascii_units(catalog_input_);wide_schema_=ascii_units(schema_input_);
      wide_table_=ascii_units(table_input_);wide_types_=ascii_units(types_input_);
      result=SQLTablesW(hstmt_,wide_catalog_.data(),SQL_NTS,wide_schema_.data(),SQL_NTS,
          wide_table_.data(),SQL_NTS,wide_types_.data(),SQL_NTS);
      for(auto* input:{&wide_catalog_,&wide_schema_,&wide_table_,&wide_types_}) { std::fill(input->begin(),input->end(),SQLWCHAR{0x5a}); }
    } else {
      result=SQLTables(hstmt_,reinterpret_cast<SQLCHAR*>(catalog_input_.data()),SQL_NTS,
          reinterpret_cast<SQLCHAR*>(schema_input_.data()),SQL_NTS,reinterpret_cast<SQLCHAR*>(table_input_.data()),SQL_NTS,
          reinterpret_cast<SQLCHAR*>(types_input_.data()),SQL_NTS);
    }
    // Synchronous catalog arguments must not be retained by the owning result.
    for(auto* input:{&catalog_input_,&schema_input_,&table_input_,&types_input_}) { std::fill(input->begin(),input->end(),'!'); }
    ASSERT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_STMT,hstmt_);
    ASSERT_LT(rs::util::Clock::now(),window_end_);
  }
  template<class Cell> void poison(Cell& cell) {cell.before.fill(0x5a);cell.after.fill(0x5a);}
  template<class Cell> void guards(const Cell& cell) {
    EXPECT_TRUE(std::all_of(cell.before.begin(),cell.before.end(),[](unsigned char ch){return ch==0x5a;}));
    EXPECT_TRUE(std::all_of(cell.after.begin(),cell.after.end(),[](unsigned char ch){return ch==0x5a;}));
  }
  void metadata(std::array<Descriptor,5>& owned) {
    SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(5,columns);
    constexpr std::array<std::string_view,5> names{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","TABLE_TYPE","REMARKS"};
    for(SQLUSMALLINT column=1;column<=5;++column) {
      poison(name_);name_.value.fill(0x5a);SQLSMALLINT length=-1,type=-1,scale=-1,nullable=-1;SQLULEN capacity=99;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name_.value.data(),static_cast<SQLSMALLINT>(name_.value.size()),
          &length,&type,&capacity,&scale,&nullable));
      ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length)+1,name_.value.size());
      const std::string actual(reinterpret_cast<const char*>(name_.value.data()),static_cast<std::size_t>(length));
      // Exact uppercase projection is part of the SHOW adapter; legacy names
      // or success alone are insufficient. Source-bound no-fallback dispatch
      // and authenticated show_discovery capability still require root binding.
      EXPECT_TRUE(actual==names[column-1]);EXPECT_EQ(SQL_VARCHAR,type);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
      EXPECT_LE(capacity,static_cast<SQLULEN>(65535));EXPECT_EQ(0,name_.value[static_cast<std::size_t>(length)]);
      EXPECT_EQ(0x5a,name_.value[static_cast<std::size_t>(length)+1]);guards(name_);ASSERT_FALSE(HasFailure());
      owned[column-1]={actual,type,scale,nullable,capacity};
    }
  }
  void row(OwnedRow& owned,const std::array<Descriptor,5>& descriptors) {
    for(SQLUSMALLINT column=1;column<=5;++column) {
      poison(narrow_);narrow_.value.fill('\x5a');length_=97;
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,narrow_.value.data(),static_cast<SQLLEN>(narrow_.value.size()),&length_));
      if(length_==SQL_NULL_DATA) {
        ASSERT_EQ(5,column);EXPECT_TRUE(std::all_of(narrow_.value.begin(),narrow_.value.end(),[](char ch){return ch=='\x5a';}));
        owned.fields[column-1].reset();
      } else {
        ASSERT_GE(length_,0);ASSERT_LE(length_,65535);
        const auto size=static_cast<std::size_t>(length_);EXPECT_EQ(0,narrow_.value[size]);EXPECT_EQ('\x5a',narrow_.value[size+1]);
        owned.fields[column-1]=std::string(narrow_.value.data(),size);
        // For the controlled ASCII identity fields, byte and character counts agree.
        if(column<=4&&descriptors[column-1].capacity!=0&&
            std::all_of(narrow_.value.begin(),narrow_.value.begin()+static_cast<std::ptrdiff_t>(size),[](unsigned char ch){return ch<128;})) {
          EXPECT_LE(static_cast<SQLULEN>(size),descriptors[column-1].capacity);
        }
      }
      guards(narrow_);ASSERT_FALSE(HasFailure());
    }
    for(std::size_t index=0;index<4;++index) { ASSERT_TRUE(owned.fields[index]); }
    EXPECT_TRUE(*owned.fields[0]=="odbcpp_pilot");EXPECT_TRUE(*owned.fields[1]==schema_);ASSERT_FALSE(HasFailure());
  }
  static std::vector<SQLWCHAR> literal_identity(std::size_t column) {
    switch(column) {
      case 1:return {'o','d','b','c','p','p','_','p','i','l','o','t'};
      case 2:return {'o','d','b','c','p','p','_','f','i','x','t','u','r','e'};
      case 3:return {'p','i','l','o','t','_','r','o','w','s'};
      case 4:return {'T','A','B','L','E'};
      default:return {};
    }
  }
  void wide_row(OwnedRow& owned,const std::array<Descriptor,5>& descriptors) {
    for(SQLUSMALLINT column=1;column<=5;++column) {
      poison(wide_);wide_.value.fill(SQLWCHAR{0x5a});length_=97;
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_WCHAR,wide_.value.data(),static_cast<SQLLEN>(sizeof(wide_.value)),&length_));
      if(length_==SQL_NULL_DATA) {
        ASSERT_EQ(5,column);EXPECT_TRUE(std::all_of(wide_.value.begin(),wide_.value.end(),[](SQLWCHAR ch){return ch==SQLWCHAR{0x5a};}));
        owned.wide_remarks_null=true;
      } else {
        ASSERT_GE(length_,0);ASSERT_EQ(0,length_%static_cast<SQLLEN>(sizeof(SQLWCHAR)));
        const auto units=static_cast<std::size_t>(length_)/sizeof(SQLWCHAR);ASSERT_LT(units+1,wide_.value.size());
        EXPECT_EQ(SQLWCHAR{0},wide_.value[units]);EXPECT_EQ(SQLWCHAR{0x5a},wide_.value[units+1]);
        owned.wide_fields[column-1].assign(wide_.value.begin(),wide_.value.begin()+static_cast<std::ptrdiff_t>(units));
        if(column<=4) {
          const auto literal=literal_identity(column);ASSERT_EQ(literal.size(),units);
          EXPECT_TRUE(std::equal(literal.begin(),literal.end(),wide_.value.begin()));
          if(descriptors[column-1].capacity!=0) { EXPECT_LE(static_cast<SQLULEN>(units),descriptors[column-1].capacity); }
          std::string actual;actual.reserve(units);
          for(std::size_t index=0;index<units;++index) {ASSERT_LE(wide_.value[index],static_cast<SQLWCHAR>(0x7f));actual+=static_cast<char>(wide_.value[index]);}
          owned.fields[column-1]=std::move(actual);
        } else {owned.wide_remarks_null=false;} // Actual nullable remarks retained as owning WCHAR units.
      }
      guards(wide_);ASSERT_FALSE(HasFailure());
    }
  }
  void close_cursor() {ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE));}
  void empty_result() {
    std::array<Descriptor,5> descriptors;metadata(descriptors);ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();ASSERT_FALSE(HasFailure());
  }
  void recovery() {
    close_cursor();ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
    ASSERT_TRUE(cap(window_end_,true));SQLCHAR sql[]="SELECT CAST(1 AS INTEGER)";++scalar_calls_;
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt_,sql,SQL_NTS)) << get_error(SQL_HANDLE_STMT,hstmt_);
    SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns));ASSERT_EQ(1,columns);ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    integer_={17,-99,83};length_=97;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&integer_.value,sizeof(integer_.value),&length_));
    EXPECT_EQ(1,integer_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer_.value)),length_);EXPECT_EQ(17,integer_.before);EXPECT_EQ(83,integer_.after);
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();ASSERT_FALSE(HasFailure());
  }
  void recheck(const OwnedRow& owned,const std::array<Descriptor,5>& descriptors,bool use_wide) {
    ASSERT_TRUE(owned.fields[0]);ASSERT_TRUE(owned.fields[1]);ASSERT_TRUE(owned.fields[2]);ASSERT_TRUE(owned.fields[3]);
    EXPECT_TRUE(*owned.fields[0]=="odbcpp_pilot");EXPECT_TRUE(*owned.fields[1]==schema_);EXPECT_TRUE(*owned.fields[2]==table_);EXPECT_TRUE(*owned.fields[3]=="TABLE");
    constexpr std::array<std::string_view,5> names{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","TABLE_TYPE","REMARKS"};
    for(std::size_t index=0;index<descriptors.size();++index) { EXPECT_TRUE(descriptors[index].name==names[index]);EXPECT_EQ(SQL_VARCHAR,descriptors[index].type); }
    if(use_wide) {
      for(std::size_t index=0;index<4;++index) {
        const auto literal=literal_identity(index+1);ASSERT_EQ(literal.size(),owned.wide_fields[index].size());
        EXPECT_TRUE(std::equal(literal.begin(),literal.end(),owned.wide_fields[index].begin()));
      }
    }
  }
  void TearDown() override {
    const auto entry=rs::util::Clock::now();
    if(admitted_) { EXPECT_TRUE(entry<window_end_) << "SHOW tables case completed after original finite window"; }
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    if(hstmt_) {EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;}
    if(hdbc_) {
      if(connected_&&cap(cleanup_end,false)) {
        const auto result=SQLDisconnect(hdbc_);
        if(result==SQL_ERROR&&get_error(SQL_HANDLE_DBC,hdbc_)=="25000") {
          ADD_FAILURE() << "Unexpected transaction at SHOW tables cleanup";
          if(cap(cleanup_end,false)) {
            const auto rollback=SQLEndTran(SQL_HANDLE_DBC,hdbc_,SQL_ROLLBACK);EXPECT_EQ(SQL_SUCCESS,rollback) << get_error(SQL_HANDLE_DBC,hdbc_);
            if(rollback==SQL_SUCCESS&&cap(cleanup_end,false)) {EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));}
          }
        } else {EXPECT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_DBC,hdbc_);}
      }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) {EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr;}
  }
};

TEST_F(RedshiftShowTablesRealTest, ExactTableMetadataOwnsValuesAndRecovers) {
  connect_bounded();ASSERT_FALSE(HasFailure());tables(literal_pattern(table_),"TABLE",false);ASSERT_FALSE(HasFailure());
  std::array<Descriptor,5> descriptors;metadata(descriptors);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  OwnedRow owned;row(owned,descriptors);ASSERT_FALSE(HasFailure());ASSERT_TRUE(owned.fields[2]);ASSERT_TRUE(owned.fields[3]);
  EXPECT_TRUE(*owned.fields[2]==table_);EXPECT_TRUE(*owned.fields[3]=="TABLE");ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();ASSERT_FALSE(HasFailure());
  const auto remarks=owned.fields[4];narrow_.value.fill('!');name_.value.fill(0x5a);recovery();ASSERT_FALSE(HasFailure());
  recheck(owned,descriptors,false);ASSERT_FALSE(HasFailure());EXPECT_TRUE(owned.fields[4]==remarks);EXPECT_EQ(1u,catalog_calls_);EXPECT_EQ(1u,scalar_calls_);
}

TEST_F(RedshiftShowTablesRealTest, TablePatternAndTypeFilterStayBounded) {
  connect_bounded();ASSERT_FALSE(HasFailure());tables("%"+literal_pattern(table_),"'TABLE'",false);ASSERT_FALSE(HasFailure());
  std::array<Descriptor,5> descriptors;metadata(descriptors);ASSERT_FALSE(HasFailure());
  std::vector<OwnedRow> owned;std::size_t selected=0;bool exhausted=false;
  for(std::size_t index=0;index<=64;++index) {
    ASSERT_LT(rs::util::Clock::now(),window_end_);const auto result=SQLFetch(hstmt_);
    if(result==SQL_NO_DATA) {exhausted=true;break;}
    ASSERT_EQ(SQL_SUCCESS,result);ASSERT_LT(index,64u);OwnedRow value;row(value,descriptors);ASSERT_FALSE(HasFailure());
    ASSERT_TRUE(value.fields[2]);ASSERT_TRUE(value.fields[3]);EXPECT_TRUE(value.fields[2]->ends_with(table_));EXPECT_TRUE(*value.fields[3]=="TABLE");
    if(*value.fields[2]==table_) {++selected;}
    if(!owned.empty()) {ASSERT_TRUE(owned.back().fields[2]);EXPECT_TRUE(*owned.back().fields[2]<*value.fields[2]);}
    owned.push_back(std::move(value));
  }
  ASSERT_TRUE(exhausted);ASSERT_EQ(1u,selected);close_cursor();ASSERT_FALSE(HasFailure());
  // The same exact ordinary TABLE cannot be returned by a VIEW-only filter.
  tables(literal_pattern(table_),"'VIEW'",false);ASSERT_FALSE(HasFailure());empty_result();ASSERT_FALSE(HasFailure());
  narrow_.value.fill('!');name_.value.fill(0x5a);recovery();ASSERT_FALSE(HasFailure());
  std::size_t retained=0;
  for(const auto& value:owned) {ASSERT_TRUE(value.fields[2]);ASSERT_TRUE(value.fields[3]);EXPECT_TRUE(*value.fields[3]=="TABLE");if(*value.fields[2]==table_) {++retained;}}
  EXPECT_EQ(1u,retained);EXPECT_EQ(2u,catalog_calls_);EXPECT_EQ(1u,scalar_calls_);
}

TEST_F(RedshiftShowTablesRealTest, NoMatchEmptyTableFilterAndRecovery) {
  connect_bounded();ASSERT_FALSE(HasFailure());
  // Empty table LIKE is an exact no-match for validated nonempty identifiers.
  // It still executes lookup+SHOW, with normal five-field metadata. Empty type
  // vectors have independent unit coverage, not a second native SQLTables call.
  tables("","TABLE",false);ASSERT_FALSE(HasFailure());empty_result();ASSERT_FALSE(HasFailure());
  recovery();ASSERT_FALSE(HasFailure());EXPECT_EQ(1u,catalog_calls_);EXPECT_EQ(1u,scalar_calls_);
}

TEST_F(RedshiftShowTablesRealTest, WideExactMetadataOwnsValuesAndRecovers) {
  connect_bounded();ASSERT_FALSE(HasFailure());tables(literal_pattern(table_),"TABLE",true);ASSERT_FALSE(HasFailure());
  std::array<Descriptor,5> descriptors;metadata(descriptors);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  OwnedRow owned;wide_row(owned,descriptors);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();ASSERT_FALSE(HasFailure());
  const auto remarks=owned.wide_fields[4];const bool null_remarks=owned.wide_remarks_null;wide_.value.fill(SQLWCHAR{0x5a});name_.value.fill(0x5a);
  recovery();ASSERT_FALSE(HasFailure());recheck(owned,descriptors,true);ASSERT_FALSE(HasFailure());
  EXPECT_TRUE(owned.wide_fields[4]==remarks);EXPECT_EQ(null_remarks,owned.wide_remarks_null);EXPECT_EQ(1u,catalog_calls_);EXPECT_EQ(1u,scalar_calls_);
}

// Reuse committed bounded ordinary-TLS helpers, not the old SHOW TABLES cases.
class RedshiftShowColumnsRealTest : public RedshiftShowTablesRealTest {
protected:
  std::optional<std::string> column_input_;
  std::vector<SQLWCHAR> wide_column_;
  struct SmallCell {std::array<unsigned char,8> before;SQLSMALLINT value;std::array<unsigned char,8> after;} small_{};
  std::size_t column_calls_ = 0;
  struct ColumnDescriptor {std::string name;SQLSMALLINT type,scale,nullable;SQLULEN capacity;bool operator==(const ColumnDescriptor&)const=default;};
  struct OwnedColumn {
    std::array<std::optional<std::string>,18> text;
    std::array<std::optional<SQLINTEGER>,18> number;
    std::array<std::optional<std::vector<SQLWCHAR>>,18> wide_text;
    bool operator==(const OwnedColumn&)const=default;
  };
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_SHOW_COLUMNS_ADMISSION");
    if(marker==nullptr) {GTEST_SKIP() << "SHOW columns scope is not admitted";}
    ASSERT_TRUE(std::string_view(marker)=="show-columns-v1") << "Invalid SHOW columns scope marker";
    ASSERT_EQ(2u,sizeof(SQLWCHAR)) << "SHOW columns native scope requires SQLWCHAR width2";
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    // Do not invoke the unrelated SHOW TABLES marker gate.
    RedshiftRealTest::SetUp();if(HasFatalFailure()) {return;}
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    for(const char* key:{"SERVER","PORT","DATABASE","UID","PWD","SSLCAFILE"}) {ASSERT_TRUE(settings.contains(key));ASSERT_FALSE(settings.at(key).empty());}
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test");
    ASSERT_TRUE(settings.at("PORT")=="5439");ASSERT_FALSE(settings.contains("DSN"));ASSERT_FALSE(settings.contains("REDSHIFTCATALOGMODE"));
    const char* schema=std::getenv("ODBCPP_REDSHIFT_TEST_SCHEMA");const char* table=std::getenv("ODBCPP_REDSHIFT_TEST_TABLE");
    ASSERT_NE(nullptr,schema);ASSERT_NE(nullptr,table);
    for(const auto& pair:{std::pair{schema,&schema_},std::pair{table,&table_}}) {
      std::size_t count=0;while(count<=128&&pair.first[count]!='\0') {++count;}
      ASSERT_GT(count,0u);ASSERT_LE(count,128u);
      for(std::size_t index=0;index<count;++index) {
        const auto ch=static_cast<unsigned char>(pair.first[index]);ASSERT_TRUE((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_');
      }
      pair.second->assign(pair.first,count);
    }
    ASSERT_TRUE(schema_=="odbcpp_fixture");ASSERT_TRUE(table_=="pilot_rows");
    if(connection_string_.back()!=';') {connection_string_+=';';}connection_string_+="REDSHIFTCATALOGMODE=SHOW;";
  }
  void columns(std::optional<std::string> pattern,bool use_wide) {
    // SQLColumns catalog is an exact literal, unlike SQLTables catalog LIKE.
    catalog_input_="odbcpp_pilot";schema_input_=literal_pattern(schema_);table_input_=literal_pattern(table_);column_input_=std::move(pattern);
    ASSERT_TRUE(cap(window_end_,true));++column_calls_;SQLRETURN result=SQL_ERROR;
    if(use_wide) {
      wide_catalog_=ascii_units(catalog_input_);wide_schema_=ascii_units(schema_input_);wide_table_=ascii_units(table_input_);
      wide_column_=column_input_?ascii_units(*column_input_):std::vector<SQLWCHAR>{};
      result=SQLColumnsW(hstmt_,wide_catalog_.data(),SQL_NTS,wide_schema_.data(),SQL_NTS,wide_table_.data(),SQL_NTS,
          column_input_?wide_column_.data():nullptr,column_input_?SQL_NTS:0);
      for(auto* input:{&wide_catalog_,&wide_schema_,&wide_table_,&wide_column_}) {std::fill(input->begin(),input->end(),SQLWCHAR{0x5a});}
    } else {
      result=SQLColumns(hstmt_,reinterpret_cast<SQLCHAR*>(catalog_input_.data()),SQL_NTS,
          reinterpret_cast<SQLCHAR*>(schema_input_.data()),SQL_NTS,reinterpret_cast<SQLCHAR*>(table_input_.data()),SQL_NTS,
          column_input_?reinterpret_cast<SQLCHAR*>(column_input_->data()):nullptr,column_input_?SQL_NTS:0);
    }
    for(auto* input:{&catalog_input_,&schema_input_,&table_input_}) {std::fill(input->begin(),input->end(),'!');}
    if(column_input_) {std::fill(column_input_->begin(),column_input_->end(),'!');}
    ASSERT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_STMT,hstmt_);ASSERT_LT(rs::util::Clock::now(),window_end_);
  }
  static constexpr std::array<std::string_view,18> column_names{
      "TABLE_CAT","TABLE_SCHEM","TABLE_NAME","COLUMN_NAME","DATA_TYPE","TYPE_NAME","COLUMN_SIZE","BUFFER_LENGTH",
      "DECIMAL_DIGITS","NUM_PREC_RADIX","NULLABLE","REMARKS","COLUMN_DEF","SQL_DATA_TYPE","SQL_DATETIME_SUB",
      "CHAR_OCTET_LENGTH","ORDINAL_POSITION","IS_NULLABLE"};
  static SQLSMALLINT field_type(std::size_t column) {
    switch(column) {
      case 5:case 9:case 10:case 11:case 14:case 15:return SQL_SMALLINT;
      case 7:case 8:case 16:case 17:return SQL_INTEGER;
      default:return SQL_VARCHAR;
    }
  }
  void column_metadata(std::array<ColumnDescriptor,18>& owned) {
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(18,count);
    for(SQLUSMALLINT column=1;column<=18;++column) {
      poison(name_);name_.value.fill(0x5a);SQLSMALLINT length=-1,type=-1,scale=-1,nullable=-1;SQLULEN capacity=99;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,name_.value.data(),static_cast<SQLSMALLINT>(name_.value.size()),&length,&type,&capacity,&scale,&nullable));
      ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length)+1,name_.value.size());
      const std::string actual(reinterpret_cast<const char*>(name_.value.data()),static_cast<std::size_t>(length));
      EXPECT_TRUE(actual==column_names[column-1]);EXPECT_EQ(field_type(column),type);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
      if(type==SQL_SMALLINT) {EXPECT_EQ(static_cast<SQLULEN>(5),capacity);}
      else if(type==SQL_INTEGER) {EXPECT_EQ(static_cast<SQLULEN>(10),capacity);}
      else {EXPECT_LE(capacity,static_cast<SQLULEN>(65535));} // Preserve actual source dimensions, including unknown0.
      EXPECT_EQ(0,name_.value[static_cast<std::size_t>(length)]);EXPECT_EQ(0x5a,name_.value[static_cast<std::size_t>(length)+1]);guards(name_);ASSERT_FALSE(HasFailure());
      owned[column-1]={actual,type,scale,nullable,capacity};
    }
  }
  static std::optional<SQLINTEGER> literal_number(std::size_t column,bool value) {
    switch(column) {
      case 5:case 14:return value?SQL_VARCHAR:SQL_INTEGER;
      case 7:return value?32:10;
      case 8:return value?32:4;
      case 9:return value?std::nullopt:std::optional<SQLINTEGER>{0};
      case 10:return value?std::nullopt:std::optional<SQLINTEGER>{10};
      case 11:return SQL_NULLABLE;
      case 15:return std::nullopt;
      case 16:return value?std::optional<SQLINTEGER>{32}:std::nullopt;
      case 17:return value?2:1;
      default:return std::nullopt;
    }
  }
  static std::vector<SQLWCHAR> literal_column_text(std::size_t column,bool value) {
    switch(column) {
      case 1:return {'o','d','b','c','p','p','_','p','i','l','o','t'};
      case 2:return {'o','d','b','c','p','p','_','f','i','x','t','u','r','e'};
      case 3:return {'p','i','l','o','t','_','r','o','w','s'};
      case 4:return value?std::vector<SQLWCHAR>{'v','a','l','u','e'}:std::vector<SQLWCHAR>{'i','d'};
      case 6:return value?std::vector<SQLWCHAR>{'c','h','a','r','a','c','t','e','r',' ','v','a','r','y','i','n','g'}:std::vector<SQLWCHAR>{'i','n','t','e','g','e','r'};
      case 18:return {'Y','E','S'};
      default:return {};
    }
  }
  static std::string_view literal_text(std::size_t column,bool value) {
    switch(column) {
      case 1:return "odbcpp_pilot";case 2:return "odbcpp_fixture";case 3:return "pilot_rows";
      case 4:return value?"value":"id";case 6:return value?"character varying":"integer";case 18:return "YES";
      default:return {};
    }
  }
  void numeric_field(SQLUSMALLINT column,bool value,OwnedColumn& owned) {
    const auto expected=literal_number(column,value);length_=97;SQLINTEGER actual=-99;
    if(field_type(column)==SQL_SMALLINT) {
      poison(small_);small_.value=-99;
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_SSHORT,&small_.value,sizeof(small_.value),&length_));
      if(!expected) {EXPECT_EQ(SQL_NULL_DATA,length_);EXPECT_EQ(-99,small_.value);}
      else {EXPECT_EQ(static_cast<SQLLEN>(sizeof(small_.value)),length_);EXPECT_EQ(*expected,small_.value);actual=small_.value;}
      guards(small_);
    } else {
      integer_={17,-99,83};
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_SLONG,&integer_.value,sizeof(integer_.value),&length_));
      if(!expected) {EXPECT_EQ(SQL_NULL_DATA,length_);EXPECT_EQ(-99,integer_.value);}
      else {EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer_.value)),length_);EXPECT_EQ(*expected,integer_.value);actual=integer_.value;}
      EXPECT_EQ(17,integer_.before);EXPECT_EQ(83,integer_.after);
    }
    ASSERT_FALSE(HasFailure());owned.number[column-1]=length_==SQL_NULL_DATA?std::nullopt:std::optional<SQLINTEGER>{actual};
  }
  void text_field(SQLUSMALLINT column,bool value,bool use_wide,const ColumnDescriptor& descriptor,OwnedColumn& owned) {
    length_=97;const bool opaque=column==12||column==13;
    if(use_wide) {
      poison(wide_);wide_.value.fill(SQLWCHAR{0x5a});
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_WCHAR,wide_.value.data(),static_cast<SQLLEN>(sizeof(wide_.value)),&length_));
      if(length_==SQL_NULL_DATA) {
        ASSERT_TRUE(opaque);EXPECT_TRUE(std::all_of(wide_.value.begin(),wide_.value.end(),[](SQLWCHAR ch){return ch==SQLWCHAR{0x5a};}));
        owned.wide_text[column-1].reset();
      } else {
        ASSERT_GE(length_,0);ASSERT_EQ(0,length_%static_cast<SQLLEN>(sizeof(SQLWCHAR)));
        const auto units=static_cast<std::size_t>(length_)/sizeof(SQLWCHAR);ASSERT_LT(units+1,wide_.value.size());
        EXPECT_EQ(SQLWCHAR{0},wide_.value[units]);EXPECT_EQ(SQLWCHAR{0x5a},wide_.value[units+1]);
        owned.wide_text[column-1]=std::vector<SQLWCHAR>(wide_.value.begin(),wide_.value.begin()+static_cast<std::ptrdiff_t>(units));
        if(!opaque) {
          const auto literal=literal_column_text(column,value);ASSERT_EQ(literal.size(),units);EXPECT_TRUE(std::equal(literal.begin(),literal.end(),wide_.value.begin()));
          if(descriptor.capacity!=0) {EXPECT_LE(static_cast<SQLULEN>(units),descriptor.capacity);}
        }
      }
      guards(wide_);
    } else {
      poison(narrow_);narrow_.value.fill('\x5a');
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,narrow_.value.data(),static_cast<SQLLEN>(narrow_.value.size()),&length_));
      if(length_==SQL_NULL_DATA) {
        ASSERT_TRUE(opaque);EXPECT_TRUE(std::all_of(narrow_.value.begin(),narrow_.value.end(),[](char ch){return ch=='\x5a';}));owned.text[column-1].reset();
      } else {
        ASSERT_GE(length_,0);ASSERT_LE(length_,65535);const auto size=static_cast<std::size_t>(length_);
        EXPECT_EQ(0,narrow_.value[size]);EXPECT_EQ('\x5a',narrow_.value[size+1]);owned.text[column-1]=std::string(narrow_.value.data(),size);
        if(!opaque) {EXPECT_TRUE(*owned.text[column-1]==literal_text(column,value));if(descriptor.capacity!=0) {EXPECT_LE(static_cast<SQLULEN>(size),descriptor.capacity);}}
      }
      guards(narrow_);
    }
    ASSERT_FALSE(HasFailure());
  }
  void column_row(bool value,bool use_wide,const std::array<ColumnDescriptor,18>& descriptors,OwnedColumn& owned) {
    ASSERT_LT(rs::util::Clock::now(),window_end_);
    for(SQLUSMALLINT column=1;column<=18;++column) {
      if(field_type(column)==SQL_VARCHAR) {text_field(column,value,use_wide,descriptors[column-1],owned);}
      else {numeric_field(column,value,owned);}
      ASSERT_FALSE(HasFailure());
    }
  }
  void exact_rows(bool use_wide,std::array<ColumnDescriptor,18>& descriptors,std::array<OwnedColumn,2>& rows) {
    column_metadata(descriptors);ASSERT_FALSE(HasFailure());
    for(std::size_t index=0;index<2;++index) {
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));column_row(index==1,use_wide,descriptors,rows[index]);ASSERT_FALSE(HasFailure());
    }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();ASSERT_FALSE(HasFailure());
  }
  void recheck_columns(const std::array<ColumnDescriptor,18>& descriptors,const std::array<OwnedColumn,2>& rows,bool use_wide) {
    for(std::size_t index=0;index<2;++index) {
      ASSERT_LT(rs::util::Clock::now(),window_end_);
      for(std::size_t column=1;column<=18;++column) {
        EXPECT_TRUE(descriptors[column-1].name==column_names[column-1]);EXPECT_EQ(field_type(column),descriptors[column-1].type);
        if(field_type(column)!=SQL_VARCHAR) {EXPECT_TRUE(rows[index].number[column-1]==literal_number(column,index==1));}
        else if(column!=12&&column!=13) {
          if(use_wide) {ASSERT_TRUE(rows[index].wide_text[column-1]);EXPECT_TRUE(*rows[index].wide_text[column-1]==literal_column_text(column,index==1));}
          else {ASSERT_TRUE(rows[index].text[column-1]);EXPECT_TRUE(*rows[index].text[column-1]==literal_text(column,index==1));}
        }
      }
      ASSERT_FALSE(HasFailure());
    }
  }
};

TEST_F(RedshiftShowColumnsRealTest, ExactColumnsOwnEighteenFieldsAndRecover) {
  connect_bounded();ASSERT_FALSE(HasFailure());columns(std::nullopt,false);ASSERT_FALSE(HasFailure());
  std::array<ColumnDescriptor,18> descriptors;std::array<OwnedColumn,2> rows;exact_rows(false,descriptors,rows);ASSERT_FALSE(HasFailure());
  const auto saved=rows;narrow_.value.fill('!');name_.value.fill(0x5a);recovery();ASSERT_FALSE(HasFailure());
  recheck_columns(descriptors,rows,false);ASSERT_FALSE(HasFailure());EXPECT_TRUE(rows==saved);EXPECT_EQ(1u,column_calls_);EXPECT_EQ(1u,scalar_calls_);
}

TEST_F(RedshiftShowColumnsRealTest, ColumnLikeFilterPreservesTypeDimensions) {
  connect_bounded();ASSERT_FALSE(HasFailure());columns(std::string("v_lue"),false);ASSERT_FALSE(HasFailure());
  std::array<ColumnDescriptor,18> descriptors;column_metadata(descriptors);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));OwnedColumn value;column_row(true,false,descriptors,value);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();ASSERT_FALSE(HasFailure());
  const auto saved=value;narrow_.value.fill('!');name_.value.fill(0x5a);recovery();ASSERT_FALSE(HasFailure());EXPECT_TRUE(value==saved);
  ASSERT_TRUE(value.text[3]);EXPECT_TRUE(*value.text[3]=="value");EXPECT_TRUE(value.number[6]==std::optional<SQLINTEGER>{32});
  EXPECT_TRUE(value.number[7]==std::optional<SQLINTEGER>{32});EXPECT_TRUE(value.number[15]==std::optional<SQLINTEGER>{32});
  EXPECT_EQ(1u,column_calls_);EXPECT_EQ(1u,scalar_calls_);
}

TEST_F(RedshiftShowColumnsRealTest, NoMatchEmptyColumnFilterAndRecovery) {
  connect_bounded();ASSERT_FALSE(HasFailure());columns(std::string{},false);ASSERT_FALSE(HasFailure());
  std::array<ColumnDescriptor,18> descriptors;column_metadata(descriptors);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));close_cursor();ASSERT_FALSE(HasFailure());
  recovery();ASSERT_FALSE(HasFailure());EXPECT_EQ(1u,column_calls_);EXPECT_EQ(1u,scalar_calls_);
}

TEST_F(RedshiftShowColumnsRealTest, AnsiWideExactColumnsRemainEquivalentAndRecover) {
  connect_bounded();ASSERT_FALSE(HasFailure());columns(std::nullopt,false);ASSERT_FALSE(HasFailure());
  std::array<ColumnDescriptor,18> narrow_descriptors,wide_descriptors;std::array<OwnedColumn,2> narrow_rows,wide_rows;
  exact_rows(false,narrow_descriptors,narrow_rows);ASSERT_FALSE(HasFailure());columns(std::nullopt,true);ASSERT_FALSE(HasFailure());
  exact_rows(true,wide_descriptors,wide_rows);ASSERT_FALSE(HasFailure());EXPECT_TRUE(narrow_descriptors==wide_descriptors);
  for(std::size_t index=0;index<2;++index) {
    EXPECT_TRUE(narrow_rows[index].number==wide_rows[index].number);
    for(const std::size_t column:{std::size_t{12},std::size_t{13}}) {
      // Opaque source remarks/defaults are not guessed NULL/empty. Preserve their
      // actual A/W NULLness; compare exact code units when the source is ASCII.
      EXPECT_EQ(bool(narrow_rows[index].text[column-1]),bool(wide_rows[index].wide_text[column-1]));
      if(narrow_rows[index].text[column-1]) {
        ASSERT_TRUE(wide_rows[index].wide_text[column-1]);const auto& text=*narrow_rows[index].text[column-1];
        if(std::all_of(text.begin(),text.end(),[](unsigned char ch){return ch<128;})) {
          const auto units=ascii_units(text);ASSERT_EQ(text.size(),wide_rows[index].wide_text[column-1]->size());
          EXPECT_TRUE(std::equal(units.begin(),units.end()-1,wide_rows[index].wide_text[column-1]->begin()));
        }
      }
    }
  }
  const auto saved_narrow=narrow_rows,saved_wide=wide_rows;narrow_.value.fill('!');wide_.value.fill(SQLWCHAR{0x5a});name_.value.fill(0x5a);
  recovery();ASSERT_FALSE(HasFailure());recheck_columns(narrow_descriptors,narrow_rows,false);ASSERT_FALSE(HasFailure());
  recheck_columns(wide_descriptors,wide_rows,true);ASSERT_FALSE(HasFailure());EXPECT_TRUE(narrow_rows==saved_narrow);EXPECT_TRUE(wide_rows==saved_wide);
  EXPECT_EQ(2u,column_calls_);EXPECT_EQ(1u,scalar_calls_);
}

// Selected committed arrays/STATIC/bookmarks only; an independently admitted
// native runner owns credentials, binary identity, aggregate time and pause.
class RedshiftRecentFeaturesRealTest : public RedshiftBufferedLifecycleRealTest {
protected:
  static constexpr const char* rows_sql_ =
      "SELECT CAST(1 AS INTEGER) AS id,CAST('one' AS VARCHAR(16)) AS label "
      "UNION ALL SELECT CAST(2 AS INTEGER),CAST(NULL AS VARCHAR(16)) "
      "UNION ALL SELECT CAST(3 AS INTEGER),CAST('three' AS VARCHAR(16)) ORDER BY id";
  static constexpr const char* insert_sql_ =
      "INSERT INTO odbcpp_recent_array_native (ordinal,value) VALUES (?,?)";
  static constexpr const char* summary_sql_ =
      "SELECT CAST(COUNT(*) AS INTEGER),CAST(SUM(ordinal) AS INTEGER),"
      "CAST(COUNT(value) AS INTEGER),CAST(SUM(value) AS INTEGER) "
      "FROM odbcpp_recent_array_native";
  unsigned statements_{0}; bool temp_scope_started_{false};
  SQLINTEGER ids_[3]{1,2,3}, values_[3]{11,22,33}, bound_ids_[3]{-91,-91,-91};
  SQLLEN id_lengths_[3]{0,0,0}, value_lengths_[3]{0,0,SQL_NULL_DATA}, bound_lengths_[3]{73,73,73};
  SQLUSMALLINT operations_[3]{SQL_PARAM_PROCEED,SQL_PARAM_IGNORE,SQL_PARAM_PROCEED};
  struct ParameterStatus { unsigned char before{0xa1}; SQLUSMALLINT slots[3]{71,72,73}; unsigned char after{0xb2}; } parameter_statuses_;
  struct RowStatus { unsigned char before{0xc3}; SQLUSMALLINT slots[3]{71,72,73}; unsigned char after{0xd4}; } row_statuses_;
  SQLULEN processed_{91}, fetched_{91}, offset_{0};
  struct ParameterRow {
    unsigned char before[3]{0xa1,0xa2,0xa3};
    SQLINTEGER id{111},value{222}; SQLLEN id_length{0},value_length{0};
    unsigned char after[3]{0xb1,0xb2,0xb3};
  } parameter_pages_[2][3];
  unsigned char tokens_[3][24]{}, saved_[24]{}, pieces_[24]{}, invalid_[24]{}, sibling_token_[24]{};
  SQLLEN token_lengths_[3]{73,73,73}, piece_length_{73};

  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_RECENT_FEATURES_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Recent committed feature scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="recent-committed-features-v1");
    // The complete selected inventory includes only this fixed session TEMP DDL.
    const char* temp=std::getenv("ODBCPP_REDSHIFT_RECENT_TEMP_TABLE_ADMISSION");
    ASSERT_NE(temp,nullptr); ASSERT_TRUE(std::string_view(temp)=="session-temp-insert-v1");
    window_end_=rs::util::Clock::now()+std::chrono::seconds{60}; admitted_=true;
    RedshiftRealTest::SetUp(); if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(settings.contains("DATABASE")); ASSERT_TRUE(settings.contains("UID"));
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");
    ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test"); ASSERT_FALSE(settings.contains("DSN"));
  }
  bool budget(unsigned count=1) {
    if(count>24||statements_>24-count) { ADD_FAILURE() << "Recent-feature SQL bound exceeded"; return false; }
    statements_+=count; return true;
  }
  void ready() {
    connect_bounded(); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&second_));
    ASSERT_TRUE(cap(window_end_,second_));
  }
  void native_direct(SQLHSTMT statement,const char* sql) {
    ASSERT_TRUE(budget()); direct(statement,sql);
  }
  void native_execute(unsigned maximum_proceeding) {
    // One original description plus at most N executions; no retry or fallback.
    ASSERT_TRUE(budget(maximum_proceeding+1)); execute(hstmt_);
  }
  void create_temp() {
    native_direct(second_,"CREATE TEMP TABLE odbcpp_recent_array_native (ordinal INTEGER,value INTEGER)");
    ASSERT_FALSE(HasFailure());
    temp_scope_started_=true; // Confirmed session TEMP ownership only; failed CREATE never authorizes DROP.
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(second_,SQL_CLOSE));
  }
  void drop_temp() {
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(second_,SQL_CLOSE));
    native_direct(second_,"DROP TABLE IF EXISTS odbcpp_recent_array_native"); ASSERT_FALSE(HasFailure());
    temp_scope_started_=false; ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(second_,SQL_CLOSE));
  }
  void array_attributes(SQLULEN stride) {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMSET_SIZE,reinterpret_cast<SQLPOINTER>(std::uintptr_t{3}),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_BIND_TYPE,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(stride)),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_OPERATION_PTR,operations_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_STATUS_PTR,parameter_statuses_.slots,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed_,0));
  }
  void two_counts() {
    EXPECT_EQ(3u,processed_); EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_statuses_.slots[0]);
    EXPECT_EQ(SQL_PARAM_UNUSED,parameter_statuses_.slots[1]); EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_statuses_.slots[2]);
    SQLLEN count=-1; SQLSMALLINT columns=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&columns)); EXPECT_EQ(0,columns);
    ASSERT_EQ(SQL_SUCCESS,SQLRowCount(hstmt_,&count)); EXPECT_EQ(1,count);
    ASSERT_EQ(SQL_SUCCESS,SQLMoreResults(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLRowCount(hstmt_,&count)); EXPECT_EQ(1,count);
    ASSERT_EQ(SQL_NO_DATA,SQLMoreResults(hstmt_));
    EXPECT_EQ(0xa1,parameter_statuses_.before); EXPECT_EQ(0xb2,parameter_statuses_.after);
  }
  void summary(SQLINTEGER count,SQLINTEGER ordinal_sum,SQLINTEGER nonnull,SQLINTEGER value_sum) {
    native_direct(second_,summary_sql_); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(second_));
    integer(second_,1,count); ASSERT_FALSE(HasFailure());
    integer(second_,2,ordinal_sum); ASSERT_FALSE(HasFailure());
    integer(second_,3,nonnull); ASSERT_FALSE(HasFailure());
    integer(second_,4,value_sum); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(second_)); ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(second_,SQL_CLOSE));
  }
  void static_setting(SQLHSTMT statement,bool bookmarks=false) {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(statement,SQL_ATTR_CURSOR_TYPE,reinterpret_cast<SQLPOINTER>(SQL_CURSOR_STATIC),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(statement,SQL_ATTR_CONCURRENCY,reinterpret_cast<SQLPOINTER>(SQL_CONCUR_READ_ONLY),0));
    if(bookmarks) { ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(statement,SQL_ATTR_USE_BOOKMARKS,reinterpret_cast<SQLPOINTER>(SQL_UB_VARIABLE),0)); }
  }
  void row_outputs(SQLULEN count) {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_ARRAY_SIZE,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(count)),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,&fetched_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,row_statuses_.slots,0));
  }
  void row_number(SQLHSTMT statement,SQLULEN expected) {
    SQLULEN row=99; ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(statement,SQL_ATTR_ROW_NUMBER,&row,0,nullptr)); EXPECT_EQ(expected,row);
  }
  void saved_fetch(SQLLEN delta=0) {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_FETCH_BOOKMARK_PTR,saved_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_BOOKMARK,delta));
  }
  void TearDown() override {
    // Every borrowed member stays alive until both statements are freed below.
    if(temp_scope_started_&&connected_&&second_&&rs::util::Clock::now()<window_end_) {
      if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE)); }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeStmt(second_,SQL_CLOSE));
      const auto cutoff=std::min(window_end_,rs::util::Clock::now()+std::chrono::seconds{5});
      if(budget()&&cap(cutoff,second_)) {
        const auto result=SQLExecDirect(second_,reinterpret_cast<SQLCHAR*>(const_cast<char*>("DROP TABLE IF EXISTS odbcpp_recent_array_native")),SQL_NTS);
        EXPECT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_STMT,second_);
      }
    }
    // Disconnect destroys session-local TEMP state even after a fatal assertion;
    // existing bounded cleanup records an unexpected transaction rather than hiding it.
    RedshiftBufferedLifecycleRealTest::TearDown();
  }
};

TEST_F(RedshiftRecentFeaturesRealTest, ColumnCommandArraysIgnoreNullCountsAndImmediateRefill) {
  ready(); ASSERT_FALSE(HasFailure()); create_temp(); ASSERT_FALSE(HasFailure());
  prepare(hstmt_,insert_sql_); ASSERT_FALSE(HasFailure()); array_attributes(SQL_PARAM_BIND_BY_COLUMN); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,ids_,0,id_lengths_));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,2,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,values_,0,value_lengths_));
  native_execute(2); ASSERT_FALSE(HasFailure());
  for(auto& value:ids_) { value=-99; } for(auto& value:values_) { value=-99; }
  two_counts(); ASSERT_FALSE(HasFailure()); summary(2,4,1,11); ASSERT_FALSE(HasFailure());
  // Nonmutating retained IPD count, then immediate Execute without reprepare.
  SQLHDESC ipd{}; SQLSMALLINT count=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_IMP_PARAM_DESC,&ipd,0,nullptr));
  ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ipd,0,SQL_DESC_COUNT,&count,0,nullptr)); EXPECT_EQ(2,count);
  ids_[0]=4; ids_[1]=5; ids_[2]=6; values_[0]=44; values_[1]=55; values_[2]=66;
  value_lengths_[2]=0; processed_=91;
  native_execute(2); ASSERT_FALSE(HasFailure()); two_counts(); ASSERT_FALSE(HasFailure());
  summary(4,14,3,121); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS)); drop_temp(); ASSERT_FALSE(HasFailure());
  native_direct(second_,"SELECT CAST(1 AS INTEGER)"); ASSERT_FALSE(HasFailure()); scalar(second_,1); ASSERT_FALSE(HasFailure());
}

TEST_F(RedshiftRecentFeaturesRealTest, RowWiseCommandArraysOffsetIgnoreRefusalAndRecovery) {
  ready(); ASSERT_FALSE(HasFailure()); create_temp(); ASSERT_FALSE(HasFailure());
  for(std::size_t slot=0;slot<3;++slot) {
    parameter_pages_[1][slot].id=static_cast<SQLINTEGER>(7+slot);
    parameter_pages_[1][slot].value=static_cast<SQLINTEGER>(70+slot*10);
  }
  parameter_pages_[1][1].id_length=-91; // IGNORE must not consume an invalid unused indicator.
  parameter_pages_[1][2].value_length=SQL_NULL_DATA;
  prepare(hstmt_,insert_sql_); ASSERT_FALSE(HasFailure()); array_attributes(sizeof(ParameterRow)); ASSERT_FALSE(HasFailure());
  offset_=sizeof(parameter_pages_[0]);
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_BIND_OFFSET_PTR,&offset_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&parameter_pages_[0][0].id,0,&parameter_pages_[0][0].id_length));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,2,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&parameter_pages_[0][0].value,0,&parameter_pages_[0][0].value_length));
  native_execute(2); ASSERT_FALSE(HasFailure()); two_counts(); ASSERT_FALSE(HasFailure()); summary(2,16,1,70); ASSERT_FALSE(HasFailure());
  operations_[0]=99; processed_=91; parameter_statuses_.slots[0]=71;
  ASSERT_TRUE(cap(window_end_,hstmt_)); ASSERT_TRUE(budget(1));
  ASSERT_EQ(SQL_ERROR,SQLExecute(hstmt_)); EXPECT_EQ("HY024",get_error(SQL_HANDLE_STMT,hstmt_));
  EXPECT_EQ(91u,processed_); EXPECT_EQ(71,parameter_statuses_.slots[0]);
  summary(2,16,1,70); ASSERT_FALSE(HasFailure());
  operations_[0]=SQL_PARAM_PROCEED;
  parameter_pages_[1][0].id=10; parameter_pages_[1][2].id=12;
  parameter_pages_[1][0].value=100; parameter_pages_[1][2].value=120; parameter_pages_[1][2].value_length=0;
  native_execute(2); ASSERT_FALSE(HasFailure()); two_counts(); ASSERT_FALSE(HasFailure()); summary(4,38,3,290); ASSERT_FALSE(HasFailure());
  for(const auto& page:parameter_pages_) { for(const auto& row:page) {
    EXPECT_EQ(0xa1,row.before[0]); EXPECT_EQ(0xa3,row.before[2]); EXPECT_EQ(0xb1,row.after[0]); EXPECT_EQ(0xb3,row.after[2]);
  } }
  EXPECT_EQ(111,parameter_pages_[0][0].id); EXPECT_EQ(222,parameter_pages_[0][0].value);
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS)); drop_temp(); ASSERT_FALSE(HasFailure());
}

TEST_F(RedshiftRecentFeaturesRealTest, StaticRowsetsPositionNullOldSizeAndSiblingOwnership) {
  ready(); ASSERT_FALSE(HasFailure()); static_setting(hstmt_); ASSERT_FALSE(HasFailure()); row_outputs(2); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_SLONG,bound_ids_,0,bound_lengths_));
  native_direct(hstmt_,rows_sql_); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_FIRST,0)); EXPECT_EQ(2u,fetched_);
  EXPECT_EQ(1,bound_ids_[0]); EXPECT_EQ(2,bound_ids_[1]); EXPECT_EQ(SQL_ROW_SUCCESS,row_statuses_.slots[0]); EXPECT_EQ(SQL_ROW_SUCCESS,row_statuses_.slots[1]);
  ASSERT_EQ(SQL_SUCCESS,SQLSetPos(hstmt_,2,SQL_POSITION,SQL_LOCK_NO_CHANGE));
  text_.value.fill('\x5a'); output_length_=73;
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,2,SQL_C_CHAR,text_.value.data(),static_cast<SQLLEN>(text_.value.size()),&output_length_));
  EXPECT_EQ(SQL_NULL_DATA,output_length_); EXPECT_EQ('\x5a',text_.value[0]);
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND)); integer(hstmt_,1,2); ASSERT_FALSE(HasFailure());
  row_outputs(1); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_NEXT,0)); row_number(hstmt_,3); ASSERT_FALSE(HasFailure()); integer(hstmt_,1,3); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_PRIOR,0)); row_number(hstmt_,2); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_ABSOLUTE,-1)); row_number(hstmt_,3); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_NO_DATA,SQLFetchScroll(hstmt_,SQL_FETCH_NEXT,0)); EXPECT_EQ(0u,fetched_); EXPECT_EQ(SQL_ROW_NOROW,row_statuses_.slots[0]); row_number(hstmt_,0); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_FIRST,0));
  native_direct(second_,"SELECT CAST(9 AS INTEGER)"); ASSERT_FALSE(HasFailure()); scalar(second_,9); ASSERT_FALSE(HasFailure());
  integer(hstmt_,1,1); ASSERT_FALSE(HasFailure()); EXPECT_EQ(0xc3,row_statuses_.before); EXPECT_EQ(0xd4,row_statuses_.after);
}

TEST_F(RedshiftRecentFeaturesRealTest, VariableBookmarkMetadataLiteralOffsetsChunksStaleAndForeignRecovery) {
  ready(); ASSERT_FALSE(HasFailure()); static_setting(hstmt_,true); ASSERT_FALSE(HasFailure()); row_outputs(3); ASSERT_FALSE(HasFailure());
  native_direct(hstmt_,rows_sql_); ASSERT_FALSE(HasFailure());
  SQLUINTEGER mask=0;
  ASSERT_EQ(SQL_SUCCESS,SQLGetInfo(hdbc_,SQL_BOOKMARK_PERSISTENCE,&mask,sizeof(mask),nullptr)); EXPECT_EQ(SQL_BP_TRANSACTION,mask);
  ASSERT_EQ(SQL_SUCCESS,SQLGetInfo(hdbc_,SQL_STATIC_CURSOR_ATTRIBUTES1,&mask,sizeof(mask),nullptr)); EXPECT_NE(0u,mask&SQL_CA1_BOOKMARK);
  ASSERT_EQ(SQL_SUCCESS,SQLGetInfo(hdbc_,SQL_FETCH_DIRECTION,&mask,sizeof(mask),nullptr)); EXPECT_NE(0u,mask&SQL_FD_FETCH_BOOKMARK);
  SQLCHAR name[2]{0x5a,0x5a}; SQLWCHAR wide_name[2]{0x5a,0x5a};
  SQLSMALLINT length=-1,type=-1,scale=-1,nullable=-1; SQLULEN size=0; SQLLEN display=-1,unsigned_value=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,0,name,2,&length,&type,&size,&scale,&nullable));
  EXPECT_EQ(0,length); EXPECT_EQ(0,name[0]); EXPECT_EQ(0x5a,name[1]); EXPECT_EQ(SQL_BINARY,type); EXPECT_EQ(24u,size); EXPECT_EQ(0,scale); EXPECT_EQ(SQL_NO_NULLS,nullable);
  ASSERT_EQ(SQL_SUCCESS,SQLDescribeColW(hstmt_,0,wide_name,2,&length,&type,&size,&scale,&nullable)); EXPECT_EQ(0,length); EXPECT_EQ(0,wide_name[0]); EXPECT_EQ(0x5a,wide_name[1]);
  ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(hstmt_,0,SQL_DESC_DISPLAY_SIZE,nullptr,0,nullptr,&display)); EXPECT_EQ(48,display);
  ASSERT_EQ(SQL_SUCCESS,SQLColAttributeW(hstmt_,0,SQL_DESC_UNSIGNED,nullptr,0,nullptr,&unsigned_value)); EXPECT_EQ(SQL_TRUE,unsigned_value);
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,0,SQL_C_VARBOOKMARK,tokens_,24,token_lengths_));
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_FIRST,0)); EXPECT_EQ(3u,fetched_);
  for(std::size_t row=0;row<3;++row) {
    EXPECT_EQ(24,token_lengths_[row]); EXPECT_EQ(0,std::memcmp(tokens_[0],tokens_[row],16));
    for(std::size_t byte=16;byte<23;++byte) { EXPECT_EQ(0,tokens_[row][byte]); }
    EXPECT_EQ(row+1,tokens_[row][23]);
  }
  EXPECT_TRUE(std::any_of(tokens_[0],tokens_[0]+8,[](unsigned char b){return b!=0;}));
  EXPECT_TRUE(std::any_of(tokens_[0]+8,tokens_[0]+16,[](unsigned char b){return b!=0;}));
  std::memcpy(saved_,tokens_[1],24); ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND)); row_outputs(1); ASSERT_FALSE(HasFailure());
  saved_fetch(); ASSERT_FALSE(HasFailure()); row_number(hstmt_,2); ASSERT_FALSE(HasFailure()); integer(hstmt_,1,2); ASSERT_FALSE(HasFailure());
  saved_fetch(-1); ASSERT_FALSE(HasFailure()); row_number(hstmt_,1); ASSERT_FALSE(HasFailure());
  saved_fetch(1); ASSERT_FALSE(HasFailure()); row_number(hstmt_,3); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_NO_DATA,SQLFetchScroll(hstmt_,SQL_FETCH_BOOKMARK,2)); EXPECT_EQ(0u,fetched_);
  saved_fetch(); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLGetData(hstmt_,0,SQL_C_VARBOOKMARK,pieces_,7,&piece_length_)); EXPECT_EQ("01004",get_error(SQL_HANDLE_STMT,hstmt_)); EXPECT_EQ(24,piece_length_);
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,0,SQL_C_VARBOOKMARK,pieces_+7,17,&piece_length_)); EXPECT_EQ(17,piece_length_); EXPECT_EQ(0,std::memcmp(saved_,pieces_,24));
  ASSERT_EQ(SQL_NO_DATA,SQLGetData(hstmt_,0,SQL_C_VARBOOKMARK,pieces_,24,&piece_length_));
  ASSERT_EQ(SQL_SUCCESS,SQLSetPos(hstmt_,1,SQL_POSITION,SQL_LOCK_NO_CHANGE));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLGetData(hstmt_,0,SQL_C_VARBOOKMARK,pieces_,0,&piece_length_)); EXPECT_EQ(24,piece_length_);
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,0,SQL_C_VARBOOKMARK,pieces_,24,&piece_length_)); EXPECT_EQ(0,std::memcmp(saved_,pieces_,24));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_FETCH_BOOKMARK_PTR,invalid_,0)); fetched_=91; row_statuses_.slots[0]=71;
  ASSERT_EQ(SQL_ERROR,SQLFetchScroll(hstmt_,SQL_FETCH_BOOKMARK,0)); EXPECT_EQ("HY111",get_error(SQL_HANDLE_STMT,hstmt_)); EXPECT_EQ(91u,fetched_); EXPECT_EQ(71,row_statuses_.slots[0]); row_number(hstmt_,2); ASSERT_FALSE(HasFailure());
  saved_fetch(); ASSERT_FALSE(HasFailure());
  static_setting(second_,true); ASSERT_FALSE(HasFailure()); native_direct(second_,rows_sql_); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(second_,SQL_ATTR_FETCH_BOOKMARK_PTR,saved_,0));
  ASSERT_EQ(SQL_ERROR,SQLFetchScroll(second_,SQL_FETCH_BOOKMARK,0)); EXPECT_EQ("HY111",get_error(SQL_HANDLE_STMT,second_));
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(second_,SQL_FETCH_FIRST,0));
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(second_,0,SQL_C_VARBOOKMARK,sibling_token_,24,&piece_length_));
  EXPECT_NE(0,std::memcmp(saved_,sibling_token_,8));
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE)); native_direct(hstmt_,rows_sql_); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_FETCH_BOOKMARK_PTR,saved_,0));
  ASSERT_EQ(SQL_ERROR,SQLFetchScroll(hstmt_,SQL_FETCH_BOOKMARK,0)); EXPECT_EQ("HY111",get_error(SQL_HANDLE_STMT,hstmt_)); row_number(hstmt_,0); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(hstmt_,SQL_FETCH_FIRST,0)); integer(hstmt_,1,1); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetchScroll(second_,SQL_FETCH_NEXT,0)); integer(second_,1,2); ASSERT_FALSE(HasFailure());
  EXPECT_EQ(2,saved_[23]); EXPECT_EQ(1,sibling_token_[23]);
}

// Native proof for the already committed binding policies only. The independent
// root runner must bind this exact filter, source/binary, time and SQL allowance.
class RedshiftExpandedBindingRealTest : public RedshiftBufferedLifecycleRealTest {
protected:
  static constexpr const char* narrowing_sql_=
      "SELECT CAST(1 AS INTEGER) AS ordinal,CAST(1 AS INTEGER) AS n,CAST(NULL AS VARCHAR(16)) AS label "
      "UNION ALL SELECT CAST(2 AS INTEGER),CAST(32768 AS INTEGER),CAST('two' AS VARCHAR(16)) "
      "UNION ALL SELECT CAST(3 AS INTEGER),CAST(3 AS INTEGER),CAST('three' AS VARCHAR(16)) ORDER BY ordinal";
  static constexpr const char* rows_sql_=
      "SELECT CAST(1 AS INTEGER) AS id,CAST('one' AS VARCHAR(16)) AS label "
      "UNION ALL SELECT CAST(2 AS INTEGER),CAST(NULL AS VARCHAR(16)) "
      "UNION ALL SELECT CAST(3 AS INTEGER),CAST('three' AS VARCHAR(16)) ORDER BY id";
  static constexpr const char* parameter_sql_="SELECT CAST(? AS INTEGER) AS n";
  unsigned charges_{0};
  struct ColumnBuffers {
    unsigned char before{0xa1}; SQLSMALLINT narrow[2]{-91,-92};
    SQLINTEGER wide[2]{-91,-92}; SQLLEN lengths[2]{73,74};
    char text[2][16]{}; SQLLEN text_lengths[2]{75,76}; unsigned char after{0xb2};
  } columns_;
  struct StatusBuffers { unsigned char before{0xc3}; SQLUSMALLINT rows[2]{71,72}; unsigned char after{0xd4}; } statuses_;
  struct OutputRow {
    unsigned char before[3]{0xa1,0xa2,0xa3}; SQLINTEGER value{-91}; SQLLEN length{73};
    SQLLEN indicator{74},octets{75}; unsigned char after[3]{0xb1,0xb2,0xb3};
  } pages_[2][2];
  struct InputPage {
    unsigned char before[3]{0xe1,0xe2,0xe3}; SQLINTEGER value{7}; SQLLEN length{0};
    unsigned char after[3]{0xf1,0xf2,0xf3};
  } inputs_[2];
  SQLULEN fetched_{91},offset_{0},processed_{91};
  SQLUSMALLINT operation_{SQL_PARAM_PROCEED},parameter_status_{71};
  SQLWCHAR chunk_[4]{0x7777,0x7777,0x7777,0x7777}; SQLLEN chunk_length_{73};

  static SQLPOINTER number(std::uintptr_t value) { return reinterpret_cast<SQLPOINTER>(value); }
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_EXPANDED_BINDING_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Expanded binding scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="recent-expanded-bindings-v1");
    window_end_=rs::util::Clock::now()+std::chrono::seconds{60}; admitted_=true;
    RedshiftRealTest::SetUp(); if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(settings.contains("DATABASE")); ASSERT_TRUE(settings.contains("UID"));
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot");
    ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test"); ASSERT_FALSE(settings.contains("DSN"));
  }
  bool charge(unsigned count=1) {
    if(count>24||charges_>24-count) { ADD_FAILURE() << "Expanded binding SQL bound exceeded"; return false; }
    charges_+=count; return true;
  }
  void ready() {
    connect_bounded(); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&second_));
    ASSERT_TRUE(cap(window_end_,second_));
  }
  void native_direct(SQLHSTMT statement,const char* sql) {
    ASSERT_TRUE(charge()); direct(statement,sql);
  }
  void native_prepare() {
    // Opt-in Prepare performs one bounded description exchange.
    ASSERT_TRUE(charge()); prepare(hstmt_,parameter_sql_);
  }
  void native_execute() {
    // Conservative one description plus one scalar execution, even if cached
    // or refused locally. The guard is not an assertion of zero backend traffic.
    ASSERT_TRUE(charge(2)); execute(hstmt_);
  }
  void rowset(SQLULEN count) {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_ARRAY_SIZE,number(count),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROWS_FETCHED_PTR,&fetched_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_STATUS_PTR,statuses_.rows,0));
  }
  void guards() {
    EXPECT_EQ(0xa1,columns_.before); EXPECT_EQ(0xb2,columns_.after);
    EXPECT_EQ(0xc3,statuses_.before); EXPECT_EQ(0xd4,statuses_.after);
    for(const auto& page:pages_) for(const auto& row:page) {
      EXPECT_EQ(0xa1,row.before[0]); EXPECT_EQ(0xa2,row.before[1]); EXPECT_EQ(0xa3,row.before[2]);
      EXPECT_EQ(0xb1,row.after[0]); EXPECT_EQ(0xb2,row.after[1]); EXPECT_EQ(0xb3,row.after[2]);
    }
    for(const auto& input:inputs_) {
      EXPECT_EQ(0xe1,input.before[0]); EXPECT_EQ(0xe2,input.before[1]); EXPECT_EQ(0xe3,input.before[2]);
      EXPECT_EQ(0xf1,input.after[0]); EXPECT_EQ(0xf2,input.after[1]); EXPECT_EQ(0xf3,input.after[2]);
    }
  }
};

TEST_F(RedshiftExpandedBindingRealTest, ActualForwardTwoRowsNarrowingNullPartialAndRebindRecovery) {
  ready(); ASSERT_FALSE(HasFailure());
  rowset(2); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,2,SQL_C_SSHORT,columns_.narrow,0,columns_.lengths));
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,3,SQL_C_CHAR,columns_.text,sizeof(columns_.text[0]),columns_.text_lengths));
  std::memset(columns_.text,'x',sizeof(columns_.text));
  native_direct(hstmt_,narrowing_sql_); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLFetch(hstmt_));
  EXPECT_EQ("22003",get_error(SQL_HANDLE_STMT,hstmt_));
  EXPECT_EQ(2u,fetched_); EXPECT_EQ(SQL_ROW_SUCCESS,statuses_.rows[0]); EXPECT_EQ(SQL_ROW_ERROR,statuses_.rows[1]);
  EXPECT_EQ(1,columns_.narrow[0]); EXPECT_EQ(-92,columns_.narrow[1]);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLSMALLINT)),columns_.lengths[0]); EXPECT_EQ(74,columns_.lengths[1]);
  EXPECT_EQ(SQL_NULL_DATA,columns_.text_lengths[0]); EXPECT_EQ(76,columns_.text_lengths[1]);
  EXPECT_EQ('x',columns_.text[0][0]); EXPECT_EQ('x',columns_.text[1][0]);
  SQLLEN row=-1; SQLINTEGER column=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetDiagField(SQL_HANDLE_STMT,hstmt_,1,SQL_DIAG_ROW_NUMBER,&row,0,nullptr)); EXPECT_EQ(2,row);
  ASSERT_EQ(SQL_SUCCESS,SQLGetDiagField(SQL_HANDLE_STMT,hstmt_,1,SQL_DIAG_COLUMN_NUMBER,&column,0,nullptr)); EXPECT_EQ(2,column);
  guards(); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); EXPECT_EQ(1u,fetched_);
  EXPECT_EQ(SQL_ROW_SUCCESS,statuses_.rows[0]); EXPECT_EQ(SQL_ROW_NOROW,statuses_.rows[1]);
  EXPECT_EQ(3,columns_.narrow[0]); EXPECT_EQ(-92,columns_.narrow[1]);
  EXPECT_EQ(5,columns_.text_lengths[0]); EXPECT_EQ(0,std::memcmp("three\0",columns_.text[0],6));
  EXPECT_EQ(76,columns_.text_lengths[1]); EXPECT_EQ('x',columns_.text[1][0]);
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); EXPECT_EQ(0u,fetched_);
  EXPECT_EQ(SQL_ROW_NOROW,statuses_.rows[0]); EXPECT_EQ(SQL_ROW_NOROW,statuses_.rows[1]);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_)); ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,2,SQL_C_SLONG,columns_.wide,0,columns_.lengths));
  native_direct(hstmt_,narrowing_sql_); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); EXPECT_EQ(2u,fetched_);
  EXPECT_EQ(1,columns_.wide[0]); EXPECT_EQ(32768,columns_.wide[1]);
  EXPECT_EQ(SQL_ROW_SUCCESS,statuses_.rows[0]); EXPECT_EQ(SQL_ROW_SUCCESS,statuses_.rows[1]);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); EXPECT_EQ(3,columns_.wide[0]); EXPECT_EQ(SQL_ROW_NOROW,statuses_.rows[1]);
  guards();
}

TEST_F(RedshiftExpandedBindingRealTest, RowWiseOffsetIndicatorOnlySuppressionOnAndScalarGetDataRecovery) {
  ready(); ASSERT_FALSE(HasFailure());
  rowset(2); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_BIND_TYPE,number(sizeof(OutputRow)),0));
  offset_=sizeof(pages_[0]);
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_BIND_OFFSET_PTR,&offset_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,1,SQL_C_SLONG,&pages_[0][0].value,0,&pages_[0][0].length));
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt_,2,SQL_C_WCHAR,nullptr,0,&pages_[0][0].indicator));
  SQLHDESC ard{}; SQLPOINTER base{};
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_APP_ROW_DESC,&ard,0,nullptr));
  ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard,2,SQL_DESC_OCTET_LENGTH_PTR,&pages_[0][0].octets,0));
  ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ard,1,SQL_DESC_DATA_PTR,&base,0,nullptr)); EXPECT_EQ(&pages_[0][0].value,base);
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_RETRIEVE_DATA,number(SQL_RD_OFF),0));
  native_direct(hstmt_,rows_sql_); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); EXPECT_EQ(2u,fetched_);
  EXPECT_EQ(SQL_ROW_SUCCESS,statuses_.rows[0]); EXPECT_EQ(SQL_ROW_SUCCESS,statuses_.rows[1]);
  for(const auto& page:pages_) for(const auto& row:page) {
    EXPECT_EQ(-91,row.value); EXPECT_EQ(73,row.length); EXPECT_EQ(74,row.indicator); EXPECT_EQ(75,row.octets);
  }
  EXPECT_EQ(SQL_ERROR,SQLSetStmtAttr(hstmt_,SQL_ATTR_RETRIEVE_DATA,number(99),0));
  EXPECT_EQ("HY024",get_error(SQL_HANDLE_STMT,hstmt_));
  SQLULEN mode=99; ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_RETRIEVE_DATA,&mode,0,nullptr)); EXPECT_EQ(SQL_RD_OFF,mode);
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_RETRIEVE_DATA,number(SQL_RD_ON),0));
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); EXPECT_EQ(1u,fetched_); EXPECT_EQ(3,pages_[1][0].value);
  EXPECT_EQ(0,pages_[1][0].indicator); EXPECT_EQ(static_cast<SQLLEN>(5*sizeof(SQLWCHAR)),pages_[1][0].octets);
  EXPECT_EQ(SQL_ROW_NOROW,statuses_.rows[1]); EXPECT_EQ(-91,pages_[1][1].value);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  native_direct(hstmt_,rows_sql_); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); EXPECT_EQ(1,pages_[1][0].value); EXPECT_EQ(2,pages_[1][1].value);
  EXPECT_EQ(0,pages_[1][0].indicator); EXPECT_EQ(static_cast<SQLLEN>(3*sizeof(SQLWCHAR)),pages_[1][0].octets);
  EXPECT_EQ(SQL_NULL_DATA,pages_[1][1].indicator); EXPECT_EQ(75,pages_[1][1].octets);
  for(const auto& row:pages_[0]) { EXPECT_EQ(-91,row.value); EXPECT_EQ(73,row.length); EXPECT_EQ(74,row.indicator); EXPECT_EQ(75,row.octets); }
  ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ard,1,SQL_DESC_DATA_PTR,&base,0,nullptr)); EXPECT_EQ(&pages_[0][0].value,base);
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_ROW_BIND_OFFSET_PTR,&base,0,nullptr)); EXPECT_EQ(&offset_,base);
  guards(); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_)); ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_BIND_OFFSET_PTR,nullptr,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ROW_BIND_TYPE,number(SQL_BIND_BY_COLUMN),0));
  rowset(1); ASSERT_FALSE(HasFailure());
  native_direct(hstmt_,rows_sql_); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLGetData(hstmt_,2,SQL_C_WCHAR,chunk_,2*sizeof(SQLWCHAR),&chunk_length_));
  EXPECT_EQ("01004",get_error(SQL_HANDLE_STMT,hstmt_));
  EXPECT_EQ(static_cast<SQLLEN>(3*sizeof(SQLWCHAR)),chunk_length_); EXPECT_EQ('o',chunk_[0]); EXPECT_EQ(0,chunk_[1]); EXPECT_EQ(0x7777,chunk_[3]);
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,2,SQL_C_WCHAR,chunk_,3*sizeof(SQLWCHAR),&chunk_length_));
  EXPECT_EQ(static_cast<SQLLEN>(2*sizeof(SQLWCHAR)),chunk_length_); EXPECT_EQ('n',chunk_[0]); EXPECT_EQ('e',chunk_[1]); EXPECT_EQ(0,chunk_[2]); EXPECT_EQ(0x7777,chunk_[3]);
  ASSERT_EQ(SQL_NO_DATA,SQLGetData(hstmt_,2,SQL_C_WCHAR,chunk_,3*sizeof(SQLWCHAR),&chunk_length_));
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); chunk_length_=73;
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,2,SQL_C_WCHAR,chunk_,3*sizeof(SQLWCHAR),&chunk_length_)); EXPECT_EQ(SQL_NULL_DATA,chunk_length_);
  guards();
}

TEST_F(RedshiftExpandedBindingRealTest, ScalarOffsetProceedIgnoreEagerIpdNullRefusalAndRebindRecovery) {
  ready(); ASSERT_FALSE(HasFailure());
  SQLUINTEGER supported=99;
  ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_AUTO_IPD,&supported,0,nullptr)); ASSERT_EQ(SQL_TRUE,supported);
  SQLULEN enabled=99; ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_ENABLE_AUTO_IPD,&enabled,0,nullptr)); EXPECT_EQ(SQL_FALSE,enabled);
  EXPECT_EQ(SQL_ERROR,SQLSetStmtAttr(hstmt_,SQL_ATTR_ENABLE_AUTO_IPD,number(99),0));
  EXPECT_EQ("HY024",get_error(SQL_HANDLE_STMT,hstmt_));
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_ENABLE_AUTO_IPD,&enabled,0,nullptr)); EXPECT_EQ(SQL_FALSE,enabled);
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ENABLE_AUTO_IPD,number(SQL_TRUE),0));
  native_prepare(); ASSERT_FALSE(HasFailure());
  SQLHDESC apd{},ipd{}; SQLSMALLINT count=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_IMP_PARAM_DESC,&ipd,0,nullptr));
  // This nonmutating read precedes DescribeParam: lazy discovery cannot repair the eager-IPD oracle.
  ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ipd,0,SQL_DESC_COUNT,&count,0,nullptr)); EXPECT_EQ(1,count);
  SQLSMALLINT type=-1,scale=-1,nullable=-1; SQLULEN length=99;
  ASSERT_TRUE(charge()); ASSERT_TRUE(cap(window_end_,hstmt_));
  ASSERT_EQ(SQL_SUCCESS,SQLDescribeParam(hstmt_,1,&type,&length,&scale,&nullable));
  EXPECT_EQ(SQL_INTEGER,type); EXPECT_EQ(10u,length); EXPECT_EQ(0,scale); EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_APP_PARAM_DESC,&apd,0,nullptr));
  inputs_[1].value=42; offset_=sizeof(InputPage);
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMSET_SIZE,number(1),0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_BIND_TYPE,number(SQL_PARAM_BIND_BY_COLUMN),0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_BIND_OFFSET_PTR,&offset_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_OPERATION_PTR,&operation_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_STATUS_PTR,&parameter_status_,0));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&inputs_[0].value,0,&inputs_[0].length));
  native_execute(); ASSERT_FALSE(HasFailure()); inputs_[1].value=99;
  EXPECT_EQ(1u,processed_); EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status_);
  scalar(hstmt_,42); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  operation_=SQL_PARAM_IGNORE; offset_=static_cast<SQLULEN>((std::numeric_limits<SQLLEN>::max)())+1;
  processed_=91; parameter_status_=71;
  native_execute(); ASSERT_FALSE(HasFailure());
  EXPECT_EQ(0u,processed_); EXPECT_EQ(SQL_PARAM_UNUSED,parameter_status_);
  SQLLEN affected=-1; ASSERT_EQ(SQL_SUCCESS,SQLRowCount(hstmt_,&affected)); EXPECT_EQ(0,affected);
  EXPECT_EQ(SQL_ERROR,SQLFetch(hstmt_)); EXPECT_EQ("24000",get_error(SQL_HANDLE_STMT,hstmt_));
  operation_=SQL_PARAM_PROCEED; processed_=91; parameter_status_=71;
  ASSERT_TRUE(charge(2)); ASSERT_TRUE(cap(window_end_,hstmt_));
  EXPECT_EQ(SQL_ERROR,SQLExecute(hstmt_)); EXPECT_EQ("HYC00",get_error(SQL_HANDLE_STMT,hstmt_));
  EXPECT_EQ(91u,processed_); EXPECT_EQ(71,parameter_status_);
  offset_=sizeof(InputPage); inputs_[1].length=SQL_NULL_DATA;
  native_execute(); ASSERT_FALSE(HasFailure()); EXPECT_EQ(1u,processed_); EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status_);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); output_length_=73;
  ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&integer_.value,sizeof(integer_.value),&output_length_)); EXPECT_EQ(SQL_NULL_DATA,output_length_);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  inputs_[1].length=0; inputs_[1].value=17;
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&inputs_[0].value,0,&inputs_[0].length));
  native_execute(); ASSERT_FALSE(HasFailure()); scalar(hstmt_,17); ASSERT_FALSE(HasFailure());
  SQLPOINTER base{}; ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(apd,1,SQL_DESC_DATA_PTR,&base,0,nullptr)); EXPECT_EQ(&inputs_[0].value,base);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_)); ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_RESET_PARAMS));
  ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_ENABLE_AUTO_IPD,&enabled,0,nullptr)); EXPECT_EQ(SQL_TRUE,enabled);
  inputs_[1].value=18;
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&inputs_[0].value,0,&inputs_[0].length));
  native_execute(); ASSERT_FALSE(HasFailure());
  native_direct(second_,"SELECT CAST(9 AS INTEGER)"); ASSERT_FALSE(HasFailure());
  scalar(hstmt_,18); ASSERT_FALSE(HasFailure()); scalar(second_,9); ASSERT_FALSE(HasFailure());
  guards();
}

// Existing-object read-only catalog/bit scope. These markers never establish
// fixture visibility, binary identity, accounting or native launch authority.
class RedshiftCatalogBitBoundedRealTest : public RedshiftBufferedLifecycleRealTest {
protected:
  unsigned charges_{0};
  struct NameOutput { unsigned char before{0xa1}; SQLCHAR value[128]{}; unsigned char after{0xb2}; } name_output_;
  struct TextOutput { unsigned char before{0xc3}; char value[256]{}; unsigned char after{0xd4}; } text_output_;
  struct WideOutput { unsigned char before{0xe1}; SQLWCHAR value[128]{}; unsigned char after{0xf2}; } wide_output_;
  SQLLEN length_{73}; std::string last_text_; std::vector<SQLWCHAR> last_wide_;
  struct Descriptor { std::string name; SQLSMALLINT type; SQLULEN size; SQLSMALLINT scale; bool operator==(const Descriptor&)const=default; };
  bool charge(unsigned count=1) {
    if(count>24||charges_>24-count) { ADD_FAILURE() << "Catalog/bit finite SQL charge exceeded"; return false; }
    charges_+=count; return true;
  }
  void setup_handles() {
    window_end_=rs::util::Clock::now()+std::chrono::seconds{60}; admitted_=true;
    RedshiftRealTest::SetUp(); if(HasFatalFailure()) { return; }
    const auto settings=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(settings.contains("DATABASE")); ASSERT_TRUE(settings.contains("UID"));
    ASSERT_TRUE(settings.at("DATABASE")=="odbcpp_pilot"); ASSERT_TRUE(settings.at("UID")=="odbcpp_pilot_test");
    ASSERT_FALSE(settings.contains("DSN"));
  }
  void ready() {
    connect_bounded(); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&second_));
    ASSERT_TRUE(cap(window_end_,second_));
  }
  void native_direct(SQLHSTMT statement,const char* sql) {
    ASSERT_TRUE(charge()); direct(statement,sql);
  }
  void recovery() {
    reset(hstmt_); ASSERT_FALSE(HasFailure());
    native_direct(hstmt_,"SELECT CAST(1 AS INTEGER)"); ASSERT_FALSE(HasFailure());
    scalar(hstmt_,1); ASSERT_FALSE(HasFailure());
  }
  void metadata(std::span<const CatalogField> fields,std::vector<Descriptor>& owned) {
    SQLSMALLINT count=-1; ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));
    ASSERT_EQ(fields.size(),static_cast<std::size_t>(count)); owned.clear();
    for(std::size_t i=0;i<fields.size();++i) {
      std::fill(std::begin(name_output_.value),std::end(name_output_.value),SQLCHAR{0x5a});
      SQLSMALLINT size=-1,type=-1,scale=-1; SQLULEN width=99;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,static_cast<SQLUSMALLINT>(i+1),name_output_.value,
          static_cast<SQLSMALLINT>(sizeof(name_output_.value)),&size,&type,&width,&scale,nullptr));
      ASSERT_GE(size,0); ASSERT_LT(static_cast<std::size_t>(size)+1,sizeof(name_output_.value));
      std::string actual(reinterpret_cast<char*>(name_output_.value),static_cast<std::size_t>(size));
      std::transform(actual.begin(),actual.end(),actual.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});
      EXPECT_EQ(fields[i].name,actual); if(fields[i].type!=0) { EXPECT_EQ(fields[i].type,type); }
      EXPECT_EQ(0,name_output_.value[static_cast<std::size_t>(size)]);
      EXPECT_EQ(0x5a,name_output_.value[static_cast<std::size_t>(size)+1]);
      EXPECT_EQ(0xa1,name_output_.before); EXPECT_EQ(0xb2,name_output_.after);
      owned.push_back({std::move(actual),type,width,scale});
    }
  }
  void number(SQLUSMALLINT column,SQLINTEGER expected) {
    integer(hstmt_,column,expected); ASSERT_FALSE(HasFailure());
  }
  void value(SQLUSMALLINT column,std::string_view expected,const std::vector<SQLWCHAR>& literal,bool wide) {
    length_=73;
    if(wide) {
      std::fill(std::begin(wide_output_.value),std::end(wide_output_.value),SQLWCHAR{0x5a});
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_WCHAR,wide_output_.value,sizeof(wide_output_.value),&length_));
      ASSERT_EQ(static_cast<SQLLEN>(literal.size()*sizeof(SQLWCHAR)),length_);
      ASSERT_LT(literal.size()+1,std::size(wide_output_.value));
      EXPECT_TRUE(std::equal(literal.begin(),literal.end(),std::begin(wide_output_.value)));
      EXPECT_EQ(0,wide_output_.value[literal.size()]); EXPECT_EQ(0x5a,wide_output_.value[literal.size()+1]);
      EXPECT_EQ(0xe1,wide_output_.before); EXPECT_EQ(0xf2,wide_output_.after);
      last_wide_.assign(wide_output_.value,wide_output_.value+literal.size());
    } else {
      std::fill(std::begin(text_output_.value),std::end(text_output_.value),'!');
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,text_output_.value,sizeof(text_output_.value),&length_));
      ASSERT_EQ(static_cast<SQLLEN>(expected.size()),length_); ASSERT_LT(expected.size()+1,sizeof(text_output_.value));
      EXPECT_EQ(expected,std::string_view(text_output_.value,expected.size()));
      EXPECT_EQ(0,text_output_.value[expected.size()]); EXPECT_EQ('!',text_output_.value[expected.size()+1]);
      EXPECT_EQ(0xc3,text_output_.before); EXPECT_EQ(0xd4,text_output_.after);
      last_text_.assign(text_output_.value,expected.size());
    }
  }
  void null_value(SQLUSMALLINT column) {
    std::fill(std::begin(text_output_.value),std::end(text_output_.value),'!'); length_=73;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,text_output_.value,sizeof(text_output_.value),&length_));
    EXPECT_EQ(SQL_NULL_DATA,length_); EXPECT_TRUE(std::all_of(std::begin(text_output_.value),std::end(text_output_.value),[](char ch){return ch=='!';}));
    EXPECT_EQ(0xc3,text_output_.before); EXPECT_EQ(0xd4,text_output_.after);
  }
  void constraint(SQLUSMALLINT column,std::string& owned) {
    std::fill(std::begin(text_output_.value),std::end(text_output_.value),'!'); length_=73;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,column,SQL_C_CHAR,text_output_.value,sizeof(text_output_.value),&length_));
    ASSERT_GT(length_,0); ASSERT_LT(static_cast<std::size_t>(length_),sizeof(text_output_.value));
    EXPECT_EQ(0,text_output_.value[static_cast<std::size_t>(length_)]);
    EXPECT_EQ(0xc3,text_output_.before); EXPECT_EQ(0xd4,text_output_.after);
    const std::string actual(text_output_.value,static_cast<std::size_t>(length_));
    if(owned.empty()) { owned=actual; } else { EXPECT_EQ(owned,actual); }
  }
};

class RedshiftMetadataIdentifierNativeRealTest : public RedshiftCatalogBitBoundedRealTest {
protected:
  enum class Route { Tables,Columns,PrimaryKeys,ForeignKeys,Statistics,Procedures,ProcedureColumns,SpecialColumns };
  std::string catalog_{"odbcpp_pilot"},schema_,object_,member_,foreign_;
  std::vector<SQLWCHAR> wcatalog_,wschema_,wobject_,wmember_,wforeign_;
  std::vector<Descriptor> owned_descriptors_,comparison_descriptors_;
  std::string pk_name_,fk_name_,owned_table_; std::vector<SQLWCHAR> owned_wide_table_;
  static std::vector<SQLWCHAR> ascii(std::string_view value) {
    std::vector<SQLWCHAR> result; for(unsigned char ch:value) { result.push_back(static_cast<SQLWCHAR>(ch)); } result.push_back(0); return result;
  }
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_METADATA_ID_NATIVE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Identifier metadata native scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="metadata-id-existing-read-only-v1");
    const char* fixture=std::getenv("ODBCPP_REDSHIFT_METADATA_FIXTURE_REUSE_ADMISSION");
    ASSERT_NE(nullptr,fixture); ASSERT_TRUE(std::string_view(fixture)=="existing-quoted-key-and-absent-procedure-v1");
    setup_handles();
  }
  void mode(bool enabled) {
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_METADATA_ID,reinterpret_cast<SQLPOINTER>(std::uintptr_t(enabled?SQL_TRUE:SQL_FALSE)),0));
    SQLULEN actual=99; ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_METADATA_ID,&actual,0,nullptr)); EXPECT_EQ(enabled?SQL_TRUE:SQL_FALSE,actual);
  }
  void ascii_names(std::string schema,std::string object,std::string member="p_input",std::string foreign="m2_catalog_child_20261003_c01") {
    schema_=std::move(schema); object_=std::move(object); member_=std::move(member); foreign_=std::move(foreign);
    wcatalog_=ascii(catalog_); wschema_=ascii(schema_); wobject_=ascii(object_); wmember_=ascii(member_); wforeign_=ascii(foreign_);
  }
  void unicode_names(bool identifier,bool missing=false) {
    schema_=identifier?"odbcpp_u_\xc3\xa9\xe8\xa1\xa8":"odbcpp\\_u\\_\xc3\xa9\xe8\xa1\xa8";
    object_=identifier?"\"t\"\"\xc3\xa9%_\xe8\xa1\xa8\"":"t\"\xc3\xa9\\%\\_\xe8\xa1\xa8";
    member_=identifier?"\"v%_\xc3\xa9\xe8\xa1\xa8\"":"v\\%\\_\xc3\xa9\xe8\xa1\xa8";
    wcatalog_=ascii(catalog_);
    wschema_=identifier?std::vector<SQLWCHAR>{'o','d','b','c','p','p','_','u','_',0x00e9,0x8868,0}
        :std::vector<SQLWCHAR>{'o','d','b','c','p','p','\\','_','u','\\','_',0x00e9,0x8868,0};
    wobject_=identifier?std::vector<SQLWCHAR>{'"','t','"','"',0x00e9,'%','_',0x8868,'"',0}
        :std::vector<SQLWCHAR>{'t','"',0x00e9,'\\','%','\\','_',0x8868,0};
    wmember_=identifier?std::vector<SQLWCHAR>{'"','v','%','_',0x00e9,0x8868,'"',0}
        :std::vector<SQLWCHAR>{'v','\\','%','\\','_',0x00e9,0x8868,0};
    if(missing) { object_="odbcppabsentmetadataid20261010"; wobject_=ascii(object_); }
  }
  SQLRETURN request(Route route,bool wide,bool null_catalog=false,unsigned allowance=2) {
    const unsigned exchanges = allowance==2 &&
        (route==Route::Procedures || route==Route::ProcedureColumns) ? 3 : allowance;
    if(!charge(exchanges)||!cap(window_end_,hstmt_)) { return SQL_ERROR; }
    auto* c=null_catalog?nullptr:reinterpret_cast<SQLCHAR*>(catalog_.data());
    auto* s=reinterpret_cast<SQLCHAR*>(schema_.data()); auto* t=reinterpret_cast<SQLCHAR*>(object_.data());
    auto* m=reinterpret_cast<SQLCHAR*>(member_.data()); auto* f=reinterpret_cast<SQLCHAR*>(foreign_.data());
    auto* wc=null_catalog?nullptr:wcatalog_.data(); auto* ws=wschema_.data(); auto* wt=wobject_.data(); auto* wm=wmember_.data(); auto* wf=wforeign_.data();
    SQLCHAR types[]{'T','A','B','L','E',0}; SQLWCHAR wtypes[]{'T','A','B','L','E',0};
    switch(route) {
      case Route::Tables:return wide?SQLTablesW(hstmt_,wc,SQL_NTS,ws,SQL_NTS,wt,SQL_NTS,wtypes,SQL_NTS):SQLTables(hstmt_,c,SQL_NTS,s,SQL_NTS,t,SQL_NTS,types,SQL_NTS);
      case Route::Columns:return wide?SQLColumnsW(hstmt_,wc,SQL_NTS,ws,SQL_NTS,wt,SQL_NTS,wm,SQL_NTS):SQLColumns(hstmt_,c,SQL_NTS,s,SQL_NTS,t,SQL_NTS,m,SQL_NTS);
      case Route::PrimaryKeys:return wide?SQLPrimaryKeysW(hstmt_,wc,SQL_NTS,ws,SQL_NTS,wt,SQL_NTS):SQLPrimaryKeys(hstmt_,c,SQL_NTS,s,SQL_NTS,t,SQL_NTS);
      case Route::ForeignKeys:return wide?SQLForeignKeysW(hstmt_,wc,SQL_NTS,ws,SQL_NTS,wt,SQL_NTS,wcatalog_.data(),SQL_NTS,ws,SQL_NTS,wf,SQL_NTS):SQLForeignKeys(hstmt_,c,SQL_NTS,s,SQL_NTS,t,SQL_NTS,reinterpret_cast<SQLCHAR*>(catalog_.data()),SQL_NTS,s,SQL_NTS,f,SQL_NTS);
      case Route::Statistics:return wide?SQLStatisticsW(hstmt_,wc,SQL_NTS,ws,SQL_NTS,wt,SQL_NTS,SQL_INDEX_ALL,SQL_QUICK):SQLStatistics(hstmt_,c,SQL_NTS,s,SQL_NTS,t,SQL_NTS,SQL_INDEX_ALL,SQL_QUICK);
      case Route::Procedures:return wide?SQLProceduresW(hstmt_,wc,SQL_NTS,ws,SQL_NTS,wt,SQL_NTS):SQLProcedures(hstmt_,c,SQL_NTS,s,SQL_NTS,t,SQL_NTS);
      case Route::ProcedureColumns:return wide?SQLProcedureColumnsW(hstmt_,wc,SQL_NTS,ws,SQL_NTS,wt,SQL_NTS,wm,SQL_NTS):SQLProcedureColumns(hstmt_,c,SQL_NTS,s,SQL_NTS,t,SQL_NTS,m,SQL_NTS);
      case Route::SpecialColumns:return wide?SQLSpecialColumnsW(hstmt_,SQL_ROWVER,wc,SQL_NTS,ws,SQL_NTS,wt,SQL_NTS,SQL_SCOPE_CURROW,SQL_NULLABLE):SQLSpecialColumns(hstmt_,SQL_ROWVER,c,SQL_NTS,s,SQL_NTS,t,SQL_NTS,SQL_SCOPE_CURROW,SQL_NULLABLE);
    }
    ADD_FAILURE() << "Unknown fixed catalog route"; return SQL_ERROR;
  }
  static std::span<const CatalogField> fields(Route route) {
    static constexpr CatalogField tables[]{{"table_cat",SQL_VARCHAR},{"table_schem",SQL_VARCHAR},{"table_name",SQL_VARCHAR},{"table_type",SQL_VARCHAR},{"remarks",SQL_VARCHAR}};
    static constexpr CatalogField columns[]{{"table_cat",SQL_VARCHAR},{"table_schem",SQL_VARCHAR},{"table_name",SQL_VARCHAR},{"column_name",SQL_VARCHAR},{"data_type",SQL_SMALLINT},{"type_name",SQL_VARCHAR},{"column_size",SQL_INTEGER},{"buffer_length",SQL_INTEGER},{"decimal_digits",SQL_SMALLINT},{"num_prec_radix",SQL_SMALLINT},{"nullable",SQL_SMALLINT},{"remarks",SQL_VARCHAR},{"column_def",SQL_VARCHAR},{"sql_data_type",SQL_SMALLINT},{"sql_datetime_sub",SQL_SMALLINT},{"char_octet_length",SQL_INTEGER},{"ordinal_position",SQL_INTEGER},{"is_nullable",SQL_VARCHAR}};
    static constexpr CatalogField pk[]{{"table_cat",SQL_VARCHAR},{"table_schem",SQL_VARCHAR},{"table_name",SQL_VARCHAR},{"column_name",SQL_VARCHAR},{"key_seq",SQL_SMALLINT},{"pk_name",SQL_VARCHAR}};
    static constexpr CatalogField fk[]{{"pktable_cat",SQL_VARCHAR},{"pktable_schem",SQL_VARCHAR},{"pktable_name",SQL_VARCHAR},{"pkcolumn_name",SQL_VARCHAR},{"fktable_cat",SQL_VARCHAR},{"fktable_schem",SQL_VARCHAR},{"fktable_name",SQL_VARCHAR},{"fkcolumn_name",SQL_VARCHAR},{"key_seq",SQL_SMALLINT},{"update_rule",SQL_SMALLINT},{"delete_rule",SQL_SMALLINT},{"fk_name",SQL_VARCHAR},{"pk_name",SQL_VARCHAR},{"deferrability",SQL_SMALLINT}};
    static constexpr CatalogField statistics[]{{"table_cat",SQL_VARCHAR},{"table_schem",SQL_VARCHAR},{"table_name",SQL_VARCHAR},{"non_unique",SQL_SMALLINT},{"index_qualifier",SQL_VARCHAR},{"index_name",SQL_VARCHAR},{"type",SQL_SMALLINT},{"ordinal_position",SQL_SMALLINT},{"column_name",SQL_VARCHAR},{"asc_or_desc",SQL_VARCHAR},{"cardinality",SQL_INTEGER},{"pages",SQL_INTEGER},{"filter_condition",SQL_VARCHAR}};
    static constexpr CatalogField special[]{{"scope",SQL_SMALLINT},{"column_name",SQL_VARCHAR},{"data_type",SQL_SMALLINT},{"type_name",SQL_VARCHAR},{"column_size",SQL_INTEGER},{"buffer_length",SQL_INTEGER},{"decimal_digits",SQL_SMALLINT},{"pseudo_column",SQL_SMALLINT}};
    // Reserved procedure fields4-6 deliberately carry no invented type/value oracle.
    static constexpr CatalogField procedures[]{{"procedure_cat",SQL_VARCHAR},{"procedure_schem",SQL_VARCHAR},{"procedure_name",SQL_VARCHAR},{"num_input_params",0},{"num_output_params",0},{"num_result_sets",0},{"remarks",SQL_VARCHAR},{"procedure_type",SQL_SMALLINT}};
    static constexpr CatalogField procedure_columns[]{{"procedure_cat",SQL_VARCHAR},{"procedure_schem",SQL_VARCHAR},{"procedure_name",SQL_VARCHAR},{"column_name",SQL_VARCHAR},{"column_type",SQL_SMALLINT},{"data_type",SQL_SMALLINT},{"type_name",SQL_VARCHAR},{"column_size",SQL_INTEGER},{"buffer_length",SQL_INTEGER},{"decimal_digits",SQL_SMALLINT},{"num_prec_radix",SQL_SMALLINT},{"nullable",SQL_SMALLINT},{"remarks",SQL_VARCHAR},{"column_def",SQL_VARCHAR},{"sql_data_type",SQL_SMALLINT},{"sql_datetime_sub",SQL_SMALLINT},{"char_octet_length",SQL_INTEGER},{"ordinal_position",SQL_INTEGER},{"is_nullable",SQL_VARCHAR}};
    switch(route) {
      case Route::Tables:return tables;case Route::Columns:return columns;case Route::PrimaryKeys:return pk;case Route::ForeignKeys:return fk;
      case Route::Statistics:return statistics;case Route::SpecialColumns:return special;case Route::Procedures:return procedures;case Route::ProcedureColumns:return procedure_columns;
    }
    return {};
  }
  void close() { ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE)); }
  void expect_table(bool wide) {
    value(1,"odbcpp_pilot",{'o','d','b','c','p','p','_','p','i','l','o','t'},wide); ASSERT_FALSE(HasFailure());
    value(2,"odbcpp_u_\xc3\xa9\xe8\xa1\xa8",{'o','d','b','c','p','p','_','u','_',0x00e9,0x8868},wide); ASSERT_FALSE(HasFailure());
    value(3,"t\"\xc3\xa9%_\xe8\xa1\xa8",{'t','"',0x00e9,'%','_',0x8868},wide); ASSERT_FALSE(HasFailure());
    if(wide) { owned_wide_table_=last_wide_; } else { owned_table_=last_text_; }
    value(4,"TABLE",{'T','A','B','L','E'},wide); ASSERT_FALSE(HasFailure());
  }
  void expect_column(bool wide) {
    value(1,"odbcpp_pilot",{'o','d','b','c','p','p','_','p','i','l','o','t'},wide); ASSERT_FALSE(HasFailure());
    value(2,"odbcpp_u_\xc3\xa9\xe8\xa1\xa8",{'o','d','b','c','p','p','_','u','_',0x00e9,0x8868},wide); ASSERT_FALSE(HasFailure());
    value(3,"t\"\xc3\xa9%_\xe8\xa1\xa8",{'t','"',0x00e9,'%','_',0x8868},wide); ASSERT_FALSE(HasFailure());
    value(4,"v%_\xc3\xa9\xe8\xa1\xa8",{'v','%','_',0x00e9,0x8868},wide); ASSERT_FALSE(HasFailure());
    number(5,SQL_VARCHAR); number(7,32); number(8,32); null_value(9); null_value(10);
    number(11,SQL_NULLABLE); null_value(12); null_value(13); number(14,SQL_VARCHAR); null_value(15); number(16,32); number(17,2);
    value(18,"YES",{'Y','E','S'},wide);
  }
  void procedure_trial(Route route,bool wide,bool identifier) {
    ready(); ASSERT_FALSE(HasFailure()); ascii_names(identifier?"odbcpp_fixture":"odbcpp\\_fixture","odbcppabsentmetadataid20261010");
    mode(identifier); ASSERT_FALSE(HasFailure());
    // An actual server/query failure is a failure of this independent trial;
    // never substitute HYC00, fallback, a skip or future routine implementation.
    ASSERT_EQ(SQL_SUCCESS,request(route,wide)) << get_error(SQL_HANDLE_STMT,hstmt_);
    metadata(fields(route),owned_descriptors_); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
    const auto retained=owned_descriptors_; recovery(); ASSERT_FALSE(HasFailure()); EXPECT_EQ(retained,owned_descriptors_);
  }
};

TEST_F(RedshiftMetadataIdentifierNativeRealTest, QuotedExactVersusEscapedPatternTablesColumnsAndNoMatch) {
  ready(); ASSERT_FALSE(HasFailure());
  SQLULEN flag=99; ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_METADATA_ID,&flag,0,nullptr)); EXPECT_EQ(SQL_FALSE,flag);
  mode(false); ASSERT_FALSE(HasFailure()); unicode_names(false);
  ASSERT_EQ(SQL_SUCCESS,request(Route::Tables,false)) << get_error(SQL_HANDLE_STMT,hstmt_);
  metadata(fields(Route::Tables),owned_descriptors_); ASSERT_FALSE(HasFailure()); const auto tables_owned=owned_descriptors_;
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); expect_table(false); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,request(Route::Columns,false)) << get_error(SQL_HANDLE_STMT,hstmt_);
  metadata(fields(Route::Columns),owned_descriptors_); ASSERT_FALSE(HasFailure()); const auto columns_owned=owned_descriptors_;
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); expect_column(false); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
  mode(true); ASSERT_FALSE(HasFailure()); unicode_names(true);
  for(bool wide:{false,true}) {
    ASSERT_EQ(SQL_SUCCESS,request(Route::Tables,wide)) << get_error(SQL_HANDLE_STMT,hstmt_);
    metadata(fields(Route::Tables),owned_descriptors_); ASSERT_FALSE(HasFailure()); EXPECT_EQ(tables_owned,owned_descriptors_);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); expect_table(wide); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,request(Route::Columns,wide)) << get_error(SQL_HANDLE_STMT,hstmt_);
    metadata(fields(Route::Columns),owned_descriptors_); ASSERT_FALSE(HasFailure()); EXPECT_EQ(columns_owned,owned_descriptors_);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_)); expect_column(wide); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
  }
  unicode_names(true,true);
  for(bool wide:{false,true}) {
    ASSERT_EQ(SQL_SUCCESS,request(Route::Tables,wide)) << get_error(SQL_HANDLE_STMT,hstmt_);
    metadata(fields(Route::Tables),owned_descriptors_); ASSERT_FALSE(HasFailure()); EXPECT_EQ(tables_owned,owned_descriptors_);
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
  }
  recovery(); ASSERT_FALSE(HasFailure());
  EXPECT_EQ("t\"\xc3\xa9%_\xe8\xa1\xa8",owned_table_);
  EXPECT_EQ((std::vector<SQLWCHAR>{'t','"',0x00e9,'%','_',0x8868}),owned_wide_table_);
  EXPECT_EQ(5u,tables_owned.size()); EXPECT_EQ(18u,columns_owned.size());
}

TEST_F(RedshiftMetadataIdentifierNativeRealTest, AllEightRequiredNullAndMalformedQuoteKeepFlagsAndSibling) {
  ready(); ASSERT_FALSE(HasFailure()); mode(true); ASSERT_FALSE(HasFailure());
  native_direct(second_,"SELECT CAST(9 AS INTEGER)"); ASSERT_FALSE(HasFailure());
  constexpr Route routes[]{Route::Tables,Route::Columns,Route::PrimaryKeys,Route::ForeignKeys,
      Route::Statistics,Route::Procedures,Route::ProcedureColumns,Route::SpecialColumns};
  for(const auto route:routes) {
    SCOPED_TRACE(static_cast<int>(route)); ascii_names("odbcpp_fixture","m2_catalog_parent_20261003_c01");
    EXPECT_EQ(SQL_ERROR,request(route,false,true,1)); EXPECT_EQ("HY009",get_error(SQL_HANDLE_STMT,hstmt_));
    SQLULEN flag=99; ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_METADATA_ID,&flag,0,nullptr)); EXPECT_EQ(SQL_TRUE,flag);
    ascii_names("odbcpp_fixture","\"unfinished");
    EXPECT_EQ(SQL_ERROR,request(route,false,false,1)); EXPECT_EQ("HY000",get_error(SQL_HANDLE_STMT,hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_METADATA_ID,&flag,0,nullptr)); EXPECT_EQ(SQL_TRUE,flag);
    ASSERT_FALSE(HasFailure());
  }
  EXPECT_EQ(SQL_ERROR,SQLSetStmtAttr(hstmt_,SQL_ATTR_METADATA_ID,reinterpret_cast<SQLPOINTER>(std::uintptr_t{99}),0));
  EXPECT_EQ("HY024",get_error(SQL_HANDLE_STMT,hstmt_));
  SQLULEN flag=99; ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_METADATA_ID,&flag,0,nullptr)); EXPECT_EQ(SQL_TRUE,flag);
  scalar(second_,9); ASSERT_FALSE(HasFailure()); recovery();
}

TEST_F(RedshiftMetadataIdentifierNativeRealTest, ExactCompositeKeysAndDeclaredEmptyStatisticsRowVersion) {
  ready(); ASSERT_FALSE(HasFailure()); mode(true); ASSERT_FALSE(HasFailure());
  ascii_names("ODBCPP_FIXTURE","M2_CATALOG_PARENT_20261003_C01","key_b","M2_CATALOG_CHILD_20261003_C01");
  const auto literal=[](std::string_view text) { auto result=ascii(text); result.pop_back(); return result; };
  for(bool wide:{false,true}) {
    ASSERT_EQ(SQL_SUCCESS,request(Route::PrimaryKeys,wide)) << get_error(SQL_HANDLE_STMT,hstmt_);
    metadata(fields(Route::PrimaryKeys),owned_descriptors_); ASSERT_FALSE(HasFailure());
    if(wide) { EXPECT_EQ(comparison_descriptors_,owned_descriptors_); } else { comparison_descriptors_=owned_descriptors_; }
    for(SQLINTEGER sequence=1;sequence<=2;++sequence) {
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
      value(1,"odbcpp_pilot",literal("odbcpp_pilot"),wide); ASSERT_FALSE(HasFailure());
      value(2,"odbcpp_fixture",literal("odbcpp_fixture"),wide); ASSERT_FALSE(HasFailure());
      value(3,"m2_catalog_parent_20261003_c01",literal("m2_catalog_parent_20261003_c01"),wide); ASSERT_FALSE(HasFailure());
      const char* key=sequence==1?"key_b":"key_a";
      value(4,key,literal(key),wide); ASSERT_FALSE(HasFailure()); number(5,sequence); constraint(6,pk_name_); ASSERT_FALSE(HasFailure());
    }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
    ASSERT_EQ(SQL_SUCCESS,request(Route::ForeignKeys,wide)) << get_error(SQL_HANDLE_STMT,hstmt_);
    metadata(fields(Route::ForeignKeys),owned_descriptors_); ASSERT_FALSE(HasFailure());
    for(SQLINTEGER sequence=1;sequence<=2;++sequence) {
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
      value(1,"odbcpp_pilot",literal("odbcpp_pilot"),wide); ASSERT_FALSE(HasFailure());
      value(2,"odbcpp_fixture",literal("odbcpp_fixture"),wide); ASSERT_FALSE(HasFailure());
      value(3,"m2_catalog_parent_20261003_c01",literal("m2_catalog_parent_20261003_c01"),wide); ASSERT_FALSE(HasFailure());
      const char* key=sequence==1?"key_b":"key_a"; const char* ref=sequence==1?"ref_b":"ref_a";
      value(4,key,literal(key),wide); ASSERT_FALSE(HasFailure());
      value(5,"odbcpp_pilot",literal("odbcpp_pilot"),wide); ASSERT_FALSE(HasFailure());
      value(6,"odbcpp_fixture",literal("odbcpp_fixture"),wide); ASSERT_FALSE(HasFailure());
      value(7,"m2_catalog_child_20261003_c01",literal("m2_catalog_child_20261003_c01"),wide); ASSERT_FALSE(HasFailure());
      value(8,ref,literal(ref),wide); ASSERT_FALSE(HasFailure()); number(9,sequence);
      constraint(12,fk_name_); constraint(13,pk_name_); ASSERT_FALSE(HasFailure());
    }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
    for(const auto route:{Route::Statistics,Route::SpecialColumns}) {
      ASSERT_EQ(SQL_SUCCESS,request(route,wide)) << get_error(SQL_HANDLE_STMT,hstmt_);
      metadata(fields(route),owned_descriptors_); ASSERT_FALSE(HasFailure());
      ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); close(); ASSERT_FALSE(HasFailure());
    }
  }
  const auto owning_pk=pk_name_,owning_fk=fk_name_; recovery(); ASSERT_FALSE(HasFailure());
  EXPECT_FALSE(owning_pk.empty()); EXPECT_FALSE(owning_fk.empty()); EXPECT_EQ(owning_pk,pk_name_); EXPECT_EQ(owning_fk,fk_name_);
}

// Each procedure API/mode/encoding trial is its own observable test. A failure
// cannot be reclassified as an execution of any other trial or a native pass.
TEST_F(RedshiftMetadataIdentifierNativeRealTest, AbsentProceduresAnsiIdentifierStandardEmptyMetadata) {
  procedure_trial(Route::Procedures,false,true);
}
TEST_F(RedshiftMetadataIdentifierNativeRealTest, AbsentProceduresWideIdentifierStandardEmptyMetadata) {
  procedure_trial(Route::Procedures,true,true);
}
TEST_F(RedshiftMetadataIdentifierNativeRealTest, AbsentProceduresAnsiPatternStandardEmptyMetadata) {
  procedure_trial(Route::Procedures,false,false);
}
TEST_F(RedshiftMetadataIdentifierNativeRealTest, AbsentProcedureColumnsAnsiIdentifierStandardEmptyMetadata) {
  procedure_trial(Route::ProcedureColumns,false,true);
}
TEST_F(RedshiftMetadataIdentifierNativeRealTest, AbsentProcedureColumnsWideIdentifierStandardEmptyMetadata) {
  procedure_trial(Route::ProcedureColumns,true,true);
}
TEST_F(RedshiftMetadataIdentifierNativeRealTest, AbsentProcedureColumnsAnsiPatternStandardEmptyMetadata) {
  procedure_trial(Route::ProcedureColumns,false,false);
}

class RedshiftBitPrecisionNativeRealTest : public RedshiftCatalogBitBoundedRealTest {
protected:
  SQLCHAR input_[3]{0x5a,0,0xa5}; SQLLEN indicator_{1};
  struct Status { unsigned char before{0xa1}; SQLUSMALLINT value{71}; unsigned char after{0xb2}; } status_;
  struct Processed { unsigned char before{0xc3}; SQLULEN value{91}; unsigned char after{0xd4}; } processed_;
  struct Output { unsigned char before{0xe1}; SQLINTEGER value{-91}; unsigned char after{0xf2}; } output_;
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_BIT_PRECISION_NATIVE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "BIT numeric precision native scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="bit-numeric-precision-v1"); setup_handles();
  }
  void prepared(const char* sql) { ASSERT_TRUE(charge()); prepare(hstmt_,sql); }
  void bind(SQLSMALLINT type,SQLULEN precision,SQLSMALLINT scale) {
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_BIT,type,precision,scale,&input_[1],1,&indicator_));
  }
  void guards() {
    EXPECT_EQ(0x5a,input_[0]); EXPECT_EQ(0xa5,input_[2]);
    EXPECT_EQ(0xa1,status_.before); EXPECT_EQ(0xb2,status_.after);
    EXPECT_EQ(0xc3,processed_.before); EXPECT_EQ(0xd4,processed_.after);
    EXPECT_EQ(0xe1,output_.before); EXPECT_EQ(0xf2,output_.after);
  }
  void attempt(bool success,SQLINTEGER expected=0,bool nulls=false) {
    ASSERT_TRUE(charge(2)); ASSERT_TRUE(cap(window_end_,hstmt_));
    processed_.value=91; status_.value=71;
    const auto result=SQLExecute(hstmt_);
    if(!success) {
      ASSERT_EQ(SQL_ERROR,result); EXPECT_EQ("22003",get_error(SQL_HANDLE_STMT,hstmt_));
      EXPECT_EQ(1u,processed_.value); EXPECT_EQ(SQL_PARAM_ERROR,status_.value); guards();
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt_,SQL_CLOSE)); return;
    }
    ASSERT_EQ(SQL_SUCCESS,result) << get_error(SQL_HANDLE_STMT,hstmt_);
    EXPECT_EQ(1u,processed_.value); EXPECT_EQ(SQL_PARAM_SUCCESS,status_.value);
    // Installed input storage remains alive; change it before reading the owning result.
    input_[1]=static_cast<SQLCHAR>(input_[1]^1); output_.value=-91; length_=73;
    SQLSMALLINT count=-1; ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count)); EXPECT_EQ(1,count);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&output_.value,sizeof(output_.value),&length_));
    if(nulls) { EXPECT_EQ(SQL_NULL_DATA,length_); EXPECT_EQ(-91,output_.value); }
    else { EXPECT_EQ(expected,output_.value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),length_); }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); guards(); ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  }
};

TEST_F(RedshiftBitPrecisionNativeRealTest, FractionOnlyZeroNumericDecimalNullAndWholeDigitRebindRecovery) {
  ready(); ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed_.value,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_PARAM_STATUS_PTR,&status_.value,0));
  prepared("SELECT CAST(CAST(? AS DECIMAL(2,2)) AS INTEGER) AS n"); ASSERT_FALSE(HasFailure());
  input_[1]=0; bind(SQL_NUMERIC,2,2); ASSERT_FALSE(HasFailure()); attempt(true,0); ASSERT_FALSE(HasFailure());
  input_[1]=1; bind(SQL_NUMERIC,2,2); ASSERT_FALSE(HasFailure()); attempt(false); ASSERT_FALSE(HasFailure());
  input_[1]=0; bind(SQL_DECIMAL,2,2); ASSERT_FALSE(HasFailure()); attempt(true,0); ASSERT_FALSE(HasFailure());
  input_[1]=1; bind(SQL_DECIMAL,2,2); ASSERT_FALSE(HasFailure()); attempt(false); ASSERT_FALSE(HasFailure());
  input_[1]=255; indicator_=SQL_NULL_DATA; bind(SQL_DECIMAL,2,2); ASSERT_FALSE(HasFailure()); attempt(true,0,true); ASSERT_FALSE(HasFailure());
  prepared("SELECT CAST(CAST(? AS DECIMAL(2,1)) AS INTEGER) AS n"); ASSERT_FALSE(HasFailure()); indicator_=1;
  input_[1]=1; bind(SQL_NUMERIC,2,1); ASSERT_FALSE(HasFailure()); attempt(true,1); ASSERT_FALSE(HasFailure());
  input_[1]=1; bind(SQL_DECIMAL,2,1); ASSERT_FALSE(HasFailure()); attempt(true,1); ASSERT_FALSE(HasFailure());
  input_[1]=2; bind(SQL_NUMERIC,2,1); ASSERT_FALSE(HasFailure()); attempt(false); ASSERT_FALSE(HasFailure());
  input_[1]=0; bind(SQL_NUMERIC,2,1); ASSERT_FALSE(HasFailure()); attempt(true,0); ASSERT_FALSE(HasFailure());
  recovery(); guards();
}


// Selected temporal escapes must work against an ordinary table, rather than
// accidentally succeeding as a leader-only no-table expression. Execution still
// requires separate exact-session TEMP/setup/cleanup admission.
class RedshiftTemporalEscapeRealTest : public RedshiftRealTest {
 protected:
  rs::util::Deadline window_end_{};
  bool admitted_{};
  bool table_owned_{};
  bool unknown_completion_{};
  bool drop_attempted_{};
  SQLINTEGER id_{1};
  SQLLEN id_length_{};
  struct StampCell { std::array<unsigned char,8> before; SQL_TIMESTAMP_STRUCT value; std::array<unsigned char,8> after; } escaped_stamp_{},native_stamp_{};
  struct TimeCell { std::array<unsigned char,8> before; SQL_TIME_STRUCT value; std::array<unsigned char,8> after; } escaped_time_{},native_time_{};
  SQLLEN stamp_length_{97},native_stamp_length_{97},time_length_{97},native_time_length_{97};
  static constexpr const char* table_name_="odbcpp_temporal_escape_scope";

  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_TEMPORAL_ESCAPE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Temporal escape scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="temporal-escapes-v1");
    // FIRST gate, before clocks/settings/handle/provider actions.
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40}; admitted_=true;
    RedshiftRealTest::SetUp();
    if(HasFatalFailure()) { return; }
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));
    ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));
    ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));
    ASSERT_FALSE(options.contains("DSN"));
  }
  bool cap(rs::util::Deadline end,bool statement) {
    if(unknown_completion_) { return false; }
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(end-rs::util::Clock::now()).count();
    if(remaining<=0) { ADD_FAILURE() << "Original temporal escape window expired"; return false; }
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5));
    if(SQLSetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      unknown_completion_=true; return false;
    }
    if(statement&&SQLSetStmtAttr(hstmt_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) {
      unknown_completion_=true; return false;
    }
    return true;
  }
  void connect_bounded() {
    ASSERT_TRUE(cap(window_end_,false));
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();
    ASSERT_GT(remaining,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5))),0));
    if(!connect()) { unknown_completion_=true; FAIL() << "Temporal escape connection failed"; }
    ASSERT_TRUE(cap(window_end_,true));
  }
  bool known_sql(SQLRETURN result) {
    if(result==SQL_SUCCESS) { return true; }
    // Any non-successful SQL operation is conservatively uncertain here. Never
    // repair it with another query, DROP by intent or cleanup retry.
    unknown_completion_=true;
    ADD_FAILURE() << "Temporal escape SQL failed state=" << get_error(SQL_HANDLE_STMT,hstmt_);
    return false;
  }
  void direct(std::string_view text,bool wide=false) {
    ASSERT_TRUE(cap(window_end_,true));
    std::string sql{text}; SQLRETURN result;
    if(wide) {
      std::vector<SQLWCHAR> w; for(const char ch:sql) { w.push_back(static_cast<SQLWCHAR>(ch)); }
      w.push_back(0); result=SQLExecDirectW(hstmt_,w.data(),SQL_NTS);
    } else { result=SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS); }
    ASSERT_TRUE(known_sql(result));
  }
  void fixture() {
    ASSERT_NO_FATAL_FAILURE(connect_bounded());
    ASSERT_NO_FATAL_FAILURE(direct("CREATE TEMP TABLE odbcpp_temporal_escape_scope (id INTEGER)"));
    // Exact successful CREATE receipt on this connection, never name/intent.
    table_owned_=true;
    ASSERT_NO_FATAL_FAILURE(direct("INSERT INTO odbcpp_temporal_escape_scope VALUES (1)"));
  }
  static std::string projection(bool prepared) {
    return std::string("SELECT id, {fn NOW()} AS escaped_stamp, {fn CURTIME()} AS escaped_time, "
        "GETDATE() AS native_stamp, CAST(GETDATE() AS TIME) AS native_time FROM ")+table_name_+
        (prepared?" WHERE id=?":" WHERE id=1");
  }
  template<class Cell> void poison(Cell& value) {
    value.before.fill(0x5a);value.after.fill(0x5a);std::memset(&value.value,0x5a,sizeof(value.value));
  }
  template<class Cell> void guards(const Cell& value) {
    for(const auto byte:value.before) { EXPECT_EQ(0x5a,byte); }
    for(const auto byte:value.after) { EXPECT_EQ(0x5a,byte); }
  }
  void timestamp(SQLUSMALLINT column,StampCell& value,SQLLEN& length) {
    poison(value);length=97;
    const auto result=SQLGetData(hstmt_,column,SQL_C_TYPE_TIMESTAMP,&value.value,sizeof(value.value),&length);
    if(result!=SQL_SUCCESS) { unknown_completion_=true; }
    ASSERT_EQ(SQL_SUCCESS,result);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT)),length);
    EXPECT_LT(value.value.fraction,1000000000u);
    ASSERT_NO_FATAL_FAILURE(guards(value));
  }
  void time(SQLUSMALLINT column,TimeCell& value,SQLLEN& length) {
    poison(value);length=97;
    const auto expected=native_stamp_.value.fraction==0?SQL_SUCCESS:SQL_SUCCESS_WITH_INFO;
    const auto result=SQLGetData(hstmt_,column,SQL_C_TYPE_TIME,&value.value,sizeof(value.value),&length);
    if(result!=expected) { unknown_completion_=true; }
    ASSERT_EQ(expected,result);
    if(expected==SQL_SUCCESS_WITH_INFO) {
      // Read this conversion's record before the next API clears diagnostics.
      EXPECT_EQ("01S07",get_error(SQL_HANDLE_STMT,hstmt_));
      SQLCHAR state[6]{};
      EXPECT_EQ(SQL_NO_DATA,SQLGetDiagRec(SQL_HANDLE_STMT,hstmt_,2,state,nullptr,nullptr,0,nullptr));
    }
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIME_STRUCT)),length);
    EXPECT_EQ(native_stamp_.value.hour,value.value.hour);
    EXPECT_EQ(native_stamp_.value.minute,value.value.minute);
    EXPECT_EQ(native_stamp_.value.second,value.value.second);
    ASSERT_NO_FATAL_FAILURE(guards(value));
  }
  void row() {
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(5,count);
    for(SQLUSMALLINT column=2;column<=5;++column) {
      SQLSMALLINT type=-1;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,nullptr,0,nullptr,&type,nullptr,nullptr,nullptr));
      EXPECT_EQ((column==2||column==4)?SQL_TYPE_TIMESTAMP:SQL_TYPE_TIME,type);
    }
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    SQLINTEGER id=-1;SQLLEN length=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&id,sizeof(id),&length));
    EXPECT_EQ(1,id);EXPECT_EQ(static_cast<SQLLEN>(sizeof(id)),length);
    ASSERT_NO_FATAL_FAILURE(timestamp(2,escaped_stamp_,stamp_length_));
    ASSERT_NO_FATAL_FAILURE(timestamp(4,native_stamp_,native_stamp_length_));
    EXPECT_EQ(native_stamp_.value.year,escaped_stamp_.value.year);
    EXPECT_EQ(native_stamp_.value.month,escaped_stamp_.value.month);
    EXPECT_EQ(native_stamp_.value.day,escaped_stamp_.value.day);
    EXPECT_EQ(native_stamp_.value.hour,escaped_stamp_.value.hour);
    EXPECT_EQ(native_stamp_.value.minute,escaped_stamp_.value.minute);
    EXPECT_EQ(native_stamp_.value.second,escaped_stamp_.value.second);
    EXPECT_EQ(native_stamp_.value.fraction,escaped_stamp_.value.fraction);
    EXPECT_GE(native_stamp_.value.month,1);EXPECT_LE(native_stamp_.value.month,12);
    EXPECT_GE(native_stamp_.value.day,1);EXPECT_LE(native_stamp_.value.day,31);
    EXPECT_LE(native_stamp_.value.hour,23);EXPECT_LE(native_stamp_.value.minute,59);EXPECT_LE(native_stamp_.value.second,59);
    ASSERT_NO_FATAL_FAILURE(time(3,escaped_time_,time_length_));
    ASSERT_NO_FATAL_FAILURE(time(5,native_time_,native_time_length_));
    EXPECT_EQ(native_time_.value.hour,escaped_time_.value.hour);
    EXPECT_EQ(native_time_.value.minute,escaped_time_.value.minute);
    EXPECT_EQ(native_time_.value.second,escaped_time_.value.second);
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  }
  void prepared_execute() {
    ASSERT_TRUE(cap(window_end_,true));
    ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
  }
  void recovery() {
    ASSERT_NO_FATAL_FAILURE(direct("SELECT 1"));
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    SQLINTEGER value=-1;SQLLEN length=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt_,1,SQL_C_SLONG,&value,sizeof(value),&length));
    EXPECT_EQ(1,value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)),length);
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  }
  void TearDown() override {
    const auto entry=rs::util::Clock::now();
    if(admitted_) { EXPECT_TRUE(entry<window_end_); }
    const auto end=std::min(window_end_,entry+std::chrono::seconds{5});
    if(table_owned_&&!unknown_completion_&&connected_&&!drop_attempted_) {
      if(SQLFreeStmt(hstmt_,SQL_CLOSE)!=SQL_SUCCESS) { unknown_completion_=true; }
      if(!unknown_completion_&&cap(end,true)) {
        drop_attempted_=true;
        std::string sql="DROP TABLE odbcpp_temporal_escape_scope";
        if(known_sql(SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS))) { table_owned_=false; }
      }
    }
    // Unknown remote completion suppresses all later SQL, including DROP and
    // rollback. Session/handle destruction is retained, never a SQL retry.
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
    if(table_owned_||unknown_completion_) {
      ADD_FAILURE() << "Temporal escape cleanup requires exact-session parent verification";
    }
  }
};

TEST_F(RedshiftTemporalEscapeRealTest, DirectAnsiWideTableExpressionsMatchNativeTypedProjection) {
  ASSERT_NO_FATAL_FAILURE(fixture());
  ASSERT_NO_FATAL_FAILURE(direct(projection(false)));
  ASSERT_NO_FATAL_FAILURE(row());
  const auto owned=escaped_stamp_.value;
  ASSERT_NO_FATAL_FAILURE(direct(projection(false),true));
  ASSERT_NO_FATAL_FAILURE(row());
  EXPECT_GE(owned.month,1);EXPECT_LE(owned.month,12);
  ASSERT_NO_FATAL_FAILURE(recovery());
}

TEST_F(RedshiftTemporalEscapeRealTest, PreparedTableExpressionsNullRebindAndEarlyCloseRecover) {
  ASSERT_NO_FATAL_FAILURE(fixture());
  auto sql=projection(true);std::vector<SQLWCHAR> w;
  for(const char ch:sql) { w.push_back(static_cast<SQLWCHAR>(ch)); }
  w.push_back(0);
  ASSERT_TRUE(cap(window_end_,true));
  ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_,w.data(),SQL_NTS)));
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&id_,0,&id_length_));
  ASSERT_NO_FATAL_FAILURE(prepared_execute());
  ASSERT_NO_FATAL_FAILURE(row());
  id_length_=SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(prepared_execute());
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  id_length_=0;
  ASSERT_NO_FATAL_FAILURE(prepared_execute());
  // Close one owning result before fetching, then independently recover.
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  ASSERT_NO_FATAL_FAILURE(recovery());
}


#include <optional>

// Ordinary table proof for the selected two-argument escape. Native NULL/empty
// behavior is delegated, never fabricated by local translation/conversion.
class RedshiftLocateEscapeRealTest : public RedshiftTemporalEscapeRealTest {
 protected:
  struct IntegerCell { std::array<unsigned char,8> before;SQLINTEGER value;std::array<unsigned char,8> after; } escaped_{},native_{},ordinal_{};
  SQLLEN escaped_length_{97},native_length_{97},ordinal_length_{97};
  std::array<char,16> needle_{};
  std::array<char,32> hay_{};
  SQLLEN needle_length_{SQL_NTS},hay_length_{SQL_NTS};
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_LOCATE_ESCAPE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "LOCATE escape scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="locate-escapes-v1");
    // This suite has its own FIRST gate, not the temporal suite's admission.
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();
    if(HasFatalFailure()) { return; }
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));
    ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));
    ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));
    ASSERT_FALSE(options.contains("DSN"));
  }
  void locate_fixture() {
    ASSERT_NO_FATAL_FAILURE(connect_bounded());
    ASSERT_NO_FATAL_FAILURE(direct("CREATE TEMP TABLE odbcpp_locate_escape_scope (id INTEGER, hay VARCHAR(32))"));
    table_owned_=true; // Exact successful CREATE on this session, not intent/name.
    ASSERT_NO_FATAL_FAILURE(direct("INSERT INTO odbcpp_locate_escape_scope VALUES "
        "(1,'\xC3\xA9\xE8\xA1\xA8" "fish'),(2,'plain'),(3,NULL)"));
  }
  void metadata(SQLSMALLINT expected) {
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&count));ASSERT_EQ(expected,count);
    for(SQLUSMALLINT column=1;column<=static_cast<SQLUSMALLINT>(expected);++column) {
      SQLSMALLINT type=-1;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,column,nullptr,0,nullptr,&type,nullptr,nullptr,nullptr));
      EXPECT_EQ(SQL_INTEGER,type);
    }
  }
  void integer(SQLUSMALLINT column,IntegerCell& value,SQLLEN& length) {
    poison(value);length=97;
    const auto result=SQLGetData(hstmt_,column,SQL_C_SLONG,&value.value,sizeof(value.value),&length);
    if(result!=SQL_SUCCESS) { unknown_completion_=true; }
    ASSERT_EQ(SQL_SUCCESS,result);
    if(length==SQL_NULL_DATA) {
      const auto* bytes=reinterpret_cast<const unsigned char*>(&value.value);
      EXPECT_TRUE(std::all_of(bytes,bytes+sizeof(value.value),[](unsigned char b){return b==0x5a;}));
    } else { EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),length); }
    ASSERT_NO_FATAL_FAILURE(guards(value));
  }
  void pair(SQLUSMALLINT escaped_column,SQLUSMALLINT native_column,std::optional<SQLINTEGER> literal) {
    ASSERT_NO_FATAL_FAILURE(integer(native_column,native_,native_length_));
    ASSERT_NO_FATAL_FAILURE(integer(escaped_column,escaped_,escaped_length_));
    EXPECT_EQ(native_length_,escaped_length_);
    if(native_length_!=SQL_NULL_DATA) { EXPECT_EQ(native_.value,escaped_.value); }
    if(literal) {
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),native_length_);
      EXPECT_EQ(*literal,native_.value);EXPECT_EQ(*literal,escaped_.value);
    }
    // Native NULL/empty outcome is observed by equal indicators/value. No
    // invented empty->1 or unconditional native NULL propagation assertion.
  }
  void direct_rows() {
    ASSERT_NO_FATAL_FAILURE(metadata(3));
    for(SQLINTEGER id=1;id<=3;++id) {
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
      ASSERT_NO_FATAL_FAILURE(integer(1,ordinal_,ordinal_length_));
      EXPECT_EQ(id,ordinal_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),ordinal_length_);
      const auto expected=id==1?std::optional<SQLINTEGER>{3}:id==2?std::optional<SQLINTEGER>{0}:std::nullopt;
      ASSERT_NO_FATAL_FAILURE(pair(2,3,expected));
    }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_));
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  }
  void prepare_pair() {
    auto text=std::string("SELECT {fn LOCATE(?,?)}, POSITION(? IN ?) FROM odbcpp_locate_escape_scope WHERE id=1");
    std::vector<SQLWCHAR> wide;
    for(const auto ch:text) { wide.push_back(static_cast<SQLWCHAR>(ch)); }
    wide.push_back(0);ASSERT_TRUE(cap(window_end_,true));
    ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_,wide.data(),SQL_NTS)));
    for(SQLUSMALLINT p=1;p<=4;++p) {
      const bool needle=p==1||p==3;
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,p,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,
          needle?16:32,0,needle?needle_.data():hay_.data(),
          static_cast<SQLLEN>(needle?needle_.size():hay_.size()),needle?&needle_length_:&hay_length_));
    }
  }
  void prepared_pair(std::optional<SQLINTEGER> expected,bool early_close=false) {
    ASSERT_NO_FATAL_FAILURE(prepared_execute());
    ASSERT_NO_FATAL_FAILURE(metadata(2));
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt_));
    ASSERT_NO_FATAL_FAILURE(pair(1,2,expected));
    if(!early_close) { ASSERT_EQ(SQL_NO_DATA,SQLFetch(hstmt_)); }
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt_));
  }
  void TearDown() override {
    const auto entry=rs::util::Clock::now();if(admitted_) { EXPECT_TRUE(entry<window_end_); }
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    if(table_owned_&&!unknown_completion_&&connected_&&!drop_attempted_) {
      if(SQLFreeStmt(hstmt_,SQL_CLOSE)!=SQL_SUCCESS) { unknown_completion_=true; }
      if(!unknown_completion_&&cap(cleanup_end,true)) {
        drop_attempted_=true;
        std::string text="DROP TABLE odbcpp_locate_escape_scope";
        if(known_sql(SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(text.data()),SQL_NTS))) { table_owned_=false; }
      }
    }
    // Not the temporal suite's fixed DROP: exact selected owned TEMP only.
    // Unknown completion suppresses every SQL/retry; session release is retained.
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
    if(table_owned_||unknown_completion_) { ADD_FAILURE() << "LOCATE exact-session parent cleanup verification required"; }
  }
};
TEST_F(RedshiftLocateEscapeRealTest, DirectAnsiWideTableSearchCharacterPositionsNullAndRecovery) {
  ASSERT_NO_FATAL_FAILURE(locate_fixture());
  const auto query="SELECT id,{fn LOCATE('fish',hay)},POSITION('fish' IN hay) FROM odbcpp_locate_escape_scope ORDER BY id";
  ASSERT_NO_FATAL_FAILURE(direct(query));
  ASSERT_NO_FATAL_FAILURE(direct_rows());
  ASSERT_NO_FATAL_FAILURE(direct(query,true));
  ASSERT_NO_FATAL_FAILURE(direct_rows());
  ASSERT_NO_FATAL_FAILURE(recovery());
}
TEST_F(RedshiftLocateEscapeRealTest, PreparedOriginalParameterOrderEmptyNullRebindAndEarlyCloseRecover) {
  ASSERT_NO_FATAL_FAILURE(locate_fixture());
  std::memcpy(needle_.data(),"fish",5);std::memcpy(hay_.data(),"\xC3\xA9\xE8\xA1\xA8" "fish",10);
  ASSERT_NO_FATAL_FAILURE(prepare_pair());
  ASSERT_NO_FATAL_FAILURE(prepared_pair(SQLINTEGER{3}));
  needle_.fill(0);std::memcpy(needle_.data(),"absent",7);
  ASSERT_NO_FATAL_FAILURE(prepared_pair(SQLINTEGER{0}));
  needle_length_=SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(prepared_pair(std::nullopt));
  needle_.fill(0);needle_length_=0;
  ASSERT_NO_FATAL_FAILURE(prepared_pair(std::nullopt,true));
  const auto owning_native_length=native_length_;const auto owning_native_value=native_.value;
  ASSERT_NO_FATAL_FAILURE(recovery());
  EXPECT_EQ(owning_native_length,native_length_);EXPECT_EQ(owning_native_value,native_.value);
  ASSERT_NO_FATAL_FAILURE(guards(native_));
  ASSERT_NO_FATAL_FAILURE(guards(escaped_));
}


// Selected string LENGTH scope. NULL/empty/all-space compare native composition,
// not invented result literals; no full coercion or native execution claim.
class RedshiftLengthEscapeRealTest : public RedshiftLocateEscapeRealTest {
 protected:
  IntegerCell raw_{};SQLLEN raw_length_{97};
  std::array<char,32> input_{};SQLLEN input_length_{SQL_NTS};
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_LENGTH_ESCAPE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "LENGTH escape scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="length-escapes-v1");
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();
    if(HasFatalFailure()) { return; }
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));ASSERT_FALSE(options.contains("DSN"));
  }
  void length_fixture() {
    ASSERT_NO_FATAL_FAILURE(connect_bounded());
    ASSERT_NO_FATAL_FAILURE(direct("CREATE TEMP TABLE odbcpp_length_escape_scope (id INTEGER,c CHAR(6),v VARCHAR(32))"));
    table_owned_=true; // Exact confirmed SQL_SUCCESS CREATE on this session only.
    ASSERT_NO_FATAL_FAILURE(direct("INSERT INTO odbcpp_length_escape_scope VALUES "
        "(1,'cat','cat   '),(2,'cat',' \xC3\xA9\xE8\xA1\xA8  '),(3,'cat','a\t '),(4,'   ','   '),(5,'',''),(6,NULL,NULL)"));
  }
  void length_fetch(SQLRETURN expected) {
    ASSERT_TRUE(cap(window_end_,true));
    const auto result=SQLFetch(hstmt_);
    if(result!=expected) { unknown_completion_=true; }
    ASSERT_EQ(expected,result);
  }
  void length_close() {
    ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  }
  void length_rows() {
    ASSERT_NO_FATAL_FAILURE(metadata(8));
    for(SQLINTEGER id=1;id<=6;++id) {
      ASSERT_NO_FATAL_FAILURE(length_fetch(SQL_SUCCESS));
      ASSERT_NO_FATAL_FAILURE(integer(1,ordinal_,ordinal_length_));EXPECT_EQ(id,ordinal_.value);
      const auto value=id==1||id==2?std::optional<SQLINTEGER>{3}:id==3?std::optional<SQLINTEGER>{2}:std::nullopt;
      ASSERT_NO_FATAL_FAILURE(pair(2,3,value));
      ASSERT_NO_FATAL_FAILURE(integer(4,raw_,raw_length_));
      if(id<=3) { EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),raw_length_);EXPECT_EQ(id==1?6:id==2?5:3,raw_.value); }
      ASSERT_NO_FATAL_FAILURE(pair(5,6,id<=3?std::optional<SQLINTEGER>{3}:std::nullopt));
      ASSERT_NO_FATAL_FAILURE(integer(7,raw_,raw_length_));
      if(id<=3) { EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),raw_length_);EXPECT_EQ(3,raw_.value); }
      ASSERT_NO_FATAL_FAILURE(integer(8,raw_,raw_length_));
      if(id==3) {
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),raw_length_);
        EXPECT_EQ(9,raw_.value); // Prove the stored second character is TAB.
      }
    }
    ASSERT_NO_FATAL_FAILURE(length_fetch(SQL_NO_DATA));
    ASSERT_NO_FATAL_FAILURE(length_close());
  }
  void prepare_length() {
    std::string text="SELECT {fn LENGTH(?)},LENGTH(RTRIM(?, ' ')),LENGTH(?) FROM odbcpp_length_escape_scope WHERE id=1";
    std::vector<SQLWCHAR> wide;
    for(const auto ch:text) { wide.push_back(static_cast<SQLWCHAR>(ch)); }
    wide.push_back(0);ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_,wide.data(),SQL_NTS)));
    for(SQLUSMALLINT p=1;p<=3;++p) {
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,p,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,32,0,input_.data(),input_.size(),&input_length_));
    }
  }
  void length_execution(std::optional<SQLINTEGER> expected,std::optional<SQLINTEGER> raw,bool early=false) {
    const auto owning_input=input_;const auto owning_length=input_length_;
    ASSERT_NO_FATAL_FAILURE(prepared_execute());
    ASSERT_NO_FATAL_FAILURE(metadata(3));
    ASSERT_NO_FATAL_FAILURE(length_fetch(SQL_SUCCESS));
    ASSERT_NO_FATAL_FAILURE(pair(1,2,expected));
    ASSERT_NO_FATAL_FAILURE(integer(3,raw_,raw_length_));
    if(raw) { EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),raw_length_);EXPECT_EQ(*raw,raw_.value); }
    EXPECT_EQ(owning_input,input_);EXPECT_EQ(owning_length,input_length_);
    if(!early) { ASSERT_NO_FATAL_FAILURE(length_fetch(SQL_NO_DATA)); }
    ASSERT_NO_FATAL_FAILURE(length_close());
  }

  void TearDown() override {
    const auto entry=rs::util::Clock::now();if(admitted_) { EXPECT_TRUE(entry<window_end_); }
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    if(table_owned_&&!unknown_completion_&&connected_&&!drop_attempted_) {
      if(SQLFreeStmt(hstmt_,SQL_CLOSE)!=SQL_SUCCESS) { unknown_completion_=true; }
      if(!unknown_completion_&&cap(cleanup_end,true)) {
        drop_attempted_=true;
        std::string text="DROP TABLE odbcpp_length_escape_scope";
        if(known_sql(SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(text.data()),SQL_NTS))) { table_owned_=false; }
      }
    }
    // Not the temporal suite's fixed DROP: exact selected owned TEMP only.
    // Unknown completion suppresses every SQL/retry; session release is retained.
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
    if(table_owned_||unknown_completion_) { ADD_FAILURE() << "LENGTH exact-session parent cleanup verification required"; }
  }
};

TEST_F(RedshiftLengthEscapeRealTest, DirectAnsiWidePaddedCharacterLengthsAndNativeEdgesRecover) {
  ASSERT_NO_FATAL_FAILURE(length_fixture());
  const auto query="SELECT id,{fn LENGTH(v)},LENGTH(RTRIM(v, ' ')),LENGTH(v),{fn LENGTH(c)},LENGTH(RTRIM(c, ' ')),LENGTH(c),ASCII(SUBSTRING(v,2,1)) FROM odbcpp_length_escape_scope ORDER BY id";
  ASSERT_NO_FATAL_FAILURE(direct(query));
  ASSERT_NO_FATAL_FAILURE(length_rows());
  ASSERT_NO_FATAL_FAILURE(direct(query,true));
  ASSERT_NO_FATAL_FAILURE(length_rows());
  ASSERT_NO_FATAL_FAILURE(recovery());
}
TEST_F(RedshiftLengthEscapeRealTest, PreparedPaddedUnicodeNullEmptyRebindAndEarlyCloseRecover) {
  ASSERT_NO_FATAL_FAILURE(length_fixture());
  std::memcpy(input_.data(),"cat   ",7);
  ASSERT_NO_FATAL_FAILURE(prepare_length());
  ASSERT_NO_FATAL_FAILURE(length_execution(SQLINTEGER{3},SQLINTEGER{6}));
  input_.fill(0);std::memcpy(input_.data()," \xC3\xA9\xE8\xA1\xA8  ",9);
  ASSERT_NO_FATAL_FAILURE(length_execution(SQLINTEGER{3},SQLINTEGER{5}));
  input_.fill(0);std::memcpy(input_.data(),"a\t ",4);
  ASSERT_NO_FATAL_FAILURE(length_execution(SQLINTEGER{2},SQLINTEGER{3}));
  input_length_=SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(length_execution(std::nullopt,std::nullopt));
  input_.fill(0);input_length_=0;
  ASSERT_NO_FATAL_FAILURE(length_execution(std::nullopt,std::nullopt));
  std::memcpy(input_.data(),"   ",4);input_length_=SQL_NTS;
  ASSERT_NO_FATAL_FAILURE(length_execution(std::nullopt,std::nullopt,true));
  const auto snapshot=native_;const auto snapshot_length=native_length_;
  ASSERT_NO_FATAL_FAILURE(recovery());
  EXPECT_EQ(snapshot_length,native_length_);EXPECT_EQ(snapshot.value,native_.value);
  ASSERT_NO_FATAL_FAILURE(guards(native_));
  ASSERT_NO_FATAL_FAILURE(guards(escaped_));
}


class RedshiftCalendarEscapeRealTest : public RedshiftLocateEscapeRealTest {
 protected:
  SQL_DATE_STRUCT date_{2024,2,29};SQLLEN date_length_{};
  std::array<SQLINTEGER,3> snapshot_{};
  static constexpr std::size_t fixture_columns_=13,fixture_rows_=4,fixture_cells_=52;
  static_assert(fixture_columns_*fixture_rows_==fixture_cells_);
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_CALENDAR_ESCAPE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Calendar escape scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="calendar-escapes-v1");
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();if(HasFatalFailure()) { return; }
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));ASSERT_FALSE(options.contains("DSN"));
    // Reject a configured insufficient bound before connect; never silently
    // clip the thirteen-column/four-row/fifty-two-cell direct projection.
    const rs::core::database::ResultLimits defaults{};
    const auto limit=[&](const char* key,std::size_t fallback)->std::optional<std::size_t> {
      const auto found=options.find(key);if(found==options.end()) { return fallback; }
      std::size_t value{};const auto& text=found->second;
      const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
      if(result.ec!=std::errc{}||result.ptr!=text.data()+text.size()) { return std::nullopt; }
      return value;
    };
    const auto columns=limit("MAXCOLUMNS",defaults.max_columns_per_description);
    const auto rows=limit("MAXROWS",defaults.max_rows);
    const auto cells=limit("MAXCELLS",defaults.max_cells);
    ASSERT_TRUE(columns);ASSERT_TRUE(rows);ASSERT_TRUE(cells);
    ASSERT_GE(*columns,fixture_columns_);ASSERT_GE(*rows,fixture_rows_);ASSERT_GE(*cells,fixture_cells_);
  }
  void calendar_fixture() {
    ASSERT_NO_FATAL_FAILURE(connect_bounded());
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,nullptr,0));
    SQLULEN visible=97;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,&visible,0,nullptr));ASSERT_EQ(static_cast<SQLULEN>(0),visible);
    ASSERT_NO_FATAL_FAILURE(direct("CREATE TEMP TABLE odbcpp_calendar_escape_scope (id INTEGER,d DATE,t TIMESTAMP)"));
    table_owned_=true; // Confirmed CREATE success on exact session only.
    ASSERT_NO_FATAL_FAILURE(direct("INSERT INTO odbcpp_calendar_escape_scope VALUES "
        "(1,DATE '2024-02-29',TIMESTAMP '2024-02-29 23:59:58.123456'),"
        "(2,DATE '2023-12-31',TIMESTAMP '2023-12-31 00:00:01.000001'),"
        "(3,DATE '2000-01-01',TIMESTAMP '2000-01-01 12:00:00.654321'),(4,NULL,NULL)"));
  }
  void calendar_fetch(SQLRETURN expected) {
    ASSERT_TRUE(cap(window_end_,true));const auto result=SQLFetch(hstmt_);
    if(result!=expected) { unknown_completion_=true; }
    ASSERT_EQ(expected,result);
  }
  void calendar_close() { ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_))); }
  void calendar_rows() {
    ASSERT_NO_FATAL_FAILURE(metadata(13));
    const std::array<std::array<SQLINTEGER,3>,3> literal{{{{2024,2,29}},{{2023,12,31}},{{2000,1,1}}}};
    for(SQLINTEGER id=1;id<=4;++id) {
      ASSERT_NO_FATAL_FAILURE(calendar_fetch(SQL_SUCCESS));
      ASSERT_NO_FATAL_FAILURE(integer(1,ordinal_,ordinal_length_));EXPECT_EQ(id,ordinal_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),ordinal_length_);
      for(SQLUSMALLINT group=0;group<2;++group) {
        for(SQLUSMALLINT part=0;part<3;++part) {
          const auto expected=id<=3?std::optional<SQLINTEGER>{literal[static_cast<std::size_t>(id-1)][part]}:std::nullopt;
          const auto escaped=static_cast<SQLUSMALLINT>(2+group*6+part*2);
          ASSERT_NO_FATAL_FAILURE(pair(escaped,static_cast<SQLUSMALLINT>(escaped+1),expected));
        }
      }
    }
    ASSERT_NO_FATAL_FAILURE(calendar_fetch(SQL_NO_DATA));
    ASSERT_NO_FATAL_FAILURE(calendar_close());
  }
  void prepare_calendar() {
    const std::string text="SELECT {fn YEAR(?)},{fn MONTH(?)},{fn DAYOFMONTH(?)},"
        "CAST(DATE_PART(year,?) AS INTEGER),CAST(DATE_PART(month,?) AS INTEGER),CAST(DATE_PART(day,?) AS INTEGER) "
        "FROM odbcpp_calendar_escape_scope WHERE id=1";
    std::vector<SQLWCHAR> wide;
    for(const auto ch:text) { wide.push_back(static_cast<SQLWCHAR>(ch)); }
    wide.push_back(0);ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_,wide.data(),SQL_NTS)));
    for(SQLUSMALLINT p=1;p<=6;++p) {
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,p,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,&date_,sizeof(date_),&date_length_));
    }
  }
  void calendar_execution(std::optional<std::array<SQLINTEGER,3>> expected,bool early=false) {
    const auto owning_date=date_;const auto owning_length=date_length_;
    ASSERT_NO_FATAL_FAILURE(prepared_execute());
    ASSERT_NO_FATAL_FAILURE(metadata(6));
    ASSERT_NO_FATAL_FAILURE(calendar_fetch(SQL_SUCCESS));
    for(SQLUSMALLINT part=0;part<3;++part) {
      ASSERT_NO_FATAL_FAILURE(pair(static_cast<SQLUSMALLINT>(part+1),static_cast<SQLUSMALLINT>(part+4),
          expected?std::optional<SQLINTEGER>{(*expected)[part]}:std::nullopt));
      if(native_length_!=SQL_NULL_DATA) { snapshot_[part]=native_.value; }
    }
    EXPECT_EQ(owning_date.year,date_.year);EXPECT_EQ(owning_date.month,date_.month);EXPECT_EQ(owning_date.day,date_.day);EXPECT_EQ(owning_length,date_length_);
    if(!early) {
      ASSERT_NO_FATAL_FAILURE(calendar_fetch(SQL_NO_DATA));
    }
    ASSERT_NO_FATAL_FAILURE(calendar_close());
  }

  void TearDown() override {
    const auto entry=rs::util::Clock::now();if(admitted_) { EXPECT_TRUE(entry<window_end_); }
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    if(table_owned_&&!unknown_completion_&&connected_&&!drop_attempted_) {
      if(SQLFreeStmt(hstmt_,SQL_CLOSE)!=SQL_SUCCESS) { unknown_completion_=true; }
      if(!unknown_completion_&&cap(cleanup_end,true)) {
        drop_attempted_=true;
        std::string text="DROP TABLE odbcpp_calendar_escape_scope";
        if(known_sql(SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(text.data()),SQL_NTS))) { table_owned_=false; }
      }
    }
    // Not the temporal suite's fixed DROP: exact selected owned TEMP only.
    // Unknown completion suppresses every SQL/retry; session release is retained.
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
    if(table_owned_||unknown_completion_) { ADD_FAILURE() << "Calendar exact-session parent cleanup verification required"; }
  }
};

TEST_F(RedshiftCalendarEscapeRealTest, DirectAnsiWideDateTimestampCalendarFieldsAndNullRecover) {
  ASSERT_NO_FATAL_FAILURE(calendar_fixture());
  const auto query="SELECT id,"
      "{fn YEAR(d)},CAST(DATE_PART(year,d) AS INTEGER),{fn MONTH(d)},CAST(DATE_PART(month,d) AS INTEGER),{fn DAYOFMONTH(d)},CAST(DATE_PART(day,d) AS INTEGER),"
      "{fn YEAR(t)},CAST(DATE_PART(year,t) AS INTEGER),{fn MONTH(t)},CAST(DATE_PART(month,t) AS INTEGER),{fn DAYOFMONTH(t)},CAST(DATE_PART(day,t) AS INTEGER) "
      "FROM odbcpp_calendar_escape_scope ORDER BY id";
  ASSERT_NO_FATAL_FAILURE(direct(query));
  ASSERT_NO_FATAL_FAILURE(calendar_rows());
  ASSERT_NO_FATAL_FAILURE(direct(query,true));
  ASSERT_NO_FATAL_FAILURE(calendar_rows());
  ASSERT_NO_FATAL_FAILURE(recovery());
}
TEST_F(RedshiftCalendarEscapeRealTest, PreparedDateLeapBoundaryNullRebindAndEarlyCloseRecover) {
  ASSERT_NO_FATAL_FAILURE(calendar_fixture());
  ASSERT_NO_FATAL_FAILURE(prepare_calendar());
  ASSERT_NO_FATAL_FAILURE((calendar_execution(std::array<SQLINTEGER,3>{2024,2,29})));
  date_={2023,12,31};
  ASSERT_NO_FATAL_FAILURE((calendar_execution(std::array<SQLINTEGER,3>{2023,12,31})));
  date_length_=SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(calendar_execution(std::nullopt));
  date_={2000,1,1};date_length_=0;
  ASSERT_NO_FATAL_FAILURE((calendar_execution(std::array<SQLINTEGER,3>{2000,1,1})));
  date_={2024,2,29};
  ASSERT_NO_FATAL_FAILURE((calendar_execution(std::array<SQLINTEGER,3>{2024,2,29},true)));
  const auto snapshot=snapshot_;const auto last=native_;const auto length=native_length_;
  ASSERT_NO_FATAL_FAILURE(recovery());
  EXPECT_EQ(snapshot,snapshot_);EXPECT_EQ(length,native_length_);EXPECT_EQ(last.value,native_.value);
  ASSERT_NO_FATAL_FAILURE(guards(native_));
  ASSERT_NO_FATAL_FAILURE(guards(escaped_));
}

class RedshiftClockEscapeRealTest : public RedshiftLocateEscapeRealTest {
 protected:
  SQL_TIMESTAMP_STRUCT stamp_{2024,2,29,23,59,59,999999000};SQLLEN stamp_length_{};
  std::array<SQLINTEGER,3> snapshot_{};
  static constexpr std::size_t fixture_columns_=13,fixture_rows_=4,fixture_cells_=52;
  static_assert(fixture_columns_*fixture_rows_==fixture_cells_);
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_CLOCK_ESCAPE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP() << "Clock escape scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="clock-escapes-v1");
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();if(HasFatalFailure()) { return; }
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));ASSERT_FALSE(options.contains("DSN"));
    // Reject a configured insufficient bound before connect; never silently
    // clip the thirteen-column/four-row/fifty-two-cell direct projection.
    const rs::core::database::ResultLimits defaults{};
    const auto limit=[&](const char* key,std::size_t fallback)->std::optional<std::size_t> {
      const auto found=options.find(key);if(found==options.end()) { return fallback; }
      std::size_t value{};const auto& text=found->second;
      const auto result=std::from_chars(text.data(),text.data()+text.size(),value);
      if(result.ec!=std::errc{}||result.ptr!=text.data()+text.size()) { return std::nullopt; }
      return value;
    };
    const auto columns=limit("MAXCOLUMNS",defaults.max_columns_per_description);
    const auto rows=limit("MAXROWS",defaults.max_rows);
    const auto cells=limit("MAXCELLS",defaults.max_cells);
    ASSERT_TRUE(columns);ASSERT_TRUE(rows);ASSERT_TRUE(cells);
    ASSERT_GE(*columns,fixture_columns_);ASSERT_GE(*rows,fixture_rows_);ASSERT_GE(*cells,fixture_cells_);
  }
  void clock_fixture() {
    ASSERT_NO_FATAL_FAILURE(connect_bounded());
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,nullptr,0));
    SQLULEN visible=97;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,&visible,0,nullptr));ASSERT_EQ(static_cast<SQLULEN>(0),visible);
    ASSERT_NO_FATAL_FAILURE(direct("CREATE TEMP TABLE odbcpp_clock_escape_scope (id INTEGER,tm TIME,ts TIMESTAMP)"));
    table_owned_=true; // Confirmed CREATE success on exact session only.
    ASSERT_NO_FATAL_FAILURE(direct("INSERT INTO odbcpp_clock_escape_scope VALUES "
        "(1,TIME '23:59:59.999999',TIMESTAMP '2024-02-29 23:59:59.999999'),"
        "(2,TIME '00:00:00.999999',TIMESTAMP '2023-12-31 00:00:00.999999'),"
        "(3,TIME '12:08:43.101000',TIMESTAMP '2000-01-01 12:08:43.101000'),(4,NULL,NULL)"));
  }
  void clock_fetch(SQLRETURN expected) {
    ASSERT_TRUE(cap(window_end_,true));const auto result=SQLFetch(hstmt_);
    if(result!=expected) { unknown_completion_=true; }
    ASSERT_EQ(expected,result);
  }
  void clock_close() { ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_))); }
  void clock_rows() {
    ASSERT_NO_FATAL_FAILURE(metadata(13));
    const std::array<std::array<SQLINTEGER,3>,3> literal{{{{23,59,59}},{{0,0,0}},{{12,8,43}}}};
    for(SQLINTEGER id=1;id<=4;++id) {
      ASSERT_NO_FATAL_FAILURE(clock_fetch(SQL_SUCCESS));
      ASSERT_NO_FATAL_FAILURE(integer(1,ordinal_,ordinal_length_));EXPECT_EQ(id,ordinal_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),ordinal_length_);
      for(SQLUSMALLINT group=0;group<2;++group) {
        for(SQLUSMALLINT part=0;part<3;++part) {
          const auto expected=id<=3?std::optional<SQLINTEGER>{literal[static_cast<std::size_t>(id-1)][part]}:std::nullopt;
          const auto escaped=static_cast<SQLUSMALLINT>(2+group*6+part*2);
          ASSERT_NO_FATAL_FAILURE(pair(escaped,static_cast<SQLUSMALLINT>(escaped+1),expected));
        }
      }
    }
    ASSERT_NO_FATAL_FAILURE(clock_fetch(SQL_NO_DATA));
    ASSERT_NO_FATAL_FAILURE(clock_close());
  }
  void prepare_clock() {
    const std::string text="SELECT {fn HOUR(?)},{fn MINUTE(?)},{fn SECOND(?)},"
        "CAST(FLOOR(EXTRACT(hour FROM ?)) AS INTEGER),CAST(FLOOR(EXTRACT(minute FROM ?)) AS INTEGER),CAST(FLOOR(EXTRACT(second FROM ?)) AS INTEGER) "
        "FROM odbcpp_clock_escape_scope WHERE id=1";
    std::vector<SQLWCHAR> wide;
    for(const auto ch:text) { wide.push_back(static_cast<SQLWCHAR>(ch)); }
    wide.push_back(0);ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_,wide.data(),SQL_NTS)));
    for(SQLUSMALLINT p=1;p<=6;++p) {
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,p,SQL_PARAM_INPUT,SQL_C_TYPE_TIMESTAMP,SQL_TYPE_TIMESTAMP,26,6,&stamp_,sizeof(stamp_),&stamp_length_));
    }
  }
  void clock_execution(std::optional<std::array<SQLINTEGER,3>> expected,bool early=false) {
    const auto owning_stamp=stamp_;const auto owning_length=stamp_length_;
    ASSERT_NO_FATAL_FAILURE(prepared_execute());
    ASSERT_NO_FATAL_FAILURE(metadata(6));
    ASSERT_NO_FATAL_FAILURE(clock_fetch(SQL_SUCCESS));
    for(SQLUSMALLINT part=0;part<3;++part) {
      ASSERT_NO_FATAL_FAILURE(pair(static_cast<SQLUSMALLINT>(part+1),static_cast<SQLUSMALLINT>(part+4),
          expected?std::optional<SQLINTEGER>{(*expected)[part]}:std::nullopt));
      if(native_length_!=SQL_NULL_DATA) { snapshot_[part]=native_.value; }
    }
    EXPECT_EQ(owning_stamp.year,stamp_.year);EXPECT_EQ(owning_stamp.month,stamp_.month);EXPECT_EQ(owning_stamp.day,stamp_.day);
    EXPECT_EQ(owning_stamp.hour,stamp_.hour);EXPECT_EQ(owning_stamp.minute,stamp_.minute);EXPECT_EQ(owning_stamp.second,stamp_.second);EXPECT_EQ(owning_stamp.fraction,stamp_.fraction);EXPECT_EQ(owning_length,stamp_length_);
    if(!early) {
      ASSERT_NO_FATAL_FAILURE(clock_fetch(SQL_NO_DATA));
    }
    ASSERT_NO_FATAL_FAILURE(clock_close());
  }

  void TearDown() override {
    const auto entry=rs::util::Clock::now();if(admitted_) { EXPECT_TRUE(entry<window_end_); }
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    if(table_owned_&&!unknown_completion_&&connected_&&!drop_attempted_) {
      if(SQLFreeStmt(hstmt_,SQL_CLOSE)!=SQL_SUCCESS) { unknown_completion_=true; }
      if(!unknown_completion_&&cap(cleanup_end,true)) {
        drop_attempted_=true;
        std::string text="DROP TABLE odbcpp_clock_escape_scope";
        if(known_sql(SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(text.data()),SQL_NTS))) { table_owned_=false; }
      }
    }
    // Not the temporal suite's fixed DROP: exact selected owned TEMP only.
    // Unknown completion suppresses every SQL/retry; session release is retained.
    if(hstmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr; }
    if(hdbc_) {
      if(connected_) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_)); }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    }
    if(henv_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr; }
    if(table_owned_||unknown_completion_) { ADD_FAILURE() << "Clock exact-session parent cleanup verification required"; }
  }
};

TEST_F(RedshiftClockEscapeRealTest, DirectAnsiWideTimeTimestampClockFieldsAndNullRecover) {
  ASSERT_NO_FATAL_FAILURE(clock_fixture());
  const auto query="SELECT id,"
      "{fn HOUR(tm)},CAST(FLOOR(EXTRACT(hour FROM tm)) AS INTEGER),{fn MINUTE(tm)},CAST(FLOOR(EXTRACT(minute FROM tm)) AS INTEGER),{fn SECOND(tm)},CAST(FLOOR(EXTRACT(second FROM tm)) AS INTEGER),"
      "{fn HOUR(ts)},CAST(FLOOR(EXTRACT(hour FROM ts)) AS INTEGER),{fn MINUTE(ts)},CAST(FLOOR(EXTRACT(minute FROM ts)) AS INTEGER),{fn SECOND(ts)},CAST(FLOOR(EXTRACT(second FROM ts)) AS INTEGER) "
      "FROM odbcpp_clock_escape_scope ORDER BY id";
  ASSERT_NO_FATAL_FAILURE(direct(query));
  ASSERT_NO_FATAL_FAILURE(clock_rows());
  ASSERT_NO_FATAL_FAILURE(direct(query,true));
  ASSERT_NO_FATAL_FAILURE(clock_rows());
  ASSERT_NO_FATAL_FAILURE(recovery());
}
TEST_F(RedshiftClockEscapeRealTest, PreparedTimestampFractionBoundaryNullRebindAndEarlyCloseRecover) {
  ASSERT_NO_FATAL_FAILURE(clock_fixture());
  ASSERT_NO_FATAL_FAILURE(prepare_clock());
  ASSERT_NO_FATAL_FAILURE((clock_execution(std::array<SQLINTEGER,3>{23,59,59})));
  stamp_={2023,12,31,0,0,0,999999000};
  ASSERT_NO_FATAL_FAILURE((clock_execution(std::array<SQLINTEGER,3>{0,0,0})));
  stamp_length_=SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(clock_execution(std::nullopt));
  stamp_={2000,1,1,12,8,43,101000000};stamp_length_=0;
  ASSERT_NO_FATAL_FAILURE((clock_execution(std::array<SQLINTEGER,3>{12,8,43})));
  stamp_={2024,2,29,23,59,59,999999000};
  ASSERT_NO_FATAL_FAILURE((clock_execution(std::array<SQLINTEGER,3>{23,59,59},true)));
  const auto snapshot=snapshot_;const auto last=native_;const auto length=native_length_;
  ASSERT_NO_FATAL_FAILURE(recovery());
  EXPECT_EQ(snapshot,snapshot_);EXPECT_EQ(length,native_length_);EXPECT_EQ(last.value,native_.value);
  ASSERT_NO_FATAL_FAILURE(guards(native_));
  ASSERT_NO_FATAL_FAILURE(guards(escaped_));
}

// One separately admitted, same-carrier observation. This is not an ODBC FK
// success substitute, and never logs credentials or arbitrary backend messages.
class RedshiftForeignKeyObservationNativeRealTest : public RedshiftRealTest {
protected:
  rs::util::Deadline window_end_{};
  std::optional<rs::core::database::postgres::PgDatabaseConnection> raw_session_;
  std::size_t property_bytes_{};

  void SetUp() override {
    const char* marker = std::getenv("ODBCPP_REDSHIFT_FK_OBSERVATION_ADMISSION");
    if (marker == nullptr) { GTEST_SKIP() << "FK observation scope is not admitted"; }
    ASSERT_TRUE(std::string_view(marker) == "fk-owning-observation-v1");
    window_end_ = rs::util::Clock::now() + std::chrono::seconds{60};
    RedshiftRealTest::SetUp();
    if (HasFatalFailure()) return;
    using namespace rs::core::database;
    const auto fields = rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_FALSE(fields.contains("DSN"));
    for (const auto* key : {"SERVER", "PORT", "DATABASE", "UID", "PWD", "SSLCAFILE"}) {
      ASSERT_TRUE(fields.contains(key));
      ASSERT_TRUE(!fields.at(key).empty() && fields.at(key).find('\0') == std::string::npos);
    }
    ASSERT_TRUE(fields.at("DATABASE") == "odbcpp_pilot");
    ASSERT_TRUE(fields.at("UID") == "odbcpp_pilot_test");
    unsigned port = 0;
    const auto& text = fields.at("PORT");
    ASSERT_TRUE(text.size() <= 5 && !text.empty() &&
                text.find_first_not_of("0123456789") == std::string::npos);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
    ASSERT_TRUE(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && port > 0 && port <= 65535);
    for (const auto* key : {"MAXRESPONSEBYTES", "MAXRESPONSEMESSAGES", "MAXSTARTUPRESPONSEBYTES",
        "MAXSTARTUPRESPONSEMESSAGES", "MAXROWS", "MAXCELLS", "MAXCOLUMNS", "MAXRESULTS",
        "MAXMETADATAENTRIES", "MAXCOLUMNNAMEBYTES", "MAXMETADATANAMEBYTES", "MAXDIAGNOSTICBYTES",
        "MAXSQLBYTES", "MAXPARAMETERS", "MAXPARAMETERBYTES", "MAXPARAMETERTOTALBYTES",
        "MAXCONNECTIONFIELDBYTES", "MAXREQUESTWIREBYTES", "MAXSTARTUPWIREBYTES", "MAXAUTHWIREBYTES"}) {
      ASSERT_TRUE(fields.contains(key));
    }
    ConnectionOptions options;
    options.host = fields.at("SERVER"); options.port = static_cast<std::uint16_t>(port);
    options.database = fields.at("DATABASE"); options.user = fields.at("UID");
    options.password = fields.at("PWD"); options.use_ssl = true;
    options.ssl_ca_file = fields.at("SSLCAFILE"); options.redshift_catalog_mode = "show";
    rs::odbc::parse_resource_limits(fields, options);
    ASSERT_EQ(1u, options.result_limits.max_results);
    const auto limits = [](const auto& value) {
      return std::array<std::size_t,20>{value.response_limits.max_wire_bytes, value.response_limits.max_messages,
        value.startup_response_limits.max_wire_bytes, value.startup_response_limits.max_messages,
        value.result_limits.max_rows, value.result_limits.max_cells, value.result_limits.max_columns_per_description,
        value.result_limits.max_results, value.result_limits.max_metadata_entries, value.result_limits.max_column_name_bytes,
        value.result_limits.max_metadata_name_bytes, value.result_limits.max_diagnostic_bytes,
        value.input_limits.max_sql_bytes, value.input_limits.max_parameters, value.input_limits.max_parameter_bytes,
        value.input_limits.max_parameter_total_bytes, value.input_limits.max_connection_field_bytes,
        value.input_limits.max_request_wire_bytes, value.input_limits.max_startup_wire_bytes, value.input_limits.max_auth_wire_bytes};
    };
    const auto intended_limits = limits(options);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(window_end_ - rs::util::Clock::now());
    ASSERT_GT(remaining.count(), 0);
    options.timeout = std::min(remaining, std::chrono::milliseconds{5000});
    auto settings = configured_backend_provider().resolve_connection_options(std::move(options));
    ASSERT_TRUE(settings);
    ASSERT_TRUE(limits(*settings) == intended_limits);
    ASSERT_TRUE(settings->use_ssl && settings->ssl_ca_file == fields.at("SSLCAFILE") &&
                settings->host == fields.at("SERVER") && settings->port == port &&
                settings->database == fields.at("DATABASE") && settings->user == fields.at("UID"));
    raw_session_.emplace(nullptr, std::nullopt, rs::core::database::postgres::PgCatalogProfile::Redshift);
    const auto remaining_before_connect = std::chrono::duration_cast<std::chrono::milliseconds>(window_end_ - rs::util::Clock::now());
    ASSERT_GT(remaining_before_connect.count(), 0);
    settings->timeout = std::min(remaining_before_connect, std::chrono::milliseconds{5000});
    const auto connected = raw_session_->connect(*settings);
    ASSERT_TRUE(connected);
    const auto capability = raw_session_->get_parameter("show_discovery");
    std::uint32_t version = 0;
    ASSERT_TRUE(!capability.empty() && capability.size() <= 10 &&
                capability.find_first_not_of("0123456789") == std::string::npos);
    const auto parsed_version = std::from_chars(capability.data(), capability.data() + capability.size(), version);
    ASSERT_TRUE(parsed_version.ec == std::errc{} && parsed_version.ptr == capability.data() + capability.size() && version >= 4);
  }
  void TearDown() override {
    if (raw_session_) raw_session_->disconnect();
    RedshiftRealTest::TearDown();
  }
  bool property(std::string key, std::string value) {
    const auto cost = key.size() + value.size() + 64;
    if (cost > 16384 - property_bytes_) {
      ADD_FAILURE() << "FK observation exceeded its metadata property budget"; return false;
    }
    property_bytes_ += cost; RecordProperty(key, value); return true;
  }
  bool bytes(std::string key, std::string_view value) {
    if (value.size() > 128) { ADD_FAILURE() << "FK observation metadata field exceeded128 bytes"; return false; }
    constexpr char digits[] = "0123456789abcdef";
    std::string hex; hex.reserve(value.size()*2);
    for (const unsigned char byte : value) { hex.push_back(digits[byte>>4]); hex.push_back(digits[byte&15]); }
    return property(std::move(key), std::move(hex));
  }
  bool type(std::string key, const rs::core::database::NativeTypeInfo& value) {
    return property(key+"_k", value.known ? "1" : "0") &&
      property(key+"_t", std::to_string(static_cast<int>(value.type))) &&
      property(key+"_w", std::to_string(value.column_size)) &&
      property(key+"_s", std::to_string(value.decimal_digits));
  }
};

TEST_F(RedshiftForeignKeyObservationNativeRealTest, PreparedShowOwnsRawMetadataBeforeNormalization) {
  using namespace rs::core::database;
  using namespace rs::core::database::postgres;
  ASSERT_TRUE(raw_session_.has_value());
  const auto now = rs::util::Clock::now(); ASSERT_LT(now, window_end_);
  const std::vector<QueryParameter> parameters{{"odbcpp_pilot", QueryParameterType::Unspecified},
      {"odbcpp_fixture", QueryParameterType::Unspecified},
      {"m2_catalog_child_20261003_c01", QueryParameterType::Unspecified}};
  auto raw = raw_session_->execute_prepared("SHOW CONSTRAINTS FOREIGN KEYS FROM TABLE ?.?.?;",
      parameters, std::min(window_end_, now + std::chrono::seconds{5}));
  ASSERT_TRUE(property("fk_raw_ok", raw ? "1" : "0"));
  const auto snapshot = raw.session_snapshot();
  ASSERT_TRUE(property("fk_state", std::to_string(static_cast<int>(snapshot.state))));
  ASSERT_TRUE(property("fk_disposition", std::to_string(static_cast<int>(snapshot.disposition))));
  if (!raw) {
    ASSERT_TRUE(property("fk_error_class", std::to_string(static_cast<int>(raw.backend_error().error_class))));
    ASSERT_TRUE(property("fk_error_operation", std::to_string(static_cast<int>(raw.backend_error().operation))));
    FAIL() << "Fixed preparedSHOW failed before FK normalization; see bounded properties";
  }
  ASSERT_TRUE(property("fk_columns", std::to_string(raw->columns.size())));
  ASSERT_TRUE(property("fk_rows", std::to_string(raw->rows.size())));
  ASSERT_TRUE(property("fk_affected", std::to_string(raw->affected_rows)));
  ASSERT_TRUE(property("fk_extra", std::to_string(raw->additional_results.size())));
  ASSERT_TRUE(property("fk_cell_errors", std::to_string(raw->cell_errors.size())));
  ASSERT_TRUE(property("fk_error_present", raw->error ? "1" : "0"));
  ASSERT_TRUE(property("fk_params", std::to_string(raw->normalized_parameter_types.size())));
  ASSERT_TRUE(property("fk_statement", raw->statement_kind ? std::to_string(static_cast<int>(*raw->statement_kind)) : "absent"));
  ASSERT_TRUE(property("fk_execution", raw->execution_result_shape ? std::to_string(static_cast<int>(*raw->execution_result_shape)) : "absent"));
  ASSERT_EQ(14u, raw->columns.size()); ASSERT_EQ(2u, raw->rows.size());
  ASSERT_LE(raw->normalized_parameter_types.size(), 3u);
  for (std::size_t i=0; i<raw->columns.size(); ++i) {
    const auto key = "fc"+std::to_string(i);
    ASSERT_TRUE(bytes(key+"_n", raw->columns[i].name));
    ASSERT_TRUE(property(key+"_p", raw->columns[i].normalized_type ? "1" : "0"));
    if (raw->columns[i].normalized_type) { ASSERT_TRUE(type(key, *raw->columns[i].normalized_type)); }
  }
  for (std::size_t row=0; row<raw->rows.size(); ++row) {
    ASSERT_EQ(14u, raw->rows[row].size());
    for (std::size_t column=0; column<raw->rows[row].size(); ++column) {
      const auto key = "fr"+std::to_string(row)+"c"+std::to_string(column);
      const auto& cell = raw->rows[row][column];
      ASSERT_TRUE(property(key+"_null", cell ? "0" : "1"));
      if (cell) { ASSERT_TRUE(property(key+"_len", std::to_string(cell->size()))); ASSERT_TRUE(bytes(key+"_v", *cell)); }
    }
  }
  for (std::size_t i=0; i<raw->normalized_parameter_types.size(); ++i)
    ASSERT_TRUE(type("fp"+std::to_string(i), raw->normalized_parameter_types[i]));
  const QueryResult owned = *raw;
  raw_session_->disconnect();
  EXPECT_EQ(2u, owned.rows.size());
  auto plan = redshift_foreign_key_plan(RedshiftForeignKeyDirection::Imported, std::nullopt,
      RedshiftForeignKeyTable{"odbcpp_pilot", "odbcpp_fixture", "m2_catalog_child_20261003_c01"});
  ASSERT_TRUE(plan);
  auto normalized = normalize_redshift_foreign_keys(*plan, std::move(raw));
  ASSERT_TRUE(property("fk_normalized", normalized ? "1" : "0"));
  if (!normalized) { ASSERT_TRUE(property("fk_normalize_class", std::to_string(static_cast<int>(normalized.backend_error().error_class)))); }
  EXPECT_TRUE(normalized) << "Strict FK normalizer rejected the captured owning response";
  if (normalized) { EXPECT_EQ(2u, normalized->rows.size()); }
}

class RedshiftDayOrdinalEscapeRealTest : public RedshiftLocateEscapeRealTest {
 protected:
  SQL_DATE_STRUCT date_{2024,1,7};SQL_TIMESTAMP_STRUCT stamp_{2024,1,7,12,34,56,123456000};
  SQLLEN date_length_{},input_stamp_length_{};
  SQLHSTMT sibling_{};IntegerCell sibling_value_{};SQLLEN sibling_length_{97};
  std::array<SQLINTEGER,2> snapshot_{};
  static constexpr const char* owned_table_="odbcpp_day_ordinal_escape_scope";
  static std::string create_sql(){return "CREATE TEMP TABLE odbcpp_day_ordinal_escape_scope (id INTEGER,d DATE,ts TIMESTAMP)";}
  static std::string insert_sql(){return "INSERT INTO odbcpp_day_ordinal_escape_scope VALUES "
      "(1,DATE '2024-01-07',TIMESTAMP '2024-01-07 12:34:56.123456'),"
      "(2,DATE '2024-01-06',TIMESTAMP '2024-01-06 23:59:59.999999'),"
      "(3,DATE '2024-02-29',TIMESTAMP '2024-02-29 12:34:56.101000'),"
      "(4,DATE '2024-12-31',TIMESTAMP '2024-12-31 00:00:00.123456'),(5,NULL,NULL)";}
  static std::string projection(bool prepared){
    std::string sql=prepared?"SELECT ":"SELECT id,";
    for(std::size_t group=0;group<2;++group){
      for(std::size_t part=0;part<2;++part){
        if(group!=0||part!=0){sql+=',';}
        const auto value=prepared?"?":group==0?"d":"ts";
        sql+=part==0?"{fn DAYOFWEEK(":"{fn DAYOFYEAR(";sql+=value;sql+=")},";
        sql+=part==0?"(EXTRACT(dow FROM ":"EXTRACT(doy FROM ";sql+=value;sql+=part==0?")+1)":")";
      }
    }
    sql+=" FROM odbcpp_day_ordinal_escape_scope";sql+=prepared?" WHERE id=1":" ORDER BY id";return sql;
  }
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_DAY_ORDINAL_ESCAPE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP()<<"Day ordinal escape scope is not admitted"; }
    const std::string_view expected=std::string_view(::testing::UnitTest::GetInstance()->current_test_info()->name())=="DirectDateTimestampOrdinalsAnsiWideNullAndRecovery"?"day_ordinal_date_ts_direct_v1":"day_ordinal_date_ts_prepared_v1";
    ASSERT_TRUE(std::string_view(marker)==expected);
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();if(HasFatalFailure()) { return; }
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));ASSERT_FALSE(options.contains("DSN"));
    const auto limit=[&](const char* key,std::size_t fallback)->std::optional<std::size_t>{
      const auto found=options.find(key);if(found==options.end()) { return fallback; }
      std::size_t value{};const auto& text=found->second;
      const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
      if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()) { return std::nullopt; }
      return value;
    };
    const rs::core::database::ResultLimits result_defaults{};const rs::core::database::InputLimits input_defaults{};
    const auto columns=limit("MAXCOLUMNS",result_defaults.max_columns_per_description);
    const auto rows=limit("MAXROWS",result_defaults.max_rows),cells=limit("MAXCELLS",result_defaults.max_cells);
    const auto metadata=limit("MAXMETADATAENTRIES",result_defaults.max_metadata_entries);
    const auto parameters=limit("MAXPARAMETERS",input_defaults.max_parameters);
    const auto value=limit("MAXPARAMETERBYTES",input_defaults.max_parameter_bytes);
    const auto total=limit("MAXPARAMETERTOTALBYTES",input_defaults.max_parameter_total_bytes);
    const auto sql=limit("MAXSQLBYTES",input_defaults.max_sql_bytes);
    ASSERT_TRUE(columns);ASSERT_TRUE(rows);ASSERT_TRUE(cells);ASSERT_TRUE(metadata);
    ASSERT_TRUE(parameters);ASSERT_TRUE(value);ASSERT_TRUE(total);ASSERT_TRUE(sql);
    constexpr std::size_t column_count=9,row_count=5,cell_count=45;static_assert(column_count*row_count==cell_count);
    ASSERT_GE(*columns,column_count);ASSERT_GE(*rows,row_count);ASSERT_GE(*cells,cell_count);
    ASSERT_GE(*metadata,24u); // statement T8 + parameters8 + portal T8.
    ASSERT_GE(*parameters,8u);ASSERT_GE(*value,26u);ASSERT_GE(*total,4u*(10u+26u));
    ASSERT_GE(*sql,std::max({projection(false).size(),projection(true).size(),insert_sql().size(),create_sql().size()}));
  }

  void fixture_ordinal(){
    ASSERT_NO_FATAL_FAILURE(connect_bounded());
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,nullptr,0));
    SQLULEN visible=97;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,&visible,0,nullptr));ASSERT_EQ(static_cast<SQLULEN>(0),visible);
    ASSERT_NO_FATAL_FAILURE(direct(create_sql()));table_owned_=true;
    ASSERT_NO_FATAL_FAILURE(direct(insert_sql()));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&sibling_));
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(remaining,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(sibling_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5))),0));
    std::string sql="SELECT 1";ASSERT_TRUE(known_sql(SQLExecDirect(sibling_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS)));
  }
  void ordinal_fetch(SQLRETURN expected){
    ASSERT_TRUE(cap(window_end_,true));const auto rc=SQLFetch(hstmt_);if(rc!=expected){unknown_completion_=true;}ASSERT_EQ(expected,rc);
  }
  void ordinal_close(){ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));}
  void ordinal_metadata(SQLSMALLINT count){
    ASSERT_NO_FATAL_FAILURE(metadata(count));
    for(SQLUSMALLINT first=count==9?2:1;first<static_cast<SQLUSMALLINT>(count);first+=2){
      SQLULEN a=97,b=99;SQLSMALLINT sa=-1,sb=-1;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,first,nullptr,0,nullptr,nullptr,&a,&sa,nullptr));
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,first+1,nullptr,0,nullptr,nullptr,&b,&sb,nullptr));EXPECT_EQ(a,b);EXPECT_EQ(sa,sb);
    }
  }
  void ordinal_rows(){
    ASSERT_NO_FATAL_FAILURE(ordinal_metadata(9));
    const std::array<std::array<SQLINTEGER,2>,4> expected{{{{1,7}},{{7,6}},{{5,60}},{{3,366}}}};
    for(SQLINTEGER id=1;id<=5;++id){
      ASSERT_NO_FATAL_FAILURE(ordinal_fetch(SQL_SUCCESS));
      ASSERT_NO_FATAL_FAILURE(integer(1,ordinal_,ordinal_length_));EXPECT_EQ(id,ordinal_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),ordinal_length_);
      for(SQLUSMALLINT group=0;group<2;++group){for(SQLUSMALLINT part=0;part<2;++part){
        const auto literal=id<=4?std::optional<SQLINTEGER>{expected[static_cast<std::size_t>(id-1)][part]}:std::nullopt;
        ASSERT_NO_FATAL_FAILURE(pair(static_cast<SQLUSMALLINT>(2+group*4+part*2),static_cast<SQLUSMALLINT>(3+group*4+part*2),literal));
      }}
    }
    ASSERT_NO_FATAL_FAILURE(ordinal_fetch(SQL_NO_DATA));
    ASSERT_NO_FATAL_FAILURE(ordinal_close());
  }
  void prepare_ordinal(){
    const auto sql=projection(true);std::vector<SQLWCHAR> wide;
    for(const auto ch:sql){wide.push_back(static_cast<SQLWCHAR>(ch));}wide.push_back(0);
    ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_,wide.data(),SQL_NTS)));
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&count))
        << "SQLNumParams SQLSTATE: " << get_error(SQL_HANDLE_STMT,hstmt_);ASSERT_EQ(8,count);
    for(SQLUSMALLINT p=1;p<=8;++p){
      if(p<=4){ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,p,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,&date_,sizeof(date_),&date_length_));}
      else{ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,p,SQL_PARAM_INPUT,SQL_C_TYPE_TIMESTAMP,SQL_TYPE_TIMESTAMP,26,6,&stamp_,sizeof(stamp_),&input_stamp_length_));}
    }
  }
  void ordinal_execution(std::optional<std::array<SQLINTEGER,2>> expected,bool early=false){
    const auto owning_date=date_;const auto owning_stamp=stamp_;const auto dl=date_length_,sl=input_stamp_length_;
    ASSERT_NO_FATAL_FAILURE(prepared_execute());
    ASSERT_NO_FATAL_FAILURE(ordinal_metadata(8));
    ASSERT_NO_FATAL_FAILURE(ordinal_fetch(SQL_SUCCESS));
    for(SQLUSMALLINT group=0;group<2;++group){for(SQLUSMALLINT part=0;part<2;++part){
      ASSERT_NO_FATAL_FAILURE(pair(static_cast<SQLUSMALLINT>(1+group*4+part*2),static_cast<SQLUSMALLINT>(2+group*4+part*2),expected?std::optional<SQLINTEGER>{(*expected)[part]}:std::nullopt));
      if(native_length_!=SQL_NULL_DATA){snapshot_[part]=native_.value;}
    }}
    EXPECT_EQ(owning_date.year,date_.year);EXPECT_EQ(owning_date.month,date_.month);EXPECT_EQ(owning_date.day,date_.day);
    EXPECT_EQ(owning_stamp.year,stamp_.year);EXPECT_EQ(owning_stamp.month,stamp_.month);EXPECT_EQ(owning_stamp.day,stamp_.day);
    EXPECT_EQ(owning_stamp.hour,stamp_.hour);EXPECT_EQ(owning_stamp.minute,stamp_.minute);EXPECT_EQ(owning_stamp.second,stamp_.second);EXPECT_EQ(owning_stamp.fraction,stamp_.fraction);
    EXPECT_EQ(dl,date_length_);EXPECT_EQ(sl,input_stamp_length_);
    if(!early){ASSERT_NO_FATAL_FAILURE(ordinal_fetch(SQL_NO_DATA));}
    ASSERT_NO_FATAL_FAILURE(ordinal_close());
  }
  void cap_sibling(){
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(remaining,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(sibling_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5))),0));
  }
  void owning_sibling_recovery(){
    ASSERT_FALSE(unknown_completion_);ASSERT_TRUE(rs::util::Clock::now()<window_end_);
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    auto result=SQLFetch(sibling_);if(result!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,result);
    poison(sibling_value_);sibling_length_=97;
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    result=SQLGetData(sibling_,1,SQL_C_SLONG,&sibling_value_.value,sizeof(sibling_value_.value),&sibling_length_);
    if(result!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,result);EXPECT_EQ(1,sibling_value_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),sibling_length_);
    ASSERT_NO_FATAL_FAILURE(guards(sibling_value_));
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    result=SQLFetch(sibling_);if(result!=SQL_NO_DATA){unknown_completion_=true;}ASSERT_EQ(SQL_NO_DATA,result);ASSERT_TRUE(known_sql(SQLCloseCursor(sibling_)));
  }
  void TearDown() override {
    const auto entry=rs::util::Clock::now();if(admitted_){EXPECT_TRUE(entry<window_end_);}
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    if(table_owned_&&!unknown_completion_&&connected_&&!drop_attempted_){
      if(SQLFreeStmt(hstmt_,SQL_CLOSE)!=SQL_SUCCESS){unknown_completion_=true;}
      if(!unknown_completion_&&cap(cleanup_end,true)){
        drop_attempted_=true;std::string sql="DROP TABLE ";sql+=owned_table_;
        if(known_sql(SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS))){table_owned_=false;}
      }
    }
    if(sibling_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,sibling_));sibling_=nullptr;}
    if(hstmt_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;}
    if(hdbc_){if(connected_){EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));}EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;}
    if(henv_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr;}
    if(table_owned_||unknown_completion_){ADD_FAILURE()<<"Day ordinal exact-session cleanup verification required";}
  }
};
TEST_F(RedshiftDayOrdinalEscapeRealTest, DirectDateTimestampOrdinalsAnsiWideNullAndRecovery){
  ASSERT_NO_FATAL_FAILURE(fixture_ordinal());
  ASSERT_NO_FATAL_FAILURE(direct(projection(false)));
  ASSERT_NO_FATAL_FAILURE(ordinal_rows());
  ASSERT_NO_FATAL_FAILURE(direct(projection(false),true));
  ASSERT_NO_FATAL_FAILURE(ordinal_rows());
  const auto prior=native_;const auto length=native_length_;
  ASSERT_NO_FATAL_FAILURE(owning_sibling_recovery());
  EXPECT_EQ(prior.value,native_.value);EXPECT_EQ(length,native_length_);
}
TEST_F(RedshiftDayOrdinalEscapeRealTest, PreparedDateTimestampOrdinalsNullRebindEarlyCloseAndRecovery){
  ASSERT_NO_FATAL_FAILURE(fixture_ordinal());
  ASSERT_NO_FATAL_FAILURE(prepare_ordinal());
  ASSERT_NO_FATAL_FAILURE((ordinal_execution(std::array<SQLINTEGER,2>{1,7})));
  date_={2024,1,6};stamp_={2024,1,6,23,59,59,999999000};
  ASSERT_NO_FATAL_FAILURE((ordinal_execution(std::array<SQLINTEGER,2>{7,6})));
  date_={2024,2,29};stamp_={2024,2,29,12,34,56,101000000};
  ASSERT_NO_FATAL_FAILURE((ordinal_execution(std::array<SQLINTEGER,2>{5,60})));
  date_={2024,12,31};stamp_={2024,12,31,0,0,0,123456000};
  ASSERT_NO_FATAL_FAILURE((ordinal_execution(std::array<SQLINTEGER,2>{3,366})));
  date_={2024,1,1};stamp_={2024,1,1,23,59,59,999999000};
  ASSERT_NO_FATAL_FAILURE((ordinal_execution(std::array<SQLINTEGER,2>{2,1})));
  date_length_=input_stamp_length_=SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(ordinal_execution(std::nullopt));
  date_length_=input_stamp_length_=0;date_={2024,1,7};stamp_={2024,1,7,12,34,56,123456000};
  ASSERT_NO_FATAL_FAILURE((ordinal_execution(std::array<SQLINTEGER,2>{1,7},true)));
  const auto snapshot=snapshot_;
  ASSERT_NO_FATAL_FAILURE(owning_sibling_recovery());
  EXPECT_EQ(snapshot,snapshot_);
}

#include <cmath>

class RedshiftLogFloat8EscapeRealTest : public RedshiftLocateEscapeRealTest {
 protected:
  SQLDOUBLE input_{1};SQLLEN input_length_{};
  struct DoubleCell{std::array<unsigned char,8> before;SQLDOUBLE value;std::array<unsigned char,8> after;} escaped_double_{},native_double_{};
  SQLLEN escaped_double_length_{97},native_double_length_{97};
  SQLHSTMT sibling_{};IntegerCell sibling_value_{};SQLLEN sibling_length_{97};
  std::array<SQLDOUBLE,2> snapshot_{};
  static constexpr double tolerance_=1e-12;
  static constexpr const char* owned_table_="odbcpp_log_float8_escape_scope";
  static std::string create_sql(){return "CREATE TEMP TABLE odbcpp_log_float8_escape_scope (id INTEGER,x DOUBLE PRECISION)";}
  static std::string insert_sql(){return "INSERT INTO odbcpp_log_float8_escape_scope VALUES (1,1),(2,100),(3,0.1),(4,2.718281828),(5,NULL)";}
  static std::string projection(bool prepared){
    return prepared?"SELECT {fn LOG(?)},LN(?),{fn LOG10(?)},LOG(?) FROM odbcpp_log_float8_escape_scope WHERE id=1":
        "SELECT id,{fn LOG(x)},LN(x),{fn LOG10(x)},LOG(x) FROM odbcpp_log_float8_escape_scope ORDER BY id";
  }
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_LOG_FLOAT8_ESCAPE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP()<<"FLOAT8 logarithm escape scope is not admitted"; }
    const std::string_view expected=std::string_view(::testing::UnitTest::GetInstance()->current_test_info()->name())=="DirectFloat8NaturalBaseTenAnsiWideNullAndRecovery"?"log_float8_direct_v1":"log_float8_prepared_v1";
    ASSERT_TRUE(std::string_view(marker)==expected);
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();if(HasFatalFailure()) { return; }
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));ASSERT_FALSE(options.contains("DSN"));
    const auto limit=[&](const char* key,std::size_t fallback)->std::optional<std::size_t>{
      const auto found=options.find(key);if(found==options.end()) { return fallback; }
      std::size_t value{};const auto& text=found->second;
      const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
      if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()) { return std::nullopt; }
      return value;
    };
    const rs::core::database::ResultLimits result_defaults{};const rs::core::database::InputLimits input_defaults{};
    const auto columns=limit("MAXCOLUMNS",result_defaults.max_columns_per_description);
    const auto rows=limit("MAXROWS",result_defaults.max_rows),cells=limit("MAXCELLS",result_defaults.max_cells);
    const auto metadata=limit("MAXMETADATAENTRIES",result_defaults.max_metadata_entries);
    const auto parameters=limit("MAXPARAMETERS",input_defaults.max_parameters);
    const auto value=limit("MAXPARAMETERBYTES",input_defaults.max_parameter_bytes);
    const auto total=limit("MAXPARAMETERTOTALBYTES",input_defaults.max_parameter_total_bytes);
    const auto sql=limit("MAXSQLBYTES",input_defaults.max_sql_bytes);
    ASSERT_TRUE(columns);ASSERT_TRUE(rows);ASSERT_TRUE(cells);ASSERT_TRUE(metadata);
    ASSERT_TRUE(parameters);ASSERT_TRUE(value);ASSERT_TRUE(total);ASSERT_TRUE(sql);
    constexpr std::size_t column_count=5,row_count=5,cell_count=25;static_assert(column_count*row_count==cell_count);
    ASSERT_GE(*columns,column_count);ASSERT_GE(*rows,row_count);ASSERT_GE(*cells,cell_count);
    ASSERT_GE(*metadata,12u); // statement T4 + parameters4 + portal T4.
    ASSERT_GE(*parameters,4u);ASSERT_GE(*value,128u);ASSERT_GE(*total,512u);
    ASSERT_GE(*sql,std::max({projection(false).size(),projection(true).size(),insert_sql().size(),create_sql().size()}));
  }


  void cap_sibling(){
    ASSERT_TRUE(cap(window_end_,false));
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(remaining,0);
    const auto rc=SQLSetStmtAttr(sibling_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5))),0);
    if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
  }
  void fixture_log(){
    ASSERT_NO_FATAL_FAILURE(connect_bounded());
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,nullptr,0));
    SQLULEN visible=97;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,&visible,0,nullptr));ASSERT_EQ(static_cast<SQLULEN>(0),visible);
    ASSERT_NO_FATAL_FAILURE(direct(create_sql()));table_owned_=true;
    ASSERT_NO_FATAL_FAILURE(direct(insert_sql()));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&sibling_));
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    std::string sql="SELECT 1";ASSERT_TRUE(known_sql(SQLExecDirect(sibling_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS)));
  }
  void log_fetch(SQLRETURN expected){
    ASSERT_TRUE(cap(window_end_,true));const auto rc=SQLFetch(hstmt_);if(rc!=expected){unknown_completion_=true;}ASSERT_EQ(expected,rc);
  }
  void log_close(){ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));}
  void log_metadata(SQLSMALLINT count){
    ASSERT_TRUE(cap(window_end_,true));SQLSMALLINT actual=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(hstmt_,&actual));ASSERT_EQ(count,actual);
    for(SQLUSMALLINT c=1;c<=static_cast<SQLUSMALLINT>(count);++c){
      SQLSMALLINT type=-1;ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,c,nullptr,0,nullptr,&type,nullptr,nullptr,nullptr));EXPECT_EQ(count==5&&c==1?SQL_INTEGER:SQL_DOUBLE,type);
    }
    for(SQLUSMALLINT c=count==5?2:1;c<static_cast<SQLUSMALLINT>(count);c+=2){
      SQLULEN a=97,b=99;SQLSMALLINT sa=-1,sb=-1;
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,c,nullptr,0,nullptr,nullptr,&a,&sa,nullptr));ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt_,c+1,nullptr,0,nullptr,nullptr,&b,&sb,nullptr));EXPECT_EQ(a,b);EXPECT_EQ(sa,sb);
    }
  }
  void double_value(SQLUSMALLINT c,DoubleCell& output,SQLLEN& length,bool expected_null=false){
    poison(output);std::array<unsigned char,sizeof(SQLDOUBLE)> poisoned{};std::memcpy(poisoned.data(),&output.value,sizeof(output.value));length=97;
    ASSERT_TRUE(cap(window_end_,true));const auto rc=SQLGetData(hstmt_,c,SQL_C_DOUBLE,&output.value,sizeof(output.value),&length);
    if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
    if(expected_null){ASSERT_EQ(SQL_NULL_DATA,length);}
    else{ASSERT_EQ(static_cast<SQLLEN>(sizeof(SQLDOUBLE)),length);}
    if(length==SQL_NULL_DATA){EXPECT_EQ(0,std::memcmp(poisoned.data(),&output.value,sizeof(output.value)));}
    else{EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLDOUBLE)),length);EXPECT_TRUE(std::isfinite(output.value));}
    ASSERT_NO_FATAL_FAILURE(guards(output));
  }
  void log_pair(SQLUSMALLINT escaped_column,std::optional<SQLDOUBLE> literal={},bool expected_null=false){
    ASSERT_NO_FATAL_FAILURE(double_value(escaped_column+1,native_double_,native_double_length_,expected_null));
    ASSERT_NO_FATAL_FAILURE(double_value(escaped_column,escaped_double_,escaped_double_length_,expected_null));
    EXPECT_EQ(native_double_length_,escaped_double_length_);
    if(native_double_length_!=SQL_NULL_DATA){EXPECT_NEAR(native_double_.value,escaped_double_.value,tolerance_);}
    if(literal){EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLDOUBLE)),native_double_length_);EXPECT_NEAR(*literal,native_double_.value,tolerance_);EXPECT_NEAR(*literal,escaped_double_.value,tolerance_);}
  }
  void log_rows(){
    ASSERT_NO_FATAL_FAILURE(log_metadata(5));
    const std::array<SQLDOUBLE,4> natural{0,4.605170185988091,-2.302585092994046,0.9999999998311267};
    const std::array<SQLDOUBLE,3> base10{0,2,-1};
    for(SQLINTEGER id=1;id<=5;++id){
      ASSERT_NO_FATAL_FAILURE(log_fetch(SQL_SUCCESS));
      ASSERT_TRUE(cap(window_end_,true));
      ASSERT_NO_FATAL_FAILURE(integer(1,ordinal_,ordinal_length_));EXPECT_EQ(id,ordinal_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),ordinal_length_);
      ASSERT_NO_FATAL_FAILURE(log_pair(2,id<=4?std::optional<SQLDOUBLE>{natural[static_cast<std::size_t>(id-1)]}:std::nullopt,id==5));
      ASSERT_NO_FATAL_FAILURE(log_pair(4,id<=3?std::optional<SQLDOUBLE>{base10[static_cast<std::size_t>(id-1)]}:std::nullopt,id==5));
    }
    ASSERT_NO_FATAL_FAILURE(log_fetch(SQL_NO_DATA));
    ASSERT_NO_FATAL_FAILURE(log_close());
  }
  void prepare_log(){
    const auto sql=projection(true);std::vector<SQLWCHAR> wide;for(const auto ch:sql){wide.push_back(static_cast<SQLWCHAR>(ch));}wide.push_back(0);
    ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_,wide.data(),SQL_NTS)));
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&count));ASSERT_EQ(4,count);
    for(SQLUSMALLINT p=1;p<=4;++p){ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,p,SQL_PARAM_INPUT,SQL_C_DOUBLE,SQL_DOUBLE,15,0,&input_,sizeof(input_),&input_length_));}
  }
  void log_execution(std::optional<SQLDOUBLE> natural,std::optional<SQLDOUBLE> base10,bool early=false){
    const auto owning_input=input_;const auto length=input_length_;
    ASSERT_NO_FATAL_FAILURE(prepared_execute());
    ASSERT_NO_FATAL_FAILURE(log_metadata(4));
    ASSERT_NO_FATAL_FAILURE(log_fetch(SQL_SUCCESS));
    ASSERT_NO_FATAL_FAILURE(log_pair(1,natural,length==SQL_NULL_DATA));if(native_double_length_!=SQL_NULL_DATA){snapshot_[0]=native_double_.value;}
    ASSERT_NO_FATAL_FAILURE(log_pair(3,base10,length==SQL_NULL_DATA));if(native_double_length_!=SQL_NULL_DATA){snapshot_[1]=native_double_.value;}
    EXPECT_EQ(owning_input,input_);EXPECT_EQ(length,input_length_);
    if(!early){ASSERT_NO_FATAL_FAILURE(log_fetch(SQL_NO_DATA));}
    ASSERT_NO_FATAL_FAILURE(log_close());
  }
  void owning_sibling_recovery(){
    ASSERT_FALSE(unknown_completion_);
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    auto rc=SQLFetch(sibling_);if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
    poison(sibling_value_);sibling_length_=97;
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    rc=SQLGetData(sibling_,1,SQL_C_SLONG,&sibling_value_.value,sizeof(sibling_value_.value),&sibling_length_);if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
    EXPECT_EQ(1,sibling_value_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),sibling_length_);
    ASSERT_NO_FATAL_FAILURE(guards(sibling_value_));
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    rc=SQLFetch(sibling_);if(rc!=SQL_NO_DATA){unknown_completion_=true;}ASSERT_EQ(SQL_NO_DATA,rc);
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    ASSERT_TRUE(known_sql(SQLCloseCursor(sibling_)));
  }
  void TearDown() override {
    const auto entry=rs::util::Clock::now();if(admitted_){EXPECT_TRUE(entry<window_end_);}
    const auto cleanup_end=std::min(window_end_,entry+std::chrono::seconds{5});
    if(table_owned_&&!unknown_completion_&&connected_&&!drop_attempted_){
      if(!cap(cleanup_end,true)){unknown_completion_=true;}
      if(!unknown_completion_&&SQLFreeStmt(hstmt_,SQL_CLOSE)!=SQL_SUCCESS){unknown_completion_=true;}
      if(!unknown_completion_&&cap(cleanup_end,true)){
        drop_attempted_=true;std::string sql="DROP TABLE ";sql+=owned_table_;
        if(known_sql(SQLExecDirect(hstmt_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS))){table_owned_=false;}
      }
    }
    if(sibling_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,sibling_));sibling_=nullptr;}
    if(hstmt_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;}
    if(hdbc_){if(connected_){EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));}EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;}
    if(henv_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr;}
    if(table_owned_||unknown_completion_){ADD_FAILURE()<<"FLOAT8 logarithm exact-session cleanup verification required";}
  }
};
TEST_F(RedshiftLogFloat8EscapeRealTest, DirectFloat8NaturalBaseTenAnsiWideNullAndRecovery){
  ASSERT_NO_FATAL_FAILURE(fixture_log());
  ASSERT_NO_FATAL_FAILURE(direct(projection(false)));
  ASSERT_NO_FATAL_FAILURE(log_rows());
  ASSERT_NO_FATAL_FAILURE(direct(projection(false),true));
  ASSERT_NO_FATAL_FAILURE(log_rows());
  const auto escaped_length=escaped_double_length_,native_length=native_double_length_;
  ASSERT_NO_FATAL_FAILURE(owning_sibling_recovery());
  EXPECT_EQ(escaped_length,escaped_double_length_);EXPECT_EQ(native_length,native_double_length_);
}
TEST_F(RedshiftLogFloat8EscapeRealTest, PreparedFloat8NaturalBaseTenNullSmallRebindEarlyCloseAndRecovery){
  ASSERT_NO_FATAL_FAILURE(fixture_log());
  ASSERT_NO_FATAL_FAILURE(prepare_log());
  ASSERT_NO_FATAL_FAILURE(log_execution(0,0));
  input_=100;ASSERT_NO_FATAL_FAILURE(log_execution(4.605170185988091,2));
  input_=0.1;ASSERT_NO_FATAL_FAILURE(log_execution(-2.302585092994046,-1));
  input_=2.718281828;ASSERT_NO_FATAL_FAILURE(log_execution(0.9999999998311267,std::nullopt));
  input_length_=SQL_NULL_DATA;ASSERT_NO_FATAL_FAILURE(log_execution(std::nullopt,std::nullopt));
  input_length_=0;input_=1e-6;ASSERT_NO_FATAL_FAILURE(log_execution(-13.815510557964274,-6));
  input_=1;ASSERT_NO_FATAL_FAILURE(log_execution(0,0,true));
  const auto snapshot=snapshot_;
  ASSERT_NO_FATAL_FAILURE(owning_sibling_recovery());
  EXPECT_EQ(snapshot,snapshot_);
}

// Source-only happy/configuration evidence. No safe native latency trigger is
// authorized here; HYT01 expiry remains a separately admitted proof prerequisite.
class RedshiftConnectedMetadataTimeoutRealTest : public RedshiftLocateEscapeRealTest {
 protected:
  SQLHSTMT sibling_{};
  SQLINTEGER integer_{41};SQL_DATE_STRUCT date_{2024,2,29};
  SQL_TIMESTAMP_STRUCT stamp_{2024,2,29,12,34,56,123456000};
  std::array<SQLLEN,3> input_lengths_{};
  IntegerCell output_{};SQLLEN output_length_{97};
  StampCell output_stamp_{};SQLLEN stamp_output_length_{97};
  struct DateCell {std::array<unsigned char,8> before;SQL_DATE_STRUCT value;std::array<unsigned char,8> after;} output_date_{};
  SQLLEN date_output_length_{97};
  static std::string metadata_sql(){return "SELECT CAST(? AS INTEGER),CAST(? AS DATE),CAST(? AS TIMESTAMP)";}
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_CONNECTION_METADATA_TIMEOUT_ADMISSION");
    if(marker==nullptr){GTEST_SKIP()<<"Connected metadata timeout happy scope not admitted";}
    ASSERT_EQ(std::string_view("connection_metadata_timeout_happy_v1"),std::string_view(marker));
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();if(HasFatalFailure()){return;}
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));ASSERT_FALSE(options.contains("DSN"));
    const auto limit=[&](const char* key,std::size_t fallback)->std::optional<std::size_t>{
      const auto found=options.find(key);if(found==options.end()){return fallback;}
      std::size_t value{};const auto& text=found->second;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
      if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()){return std::nullopt;}return value;
    };
    const rs::core::database::InputLimits inputs{};const rs::core::database::ResultLimits results{};
    const auto sql=limit("MAXSQLBYTES",inputs.max_sql_bytes),parameters=limit("MAXPARAMETERS",inputs.max_parameters);
    const auto value=limit("MAXPARAMETERBYTES",inputs.max_parameter_bytes),total=limit("MAXPARAMETERTOTALBYTES",inputs.max_parameter_total_bytes);
    const auto columns=limit("MAXCOLUMNS",results.max_columns_per_description),rows=limit("MAXROWS",results.max_rows),cells=limit("MAXCELLS",results.max_cells),metadata=limit("MAXMETADATAENTRIES",results.max_metadata_entries);
    ASSERT_TRUE(sql);ASSERT_TRUE(parameters);ASSERT_TRUE(value);ASSERT_TRUE(total);ASSERT_TRUE(columns);ASSERT_TRUE(rows);ASSERT_TRUE(cells);ASSERT_TRUE(metadata);
    ASSERT_GE(*sql,metadata_sql().size());ASSERT_GE(*parameters,3u);ASSERT_GE(*value,26u);ASSERT_GE(*total,38u);
    ASSERT_GE(*columns,3u);ASSERT_GE(*rows,1u);ASSERT_GE(*cells,3u);ASSERT_GE(*metadata,9u);
  }
  void metadata_cap(){
    ASSERT_FALSE(unknown_completion_);
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(remaining,0);
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,2));
    ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttrW(hdbc_,SQL_ATTR_CONNECTION_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_QUERY_TIMEOUT,nullptr,0));
    SQLUINTEGER observed=97;ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttr(hdbc_,SQL_ATTR_CONNECTION_TIMEOUT,&observed,sizeof(observed),nullptr));EXPECT_EQ(seconds,observed);
    SQLULEN query=97;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_QUERY_TIMEOUT,&query,0,nullptr));EXPECT_EQ(0u,query);
  }
  void cap_sibling(){
    ASSERT_TRUE(cap(window_end_,false));
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(remaining,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(sibling_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5))),0));
  }
  void prepare_metadata(bool eager){
    ASSERT_NO_FATAL_FAILURE(metadata_cap());
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_ENABLE_AUTO_IPD,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(eager?SQL_TRUE:SQL_FALSE)),0));
    auto sql=metadata_sql();std::vector<SQLWCHAR> wide;for(const auto ch:sql){wide.push_back(static_cast<SQLWCHAR>(ch));}wide.push_back(0);
    const auto rc=eager?SQLPrepare(hstmt_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS):SQLPrepareW(hstmt_,wide.data(),SQL_NTS);
    if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc)<<get_error(SQL_HANDLE_STMT,hstmt_);
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&count));ASSERT_EQ(3,count);
    ASSERT_NO_FATAL_FAILURE(metadata_cap());
    SQLSMALLINT columns=-1;const auto result=SQLNumResultCols(hstmt_,&columns);if(result!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,result);EXPECT_EQ(3,columns);
    for(SQLUSMALLINT p=1;p<=3;++p){
      ASSERT_NO_FATAL_FAILURE(metadata_cap());
      SQLSMALLINT type=-1,scale=-1,nullable=-1;SQLULEN size=97;
      const auto described=SQLDescribeParam(hstmt_,p,&type,&size,&scale,&nullable);if(described!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,described);
      EXPECT_EQ(p==1?SQL_INTEGER:p==2?SQL_TYPE_DATE:SQL_TYPE_TIMESTAMP,type);EXPECT_GT(size,0u);
    }
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&integer_,0,&input_lengths_[0]));
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,2,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,&date_,sizeof(date_),&input_lengths_[1]));
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,3,SQL_PARAM_INPUT,SQL_C_TYPE_TIMESTAMP,SQL_TYPE_TIMESTAMP,26,6,&stamp_,sizeof(stamp_),&input_lengths_[2]));
  }
  void executed_row(bool nulls,bool early=false){
    const auto expected_integer=integer_;const auto expected_date=date_;const auto expected_stamp=stamp_;
    ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
    integer_=99;date_={2000,1,1};stamp_={2000,1,1,0,0,0,0};
    ASSERT_TRUE(cap(window_end_,true));auto rc=SQLFetch(hstmt_);if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
    poison(output_);poison(output_date_);poison(output_stamp_);
    std::array<unsigned char,sizeof(SQLINTEGER)> integer_poison{};std::memcpy(integer_poison.data(),&output_.value,sizeof(output_.value));
    std::array<unsigned char,sizeof(SQL_DATE_STRUCT)> date_poison{};std::memcpy(date_poison.data(),&output_date_.value,sizeof(output_date_.value));
    std::array<unsigned char,sizeof(SQL_TIMESTAMP_STRUCT)> stamp_poison{};std::memcpy(stamp_poison.data(),&output_stamp_.value,sizeof(output_stamp_.value));
    ASSERT_TRUE(cap(window_end_,true));rc=SQLGetData(hstmt_,1,SQL_C_SLONG,&output_.value,sizeof(output_.value),&output_length_);if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
    ASSERT_TRUE(cap(window_end_,true));rc=SQLGetData(hstmt_,2,SQL_C_TYPE_DATE,&output_date_.value,sizeof(output_date_.value),&date_output_length_);if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
    ASSERT_TRUE(cap(window_end_,true));rc=SQLGetData(hstmt_,3,SQL_C_TYPE_TIMESTAMP,&output_stamp_.value,sizeof(output_stamp_.value),&stamp_output_length_);if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
    if(nulls){
      EXPECT_EQ(SQL_NULL_DATA,output_length_);EXPECT_EQ(SQL_NULL_DATA,date_output_length_);EXPECT_EQ(SQL_NULL_DATA,stamp_output_length_);
      EXPECT_EQ(0,std::memcmp(integer_poison.data(),&output_.value,sizeof(output_.value)));EXPECT_EQ(0,std::memcmp(date_poison.data(),&output_date_.value,sizeof(output_date_.value)));EXPECT_EQ(0,std::memcmp(stamp_poison.data(),&output_stamp_.value,sizeof(output_stamp_.value)));
    }else{
      ASSERT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),output_length_);ASSERT_EQ(static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)),date_output_length_);ASSERT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT)),stamp_output_length_);
      EXPECT_EQ(expected_integer,output_.value);EXPECT_EQ(expected_date.year,output_date_.value.year);EXPECT_EQ(expected_date.month,output_date_.value.month);EXPECT_EQ(expected_date.day,output_date_.value.day);
      EXPECT_EQ(expected_stamp.year,output_stamp_.value.year);EXPECT_EQ(expected_stamp.month,output_stamp_.value.month);EXPECT_EQ(expected_stamp.day,output_stamp_.value.day);EXPECT_EQ(expected_stamp.hour,output_stamp_.value.hour);EXPECT_EQ(expected_stamp.minute,output_stamp_.value.minute);EXPECT_EQ(expected_stamp.second,output_stamp_.value.second);EXPECT_EQ(expected_stamp.fraction,output_stamp_.value.fraction);
    }
    ASSERT_NO_FATAL_FAILURE(guards(output_));
    ASSERT_NO_FATAL_FAILURE(guards(output_date_));
    ASSERT_NO_FATAL_FAILURE(guards(output_stamp_));
    if(!early){ASSERT_TRUE(cap(window_end_,true));rc=SQLFetch(hstmt_);if(rc!=SQL_NO_DATA){unknown_completion_=true;}ASSERT_EQ(SQL_NO_DATA,rc);}
    ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  }
  void TearDown() override {
    if(admitted_){EXPECT_TRUE(rs::util::Clock::now()<window_end_);}
    // No fixture/DDL exists. Uncertain remote completion fences further SQL;
    // release owning parameter storage only after these registered handles.
    if(sibling_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,sibling_));sibling_=nullptr;}
    if(hstmt_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;}
    if(hdbc_){if(connected_){EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));}EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;}
    if(henv_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr;}
    if(unknown_completion_){ADD_FAILURE()<<"Connected metadata happy scope uncertain completion";}
  }
};
TEST_F(RedshiftConnectedMetadataTimeoutRealTest, EagerLazyTypedMetadataNullOwningRefillAndSiblingWithoutLatencyTrigger){
  ASSERT_NO_FATAL_FAILURE(connect_bounded());
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,nullptr,0));
  SQLULEN visible=97;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,&visible,0,nullptr));ASSERT_EQ(0u,visible);
  ASSERT_NO_FATAL_FAILURE(prepare_metadata(true));
  ASSERT_NO_FATAL_FAILURE(executed_row(false));
  integer_=42;date_={2024,2,29};stamp_={2024,2,29,12,34,56,123456000};
  ASSERT_NO_FATAL_FAILURE(prepare_metadata(false));
  ASSERT_NO_FATAL_FAILURE(executed_row(false));
  input_lengths_.fill(SQL_NULL_DATA);
  ASSERT_NO_FATAL_FAILURE(executed_row(true,true));
  ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&sibling_));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  std::string sql="SELECT 1";ASSERT_TRUE(known_sql(SQLExecDirect(sibling_,reinterpret_cast<SQLCHAR*>(sql.data()),SQL_NTS)));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());auto rc=SQLFetch(sibling_);if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);
  SQLINTEGER value=-1;SQLLEN bytes=-1;
  ASSERT_NO_FATAL_FAILURE(cap_sibling());rc=SQLGetData(sibling_,1,SQL_C_SLONG,&value,sizeof(value),&bytes);if(rc!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,rc);EXPECT_EQ(1,value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)),bytes);
  ASSERT_NO_FATAL_FAILURE(cap_sibling());ASSERT_TRUE(known_sql(SQLCloseCursor(sibling_)));
}

#include "odbc/unicode.h"

// Source-only no-fixture native proposal. Keyword plumbing and readback are
// observed on successful login; no delay, bad credential or expiry is induced.
class RedshiftLoginTimeoutOptionsRealTest : public RedshiftTemporalEscapeRealTest {
 protected:
  SQLHSTMT sibling_{};
  SQLINTEGER input_{41};
  SQLLEN input_length_{};
  struct IntCell { std::array<unsigned char,8> before;SQLINTEGER value;std::array<unsigned char,8> after; } output_{};
  SQLLEN output_length_{97};
  SQLINTEGER sibling_snapshot_{-1};
  static std::string typed_sql(){return "SELECT CAST(? AS INTEGER)";}
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_LOGIN_TIMEOUT_ADMISSION",
                   "login_timeout_options_happy_v1");
  }
  void setup_admitted(const char* environment, const char* expected_marker) {
    const char* marker=std::getenv(environment);
    if(marker==nullptr){GTEST_SKIP()<<"Selected happy scope not admitted";}
    ASSERT_EQ(std::string_view(expected_marker),std::string_view(marker));
    window_end_=rs::util::Clock::now()+std::chrono::seconds{40};admitted_=true;
    RedshiftRealTest::SetUp();if(HasFatalFailure()){return;}
    const auto options=rs::odbc::ConnectionString::parse(connection_string_);
    ASSERT_TRUE(options.contains("DATABASE"));ASSERT_TRUE(options.contains("UID"));
    ASSERT_EQ("odbcpp_pilot",options.at("DATABASE"));ASSERT_EQ("odbcpp_pilot_test",options.at("UID"));ASSERT_FALSE(options.contains("DSN"));
    const auto limit=[&](const char* key,std::size_t fallback)->std::optional<std::size_t>{
      const auto found=options.find(key);if(found==options.end()){return fallback;}
      const auto& text=found->second;std::size_t value{};
      const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
      if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()){return std::nullopt;}return value;
    };
    const rs::core::database::InputLimits inputs{};const rs::core::database::ResultLimits results{};
    const auto sql=limit("MAXSQLBYTES",inputs.max_sql_bytes),parameters=limit("MAXPARAMETERS",inputs.max_parameters);
    const auto value=limit("MAXPARAMETERBYTES",inputs.max_parameter_bytes),total=limit("MAXPARAMETERTOTALBYTES",inputs.max_parameter_total_bytes);
    const auto columns=limit("MAXCOLUMNS",results.max_columns_per_description),rows=limit("MAXROWS",results.max_rows),cells=limit("MAXCELLS",results.max_cells),metadata=limit("MAXMETADATAENTRIES",results.max_metadata_entries);
    ASSERT_TRUE(sql);ASSERT_TRUE(parameters);ASSERT_TRUE(value);ASSERT_TRUE(total);ASSERT_TRUE(columns);ASSERT_TRUE(rows);ASSERT_TRUE(cells);ASSERT_TRUE(metadata);
    ASSERT_GE(*sql,typed_sql().size());ASSERT_GE(*parameters,1u);ASSERT_GE(*value,11u);ASSERT_GE(*total,11u);
    ASSERT_GE(*columns,1u);ASSERT_GE(*rows,1u);ASSERT_GE(*cells,1u);ASSERT_GE(*metadata,3u);
    // Base fixture sets explicit15. A fresh unconnected handle is essential:
    // keyword tests must not accidentally exercise that overriding application value.
    ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_DBC,henv_,&hdbc_));
    ASSERT_NO_FATAL_FAILURE(readback(30));
  }
  std::string keyword_input(const char* key,const char* value){
    auto options=rs::odbc::ConnectionString::parse(connection_string_);
    options.erase("LOGINTIMEOUT");options.erase("LT");options[key]=value;
    std::string result;
    for(const auto& [name,text]:options){
      result+=name;result+="={";
      for(const auto ch:text){result.push_back(ch);if(ch=='}'){result.push_back('}');}}
      result+="};";
    }
    // Test-side serialization preserves all endpoint/principal/TLS values and
    // first-key policy. Never print the input or credentials in a failure.
    EXPECT_TRUE(options==rs::odbc::ConnectionString::parse(result))<<"Login input preservation failed";
    return result;
  }
  void readback(SQLUINTEGER expected){
    struct {SQLUINTEGER value;SQLUINTEGER guard;} captured{97,0x12345678u};SQLINTEGER bytes=-1;
    ASSERT_EQ(SQL_SUCCESS,SQLGetConnectAttrW(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,&captured.value,sizeof(captured.value),&bytes));
    EXPECT_EQ(expected,captured.value);EXPECT_EQ(0x12345678u,captured.guard);EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(captured.value)),bytes);
  }
  void login(const char* key,const char* value,SQLUINTEGER expected,bool wide){
    ASSERT_TRUE(cap(window_end_,false));
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GE(remaining,static_cast<std::int64_t>(expected));
    auto text=keyword_input(key,value);SQLRETURN result;
    if(wide){
      auto input=rs::odbc::utf8_to_wide(text);ASSERT_TRUE(input);input->push_back(0);
      result=SQLDriverConnectW(hdbc_,nullptr,input->data(),SQL_NTS,nullptr,0,nullptr,SQL_DRIVER_NOPROMPT);
    }else{result=SQLDriverConnect(hdbc_,nullptr,reinterpret_cast<SQLCHAR*>(text.data()),SQL_NTS,nullptr,0,nullptr,SQL_DRIVER_NOPROMPT);}
    if(result==SQL_SUCCESS||result==SQL_SUCCESS_WITH_INFO){connected_=true;}
    if(result!=SQL_SUCCESS){unknown_completion_=true;}
    ASSERT_EQ(SQL_SUCCESS,result)<<get_error(SQL_HANDLE_DBC,hdbc_);
    ASSERT_NO_FATAL_FAILURE(readback(expected));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&hstmt_));
    ASSERT_TRUE(cap(window_end_,true));
  }
  void invalid_selected(bool wide,SQLUINTEGER unchanged){
    ASSERT_FALSE(unknown_completion_);ASSERT_TRUE(rs::util::Clock::now()<window_end_);
    auto text=keyword_input("LOGINTIMEOUT","not-decimal");SQLSMALLINT bytes=66;SQLRETURN result;
    if(wide){
      auto input=rs::odbc::utf8_to_wide(text);ASSERT_TRUE(input);input->push_back(0);
      SQLWCHAR output[4]{static_cast<SQLWCHAR>('x'),0,static_cast<SQLWCHAR>('y'),0};
      result=SQLDriverConnectW(hdbc_,nullptr,input->data(),SQL_NTS,output,4,&bytes,SQL_DRIVER_NOPROMPT);
      EXPECT_EQ(static_cast<SQLWCHAR>('x'),output[0]);EXPECT_EQ(static_cast<SQLWCHAR>('y'),output[2]);
    }else{
      SQLCHAR output[4]{'x',0,'y',0};
      result=SQLDriverConnect(hdbc_,nullptr,reinterpret_cast<SQLCHAR*>(text.data()),SQL_NTS,output,4,&bytes,SQL_DRIVER_NOPROMPT);
      EXPECT_EQ('x',output[0]);EXPECT_EQ('y',output[2]);
    }
    // This exact local invalid-option refusal is known, not an uncertain native
    // operation. Any other return/state fences the subsequent recovery login.
    if(result==SQL_SUCCESS||result==SQL_SUCCESS_WITH_INFO){connected_=true;}
    if(result!=SQL_ERROR){unknown_completion_=true;}ASSERT_EQ(SQL_ERROR,result);EXPECT_EQ(66,bytes);
    SQLCHAR state[6]{},message[128]{};
    ASSERT_EQ(SQL_SUCCESS,SQLGetDiagRec(SQL_HANDLE_DBC,hdbc_,1,state,nullptr,message,sizeof(message),nullptr));
    if(std::string_view(reinterpret_cast<const char*>(state))!="HY000"||std::string_view(reinterpret_cast<const char*>(message))!="Invalid LoginTimeout value"){unknown_completion_=true;}
    ASSERT_STREQ("HY000",reinterpret_cast<const char*>(state));ASSERT_STREQ("Invalid LoginTimeout value",reinterpret_cast<const char*>(message));
    ASSERT_FALSE(connected_);ASSERT_NO_FATAL_FAILURE(readback(unchanged));
  }
  void release_connection(){
    ASSERT_FALSE(unknown_completion_);
    if(sibling_){ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,sibling_));sibling_=nullptr;}
    if(hstmt_){ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;}
    ASSERT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));connected_=false;
  }
  void cap_sibling(){
    ASSERT_TRUE(cap(window_end_,false));
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(window_end_-rs::util::Clock::now()).count();ASSERT_GT(remaining,0);
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(sibling_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5))),0));
  }
  void typed_row(bool nulls,bool early=false){
    const auto expected=input_;ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
    input_=99;ASSERT_TRUE(cap(window_end_,true));auto result=SQLFetch(hstmt_);if(result!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,result);
    poison(output_);std::array<unsigned char,sizeof(SQLINTEGER)> poisoned{};std::memcpy(poisoned.data(),&output_.value,sizeof(output_.value));output_length_=97;
    ASSERT_TRUE(cap(window_end_,true));result=SQLGetData(hstmt_,1,SQL_C_SLONG,&output_.value,sizeof(output_.value),&output_length_);if(result!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,result);
    if(nulls){EXPECT_EQ(SQL_NULL_DATA,output_length_);EXPECT_EQ(0,std::memcmp(poisoned.data(),&output_.value,sizeof(output_.value)));}
    else{ASSERT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)),output_length_);EXPECT_EQ(expected,output_.value);}
    ASSERT_NO_FATAL_FAILURE(guards(output_));
    if(!early){ASSERT_TRUE(cap(window_end_,true));result=SQLFetch(hstmt_);if(result!=SQL_NO_DATA){unknown_completion_=true;}ASSERT_EQ(SQL_NO_DATA,result);}
    ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  }
  void TearDown() override {
    if(admitted_){EXPECT_TRUE(rs::util::Clock::now()<window_end_);}
    // No table, DDL, DROP, recovery query or latency trigger exists. Unknown
    // remote completion suppresses further SQL; handle/session release remains.
    if(sibling_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,sibling_));sibling_=nullptr;}
    if(hstmt_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,hstmt_));hstmt_=nullptr;}
    if(hdbc_){if(connected_){EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(hdbc_));}EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,hdbc_));hdbc_=nullptr;}
    if(henv_){EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,henv_));henv_=nullptr;}
    if(unknown_completion_){ADD_FAILURE()<<"LoginTimeout happy scope uncertain completion";}
  }
};

TEST_F(RedshiftLoginTimeoutOptionsRealTest, KeywordsAAndWLocalRefusalAndSameDbcHappyRecovery){
  ASSERT_NO_FATAL_FAILURE(invalid_selected(false,30));
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT","2",2,false));
  EXPECT_EQ(SQL_ERROR,SQLSetConnectAttr(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(std::uintptr_t{3}),0));
  EXPECT_EQ("HY011",get_error(SQL_HANDLE_DBC,hdbc_));
  ASSERT_NO_FATAL_FAILURE(readback(2));
  ASSERT_NO_FATAL_FAILURE(release_connection());
  ASSERT_NO_FATAL_FAILURE(readback(2));
  ASSERT_NO_FATAL_FAILURE(invalid_selected(true,2));
  ASSERT_NO_FATAL_FAILURE(login("LT","3",3,true));
  ASSERT_NO_FATAL_FAILURE(readback(3));
}

TEST_F(RedshiftLoginTimeoutOptionsRealTest, ExplicitLifetimeTypedNullOwningRefillEarlyCloseAndSibling){
  ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}),0));
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT","not-decimal",2,false));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt_,SQL_ATTR_MAX_ROWS,nullptr,0));
  auto sql=typed_sql();auto wide=rs::odbc::utf8_to_wide(sql);ASSERT_TRUE(wide);wide->push_back(0);
  ASSERT_TRUE(cap(window_end_,true));ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_,wide->data(),SQL_NTS)));
  SQLSMALLINT parameters=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(hstmt_,&parameters));ASSERT_EQ(1,parameters);
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(hstmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&input_,0,&input_length_));
  ASSERT_NO_FATAL_FAILURE(typed_row(false));
  input_length_=SQL_NULL_DATA;ASSERT_NO_FATAL_FAILURE(typed_row(true));
  input_=43;input_length_=0;ASSERT_NO_FATAL_FAILURE(typed_row(false,true));
  const auto owned_value=output_.value;
  ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,hdbc_,&sibling_));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  std::string sibling_sql="SELECT 1";ASSERT_TRUE(known_sql(SQLExecDirect(sibling_,reinterpret_cast<SQLCHAR*>(sibling_sql.data()),SQL_NTS)));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  auto result=SQLFetch(sibling_);if(result!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,result);
  SQLLEN bytes=-1;
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  result=SQLGetData(sibling_,1,SQL_C_SLONG,&sibling_snapshot_,sizeof(sibling_snapshot_),&bytes);if(result!=SQL_SUCCESS){unknown_completion_=true;}ASSERT_EQ(SQL_SUCCESS,result);
  EXPECT_EQ(1,sibling_snapshot_);EXPECT_EQ(static_cast<SQLLEN>(sizeof(sibling_snapshot_)),bytes);EXPECT_EQ(43,owned_value);
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_TRUE(known_sql(SQLCloseCursor(sibling_)));
  ASSERT_NO_FATAL_FAILURE(release_connection());
  ASSERT_NO_FATAL_FAILURE(readback(2));
  ASSERT_NO_FATAL_FAILURE(login("LT","3",2,true));
  ASSERT_NO_FATAL_FAILURE(release_connection());
  ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttrW(hdbc_,SQL_ATTR_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(std::uintptr_t{3}),0));
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT","2",3,false));
  EXPECT_EQ(43,owned_value);EXPECT_EQ(1,sibling_snapshot_);
}

// Advisory ACCESS_MODE qualification shares the owning typed-row/deadline and
// cleanup fixture. Its separate admission marker never admits LoginTimeout cases.
class RedshiftAccessModeHintRealTest : public RedshiftLoginTimeoutOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_ACCESS_MODE_ADMISSION", "access_mode_hint_happy_v1");
  }
  void mode_readback(SQLUINTEGER expected) {
    struct { SQLUINTEGER value; SQLUINTEGER guard; } output{97, 0x12345678u};
    SQLINTEGER length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(hdbc_, SQL_ATTR_ACCESS_MODE,
        &output.value, sizeof(output.value), &length));
    EXPECT_EQ(expected, output.value);
    EXPECT_EQ(0x12345678u, output.guard);
    EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(output.value)), length);
    output.value = 97;
    ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttrW(hdbc_, SQL_ATTR_ACCESS_MODE,
        &output.value, sizeof(output.value), &length));
    EXPECT_EQ(expected, output.value);
    EXPECT_EQ(0x12345678u, output.guard);
  }
};

TEST_F(RedshiftAccessModeHintRealTest, AdvisoryModePersistsThroughTypedNullRefillAndSibling) {
  ASSERT_NO_FATAL_FAILURE(mode_readback(SQL_MODE_READ_WRITE));
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttrW(hdbc_, SQL_ATTR_ACCESS_MODE,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_MODE_READ_ONLY}), 0));
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, true));
  ASSERT_NO_FATAL_FAILURE(mode_readback(SQL_MODE_READ_ONLY));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_MAX_ROWS, nullptr, 0));
  auto sql = typed_sql(); auto wide = rs::odbc::utf8_to_wide(sql);
  ASSERT_TRUE(wide); wide->push_back(0);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, wide->data(), SQL_NTS)));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &input_, 0, &input_length_));
  ASSERT_NO_FATAL_FAILURE(typed_row(false));
  input_length_ = SQL_NULL_DATA; ASSERT_NO_FATAL_FAILURE(typed_row(true));
  input_ = 43; input_length_ = 0; ASSERT_NO_FATAL_FAILURE(typed_row(false, true));
  const auto owned_value = output_.value;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &sibling_));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_TRUE(known_sql(SQLExecDirect(sibling_, (SQLCHAR*)"SELECT 1", SQL_NTS)));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  auto result = SQLFetch(sibling_); if (result != SQL_SUCCESS) unknown_completion_ = true;
  ASSERT_EQ(SQL_SUCCESS, result);
  SQLLEN length = -1;
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  result = SQLGetData(sibling_, 1, SQL_C_SLONG, &sibling_snapshot_, sizeof(sibling_snapshot_), &length);
  if (result != SQL_SUCCESS) unknown_completion_ = true;
  ASSERT_EQ(SQL_SUCCESS, result); EXPECT_EQ(1, sibling_snapshot_);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(sibling_snapshot_)), length);
  for (auto selected : {SQL_MODE_READ_WRITE, SQL_MODE_READ_ONLY}) {
    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(hdbc_, SQL_ATTR_ACCESS_MODE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(selected)), 0));
    ASSERT_NO_FATAL_FAILURE(mode_readback(selected));
    EXPECT_EQ(43, owned_value); EXPECT_EQ(1, sibling_snapshot_);
  }
  ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(known_sql(SQLCloseCursor(sibling_)));
  ASSERT_NO_FATAL_FAILURE(release_connection());
  ASSERT_NO_FATAL_FAILURE(mode_readback(SQL_MODE_READ_ONLY));
}

// Real workflow qualification only: audit-table attribution requires a separately
// authorized observer and remains acceptance debt. No server echo is invented.
class RedshiftApplicationNameRealTest : public RedshiftLoginTimeoutOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_APPLICATION_NAME_ADMISSION", "application_name_happy_v1");
  }
};

TEST_F(RedshiftApplicationNameRealTest, AnsiWideNamesAndEmptyReconnectKeepTypedNullRefillsOwning) {
  const auto base = connection_string_;
  const std::array<std::string, 3> names{"odbcpp-appname-a", "odbcpp-é表😀", ""};
  SQLINTEGER previous_snapshot = 43;
  for (std::size_t i = 0; i < names.size(); ++i) {
    connection_string_ = base;
    connection_string_ = keyword_input("APPLICATIONNAME", names[i].c_str());
    ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, i != 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_MAX_ROWS, nullptr, 0));
    auto text = rs::odbc::utf8_to_wide(typed_sql()); ASSERT_TRUE(text); text->push_back(0);
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, text->data(), SQL_NTS)));
    input_ = 41; input_length_ = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
        SQL_INTEGER, 10, 0, &input_, 0, &input_length_));
    ASSERT_NO_FATAL_FAILURE(typed_row(false));
    input_length_ = SQL_NULL_DATA; ASSERT_NO_FATAL_FAILURE(typed_row(true));
    input_ = 43; input_length_ = 0; ASSERT_NO_FATAL_FAILURE(typed_row(false, true));
    EXPECT_EQ(43, previous_snapshot); previous_snapshot = output_.value;
    ASSERT_NO_FATAL_FAILURE(release_connection());
    EXPECT_EQ(43, previous_snapshot);
  }
  connection_string_ = base;
}

// Source-authored only. A fresh finite admission must name the exact ordinary
// table, DML/DDL scope and cleanup owner; the marker alone grants no authority.
class RedshiftReadOnlySettingRealTest : public RedshiftLoginTimeoutOptionsRealTest {
 protected:
  SQLHDBC writer_{}; SQLHSTMT writer_stmt_{};
  bool writer_connected_{}, table_owned_{}, drop_attempted_{};
  std::string table_;
  bool observed(SQLRETURN result, SQLRETURN expected = SQL_SUCCESS) {
    if (result != expected) { unknown_completion_ = true; return false; }
    return true;
  }
  void release_connection() {
    ASSERT_FALSE(unknown_completion_);
    ASSERT_TRUE(cap(window_end_, hstmt_ != nullptr));
    if (sibling_) { ASSERT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, sibling_))); sibling_ = nullptr; }
    if (hstmt_) { ASSERT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, hstmt_))); hstmt_ = nullptr; }
    ASSERT_TRUE(observed(SQLDisconnect(hdbc_))); connected_ = false;
  }
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_READ_ONLY_ADMISSION", "readonly_owned_fixture_v1");
    if (IsSkipped() || HasFatalFailure()) return;
    const char* name = std::getenv("ODBCPP_REDSHIFT_READ_ONLY_TABLE");
    ASSERT_NE(nullptr, name);
    const std::string_view selected(name);
    constexpr std::string_view prefix = "odbcpp_readonly_";
    ASSERT_EQ(prefix.size() + 16, selected.size());
    ASSERT_TRUE(selected.starts_with(prefix));
    ASSERT_TRUE(std::all_of(selected.begin() + static_cast<std::ptrdiff_t>(prefix.size()), selected.end(),
        [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }));
    table_ = "public.\"" + std::string(selected) + "\"";
    const auto options = rs::odbc::ConnectionString::parse(connection_string_);
    if (const auto limit = options.find("MAXSQLBYTES"); limit != options.end()) {
      std::size_t bytes{};
      const auto parsed = std::from_chars(limit->second.data(), limit->second.data() + limit->second.size(), bytes);
      ASSERT_EQ(std::errc{}, parsed.ec); ASSERT_EQ(limit->second.data() + limit->second.size(), parsed.ptr);
      ASSERT_GE(bytes, 256u);
    }
  }
  bool writer_cap() {
    if (unknown_completion_) return false;
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(window_end_ - rs::util::Clock::now()).count();
    if (remaining <= 0) { ADD_FAILURE() << "ReadOnly fixture window expired"; return false; }
    const auto seconds = reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining, 5)));
    return observed(SQLSetConnectAttr(writer_, SQL_ATTR_CONNECTION_TIMEOUT, seconds, 0)) &&
        (!writer_stmt_ || observed(SQLSetStmtAttr(writer_stmt_, SQL_ATTR_QUERY_TIMEOUT, seconds, 0)));
  }
  bool writer_sql(std::string text) {
    if (!writer_cap()) return false;
    const auto rc = SQLExecDirect(writer_stmt_, reinterpret_cast<SQLCHAR*>(text.data()), SQL_NTS);
    if (rc != SQL_SUCCESS) { unknown_completion_ = true; ADD_FAILURE() << "ReadOnly fixture writer failed"; return false; }
    return true;
  }
  void writer_count(SQLINTEGER expected) {
    ASSERT_TRUE(writer_sql("SELECT CAST(COUNT(*) AS INTEGER) FROM " + table_));
    ASSERT_TRUE(writer_cap()); ASSERT_TRUE(observed(SQLFetch(writer_stmt_)));
    SQLINTEGER count = -1; SQLLEN length = -1;
    ASSERT_TRUE(writer_cap());
    ASSERT_TRUE(observed(SQLGetData(writer_stmt_, 1, SQL_C_SLONG, &count, sizeof(count), &length)));
    ASSERT_EQ(static_cast<SQLLEN>(sizeof(count)), length); EXPECT_EQ(expected, count);
    ASSERT_TRUE(writer_cap()); ASSERT_TRUE(observed(SQLFetch(writer_stmt_), SQL_NO_DATA));
    ASSERT_TRUE(writer_cap()); ASSERT_TRUE(observed(SQLCloseCursor(writer_stmt_)));
  }
  void readonly_write(bool prepared) {
    const std::string sql = "INSERT INTO " + table_ + " VALUES (2)";
    ASSERT_TRUE(cap(window_end_, true)); SQLRETURN rc;
    if (prepared) {
      auto wide = rs::odbc::utf8_to_wide(sql); ASSERT_TRUE(wide); wide->push_back(0);
      ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, wide->data(), SQL_NTS)));
      ASSERT_TRUE(cap(window_end_, true)); rc = SQLExecute(hstmt_);
    } else {
      rc = SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
    }
    if (rc != SQL_ERROR) { unknown_completion_ = true; FAIL() << "ReadOnly write was not refused"; }
    SQLCHAR state[6]{}; SQLINTEGER native{};
    ASSERT_TRUE(observed(SQLGetDiagRec(SQL_HANDLE_STMT, hstmt_, 1, state, &native, nullptr, 0, nullptr)));
    const std::string observed_state(reinterpret_cast<char*>(state));
    // Paired same-principal RW success and post-reconnect identical write success
    // prove the boundary; permission/syntax/transport failures cannot substitute.
    if (observed_state.starts_with("08") || observed_state.starts_with("HYT")) {
      unknown_completion_ = true; FAIL() << "ReadOnly refusal was not a usable mode result";
    }
    RecordProperty(prepared ? "readonly_prepared_odbc_state" : "readonly_direct_odbc_state", observed_state);
    SQLUINTEGER dead = SQL_CD_TRUE;
    ASSERT_TRUE(observed(SQLGetConnectAttr(hdbc_, SQL_ATTR_CONNECTION_DEAD, &dead, sizeof(dead), nullptr)));
    if (dead != SQL_CD_FALSE) { unknown_completion_ = true; FAIL() << "ReadOnly refusal retired the connection"; }
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFreeStmt(hstmt_, SQL_CLOSE)));
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFreeStmt(hstmt_, SQL_RESET_PARAMS)));
  }
  void TearDown() override {
    // Release the target first. No cleanup SQL after uncertain completion or
    // expired original window; the external exact-principal cleanup retains debt.
    if (sibling_) { EXPECT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, sibling_))); sibling_ = nullptr; }
    if (hstmt_) { EXPECT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, hstmt_))); hstmt_ = nullptr; }
    if (hdbc_) {
      if (connected_) {
        const auto rc = SQLDisconnect(hdbc_);
        if (rc != SQL_SUCCESS) unknown_completion_ = true;
        EXPECT_EQ(SQL_SUCCESS, rc); connected_ = false;
      }
      EXPECT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_DBC, hdbc_))); hdbc_ = nullptr;
    }
    if (table_owned_ && !unknown_completion_ && writer_connected_ && !drop_attempted_) {
      drop_attempted_ = true;
      if (writer_sql("DROP TABLE " + table_)) table_owned_ = false;
    }
    if (table_owned_) ADD_FAILURE() << "ReadOnly exact fixture cleanup remains pending";
    if (writer_stmt_) { EXPECT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, writer_stmt_))); writer_stmt_ = nullptr; }
    if (writer_) {
      if (writer_connected_) { EXPECT_TRUE(observed(SQLDisconnect(writer_))); }
      EXPECT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_DBC, writer_))); writer_ = nullptr;
    }
    RedshiftLoginTimeoutOptionsRealTest::TearDown();
  }
};

TEST_F(RedshiftReadOnlySettingRealTest, OwnedWriterAndReadOnlyTargetRefuseWritesAndReconnectRecovers) {
  const auto base = connection_string_;
  connection_string_ = keyword_input("READONLY", "false");
  auto writer_input = keyword_input("LOGINTIMEOUT", "5");
  ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_DBC, henv_, &writer_)));
  ASSERT_TRUE(writer_cap());
  const auto opened = SQLDriverConnect(writer_, nullptr, reinterpret_cast<SQLCHAR*>(writer_input.data()),
      SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
  writer_connected_ = opened == SQL_SUCCESS || opened == SQL_SUCCESS_WITH_INFO;
  if (opened != SQL_SUCCESS) { unknown_completion_ = true; FAIL() << "ReadOnly fixture writer login failed"; }
  ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, writer_, &writer_stmt_)));
  ASSERT_TRUE(writer_sql("CREATE TABLE " + table_ + " (id INTEGER)")); table_owned_ = true;
  ASSERT_TRUE(writer_sql("INSERT INTO " + table_ + " VALUES (1)"));
  ASSERT_NO_FATAL_FAILURE(writer_count(1));
  connection_string_ = base; connection_string_ = keyword_input("READONLY", "true");
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, true));
  SQLCHAR info[4]{};
  ASSERT_TRUE(observed(SQLGetInfo(hdbc_, SQL_DATA_SOURCE_READ_ONLY, info,
      static_cast<SQLSMALLINT>(sizeof(info)), nullptr))); ASSERT_EQ('Y', info[0]);
  ASSERT_TRUE(observed(SQLSetConnectAttr(hdbc_, SQL_ATTR_ACCESS_MODE,
      reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_MODE_READ_WRITE)), 0)));
  for (const bool prepared : {false, true}) {
    ASSERT_NO_FATAL_FAILURE(readonly_write(prepared));
    ASSERT_NO_FATAL_FAILURE(writer_count(1));
  }
  ASSERT_TRUE(cap(window_end_, true));
  std::string select = "SELECT id FROM " + table_;
  ASSERT_TRUE(known_sql(SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(select.data()), SQL_NTS)));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_))); SQLINTEGER id = -1; SQLLEN length = -1;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_SLONG, &id, sizeof(id), &length))); EXPECT_EQ(1, id);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  auto typed = rs::odbc::utf8_to_wide(typed_sql()); ASSERT_TRUE(typed); typed->push_back(0);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, typed->data(), SQL_NTS)));
  ASSERT_TRUE(observed(SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &input_, 0, &input_length_)));
  ASSERT_NO_FATAL_FAILURE(typed_row(false)); input_length_ = SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(typed_row(true)); input_ = 43; input_length_ = 0;
  ASSERT_NO_FATAL_FAILURE(typed_row(false, true)); const auto snapshot = output_.value;
  ASSERT_NO_FATAL_FAILURE(release_connection());
  connection_string_ = base; connection_string_ = keyword_input("READONLY", "false");
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, false));
  ASSERT_TRUE(observed(SQLGetInfo(hdbc_, SQL_DATA_SOURCE_READ_ONLY, info,
      static_cast<SQLSMALLINT>(sizeof(info)), nullptr))); ASSERT_EQ('N', info[0]);
  std::string insert = "INSERT INTO " + table_ + " VALUES (2)";
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(insert.data()), SQL_NTS)));
  auto write = rs::odbc::utf8_to_wide(insert); ASSERT_TRUE(write); write->push_back(0);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, write->data(), SQL_NTS)));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
  ASSERT_NO_FATAL_FAILURE(writer_count(3)); EXPECT_EQ(43, snapshot);
  ASSERT_NO_FATAL_FAILURE(release_connection()); connection_string_ = base;
}

class RedshiftQueryTimeoutDefaultRealTest : public RedshiftLoginTimeoutOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_QUERY_TIMEOUT_DEFAULT_ADMISSION", "query_timeout_default_happy_v1");
  }
  bool observed(SQLRETURN rc, SQLRETURN expected = SQL_SUCCESS) {
    if (rc != expected) { unknown_completion_ = true; return false; }
    return true;
  }
  // The connection ceiling clips every operation to the original window, while
  // leaving the configured or explicitly chosen statement timeout untouched.
  bool bounded() { return cap(window_end_, false); }
  void seconds(SQLHSTMT statement, SQLULEN expected) {
    std::array<unsigned char, sizeof(SQLULEN) + 2> output{}; output.fill(0xa5);
    SQLINTEGER length = -1;
    ASSERT_TRUE(observed(SQLGetStmtAttrW(statement, SQL_ATTR_QUERY_TIMEOUT,
        output.data() + 1, static_cast<SQLINTEGER>(sizeof(SQLULEN)), &length)));
    SQLULEN value{}; std::memcpy(&value, output.data() + 1, sizeof(value));
    EXPECT_EQ(expected, value); EXPECT_EQ(static_cast<SQLINTEGER>(sizeof(value)), length);
    EXPECT_EQ(0xa5, output.front()); EXPECT_EQ(0xa5, output.back());
  }
  std::string query_input(const char* key, const char* value) {
    auto options = rs::odbc::ConnectionString::parse(connection_string_);
    options.erase("QUERYTIMEOUT"); options.erase("QT");
    options.erase("LOGINTIMEOUT"); options.erase("LT");
    options[key] = value; options["LOGINTIMEOUT"] = "5";
    std::string text;
    for (const auto& [name, item] : options) {
      text += name; text += "={";
      for (char ch : item) { text.push_back(ch); if (ch == '}') text.push_back('}'); }
      text += "};";
    }
    EXPECT_TRUE(options == rs::odbc::ConnectionString::parse(text))
        << "QueryTimeout login input preservation failed";
    return text;
  }
  void open_default(const char* key, const char* value, SQLULEN expected, bool wide) {
    ASSERT_TRUE(bounded());
    ASSERT_GE(std::chrono::duration_cast<std::chrono::seconds>(window_end_ - rs::util::Clock::now()).count(), 5);
    auto text = query_input(key, value); SQLRETURN rc;
    if (wide) {
      auto input = rs::odbc::utf8_to_wide(text); ASSERT_TRUE(input); input->push_back(0);
      rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    } else {
      rc = SQLDriverConnect(hdbc_, nullptr, reinterpret_cast<SQLCHAR*>(text.data()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    }
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
    ASSERT_TRUE(observed(rc));
    ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &hstmt_)));
    ASSERT_NO_FATAL_FAILURE(seconds(hstmt_, expected));
  }
  void typed_default_row(bool nulls, bool early = false) {
    const auto expected = input_; ASSERT_TRUE(bounded()); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
    input_ = 99; ASSERT_TRUE(bounded()); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
    poison(output_); std::array<unsigned char, sizeof(SQLINTEGER)> saved{};
    std::memcpy(saved.data(), &output_.value, sizeof(output_.value)); output_length_ = 97;
    ASSERT_TRUE(bounded());
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_SLONG, &output_.value, sizeof(output_.value), &output_length_)));
    if (nulls) {
      EXPECT_EQ(SQL_NULL_DATA, output_length_);
      EXPECT_EQ(0, std::memcmp(saved.data(), &output_.value, sizeof(output_.value)));
    } else { EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)), output_length_); EXPECT_EQ(expected, output_.value); }
    ASSERT_NO_FATAL_FAILURE(guards(output_));
    if (!early) { ASSERT_TRUE(bounded()); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA)); }
    ASSERT_TRUE(bounded()); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  }
  void simple(SQLHSTMT statement) {
    ASSERT_TRUE(bounded());
    ASSERT_TRUE(observed(SQLExecDirect(statement, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 1")), SQL_NTS)));
    ASSERT_TRUE(bounded()); ASSERT_TRUE(observed(SQLFetch(statement)));
    SQLINTEGER value = -1; SQLLEN length = -1;
    ASSERT_TRUE(bounded()); ASSERT_TRUE(observed(SQLGetData(statement, 1, SQL_C_SLONG, &value, sizeof(value), &length)));
    EXPECT_EQ(1, value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_TRUE(bounded()); ASSERT_TRUE(observed(SQLFetch(statement), SQL_NO_DATA));
    ASSERT_TRUE(bounded()); ASSERT_TRUE(observed(SQLCloseCursor(statement)));
  }
  void release_default() {
    ASSERT_TRUE(bounded());
    if (sibling_) { ASSERT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, sibling_))); sibling_ = nullptr; }
    if (hstmt_) { ASSERT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, hstmt_))); hstmt_ = nullptr; }
    ASSERT_TRUE(observed(SQLDisconnect(hdbc_))); connected_ = false;
  }
};

TEST_F(RedshiftQueryTimeoutDefaultRealTest, AnsiWideDefaultsOverridesTypedNullRefillAndReconnectStayOwning) {
  ASSERT_NO_FATAL_FAILURE(open_default("QT", "2", 2, false));
  ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &sibling_)));
  ASSERT_NO_FATAL_FAILURE(seconds(sibling_, 2));
  ASSERT_TRUE(observed(SQLSetStmtAttrW(hstmt_, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{0}), 0)));
  ASSERT_NO_FATAL_FAILURE(seconds(hstmt_, 0));
  ASSERT_NO_FATAL_FAILURE(seconds(sibling_, 2));
  // Unlimited is read back only; restore a finite statement budget before SQL.
  ASSERT_TRUE(observed(SQLSetStmtAttr(hstmt_, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{1}), 0)));
  ASSERT_NO_FATAL_FAILURE(seconds(hstmt_, 1));
  auto sql = rs::odbc::utf8_to_wide(typed_sql()); ASSERT_TRUE(sql); sql->push_back(0);
  ASSERT_TRUE(bounded()); ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, sql->data(), SQL_NTS)));
  ASSERT_TRUE(observed(SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &input_, 0, &input_length_)));
  ASSERT_NO_FATAL_FAILURE(typed_default_row(false)); input_length_ = SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(typed_default_row(true)); input_ = 43; input_length_ = 0;
  ASSERT_NO_FATAL_FAILURE(typed_default_row(false, true));
  ASSERT_NO_FATAL_FAILURE(seconds(hstmt_, 1));
  ASSERT_NO_FATAL_FAILURE(simple(sibling_));
  const auto snapshot = output_.value; ASSERT_NO_FATAL_FAILURE(release_default());
  ASSERT_NO_FATAL_FAILURE(open_default("QUERYTIMEOUT", "3", 3, true));
  ASSERT_NO_FATAL_FAILURE(simple(hstmt_)); EXPECT_EQ(43, snapshot);
  ASSERT_NO_FATAL_FAILURE(seconds(hstmt_, 3));
  ASSERT_NO_FATAL_FAILURE(release_default());
}

class RedshiftKeepaliveOptionsRealTest : public RedshiftLoginTimeoutOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_KEEPALIVE_ADMISSION", "keepalive_options_happy_v1");
  }
  bool observed(SQLRETURN result, SQLRETURN expected = SQL_SUCCESS) {
    if (result != expected) { unknown_completion_ = true; return false; }
    return true;
  }
  std::string keepalive_input(bool enabled, bool invalid = false) {
    auto options = rs::odbc::ConnectionString::parse(connection_string_);
    for (const char* key : {"KEEPALIVE", "KEEPALIVEIDLE", "KEEPALIVEINTERVAL", "KEEPALIVECOUNT", "LT", "LOGINTIMEOUT"}) options.erase(key);
    options["KEEPALIVE"] = enabled ? "true" : "false";
    options["KEEPALIVEIDLE"] = "7"; options["KEEPALIVEINTERVAL"] = "3";
    options["KEEPALIVECOUNT"] = invalid ? "-1" : "4";
    options["LOGINTIMEOUT"] = "5";
    std::string text;
    for (const auto& [key, value] : options) {
      text += key; text += "={";
      for (char ch : value) { text.push_back(ch); if (ch == '}') text.push_back('}'); }
      text += "};";
    }
    EXPECT_TRUE(options == rs::odbc::ConnectionString::parse(text))
        << "Keepalive login input preservation failed";
    return text;
  }
  void local_refusal() {
    ASSERT_TRUE(cap(window_end_, false));
    auto text = rs::odbc::utf8_to_wide(keepalive_input(false, true)); ASSERT_TRUE(text); text->push_back(0);
    SQLWCHAR output[4]{static_cast<SQLWCHAR>('x'), 0, static_cast<SQLWCHAR>('y'), 0}; SQLSMALLINT length = 66;
    const auto rc = SQLDriverConnectW(hdbc_, nullptr, text->data(), SQL_NTS, output, 4, &length, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
    if (rc != SQL_ERROR) unknown_completion_ = true;
    ASSERT_EQ(SQL_ERROR, rc); EXPECT_EQ(66, length);
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), output[0]); EXPECT_EQ(static_cast<SQLWCHAR>('y'), output[2]);
    SQLCHAR state[6]{}, message[128]{};
    ASSERT_TRUE(observed(SQLGetDiagRec(SQL_HANDLE_DBC, hdbc_, 1, state, nullptr, message,
        static_cast<SQLSMALLINT>(sizeof(message)), nullptr)));
    if (std::string_view(reinterpret_cast<char*>(state)) != "HY000" ||
        std::string_view(reinterpret_cast<char*>(message)) != "Invalid KeepAliveCount value") unknown_completion_ = true;
    ASSERT_STREQ("HY000", reinterpret_cast<char*>(state));
    ASSERT_STREQ("Invalid KeepAliveCount value", reinterpret_cast<char*>(message));
  }
  void open_keepalive(bool enabled, bool wide) {
    ASSERT_TRUE(cap(window_end_, false));
    ASSERT_GE(std::chrono::duration_cast<std::chrono::seconds>(window_end_ - rs::util::Clock::now()).count(), 5);
    auto text = keepalive_input(enabled); SQLRETURN rc;
    if (wide) {
      auto input = rs::odbc::utf8_to_wide(text); ASSERT_TRUE(input); input->push_back(0);
      rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    } else {
      rc = SQLDriverConnect(hdbc_, nullptr, reinterpret_cast<SQLCHAR*>(text.data()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    }
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO; ASSERT_TRUE(observed(rc));
    ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &hstmt_)));
    ASSERT_TRUE(cap(window_end_, true));
  }
  void release_keepalive() {
    ASSERT_TRUE(cap(window_end_, hstmt_ != nullptr));
    if (sibling_) { ASSERT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, sibling_))); sibling_ = nullptr; }
    if (hstmt_) { ASSERT_TRUE(observed(SQLFreeHandle(SQL_HANDLE_STMT, hstmt_))); hstmt_ = nullptr; }
    ASSERT_TRUE(observed(SQLDisconnect(hdbc_))); connected_ = false;
  }
};

TEST_F(RedshiftKeepaliveOptionsRealTest, ConfiguredTuningLocalRefusalTypedNullSiblingAndDisabledReconnectStayOwning) {
  ASSERT_NO_FATAL_FAILURE(local_refusal());
  ASSERT_NO_FATAL_FAILURE(open_keepalive(true, false));
  ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &sibling_)));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_TRUE(known_sql(SQLExecDirect(sibling_, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 1")), SQL_NTS)));
  auto text = rs::odbc::utf8_to_wide(typed_sql()); ASSERT_TRUE(text); text->push_back(0);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, text->data(), SQL_NTS)));
  ASSERT_TRUE(observed(SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &input_, 0, &input_length_)));
  ASSERT_NO_FATAL_FAILURE(typed_row(false)); input_length_ = SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(typed_row(true)); input_ = 43; input_length_ = 0;
  ASSERT_NO_FATAL_FAILURE(typed_row(false, true));
  ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(observed(SQLFetch(sibling_)));
  SQLINTEGER sibling_value = -1; SQLLEN sibling_length = -1;
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_TRUE(observed(SQLGetData(sibling_, 1, SQL_C_SLONG, &sibling_value, sizeof(sibling_value), &sibling_length)));
  EXPECT_EQ(1, sibling_value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(sibling_value)), sibling_length);
  ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(observed(SQLFetch(sibling_), SQL_NO_DATA));
  ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(observed(SQLCloseCursor(sibling_)));
  const auto snapshot = output_.value; ASSERT_NO_FATAL_FAILURE(release_keepalive());
  ASSERT_NO_FATAL_FAILURE(open_keepalive(false, true));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 1")), SQL_NTS)));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
  SQLINTEGER value = -1; SQLLEN length = -1;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_SLONG, &value, sizeof(value), &length)));
  EXPECT_EQ(1, value); EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length); EXPECT_EQ(43, snapshot);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLCloseCursor(hstmt_)));
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
}

class RedshiftVarcharSizingRealTest : public RedshiftKeepaliveOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_VARCHAR_SIZING_ADMISSION", "varchar_sizing_happy_v1");
  }
  static constexpr const char* value = "é表😀-value-longer-than-the-report";
  std::string sizing_input(bool disabled, bool invalid = false) {
    auto options = rs::odbc::ConnectionString::parse(connection_string_);
    for (const char* key : {"MAXVARCHARSIZE", "MAXLONGVARCHARSIZE", "LT", "LOGINTIMEOUT"}) options.erase(key);
    options["MAXVARCHARSIZE"] = disabled ? "0" : "8";
    options["MAXLONGVARCHARSIZE"] = invalid ? "65536" : disabled ? "0" : "17";
    options["LOGINTIMEOUT"] = "5";
    std::string text;
    for (const auto& [key, item] : options) {
      text += key; text += "={";
      for (char ch : item) { text.push_back(ch); if (ch == '}') text.push_back('}'); }
      text += "};";
    }
    EXPECT_TRUE(options == rs::odbc::ConnectionString::parse(text)) << "Sizing input preservation failed";
    return text;
  }
  void refuse_sizing() {
    ASSERT_TRUE(cap(window_end_, false));
    auto text = rs::odbc::utf8_to_wide(sizing_input(true, true)); ASSERT_TRUE(text); text->push_back(0);
    SQLWCHAR guard[3]{static_cast<SQLWCHAR>('x'), 0, static_cast<SQLWCHAR>('y')}; SQLSMALLINT length = 66;
    const auto rc = SQLDriverConnectW(hdbc_, nullptr, text->data(), SQL_NTS, guard, 3, &length, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
    if (rc != SQL_ERROR) unknown_completion_ = true;
    ASSERT_EQ(SQL_ERROR, rc); EXPECT_EQ(66, length);
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), guard[0]); EXPECT_EQ(static_cast<SQLWCHAR>('y'), guard[2]);
    SQLCHAR state[6]{}, message[128]{};
    ASSERT_TRUE(observed(SQLGetDiagRec(SQL_HANDLE_DBC, hdbc_, 1, state, nullptr, message,
        static_cast<SQLSMALLINT>(sizeof(message)), nullptr)));
    if (std::string_view(reinterpret_cast<char*>(state)) != "HY000" ||
        std::string_view(reinterpret_cast<char*>(message)) != "Invalid MaxLongVarcharSize value") unknown_completion_ = true;
    ASSERT_STREQ("HY000", reinterpret_cast<char*>(state));
    ASSERT_STREQ("Invalid MaxLongVarcharSize value", reinterpret_cast<char*>(message));
  }
  void open_sizing(bool disabled, bool wide) {
    ASSERT_TRUE(cap(window_end_, false));
    ASSERT_GE(std::chrono::duration_cast<std::chrono::seconds>(window_end_ - rs::util::Clock::now()).count(), 5);
    auto text = sizing_input(disabled); SQLRETURN rc;
    if (wide) {
      auto input = rs::odbc::utf8_to_wide(text); ASSERT_TRUE(input); input->push_back(0);
      rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    } else rc = SQLDriverConnect(hdbc_, nullptr, reinterpret_cast<SQLCHAR*>(text.data()), SQL_NTS,
        nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO; ASSERT_TRUE(observed(rc));
    ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &hstmt_)));
    ASSERT_TRUE(cap(window_end_, true));
  }
  void sizing_descriptor(SQLHSTMT statement, SQLSMALLINT expected, SQLULEN size) {
    ASSERT_TRUE(cap(window_end_, true));
    SQLSMALLINT type = 99; SQLULEN length = 99;
    ASSERT_TRUE(observed(SQLDescribeCol(statement, 1, nullptr, 0, nullptr, &type, &length, nullptr, nullptr)));
    EXPECT_EQ(expected, type); EXPECT_EQ(size, length);
    ASSERT_TRUE(cap(window_end_, true)); type = 99; length = 99;
    ASSERT_TRUE(observed(SQLDescribeColW(statement, 1, nullptr, 0, nullptr, &type, &length, nullptr, nullptr)));
    EXPECT_EQ(expected, type); EXPECT_EQ(size, length);
    SQLCHAR name[32]{}; SQLSMALLINT name_length = 99;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLColAttribute(statement, 1, SQL_DESC_TYPE_NAME, name, sizeof(name), &name_length, nullptr)));
    EXPECT_STREQ("varchar", reinterpret_cast<char*>(name));
  }
  void sizing_row(SQLHSTMT statement, bool null, bool wide, bool early = false) {
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(statement)));
    SQLLEN length = 66;
    if (wide) {
      SQLWCHAR output[128]{}; output[0] = static_cast<SQLWCHAR>('x'); output[127] = static_cast<SQLWCHAR>('y');
      ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLGetData(statement, 1, SQL_C_WCHAR, output, sizeof(output) - sizeof(SQLWCHAR), &length)));
      if (null) { EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(static_cast<SQLWCHAR>('x'), output[0]); }
      else {
        const auto expected = rs::odbc::utf8_to_wide(value); ASSERT_TRUE(expected);
        EXPECT_EQ(static_cast<SQLLEN>(expected->size() * sizeof(SQLWCHAR)), length);
        EXPECT_TRUE(std::equal(expected->begin(), expected->end(), output)); EXPECT_EQ(0, output[expected->size()]);
      }
      EXPECT_EQ(static_cast<SQLWCHAR>('y'), output[127]);
    } else {
      SQLCHAR output[128]{}; output[0] = 'x'; output[127] = 'y';
      ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLGetData(statement, 1, SQL_C_CHAR, output, sizeof(output) - 1, &length)));
      if (null) { EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ('x', output[0]); }
      else { EXPECT_EQ(static_cast<SQLLEN>(std::strlen(value)), length); EXPECT_STREQ(value, reinterpret_cast<char*>(output)); }
      EXPECT_EQ('y', output[127]);
    }
    if (!early) { ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(statement), SQL_NO_DATA)); }
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLCloseCursor(statement)));
  }
};

TEST_F(RedshiftVarcharSizingRealTest, ReportedMetadataPreservesValuesParametersNullSiblingAndDisabledReconnect) {
  ASSERT_NO_FATAL_FAILURE(refuse_sizing());
  ASSERT_NO_FATAL_FAILURE(open_sizing(false, false));
  ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &sibling_)));
  const auto direct = std::string("SELECT CAST('") + value + "' AS VARCHAR(300)) AS sizing_value";
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_TRUE(known_sql(SQLExecDirect(sibling_, reinterpret_cast<SQLCHAR*>(const_cast<char*>(direct.c_str())), SQL_NTS)));
  ASSERT_NO_FATAL_FAILURE(sizing_descriptor(sibling_, SQL_LONGVARCHAR, 17));
  auto prepared = rs::odbc::utf8_to_wide("SELECT CAST(? AS VARCHAR(300)) AS sizing_value"); ASSERT_TRUE(prepared); prepared->push_back(0);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, prepared->data(), SQL_NTS)));
  SQLLEN indicator = SQL_NTS;
  ASSERT_TRUE(observed(SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 300, 0,
      const_cast<char*>(value), static_cast<SQLLEN>(std::strlen(value) + 1), &indicator)));
  SQLSMALLINT parameter_type = 99; SQLULEN parameter_size = 99;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLDescribeParam(hstmt_, 1, &parameter_type, &parameter_size, nullptr, nullptr)));
  EXPECT_EQ(SQL_VARCHAR, parameter_type); EXPECT_EQ(300u, parameter_size);
  for (int row = 0; row < 3; ++row) {
    indicator = row == 1 ? SQL_NULL_DATA : SQL_NTS;
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
    ASSERT_NO_FATAL_FAILURE(sizing_descriptor(hstmt_, SQL_LONGVARCHAR, 17));
    ASSERT_NO_FATAL_FAILURE(sizing_row(hstmt_, row == 1, row != 0, row == 2));
  }
  ASSERT_NO_FATAL_FAILURE(sizing_row(sibling_, false, false));
  const std::string owning_snapshot(value);
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
  ASSERT_NO_FATAL_FAILURE(open_sizing(true, true));
  auto wide_direct = rs::odbc::utf8_to_wide(direct); ASSERT_TRUE(wide_direct); wide_direct->push_back(0);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecDirectW(hstmt_, wide_direct->data(), SQL_NTS)));
  ASSERT_NO_FATAL_FAILURE(sizing_descriptor(hstmt_, SQL_VARCHAR, 300));
  ASSERT_NO_FATAL_FAILURE(sizing_row(hstmt_, false, true));
  EXPECT_EQ(value, owning_snapshot);
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
}

class RedshiftBooleanCharacterRealTest : public RedshiftKeepaliveOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_BOOLEAN_CHARACTER_ADMISSION", "boolean_character_happy_v1");
  }
  std::string boolean_input(bool enabled, bool invalid = false) {
    auto options = rs::odbc::ConnectionString::parse(connection_string_);
    for (const char* key : {"BOOLSASCHAR", "MAXVARCHARSIZE", "MAXLONGVARCHARSIZE", "LT", "LOGINTIMEOUT"}) options.erase(key);
    options["BOOLSASCHAR"] = invalid ? "invalid-secret" : enabled ? "TRUE" : "FALSE";
    options["MAXVARCHARSIZE"] = "1"; options["MAXLONGVARCHARSIZE"] = "17";
    options["LOGINTIMEOUT"] = "5";
    std::string text;
    for (const auto& [key, item] : options) {
      text += key; text += "={";
      for (char ch : item) { text.push_back(ch); if (ch == '}') text.push_back('}'); }
      text += "};";
    }
    EXPECT_TRUE(options == rs::odbc::ConnectionString::parse(text)) << "Boolean input preservation failed";
    return text;
  }
  void refuse_boolean() {
    ASSERT_TRUE(cap(window_end_, false));
    auto input = rs::odbc::utf8_to_wide(boolean_input(true, true)); ASSERT_TRUE(input); input->push_back(0);
    SQLWCHAR output[3]{static_cast<SQLWCHAR>('x'), 0, static_cast<SQLWCHAR>('y')}; SQLSMALLINT length = 66;
    const auto rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, output, 3, &length, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
    if (rc != SQL_ERROR) unknown_completion_ = true;
    ASSERT_EQ(SQL_ERROR, rc); EXPECT_EQ(66, length);
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), output[0]); EXPECT_EQ(static_cast<SQLWCHAR>('y'), output[2]);
    SQLCHAR state[6]{}, message[128]{};
    ASSERT_TRUE(observed(SQLGetDiagRec(SQL_HANDLE_DBC, hdbc_, 1, state, nullptr, message, sizeof(message), nullptr)));
    if (std::string_view(reinterpret_cast<char*>(state)) != "HY000" ||
        std::string_view(reinterpret_cast<char*>(message)) != "Invalid BoolsAsChar value") unknown_completion_ = true;
    ASSERT_STREQ("HY000", reinterpret_cast<char*>(state));
    ASSERT_STREQ("Invalid BoolsAsChar value", reinterpret_cast<char*>(message));
  }
  void open_boolean(bool enabled, bool wide) {
    ASSERT_TRUE(cap(window_end_, false));
    ASSERT_GE(std::chrono::duration_cast<std::chrono::seconds>(window_end_ - rs::util::Clock::now()).count(), 5);
    auto text = boolean_input(enabled); SQLRETURN rc;
    if (wide) {
      auto input = rs::odbc::utf8_to_wide(text); ASSERT_TRUE(input); input->push_back(0);
      rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    } else rc = SQLDriverConnect(hdbc_, nullptr, reinterpret_cast<SQLCHAR*>(text.data()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO; ASSERT_TRUE(observed(rc));
    ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &hstmt_)));
    ASSERT_TRUE(cap(window_end_, true));
  }
  void boolean_descriptor(SQLHSTMT statement, SQLSMALLINT expected) {
    for (const bool wide : {false, true}) {
      ASSERT_TRUE(cap(window_end_, true));
      SQLSMALLINT type = 99; struct { SQLULEN size = 99; SQLULEN guard = 77; } dimension;
      const auto rc = wide ? SQLDescribeColW(statement, 1, nullptr, 0, nullptr, &type, &dimension.size, nullptr, nullptr)
          : SQLDescribeCol(statement, 1, nullptr, 0, nullptr, &type, &dimension.size, nullptr, nullptr);
      ASSERT_TRUE(observed(rc)); EXPECT_EQ(expected, type); EXPECT_EQ(1u, dimension.size); EXPECT_EQ(77u, dimension.guard);
    }
    ASSERT_TRUE(cap(window_end_, true));
    SQLCHAR name[32]{};
    ASSERT_TRUE(observed(SQLColAttribute(statement, 1, SQL_DESC_TYPE_NAME, name, sizeof(name), nullptr, nullptr)));
    EXPECT_STREQ("boolean", reinterpret_cast<char*>(name));
  }
  void boolean_row(SQLHSTMT statement, int expected, bool character_default) {
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(statement)));
    SQLCHAR text[3]{'x', 'y', 'z'}; SQLLEN length = 66;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(statement, 1, SQL_C_DEFAULT, text, sizeof(text) - 1, &length)));
    if (expected < 0) { EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ('x', text[0]); }
    else if (character_default) { EXPECT_EQ(static_cast<SQLCHAR>('0' + expected), text[0]); EXPECT_EQ(0, text[1]); EXPECT_EQ(1, length); }
    else { EXPECT_EQ(static_cast<SQLCHAR>(expected), text[0]); EXPECT_EQ(1, length); }
    EXPECT_EQ('z', text[2]);
    SQLCHAR bit = 88;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(statement, 2, SQL_C_BIT, &bit, sizeof(bit), &length)));
    if (expected < 0) { EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(88, bit); }
    else { EXPECT_EQ(static_cast<SQLCHAR>(expected), bit); EXPECT_EQ(1, length); }
    SQLWCHAR output[3]{static_cast<SQLWCHAR>('x'), static_cast<SQLWCHAR>('y'), static_cast<SQLWCHAR>('z')};
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(statement, 3, SQL_C_WCHAR, output, sizeof(output) - sizeof(SQLWCHAR), &length)));
    if (expected < 0) { EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(static_cast<SQLWCHAR>('x'), output[0]); }
    else { EXPECT_EQ(static_cast<SQLWCHAR>('0' + expected), output[0]); EXPECT_EQ(0, output[1]); EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length); }
    EXPECT_EQ(static_cast<SQLWCHAR>('z'), output[2]);
  }
};

// Future admission upper bound: two verified TLS logins, seven SQL exchanges
// (one Prepare, one DescribeParam, five executions), 40s aggregate/per-op5.
// No fixture, DDL, observer query, delay, fault or cleanup SQL is authorized here.
TEST_F(RedshiftBooleanCharacterRealTest, CanonicalValuesNativeParametersNullRefillSiblingAndDisabledReconnect) {
  ASSERT_NO_FATAL_FAILURE(refuse_boolean());
  ASSERT_NO_FATAL_FAILURE(open_boolean(true, false));
  ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &sibling_)));
  SQLCHAR direct[] = "SELECT flag, flag, flag FROM (SELECT TRUE AS flag, 1 AS ordinal UNION ALL SELECT FALSE, 2 UNION ALL SELECT CAST(NULL AS BOOLEAN), 3) AS flags ORDER BY ordinal";
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_TRUE(known_sql(SQLExecDirect(sibling_, direct, SQL_NTS)));
  ASSERT_NO_FATAL_FAILURE(boolean_descriptor(sibling_, SQL_VARCHAR));
  auto query = rs::odbc::utf8_to_wide("SELECT flag, flag, flag FROM (SELECT CAST(? AS BOOLEAN) AS flag) AS flags"); ASSERT_TRUE(query); query->push_back(0);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLPrepareW(hstmt_, query->data(), SQL_NTS)));
  SQLCHAR bit = 1; SQLLEN indicator = 1;
  ASSERT_TRUE(observed(SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 1, 0, &bit, sizeof(bit), &indicator)));
  SQLSMALLINT parameter_type = 99; struct { SQLULEN size = 99; SQLULEN guard = 37; } dimensions;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLDescribeParam(hstmt_, 1, &parameter_type, &dimensions.size, nullptr, nullptr)));
  EXPECT_EQ(SQL_BIT, parameter_type); EXPECT_EQ(1u, dimensions.size); EXPECT_EQ(37u, dimensions.guard);
  for (int expected : {1, 0, -1}) {
    bit = expected == 1 ? 1 : 0; indicator = expected < 0 ? SQL_NULL_DATA : 1;
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
    ASSERT_NO_FATAL_FAILURE(boolean_descriptor(hstmt_, SQL_VARCHAR));
    ASSERT_NO_FATAL_FAILURE(boolean_row(hstmt_, expected, true));
    if (expected >= 0) { ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA)); }
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLCloseCursor(hstmt_)));
  }
  for (int expected : {1, 0, -1}) {
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    ASSERT_NO_FATAL_FAILURE(boolean_row(sibling_, expected, true));
  }
  ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(observed(SQLFetch(sibling_), SQL_NO_DATA));
  ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(observed(SQLCloseCursor(sibling_)));
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
  ASSERT_NO_FATAL_FAILURE(open_boolean(false, true));
  auto wide = rs::odbc::utf8_to_wide("SELECT TRUE AS flag, FALSE AS other_flag, CAST(NULL AS BOOLEAN) AS null_flag"); ASSERT_TRUE(wide); wide->push_back(0);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecDirectW(hstmt_, wide->data(), SQL_NTS)));
  ASSERT_NO_FATAL_FAILURE(boolean_descriptor(hstmt_, SQL_BIT));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
  SQLCHAR value = 88; SQLLEN length = 66;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_DEFAULT, &value, sizeof(value), &length)));
  EXPECT_EQ(1, value); EXPECT_EQ(1, length);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLCloseCursor(hstmt_)));
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
}

class RedshiftTableTypesRealTest : public RedshiftKeepaliveOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_TABLE_TYPES_ADMISSION", "table_types_happy_v1");
  }
  std::string table_types_input(bool enabled, bool invalid = false) {
    auto options = rs::odbc::ConnectionString::parse(connection_string_);
    for (const char* key : {"ENABLETABLETYPES", "REDSHIFTCATALOGMODE", "LT", "LOGINTIMEOUT"}) options.erase(key);
    options["ENABLETABLETYPES"] = invalid ? "invalid-secret" : enabled ? "TRUE" : "FALSE";
    options["REDSHIFTCATALOGMODE"] = "LEGACY";
    options["LOGINTIMEOUT"] = "5";
    std::string text;
    for (const auto& [key, item] : options) {
      text += key; text += "={";
      for (char ch : item) { text.push_back(ch); if (ch == '}') text.push_back('}'); }
      text += "};";
    }
    EXPECT_TRUE(options == rs::odbc::ConnectionString::parse(text)) << "Table types input preservation failed";
    return text;
  }
  void refuse_table_types() {
    ASSERT_TRUE(cap(window_end_, false));
    auto input = rs::odbc::utf8_to_wide(table_types_input(true, true)); ASSERT_TRUE(input); input->push_back(0);
    SQLWCHAR output[3]{static_cast<SQLWCHAR>('x'), 0, static_cast<SQLWCHAR>('y')}; SQLSMALLINT length = 66;
    const auto rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, output, 3, &length, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
    if (rc != SQL_ERROR) unknown_completion_ = true;
    ASSERT_EQ(SQL_ERROR, rc); EXPECT_EQ(66, length);
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), output[0]); EXPECT_EQ(static_cast<SQLWCHAR>('y'), output[2]);
    SQLCHAR state[6]{}, message[128]{};
    ASSERT_TRUE(observed(SQLGetDiagRec(SQL_HANDLE_DBC, hdbc_, 1, state, nullptr, message, sizeof(message), nullptr)));
    if (std::string_view(reinterpret_cast<char*>(state)) != "HY000" ||
        std::string_view(reinterpret_cast<char*>(message)) != "Invalid EnableTableTypes value") unknown_completion_ = true;
    ASSERT_STREQ("HY000", reinterpret_cast<char*>(state));
    ASSERT_STREQ("Invalid EnableTableTypes value", reinterpret_cast<char*>(message));
  }
  void open_table_types(bool enabled, bool wide) {
    ASSERT_TRUE(cap(window_end_, false));
    ASSERT_GE(std::chrono::duration_cast<std::chrono::seconds>(window_end_ - rs::util::Clock::now()).count(), 5);
    auto text = table_types_input(enabled); SQLRETURN rc;
    if (wide) {
      auto input = rs::odbc::utf8_to_wide(text); ASSERT_TRUE(input); input->push_back(0);
      rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    } else rc = SQLDriverConnect(hdbc_, nullptr, reinterpret_cast<SQLCHAR*>(text.data()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO; ASSERT_TRUE(observed(rc));
    ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &hstmt_)));
    ASSERT_TRUE(cap(window_end_, true));
  }

  void enumeration(SQLHSTMT statement, bool wide) {
    ASSERT_TRUE(cap(window_end_, true));
    if (wide) {
      SQLWCHAR empty[]{0}, all[]{'%',0};
      ASSERT_TRUE(known_sql(SQLTablesW(statement, empty, SQL_NTS, empty, SQL_NTS, empty, SQL_NTS, all, SQL_NTS)));
    } else {
      ASSERT_TRUE(known_sql(SQLTables(statement, (SQLCHAR*)"", SQL_NTS, (SQLCHAR*)"", SQL_NTS,
          (SQLCHAR*)"", SQL_NTS, (SQLCHAR*)SQL_ALL_TABLE_TYPES, SQL_NTS)));
    }
  }
  void enumeration_rows(SQLHSTMT statement, bool detailed, bool wide) {
    const std::vector<std::string> expected = detailed
        ? std::vector<std::string>{"EXTERNAL TABLE", "LOCAL TEMPORARY", "SYSTEM TABLE", "SYSTEM VIEW", "TABLE", "VIEW"}
        : std::vector<std::string>{"TABLE", "VIEW"};
    for (const auto& kind : expected) {
      ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLFetch(statement)));
      SQLLEN bytes = 66;
      for (SQLUSMALLINT column : {1, 2, 3}) {
        SQLCHAR value = 71;
        ASSERT_TRUE(cap(window_end_, true));
        ASSERT_TRUE(observed(SQLGetData(statement, column, SQL_C_CHAR, &value, 1, &bytes)));
        EXPECT_EQ(SQL_NULL_DATA, bytes); EXPECT_EQ(71, value);
      }
      ASSERT_TRUE(cap(window_end_, true));
      if (wide) {
        struct { SQLWCHAR value[32]{}; SQLWCHAR guard = 77; } output;
        ASSERT_TRUE(observed(SQLGetData(statement, 4, SQL_C_WCHAR, output.value, sizeof(output.value), &bytes)));
        EXPECT_EQ(static_cast<SQLLEN>(kind.size() * sizeof(SQLWCHAR)), bytes);
        for (std::size_t i = 0; i < kind.size(); ++i) EXPECT_EQ(static_cast<SQLWCHAR>(kind[i]), output.value[i]);
        EXPECT_EQ(0, output.value[kind.size()]); EXPECT_EQ(77, output.guard);
      } else {
        struct { SQLCHAR value[32]{}; SQLCHAR guard = 77; } output;
        ASSERT_TRUE(observed(SQLGetData(statement, 4, SQL_C_CHAR, output.value, sizeof(output.value), &bytes)));
        EXPECT_EQ(kind, reinterpret_cast<char*>(output.value)); EXPECT_EQ(static_cast<SQLLEN>(kind.size()), bytes); EXPECT_EQ(77, output.guard);
      }
      SQLCHAR remark = 71;
      ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLGetData(statement, 5, SQL_C_CHAR, &remark, 1, &bytes)));
      EXPECT_EQ(SQL_NULL_DATA, bytes); EXPECT_EQ(71, remark);
    }
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLFetch(statement), SQL_NO_DATA));
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLCloseCursor(statement)));
  }
};

// Future finite admission: two TLS logins and five catalog SQL executions;
// 40s aggregate, per-operation5s. No DDL, fixture, observer, delay or cleanup SQL.
// Empty ordinary discovery qualifies live query grammar/filtering, not native
// system/external object coverage (six-kind source coverage is synthetic).
TEST_F(RedshiftTableTypesRealTest, GenericAndDetailedEnumerationNullsSiblingEmptyFilterAndReconnect) {
  ASSERT_NO_FATAL_FAILURE(refuse_table_types());
  ASSERT_NO_FATAL_FAILURE(open_table_types(false, false));
  ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &sibling_)));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_NO_FATAL_FAILURE(enumeration(sibling_, false));
  ASSERT_NO_FATAL_FAILURE(enumeration(hstmt_, true));
  ASSERT_NO_FATAL_FAILURE(enumeration_rows(hstmt_, false, true));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLTables(hstmt_, (SQLCHAR*)"", SQL_NTS, (SQLCHAR*)"", SQL_NTS,
      (SQLCHAR*)"", SQL_NTS, (SQLCHAR*)"EXTERNAL TABLE", SQL_NTS)));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLCloseCursor(hstmt_)));
  ASSERT_NO_FATAL_FAILURE(enumeration(hstmt_, false));
  ASSERT_NO_FATAL_FAILURE(enumeration_rows(hstmt_, false, false));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_NO_FATAL_FAILURE(enumeration_rows(sibling_, false, false));
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
  ASSERT_NO_FATAL_FAILURE(open_table_types(true, true));
  ASSERT_NO_FATAL_FAILURE(enumeration(hstmt_, false));
  ASSERT_NO_FATAL_FAILURE(enumeration_rows(hstmt_, true, true));
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
}

// Native happy/local-refusal proof only; active server-cancel timing remains
// a separately admitted scenario, never inferred from idle success.
class RedshiftCancelHandleRealTest : public RedshiftLoginTimeoutOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_CANCEL_HANDLE_ADMISSION", "cancel_handle_happy_v1");
  }
};

TEST_F(RedshiftCancelHandleRealTest, IdleNeedDataRefillNullAndSiblingRemainOwning) {
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, false));
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &sibling_));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_TRUE(known_sql(SQLExecDirect(sibling_, (SQLCHAR*)"SELECT 1", SQL_NTS)));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLPrepare(hstmt_, (SQLCHAR*)"SELECT CAST(? AS INTEGER)", SQL_NTS)));
  input_length_ = SQL_DATA_AT_EXEC;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &input_, sizeof(input_), &input_length_));
  ASSERT_TRUE(cap(window_end_, true));
  auto result = SQLExecute(hstmt_);
  if (result != SQL_NEED_DATA) unknown_completion_ = true;
  ASSERT_EQ(SQL_NEED_DATA, result);
  ASSERT_EQ(SQL_SUCCESS, SQLCancelHandle(SQL_HANDLE_STMT, hstmt_));
  input_length_ = 0; input_ = 41;
  ASSERT_NO_FATAL_FAILURE(typed_row(false));
  input_length_ = SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(typed_row(true));
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  ASSERT_EQ(SQL_SUCCESS, SQLCancelHandle(SQL_HANDLE_STMT, sibling_));
  ASSERT_EQ(SQL_ERROR, SQLCancelHandle(SQL_HANDLE_DBC, hdbc_));
  EXPECT_EQ("IM001", get_error(SQL_HANDLE_DBC, hdbc_));
  SQLUSMALLINT supported = SQL_TRUE;
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(hdbc_, SQL_API_SQLCANCELHANDLE, &supported));
  EXPECT_EQ(SQL_FALSE, supported);
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  result = SQLFetch(sibling_); if (result != SQL_SUCCESS) unknown_completion_ = true;
  ASSERT_EQ(SQL_SUCCESS, result);
  SQLINTEGER value = -1; SQLLEN length = 97;
  ASSERT_NO_FATAL_FAILURE(cap_sibling());
  result = SQLGetData(sibling_, 1, SQL_C_SLONG, &value, sizeof(value), &length);
  if (result != SQL_SUCCESS) unknown_completion_ = true;
  ASSERT_EQ(SQL_SUCCESS, result); EXPECT_EQ(1, value); EXPECT_EQ(sizeof(value), static_cast<std::size_t>(length));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLExecDirect(hstmt_, (SQLCHAR*)"SELECT 7", SQL_NTS)));
  ASSERT_EQ(SQL_SUCCESS, SQLCancelHandle(SQL_HANDLE_STMT, hstmt_));
  ASSERT_TRUE(cap(window_end_, true));
  result = SQLFetch(hstmt_); if (result != SQL_SUCCESS) unknown_completion_ = true;
  ASSERT_EQ(SQL_SUCCESS, result);
  ASSERT_TRUE(cap(window_end_, true));
  result = SQLGetData(hstmt_, 1, SQL_C_SLONG, &value, sizeof(value), &length);
  if (result != SQL_SUCCESS) unknown_completion_ = true;
  ASSERT_EQ(SQL_SUCCESS, result); EXPECT_EQ(7, value);
  ASSERT_NO_FATAL_FAILURE(release_connection());
}

class RedshiftMetadataScopeRealTest : public RedshiftKeepaliveOptionsRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_METADATA_SCOPE_ADMISSION", "metadata_scope_existing_principal_v1");
  }
  std::string scope_input(const char* value, const char* mode = "LEGACY") {
    auto options = rs::odbc::ConnectionString::parse(connection_string_);
    for (const char* key : {"DATABASEMETADATACURRENTDBONLY", "REDSHIFTCATALOGMODE", "LT", "LOGINTIMEOUT"}) options.erase(key);
    if (value) options["DATABASEMETADATACURRENTDBONLY"] = value;
    options["REDSHIFTCATALOGMODE"] = mode; options["LOGINTIMEOUT"] = "5";
    std::string text;
    for (const auto& [key, item] : options) {
      text += key; text += "={";
      for (char ch : item) { text.push_back(ch); if (ch == '}') text.push_back('}'); }
      text += "};";
    }
    EXPECT_TRUE(options == rs::odbc::ConnectionString::parse(text)) << "Metadata scope input preservation failed";
    return text;
  }
  void invalid_scope() {
    ASSERT_TRUE(cap(window_end_, false));
    auto input = rs::odbc::utf8_to_wide(scope_input("invalid-secret")); ASSERT_TRUE(input); input->push_back(0);
    SQLWCHAR output[3]{'x', 0, 'y'}; SQLSMALLINT bytes = 66;
    const auto rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, output, 3, &bytes, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO;
    if (rc != SQL_ERROR) unknown_completion_ = true;
    ASSERT_EQ(SQL_ERROR, rc); EXPECT_EQ(66, bytes); EXPECT_EQ('x', output[0]); EXPECT_EQ('y', output[2]);
    SQLCHAR state[6]{}, message[128]{};
    ASSERT_TRUE(observed(SQLGetDiagRec(SQL_HANDLE_DBC, hdbc_, 1, state, nullptr, message,
        static_cast<SQLSMALLINT>(sizeof(message)), nullptr)));
    if (std::string_view(reinterpret_cast<char*>(state)) != "HY000" ||
        std::string_view(reinterpret_cast<char*>(message)) != "Invalid DatabaseMetadataCurrentDbOnly value") unknown_completion_ = true;
    ASSERT_STREQ("HY000", reinterpret_cast<char*>(state));
    ASSERT_STREQ("Invalid DatabaseMetadataCurrentDbOnly value", reinterpret_cast<char*>(message));
  }
  void open_scope(const char* value, bool wide, const char* mode = "LEGACY") {
    ASSERT_TRUE(cap(window_end_, false));
    ASSERT_GE(std::chrono::duration_cast<std::chrono::seconds>(window_end_ - rs::util::Clock::now()).count(), 5);
    auto text = scope_input(value, mode); SQLRETURN rc;
    if (wide) {
      auto input = rs::odbc::utf8_to_wide(text); ASSERT_TRUE(input); input->push_back(0);
      rc = SQLDriverConnectW(hdbc_, nullptr, input->data(), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    } else rc = SQLDriverConnect(hdbc_, nullptr, reinterpret_cast<SQLCHAR*>(text.data()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
    connected_ = rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO; ASSERT_TRUE(observed(rc));
    ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &hstmt_)));
    ASSERT_TRUE(observed(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &sibling_)));
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_NO_FATAL_FAILURE(cap_sibling());
  }
  void catalogs_with_sibling(bool wide, std::vector<std::string>& catalogs) {
    ASSERT_TRUE(cap(window_end_, true));
    if (wide) {
      SQLWCHAR all[]{'%', 0}, empty[]{0};
      ASSERT_TRUE(known_sql(SQLTablesW(hstmt_, all, SQL_NTS, empty, SQL_NTS, empty, SQL_NTS, nullptr, 0)));
    } else ASSERT_TRUE(known_sql(SQLTables(hstmt_, (SQLCHAR*)SQL_ALL_CATALOGS, SQL_NTS,
        (SQLCHAR*)"", SQL_NTS, (SQLCHAR*)"", SQL_NTS, nullptr, 0)));
    ASSERT_NO_FATAL_FAILURE(cap_sibling());
    ASSERT_TRUE(known_sql(SQLExecDirect(sibling_, (SQLCHAR*)"SELECT CAST(NULL AS INTEGER)", SQL_NTS)));
    SQLSMALLINT count = -1; ASSERT_TRUE(observed(SQLNumResultCols(hstmt_, &count))); ASSERT_EQ(5, count);
    for (;;) {
      ASSERT_TRUE(cap(window_end_, true)); const auto rc = SQLFetch(hstmt_);
      if (rc == SQL_NO_DATA) break;
      ASSERT_TRUE(observed(rc));
      if (catalogs.size() >= 64) { unknown_completion_ = true; FAIL() << "Catalog coverage exceeded admitted row bound"; }
      struct { SQLCHAR value[512]{}; SQLCHAR guard = 77; } cell;
      SQLLEN bytes = -1; ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_CHAR, cell.value, sizeof(cell.value), &bytes)));
      ASSERT_GT(bytes, 0); ASSERT_LT(bytes, static_cast<SQLLEN>(sizeof(cell.value)));
      EXPECT_EQ(77, cell.guard); EXPECT_EQ(0, cell.value[static_cast<std::size_t>(bytes)]);
      catalogs.emplace_back(reinterpret_cast<char*>(cell.value), static_cast<std::size_t>(bytes));
      for (SQLUSMALLINT column : {2, 3, 4, 5}) {
        SQLCHAR unchanged = 71; ASSERT_TRUE(cap(window_end_, true));
        ASSERT_TRUE(observed(SQLGetData(hstmt_, column, SQL_C_CHAR, &unchanged, 1, &bytes)));
        EXPECT_EQ(SQL_NULL_DATA, bytes); EXPECT_EQ(71, unchanged);
      }
    }
    EXPECT_TRUE(std::is_sorted(catalogs.begin(), catalogs.end()));
    EXPECT_EQ(catalogs.end(), std::adjacent_find(catalogs.begin(), catalogs.end()));
    EXPECT_NE(catalogs.end(), std::find(catalogs.begin(), catalogs.end(), "odbcpp_pilot"));
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLCloseCursor(hstmt_)));
    ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(observed(SQLFetch(sibling_)));
    SQLINTEGER null_value = 71; SQLLEN bytes = 97; ASSERT_NO_FATAL_FAILURE(cap_sibling());
    ASSERT_TRUE(observed(SQLGetData(sibling_, 1, SQL_C_SLONG, &null_value, sizeof(null_value), &bytes)));
    EXPECT_EQ(SQL_NULL_DATA, bytes); EXPECT_EQ(71, null_value);
    ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(observed(SQLFetch(sibling_), SQL_NO_DATA));
    ASSERT_NO_FATAL_FAILURE(cap_sibling()); ASSERT_TRUE(observed(SQLCloseCursor(sibling_)));
    // Empty explicit catalog is a no-match, never the connected database.
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(known_sql(SQLColumns(hstmt_, (SQLCHAR*)"", SQL_NTS, nullptr, 0, nullptr, 0, nullptr, 0)));
    ASSERT_TRUE(observed(SQLNumResultCols(hstmt_, &count))); EXPECT_EQ(18, count);
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLCloseCursor(hstmt_)));
  }
};

// Future finite admission: three verified TLS logins, at most nine SQL exchanges
// (three catalog enumerations, three fixed NULL SELECTs, three empty-catalog
// column queries), 40s aggregate/per-operation5s; no objects, DDL, DML or grants.
// FALSE can equal TRUE when authenticated sharing/external flags are off. This
// case proves native existing-principal grammar/lifecycle, not two-database
// completeness or privileged/denied visibility; those remain explicit debt.
TEST_F(RedshiftMetadataScopeRealTest, DefaultVisibleAndCurrentReconnectKeepCatalogNullSiblingAndEmptySemantics) {
  ASSERT_NO_FATAL_FAILURE(invalid_scope());
  std::vector<std::string> defaults, visible, current;
  ASSERT_NO_FATAL_FAILURE(open_scope(nullptr, false));
  ASSERT_NO_FATAL_FAILURE(catalogs_with_sibling(false, defaults));
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
  ASSERT_NO_FATAL_FAILURE(open_scope("FALSE", true));
  ASSERT_NO_FATAL_FAILURE(catalogs_with_sibling(true, visible));
  EXPECT_TRUE(std::includes(visible.begin(), visible.end(), defaults.begin(), defaults.end()));
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
  ASSERT_NO_FATAL_FAILURE(open_scope("TRUE", false));
  ASSERT_NO_FATAL_FAILURE(catalogs_with_sibling(false, current)); EXPECT_EQ(defaults, current);
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
}

// This conditional existing-principal case must have its own frozen admission.
// It never creates a second database/share/schema or expands privileges.
class RedshiftMetadataScopeShowRealTest : public RedshiftMetadataScopeRealTest {
 protected:
  std::string second_database_, common_schema_;
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_METADATA_SCOPE_SHOW_ADMISSION", "metadata_scope_two_visible_contexts_v1");
    if (IsSkipped() || HasFatalFailure()) return;
    const auto existing_name = [](const char* value) {
      return value && *value && std::strlen(value) <= 128 && rs::odbc::utf8_code_point_count(value).has_value();
    };
    const char* second = std::getenv("ODBCPP_REDSHIFT_METADATA_SCOPE_SECOND_DATABASE");
    const char* schema = std::getenv("ODBCPP_REDSHIFT_METADATA_SCOPE_COMMON_SCHEMA");
    ASSERT_TRUE(existing_name(second)); ASSERT_TRUE(existing_name(schema));
    second_database_ = second; common_schema_ = schema; ASSERT_NE("odbcpp_pilot", second_database_);
    // Explicit finite controls are prerequisites, not silently enlarged here.
    const auto options = rs::odbc::ConnectionString::parse(connection_string_);
    for (const auto& required : {std::pair{"MAXRESULTS", "3"}, std::pair{"MAXROWS", "128"},
                                std::pair{"MAXCELLS", "4096"}, std::pair{"MAXCOLUMNS", "18"}}) {
      ASSERT_TRUE(options.contains(required.first)); ASSERT_EQ(required.second, options.at(required.first));
    }
  }
};

// Source ledger: one TLS login, at most six SQL exchanges: catalog discovery1,
// fixed NULL SELECT1, empty-catalog columns1, database discovery1 and schemas2.
// Full catalog enumeration must prove exactly the two admitted visible DBs
// before schema fanout. Each result remains bounded; no source LIMIT/clipping.
// Existing common-schema collision is mandatory; 40s aggregate/per-operation5s.
TEST_F(RedshiftMetadataScopeShowRealTest, TwoVisibleDatabasesKeepCommonSchemaIdentityThroughWideFanoutAndSibling) {
  ASSERT_NO_FATAL_FAILURE(open_scope("FALSE", true, "SHOW"));
  std::vector<std::string> catalogs; ASSERT_NO_FATAL_FAILURE(catalogs_with_sibling(true, catalogs));
  std::vector<std::string> expected_catalogs{"odbcpp_pilot", second_database_};
  std::sort(expected_catalogs.begin(), expected_catalogs.end());
  if (catalogs != expected_catalogs) unknown_completion_ = true;
  ASSERT_TRUE(catalogs == expected_catalogs) << "Visible catalog prerequisite mismatch";
  ASSERT_TRUE(cap(window_end_, true));
  SQLWCHAR empty[]{0}, all[]{'%', 0};
  ASSERT_TRUE(known_sql(SQLTablesW(hstmt_, empty, SQL_NTS, all, SQL_NTS, empty, SQL_NTS, nullptr, 0)));
  std::set<std::pair<std::string, std::string>> identities;
  unsigned rows = 0;
  for (;;) {
    ASSERT_TRUE(cap(window_end_, true)); const auto rc = SQLFetch(hstmt_);
    if (rc == SQL_NO_DATA) break;
    ASSERT_TRUE(observed(rc));
    if (++rows > 64) { unknown_completion_ = true; FAIL() << "Schema coverage exceeded admitted row bound"; }
    std::string catalog, schema;
    for (SQLUSMALLINT column : {1, 2}) {
      struct { SQLCHAR value[512]{}; SQLCHAR guard = 77; } cell; SQLLEN bytes = -1;
      ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLGetData(hstmt_, column, SQL_C_CHAR, cell.value, sizeof(cell.value), &bytes)));
      ASSERT_GT(bytes, 0); ASSERT_LT(bytes, static_cast<SQLLEN>(sizeof(cell.value))); EXPECT_EQ(77, cell.guard);
      auto& name = column == 1 ? catalog : schema;
      name.assign(reinterpret_cast<char*>(cell.value), static_cast<std::size_t>(bytes));
    }
    EXPECT_TRUE(catalog == "odbcpp_pilot" || catalog == second_database_);
    EXPECT_TRUE(identities.emplace(catalog, schema).second);
    for (SQLUSMALLINT column : {3, 4, 5}) {
      SQLCHAR unchanged = 71; SQLLEN bytes = 97; ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLGetData(hstmt_, column, SQL_C_CHAR, &unchanged, 1, &bytes)));
      EXPECT_EQ(SQL_NULL_DATA, bytes); EXPECT_EQ(71, unchanged);
    }
  }
  EXPECT_TRUE(identities.contains({"odbcpp_pilot", common_schema_}));
  EXPECT_TRUE(identities.contains({second_database_, common_schema_}));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLCloseCursor(hstmt_)));
  ASSERT_NO_FATAL_FAILURE(release_keepalive());
  EXPECT_TRUE(identities.contains({second_database_, common_schema_}));
}

// Authoring only: one future selected admission, one TLS login and at most four
// SQL operations (fixed SELECT, Prepare, value Execute, NULL Execute). Reuses
// the existing 40s aggregate/per-operation5s/uncertain-completion lifecycle.
class RedshiftNegativeNumericScaleRealTest : public RedshiftLoginTimeoutOptionsRealTest {
 protected:
  static std::string numeric_sql() {
    return "WITH numbers AS ("
        "SELECT 1 AS ordinal, CAST(12000 AS DECIMAL(10,2)) AS n "
        "UNION ALL SELECT 2, CAST(-120.45 AS DECIMAL(10,2)) "
        "UNION ALL SELECT 3, CAST(123.45 AS DECIMAL(10,2)) "
        "UNION ALL SELECT 4, CAST(NULL AS DECIMAL(10,2)) "
        "UNION ALL SELECT 5, CAST(120.00 AS DECIMAL(10,2))) "
        "SELECT n, CAST(-120.45 AS DECIMAL(10,2)), "
        "CAST(-123.45 AS DECIMAL(10,2)), CAST(NULL AS DECIMAL(10,2)), "
        "CAST(42 AS INTEGER), CAST(123.45 AS DECIMAL(10,2)) "
        "FROM numbers ORDER BY ordinal";
  }
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_NEGATIVE_NUMERIC_SCALE_ADMISSION",
                   "negative_numeric_scale_v1");
    if (IsSkipped() || HasFatalFailure()) return;
    const auto options = rs::odbc::ConnectionString::parse(connection_string_);
    const auto minimum = [&](const char* key, std::size_t required) {
      const auto found = options.find(key);
      if (found == options.end()) return true;
      std::size_t value{};
      const auto& text = found->second;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value >= required;
    };
    ASSERT_TRUE(minimum("MAXSQLBYTES", numeric_sql().size()));
    ASSERT_TRUE(minimum("MAXPARAMETERS", 1));
    ASSERT_TRUE(minimum("MAXPARAMETERBYTES", 5));
    ASSERT_TRUE(minimum("MAXPARAMETERTOTALBYTES", 5));
    ASSERT_TRUE(minimum("MAXCOLUMNS", 6));
    ASSERT_TRUE(minimum("MAXROWS", 5));
    ASSERT_TRUE(minimum("MAXCELLS", 30));
    ASSERT_TRUE(minimum("MAXMETADATAENTRIES", 18));
  }
  bool observed(SQLRETURN result, SQLRETURN expected = SQL_SUCCESS,
                const char* expected_state = nullptr) {
    if (result != expected) { unknown_completion_ = true; return false; }
    if (expected_state) {
      SQLCHAR state[6]{};
      const auto diagnostic = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt_, 1,
          state, nullptr, nullptr, 0, nullptr);
      if (diagnostic != SQL_SUCCESS ||
          std::string_view(reinterpret_cast<char*>(state)) != expected_state) {
        unknown_completion_ = true; return false;
      }
    }
    return true;
  }
  SQLHDESC descriptor(SQLINTEGER attribute) {
    SQLHDESC result{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt_, attribute, &result, 0, nullptr));
    return result;
  }
  void dimensions(SQLHDESC desc, SQLSMALLINT column, SQLSMALLINT precision,
                  SQLSMALLINT scale, void* data = nullptr) {
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(desc, column, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(SQL_C_NUMERIC), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(desc, column, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(precision)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(desc, column, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(scale)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(desc, column, SQL_DESC_DATA_PTR, data, 0));
  }
  void small(const SQL_NUMERIC_STRUCT& value, SQLSMALLINT precision,
             SQLSMALLINT scale, unsigned coefficient, unsigned sign) {
    EXPECT_EQ(precision, value.precision); EXPECT_EQ(scale, value.scale);
    EXPECT_EQ(sign, value.sign); EXPECT_EQ(coefficient, value.val[0]);
    for (std::size_t i = 1; i < sizeof(value.val); ++i) EXPECT_EQ(0, value.val[i]);
  }
};

TEST_F(RedshiftNegativeNumericScaleRealTest, DescriptorLossNullOwnershipAndRecovery) {
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, false));
  std::array<unsigned char, sizeof(SQL_NUMERIC_STRUCT) + 2> output{};
  output.fill(0x5a);
  void* numeric_buffer = output.data() + 1;
  SQLLEN length = 73;
  SQLUSMALLINT status = SQL_ROW_NOROW;
  SQLULEN fetched = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_ROW_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt_, 1, SQL_C_NUMERIC, numeric_buffer, 0, &length));
  const auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
  ASSERT_NO_FATAL_FAILURE(dimensions(ard, 1, 2, -3, numeric_buffer));
  auto sql = numeric_sql();
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLExecDirect(hstmt_, reinterpret_cast<SQLCHAR*>(sql.data()), SQL_NTS)));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
  SQL_NUMERIC_STRUCT numeric{};
  std::memcpy(&numeric, numeric_buffer, sizeof(numeric));
  ASSERT_NO_FATAL_FAILURE(small(numeric, 2, -3, 12, 1));
  EXPECT_EQ(sizeof(numeric), static_cast<std::size_t>(length));
  EXPECT_EQ(SQL_ROW_SUCCESS, status); EXPECT_EQ(1u, fetched);
  ASSERT_NO_FATAL_FAILURE(dimensions(ard, 2, 2, -1));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_ARD_TYPE, &numeric, 0, &length),
      SQL_SUCCESS_WITH_INFO, "01S07"));
  ASSERT_NO_FATAL_FAILURE(small(numeric, 2, -1, 12, 0));
  ASSERT_NO_FATAL_FAILURE(dimensions(ard, 3, 5, -1));
  std::memset(&numeric, 0x5a, sizeof(numeric)); const auto poison = numeric; length = 73;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 3, SQL_ARD_TYPE, &numeric, 0, &length), SQL_ERROR, "22003"));
  EXPECT_EQ(73, length); EXPECT_EQ(0, std::memcmp(&poison, &numeric, sizeof(numeric)));
  ASSERT_NO_FATAL_FAILURE(dimensions(ard, 3, 5, 2));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 3, SQL_ARD_TYPE, &numeric, 0, &length)));
  EXPECT_EQ(5, numeric.precision); EXPECT_EQ(2, numeric.scale); EXPECT_EQ(0, numeric.sign);
  EXPECT_EQ(0x39, numeric.val[0]); EXPECT_EQ(0x30, numeric.val[1]);
  std::memset(&numeric, 0x5a, sizeof(numeric));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 4, SQL_C_NUMERIC, &numeric, 0, &length)));
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(0, std::memcmp(&poison, &numeric, sizeof(numeric)));
  SQLINTEGER neighbor{}; SQLLEN neighbor_length{};
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 5, SQL_C_SLONG, &neighbor, 0, &neighbor_length)));
  EXPECT_EQ(42, neighbor); EXPECT_EQ(sizeof(neighbor), static_cast<std::size_t>(neighbor_length));
  ASSERT_NO_FATAL_FAILURE(dimensions(ard, 6, 2, -3));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 6, SQL_C_NUMERIC, &numeric, 0, &length),
      SQL_SUCCESS_WITH_INFO, "01S07"));
  ASSERT_NO_FATAL_FAILURE(small(numeric, 38, 0, 123, 1));

  ASSERT_NO_FATAL_FAILURE(dimensions(ard, 1, 2, -1, numeric_buffer));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_SUCCESS_WITH_INFO, "01S07"));
  std::memcpy(&numeric, numeric_buffer, sizeof(numeric));
  ASSERT_NO_FATAL_FAILURE(small(numeric, 2, -1, 12, 0));
  EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO, status); EXPECT_EQ(1u, fetched);
  output.fill(0x5a); length = 73;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_ERROR, "22003"));
  EXPECT_EQ(SQL_ROW_ERROR, status); EXPECT_EQ(1u, fetched); EXPECT_EQ(73, length);
  for (const auto byte : output) EXPECT_EQ(0x5a, byte);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 5, SQL_C_SLONG, &neighbor, 0, &neighbor_length)));
  EXPECT_EQ(42, neighbor);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(SQL_ROW_SUCCESS, status);
  for (const auto byte : output) EXPECT_EQ(0x5a, byte);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
  std::memcpy(&numeric, numeric_buffer, sizeof(numeric));
  ASSERT_NO_FATAL_FAILURE(small(numeric, 2, -1, 12, 1));
  EXPECT_EQ(0x5a, output.front()); EXPECT_EQ(0x5a, output.back());
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt_, SQL_UNBIND));

  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLPrepare(hstmt_, (SQLCHAR*)"SELECT CAST(? AS DECIMAL(5,0)), CAST(42 AS INTEGER)", SQL_NTS)));
  SQL_NUMERIC_STRUCT input{};
  input.precision = 1; input.scale = 0; input.sign = 1; input.val[0] = 12;
  std::array<unsigned char, sizeof(input) + 2> input_storage{};
  input_storage.fill(0x5a); std::memcpy(input_storage.data() + 1, &input, sizeof(input));
  SQLLEN input_length = sizeof(input);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT, SQL_C_NUMERIC,
      SQL_NUMERIC, 5, 0, input_storage.data() + 1, sizeof(input), &input_length));
  const auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  ASSERT_NO_FATAL_FAILURE(dimensions(apd, 1, 2, -3, input_storage.data() + 1));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
  std::fill(input_storage.begin() + 1, input_storage.end() - 1, 0);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_NUMERIC, &numeric, 0, &length)));
  EXPECT_EQ(38, numeric.precision); EXPECT_EQ(0, numeric.scale); EXPECT_EQ(1, numeric.sign);
  EXPECT_EQ(0xe0, numeric.val[0]); EXPECT_EQ(0x2e, numeric.val[1]);
  for (std::size_t i = 2; i < sizeof(numeric.val); ++i) EXPECT_EQ(0, numeric.val[i]);
  EXPECT_EQ(sizeof(numeric), static_cast<std::size_t>(length));
  EXPECT_EQ(0x5a, input_storage.front()); EXPECT_EQ(0x5a, input_storage.back());
  EXPECT_EQ(sizeof(input), static_cast<std::size_t>(input_length));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  input_length = SQL_NULL_DATA;
  const auto untouched_input = input_storage;
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
  std::memset(&numeric, 0x5a, sizeof(numeric)); length = 73;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_NUMERIC, &numeric, 0, &length)));
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(SQL_NULL_DATA, input_length);
  EXPECT_EQ(0, std::memcmp(&poison, &numeric, sizeof(numeric))); EXPECT_EQ(untouched_input, input_storage);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_C_SLONG, &neighbor, 0, &neighbor_length)));
  EXPECT_EQ(42, neighbor);
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  ASSERT_NO_FATAL_FAILURE(release_connection());
}

class RedshiftNumericTargetRescaleRealTest : public RedshiftNegativeNumericScaleRealTest {
 protected:
  static constexpr const char* echo_sql =
      "SELECT CAST(? AS DECIMAL(4,2)), CAST(42 AS INTEGER)";
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_NUMERIC_TARGET_RESCALE_ADMISSION",
                   "numeric_target_rescale_v1");
    if (IsSkipped() || HasFatalFailure()) return;
    const auto options = rs::odbc::ConnectionString::parse(connection_string_);
    const std::pair<const char*, std::size_t> minima[] = {
        {"MAXSQLBYTES", std::string_view(echo_sql).size()},
        {"MAXCOLUMNS", 2}, {"MAXCELLS", 2}};
    for (const auto& [key, required] : minima) {
      const auto found = options.find(key);
      if (found == options.end()) continue;
      std::size_t value{};
      const auto& text = found->second;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      ASSERT_EQ(std::errc{}, parsed.ec);
      ASSERT_EQ(text.data() + text.size(), parsed.ptr);
      ASSERT_GE(value, required);
    }
  }
};

TEST_F(RedshiftNumericTargetRescaleRealTest, PreparedFractionWholeNullAndOwningRefill) {
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, false));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLPrepare(hstmt_, (SQLCHAR*)echo_sql, SQL_NTS)));
  SQL_NUMERIC_STRUCT input{};
  input.precision = 1; input.scale = 0; input.sign = 1;
  input.val[0] = 0x39; input.val[1] = 0x30;  // Independent coefficient12345.
  std::array<unsigned char, sizeof(input) + 2> storage{};
  SQLLEN input_length = sizeof(input);
  const auto bind = [&](SQLSMALLINT scale) {
    storage.fill(0x5a); std::memcpy(storage.data() + 1, &input, sizeof(input));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT,
        SQL_C_NUMERIC, SQL_DECIMAL, 4, 2, storage.data() + 1,
        sizeof(input), &input_length));
    ASSERT_NO_FATAL_FAILURE(dimensions(descriptor(SQL_ATTR_APP_PARAM_DESC),
        1, 5, scale, storage.data() + 1));
  };
  const auto echo = [&](const char* expected) {
    const auto before = storage;
    const auto before_length = input_length;
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
    EXPECT_EQ(before, storage); EXPECT_EQ(before_length, input_length);
    std::fill(storage.begin() + 1, storage.end() - 1, 0);
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
    std::array<unsigned char, 18> text{}; text.fill(0x5a); SQLLEN length = 73;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_CHAR, text.data() + 1, 16, &length)));
    if (expected) {
      EXPECT_EQ(static_cast<SQLLEN>(std::strlen(expected)), length);
      EXPECT_EQ(std::string(expected), reinterpret_cast<const char*>(text.data() + 1));
    } else {
      EXPECT_EQ(SQL_NULL_DATA, length);
      for (const auto byte : text) EXPECT_EQ(0x5a, byte);
    }
    EXPECT_EQ(0x5a, text.front()); EXPECT_EQ(0x5a, text.back());
    SQLINTEGER neighbor = -1; SQLLEN neighbor_length = 73;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_C_SLONG, &neighbor, 0, &neighbor_length)));
    EXPECT_EQ(42, neighbor); EXPECT_EQ(static_cast<SQLLEN>(sizeof(neighbor)), neighbor_length);
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  };
  ASSERT_NO_FATAL_FAILURE(bind(3));
  ASSERT_NO_FATAL_FAILURE(echo("12.34"));  // Server rounding unconverted12.345 would give12.35.
  ASSERT_NO_FATAL_FAILURE(bind(2));
  const auto overflow_input = storage;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLExecute(hstmt_), SQL_ERROR, "22003"));
  EXPECT_EQ(overflow_input, storage);
  input.sign = 0;
  ASSERT_NO_FATAL_FAILURE(bind(3));
  ASSERT_NO_FATAL_FAILURE(echo("-12.34"));
  input_length = SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(bind(-129));  // Poison unused C dimensions; NULL bypasses conversion.
  ASSERT_NO_FATAL_FAILURE(echo(nullptr));
  EXPECT_EQ(SQL_NULL_DATA, input_length);
  EXPECT_EQ(0x5a, storage.front()); EXPECT_EQ(0x5a, storage.back());
  ASSERT_NO_FATAL_FAILURE(release_connection());
}

class RedshiftCharacterNumericLossRealTest : public RedshiftNumericTargetRescaleRealTest {
 protected:
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_CHARACTER_NUMERIC_LOSS_ADMISSION",
                   "character_numeric_loss_v1");
    if (IsSkipped() || HasFatalFailure()) return;
    const auto options = rs::odbc::ConnectionString::parse(connection_string_);
    const std::pair<const char*, std::size_t> minima[] = {
        {"MAXSQLBYTES", std::string_view(echo_sql).size()},
        {"MAXCOLUMNS", 2}, {"MAXCELLS", 2}};
    for (const auto& [key, required] : minima) {
      const auto found = options.find(key);
      if (found == options.end()) continue;
      std::size_t value{};
      const auto& text = found->second;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      ASSERT_EQ(std::errc{}, parsed.ec);
      ASSERT_EQ(text.data() + text.size(), parsed.ptr); ASSERT_GE(value, required);
    }
  }
};

TEST_F(RedshiftCharacterNumericLossRealTest, AnsiWideLossRefusalNullAndPreparedRecovery) {
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, false));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLPrepare(hstmt_, (SQLCHAR*)echo_sql, SQL_NTS)));
  std::array<char, 12> narrow{};
  std::array<SQLWCHAR, 12> wide{};
  SQLLEN input_length = SQL_NTS;
  SQLUSMALLINT status = SQL_PARAM_UNUSED; SQLULEN processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  const auto bind = [&](const char* text, bool use_wide) {
    narrow.fill(0x5a); wide.fill(static_cast<SQLWCHAR>(0x5a));
    ASSERT_LT(std::strlen(text), 8u);
    std::memcpy(narrow.data() + 1, text, std::strlen(text) + 1);
    for (std::size_t i = 0; i <= std::strlen(text); ++i)
      wide[i + 1] = static_cast<SQLWCHAR>(text[i]);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, 1, SQL_PARAM_INPUT,
        use_wide ? SQL_C_WCHAR : SQL_C_CHAR, use_wide ? SQL_NUMERIC : SQL_DECIMAL,
        4, 2, use_wide ? static_cast<void*>(wide.data() + 1)
                       : static_cast<void*>(narrow.data() + 1),
        use_wide ? 8 * sizeof(SQLWCHAR) : 8, &input_length));
  };
  const auto echo = [&](const char* expected) {
    const auto before_narrow = narrow; const auto before_wide = wide;
    const auto before_length = input_length;
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
    EXPECT_EQ(before_narrow, narrow); EXPECT_EQ(before_wide, wide);
    EXPECT_EQ(before_length, input_length); EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    narrow.fill(0); wide.fill(0);
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
    std::array<unsigned char, 18> output{}; output.fill(0x5a); SQLLEN length = 73;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_CHAR, output.data() + 1, 16, &length)));
    if (expected) {
      EXPECT_EQ(static_cast<SQLLEN>(std::strlen(expected)), length);
      EXPECT_EQ(std::string(expected), reinterpret_cast<const char*>(output.data() + 1));
    } else {
      EXPECT_EQ(SQL_NULL_DATA, length);
      for (const auto byte : output) EXPECT_EQ(0x5a, byte);
    }
    EXPECT_EQ(0x5a, output.front()); EXPECT_EQ(0x5a, output.back());
    SQLINTEGER neighbor = -1; SQLLEN neighbor_length = 73;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_C_SLONG, &neighbor, 0, &neighbor_length)));
    EXPECT_EQ(42, neighbor); EXPECT_EQ(static_cast<SQLLEN>(sizeof(neighbor)), neighbor_length);
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  };
  ASSERT_NO_FATAL_FAILURE(bind("12.34", false));
  ASSERT_NO_FATAL_FAILURE(echo("12.34"));
  for (const auto* refused : {"123.45", "12.345"}) {
    for (const bool use_wide : {false, true}) {
      ASSERT_NO_FATAL_FAILURE(bind(refused, use_wide));
      const auto before_narrow = narrow; const auto before_wide = wide;
      status = SQL_PARAM_UNUSED; processed = 99;
      ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLExecute(hstmt_), SQL_ERROR, "22001"));
      EXPECT_EQ(before_narrow, narrow); EXPECT_EQ(before_wide, wide);
      EXPECT_EQ(SQL_NTS, input_length); EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    }
  }
  ASSERT_NO_FATAL_FAILURE(bind("-12.34", true));
  ASSERT_NO_FATAL_FAILURE(echo("-12.34"));
  input_length = SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(bind("invalid", true));
  ASSERT_NO_FATAL_FAILURE(echo(nullptr));
  EXPECT_EQ(SQL_NULL_DATA, input_length);
  ASSERT_NO_FATAL_FAILURE(release_connection());
}

class RedshiftFixedNumericCompletionRealTest : public RedshiftNegativeNumericScaleRealTest {
 protected:
  static constexpr const char* completion_sql =
      "SELECT CAST(12.345 AS DECIMAL(5,3)) AS n, "
      "CAST(-12.345 AS DECIMAL(5,3)) AS w, "
      "CAST('12.345' AS VARCHAR(6)) AS text_value, "
      "CAST(NULL AS DECIMAL(5,3)) AS absent, CAST(42 AS INTEGER) AS neighbor";
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_FIXED_NUMERIC_COMPLETION_ADMISSION",
                   "fixed_numeric_completion_v1");
    if (IsSkipped() || HasFatalFailure()) return;
    const auto options = rs::odbc::ConnectionString::parse(connection_string_);
    const std::pair<const char*, std::size_t> minima[] = {
        {"MAXSQLBYTES", std::string_view(completion_sql).size()},
        {"MAXCOLUMNS", 5}, {"MAXROWS", 1}, {"MAXCELLS", 5},
        {"MAXMETADATAENTRIES", 5}};
    for (const auto& [key, required] : minima) {
      const auto found = options.find(key);
      if (found == options.end()) continue;
      std::size_t value{};
      const auto& text = found->second;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      ASSERT_EQ(std::errc{}, parsed.ec);
      ASSERT_EQ(text.data() + text.size(), parsed.ptr);
      ASSERT_GE(value, required);
    }
  }
  void completed(SQLUSMALLINT column, SQLSMALLINT target = SQL_C_CHAR) {
    std::array<unsigned char, 64> output{};
    output.fill(0x5a);
    const auto poison = output;
    SQLLEN length = 73;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, column, target,
        output.data(), output.size(), &length), SQL_NO_DATA));
    EXPECT_EQ(poison, output);
    EXPECT_EQ(73, length);
  }
};

TEST_F(RedshiftFixedNumericCompletionRealTest, FractionWholeNullAndVariableSourceCompletion) {
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, false));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLExecDirect(hstmt_, (SQLCHAR*)completion_sql, SQL_NTS)));
  SQLSMALLINT columns = -1;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt_, &columns));
  ASSERT_EQ(5, columns);
  for (const SQLUSMALLINT column : {SQLUSMALLINT{1}, SQLUSMALLINT{2}, SQLUSMALLINT{4}}) {
    SQLCHAR name[16]{};
    SQLSMALLINT name_length{}, type{}, scale{}, nullable{};
    SQLULEN size{};
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt_, column, name, sizeof(name),
        &name_length, &type, &size, &scale, &nullable));
    ASSERT_TRUE(type == SQL_NUMERIC || type == SQL_DECIMAL);
    EXPECT_EQ(5u, size);
    EXPECT_EQ(3, scale);
  }
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLFetch(hstmt_)));

  std::array<unsigned char, 18> narrow{};
  narrow.fill(0x5a);
  const auto narrow_poison = narrow;
  SQLLEN length = 73;
  for (const SQLLEN capacity : {SQLLEN{0}, SQLLEN{2}}) {
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_CHAR,
        narrow.data() + 1, capacity, &length), SQL_ERROR, "22003"));
    EXPECT_EQ(narrow_poison, narrow);
    EXPECT_EQ(73, length);
  }
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_CHAR,
      narrow.data() + 1, 5, &length), SQL_SUCCESS_WITH_INFO, "01004"));
  EXPECT_STREQ("12.3", reinterpret_cast<const char*>(narrow.data() + 1));
  EXPECT_EQ(6, length);
  EXPECT_EQ(0x5a, narrow.front());
  EXPECT_EQ(0x5a, narrow[6]);
  ASSERT_NO_FATAL_FAILURE(completed(1));

  const auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_WCHAR), 0));
  std::array<unsigned char, 6 * sizeof(SQLWCHAR) + 2> wide{};
  wide.fill(0x5a);
  const auto wide_poison = wide;
  length = 73;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_ARD_TYPE,
      wide.data() + 1, 3 * sizeof(SQLWCHAR), &length), SQL_ERROR, "22003"));
  EXPECT_EQ(wide_poison, wide);
  EXPECT_EQ(73, length);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_ARD_TYPE,
      wide.data() + 1, 6 * sizeof(SQLWCHAR), &length),
      SQL_SUCCESS_WITH_INFO, "01004"));
  const std::array<SQLWCHAR, 6> expected{
      static_cast<SQLWCHAR>('-'), static_cast<SQLWCHAR>('1'),
      static_cast<SQLWCHAR>('2'), static_cast<SQLWCHAR>('.'),
      static_cast<SQLWCHAR>('3'), 0};
  EXPECT_EQ(0, std::memcmp(wide.data() + 1, expected.data(), sizeof(expected)));
  EXPECT_EQ(static_cast<SQLLEN>(7 * sizeof(SQLWCHAR)), length);
  EXPECT_EQ(0x5a, wide.front());
  EXPECT_EQ(0x5a, wide.back());
  ASSERT_NO_FATAL_FAILURE(completed(2, SQL_ARD_TYPE));
  SQLSMALLINT concise_type{};
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ard, 2, SQL_DESC_CONCISE_TYPE,
      &concise_type, 0, nullptr));
  EXPECT_EQ(SQL_C_WCHAR, concise_type);

  narrow.fill(0x5a);
  length = 73;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 3, SQL_C_CHAR,
      narrow.data() + 1, 5, &length), SQL_SUCCESS_WITH_INFO, "01004"));
  EXPECT_STREQ("12.3", reinterpret_cast<const char*>(narrow.data() + 1));
  EXPECT_EQ(6, length);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 3, SQL_C_CHAR,
      narrow.data() + 1, 3, &length)));
  EXPECT_STREQ("45", reinterpret_cast<const char*>(narrow.data() + 1));
  EXPECT_EQ(2, length);
  EXPECT_EQ(0x5a, narrow.front());
  EXPECT_EQ(0x5a, narrow[6]);
  ASSERT_NO_FATAL_FAILURE(completed(3));

  narrow.fill(0x5a);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 4, SQL_C_CHAR,
      narrow.data() + 1, 0, nullptr), SQL_ERROR, "22002"));
  EXPECT_EQ(narrow_poison, narrow);
  length = 73;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 4, SQL_C_CHAR,
      narrow.data() + 1, 0, &length)));
  EXPECT_EQ(SQL_NULL_DATA, length);
  EXPECT_EQ(narrow_poison, narrow);
  ASSERT_NO_FATAL_FAILURE(completed(4));

  SQLINTEGER neighbor = -1;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 5, SQL_C_SLONG,
      &neighbor, 0, &length)));
  EXPECT_EQ(42, neighbor);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(neighbor)), length);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  ASSERT_NO_FATAL_FAILURE(release_connection());
}

// Authoring only: one TLS login, one Prepare and at most seven Execute
// attempts (five fits/NULL and two expected local22003 refusals). Conservatively
// charge all eight operations; no DDL/DML/resources. Existing40s/per-op5s caps.
class RedshiftCnumericApproximateRangeRealTest : public RedshiftNegativeNumericScaleRealTest {
 protected:
  static constexpr const char* echo_sql =
      "SELECT CAST(? AS REAL), CAST(? AS DOUBLE PRECISION), "
      "CAST(? AS DOUBLE PRECISION), CAST(42 AS INTEGER)";
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_CNUMERIC_APPROXIMATE_RANGE_ADMISSION",
                   "cnumeric_approximate_range_v1");
    if (IsSkipped() || HasFatalFailure()) return;
    const auto options = rs::odbc::ConnectionString::parse(connection_string_);
    const std::pair<const char*, std::size_t> minima[] = {
        {"MAXSQLBYTES", std::string_view(echo_sql).size()},
        {"MAXPARAMETERS", 3}, {"MAXPARAMETERBYTES", 102},
        {"MAXPARAMETERTOTALBYTES", 306}, {"MAXCOLUMNS", 4},
        {"MAXROWS", 1}, {"MAXCELLS", 4}, {"MAXMETADATAENTRIES", 12}};
    for (const auto& [key, required] : minima) {
      const auto found = options.find(key);
      if (found == options.end()) continue;
      std::size_t value{};
      const auto& text = found->second;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      ASSERT_EQ(std::errc{}, parsed.ec);
      ASSERT_EQ(text.data() + text.size(), parsed.ptr);
      ASSERT_GE(value, required);
    }
  }
};

TEST_F(RedshiftCnumericApproximateRangeRealTest, PreparedFitSubnormalWholeNullAndRecovery) {
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, false));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLPrepare(hstmt_, (SQLCHAR*)echo_sql, SQL_NTS)));
  std::array<std::array<unsigned char, sizeof(SQL_NUMERIC_STRUCT) + 2>, 3> storage{};
  std::array<SQLLEN, 3> indicators{73, 74, 75};
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  SQLULEN processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt_, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  const auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  const auto bind = [&](std::size_t slot, SQLSMALLINT target,
                        SQLSMALLINT precision, SQLSMALLINT scale,
                        SQLCHAR coefficient, SQLCHAR sign) {
    SQL_NUMERIC_STRUCT input{};
    input.precision = 1; input.scale = 0; input.sign = sign; input.val[0] = coefficient;
    storage[slot].fill(0x5a);
    std::memcpy(storage[slot].data() + 1, &input, sizeof(input));
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt_, static_cast<SQLUSMALLINT>(slot + 1),
        SQL_PARAM_INPUT, SQL_C_NUMERIC, target, 0, 0, storage[slot].data() + 1,
        sizeof(input), &indicators[slot]));
    ASSERT_NO_FATAL_FAILURE(dimensions(apd, static_cast<SQLSMALLINT>(slot + 1),
        precision, scale, storage[slot].data() + 1));
  };
  ASSERT_NO_FATAL_FAILURE(bind(1, SQL_FLOAT, 1, -100, 1, 1));
  ASSERT_NO_FATAL_FAILURE(bind(2, SQL_DOUBLE, 1, -100, 1, 0));
  const auto echo = [&](SQLDOUBLE expected, bool subnormal, bool absent) {
    const auto before = storage;
    const auto before_indicators = indicators;
    status = SQL_PARAM_UNUSED; processed = 99;
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLExecute(hstmt_)));
    EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(before, storage); EXPECT_EQ(before_indicators, indicators);
    // Input pointers are unaligned and may change after execution. Results own values.
    std::fill(storage[0].begin() + 1, storage[0].end() - 1, 0);
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
    SQLDOUBLE real = 97; SQLLEN length = 73;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_DOUBLE, &real, 0, &length)));
    if (absent) {
      EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(97, real);
    } else {
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(real)), length);
      if (subnormal) {
        EXPECT_GT(real, 9e-39); EXPECT_LT(real, 1.1e-38);
      } else {
        EXPECT_DOUBLE_EQ(expected, real);
      }
    }
    for (SQLUSMALLINT col = 2; col <= 3; ++col) {
      SQLDOUBLE large = 0; length = 73;
      ASSERT_TRUE(cap(window_end_, true));
      ASSERT_TRUE(observed(SQLGetData(hstmt_, col, SQL_C_DOUBLE, &large, 0, &length)));
      EXPECT_DOUBLE_EQ(col == 2 ? 1e100 : -1e100, large);
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(large)), length);
    }
    SQLINTEGER neighbor = -1; length = 73;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 4, SQL_C_SLONG, &neighbor, 0, &length)));
    EXPECT_EQ(42, neighbor); EXPECT_EQ(static_cast<SQLLEN>(sizeof(neighbor)), length);
    for (const auto& cell : storage) { EXPECT_EQ(0x5a, cell.front()); EXPECT_EQ(0x5a, cell.back()); }
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
    ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  };
  ASSERT_NO_FATAL_FAILURE(bind(0, SQL_REAL, 1, 0, 1, 1));
  ASSERT_NO_FATAL_FAILURE(echo(1, false, false));
  ASSERT_NO_FATAL_FAILURE(bind(0, SQL_REAL, 38, 38, 1, 1));
  ASSERT_NO_FATAL_FAILURE(echo(0, true, false));
  for (const SQLCHAR sign : {SQLCHAR{1}, SQLCHAR{0}}) {
    ASSERT_NO_FATAL_FAILURE(bind(0, SQL_REAL, 1, -100, 1, sign));
    const auto before = storage; const auto before_indicators = indicators;
    status = SQL_PARAM_UNUSED; processed = 99;
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLExecute(hstmt_), SQL_ERROR, "22003"));
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(before, storage); EXPECT_EQ(before_indicators, indicators);
  }
  ASSERT_NO_FATAL_FAILURE(bind(0, SQL_REAL, 2, 0, 42, 1));
  ASSERT_NO_FATAL_FAILURE(echo(42, false, false));
  indicators[0] = SQL_NULL_DATA;
  std::fill(storage[0].begin() + 1, storage[0].end() - 1, 0xff);
  ASSERT_NO_FATAL_FAILURE(echo(0, false, true));
  indicators[0] = 73;
  ASSERT_NO_FATAL_FAILURE(bind(0, SQL_REAL, 1, 0, 1, 0));
  ASSERT_NO_FATAL_FAILURE(echo(-1, false, false));
  ASSERT_NO_FATAL_FAILURE(release_connection());
}

// Authored only: 1 verified TLS login and 1 fixed SELECT, no Prepare/DDL/DML.
// Existing 40-second aggregate and <=5-second operation caps remain mandatory.
class RedshiftFixedTemporalCompletionRealTest : public RedshiftFixedNumericCompletionRealTest {
 protected:
  static constexpr const char* temporal_sql =
      "SELECT CAST('12:34:56.789' AS TIME) AS t, "
      "CAST('2026-10-12 12:34:56.789' AS TIMESTAMP) AS s, "
      "CAST('12:34:56.789' AS VARCHAR(12)) AS text_value, "
      "CAST(NULL AS TIME) AS absent, CAST(42 AS INTEGER) AS neighbor";
  void SetUp() override {
    setup_admitted("ODBCPP_REDSHIFT_FIXED_TEMPORAL_COMPLETION_ADMISSION",
                   "fixed_temporal_completion_v1");
    if (IsSkipped() || HasFatalFailure()) return;
    const auto options = rs::odbc::ConnectionString::parse(connection_string_);
    const std::pair<const char*, std::size_t> minima[] = {
        {"MAXSQLBYTES", std::string_view(temporal_sql).size()},
        {"MAXCOLUMNS", 5}, {"MAXROWS", 1}, {"MAXCELLS", 5},
        {"MAXMETADATAENTRIES", 5}};
    for (const auto& [key, required] : minima) {
      const auto found = options.find(key);
      if (found == options.end()) continue;
      std::size_t value{};
      const auto& text = found->second;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
      ASSERT_EQ(std::errc{}, parsed.ec);
      ASSERT_EQ(text.data() + text.size(), parsed.ptr);
      ASSERT_GE(value, required);
    }
  }
};

TEST_F(RedshiftFixedTemporalCompletionRealTest, FractionWholeNullWideAndVariableRecovery) {
  ASSERT_NO_FATAL_FAILURE(login("LOGINTIMEOUT", "5", 5, false));
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(known_sql(SQLExecDirect(hstmt_, (SQLCHAR*)temporal_sql, SQL_NTS)));
  SQLSMALLINT columns = -1;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt_, &columns)); ASSERT_EQ(5, columns);
  for (const SQLUSMALLINT column : {SQLUSMALLINT{1}, SQLUSMALLINT{2}, SQLUSMALLINT{4}}) {
    SQLCHAR name[16]{}; SQLSMALLINT name_length{}, type{}, scale{}, nullable{}; SQLULEN size{};
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt_, column, name, sizeof(name),
        &name_length, &type, &size, &scale, &nullable));
    EXPECT_EQ(column == 2 ? SQL_TYPE_TIMESTAMP : SQL_TYPE_TIME, type);
  }
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_)));
  std::array<unsigned char, 66> narrow{}; narrow.fill(0x5a);
  const auto narrow_poison = narrow; SQLLEN length = 73;
  for (const SQLLEN capacity : {SQLLEN{0}, SQLLEN{8}}) {
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_CHAR,
        narrow.data() + 1, capacity, &length), SQL_ERROR, "22003"));
    EXPECT_EQ(narrow_poison, narrow); EXPECT_EQ(73, length);
  }
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_CHAR,
      narrow.data() + 1, 9, &length), SQL_SUCCESS_WITH_INFO, "01004"));
  EXPECT_STREQ("12:34:56", reinterpret_cast<const char*>(narrow.data() + 1));
  EXPECT_EQ(12, length); EXPECT_EQ(0x5a, narrow.front()); EXPECT_EQ(0x5a, narrow[10]);
  ASSERT_NO_FATAL_FAILURE(completed(1));

  ASSERT_TRUE(cap(window_end_, true)); const auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_WCHAR), 0));
  std::array<unsigned char, 25 * sizeof(SQLWCHAR) + 3> wide{}; wide.fill(0x5a);
  const auto wide_poison = wide; length = 73;
  for (const SQLLEN capacity : {SQLLEN{sizeof(SQLWCHAR) - 1}, SQLLEN{19 * sizeof(SQLWCHAR)}}) {
    ASSERT_TRUE(cap(window_end_, true));
    ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_ARD_TYPE,
        wide.data() + 1, capacity, &length), SQL_ERROR, "22003"));
    EXPECT_EQ(wide_poison, wide); EXPECT_EQ(73, length);
  }
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_ARD_TYPE,
      wide.data() + 1, 20 * sizeof(SQLWCHAR) + 1, &length), SQL_SUCCESS_WITH_INFO, "01004"));
  std::array<SQLWCHAR, 24> expected{};
  constexpr const char* timestamp = "2026-10-12 12:34:56.789";
  for (std::size_t i = 0; i < 19; ++i) expected[i] = static_cast<SQLWCHAR>(timestamp[i]);
  EXPECT_EQ(0, std::memcmp(wide.data() + 1, expected.data(), 20 * sizeof(SQLWCHAR)));
  EXPECT_EQ(static_cast<SQLLEN>(23 * sizeof(SQLWCHAR)), length);
  EXPECT_EQ(0x5a, wide.front()); EXPECT_EQ(0x5a, wide[20 * sizeof(SQLWCHAR) + 1]);
  EXPECT_EQ(0x5a, wide.back()); ASSERT_NO_FATAL_FAILURE(completed(2, SQL_ARD_TYPE));
  SQLSMALLINT concise{}; ASSERT_TRUE(cap(window_end_, true));
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ard, 2, SQL_DESC_CONCISE_TYPE, &concise, 0, nullptr));
  EXPECT_EQ(SQL_C_WCHAR, concise);

  narrow.fill(0x5a); length = 73;
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 3, SQL_C_CHAR,
      narrow.data() + 1, 9, &length), SQL_SUCCESS_WITH_INFO, "01004"));
  EXPECT_STREQ("12:34:56", reinterpret_cast<const char*>(narrow.data() + 1)); EXPECT_EQ(12, length);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 3, SQL_C_CHAR, narrow.data() + 1, 5, &length)));
  EXPECT_STREQ(".789", reinterpret_cast<const char*>(narrow.data() + 1)); EXPECT_EQ(4, length);
  ASSERT_NO_FATAL_FAILURE(completed(3));
  narrow.fill(0x5a); ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 4, SQL_C_CHAR,
      narrow.data() + 1, 0, nullptr), SQL_ERROR, "22002")); EXPECT_EQ(narrow_poison, narrow);
  length = 73; ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 4, SQL_C_CHAR, narrow.data() + 1, 0, &length)));
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(narrow_poison, narrow);
  ASSERT_NO_FATAL_FAILURE(completed(4));
  SQLINTEGER neighbor = -1; ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 5, SQL_C_SLONG, &neighbor, 0, &length)));
  EXPECT_EQ(42, neighbor); EXPECT_EQ(static_cast<SQLLEN>(sizeof(neighbor)), length);

  // A different column restarts the selected column; now return the full value.
  wide.fill(0x5a); expected.fill(0);
  for (std::size_t i = 0; i < 23; ++i) expected[i] = static_cast<SQLWCHAR>(timestamp[i]);
  ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 2, SQL_ARD_TYPE,
      wide.data() + 1, 24 * sizeof(SQLWCHAR), &length)));
  EXPECT_EQ(0, std::memcmp(wide.data() + 1, expected.data(), sizeof(expected)));
  EXPECT_EQ(static_cast<SQLLEN>(23 * sizeof(SQLWCHAR)), length);
  EXPECT_EQ(0x5a, wide.front()); EXPECT_EQ(0x5a, wide.back());
  ASSERT_NO_FATAL_FAILURE(completed(2, SQL_ARD_TYPE));
  narrow.fill(0x5a); ASSERT_TRUE(cap(window_end_, true));
  ASSERT_TRUE(observed(SQLGetData(hstmt_, 1, SQL_C_CHAR, narrow.data() + 1, 13, &length)));
  EXPECT_STREQ("12:34:56.789", reinterpret_cast<const char*>(narrow.data() + 1)); EXPECT_EQ(12, length);
  ASSERT_NO_FATAL_FAILURE(completed(1));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(observed(SQLFetch(hstmt_), SQL_NO_DATA));
  ASSERT_TRUE(cap(window_end_, true)); ASSERT_TRUE(known_sql(SQLCloseCursor(hstmt_)));
  ASSERT_NO_FATAL_FAILURE(release_connection());
}
