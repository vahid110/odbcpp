#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "tests/test_handle_helpers.h"
#include "core/util/hex.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <atomic>

namespace {
using namespace rs::core::database;
using rs::util::Result;
using rs::util::DbErrorCode;
using rs::util::Deadline;

struct Observations {
  int created{}, transports{}, disconnects{}, queries{};
  ConnectionSettings settings;
  std::string sql;
  std::vector<QueryParameter> parameters;
  Deadline deadline{};
  bool malformed_value{}, malformed_state{};
  std::string failure_message = "fake error";
};

// Deliberately implements only the database boundary: no PG parser or session.
class FakeBackend final : public IDatabaseConnection {
 public:
  explicit FakeBackend(std::shared_ptr<Observations> seen) : seen_(std::move(seen)) {}
  BackendResult<void> connect(const ConnectionSettings& settings) override {
    seen_->settings = settings; connected_ = true; return {};
  }
  void disconnect() override { connected_ = false; ++seen_->disconnects; }
  bool is_connected() const override { return connected_; }
  std::size_t count_parameter_markers(std::string_view sql) const override {
    return std::count(sql.begin(), sql.end(), '?');
  }
  SqlTranslationResult translate_sql(std::string_view sql) const override {
    if (sql == "unsupported") return {{}, SqlTranslationError::Unsupported, "fake unsupported SQL"};
    return {"native:" + std::string(sql), SqlTranslationError::None, {}};
  }
  NativeTypeInfo describe_type(std::uint32_t id, std::int16_t, std::int32_t) const override {
    if (id == 23) return {ScalarType::Binary, 8, 0, true}; // PG integer ID!
    if (id == 17) return {ScalarType::Boolean, 1, 0, true}; // PG bytea ID!
    return {ScalarType::VarChar, 32, 0, true};
  }
  std::optional<std::string> normalize_result_value(ScalarType type, std::string_view value) const override {
    if (seen_->malformed_value) return std::nullopt;
    if (type == ScalarType::Binary) {
      if (!value.starts_with("bytes:")) return std::nullopt;
      return rs::util::decode_hex(value.substr(6));
    }
    if (type == ScalarType::Boolean) {
      if (value == "yes") return "1";
      if (value == "no") return "0";
      return std::nullopt;
    }
    return std::string(value);
  }
  Result<std::string> catalog_query(const CatalogRequest&) const override {
    return {DbErrorCode::UnsupportedFeature, "fake has no catalogs"};
  }
  std::span<const TypeDefinition> type_catalog() const override {
    static const TypeDefinition types[]{
        {ScalarType::Binary, "octets", 8, {}, {}, {}, false, {}, {}, {}, 0},
        {ScalarType::Boolean, "truth", 1, {}, {}, {}, false, {}, {}, {}, 0},
        {ScalarType::VarChar, "words", 32, {}, {}, {}, true, {}, {}, {}, 0}};
    return types;
  }
  BackendResult<ResolvedTypeMap> resolve_types(std::span<const std::uint32_t> ids, Deadline) override {
    ResolvedTypeMap result;
    for (const auto id : ids) result.emplace(id, describe_type(id, -1, -1));
    return result;
  }
  BackendCapabilities capabilities() const override {
    BackendCapabilities result;
    result.dbms_name = "ContractDB";
    result.max_identifier_length = 117;
    result.identifier_case = IdentifierCase::Upper;
    result.null_collation = NullCollation::Low;
    return result;
  }
  std::optional<std::string> normalize_error_sqlstate(std::string_view state, ErrorContext) const override {
    if (seen_->malformed_state) return "bad";
    if (state == "FAKE_ERROR") return "22018";
    return std::nullopt;
  }
  TransactionCapabilities transaction_capabilities() const override { return {}; }
  BackendResult<void> transaction(TransactionAction, Deadline) override {
    return {DbErrorCode::UnsupportedFeature, "fake has no transactions"};
  }
  BackendResult<void> set_transaction_isolation(TransactionIsolation, Deadline) override {
    return {DbErrorCode::UnsupportedFeature, "fake has no isolation levels"};
  }
  QueryResult rows() const {
    QueryResult result;
    result.columns = {{"binary", 0, 0, 23}, {"flag", 0, 0, 17}, {"text", 0, 0, 999}};
    result.rows = {{"bytes:00ff5c", "yes", std::nullopt}, {"bytes:", "no", ""}};
    result.command_tag = "deliberately not SQL";
    result.statement_kind = StatementKind::SelectCursor;
    return result;
  }
  BackendResult<QueryResult> execute_query(std::string_view sql, Deadline deadline) override {
    ++seen_->queries; seen_->sql = sql; seen_->deadline = deadline;
    if (sql.ends_with("timeout")) return {DbErrorCode::Timeout, "fake deadline"};
    if (sql.ends_with("network")) { connected_ = false; return {DbErrorCode::NetworkError, "fake loss"}; }
    if (sql.ends_with("error")) {
      BackendError error{rs::util::make_error_code(DbErrorCode::QueryFailed), seen_->failure_message};
      error.native_state = "FAKE_ERROR";
      error.disposition = SessionDisposition::Reusable;
      error.session_state = SessionState::Idle;
      error.operation = BackendOperation::ExecuteDirect;
      return error;
    }
    auto result = rows();
    if (sql.ends_with("deferred")) {
      QueryResult error; error.error_message = "later error"; error.error_sqlstate = "FAKE_ERROR";
      result.additional_results.push_back(std::move(error));
    }
    return result;
  }
  BackendResult<QueryResult> execute_prepared(std::string_view sql, std::span<const QueryParameter> params,
                                      Deadline deadline) override {
    seen_->parameters.assign(params.begin(), params.end());
    return execute_query(sql, deadline);
  }
  BackendResult<QueryResult> describe_statement(std::string_view, std::span<const QueryParameterType> types,
                                        Deadline) override {
    QueryResult result = rows(); result.rows.clear();
    result.parameter_type_ids.assign(types.size(), 23);
    return result;
  }
  std::string get_parameter(std::string_view key) const override { return key == "server_version" ? "1.0" : ""; }
 private:
  std::shared_ptr<Observations> seen_;
  bool connected_{};
};

class FakeProvider final : public IBackendProvider {
 public:
  explicit FakeProvider(std::shared_ptr<Observations> seen)
      : seen_(std::move(seen)) {}

