#include "tests/support/redshift_scripted_wire.h"

#include <sql.h>
#include <sqlext.h>

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace rs::core::database;
using namespace rs::tests::redshift_wire;

// Helper to construct a wire ErrorResponse ('E') frame with severity, SQLSTATE, and message.
std::string error_frame(std::string_view sqlstate, std::string_view message) {
  std::string body;
  body += 'S'; body += "ERROR"; body.push_back('\0');
  body += 'C'; body += sqlstate; body.push_back('\0');
  body += 'M'; body += message; body.push_back('\0');
  body.push_back('\0');
  return frame('E', body);
}

// Scripted provider for the PostgreSQL profile.
class ScriptedPostgresProvider final : public IBackendProvider {
 public:
  explicit ScriptedPostgresProvider(std::shared_ptr<WireScript> script)
      : script_(std::move(script)),
        product_(BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
                 BackendConnectionDefaults{"localhost", 5432, std::nullopt, true},
                 std::nullopt, postgres::PgCatalogProfile::PostgreSQL) {}

  const BackendIdentity& identity() const noexcept override { return product_.identity(); }
  const BackendConnectionDefaults& connection_defaults() const noexcept override {
    return product_.connection_defaults();
  }
  const ISqlDialect& sql_dialect() const noexcept override { return product_.sql_dialect(); }
  BackendCapabilities capabilities() const noexcept override { return product_.capabilities(); }
  std::optional<std::string> normalize_error_sqlstate(
      std::string_view state, ErrorContext context) const override {
    return product_.normalize_error_sqlstate(state, context);
  }
  std::span<const TypeDefinition> type_catalog(std::string_view version = {}) const noexcept override {
    return product_.type_catalog(version);
  }
  std::span<const TypeDefinition> result_type_catalog(
      std::string_view version = {}) const noexcept override {
    return product_.result_type_catalog(version);
  }
  TransactionCapabilities transaction_capabilities() const noexcept override {
    return product_.transaction_capabilities();
  }
  rs::util::Result<ConnectionSettings> resolve_connection_options(
      ConnectionOptions options) const override {
    return product_.resolve_connection_options(std::move(options));
  }
  std::unique_ptr<IDatabaseConnection> create_session(
      std::unique_ptr<rs::core::transport::ITransport>) const override {
    return std::make_unique<postgres::PgDatabaseConnection>(
        std::make_unique<RedshiftWire>(script_), std::nullopt,
        postgres::PgCatalogProfile::PostgreSQL);
  }

 private:
  std::shared_ptr<WireScript> script_;
  postgres::PgBackendProvider product_;
};

// Test fixture parametrized by whether Redshift profile (true) or Postgres profile (false) is used.
class PreparedBatchSyntaxDiagnosticsTest : public ::testing::TestWithParam<bool> {
 protected:
  std::shared_ptr<WireScript> script = std::make_shared<WireScript>();
  SQLHENV env{};
  SQLHDBC dbc{};
  SQLHSTMT stmt{};

  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &env));
    ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
        reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));

    std::shared_ptr<IBackendProvider> provider;
    if (GetParam()) {
      provider = std::make_shared<ScriptedRedshiftProvider>(script);
    } else {
      provider = std::make_shared<ScriptedPostgresProvider>(script);
    }

    auto connection = std::make_unique<rs::odbc::ODBCConnection>(nullptr, std::move(provider));
    dbc = reinterpret_cast<SQLHDBC>(connection.get());
    rs::odbc::HandleRegistry::instance().register_handle(dbc, std::move(connection), env);

    SQLCHAR input[] = "SERVER=fake;DATABASE=test;UID=test;SSL=0";
    ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr, input, SQL_NTS, nullptr, 0,
                                            nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  }

  void TearDown() override {
    if (stmt) {
      SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    }
    if (dbc) {
      SQLDisconnect(dbc);
      SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    }
    if (env) {
      SQLFreeHandle(SQL_HANDLE_ENV, env);
    }
  }

  std::pair<std::string, std::string> diagnostic() {
    SQLCHAR sqlstate[6]{};
    SQLCHAR message[1024]{};
    SQLINTEGER native_error = 0;
    SQLSMALLINT length = 0;
    const auto rc = SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, sqlstate, &native_error,
                                  message, sizeof(message), &length);
    if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
      return {"", ""};
    }
    return {reinterpret_cast<char*>(sqlstate), reinterpret_cast<char*>(message)};
  }
};

INSTANTIATE_TEST_SUITE_P(Profiles, PreparedBatchSyntaxDiagnosticsTest, ::testing::Bool(),
    [](const ::testing::TestParamInfo<bool>& info) {
      return info.param ? "Redshift" : "Postgres";
    });

TEST_P(PreparedBatchSyntaxDiagnosticsTest, ParameterFreePreparedBatchPreservesSyntaxError42601) {
  // Wire responds with standard PostgreSQL/Redshift ErrorResponse for prepared multi-statement batch
  const std::string sql = "SELECT 11; SELECT 22";
  script->responses[sql] = error_frame("42601", "cannot insert multiple commands into a prepared statement");

  // Step 1: Default SQLPrepare defers server dispatch (0 network wire operations).
  const auto prepare_count_before = script->received_sql.size();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS));
  EXPECT_EQ(prepare_count_before, script->received_sql.size());

  // Step 2: SQLExecute dispatches the prepared statement to the wire and fails.
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));

  // Step 3: Diagnostic verification: must preserve 42601 rather than collapsing to generic 42000.
  const auto [state, msg] = diagnostic();
  EXPECT_EQ("42601", state);
  EXPECT_NE(msg.find("cannot insert multiple commands into a prepared statement"), std::string::npos);

  // Step 4: Proving statement handle cleanup and recovery:
  // Freeing statement resources succeeds.
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));

  // Step 5: Proving a valid single-statement operation works afterward on the same statement handle.
  script->responses["SELECT 11"] = row_description({{"val", 23}}) + data_row({{"11"}}) + complete("SELECT 1");
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 11")), SQL_NTS));
  SQLCHAR buf[32]{};
  SQLLEN len = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, buf, sizeof(buf), &len));
  EXPECT_STREQ("11", reinterpret_cast<char*>(buf));
}

