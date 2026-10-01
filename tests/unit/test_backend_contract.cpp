#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "tests/test_handle_helpers.h"
#include "core/util/hex.h"
#include "odbc/unicode.h"

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
  int created{}, transports{}, disconnects{}, queries{}, descriptions{}, translations{};
  ConnectionSettings settings;
  std::string sql;
  std::vector<QueryParameter> parameters;
  Deadline deadline{};
  bool malformed_value{}, malformed_state{}, malformed_text{};
  std::string failure_message = "fake error";
  std::string server_version = "1.0";
  bool setup_allocation_failure{false};
  bool advertised_transactions{false};
  bool absent_description{false};
  bool missing_parameter_metadata{}, parameter_metadata_error{};
  int invalid_cell_errors{}, invalid_result_structure{}, invalid_execution_shape{}, invalid_description_shape{};
};

template <typename T>
concept HasNativeTypeInterpretation = requires(const T& backend) {
  backend.describe_type(23, -1, -1);
};
template <typename T>
concept HasNativeTypeResolution = requires(T& backend) {
  backend.resolve_types(std::span<const std::uint32_t>{}, Deadline::max());
};
template <typename T>
concept HasNativeServerParameters = requires(const T& backend) {
  backend.get_parameter("server_version");
};
template <typename T>
concept HasNativeCompletionTag = requires(T result) { result.command_tag; };
template <typename T>
concept HasNativeTableProvenance = requires(T column) { column.table_id; column.table_column; };
static_assert(!HasNativeCompletionTag<QueryResult>);
static_assert(!HasNativeTableProvenance<ResultColumnMetadata>);
template <typename T> concept HasNativeTypeId = requires(T value) { value.type_id; };
template <typename T> concept HasNativeTypeSize = requires(T value) { value.type_size; };
template <typename T> concept HasNativeTypeModifier = requires(T value) { value.type_modifier; };
template <typename T> concept HasNativeFormatCode = requires(T value) { value.format_code; };
template <typename T> concept HasNativeParameterIds = requires(T value) { value.parameter_type_ids; };
static_assert(!HasNativeTypeId<ResultColumnMetadata>);
static_assert(!HasNativeTypeSize<ResultColumnMetadata>);
static_assert(!HasNativeTypeModifier<ResultColumnMetadata>);
static_assert(!HasNativeFormatCode<ResultColumnMetadata>);
static_assert(!HasNativeParameterIds<QueryResult>);
template <typename T> concept HasSessionSqlTranslation = requires(const T& session) { session.translate_sql("SELECT 1"); };
template <typename T> concept HasSessionMarkerCounting = requires(const T& session) { session.count_parameter_markers("?"); };
template <typename T> concept HasRequiredTransaction = requires(T& session) { session.transaction(TransactionAction::Commit, Deadline::max()); };
template <typename T> concept HasRequiredIsolation = requires(T& session) { session.set_transaction_isolation(TransactionIsolation::Serializable, Deadline::max()); };
template <typename T> concept HasRequiredDescription = requires(T& session) { session.describe_statement("SELECT ?", std::span<const QueryParameterType>{}, Deadline::max()); };
template <typename T> concept HasRequiredCatalog = requires(const T& session) { session.catalog_query(CatalogRequest{TablesCatalogRequest{}}); };
template <typename T> concept HasSessionTypeCatalog = requires(const T& session) { session.type_catalog(); };
static_assert(!HasSessionTypeCatalog<IDatabaseConnection>);
static_assert(!HasRequiredCatalog<IDatabaseConnection>);
static_assert(!HasRequiredDescription<IDatabaseConnection>);
static_assert(!HasRequiredTransaction<IDatabaseConnection>);
static_assert(!HasRequiredIsolation<IDatabaseConnection>);
static_assert(!HasSessionSqlTranslation<IDatabaseConnection>);
static_assert(!HasSessionMarkerCounting<IDatabaseConnection>);
static_assert(!HasNativeServerParameters<IDatabaseConnection>);
static_assert(!HasNativeTypeInterpretation<IDatabaseConnection>);
static_assert(!HasNativeTypeResolution<IDatabaseConnection>);