  const BackendIdentity& identity() const noexcept override {
    static const BackendIdentity identity{
        "contract", "ContractDB", "ODBCPP Contract"};
    return identity;
  }
  const BackendConnectionDefaults& connection_defaults() const noexcept override {
    static const BackendConnectionDefaults defaults{
        "contract-host", 6543, "contract-default", true};
    return defaults;
  }
  BackendCapabilities capabilities() const noexcept override {
    return FakeBackend(seen_).capabilities();
  }
  std::span<const TypeDefinition> type_catalog() const noexcept override {
    return FakeBackend(seen_).type_catalog();
  }
  TransactionCapabilities transaction_capabilities() const noexcept override {
    return {};
  }
  Result<ConnectionSettings> resolve_connection_options(
      ConnectionOptions options) const override {
    ConnectionSettings settings;
    const auto& defaults = connection_defaults();
    settings.host = options.host.value_or(defaults.host);
    settings.port = options.port.value_or(defaults.port);
    settings.database = options.database.value_or(*defaults.database);
    settings.user = options.user.value_or(std::string{});
    settings.password = options.password.value_or(std::string{});
    settings.use_ssl = options.use_ssl.value_or(defaults.use_ssl);
    settings.ssl_ca_file = options.ssl_ca_file.value_or(std::string{});
    settings.ssl_ca_dir = options.ssl_ca_dir.value_or(std::string{});
    settings.timeout = options.timeout;
    return settings;
  }
  std::unique_ptr<IDatabaseConnection> create_session(
      std::unique_ptr<rs::core::transport::ITransport> transport) const override {
    ++seen_->created;
    if (transport) ++seen_->transports;
    return std::make_unique<FakeBackend>(seen_);
  }

