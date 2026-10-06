#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/connection_string.h"
#include "odbcpp/database/backend_provider.h"
#include "core/database/postgres/pg_database_connection.h"
#include "core/database/postgres/redshift_primary_key_contract.h"
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
TEST_F(RedshiftRealTest, IntegerBoundariesAndNarrowing) {
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
  SQLSMALLINT sentinel = 17; SQLLEN length = 93;
  EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt_, 3, SQL_C_SSHORT,
      &sentinel, sizeof(sentinel), &length));
  EXPECT_EQ("22003", get_error(SQL_HANDLE_STMT, hstmt_));
  EXPECT_EQ(17, sentinel);
  EXPECT_EQ(93, length);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt_));
}

TEST_F(RedshiftRealTest, ExactDecimalAndNull) {
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
  enum class DescriptorScope { Tables, AllSchemas, Columns };
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
      std::transform(actual.begin(),actual.end(),actual.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
      EXPECT_TRUE(actual==fields[column-1].name);EXPECT_EQ(fields[column-1].type,type);
      EXPECT_EQ(0,digits);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
      if(type==SQL_SMALLINT) { EXPECT_EQ(5U,size); }
      else if(type==SQL_INTEGER) { EXPECT_EQ(10U,size); }
      else if(type==SQL_VARCHAR) {
        if(scope==DescriptorScope::AllSchemas) {
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
    ASSERT_EQ(SQL_SUCCESS,SQLTables(hstmt_,empty,SQL_NTS,all_schemas,SQL_NTS,empty,SQL_NTS,nullptr,0)) << get_error(SQL_HANDLE_STMT,hstmt_);
    descriptors(tables,DescriptorScope::AllSchemas);ASSERT_FALSE(HasFailure());
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