TEST_P(PreparedBatchSyntaxDiagnosticsTest, ParameterMarkerPreparedBatchPreserves42601AndRecovers) {
  const std::string sql = "SELECT CAST(? AS INTEGER); SELECT CAST(? AS INTEGER)";
  // Parser replaces ? markers with $1 and $2 for backend wire dispatch
  const std::string wire_sql = "SELECT CAST($1 AS INTEGER); SELECT CAST($2 AS INTEGER)";
  script->responses[wire_sql] = error_frame("42601", "cannot insert multiple commands into a prepared statement");

  // Default prepare succeeds locally with 0 network calls
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS));

  // Binding parameters
  SQLINTEGER p1 = 31, p2 = 42;
  SQLLEN len1 = sizeof(p1), len2 = sizeof(p2);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &p1, 0, &len1));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &p2, 0, &len2));

  // Execution fails with 42601
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
  const auto [state, msg] = diagnostic();
  EXPECT_EQ("42601", state);
  EXPECT_NE(msg.find("cannot insert multiple commands into a prepared statement"), std::string::npos);

  // Proving parameter and statement cleanup
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_RESET_PARAMS));

  // Proving single-statement execution succeeds afterward on the same statement
  script->responses["SELECT 31"] = row_description({{"val", 23}}) + data_row({{"31"}}) + complete("SELECT 1");
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 31")), SQL_NTS));

  SQLCHAR buf[32]{};
  SQLLEN len = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, buf, sizeof(buf), &len));
  EXPECT_STREQ("31", reinterpret_cast<char*>(buf));
}

TEST_P(PreparedBatchSyntaxDiagnosticsTest, DeferredPrepareSurfaces42601AtMetadataDescriptionTime) {
  // Proves timing point 2: In default deferred prepare mode, SQLPrepare succeeds locally,
  // but explicit parameter description (SQLDescribeParam) forces server description over the wire
  // and surfaces 42601 at metadata description time before SQLExecute is ever called.
  const std::string sql = "SELECT CAST(? AS INTEGER); SELECT CAST(? AS INTEGER)";
  const std::string wire_sql = "SELECT CAST($1 AS INTEGER); SELECT CAST($2 AS INTEGER)";
  script->responses[wire_sql] = error_frame("42601", "cannot insert multiple commands into a prepared statement");

  const auto prepare_count_before = script->received_sql.size();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS));
  EXPECT_EQ(prepare_count_before, script->received_sql.size());

  // Requesting parameter metadata forces server description and surfaces 42601
  SQLSMALLINT data_type = 0;
  SQLULEN parameter_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = 0;
  EXPECT_EQ(SQL_ERROR, SQLDescribeParam(stmt, 1, &data_type, &parameter_size, &decimal_digits, &nullable));

  const auto [state, msg] = diagnostic();
  EXPECT_EQ("42601", state);
  EXPECT_NE(msg.find("cannot insert multiple commands into a prepared statement"), std::string::npos);

  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
}

TEST_P(PreparedBatchSyntaxDiagnosticsTest, EagerAutoIpdFailsAtPrepareTimeWith42601) {
  // Proves timing point 3: With SQL_ATTR_ENABLE_AUTO_IPD enabled, SQLPrepare eagerly requests
  // statement description over the wire, surfacing 42601 synchronously at prepare time.
  const std::string sql = "SELECT CAST(? AS INTEGER); SELECT CAST(? AS INTEGER)";
  const std::string wire_sql = "SELECT CAST($1 AS INTEGER); SELECT CAST($2 AS INTEGER)";
  script->responses[wire_sql] = error_frame("42601", "cannot insert multiple commands into a prepared statement");

  // Enable automatic IPD population, forcing eager statement description at prepare time
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ENABLE_AUTO_IPD,
                                        reinterpret_cast<SQLPOINTER>(SQL_TRUE), 0));

  // SQLPrepare issues description over the wire and fails immediately
  EXPECT_EQ(SQL_ERROR, SQLPrepare(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS));
  const auto [state, msg] = diagnostic();
  EXPECT_EQ("42601", state);
  EXPECT_NE(msg.find("cannot insert multiple commands into a prepared statement"), std::string::npos);

  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
}

TEST_P(PreparedBatchSyntaxDiagnosticsTest, DirectExecutionAllowsMultipleStatements) {
  // In simple query protocol (SQLExecDirect), multiple statements are valid and return multiple results
  const std::string sql = "SELECT 11; SELECT 22";
  script->responses[sql] = row_description({{"val1", 23}}) + data_row({{"11"}}) + complete("SELECT 1")
                         + row_description({{"val2", 23}}) + data_row({{"22"}}) + complete("SELECT 1");

  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS));

  // Result 1: 11
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  SQLCHAR buf[32]{};
  SQLLEN len = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, buf, sizeof(buf), &len));
  EXPECT_STREQ("11", reinterpret_cast<char*>(buf));
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));

  // Result 2: 22
  ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, buf, sizeof(buf), &len));
  EXPECT_STREQ("22", reinterpret_cast<char*>(buf));
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));

  // End of results
  EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(stmt));
}

} // namespace