 private:
  std::shared_ptr<Observations> seen_;
};

class BackendContractTest : public ::testing::Test {
 protected:
  std::shared_ptr<Observations> seen = std::make_shared<Observations>();
  SQLHENV env{}; SQLHDBC dbc{}; SQLHSTMT stmt{};
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &env));
    ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
        reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    auto connection = std::make_unique<rs::odbc::ODBCConnection>(
        nullptr, std::make_shared<FakeProvider>(seen));
    dbc = reinterpret_cast<SQLHDBC>(connection.get());
    rs::odbc::HandleRegistry::instance().register_handle(dbc, std::move(connection), env);
  }
  void connect() {
    ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr,
        (SQLCHAR*)"SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0", SQL_NTS,
        nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  }
  void connect_with(std::string_view connection_string) {
    std::string input(connection_string);
    ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(
        dbc, nullptr, reinterpret_cast<SQLCHAR*>(input.data()), SQL_NTS,
        nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  }
  SQLRETURN execute(const char* sql) { return SQLExecDirect(stmt, (SQLCHAR*)sql, SQL_NTS); }
  std::string state(SQLSMALLINT kind = SQL_HANDLE_STMT, SQLHANDLE handle = nullptr) {
    SQLCHAR text[6]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(kind, handle ? handle : stmt, 1, text, nullptr, nullptr, 0, nullptr));
    return reinterpret_cast<char*>(text);
  }
  void TearDown() override {
    if (stmt) SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (dbc) { SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc); }
    if (env) SQLFreeHandle(SQL_HANDLE_ENV, env);
  }
};

TEST_F(BackendContractTest, UsesVerifiedTlsByDefaultAndAllowsExplicitPlaintext) {
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test");
  EXPECT_TRUE(seen->settings.use_ssl);
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));

  connect_with(
      "SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=false");
  EXPECT_FALSE(seen->settings.use_ssl);
}

TEST_F(BackendContractTest, PassesOneCustomTlsTrustLocationToBackend) {
  connect_with(
      "SERVER=fake;PORT=9999;DATABASE=contract;UID=test;"
      "SSLCAFILE=/test/private-ca.pem");
  EXPECT_TRUE(seen->settings.use_ssl);
  EXPECT_EQ(seen->settings.ssl_ca_file, "/test/private-ca.pem");
  EXPECT_TRUE(seen->settings.ssl_ca_dir.empty());
}

TEST_F(BackendContractTest, SelectsSameBackendBeforeAndAfterLoginAndReportsCapabilities) {
  auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  EXPECT_EQ("octets", connection->type_catalog()[0].name);
  EXPECT_EQ(0, seen->created); EXPECT_EQ(0, seen->transports);
  connect();
  EXPECT_EQ(1, seen->created); EXPECT_EQ(1, seen->transports);
  EXPECT_EQ("fake", seen->settings.host); EXPECT_EQ(9999, seen->settings.port);
  EXPECT_EQ("contract", seen->settings.database);
  char name[32]{}; SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_DBMS_NAME, name, sizeof(name), &length));
  EXPECT_STREQ("ContractDB", name);
  SQLWCHAR wide[32]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfoW(dbc, SQL_DBMS_NAME, wide, sizeof(wide), &length));
  EXPECT_EQ(static_cast<SQLWCHAR>('C'), wide[0]);
  SQLUSMALLINT maximum = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_MAX_IDENTIFIER_LEN, &maximum, 0, nullptr));
  EXPECT_EQ(117, maximum);
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_IDENTIFIER_CASE, &maximum, 0, nullptr));
  EXPECT_EQ(SQL_IC_UPPER, maximum);
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_IDENTIFIER_QUOTE_CHAR, name, sizeof(name), &length));
  EXPECT_EQ(0, length); EXPECT_EQ(0, name[0]); // empty string_view has no storage
  EXPECT_EQ(SQL_ERROR, SQLTables(stmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0));
  EXPECT_EQ("HYC00", state()); EXPECT_EQ(0, seen->queries);
  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(dbc, SQL_ATTR_TXN_ISOLATION,
      reinterpret_cast<SQLPOINTER>(SQL_TXN_SERIALIZABLE), 0));
  EXPECT_EQ("HYC00", state(SQL_HANDLE_DBC, dbc));
  EXPECT_EQ(SQL_ERROR, execute("unsupported")); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(0, seen->queries);
}

TEST_F(BackendContractTest, ProviderOwnsConnectionDefaultsWithoutCreatingSession) {
  connect_with("UID=test;SSL=0");
  EXPECT_EQ(1, seen->created);
  EXPECT_EQ("contract-host", seen->settings.host);
  EXPECT_EQ(6543, seen->settings.port);
  EXPECT_EQ("contract-default", seen->settings.database);
}