// Deliberately implements only the database boundary: no PG parser or session.
class FakeBackend final : public IDatabaseConnection, public IStatementDescription {
 public:
  explicit FakeBackend(std::shared_ptr<Observations> seen)
      : seen_(std::move(seen)), absent_description_(seen_->absent_description) {}
  BackendResult<void> connect(const ConnectionSettings& settings) override {
    if (seen_->setup_allocation_failure) return {DbErrorCode::AllocationFailure, {}};
    seen_->settings = settings; connected_ = true; return {};
  }
  void disconnect() override { connected_ = false; ++seen_->disconnects; }
  bool is_connected() const override { return connected_; }
  BackendCapabilities capabilities() const override {
    BackendCapabilities result;
    result.dbms_name = "ContractDB";
    result.describe_parameters = true;
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
  QueryResult rows() const {
    QueryResult result;
    result.columns = {{"binary", {}}, {"flag", {}}, {"text", {}}};
    result.columns[0].normalized_type = NativeTypeInfo{ScalarType::Binary, 8, 0, true};
    result.columns[1].normalized_type = NativeTypeInfo{ScalarType::Boolean, 1, 0, true};
    result.columns[2].normalized_type = NativeTypeInfo{ScalarType::VarChar, 32, 0, true};
    result.rows = {{std::string("\0\xff\\", 3), "1", std::nullopt}, {"", "0", ""}};
    if (seen_->malformed_value) {
      result.rows[0][0] = "";
      result.cell_errors.push_back({0, 0});
    }
    if (seen_->malformed_text) {
      result.rows[0][2] = "";
      result.rows[1][2] = "\xe2\x82\xac\xf0\x9f\x98\x80";
      result.cell_errors.push_back({0, 2});
    }
    switch (seen_->invalid_cell_errors) {
      case 1: result.cell_errors = {{99, 0}}; break;
      case 2: result.cell_errors = {{0, 99}}; break;
      case 3: result.cell_errors = {{0, 2}}; break; // NULL
      case 4: result.cell_errors = {{1, 0}, {0, 0}}; break;
      case 5: result.cell_errors = {{0, 0}, {0, 0}}; break;
      default: break;
    }
    switch (seen_->invalid_result_structure) {
      case 1: result.rows[0].pop_back(); break;
      case 2: result.rows[0].push_back(std::nullopt); break;
      case 3: result.columns.clear(); break;
      case 4: result.columns[0].name = "\xc0\x80"; break;
      case 5: result.columns[0].name = std::string("a\0b", 3); break;
      case 6: {
        QueryResult later; later.columns = result.columns; later.rows = {{"x"}};
        result.additional_results.push_back(std::move(later)); break;
      }
      case 7: {
        QueryResult later; later.columns = result.columns; later.columns[0].name = "\x80";
        result.additional_results.push_back(std::move(later)); break;
      }
      case 8: result.columns[0].name = ""; break;
      case 9: result.columns[0].name = "\xe2\x82\xac\xf0\x9f\x98\x80"; break;
      default: break;
    }
    result.statement_kind = StatementKind::SelectCursor;
    return result;
  }
  BackendResult<QueryResult> execute_query(std::string_view sql, Deadline deadline) override {
    ++seen_->queries; seen_->sql = sql; seen_->deadline = deadline;
    if (sql.ends_with("limit")) { connected_ = false; return {DbErrorCode::ResourceLimit, "Database response byte limit exceeded"}; }
    if (sql.ends_with("allocation")) { connected_ = false; return {DbErrorCode::AllocationFailure, {}}; }
    if (sql.ends_with("timeout")) return {DbErrorCode::Timeout, "fake deadline"};
    if (sql.ends_with("network")) { connected_ = false; return {DbErrorCode::NetworkError, "fake loss"}; }
    if (sql.ends_with("metadata_failure")) {
      BackendError error{rs::util::make_error_code(DbErrorCode::QueryFailed), "invalid normalized metadata"};
      error.error_class = BackendErrorClass::InvalidMetadata;
      error.native_state = "FAKE_ERROR"; // must not turn a contract failure into server SQLSTATE
      error.disposition = SessionDisposition::Reusable;
      error.session_state = SessionState::Idle;
      return error;
    }
    if (sql.ends_with("error")) {
      BackendError error{rs::util::make_error_code(DbErrorCode::QueryFailed), seen_->failure_message};
      error.native_state = "FAKE_ERROR";
      error.disposition = SessionDisposition::Reusable;
      error.session_state = SessionState::Idle;
      error.operation = BackendOperation::ExecuteDirect;
      return error;
    }
    auto result = rows();
    if (sql.ends_with("unnormalized")) result.columns[0].normalized_type.reset();
    if (sql.ends_with("deferred") || sql.ends_with("deferred_metadata")) {
      QueryResult error;
      error.error.emplace(rs::util::make_error_code(DbErrorCode::QueryFailed), "Query error: later error");
      error.error->native_state = "FAKE_ERROR";
      if (sql.ends_with("deferred_metadata")) error.error->error_class = BackendErrorClass::InvalidMetadata;
      result.additional_results.push_back(std::move(error));
    }
    if (sql.ends_with("ordered")) {
      result = QueryResult{};
      result.affected_rows = 7; result.statement_kind = StatementKind::UpdateWhere;
      auto empty_rowset = rows(); empty_rowset.rows.clear();
      result.additional_results.push_back(std::move(empty_rowset));
      QueryResult zero_count; zero_count.statement_kind = StatementKind::DeleteWhere;
      result.additional_results.push_back(std::move(zero_count));
      QueryResult error;
      error.error.emplace(rs::util::make_error_code(DbErrorCode::QueryFailed), "ordered later error");
      error.error->native_state = "FAKE_ERROR";
      result.additional_results.push_back(std::move(error));
    }
    if (seen_->invalid_execution_shape != 0) {
      QueryResult later;
      later.error.emplace(rs::util::make_error_code(DbErrorCode::QueryFailed), "invalid item");
      switch (seen_->invalid_execution_shape) {
        case 1: result.error = later.error; break;
        case 2: later.error.reset(); later.additional_results.emplace_back(); break;
        case 3: later.columns = result.columns; break;
        case 4: later.rows = result.rows; break;
        case 5: later.cell_errors = {{0, 0}}; break;
        case 6: later.normalized_parameter_types.push_back({ScalarType::Integer, 10, 0, true}); break;
        case 7: later.affected_rows = 1; break;
        case 8: later.statement_kind = StatementKind::Unknown; break;
      }
      if (seen_->invalid_execution_shape != 1) result.additional_results.push_back(std::move(later));
    }
    return result;
  }
  BackendResult<QueryResult> execute_prepared(std::string_view sql, std::span<const QueryParameter> params,
                                      Deadline deadline) override {
    seen_->parameters.assign(params.begin(), params.end());
    return execute_query(sql, deadline);
  }
  IStatementDescription* statement_description() noexcept override {
    return absent_description_ ? nullptr : this;
  }
  BackendResult<QueryResult> describe_statement(std::string_view sql, std::span<const QueryParameterType> types,
                                        Deadline) override {
    ++seen_->descriptions;
    seen_->sql = sql;
    QueryResult result = rows(); result.rows.clear();
    result.normalized_parameter_types.assign(types.size(),
        NativeTypeInfo{ScalarType::Binary, 8, 0, true});
    switch (seen_->invalid_description_shape) {
      case 1: result.error.emplace(rs::util::make_error_code(DbErrorCode::QueryFailed), "invalid description"); break;
      case 2: result.additional_results.emplace_back(); break;
      case 3: result.additional_results.emplace_back(); result.additional_results.back().additional_results.emplace_back(); break;
      case 4: result.rows = {{"a", "1", "b"}}; break;
      case 5: result.cell_errors = {{0, 0}}; break;
    }
    if (seen_->missing_parameter_metadata) result.normalized_parameter_types.clear();
    if (seen_->parameter_metadata_error) {
      BackendError error{rs::util::make_error_code(DbErrorCode::QueryFailed), "fake metadata error"};
      error.operation = BackendOperation::ResolveTypes;
      error.native_state = "FAKE_ERROR";
      return error;
    }
    return result;
  }
  std::string server_version() const override { return seen_->server_version; }
 private:
  std::shared_ptr<Observations> seen_;
  bool connected_{};
  const bool absent_description_;
};

class FakeSqlDialect final : public ISqlDialect {
 public:
  explicit FakeSqlDialect(std::shared_ptr<Observations> seen) : seen_(std::move(seen)) {}
  std::size_t count_parameter_markers(std::string_view sql) const override {
    return std::count(sql.begin(), sql.end(), '?');
  }
  SqlTranslationResult translate_sql(std::string_view sql) const override {
    ++seen_->translations;
    if (sql == "unsupported") return {{}, SqlTranslationError::Unsupported, "fake unsupported SQL"};
    return {"native:" + std::string(sql), SqlTranslationError::None, {}};
  }
 private:
  std::shared_ptr<Observations> seen_;
};

class FakeProvider final : public IBackendProvider {
 public:
  explicit FakeProvider(std::shared_ptr<Observations> seen)
      : seen_(std::move(seen)), dialect_(seen_) {}

  const ISqlDialect& sql_dialect() const noexcept override { return dialect_; }
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
  std::span<const TypeDefinition> type_catalog(std::string_view version = {}) const noexcept override {
    static const TypeDefinition types[]{
        {ScalarType::Binary, "octets", 8, {}, {}, {}, false, {}, {}, {}, 0},
        {ScalarType::Boolean, "truth", 1, {}, {}, {}, false, {}, {}, {}, 0},
        {ScalarType::VarChar, "words", 32, {}, {}, {}, true, {}, {}, {}, 0}};
    static const TypeDefinition modern[]{
        {ScalarType::Binary, "octets", 16, {}, {}, {}, false, {}, {}, {}, 0},
        types[1], types[2]};
    return version == "2.0" ? std::span<const TypeDefinition>(modern) : std::span<const TypeDefinition>(types);
  }
  TransactionCapabilities transaction_capabilities() const noexcept override {
    return seen_->advertised_transactions
        ? TransactionCapabilities{true, true, TransactionIsolation::ReadCommitted, {true, true, true, true}}
        : TransactionCapabilities{};
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
    settings.response_limits = options.response_limits;
    settings.startup_response_limits = options.startup_response_limits;
    settings.result_limits = options.result_limits;
    settings.input_limits = options.input_limits;
    if (!valid_resource_limits(settings)) return {DbErrorCode::InvalidParameter, "Invalid resource limits"};
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
  FakeSqlDialect dialect_;
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
  SQLCHAR deferred_message[128]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, nullptr,
      nullptr, deferred_message, sizeof(deferred_message), nullptr));
  EXPECT_STREQ("Query error: later error", reinterpret_cast<char*>(deferred_message));
  EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(stmt));
  seen->malformed_value = true;
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  char output[16] = "untouched"; SQLLEN length = 73;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_CHAR, output, sizeof(output), &length));
  EXPECT_EQ("22018", state()); EXPECT_STREQ("untouched", output); EXPECT_EQ(73, length);
  seen->malformed_value = false;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_CHAR, output, sizeof(output), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
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
  seen->malformed_value = true;
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
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
  input.close();
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
      BackendErrorClass::InvalidMetadata, BackendErrorClass::ResourceLimit, BackendErrorClass::AllocationFailure, static_cast<BackendErrorClass>(999)}) {
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

TEST_F(BackendContractTest, ResourceLimitReportsGeneralErrorAndDeadConnection) {
  connect();
  EXPECT_EQ(SQL_ERROR, execute("limit"));
  EXPECT_EQ("HY000", state());
  SQLUINTEGER dead = SQL_CD_FALSE;
  ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(dbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
  EXPECT_EQ(SQL_CD_TRUE, dead);
}
} // namespace

TEST_F(BackendContractTest, AllocationFailureReportsMemoryErrorAndDeadConnection) {
  connect();
  EXPECT_EQ(SQL_ERROR, execute("allocation"));
  SQLCHAR state[6]{};
  SQLCHAR message[128]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, nullptr,
                                     message, sizeof(message), nullptr));
  EXPECT_STREQ("HY001", reinterpret_cast<const char*>(state));
  SQLUINTEGER dead = SQL_CD_FALSE;
  ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(dbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
  EXPECT_EQ(SQL_CD_TRUE, dead);
}