TEST_F(BackendContractTest, FetchAndGetDataUseBackendValuesMetadataAndNullSemantics) {
  connect();
  unsigned char binary[4]{9,9,9,9}; SQLLEN binary_length = -9;
  SQLCHAR bit = 9; SQLLEN bit_length = -9;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_BINARY, binary, sizeof(binary), &binary_length));
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 2, SQL_C_BIT, &bit, sizeof(bit), &bit_length));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); EXPECT_EQ("native:rows", seen->sql);
  SQLSMALLINT type = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, 1, nullptr, 0, nullptr, &type, nullptr, nullptr, nullptr));
  EXPECT_EQ(SQL_VARBINARY, type);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(3, binary_length); EXPECT_EQ(0, binary[0]); EXPECT_EQ(255, binary[1]); EXPECT_EQ('\\', binary[2]);
  EXPECT_EQ(9, binary[3]); EXPECT_EQ(1, bit);
  char text[16] = "untouched"; SQLLEN length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_CHAR, text, sizeof(text), &length));
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_STREQ("untouched", text);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(0, binary_length); EXPECT_EQ(0, bit);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_CHAR, text, sizeof(text), &length));
  EXPECT_EQ(0, length); EXPECT_STREQ("", text);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_BINARY, binary, 1, &length));
  EXPECT_EQ(3, length); EXPECT_EQ(0, binary[0]);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY, binary, 2, &length));
  EXPECT_EQ(2, length); EXPECT_EQ(255, binary[0]); EXPECT_EQ('\\', binary[1]);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_CHAR, text, sizeof(text), &length));
  EXPECT_STREQ("1", text);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, text, sizeof(text), &length));
  EXPECT_STREQ("00ff5c", text);
}

TEST_F(BackendContractTest, ErrorsNormalizeAndRecoverWithoutPostgresStates) {
  connect();
  EXPECT_EQ(SQL_ERROR, execute("error")); EXPECT_EQ("22018", state());
  seen->malformed_state = true;
  EXPECT_EQ(SQL_ERROR, execute("error")); EXPECT_EQ("42000", state());
  seen->malformed_state = false;
  ASSERT_EQ(SQL_SUCCESS, execute("deferred"));
  EXPECT_EQ(SQL_ERROR, SQLMoreResults(stmt)); EXPECT_EQ("22018", state());
  EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(stmt));
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  seen->malformed_value = true;
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  char output[16] = "untouched"; SQLLEN length = 73;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_CHAR, output, sizeof(output), &length));
  EXPECT_EQ("22018", state()); EXPECT_STREQ("untouched", output); EXPECT_EQ(73, length);
  seen->malformed_value = false;
  EXPECT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, output, sizeof(output), &length));
  EXPECT_STREQ("00ff5c", output);
}

TEST_F(BackendContractTest, PreparedParametersAreRawAndDeadlineIsAbsolute) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  unsigned char input[]{0, 255, '\\'}; SQLLEN length = 3;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_VARBINARY, 3, 0, input, sizeof(input), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(2), 0));
  const auto before = rs::util::Clock::now();
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  const auto after = rs::util::Clock::now();
  EXPECT_EQ("native:rows ?", seen->sql);
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Binary, seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>(std::string("\0\xff\\", 3)), seen->parameters[0].value);
  EXPECT_GE(seen->deadline, before + std::chrono::seconds(2));
  EXPECT_LE(seen->deadline, after + std::chrono::seconds(2));
}