TEST_F(BackendContractTest, StartupAllocationFailureReportsMemoryStateAndAllowsFreshConnect) {
  seen->setup_allocation_failure = true;
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr,
      (SQLCHAR*)"SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0", SQL_NTS,
      nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("HY001", state(SQL_HANDLE_DBC, dbc));
  seen->setup_allocation_failure = false;
  connect();
}

TEST_F(BackendContractTest, ResourceLimitOptionsReachBackendSession) {
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0;MaxResponseBytes=8192;MaxResponseMessages=16;"
      "MaxStartupResponseBytes=2048;MaxStartupResponseMessages=8;MaxRows=7;MaxCells=21;"
      "MaxColumns=3;MaxResults=2;MaxMetadataEntries=6;MaxColumnNameBytes=32;"
      "MaxMetadataNameBytes=192;MaxDiagnosticBytes=256;MaxSqlBytes=1024;MaxParameters=4;"
      "MaxParameterBytes=32;MaxParameterTotalBytes=128;MaxConnectionFieldBytes=64;"
      "MaxRequestWireBytes=4096;MaxStartupWireBytes=512;MaxAuthWireBytes=256");
  const auto& settings = seen->settings;
  EXPECT_EQ(settings.response_limits.max_wire_bytes, 8192u);
  EXPECT_EQ(settings.response_limits.max_messages, 16u);
  EXPECT_EQ(settings.startup_response_limits.max_wire_bytes, 2048u);
  EXPECT_EQ(settings.startup_response_limits.max_messages, 8u);
  EXPECT_EQ(settings.result_limits.max_rows, 7u);
  EXPECT_EQ(settings.result_limits.max_cells, 21u);
  EXPECT_EQ(settings.result_limits.max_columns_per_description, 3u);
  EXPECT_EQ(settings.result_limits.max_results, 2u);
  EXPECT_EQ(settings.result_limits.max_metadata_entries, 6u);
  EXPECT_EQ(settings.result_limits.max_column_name_bytes, 32u);
  EXPECT_EQ(settings.result_limits.max_metadata_name_bytes, 192u);
  EXPECT_EQ(settings.result_limits.max_diagnostic_bytes, 256u);
  EXPECT_EQ(settings.input_limits.max_sql_bytes, 1024u);
  EXPECT_EQ(settings.input_limits.max_parameters, 4u);
  EXPECT_EQ(settings.input_limits.max_parameter_bytes, 32u);
  EXPECT_EQ(settings.input_limits.max_parameter_total_bytes, 128u);
  EXPECT_EQ(settings.input_limits.max_connection_field_bytes, 64u);
  EXPECT_EQ(settings.input_limits.max_request_wire_bytes, 4096u);
  EXPECT_EQ(settings.input_limits.max_startup_wire_bytes, 512u);
  EXPECT_EQ(settings.input_limits.max_auth_wire_bytes, 256u);
}

TEST_F(BackendContractTest, InvalidResourceOptionsFailBeforeSessionCreationAndPermitRecovery) {
  for (const auto suffix : {"MaxSqlBytes=", "MaxSqlBytes=-1", "MaxSqlBytes=+1", "MaxSqlBytes=12x",
       "MaxSqlBytes=184467440737095516160", "MaxSqlBytes=67108865", "MaxParameters=65536",
       "MaxResponseBytes=4", "MaxResponseMessages=0", "MaxStartupResponseMessages=0", "MaxResults=0"}) {
    const std::string input = std::string("SERVER=fake;SSL=0;") + suffix;
    EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr, (SQLCHAR*)input.c_str(), SQL_NTS,
        nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(seen->created, 0u);
    EXPECT_EQ(seen->transports, 0u);
  }
  connect();
}

TEST_F(BackendContractTest, OmittedResourceOptionsPreserveSdkDefaultsAndZeroDataBudgetsAreAccepted) {
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0");
  EXPECT_EQ(seen->settings.response_limits.max_wire_bytes, ConnectionSettings{}.response_limits.max_wire_bytes);
  EXPECT_EQ(seen->settings.input_limits.max_request_wire_bytes, ConnectionSettings{}.input_limits.max_request_wire_bytes);
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0;MaxRows=0;MaxCells=0;MaxColumns=0;MaxSqlBytes=0;MaxParameters=0");
  EXPECT_EQ(seen->settings.result_limits.max_rows, 0u);
  EXPECT_EQ(seen->settings.input_limits.max_parameters, 0u);
}

TEST_F(BackendContractTest, WideConnectionStringsAcceptResourceOptionsWithoutKeywordWarning) {
  const std::string text = "SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0;MaxRows=17;MaxSqlBytes=1024";
  std::vector<SQLWCHAR> input(text.begin(), text.end());
  input.push_back(0);
  ASSERT_EQ(SQL_SUCCESS, SQLDriverConnectW(dbc, nullptr, input.data(), SQL_NTS,
      nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ(seen->settings.result_limits.max_rows, 17u);
  EXPECT_EQ(seen->settings.input_limits.max_sql_bytes, 1024u);
}

TEST_F(BackendContractTest, SqlInputLimitsApplyBeforeAnsiAndWideCopiesAndAllowRecovery) {
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0;MaxSqlBytes=4");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  for (const bool prepare : {false, true}) {
    for (const bool wide : {false, true}) {
      for (const SQLINTEGER length : {SQLINTEGER{SQL_NTS}, SQLINTEGER{5}}) {
        const auto queries = seen->queries;
        const auto descriptions = seen->descriptions;
        SQLCHAR narrow[] = "12345";
        SQLWCHAR text[]{'1', '2', '3', '4', '5', 0};
        const auto result = wide ? (prepare ? SQLPrepareW(stmt, text, length) : SQLExecDirectW(stmt, text, length)) :
            (prepare ? SQLPrepare(stmt, narrow, length) : SQLExecDirect(stmt, narrow, length));
        EXPECT_EQ(result, SQL_ERROR);
        EXPECT_EQ(state(), "HY000");
        EXPECT_EQ(seen->queries, queries);
        EXPECT_EQ(seen->descriptions, descriptions);
        EXPECT_EQ(seen->disconnects, 0u);
      }
    }
  }
  SQLCHAR exact[] = "rows";
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, exact, 4));
  EXPECT_EQ(seen->queries, 1u);
}

TEST_F(BackendContractTest, WideSqlLimitCountsUtf8ExpansionAndPreservesUnicodeErrors) {
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0;MaxSqlBytes=4");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  SQLWCHAR over[]{0x20ac, 0x20ac, 0}; // two units, six UTF-8 bytes
  EXPECT_EQ(SQL_ERROR, SQLExecDirectW(stmt, over, SQL_NTS));
  EXPECT_EQ(state(), "HY000");
  EXPECT_EQ(seen->queries, 0u);
  SQLWCHAR invalid[]{0xd800, 0};
  EXPECT_EQ(SQL_ERROR, SQLPrepareW(stmt, invalid, SQL_NTS));
  EXPECT_EQ(state(), "22018");
  SQLWCHAR exact[]{0x20ac, 'x', 0};
  ASSERT_EQ(SQL_SUCCESS, SQLPrepareW(stmt, exact, SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(seen->sql, std::string("native:\xe2\x82\xac") + "x");
}

TEST_F(BackendContractTest, RejectedSqlInputPreservesPreviouslyPreparedStatement) {
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0;MaxSqlBytes=4");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows", SQL_NTS));
  EXPECT_EQ(SQL_ERROR, SQLPrepare(stmt, (SQLCHAR*)"too long", SQL_NTS));
  EXPECT_EQ(state(), "HY000");
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(seen->sql, "native:rows");
  EXPECT_EQ(seen->queries, 1u);
}

TEST_F(BackendContractTest, ParameterCountLimitPreservesPreparedStatement) {
  connect_with("SSL=0;MaxParameters=1");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  EXPECT_EQ(SQL_ERROR, SQLPrepare(stmt, (SQLCHAR*)"rows ??", SQL_NTS));
  EXPECT_EQ("HY000", state());
  char input[] = "ok"; SQLLEN length = 2;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 16, 0, input, sizeof(input), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ("native:rows ?", seen->sql);
  EXPECT_EQ(1u, seen->parameters.size());
}

TEST_F(BackendContractTest, ParameterCopiesRejectOverflowAndRecoverAtExactBoundary) {
  connect_with("SSL=0;MaxParameterBytes=3;MaxParameterTotalBytes=3");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99; SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  char input[] = "abcd";
  for (const SQLSMALLINT type : {SQLSMALLINT(SQL_C_CHAR), SQLSMALLINT(SQL_C_BINARY)}) {
    for (const SQLLEN initial_length : {SQLLEN(4), SQLLEN(SQL_NTS)}) {
      if (type == SQL_C_BINARY && initial_length == SQL_NTS) continue;
      SQLLEN length = initial_length;
      ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, type,
          type == SQL_C_BINARY ? SQL_VARBINARY : SQL_VARCHAR,
          16, 0, input, sizeof(input), &length));
      const auto queries = seen->queries;
      const auto disconnects = seen->disconnects;
      EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
      EXPECT_EQ("HY000", state());
      EXPECT_EQ(queries, seen->queries);
      EXPECT_EQ(disconnects, seen->disconnects);
      EXPECT_EQ(1u, processed); EXPECT_EQ(SQL_PARAM_ERROR, status);
      length = initial_length == SQL_NTS ? SQL_NTS : 3;
      input[3] = '\0';
      ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
      ASSERT_EQ(1u, seen->parameters.size());
      EXPECT_EQ(std::optional<std::string>("abc"), seen->parameters[0].value);
      EXPECT_EQ(SQL_PARAM_SUCCESS, status);
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
      input[3] = 'd';
    }
  }
}

TEST_F(BackendContractTest, WideParameterBudgetMeasuresUtf8Bytes) {
  connect_with("SSL=0;MaxParameterBytes=4;MaxParameterTotalBytes=4");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLWCHAR input[]{0x20ac, 0x20ac, 0};
  for (const SQLLEN initial_length : {SQLLEN(2 * sizeof(SQLWCHAR)), SQLLEN(SQL_NTS)}) {
    SQLLEN length = initial_length;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
        SQL_VARCHAR, 16, 0, input, sizeof(input), &length));
    const auto queries = seen->queries;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY000", state());
    EXPECT_EQ(queries, seen->queries);
    input[1] = 'x';
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(std::optional<std::string>("\xe2\x82\xac" "x"), seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    input[1] = 0x20ac;
  }
  input[0] = 0xd800; input[1] = 0;
  SQLLEN length = sizeof(SQLWCHAR);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
      SQL_VARCHAR, 16, 0, input, sizeof(input), &length));
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22018", state());
}

TEST_F(BackendContractTest, AggregateParameterBudgetIncludesNormalizedScalars) {
  connect_with("SSL=0;MaxParameterBytes=4;MaxParameterTotalBytes=4");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ??", SQL_NTS));
  char input[] = "abc"; SQLLEN length = 3; SQLINTEGER number = 12;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 16, 0, input, sizeof(input), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &number, sizeof(number), nullptr));
  const auto queries = seen->queries;
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY000", state());
  EXPECT_EQ(queries, seen->queries);
  number = 1;
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(2u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>("1"), seen->parameters[1].value);
}

TEST_F(BackendContractTest, ZeroParameterByteBudgetAllowsNullAndExplicitEmpty) {
  connect_with("SSL=0;MaxParameterBytes=0;MaxParameterTotalBytes=0");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ??", SQL_NTS));
  char input[] = "ignored"; SQLLEN empty = 0, null = SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 16, 0, input, sizeof(input), &empty));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_VARBINARY, 16, 0, nullptr, 0, &null));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(2u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>(""), seen->parameters[0].value);
  EXPECT_EQ(std::nullopt, seen->parameters[1].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  empty = SQL_NTS;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 16, 0, input, sizeof(input), &empty));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_VARBINARY, 16, 0, nullptr, 0, &null));
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY000", state());
}

TEST_F(BackendContractTest, AggregateBudgetRejectsLaterTextBeforeBackendWork) {
  connect_with("SSL=0;MaxParameterBytes=3;MaxParameterTotalBytes=4");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ??", SQL_NTS));
  char first[] = "abc", second[] = "de"; SQLLEN length = SQL_NTS;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 16, 0, first, sizeof(first), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 16, 0, second, sizeof(second), &length));
  const auto queries = seen->queries;
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY000", state());
  EXPECT_EQ(queries, seen->queries);
  second[1] = '\0';
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>("d"), seen->parameters[1].value);
}