TEST_F(BackendContractTest, TimeoutRetiresConnectionAndReconnectSelectsBackendAgain) {
  connect();
  EXPECT_EQ(SQL_ERROR, execute("timeout")); EXPECT_EQ("HYT00", state());
  EXPECT_GT(seen->disconnects, 0);
  SQLUINTEGER dead = SQL_CD_FALSE;
  EXPECT_EQ(SQL_ERROR, SQLGetConnectAttr(dbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
  EXPECT_EQ("08003", state(SQL_HANDLE_DBC, dbc));
  EXPECT_EQ(SQL_CD_FALSE, dead);
  EXPECT_EQ(SQL_ERROR, execute("rows")); EXPECT_EQ("08001", state()); // existing statement fallback
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
  const auto created = seen->created;
  connect(); EXPECT_EQ(created + 1, seen->created);
  EXPECT_EQ(SQL_SUCCESS, execute("rows"));
}
TEST_F(BackendContractTest, InvalidBoundValuePreservesOutputAndReportsRowError) {
  connect();
  unsigned char output[4]{7,7,7,7}; SQLLEN length = 73;
  SQLUSMALLINT row_status = SQL_ROW_SUCCESS;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &row_status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_BINARY, output, sizeof(output), &length));
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  seen->malformed_value = true;
  EXPECT_EQ(SQL_ERROR, SQLFetch(stmt)); EXPECT_EQ("22018", state());
  EXPECT_EQ(SQL_ROW_ERROR, row_status); EXPECT_EQ(73, length); EXPECT_EQ(7, output[0]);
  seen->malformed_value = false;
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(3, length); EXPECT_EQ(0, output[0]);
}

TEST_F(BackendContractTest, TransportLossReportsBackendLiveness) {
  connect();
  EXPECT_EQ(SQL_ERROR, execute("network")); EXPECT_EQ("08S01", state());
  SQLUINTEGER dead = SQL_CD_FALSE;
  ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(dbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
  EXPECT_EQ(SQL_CD_TRUE, dead);
}

TEST_F(BackendContractTest, BackendDiagnosticStaysDetailedWhileFailureLogUsesPublicSummary) {
  static std::atomic<unsigned> sequence{0};
  const auto directory = std::filesystem::temp_directory_path() /
      ("odbcpp-backend-secret-log-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
       std::to_string(sequence.fetch_add(1)));
  std::filesystem::create_directories(directory);
  const auto path = directory / "driver.log";
  seen->failure_message = "credential=server-secret-981724\nprivate SQL and identifiers";
  connect_with("SERVER=fake;SSL=0;LogLevel=Trace;LogQueries=false;LogAsync=false;LogSink=File;LogFile=" + path.string());
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  for (const bool prepared : {false, true}) {
    if (prepared) {
      ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"error", SQL_NTS));
      EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
    } else {
      EXPECT_EQ(SQL_ERROR, execute("error"));
    }
    SQLCHAR diagnostic[1024]{};
    SQLCHAR native_state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, native_state,
        nullptr, diagnostic, sizeof(diagnostic), nullptr));
    EXPECT_EQ(seen->failure_message, reinterpret_cast<char*>(diagnostic));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(native_state));
  }
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt));
  stmt = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc));
  dbc = nullptr;
  std::ifstream input(path);
  const std::string contents((std::istreambuf_iterator<char>(input)), {});
  EXPECT_NE(std::string::npos, contents.find("Database server rejected the operation"));
  EXPECT_EQ(std::string::npos, contents.find("server-secret-981724"));
  EXPECT_EQ(std::string::npos, contents.find("private SQL and identifiers"));
  std::filesystem::remove_all(directory);
}

TEST(BackendErrorSummaryTest, PublicSummaryNeverDependsOnOwnedSensitiveDetails) {
  for (const auto kind : {BackendErrorClass::Unknown, BackendErrorClass::Connection,
      BackendErrorClass::Authentication, BackendErrorClass::Server, BackendErrorClass::Timeout,
      BackendErrorClass::Transport, BackendErrorClass::Tls, BackendErrorClass::InvalidInput,
      BackendErrorClass::NotConnected, BackendErrorClass::Protocol, BackendErrorClass::Unsupported,
      BackendErrorClass::InvalidMetadata, static_cast<BackendErrorClass>(999)}) {
    BackendError error{rs::util::make_error_code(DbErrorCode::QueryFailed), "password=private-marker"};
    error.error_class = kind;
    error.native_state = "private-native-state";
    error.native_code = 981724;
    const std::string saved(error.safe_summary());
    EXPECT_FALSE(saved.empty());
    EXPECT_EQ(std::string::npos, saved.find("private"));
    EXPECT_EQ(std::string::npos, saved.find("981724"));
    error.message.assign(4096, 'x');
    error.native_state.reset();
    EXPECT_EQ(saved, error.safe_summary());
    auto copy = error;
    auto moved = std::move(copy);
    EXPECT_EQ(saved, moved.safe_summary());
  }
}
} // namespace