TEST_F(BackendContractTest, ZeroParameterCountAllowsUnparameterizedPreparation) {
  connect_with("SSL=0;MaxParameters=0");
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows", SQL_NTS));
  EXPECT_EQ(SQL_ERROR, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  EXPECT_EQ("HY000", state());
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_TRUE(seen->parameters.empty());
  EXPECT_EQ("native:rows", seen->sql);
}

TEST_F(BackendContractTest, NarrowConnectionCaptureRejectsOversizeBeforeCreationAndOutput) {
  constexpr std::size_t cap = 1024 * 1024;
  std::string input = "SSL=0;DESCRIPTION=";
  input.resize(cap + 1, 'x');
  SQLCHAR output[]{'o', 'k', 0}; SQLSMALLINT output_length = 77;
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr, (SQLCHAR*)input.data(), SQL_NTS,
      output, sizeof(output), &output_length, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
  EXPECT_EQ(0, seen->created); EXPECT_EQ(0, seen->transports);
  EXPECT_EQ(77, output_length); EXPECT_STREQ("ok", (const char*)output);
  input.resize(cap);
  ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr, (SQLCHAR*)input.data(), SQL_NTS,
      nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ(1, seen->created);
  const auto disconnects = seen->disconnects;
  input.push_back('x');
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr, (SQLCHAR*)input.data(), SQL_NTS,
      output, sizeof(output), &output_length, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
  EXPECT_EQ(disconnects, seen->disconnects);
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  EXPECT_EQ(SQL_SUCCESS, execute("rows"));
}

TEST_F(BackendContractTest, WideConnectionCaptureBoundsActualUtf8Expansion) {
  constexpr std::size_t cap = 1024 * 1024;
  const std::string prefix = "SSL=0;DESCRIPTION=";
  std::vector<SQLWCHAR> input(prefix.begin(), prefix.end());
  const auto euro_count = (cap - prefix.size()) / 3;
  input.insert(input.end(), euro_count, SQLWCHAR(0x20ac));
  input.insert(input.end(), (cap - prefix.size()) % 3, SQLWCHAR('x'));
  input.push_back('x'); input.push_back(0);
  SQLWCHAR output[]{'o', 'k', 0}; SQLSMALLINT output_length = 77;
  EXPECT_EQ(SQL_ERROR, SQLDriverConnectW(dbc, nullptr, input.data(), SQL_NTS,
      output, 3, &output_length, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
  EXPECT_EQ(0, seen->created); EXPECT_EQ(77, output_length);
  EXPECT_EQ(SQLWCHAR('o'), output[0]); EXPECT_EQ(SQLWCHAR('k'), output[1]);
  input[input.size() - 2] = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLDriverConnectW(dbc, nullptr, input.data(), SQL_NTS,
      nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ(1, seen->created);
}

TEST_F(BackendContractTest, ConnectFieldCaptureBoundsEachAnsiAndWideInput) {
  constexpr std::size_t cap = 1024 * 1024;
  std::string excessive(cap + 1, 'x');
  std::vector<SQLWCHAR> wide_excessive(cap + 2, 'x'); wide_excessive.back() = 0;
  SQLCHAR dsn[] = "SSL=0", user[] = "test", password[] = "secret";
  SQLWCHAR wide_dsn[]{'S','S','L','=','0',0}, wide_user[]{'t',0}, wide_password[]{'s',0};
  for (int field = 0; field < 3; ++field) {
    EXPECT_EQ(SQL_ERROR, SQLConnect(dbc,
        field == 0 ? (SQLCHAR*)excessive.data() : dsn, SQL_NTS,
        field == 1 ? (SQLCHAR*)excessive.data() : user, SQL_NTS,
        field == 2 ? (SQLCHAR*)excessive.data() : password, SQL_NTS));
    EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
    EXPECT_EQ(0, seen->created);
    EXPECT_EQ(SQL_ERROR, SQLConnectW(dbc,
        field == 0 ? wide_excessive.data() : wide_dsn, SQL_NTS,
        field == 1 ? wide_excessive.data() : wide_user, SQL_NTS,
        field == 2 ? wide_excessive.data() : wide_password, SQL_NTS));
    EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
    EXPECT_EQ(0, seen->created);
  }
  ASSERT_EQ(SQL_SUCCESS, SQLConnect(dbc, dsn, SQL_NTS, user, 2, password, 3));
  EXPECT_EQ("te", seen->settings.user); EXPECT_EQ("sec", seen->settings.password);
}

TEST_F(BackendContractTest, ConnectFieldsAcceptExactBootstrapLimit) {
  constexpr std::size_t cap = 1024 * 1024;
  std::string dsn = "SSL=0;MaxConnectionFieldBytes=1048576;DESCRIPTION=";
  dsn.resize(cap, 'x');
  std::string field(cap, 'u');
  ASSERT_EQ(SQL_SUCCESS, SQLConnect(dbc, (SQLCHAR*)dsn.data(), SQL_NTS,
      (SQLCHAR*)field.data(), SQL_NTS, (SQLCHAR*)field.data(), SQL_NTS));
  EXPECT_EQ(cap, seen->settings.user.size()); EXPECT_EQ(cap, seen->settings.password.size());
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  std::vector<SQLWCHAR> wide_dsn(dsn.begin(), dsn.end()); wide_dsn.push_back(0);
  std::vector<SQLWCHAR> wide_field(field.begin(), field.end()); wide_field.push_back(0);
  ASSERT_EQ(SQL_SUCCESS, SQLConnectW(dbc, wide_dsn.data(), SQL_NTS,
      wide_field.data(), SQL_NTS, wide_field.data(), SQL_NTS));
  EXPECT_EQ(cap, seen->settings.user.size()); EXPECT_EQ(cap, seen->settings.password.size());
}

TEST_F(BackendContractTest, WideConnectFieldsKeepMalformedAndExplicitEmptySemantics) {
  SQLWCHAR dsn[]{'S','S','L','=','0',0}, malformed[]{0xd800,0}, text[]{'x',0};
  for (int field = 0; field < 3; ++field) {
    EXPECT_EQ(SQL_ERROR, SQLConnectW(dbc,
        field == 0 ? malformed : dsn, static_cast<SQLSMALLINT>(field == 0 ? 1 : SQL_NTS),
        field == 1 ? malformed : text, 1,
        field == 2 ? malformed : text, 1));
    EXPECT_EQ("22018", state(SQL_HANDLE_DBC, dbc)); EXPECT_EQ(0, seen->created);
  }
  ASSERT_EQ(SQL_SUCCESS, SQLConnectW(dbc, dsn, SQL_NTS, text, 0, text, 0));
  EXPECT_EQ("", seen->settings.user); EXPECT_EQ("", seen->settings.password);
}

TEST_F(BackendContractTest, WideDriverCapturePreservesUnicodeAndLengthErrors) {
  SQLWCHAR invalid[]{0xd800, 0};
  SQLWCHAR output[]{'o', 0}; SQLSMALLINT output_length = 77;
  for (const SQLSMALLINT length : {SQLSMALLINT(1), SQLSMALLINT(SQL_NTS)}) {
    EXPECT_EQ(SQL_ERROR, SQLDriverConnectW(dbc, nullptr, invalid, length,
        output, 2, &output_length, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ("22018", state(SQL_HANDLE_DBC, dbc));
    EXPECT_EQ(0, seen->created); EXPECT_EQ(77, output_length); EXPECT_EQ(SQLWCHAR('o'), output[0]);
  }
  EXPECT_EQ(SQL_ERROR, SQLDriverConnectW(dbc, nullptr, invalid, -2,
      output, 2, &output_length, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("HY090", state(SQL_HANDLE_DBC, dbc)); EXPECT_EQ(0, seen->created);
  SQLWCHAR valid[]{'S','S','L','=','0','x'};
  ASSERT_EQ(SQL_SUCCESS, SQLDriverConnectW(dbc, nullptr, valid, 5,
      nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_FALSE(seen->settings.use_ssl);
}


TEST_F(BackendContractTest, NativeSqlCaptureLimitsPreserveOutputsAndSkipTranslation) {
  connect_with("SSL=0;MaxSqlBytes=4");
  SQLCHAR narrow_output[]{'o', 'k', 0};
  SQLWCHAR wide_output[]{'o', 'k', 0};
  SQLINTEGER output_length = 77;
  SQLCHAR input[]{'r', 'o', 'w', 's', 'x', 0};
  SQLWCHAR wide_input[]{'r', 'o', 'w', 's', 'x', 0};
  for (const SQLINTEGER length : {SQLINTEGER(5), SQLINTEGER(SQL_NTS)}) {
    EXPECT_EQ(SQL_ERROR, SQLNativeSql(dbc, input, length,
        narrow_output, 3, &output_length));
    EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
    EXPECT_EQ(0, seen->translations); EXPECT_EQ(77, output_length);
    EXPECT_STREQ("ok", (const char*)narrow_output);
    EXPECT_EQ(SQL_ERROR, SQLNativeSqlW(dbc, wide_input, length,
        wide_output, 3, &output_length));
    EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
    EXPECT_EQ(0, seen->translations); EXPECT_EQ(77, output_length);
    EXPECT_EQ(SQLWCHAR('o'), wide_output[0]); EXPECT_EQ(SQLWCHAR('k'), wide_output[1]);
  }
  input[4] = 0; wide_input[4] = 0;
  for (const SQLINTEGER length : {SQLINTEGER(4), SQLINTEGER(SQL_NTS)}) {
    SQLCHAR result[32]{}; SQLWCHAR wide_result[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLNativeSql(dbc, input, length, result, 32, &output_length));
    EXPECT_STREQ("native:rows", (const char*)result); EXPECT_EQ(11, output_length);
    ASSERT_EQ(SQL_SUCCESS, SQLNativeSqlW(dbc, wide_input, length, wide_result, 32, &output_length));
    EXPECT_EQ(11, output_length); EXPECT_EQ(SQLWCHAR('n'), wide_result[0]);
  }
  EXPECT_EQ(4, seen->translations); EXPECT_EQ(0, seen->queries); EXPECT_EQ(0, seen->disconnects);
}

TEST_F(BackendContractTest, NativeSqlWideLimitsCountExpansionAndKeepUnicodeErrors) {
  connect_with("SSL=0;MaxSqlBytes=4");
  SQLWCHAR input[]{0x20ac, 0x20ac, 0}, output[]{'o', 0};
  SQLINTEGER output_length = 77;
  for (const SQLINTEGER length : {SQLINTEGER(2), SQLINTEGER(SQL_NTS)}) {
    EXPECT_EQ(SQL_ERROR, SQLNativeSqlW(dbc, input, length, output, 2, &output_length));
    EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
    EXPECT_EQ(0, seen->translations); EXPECT_EQ(77, output_length); EXPECT_EQ(SQLWCHAR('o'), output[0]);
  }
  input[0] = 0xd800; input[1] = 0;
  EXPECT_EQ(SQL_ERROR, SQLNativeSqlW(dbc, input, 1, output, 2, &output_length));
  EXPECT_EQ("22018", state(SQL_HANDLE_DBC, dbc)); EXPECT_EQ(0, seen->translations);
  input[0] = 0x20ac; input[1] = 'x';
  SQLWCHAR result[32]{};
  ASSERT_EQ(SQL_SUCCESS, SQLNativeSqlW(dbc, input, SQL_NTS, result, 32, &output_length));
  EXPECT_EQ(9, output_length); EXPECT_EQ(SQLWCHAR(0x20ac), result[7]); EXPECT_EQ(SQLWCHAR('x'), result[8]);
  EXPECT_EQ(1, seen->translations);
}

TEST_F(BackendContractTest, NativeSqlZeroBudgetAllowsEmptyAndKeepsValidationPrecedence) {
  connect_with("SSL=0;MaxSqlBytes=0");
  SQLCHAR empty[]{0}; SQLWCHAR wide_empty[]{0}; SQLINTEGER output_length = 77;
  ASSERT_EQ(SQL_SUCCESS, SQLNativeSql(dbc, empty, SQL_NTS, nullptr, 0, &output_length));
  EXPECT_EQ(7, output_length);
  ASSERT_EQ(SQL_SUCCESS, SQLNativeSqlW(dbc, wide_empty, 0, nullptr, 0, &output_length));
  EXPECT_EQ(7, output_length);
  SQLCHAR input[]{'x', 0};
  EXPECT_EQ(SQL_ERROR, SQLNativeSql(dbc, input, SQL_NTS, nullptr, 0, &output_length));
  EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc)); EXPECT_EQ(2, seen->translations);
  EXPECT_EQ(SQL_ERROR, SQLNativeSql(dbc, input, -2, nullptr, 0, &output_length));
  EXPECT_EQ("HY090", state(SQL_HANDLE_DBC, dbc));
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  EXPECT_EQ(SQL_ERROR, SQLNativeSql(dbc, input, SQL_NTS, nullptr, 0, &output_length));
  EXPECT_EQ("08003", state(SQL_HANDLE_DBC, dbc)); EXPECT_EQ(2, seen->translations);
}


TEST_F(BackendContractTest, ColumnMetadataUsesNormalizedTypesWithoutNativeInterpretation) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  SQLCHAR name[32]{}; SQLSMALLINT name_length{}, type{}, digits{}, nullable{}; SQLULEN size{};
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, 1, name, 32, &name_length, &type, &size, &digits, &nullable));
  EXPECT_EQ(SQL_VARBINARY, type); EXPECT_EQ(8u, size);
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, 2, name, 32, &name_length, &type, &size, &digits, &nullable));
  EXPECT_EQ(SQL_BIT, type); EXPECT_EQ(1u, size);
}

TEST_F(BackendContractTest, MissingNormalizedColumnMetadataRejectsContractAndAllowsRecovery) {
  connect();
  EXPECT_EQ(SQL_ERROR, execute("unnormalized")); EXPECT_EQ("HY000", state());
  EXPECT_EQ(0, seen->disconnects);
  SQLSMALLINT columns = 99;
  EXPECT_EQ(SQL_ERROR, SQLNumResultCols(stmt, &columns)); EXPECT_EQ(99, columns);
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
}


TEST_F(BackendContractTest, ParameterDescriptionsUseNormalizedMetadataWithoutNativeLookup) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(SQL_VARBINARY, type); EXPECT_EQ(8u, size);
  EXPECT_EQ(1, seen->descriptions);
}

TEST_F(BackendContractTest, InvalidParameterDescriptionsPreserveOutputAndRecover) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLSMALLINT type = 77, digits = 78, nullable = 79; SQLULEN size = 80;
  for (const bool resolver_error : {false, true}) {
    seen->missing_parameter_metadata = !resolver_error;
    seen->parameter_metadata_error = resolver_error;
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
    EXPECT_EQ("HY000", state());
    EXPECT_EQ(77, type); EXPECT_EQ(78, digits); EXPECT_EQ(79, nullable); EXPECT_EQ(80u, size);
    EXPECT_EQ(0, seen->disconnects);
  }
  seen->missing_parameter_metadata = false; seen->parameter_metadata_error = false;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(SQL_VARBINARY, type); EXPECT_EQ(8u, size);
}


TEST_F(BackendContractTest, InvalidCellErrorCoordinatesRejectContractAndRecover) {
  connect();
  for (int invalid = 1; invalid <= 5; ++invalid) {
    seen->invalid_cell_errors = invalid;
    EXPECT_EQ(SQL_ERROR, execute("rows")); EXPECT_EQ("HY000", state());
    EXPECT_EQ(0, seen->disconnects);
  }
  seen->invalid_cell_errors = 0;
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  unsigned char output[3]{}; SQLLEN length{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY, output, 3, &length));
  EXPECT_EQ(3, length); EXPECT_EQ(0, output[0]); EXPECT_EQ(255, output[1]);
}

TEST_F(BackendContractTest, TextEncodingErrorsAreDeferredForAllCharacterTargets) {
  connect();
  seen->malformed_text = true;
  for (const SQLSMALLINT target : {SQLSMALLINT(SQL_C_CHAR), SQLSMALLINT(SQL_C_WCHAR), SQLSMALLINT(SQL_C_BINARY)}) {
    SCOPED_TRACE(target);
    alignas(SQLWCHAR) unsigned char output[32];
    std::fill(std::begin(output), std::end(output), 0x5a);
    SQLLEN length = 73;
    ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); // unrequested malformed text is deferred
    EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 3, target, output, sizeof(output), &length));
    EXPECT_EQ("22018", state()); EXPECT_EQ(73, length);
    EXPECT_TRUE(std::all_of(std::begin(output), std::end(output), [](auto byte) { return byte == 0x5a; }));
    EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 3, target, output, sizeof(output), &length));
    EXPECT_EQ(73, length); // no chunk offset or indicator is advanced by failure
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, target, output, sizeof(output), &length));
    const std::string text = "\xe2\x82\xac\xf0\x9f\x98\x80";
    if (target == SQL_C_WCHAR) {
      const auto wide = rs::odbc::utf8_to_wide(text); ASSERT_TRUE(wide);
      EXPECT_EQ(static_cast<SQLLEN>(wide->size() * sizeof(SQLWCHAR)), length);
      EXPECT_EQ(0, std::memcmp(output, wide->data(), static_cast<std::size_t>(length)));
    } else {
      EXPECT_EQ(static_cast<SQLLEN>(text.size()), length);
      EXPECT_EQ(0, std::memcmp(output, text.data(), text.size()));
    }
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(BackendContractTest, BoundTextEncodingErrorsPreserveBuffersAndRecoverOnNextRow) {
  connect();
  seen->malformed_text = true;
  SQLUSMALLINT row_status = SQL_ROW_SUCCESS;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &row_status, 0));
  for (const SQLSMALLINT target : {SQLSMALLINT(SQL_C_CHAR), SQLSMALLINT(SQL_C_WCHAR), SQLSMALLINT(SQL_C_BINARY)}) {
    SCOPED_TRACE(target);
    alignas(SQLWCHAR) unsigned char output[32];
    std::fill(std::begin(output), std::end(output), 0x5a);
    SQLLEN length = 73;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 3, target, output, sizeof(output), &length));
    ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    EXPECT_EQ(SQL_ERROR, SQLFetch(stmt)); EXPECT_EQ("22018", state());
    EXPECT_EQ(SQL_ROW_ERROR, row_status); EXPECT_EQ(73, length);
    EXPECT_TRUE(std::all_of(std::begin(output), std::end(output), [](auto byte) { return byte == 0x5a; }));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_SUCCESS, row_status);
    EXPECT_GT(length, 0);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
  }
}

TEST_F(BackendContractTest, InvalidResultStructureRejectsPrimaryAndAdditionalResultsAndRecovers) {
  connect();
  for (const int invalid : {1, 2, 3, 4, 5, 6, 7}) {
    SCOPED_TRACE(invalid);
    seen->invalid_result_structure = invalid;
    EXPECT_EQ(SQL_ERROR, execute("rows")); EXPECT_EQ("HY000", state());
    EXPECT_EQ(0, seen->disconnects);
    SQLSMALLINT columns = 99;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(stmt, &columns)); EXPECT_EQ(99, columns);
    seen->invalid_result_structure = 0;
    ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(BackendContractTest, EmptyAndUnicodeColumnNamesMapToAnsiAndWideMetadata) {
  connect();
  for (const int name_kind : {8, 9}) {
    seen->invalid_result_structure = name_kind;
    ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    const std::string expected = name_kind == 8 ? "" : "\xe2\x82\xac\xf0\x9f\x98\x80";
    SQLCHAR name[32]{}; SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, 1, name, sizeof(name), &length, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(static_cast<SQLSMALLINT>(expected.size()), length);
    EXPECT_EQ(expected, reinterpret_cast<const char*>(name));
    SQLWCHAR wide[32]{}; length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(stmt, 1, wide, 32, &length, nullptr, nullptr, nullptr, nullptr));
    const auto expected_wide = rs::odbc::utf8_to_wide(expected); ASSERT_TRUE(expected_wide);
    EXPECT_EQ(static_cast<SQLSMALLINT>(expected_wide->size()), length);
    EXPECT_TRUE(std::equal(expected_wide->begin(), expected_wide->end(), wide));
    EXPECT_EQ(0, wide[expected_wide->size()]);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(BackendContractTest, BackendMetadataErrorsUseGeneralStateAndAllowDirectAndPreparedRecovery) {
  connect();
  EXPECT_EQ(SQL_ERROR, execute("metadata_failure")); EXPECT_EQ("HY000", state());
  EXPECT_EQ(0, seen->disconnects);
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"metadata_failure", SQL_NTS));
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY000", state());
  EXPECT_EQ(0, seen->disconnects);
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
}

TEST_F(BackendContractTest, DeferredMetadataErrorUsesGeneralStateAndClearsPendingResults) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, execute("deferred_metadata")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(SQL_ERROR, SQLMoreResults(stmt)); EXPECT_EQ("HY000", state());
  EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(stmt)); EXPECT_EQ(0, seen->disconnects);
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
}

TEST_F(BackendContractTest, ServerVersionUsesTypedBackendServiceForAnsiAndWideInfo) {
  connect();
  for (const auto& [raw, expected] : {
      std::pair{"1.0", "01.00.0000"}, std::pair{"17.11 (vendor)", "17.11.0000"},
      std::pair{"", "00.00.0000"}, std::pair{"invalid", "00.00.0000"}}) {
    SCOPED_TRACE(raw);
    seen->server_version = raw;
    SQLCHAR version[16]{}; SQLSMALLINT length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_DBMS_VER, version, sizeof(version), &length));
    EXPECT_STREQ(expected, reinterpret_cast<const char*>(version)); EXPECT_EQ(10, length);
    SQLWCHAR wide[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfoW(dbc, SQL_DBMS_VER, wide, sizeof(wide), &length));
    EXPECT_EQ(10 * sizeof(SQLWCHAR), static_cast<std::size_t>(length));
    const auto expected_wide = rs::odbc::utf8_to_wide(expected); ASSERT_TRUE(expected_wide);
    EXPECT_TRUE(std::equal(expected_wide->begin(), expected_wide->end(), wide));
    EXPECT_EQ(0, wide[10]);
    EXPECT_EQ(0, seen->queries); EXPECT_EQ(0, seen->descriptions);
  }
}

TEST_F(BackendContractTest, InvalidExecutionSequenceRejectsAtomicallyAndRecovers) {
  connect();
  for (const bool prepared : {false, true}) {
    for (const int invalid : {1, 2, 3, 4, 5, 6, 7, 8}) {
      SCOPED_TRACE(prepared);
      SCOPED_TRACE(invalid);
      seen->invalid_execution_shape = invalid;
      if (prepared) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows", SQL_NTS));
        EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
      } else {
        EXPECT_EQ(SQL_ERROR, execute("rows"));
      }
      EXPECT_EQ("HY000", state()); EXPECT_EQ(0, seen->disconnects);
      EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(stmt));
      seen->invalid_execution_shape = 0;
      ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    }
  }
}

TEST_F(BackendContractTest, OrderedCountsEmptyRowsetAndDeferredErrorRemainDistinct) {
  connect();
  for (const bool prepared : {false, true}) {
    if (prepared) {
      ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"ordered", SQL_NTS));
      ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    } else {
      ASSERT_EQ(SQL_SUCCESS, execute("ordered"));
    }
    SQLLEN count = -1; SQLSMALLINT columns = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(stmt, &count)); EXPECT_EQ(7, count);
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt, &columns)); EXPECT_EQ(0, columns);
    ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt, &columns)); EXPECT_EQ(3, columns);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(stmt, &count)); EXPECT_EQ(0, count);
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt, &columns)); EXPECT_EQ(0, columns);
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(stmt)); EXPECT_EQ("22018", state());
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(stmt)); EXPECT_EQ(0, seen->disconnects);
    ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(BackendContractTest, InvalidDescriptionSequencePreservesOutputsAndRetriesWithoutCachedSuccess) {
  connect();
  for (const int invalid : {1, 2, 3, 4, 5}) {
    SCOPED_TRACE(invalid);
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    seen->invalid_description_shape = invalid;
    SQLSMALLINT type = -17, digits = -19, nullable = -23, columns = -29;
    SQLULEN size = 12345;
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
    EXPECT_EQ("HY000", state());
    EXPECT_EQ(-17, type); EXPECT_EQ(12345u, size); EXPECT_EQ(-19, digits); EXPECT_EQ(-23, nullable);
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(stmt, &columns)); EXPECT_EQ(-29, columns);
    EXPECT_EQ(0, seen->disconnects);
    seen->invalid_description_shape = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
    EXPECT_EQ(SQL_VARBINARY, type);
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt, &columns)); EXPECT_EQ(3, columns);
    ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(BackendContractTest, ProviderDialectWorksBeforeSessionCreationAndOwnsResults) {
  SqlTranslationResult retained;
  {
    FakeProvider provider{seen};
    const auto& dialect = provider.sql_dialect();
    EXPECT_EQ(0, seen->created); EXPECT_EQ(0, seen->transports);
    std::string input = "hello ?";
    EXPECT_EQ(1u, dialect.count_parameter_markers(input));
    retained = dialect.translate_sql(input);
    ASSERT_TRUE(retained); EXPECT_EQ("native:hello ?", retained.sql);
    input.assign("overwritten"); EXPECT_EQ("native:hello ?", retained.sql);
    const auto rejected = dialect.translate_sql("unsupported");
    EXPECT_FALSE(rejected); EXPECT_EQ(SqlTranslationError::Unsupported, rejected.error);
    const auto recovered = dialect.translate_sql("again");
    ASSERT_TRUE(recovered); EXPECT_EQ("native:again", recovered.sql);
    EXPECT_EQ(0, seen->created); EXPECT_EQ(0, seen->queries); EXPECT_EQ(0, seen->descriptions);
  }
  EXPECT_EQ("native:hello ?", retained.sql);
}

TEST_F(BackendContractTest, AbsentTransactionFacetSuppressesLiveCapabilitiesAndRecoversWithoutIo) {
  seen->advertised_transactions = true;
  // A static provider declaration cannot grant a missing live facet.
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0));
  connect();
  SQLUSMALLINT capability = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_TXN_CAPABLE, &capability, sizeof(capability), nullptr));
  EXPECT_EQ(SQL_TC_NONE, capability);
  EXPECT_EQ(SQL_ERROR, execute("rows")); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(0, seen->queries); EXPECT_EQ(0, seen->disconnects);
  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(dbc, SQL_ATTR_TXN_ISOLATION,
      (SQLPOINTER)SQL_TXN_SERIALIZABLE, 0));
  EXPECT_EQ("HYC00", state(SQL_HANDLE_DBC, dbc)); EXPECT_EQ(0, seen->queries);
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      (SQLPOINTER)SQL_AUTOCOMMIT_ON, 0));
  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0));
  EXPECT_EQ("HYC00", state(SQL_HANDLE_DBC, dbc));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
}

TEST_F(BackendContractTest, AbsentFacetRejectsDeferredIsolationDuringOpenAndAllowsRetry) {
  seen->advertised_transactions = true;
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_TXN_ISOLATION,
      (SQLPOINTER)SQL_TXN_SERIALIZABLE, 0));
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr,
      (SQLCHAR*)"SERVER=fake;SSL=0", SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("HYC00", state(SQL_HANDLE_DBC, dbc));
  EXPECT_EQ(0, seen->queries);
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_TXN_ISOLATION,
      (SQLPOINTER)SQL_TXN_READ_COMMITTED, 0));
  connect();
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
}

TEST_F(BackendContractTest, AbsentDescriptionFacetRejectsMetadataWithoutIoAndExecutionStillWorks) {
  seen->absent_description = true;
  connect();
  SQLCHAR advertised[2]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_DESCRIBE_PARAMETER, advertised, sizeof(advertised), nullptr));
  EXPECT_STREQ("N", reinterpret_cast<char*>(advertised));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLSMALLINT type = -17, digits = -19, nullable = -23, columns = -29;
  SQLULEN size = 12345;
  for (int attempt = 0; attempt < 2; ++attempt) {
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
    EXPECT_EQ("HYC00", state());
    EXPECT_EQ(-17, type); EXPECT_EQ(12345u, size); EXPECT_EQ(-19, digits); EXPECT_EQ(-23, nullable);
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(stmt, &columns));
    EXPECT_EQ("HYC00", state()); EXPECT_EQ(-29, columns);
  }
  EXPECT_EQ(0, seen->descriptions); EXPECT_EQ(0, seen->queries); EXPECT_EQ(0, seen->disconnects);
  unsigned char input[]{0, 255}; SQLLEN length = sizeof(input);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_VARBINARY, sizeof(input), 0, input, sizeof(input), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(stmt, &columns)); EXPECT_EQ(3, columns);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(0, seen->descriptions); EXPECT_EQ(2, seen->queries); EXPECT_EQ(0, seen->disconnects);
}

TEST_F(BackendContractTest, AbsentCatalogFacetRejectsAllCatalogApisAndPreservesCursor) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  SQLCHAR name[] = "table";
  SQLWCHAR wide_name[]{'t', 'a', 'b', 'l', 'e', 0};
  const SQLUSMALLINT functions[]{SQL_API_SQLTABLES, SQL_API_SQLCOLUMNS,
      SQL_API_SQLPRIMARYKEYS, SQL_API_SQLFOREIGNKEYS, SQL_API_SQLSTATISTICS,
      SQL_API_SQLPROCEDURES, SQL_API_SQLPROCEDURECOLUMNS, SQL_API_SQLSPECIALCOLUMNS};
  SQLUSMALLINT legacy[100]{}, bitmap[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(dbc, SQL_API_ALL_FUNCTIONS, legacy));
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(dbc, SQL_API_ODBC3_ALL_FUNCTIONS, bitmap));
  for (const auto function : functions) {
    SCOPED_TRACE(function);
    SQLUSMALLINT supported = SQL_TRUE;
    ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(dbc, function, &supported));
    EXPECT_EQ(SQL_FALSE, supported); EXPECT_EQ(SQL_FALSE, legacy[function]);
    EXPECT_FALSE(SQL_FUNC_EXISTS(bitmap, function));
    for (const bool wide : {false, true}) {
      const auto invoke = [&]() -> SQLRETURN {
        switch (function) {
          case SQL_API_SQLTABLES: return wide
              ? SQLTablesW(stmt, nullptr, 0, nullptr, 0, wide_name, SQL_NTS, nullptr, 0)
              : SQLTables(stmt, nullptr, 0, nullptr, 0, name, SQL_NTS, nullptr, 0);
          case SQL_API_SQLCOLUMNS: return wide
              ? SQLColumnsW(stmt, nullptr, 0, nullptr, 0, wide_name, SQL_NTS, nullptr, 0)
              : SQLColumns(stmt, nullptr, 0, nullptr, 0, name, SQL_NTS, nullptr, 0);
          case SQL_API_SQLPRIMARYKEYS: return wide
              ? SQLPrimaryKeysW(stmt, nullptr, 0, nullptr, 0, wide_name, SQL_NTS)
              : SQLPrimaryKeys(stmt, nullptr, 0, nullptr, 0, name, SQL_NTS);
          case SQL_API_SQLFOREIGNKEYS: return wide
              ? SQLForeignKeysW(stmt, nullptr, 0, nullptr, 0, wide_name, SQL_NTS, nullptr, 0, nullptr, 0, nullptr, 0)
              : SQLForeignKeys(stmt, nullptr, 0, nullptr, 0, name, SQL_NTS, nullptr, 0, nullptr, 0, nullptr, 0);
          case SQL_API_SQLSTATISTICS: return wide
              ? SQLStatisticsW(stmt, nullptr, 0, nullptr, 0, wide_name, SQL_NTS, SQL_INDEX_ALL, SQL_QUICK)
              : SQLStatistics(stmt, nullptr, 0, nullptr, 0, name, SQL_NTS, SQL_INDEX_ALL, SQL_QUICK);
          case SQL_API_SQLPROCEDURES: return wide
              ? SQLProceduresW(stmt, nullptr, 0, nullptr, 0, wide_name, SQL_NTS)
              : SQLProcedures(stmt, nullptr, 0, nullptr, 0, name, SQL_NTS);
          case SQL_API_SQLPROCEDURECOLUMNS: return wide
              ? SQLProcedureColumnsW(stmt, nullptr, 0, nullptr, 0, wide_name, SQL_NTS, nullptr, 0)
              : SQLProcedureColumns(stmt, nullptr, 0, nullptr, 0, name, SQL_NTS, nullptr, 0);
          default: return wide
              ? SQLSpecialColumnsW(stmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0, wide_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE)
              : SQLSpecialColumns(stmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0, name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE);
        }
      };
      EXPECT_EQ(SQL_ERROR, invoke()); EXPECT_EQ("HYC00", state());
      EXPECT_EQ(1, seen->queries); EXPECT_EQ(0, seen->descriptions); EXPECT_EQ(0, seen->disconnects);
    }
  }
  SQLUSMALLINT supported{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(dbc, SQL_API_SQLEXECDIRECT, &supported)); EXPECT_EQ(SQL_TRUE, supported);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(2, seen->queries); EXPECT_EQ(0, seen->disconnects);
}

TEST_F(BackendContractTest, ProviderTypePolicyUsesAdvertisedVersionWithoutCatalogIo) {
  auto* connection = static_cast<rs::odbc::ODBCConnection*>(dbc);
  const auto before = connection->type_catalog();
  ASSERT_FALSE(before.empty()); EXPECT_EQ(8u, before.front().column_size);
  seen->server_version = "2.0";
  connect();
  const auto live = connection->type_catalog();
  ASSERT_FALSE(live.empty()); EXPECT_EQ(16u, live.front().column_size);
  EXPECT_EQ(8u, before.front().column_size);
  ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(stmt, SQL_VARBINARY));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  SQLUINTEGER size{}; SQLLEN indicator{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_ULONG, &size, sizeof(size), &indicator));
  EXPECT_EQ(16u, size);
  EXPECT_EQ(0, seen->queries); EXPECT_EQ(0, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  EXPECT_EQ(16u, live.front().column_size);
  EXPECT_EQ(16u, connection->type_catalog().front().column_size);
  seen->server_version.clear();
  ASSERT_FALSE(connection->type_catalog().empty());
  EXPECT_EQ(8u, connection->type_catalog().front().column_size);
}
