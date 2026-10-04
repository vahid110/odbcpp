#include "core/database/transaction_session.h"
#include "core/database/postgres/pg_backend_provider.h"
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

template<class T> concept ExposesRawPhysicalSession = requires(T& connection) { connection.get_db_connection(); };
static_assert(!ExposesRawPhysicalSession<rs::odbc::ODBCConnection>);

struct Observations {
  int created{}, transports{}, disconnects{}, destructions{}, queries{}, descriptions{}, translations{};
  bool validate_redshift_mode{false};
  bool catalog_executor{false}, catalog_builder{false};
  int catalog_calls{}, catalog_builds{}, catalog_error{};
  bool catalog_retire{}, catalog_throw{};
  int terminal_execution{}, connect_exception{};
  SQLULEN description_size{8};
  bool observation_exception{}, terminal_description{};
  ConnectionSettings settings;
  std::string sql;
  std::vector<QueryParameter> parameters;
  Deadline deadline{};
  bool malformed_value{}, malformed_state{}, malformed_text{}, long_binary{};
  std::optional<QueryResult> date_result;
  std::string failure_message = "fake error";
  std::string server_version = "1.0";
  bool setup_allocation_failure{false};
  bool advertised_transactions{false};
  bool absent_description{false};
  bool missing_parameter_metadata{}, parameter_metadata_error{};
  int isolation_mode{};
  // Opt-in BEGIN control only: legacy fixtures retain their original behavior.
  std::optional<BackendResult<void>> begin_result;
  std::optional<BackendResult<void>> end_result;
  std::optional<SessionState> transaction_state;
  std::vector<TransactionAction> transaction_calls;
  std::vector<Deadline> transaction_deadlines;
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
template <typename T> concept HasSessionCapabilities = requires(const T& session) { session.capabilities(); };
template <typename T> concept HasSessionErrorPolicy = requires(const T& session) { session.normalize_error_sqlstate("22012", ErrorContext::Unknown); };
static_assert(!HasSessionErrorPolicy<IDatabaseConnection>);
static_assert(!HasSessionCapabilities<IDatabaseConnection>);
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
class FakeBackend final : public IDatabaseConnection, public IStatementDescription, public ITransactionSession, public ICatalogExecution, public ICatalogQueries {
 public:
  explicit FakeBackend(std::shared_ptr<Observations> seen)
      : seen_(std::move(seen)), absent_description_(seen_->absent_description) {}
  ~FakeBackend() override { ++seen_->destructions; }
  BackendResult<void> connect(const ConnectionSettings& settings) override {
    if (seen_->connect_exception == 1) throw std::bad_alloc{};
    if (seen_->connect_exception == 2) throw std::runtime_error("fixture connect exception");
    if (seen_->setup_allocation_failure) return {DbErrorCode::AllocationFailure, {}};
    seen_->settings = settings; connected_ = true;
    if (seen_->connect_exception == 4) return BackendResult<void>{};
    if (seen_->connect_exception == 5) return BackendResult<void>{{SessionState::Transaction, SessionDisposition::ResetRequired}};
    return BackendResult<void>{{SessionState::Idle, SessionDisposition::Reusable}};
  }
  ICatalogExecution* catalog_execution() noexcept override { return seen_->catalog_executor ? this : nullptr; }
  const ICatalogQueries* catalog_queries() const noexcept override { return seen_->catalog_builder || seen_->catalog_executor ? this : nullptr; }
  rs::util::Result<std::string> catalog_query(const CatalogRequest&) const override {
    ++seen_->catalog_builds;
    return std::string("SELECT legacy fixture");
  }
  BackendResult<QueryResult> execute_catalog(const CatalogRequest& request, Deadline deadline) override {
    ++seen_->catalog_calls; seen_->deadline = deadline;
    if (seen_->catalog_throw) throw 42;
    EXPECT_TRUE(std::holds_alternative<PrimaryKeysCatalogRequest>(request));
    if (seen_->catalog_error) {
      BackendError error{rs::util::make_error_code(seen_->catalog_error == 2 ? DbErrorCode::Timeout : seen_->catalog_error == 3 ? DbErrorCode::QueryFailed : DbErrorCode::UnsupportedFeature), "catalog fixture"};
      error.operation = BackendOperation::ExecuteCatalog;
      if (seen_->catalog_error == 3) error.native_state = "42501";
      error.session_state = seen_->catalog_error == 2 ? SessionState::Disconnected : SessionState::Idle;
      error.disposition = seen_->catalog_error == 2 ? SessionDisposition::Retire : SessionDisposition::Reusable;
      return error;
    }
    return BackendResult<QueryResult>{rows(), {SessionState::Idle, seen_->catalog_retire ? SessionDisposition::Retire : SessionDisposition::Reusable}};
  }
  ITransactionSession* transaction_session() noexcept override { return seen_->isolation_mode ? this : nullptr; }
  TransactionCapabilities transaction_capabilities() const override {
    return {true, true, TransactionIsolation::ReadCommitted, {true, true, true, true}};
  }
  BackendResult<void> transaction(TransactionAction action, Deadline deadline) override {
    seen_->transaction_calls.push_back(action);
    seen_->transaction_deadlines.push_back(deadline);
    if (action == TransactionAction::Begin && seen_->begin_result) {
      seen_->transaction_state = seen_->begin_result->session_snapshot().state;
      return *seen_->begin_result;
    }
    if (action != TransactionAction::Begin && seen_->end_result) {
      seen_->transaction_state = seen_->end_result->session_snapshot().state;
      return *seen_->end_result;
    }
    if (seen_->begin_result) seen_->transaction_state = SessionState::Idle;
    return BackendResult<void>{{SessionState::Idle, SessionDisposition::Reusable}};
  }
  BackendResult<void> set_transaction_isolation(TransactionIsolation, Deadline) override {
    if (seen_->isolation_mode == 2) return BackendResult<void>{};
    if (seen_->isolation_mode == 3) return BackendResult<void>{{SessionState::Idle, SessionDisposition::ResetRequired}};
    if (seen_->isolation_mode == 4) seen_->connect_exception = 6;
    return BackendResult<void>{{SessionState::Idle, SessionDisposition::Reusable}};
  }
  void disconnect() override { connected_ = false; ++seen_->disconnects; }
  bool is_connected() const override { return connected_; }
  SessionState session_state() const override { return connected_ ? seen_->transaction_state.value_or(seen_->connect_exception == 6 ? SessionState::Transaction : seen_->connect_exception == 7 ? SessionState::Unknown : SessionState::Idle) : SessionState::Disconnected; }
  QueryResult rows() const {
    if (seen_->date_result) return *seen_->date_result;
    QueryResult result;
    result.columns = {{"binary", {}}, {"flag", {}}, {"text", {}}};
    result.columns[0].normalized_type = NativeTypeInfo{seen_->long_binary ? ScalarType::LongVarBinary : ScalarType::Binary,
        seen_->long_binary ? 0u : 8u, 0, true};
    result.columns[1].normalized_type = NativeTypeInfo{ScalarType::Boolean, 1, 0, true};
    result.columns[2].normalized_type = NativeTypeInfo{ScalarType::VarChar, 32, 0, true};
    result.rows = {{std::string("\0\xff\\", 3), "1", std::nullopt}, {"", "0", ""}};
    if (seen_->long_binary) result.rows.push_back({std::nullopt, "0", ""});
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
    if (seen_->terminal_execution == 1) return BackendResult<QueryResult>{rows(), {SessionState::Unknown, SessionDisposition::Retire}};
    if (seen_->terminal_execution == 2) {
      BackendError error{rs::util::make_error_code(DbErrorCode::QueryFailed), seen_->failure_message};
      error.native_state = "FAKE_ERROR"; error.operation = BackendOperation::ExecuteDirect;
      return error;
    }
    if (seen_->terminal_execution == 3) throw std::runtime_error("fixture execution exception");
    if (seen_->terminal_execution == 4) return BackendResult<QueryResult>{rows()};
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
    return BackendResult<QueryResult>{std::move(result), {SessionState::Idle, SessionDisposition::Reusable}};
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
        NativeTypeInfo{ScalarType::Binary, seen_->description_size, 0, true});
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
      error.session_state = SessionState::Idle; error.disposition = SessionDisposition::Reusable;
      error.native_state = "FAKE_ERROR";
      return error;
    }
    if (seen_->terminal_description) return BackendResult<QueryResult>{std::move(result)};
    return BackendResult<QueryResult>{std::move(result), {SessionState::Idle, SessionDisposition::Reusable}};
  }
  std::string server_version() const override {
    if (seen_->observation_exception) throw std::runtime_error("fixture observation exception");
    return seen_->server_version;
  }
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
  explicit FakeProvider(std::shared_ptr<Observations> seen,
      std::shared_ptr<const IBackendProvider> profile = {})
      : seen_(std::move(seen)), dialect_(seen_), profile_(std::move(profile)) {}

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
    if (profile_) return profile_->capabilities();
    BackendCapabilities result;
    result.dbms_name = "ContractDB";
    result.describe_parameters = true;
    result.max_identifier_length = 117;
    result.identifier_case = IdentifierCase::Upper;
    result.null_collation = NullCollation::Low;
    return result;
  }
  std::span<const TypeDefinition> type_catalog(std::string_view version = {}) const noexcept override {
    if (profile_) return profile_->type_catalog(version);
    static const TypeDefinition types[]{
        {ScalarType::Binary, "octets", 8, {}, {}, {}, false, {}, {}, {}, 0},
        {ScalarType::Boolean, "truth", 1, {}, {}, {}, false, {}, {}, {}, 0},
        {ScalarType::VarChar, "words", 32, {}, {}, {}, true, {}, {}, {}, 0}};
    static const TypeDefinition modern[]{
        {ScalarType::Binary, "octets", 16, {}, {}, {}, false, {}, {}, {}, 0},
        types[1], types[2]};
    return version == "2.0" ? std::span<const TypeDefinition>(modern) : std::span<const TypeDefinition>(types);
  }
  std::optional<std::string> normalize_error_sqlstate(std::string_view state, ErrorContext) const override {
    if (seen_->malformed_state) return "bad";
    if (state == "FAKE_ERROR") return "22018";
    if (state == "42501") return "42501";
    return std::nullopt;
  }
  TransactionCapabilities transaction_capabilities() const noexcept override {
    return seen_->advertised_transactions
        ? TransactionCapabilities{true, true, TransactionIsolation::ReadCommitted, {true, true, true, true}}
        : TransactionCapabilities{};
  }
  Result<ConnectionSettings> resolve_connection_options(
      ConnectionOptions options) const override {
    if (seen_->validate_redshift_mode) {
      rs::core::database::postgres::PgBackendProvider provider{
          BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
          BackendConnectionDefaults{"host", 5439, "db", true}, std::nullopt,
          rs::core::database::postgres::PgCatalogProfile::Redshift};
      return provider.resolve_connection_options(std::move(options));
    }
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
  std::shared_ptr<const IBackendProvider> profile_;
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

// These tests specify the prerequisite BEGIN boundary only. They do not enable
// manual-transaction PrimaryKeys or model native PostgreSQL/Redshift acceptance.
TEST_F(BackendContractTest, BeginPreservesVerifiedTransactionSnapshotAndOriginalDeadline) {
  seen->advertised_transactions = true; seen->isolation_mode = 1; connect();
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0));
  const SessionSnapshot expected{SessionState::Transaction, SessionDisposition::ResetRequired};
  seen->begin_result = BackendResult<void>{expected};
  const auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
  auto result = connection->begin_transaction_if_needed(deadline);
  ASSERT_TRUE(result);
  EXPECT_EQ(expected, result.session_snapshot());
  EXPECT_TRUE(connection->is_connected());
  ASSERT_EQ(1u, seen->transaction_calls.size());
  EXPECT_EQ(TransactionAction::Begin, seen->transaction_calls[0]);
  ASSERT_EQ(1u, seen->transaction_deadlines.size());
  EXPECT_EQ(deadline, seen->transaction_deadlines[0]);
  EXPECT_EQ(0, seen->queries);
  ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK));
  ASSERT_EQ(2u, seen->transaction_calls.size());
  EXPECT_EQ(TransactionAction::Rollback, seen->transaction_calls[1]);
}

class BeginSnapshotRefusalTest : public BackendContractTest,
                                public ::testing::WithParamInterface<SessionSnapshot> {};

TEST_F(BackendContractTest, RetiredEndTransactionClearsConnectionAndPreventsFurtherDispatch) {
  seen->advertised_transactions = true; seen->isolation_mode = 1; connect();
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0));
  seen->begin_result = BackendResult<void>{{SessionState::Transaction, SessionDisposition::ResetRequired}};
  const auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  ASSERT_TRUE(connection->begin_transaction_if_needed(Deadline::max()));
  BackendError error{rs::util::make_error_code(DbErrorCode::ProtocolError),
                     "Unexpected transaction completion state"};
  error.operation = BackendOperation::CommitTransaction;
  error.session_state = SessionState::Disconnected;
  error.disposition = SessionDisposition::Retire;
  seen->end_result = BackendResult<void>{error};
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_COMMIT));
  EXPECT_FALSE(connection->is_connected());
  ASSERT_EQ(2u, seen->transaction_calls.size());
  EXPECT_EQ(TransactionAction::Commit, seen->transaction_calls.back());
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK));
  EXPECT_EQ(SQL_ERROR, execute("SELECT must_not_dispatch"));
  EXPECT_EQ(2u, seen->transaction_calls.size());
  EXPECT_EQ(0, seen->queries);
}

TEST_P(BeginSnapshotRefusalTest, InvalidSuccessfulBeginRetiresBeforeQuery) {
  seen->advertised_transactions = true; seen->isolation_mode = 1; connect();
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0));
  seen->begin_result = BackendResult<void>{GetParam()};
  EXPECT_EQ(SQL_ERROR, execute("SELECT must_not_dispatch"));
  EXPECT_EQ(0, seen->queries);
  const auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  EXPECT_FALSE(connection->is_connected());
  ASSERT_EQ(1u, seen->transaction_calls.size());
  EXPECT_EQ(TransactionAction::Begin, seen->transaction_calls[0]);
  // No false active-transaction flag may dispatch cleanup after retirement.
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK));
  EXPECT_EQ(1u, seen->transaction_calls.size());
  EXPECT_EQ(SQL_ERROR, execute("SELECT still_not_dispatch"));
  EXPECT_EQ(0, seen->queries);
}

INSTANTIATE_TEST_SUITE_P(ConservativeBeginSnapshots, BeginSnapshotRefusalTest,
    ::testing::Values(
        SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable},
        SessionSnapshot{SessionState::Unknown, SessionDisposition::ResetRequired},
        SessionSnapshot{SessionState::Disconnected, SessionDisposition::Reusable},
        SessionSnapshot{SessionState::FailedTransaction, SessionDisposition::ResetRequired},
        SessionSnapshot{SessionState::Transaction, SessionDisposition::Reusable},
        SessionSnapshot{SessionState::Transaction, SessionDisposition::Retire},
        SessionSnapshot{SessionState::Idle, SessionDisposition::Retire},
        SessionSnapshot{SessionState::Unknown, SessionDisposition::Retire},
        SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}));

class FailedBeginSnapshotTest : public BackendContractTest,
                               public ::testing::WithParamInterface<SessionSnapshot> {};

TEST_P(FailedBeginSnapshotTest, PreservesOriginalErrorAndReconcilesWithoutExecutingQuery) {
  seen->advertised_transactions = true; seen->isolation_mode = 1; connect();
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0));
  const auto snapshot = GetParam();
  BackendError error{rs::util::make_error_code(DbErrorCode::QueryFailed), "controlled BEGIN rejection"};
  error.native_state = "42501"; error.native_code = 731;
  error.operation = BackendOperation::BeginTransaction;
  error.session_state = snapshot.state; error.disposition = snapshot.disposition;
  error.retry_safe = false;
  seen->begin_result = BackendResult<void>{error};
  const auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
  auto result = connection->begin_transaction_if_needed(deadline);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(error.code, result.error());
  EXPECT_EQ(error.message, result.error_message());
  EXPECT_EQ(error.error_class, result.backend_error().error_class);
  EXPECT_EQ(error.native_state, result.backend_error().native_state);
  EXPECT_EQ(error.native_code, result.backend_error().native_code);
  EXPECT_EQ(error.operation, result.backend_error().operation);
  EXPECT_EQ(error.retry_safe, result.backend_error().retry_safe);
  EXPECT_EQ(snapshot, result.session_snapshot());
  EXPECT_EQ(0, seen->queries);
  ASSERT_EQ(1u, seen->transaction_calls.size());
  ASSERT_EQ(1u, seen->transaction_deadlines.size());
  EXPECT_EQ(deadline, seen->transaction_deadlines[0]);
  const bool terminal = snapshot.disposition == SessionDisposition::Retire ||
      snapshot.state == SessionState::Unknown || snapshot.state == SessionState::Disconnected;
  EXPECT_EQ(!terminal, connection->is_connected());
  if (terminal) {
    EXPECT_EQ(SQL_ERROR, execute("SELECT must_not_dispatch"));
    EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK));
    EXPECT_EQ(1u, seen->transaction_calls.size());
  } else if (snapshot.state == SessionState::Idle) {
    // A failed Idle BEGIN leaves no active flag: a second statement must BEGIN
    // again, encounter the same original failure and never execute its query.
    EXPECT_EQ(SQL_ERROR, execute("SELECT must_not_dispatch"));
    EXPECT_EQ(2u, seen->transaction_calls.size());
    EXPECT_EQ(TransactionAction::Begin, seen->transaction_calls.back());
    EXPECT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK));
    EXPECT_EQ(2u, seen->transaction_calls.size());
  } else {
    // The error may still leave a real transaction or failed transaction. Do
    // not lose that ownership: explicit rollback must reach the backend once.
    EXPECT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK));
    ASSERT_EQ(2u, seen->transaction_calls.size());
    EXPECT_EQ(TransactionAction::Rollback, seen->transaction_calls.back());
  }
  EXPECT_EQ(0, seen->queries);
}

INSTANTIATE_TEST_SUITE_P(OriginalBeginFailures, FailedBeginSnapshotTest,
    ::testing::Values(
        SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable},
        SessionSnapshot{SessionState::Transaction, SessionDisposition::ResetRequired},
        SessionSnapshot{SessionState::FailedTransaction, SessionDisposition::ResetRequired},
        SessionSnapshot{SessionState::Unknown, SessionDisposition::ResetRequired},
        SessionSnapshot{SessionState::Idle, SessionDisposition::Retire},
        SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}));

TEST_F(BackendContractTest, PrimaryKeysExecutorUsesDeadlineAndPreservesPendingCursor) {
  seen->catalog_executor = true; connect();
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(std::uintptr_t{3}), 0));
  SQLINTEGER bound = 77; SQLLEN bound_length = 88;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 2, SQL_C_LONG, &bound, sizeof(bound), &bound_length));
  const auto start = std::chrono::steady_clock::now();
  ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ(1, seen->catalog_calls); EXPECT_EQ(0, seen->queries); EXPECT_EQ(0, seen->catalog_builds);
  EXPECT_GE(seen->deadline, start + std::chrono::seconds{3});
  EXPECT_LE(seen->deadline, std::chrono::steady_clock::now() + std::chrono::seconds{3});
  ASSERT_EQ(SQL_ERROR, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ("24000", state()); EXPECT_EQ(1, seen->catalog_calls);
  EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt)); // Original cursor and application binding survive refusal.
  EXPECT_EQ(1, bound); EXPECT_EQ(static_cast<SQLLEN>(sizeof(bound)), bound_length);
}

TEST_F(BackendContractTest, PrimaryKeysExecutorNeverFallsBackAndRejectsManualTransactions) {
  seen->catalog_executor = true; seen->advertised_transactions = true; seen->isolation_mode = 1; connect();
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0));
  EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ("HYC00", state()); EXPECT_EQ(0, seen->catalog_calls); EXPECT_EQ(0, seen->queries);
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT,
      reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0));
  seen->catalog_error = 1;
  EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ("HYC00", state()); EXPECT_EQ(1, seen->catalog_calls); EXPECT_EQ(0, seen->catalog_builds);
  seen->catalog_error = 2;
  EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ(2, seen->catalog_calls); EXPECT_EQ(0, seen->catalog_builds);
  EXPECT_EQ(SQL_ERROR, execute("SELECT after timeout"));
  EXPECT_EQ(0, seen->queries);
}

TEST_F(BackendContractTest, PrimaryKeysNativeErrorAndRetiredOwningResultsPreserveSemantics) {
  seen->catalog_executor = true; connect(); seen->catalog_error = 3;
  EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ("42501", state()); EXPECT_EQ(0, seen->catalog_builds);
  seen->catalog_error = 0; seen->catalog_retire = true;
  ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt)); // Installed owning result survives retirement.
  const auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  EXPECT_FALSE(connection->is_connected());
}

TEST_F(BackendContractTest, PrimaryKeysNonStandardExceptionRetiresWithoutFallback) {
  seen->catalog_executor = true; connect(); seen->catalog_throw = true;
  EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ("HY000", state()); EXPECT_EQ(0, seen->catalog_builds);
  const auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  EXPECT_FALSE(connection->is_connected());
}

TEST_F(BackendContractTest, PrimaryKeysInspectionExceptionSynchronizesConnectionRetirement) {
  seen->catalog_executor = true; connect(); seen->observation_exception = true;
  EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ("HY000", state()); EXPECT_EQ(0, seen->catalog_calls); EXPECT_EQ(0, seen->queries);
  const auto connection = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);
  EXPECT_FALSE(connection->is_connected());
}

TEST_F(BackendContractTest, PrimaryKeysNonIdleStatesPerformNoExecutionOrFallback) {
  seen->catalog_executor = true; connect();
  for (int mode : {6, 7}) {
    seen->connect_exception = mode;
    EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
        (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
    EXPECT_EQ("HYC00", state());
    EXPECT_EQ(0, seen->catalog_calls); EXPECT_EQ(0, seen->catalog_builds); EXPECT_EQ(0, seen->queries);
  }
}

TEST_F(BackendContractTest, PrimaryKeysAbsentExecutorUsesExistingCatalogBuilder) {
  seen->catalog_builder = true; connect();
  EXPECT_EQ(SQL_SUCCESS, SQLPrimaryKeys(stmt, (SQLCHAR*)"contract", SQL_NTS,
      (SQLCHAR*)"schema", SQL_NTS, (SQLCHAR*)"table", SQL_NTS));
  EXPECT_EQ(0, seen->catalog_calls); EXPECT_EQ(1, seen->catalog_builds); EXPECT_EQ(1, seen->queries);
}

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

TEST_F(BackendContractTest, MetadataCacheEventsExposeNoSqlContents) {
  const auto directory = std::filesystem::temp_directory_path() /
      ("odbcpp-metadata-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const auto path = directory / "driver.log";
  connect_with("SERVER=fake;SSL=0;LogLevel=Debug;LogQueries=false;LogAsync=false;LogSink=File;LogFile=" + path.string());
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"private_sql_canary_781 ?", SQL_NTS));
  SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  SQLHSTMT other{}; ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &other));
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other, (SQLCHAR*)"rows", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc)); dbc = nullptr;
  std::ifstream input(path);
  const std::string contents((std::istreambuf_iterator<char>(input)), {}); input.close();
  for (const char* event : {"prepared_metadata_cache_miss", "prepared_metadata_cache_hit",
      "prepared_metadata_cache_invalidation", "prepared_metadata_cache_stale", "prepared_metadata_cache_publish"}) {
    EXPECT_NE(std::string::npos, contents.find(event));
  }
  EXPECT_EQ(std::string::npos, contents.find("private_sql_canary_781"));
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

TEST_F(BackendContractTest, PreparedMetadataCacheIsSessionBoundAcrossReconnectAndFailedOpen) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(8u, size); EXPECT_EQ(1, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(1, seen->descriptions); // Same-session reuse remains local and I/O-free.
  SQLHSTMT other{}; ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &other));
  ASSERT_EQ(SQL_ERROR, SQLExecDirect(other, (SQLCHAR*)"timeout", SQL_NTS)); // Internal close retains children.
  seen->connect_exception = 2;
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr, (SQLCHAR*)"SERVER=fake;SSL=0", SQL_NTS,
      nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  type = 77; size = 80;
  EXPECT_EQ(SQL_ERROR, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(77, type); EXPECT_EQ(80u, size); EXPECT_EQ(1, seen->descriptions);
  seen->connect_exception = 0; seen->description_size = 32;
  ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr, (SQLCHAR*)"SERVER=fake;SSL=0", SQL_NTS,
      nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(32u, size); EXPECT_EQ(2, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(2, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other));
}

TEST_F(BackendContractTest, TerminalRetirementRejectsCachedPreparedMetadataButKeepsExecutedMetadata) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  SQLHSTMT other{}; ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &other));
  seen->terminal_execution = 1;
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other, (SQLCHAR*)"rows", SQL_NTS));
  type = 77; size = 80;
  EXPECT_EQ(SQL_ERROR, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ("08S01", state()); EXPECT_EQ(77, type); EXPECT_EQ(80u, size);
  EXPECT_EQ(1, seen->descriptions);
  SQLSMALLINT columns{};
  ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(other, &columns)); EXPECT_EQ(3, columns);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(other));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other));
}

TEST_F(BackendContractTest, PreparedMetadataInvalidatesOnSuccessRecoverableFailureAndOtherDescription) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLHSTMT other{}; ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &other));
  SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
  auto describe = [&](SQLHSTMT handle) {
    return SQLDescribeParam(handle, 1, &type, &size, &digits, &nullable);
  };
  ASSERT_EQ(SQL_SUCCESS, describe(stmt)); EXPECT_EQ(1, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, describe(stmt)); EXPECT_EQ(1, seen->descriptions);
  seen->description_size = 16;
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other, (SQLCHAR*)"rows", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, describe(stmt)); EXPECT_EQ(16u, size); EXPECT_EQ(2, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(other));
  seen->description_size = 32;
  ASSERT_EQ(SQL_ERROR, SQLExecDirect(other, (SQLCHAR*)"metadata_failure", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, describe(stmt)); EXPECT_EQ(32u, size); EXPECT_EQ(3, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(other, (SQLCHAR*)"rows ?", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, describe(other)); EXPECT_EQ(4, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, describe(other)); EXPECT_EQ(4, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, describe(stmt)); EXPECT_EQ(5, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other));
}

TEST_F(BackendContractTest, MetadataEpochPreservesPassiveObservationAndInvalidatesPrivateControlAttempts) {
  seen->advertised_transactions = true; seen->isolation_mode = 1;
  connect();
  auto view = rs::odbc::detail::ODBCBackendTestAccess::view(
      rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
  auto describe = [&] { return SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable); };
  ASSERT_EQ(SQL_SUCCESS, describe()); EXPECT_EQ(1, seen->descriptions);
  EXPECT_TRUE(view.is_connected());
  SQLUINTEGER dead{}; ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(dbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, describe()); EXPECT_EQ(1, seen->descriptions);
  EXPECT_TRUE(view.transaction(TransactionAction::Commit, Deadline::max()));
  ASSERT_EQ(SQL_SUCCESS, describe()); EXPECT_EQ(2, seen->descriptions);
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_TXN_ISOLATION, (SQLPOINTER)SQL_TXN_SERIALIZABLE, 0));
  ASSERT_EQ(SQL_SUCCESS, describe()); EXPECT_EQ(3, seen->descriptions);
  EXPECT_FALSE(view.check_health(Deadline::max())); // Missing facet preserves active lease.
  ASSERT_EQ(SQL_SUCCESS, describe()); EXPECT_EQ(4, seen->descriptions);
  EXPECT_FALSE(view.reset_session(Deadline::max())); // Missing reset retires.
  size = 80; EXPECT_EQ(SQL_ERROR, describe()); EXPECT_EQ(80u, size); EXPECT_EQ("08S01", state());
}

TEST_F(BackendContractTest, TerminalSuccessfulDescriptionReturnsOwnedMetadataWithoutCachePublication) {
  connect(); seen->terminal_description = true;
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ(8u, size); EXPECT_EQ(1, seen->descriptions); EXPECT_EQ(1, seen->destructions);
  size = 80;
  EXPECT_EQ(SQL_ERROR, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ("08S01", state()); EXPECT_EQ(80u, size); EXPECT_EQ(1, seen->descriptions);
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
  for (const auto& [raw, expected] : {
      std::pair{"1.0", "01.00.0000"}, std::pair{"17.11 (vendor)", "17.11.0000"},
      std::pair{"", "00.00.0000"}, std::pair{"invalid", "00.00.0000"}}) {
    SCOPED_TRACE(raw);
    seen->server_version = raw;
    connect();
    seen->server_version = "changed after adoption"; // The negotiated observation is owned.
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
    ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
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
  EXPECT_EQ(8u, connection->type_catalog().front().column_size); // Disconnected provider defaults.
  seen->server_version.clear();
  ASSERT_FALSE(connection->type_catalog().empty());
  EXPECT_EQ(8u, connection->type_catalog().front().column_size);
}

TEST_F(BackendContractTest, ProviderCapabilitiesRemainStableAcrossSessionAndFacetLifecycle) {
  seen->absent_description = true;
  auto* connection = static_cast<rs::odbc::ODBCConnection*>(dbc);
  const auto before = connection->capabilities();
  EXPECT_TRUE(before.describe_parameters);
  EXPECT_EQ("ContractDB", before.dbms_name);
  connect();
  const auto live = connection->capabilities();
  EXPECT_FALSE(live.describe_parameters);
  EXPECT_EQ(before.dbms_name, live.dbms_name);
  EXPECT_EQ(before.max_identifier_length, live.max_identifier_length);
  EXPECT_TRUE(before.describe_parameters); // Adapter masks a copy, not provider policy.
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  const auto closed = connection->capabilities();
  EXPECT_TRUE(closed.describe_parameters);
  EXPECT_EQ(before.dbms_name, closed.dbms_name);
  EXPECT_EQ(before.max_identifier_length, closed.max_identifier_length);
  EXPECT_EQ(0, seen->queries); EXPECT_EQ(0, seen->descriptions);
}

TEST_F(BackendContractTest, ProviderErrorPolicyWorksWithoutSessionAndOwnsReturnedState) {
  std::optional<std::string> retained;
  {
    FakeProvider provider{seen};
    EXPECT_FALSE(provider.IBackendProvider::normalize_error_sqlstate("FAKE_ERROR", ErrorContext::Unknown));
    std::string native = "FAKE_ERROR";
    retained = provider.normalize_error_sqlstate(native, ErrorContext::Unknown);
    native.assign("overwritten");
    ASSERT_EQ(std::optional<std::string>("22018"), retained);
    EXPECT_FALSE(provider.normalize_error_sqlstate(native, ErrorContext::Unknown));
    EXPECT_EQ(0, seen->created); EXPECT_EQ(0, seen->transports); EXPECT_EQ(0, seen->queries);
  }
  EXPECT_EQ(std::optional<std::string>("22018"), retained);
}

TEST_F(BackendContractTest, TerminalLeaseSuccessKeepsOwningRowsAndDisconnectsExactlyOnce) {
  for (int mode : {1, 4}) {
    connect(); seen->terminal_execution = mode;
    const auto retired_before = seen->destructions;
    const auto disconnected_before = seen->disconnects;
    ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    EXPECT_EQ(retired_before + 1, seen->destructions);
    EXPECT_EQ(disconnected_before + 1, seen->disconnects);
    SQLUINTEGER dead = SQL_CD_FALSE;
    ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(dbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
    EXPECT_EQ(SQL_CD_TRUE, dead);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    char output[16]{}; SQLLEN length{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, output, sizeof(output), &length));
    EXPECT_STREQ("00ff5c", output); // Stored rows survive physical destruction.
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
    EXPECT_EQ(disconnected_before + 1, seen->disconnects);
    seen->terminal_execution = 0;
  }
}
TEST_F(BackendContractTest, TerminalErrorAndBackendExceptionRetireAndAllowExplicitReconnect) {
  for (int mode : {2, 3}) {
    connect(); seen->terminal_execution = mode; seen->failure_message = "owned terminal error";
    const auto retired_before = seen->destructions;
    const auto disconnected_before = seen->disconnects;
    ASSERT_EQ(SQL_ERROR, execute("rows"));
    EXPECT_EQ(mode == 2 ? "22018" : "HY000", state());
    EXPECT_EQ(retired_before + 1, seen->destructions);
    EXPECT_EQ(disconnected_before + 1, seen->disconnects);
    SQLUINTEGER dead = SQL_CD_FALSE;
    ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(dbc, SQL_ATTR_CONNECTION_DEAD, &dead, 0, nullptr));
    EXPECT_EQ(SQL_CD_TRUE, dead);
    seen->failure_message.clear();
    SQLCHAR message[128]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, nullptr, nullptr, message, sizeof(message), nullptr));
    EXPECT_NE(std::string::npos, std::string(reinterpret_cast<char*>(message)).find(mode == 2 ? "owned terminal error" : "fixture execution exception"));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
    EXPECT_EQ(disconnected_before + 1, seen->disconnects); seen->terminal_execution = 0;
  }
  connect(); ASSERT_EQ(SQL_SUCCESS, execute("rows"));
}
TEST_F(BackendContractTest, NegotiatedCapabilitiesSurviveTerminalRetirementUntilDisconnect) {
  connect();
  auto* connection = static_cast<rs::odbc::ODBCConnection*>(dbc);
  const auto before = connection->capabilities();
  const auto transactions = connection->transaction_capabilities();
  SQLUSMALLINT supported_before{}, supported_after{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(dbc, SQL_API_SQLDESCRIBEPARAM, &supported_before));
  seen->terminal_execution = 1;
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  EXPECT_EQ(before.describe_parameters, connection->capabilities().describe_parameters);
  EXPECT_EQ(transactions.supported, connection->transaction_capabilities().supported);
  EXPECT_TRUE(connection->has_statement_description_facet());
  ASSERT_EQ(SQL_SUCCESS, SQLGetFunctions(dbc, SQL_API_SQLDESCRIBEPARAM, &supported_after));
  EXPECT_EQ(supported_before, supported_after);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLSMALLINT type{}, digits{}, nullable{}; SQLULEN size{};
  EXPECT_EQ(SQL_ERROR, SQLDescribeParam(stmt, 1, &type, &size, &digits, &nullable));
  EXPECT_EQ("08S01", state());
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  EXPECT_FALSE(connection->has_statement_description_facet());
}

TEST_F(BackendContractTest, FailedAuthenticationOrObservationCleansAttemptAndPermitsRetry) {
  for (int mode : {1, 2, 3, 4, 5, 6, 7}) {
    seen->connect_exception = mode == 3 ? 0 : mode;
    seen->observation_exception = mode == 3;
    const auto retired_before = seen->destructions;
    const auto disconnected_before = seen->disconnects;
    EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr, (SQLCHAR*)"SERVER=fake;SSL=0", SQL_NTS,
        nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ(mode == 1 ? "HY001" : "HY000", state(SQL_HANDLE_DBC, dbc));
    EXPECT_EQ(retired_before + 1, seen->destructions);
    EXPECT_EQ(disconnected_before + 1, seen->disconnects);
    seen->connect_exception = 0; seen->observation_exception = false;
    connect(); ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  }
}
TEST_F(BackendContractTest, DeferredIsolationMustPreserveIdleAdmissionAndPermitRetry) {
  seen->advertised_transactions = true;
  ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(dbc, SQL_ATTR_TXN_ISOLATION,
      (SQLPOINTER)SQL_TXN_SERIALIZABLE, 0));
  for (int mode : {2, 3, 4}) {
    seen->isolation_mode = mode;
    const auto before = seen->disconnects;
    EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr, (SQLCHAR*)"SERVER=fake;SSL=0", SQL_NTS,
        nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    EXPECT_EQ("HY000", state(SQL_HANDLE_DBC, dbc));
    EXPECT_EQ(before + 1, seen->disconnects); EXPECT_EQ(before + 1, seen->destructions);
    seen->connect_exception = 0; seen->isolation_mode = 1;
    connect(); ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  }
  seen->isolation_mode = 0;
}

TEST_F(BackendContractTest, FreeingConnectionRetiresActiveLeaseExactlyOnce) {
  connect(); ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
  EXPECT_EQ(0, seen->disconnects); EXPECT_EQ(0, seen->destructions);
  EXPECT_EQ(SQL_ERROR, SQLFreeHandle(SQL_HANDLE_DBC, dbc)); // A logical open cannot be freed.
  EXPECT_EQ(0, seen->disconnects); EXPECT_EQ(0, seen->destructions);
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc)); dbc = nullptr;
  EXPECT_EQ(1, seen->disconnects); EXPECT_EQ(1, seen->destructions);
}

TEST_F(BackendContractTest, ExplicitRedshiftProfileReportsBoundedNumericAndNoIndexes) {
  using postgres::PgCatalogProfile;
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, dbc));
  dbc = nullptr;
  auto profile = std::make_shared<postgres::PgBackendProvider>(
      BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"localhost", 5439, std::nullopt, true},
      std::nullopt, PgCatalogProfile::Redshift);
  auto connection = std::make_unique<rs::odbc::ODBCConnection>(
      nullptr, std::make_shared<FakeProvider>(seen, profile));
  dbc = reinterpret_cast<SQLHDBC>(connection.get());
  rs::odbc::HandleRegistry::instance().register_handle(dbc, std::move(connection), env);
  connect();
  SQLUSMALLINT identifier_length{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_MAX_IDENTIFIER_LEN,
      &identifier_length, sizeof(identifier_length), nullptr));
  EXPECT_EQ(127, identifier_length);
  SQLUINTEGER indexes{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_DDL_INDEX,
      &indexes, sizeof(indexes), nullptr));
  EXPECT_EQ(0u, indexes);
  SQLUINTEGER schema_usage{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(dbc, SQL_SCHEMA_USAGE,
      &schema_usage, sizeof(schema_usage), nullptr));
  EXPECT_EQ(0u, schema_usage & SQL_SU_INDEX_DEFINITION);
  const SQLSMALLINT targets[]{SQL_NUMERIC, SQL_DECIMAL};
  for (const auto target : targets) {
    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(stmt, target));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    const SQLUSMALLINT fields[]{3, 14, 15};
    const SQLINTEGER expected[]{38, 0, 37};
    for (size_t i = 0; i < 3; ++i) {
      SQLINTEGER value{}; SQLLEN length{};
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, fields[i], SQL_C_LONG,
          &value, sizeof(value), &length));
      EXPECT_EQ(expected[i], value);
      EXPECT_NE(SQL_NULL_DATA, length);
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
}

TEST_F(BackendContractTest, LongBinaryResultPreservesUnknownMetadataAndChunkNullEmptySemantics) {
  seen->long_binary = true; connect();
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  SQLSMALLINT type = 0; SQLULEN size = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, 1, nullptr, 0, nullptr, &type, &size, nullptr, nullptr));
  EXPECT_EQ(SQL_LONGVARBINARY, type); EXPECT_EQ(0u, size);
  SQLLEN metadata = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(stmt, 1, SQL_DESC_OCTET_LENGTH,
      nullptr, 0, nullptr, &metadata));
  EXPECT_EQ(SQL_NO_TOTAL, metadata);
  ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(stmt, 1, SQL_DESC_DISPLAY_SIZE,
      nullptr, 0, nullptr, &metadata));
  EXPECT_EQ(SQL_NO_TOTAL, metadata);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  unsigned char bytes[3]{9,9,9}; SQLLEN length = -9;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_BINARY, bytes, 1, &length));
  EXPECT_EQ("01004", state()); EXPECT_EQ(3, length); EXPECT_EQ(0, bytes[0]); EXPECT_EQ(9, bytes[1]);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY, bytes, 2, &length));
  EXPECT_EQ(2, length); EXPECT_EQ(255, bytes[0]); EXPECT_EQ('\\', bytes[1]);
  EXPECT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, SQL_C_BINARY, bytes, 2, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  bytes[0] = 9;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY, bytes, 2, &length));
  EXPECT_EQ(0, length);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  bytes[0] = 9;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY, bytes, 2, &length));
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(9, bytes[0]);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  char text[16]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, text, sizeof(text), &length));
  EXPECT_STREQ("00ff5c", text); EXPECT_EQ(6, length);
}

TEST_F(BackendContractTest, LongBinaryMalformedCellPreservesOutputAndIndicator) {
  seen->long_binary = true; seen->malformed_value = true; connect();
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  unsigned char bytes[]{7, 8, 9}; SQLLEN length = 73;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_BINARY, bytes, sizeof(bytes), &length));
  EXPECT_EQ("22018", state()); EXPECT_EQ(73, length);
  EXPECT_EQ(7, bytes[0]); EXPECT_EQ(8, bytes[1]); EXPECT_EQ(9, bytes[2]);
}

TEST_F(BackendContractTest, CatalogModeEffectiveOptionReachesResolverAndInvalidNeverCreatesSession) {
  seen->validate_redshift_mode = true;
  std::string invalid = "SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0;RedshiftCatalogMode=AUTO";
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(dbc, nullptr,
      reinterpret_cast<SQLCHAR*>(invalid.data()), SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  EXPECT_EQ(0, seen->created); EXPECT_EQ(0, seen->transports);
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0;redshiftcatalogmode={LeGaCy}");
  EXPECT_EQ(RedshiftCatalogMode::Legacy, seen->settings.redshift_catalog_mode);
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  connect_with("SERVER=fake;PORT=9999;DATABASE=contract;UID=test;SSL=0");
  EXPECT_EQ(RedshiftCatalogMode::Show, seen->settings.redshift_catalog_mode);
}

namespace {
QueryResult owning_date_contract_rows() {
  QueryResult result;
  result.columns={{"date",NativeTypeInfo{ScalarType::Date,10,0,true}},
      {"neighbor",NativeTypeInfo{ScalarType::BigInt,19,0,true}}};
  result.rows={{"1000-01-01","42"},{"9999-12-31","42"},{"2000-02-29","42"},{std::nullopt,"42"}};
  result.statement_kind=StatementKind::SelectCursor;
  return result;
}
}
TEST_F(BackendContractTest, OwningDateMetadataDescriptorsAndAllRetrievalFormsAgree) {
  seen->date_result=owning_date_contract_rows();connect();
  for (SQLSMALLINT target:{SQLSMALLINT(SQL_C_TYPE_DATE),SQLSMALLINT(SQL_C_DEFAULT),SQLSMALLINT(SQL_C_CHAR),SQLSMALLINT(SQL_C_WCHAR)}) {
    SCOPED_TRACE(target);seen->date_result=owning_date_contract_rows();
    ASSERT_EQ(SQL_SUCCESS,execute("rows"));
    SQLCHAR name[16]{};SQLSMALLINT name_length{},type{},digits{},nullable{};SQLULEN size{};
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(stmt,1,name,sizeof(name),&name_length,&type,&size,&digits,&nullable));
    EXPECT_STREQ("date",reinterpret_cast<char*>(name));EXPECT_EQ(4,name_length);
    EXPECT_EQ(SQL_TYPE_DATE,type);EXPECT_EQ(10u,size);EXPECT_EQ(0,digits);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
    SQLHDESC ird{};ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(stmt,SQL_ATTR_IMP_ROW_DESC,&ird,sizeof(ird),nullptr));
    for (const auto& [field,expected]:{std::pair{SQL_DESC_CONCISE_TYPE,SQL_TYPE_DATE},std::pair{SQL_DESC_TYPE,SQL_DATETIME},
        std::pair{SQL_DESC_DATETIME_INTERVAL_CODE,SQL_CODE_DATE},std::pair{SQL_DESC_PRECISION,0},std::pair{SQL_DESC_SCALE,0}}) {
      SQLSMALLINT value=73;ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,1,static_cast<SQLSMALLINT>(field),&value,0,nullptr));EXPECT_EQ(expected,value);
    }
    SQLULEN descriptor_length{};ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,1,SQL_DESC_LENGTH,&descriptor_length,0,nullptr));EXPECT_EQ(10u,descriptor_length);
    SQLLEN octets{};ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,1,SQL_DESC_OCTET_LENGTH,&octets,0,nullptr));EXPECT_EQ(6,octets);
    SQLLEN concise{};ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(stmt,1,SQL_DESC_CONCISE_TYPE,nullptr,0,nullptr,&concise));EXPECT_EQ(SQL_TYPE_DATE,concise);
    // The adapter owns the returned copy; the source observation is now changed.
    seen->date_result->rows[0][0]="changed after execution";
    const char* expected[]{"1000-01-01","9999-12-31","2000-02-29"};
    const SQL_DATE_STRUCT dates[]{{1000,1,1},{9999,12,31},{2000,2,29}};
    for (unsigned row=0;row<3;++row) {
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));SQLLEN length=73;
      if (target==SQL_C_TYPE_DATE || target==SQL_C_DEFAULT) {
        SQL_DATE_STRUCT output{73,74,75};
        ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,target,&output,sizeof(output),&length));
        EXPECT_EQ(dates[row].year,output.year);EXPECT_EQ(dates[row].month,output.month);EXPECT_EQ(dates[row].day,output.day);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)),length);
      } else if (target==SQL_C_CHAR) {
        char output[16]="untouched";ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,target,output,sizeof(output),&length));
        EXPECT_STREQ(expected[row],output);EXPECT_EQ(10,length);
      } else {
        SQLWCHAR output[16]{};ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,target,output,sizeof(output),&length));
        const auto wide=rs::odbc::utf8_to_wide(expected[row]);ASSERT_TRUE(wide);
        EXPECT_EQ(0,std::memcmp(output,wide->data(),wide->size()*sizeof(SQLWCHAR)));EXPECT_EQ(0,output[10]);
        EXPECT_EQ(static_cast<SQLLEN>(10*sizeof(SQLWCHAR)),length);
      }
    }
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
}
TEST_F(BackendContractTest, DateNullPreservesTargetsAndRequiresIndicatorWithoutConsumingValue) {
  seen->date_result=owning_date_contract_rows();seen->date_result->rows={{std::nullopt,"42"}};connect();
  for (SQLSMALLINT target:{SQLSMALLINT(SQL_C_TYPE_DATE),SQLSMALLINT(SQL_C_CHAR),SQLSMALLINT(SQL_C_WCHAR)}) {
    SCOPED_TRACE(target);ASSERT_EQ(SQL_SUCCESS,execute("rows"));ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));
    alignas(SQL_DATE_STRUCT) unsigned char output[32];std::fill(std::begin(output),std::end(output),0x5a);SQLLEN length=73;
    EXPECT_EQ(SQL_ERROR,SQLGetData(stmt,1,target,output,sizeof(output),nullptr));EXPECT_EQ("22002",state());
    EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto value){return value==0x5a;}));
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,target,output,sizeof(output),&length));EXPECT_EQ(SQL_NULL_DATA,length);
    EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto value){return value==0x5a;}));
    SQLBIGINT neighbor{};ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,2,SQL_C_SBIGINT,&neighbor,sizeof(neighbor),&length));EXPECT_EQ(42,neighbor);
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
}
TEST_F(BackendContractTest, MarkedDateGetDataErrorsDifferFromUnmarkedCalendarErrorsAndRecover) {
  connect();
  for (bool marked:{false,true}) {
    for (SQLSMALLINT target:{SQLSMALLINT(SQL_C_TYPE_DATE),SQLSMALLINT(SQL_C_CHAR),SQLSMALLINT(SQL_C_WCHAR)}) {
      if (!marked && target!=SQL_C_TYPE_DATE) continue; // unmarked text is not a native cell error
      SCOPED_TRACE(marked);
      SCOPED_TRACE(target);
      seen->date_result=owning_date_contract_rows();seen->date_result->rows={{marked?"":"2023-02-29","42"},{"2000-02-29","42"}};
      if (marked) seen->date_result->cell_errors={{0,0}};
      ASSERT_EQ(SQL_SUCCESS,execute("rows"));ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));
      alignas(SQL_DATE_STRUCT) alignas(SQLWCHAR) unsigned char output[std::max(11*sizeof(SQLWCHAR),sizeof(SQL_DATE_STRUCT))];std::fill(std::begin(output),std::end(output),0x5a);SQLLEN length=73;
      for (unsigned attempt=0;attempt<2;++attempt) {
        EXPECT_EQ(SQL_ERROR,SQLGetData(stmt,1,target,output,sizeof(output),&length));EXPECT_EQ(marked?"22018":"22007",state());
        EXPECT_EQ(73,length);EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto value){return value==0x5a;}));
      }
      SQLBIGINT neighbor{};ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,2,SQL_C_SBIGINT,&neighbor,sizeof(neighbor),&length));EXPECT_EQ(42,neighbor);
      ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,target,output,sizeof(output),&length));
      if (target==SQL_C_TYPE_DATE) {
        SQL_DATE_STRUCT date{};std::memcpy(&date,output,sizeof(date));
        EXPECT_EQ(2000,date.year);EXPECT_EQ(2,date.month);EXPECT_EQ(29,date.day);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)),length);
      } else if (target==SQL_C_CHAR) {
        EXPECT_STREQ("2000-02-29",reinterpret_cast<char*>(output));EXPECT_EQ(10,length);
      } else {
        const auto wide=rs::odbc::utf8_to_wide("2000-02-29");ASSERT_TRUE(wide);
        EXPECT_EQ(0,std::memcmp(output,wide->data(),wide->size()*sizeof(SQLWCHAR)));
        SQLWCHAR terminator=1;std::memcpy(&terminator,output+10*sizeof(SQLWCHAR),sizeof(terminator));EXPECT_EQ(0,terminator);
        EXPECT_EQ(static_cast<SQLLEN>(10*sizeof(SQLWCHAR)),length);
      }
      ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));EXPECT_EQ(0,seen->disconnects);
    }
  }
}
TEST_F(BackendContractTest, BoundDateErrorsPreserveFailingStructAndRecoverIncludingNull) {
  connect();SQLUSMALLINT row_status=SQL_ROW_SUCCESS;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_ROW_STATUS_PTR,&row_status,0));
  for (bool marked:{false,true}) {
    SCOPED_TRACE(marked);seen->date_result=owning_date_contract_rows();
    seen->date_result->rows={{marked?"":"2023-02-29","42"},{"2000-02-29","42"},{std::nullopt,"42"}};
    if (marked) seen->date_result->cell_errors={{0,0}};
    SQL_DATE_STRUCT output{73,74,75};SQLLEN length=76;
    ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt,1,SQL_C_TYPE_DATE,&output,sizeof(output),&length));ASSERT_EQ(SQL_SUCCESS,execute("rows"));
    EXPECT_EQ(SQL_ERROR,SQLFetch(stmt));EXPECT_EQ(marked?"22018":"22007",state());EXPECT_EQ(SQL_ROW_ERROR,row_status);
    EXPECT_EQ(73,output.year);EXPECT_EQ(74,output.month);EXPECT_EQ(75,output.day);EXPECT_EQ(76,length);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));EXPECT_EQ(SQL_ROW_SUCCESS,row_status);
    EXPECT_EQ(2000,output.year);EXPECT_EQ(2,output.month);EXPECT_EQ(29,output.day);EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)),length);
    output={73,74,75};length=76;ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));EXPECT_EQ(SQL_NULL_DATA,length);EXPECT_EQ(SQL_ROW_SUCCESS,row_status);
    EXPECT_EQ(73,output.year);EXPECT_EQ(74,output.month);EXPECT_EQ(75,output.day);
    EXPECT_EQ(SQL_NO_DATA,SQLFetch(stmt));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_UNBIND));
    EXPECT_EQ(0,seen->disconnects);
  }
}

namespace {
QueryResult owning_datetime_contract_rows() {
  QueryResult result;
  result.columns={{"d0",NativeTypeInfo{ScalarType::Timestamp,19,0,true}},
      {"d3",NativeTypeInfo{ScalarType::Timestamp,23,3,true}},
      {"d6",NativeTypeInfo{ScalarType::Timestamp,26,6,true}},
      {"neighbor",NativeTypeInfo{ScalarType::BigInt,19,0,true}}};
  result.rows={{"1000-01-01 00:00:00","1000-01-01 00:00:00.000","1000-01-01 00:00:00.000000","42"},
      {"9999-12-31 23:59:59","9999-12-31 23:59:59.000","9999-12-31 23:59:59.000000","42"},
      {"2000-02-29 12:34:56","2000-02-29 12:34:56.123","2000-02-29 12:34:56.123456","42"},
      {"2024-02-29 01:02:03","2024-02-29 01:02:03.001","2024-02-29 01:02:03.000001","42"},
      {std::nullopt,std::nullopt,std::nullopt,"42"}};
  result.statement_kind=StatementKind::SelectCursor;
  return result;
}
void expect_datetime_output(SQLSMALLINT target,const unsigned char* output,SQLLEN length,
    const std::string& text,const SQL_TIMESTAMP_STRUCT& expected) {
  if (target==SQL_C_TYPE_TIMESTAMP || target==SQL_C_TIMESTAMP || target==SQL_C_DEFAULT) {
    SQL_TIMESTAMP_STRUCT value{};std::memcpy(&value,output,sizeof(value));
    EXPECT_EQ(expected.year,value.year);EXPECT_EQ(expected.month,value.month);EXPECT_EQ(expected.day,value.day);
    EXPECT_EQ(expected.hour,value.hour);EXPECT_EQ(expected.minute,value.minute);EXPECT_EQ(expected.second,value.second);
    EXPECT_EQ(expected.fraction,value.fraction);EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)),length);
  } else if (target==SQL_C_CHAR) {
    EXPECT_STREQ(text.c_str(),reinterpret_cast<const char*>(output));EXPECT_EQ(static_cast<SQLLEN>(text.size()),length);
  } else {
    const auto wide=rs::odbc::utf8_to_wide(text);ASSERT_TRUE(wide);
    EXPECT_EQ(0,std::memcmp(output,wide->data(),wide->size()*sizeof(SQLWCHAR)));
    SQLWCHAR terminator=1;std::memcpy(&terminator,output+wide->size()*sizeof(SQLWCHAR),sizeof(terminator));EXPECT_EQ(0,terminator);
    EXPECT_EQ(static_cast<SQLLEN>(wide->size()*sizeof(SQLWCHAR)),length);
  }
}
}
TEST_F(BackendContractTest, OwningDatetimeMetadataDescriptorsAndRetrievalFormsAgree) {
  connect();
  for (SQLSMALLINT target:{SQLSMALLINT(SQL_C_TYPE_TIMESTAMP),SQLSMALLINT(SQL_C_TIMESTAMP),SQLSMALLINT(SQL_C_DEFAULT),SQLSMALLINT(SQL_C_CHAR),SQLSMALLINT(SQL_C_WCHAR)}) {
    SCOPED_TRACE(target);const auto expected=owning_datetime_contract_rows();seen->date_result=expected;
    ASSERT_EQ(SQL_SUCCESS,execute("rows"));
    SQLHDESC ird{};ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(stmt,SQL_ATTR_IMP_ROW_DESC,&ird,sizeof(ird),nullptr));
    const SQLSMALLINT precisions[]{0,3,6};const SQLULEN widths[]{19,23,26};
    for (SQLUSMALLINT col=1;col<=3;++col) {
      SCOPED_TRACE(col);SQLCHAR name[16]{};SQLSMALLINT name_length{},type{},digits{},nullable{};SQLULEN size{};
      ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(stmt,col,name,sizeof(name),&name_length,&type,&size,&digits,&nullable));
      EXPECT_STREQ(expected.columns[col-1].name.c_str(),reinterpret_cast<char*>(name));EXPECT_EQ(2,name_length);
      EXPECT_EQ(SQL_TYPE_TIMESTAMP,type);EXPECT_EQ(widths[col-1],size);EXPECT_EQ(precisions[col-1],digits);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
      for (const auto& [field,value]:{std::pair{SQL_DESC_CONCISE_TYPE,SQL_TYPE_TIMESTAMP},std::pair{SQL_DESC_TYPE,SQL_DATETIME},
          std::pair{SQL_DESC_DATETIME_INTERVAL_CODE,SQL_CODE_TIMESTAMP},std::pair{SQL_DESC_PRECISION,int(precisions[col-1])},std::pair{SQL_DESC_SCALE,int(precisions[col-1])}}) {
        SQLSMALLINT actual=73;ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,col,static_cast<SQLSMALLINT>(field),&actual,0,nullptr));EXPECT_EQ(value,actual);
      }
      SQLULEN descriptor_length{};ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,col,SQL_DESC_LENGTH,&descriptor_length,0,nullptr));EXPECT_EQ(widths[col-1],descriptor_length);
      SQLLEN octets{};ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,col,SQL_DESC_OCTET_LENGTH,&octets,0,nullptr));EXPECT_EQ(16,octets);
      for (SQLUSMALLINT field:{SQLUSMALLINT(SQL_DESC_CONCISE_TYPE),SQLUSMALLINT(SQL_COLUMN_TYPE),SQLUSMALLINT(SQL_DESC_DISPLAY_SIZE)}) {
        SQLLEN actual=73;ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(stmt,col,field,nullptr,0,nullptr,&actual));
        EXPECT_EQ(field==SQL_DESC_DISPLAY_SIZE?static_cast<SQLLEN>(widths[col-1]):SQL_TYPE_TIMESTAMP,actual);
      }
    }
    // The fake source changes after execution; the statement retains its own snapshot.
    seen->date_result->rows[0][0]="changed after execution";
    const SQL_TIMESTAMP_STRUCT calendars[]{{1000,1,1,0,0,0,0},{9999,12,31,23,59,59,0},{2000,2,29,12,34,56,0},{2024,2,29,1,2,3,0}};
    const SQLUINTEGER fractions[][3]{{0,0,0},{0,0,0},{0,123000000,123456000},{0,1000000,1000}};
    for (unsigned row=0;row<4;++row) {
      SCOPED_TRACE(row);ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));
      for (SQLUSMALLINT col=1;col<=3;++col) {
        alignas(SQL_TIMESTAMP_STRUCT) alignas(SQLWCHAR) unsigned char output[std::max(sizeof(SQL_TIMESTAMP_STRUCT),27*sizeof(SQLWCHAR))]{};SQLLEN length=73;
        ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,col,target,output,sizeof(output),&length));
        auto calendar=calendars[row];calendar.fraction=fractions[row][col-1];
        expect_datetime_output(target,output,length,*expected.rows[row][col-1],calendar);
      }
    }
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));EXPECT_EQ(0,seen->disconnects);
  }
}
TEST_F(BackendContractTest, DatetimeNullPreservesTargetsAndRequiresIndicatorWithoutConsumingValue) {
  seen->date_result=owning_datetime_contract_rows();seen->date_result->rows={{std::nullopt,std::nullopt,std::nullopt,"42"}};connect();
  for (SQLSMALLINT target:{SQLSMALLINT(SQL_C_TYPE_TIMESTAMP),SQLSMALLINT(SQL_C_TIMESTAMP),SQLSMALLINT(SQL_C_DEFAULT),SQLSMALLINT(SQL_C_CHAR),SQLSMALLINT(SQL_C_WCHAR)}) {
    SCOPED_TRACE(target);ASSERT_EQ(SQL_SUCCESS,execute("rows"));ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));
    for (SQLUSMALLINT col=1;col<=3;++col) {
      alignas(SQL_TIMESTAMP_STRUCT) alignas(SQLWCHAR) unsigned char output[std::max(sizeof(SQL_TIMESTAMP_STRUCT),27*sizeof(SQLWCHAR))];
      std::fill(std::begin(output),std::end(output),0x5a);SQLLEN length=73;
      EXPECT_EQ(SQL_ERROR,SQLGetData(stmt,col,target,output,sizeof(output),nullptr));EXPECT_EQ("22002",state());
      EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto byte){return byte==0x5a;}));
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,col,target,output,sizeof(output),&length));EXPECT_EQ(SQL_NULL_DATA,length);
      EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto byte){return byte==0x5a;}));
    }
    SQLBIGINT neighbor{};SQLLEN length{};ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,4,SQL_C_SBIGINT,&neighbor,sizeof(neighbor),&length));EXPECT_EQ(42,neighbor);
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));EXPECT_EQ(0,seen->disconnects);
  }
}
TEST_F(BackendContractTest, MarkedDatetimeGetDataErrorsDifferFromUnmarkedCalendarErrorsAndRecover) {
  connect();
  for (bool marked:{false,true}) for (SQLSMALLINT target:{SQLSMALLINT(SQL_C_TYPE_TIMESTAMP),SQLSMALLINT(SQL_C_TIMESTAMP),SQLSMALLINT(SQL_C_DEFAULT),SQLSMALLINT(SQL_C_CHAR),SQLSMALLINT(SQL_C_WCHAR)}) {
    if (!marked && (target==SQL_C_CHAR || target==SQL_C_WCHAR)) continue;
    SCOPED_TRACE(marked);
    SCOPED_TRACE(target);
    seen->date_result=owning_datetime_contract_rows();
    seen->date_result->rows={{marked?"":"2023-02-29 01:02:03","2024-02-29 01:02:03.001","2024-02-29 01:02:03.000001","42"},
        {"2000-02-29 12:34:56","2000-02-29 12:34:56.123","2000-02-29 12:34:56.123456","42"}};
    if (marked) seen->date_result->cell_errors={{0,0}};
    ASSERT_EQ(SQL_SUCCESS,execute("rows"));ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));
    alignas(SQL_TIMESTAMP_STRUCT) alignas(SQLWCHAR) unsigned char output[std::max(sizeof(SQL_TIMESTAMP_STRUCT),27*sizeof(SQLWCHAR))];
    std::fill(std::begin(output),std::end(output),0x5a);SQLLEN length=73;
    for (unsigned attempt=0;attempt<2;++attempt) {
      EXPECT_EQ(SQL_ERROR,SQLGetData(stmt,1,target,output,sizeof(output),&length));EXPECT_EQ(marked?"22018":"22007",state());EXPECT_EQ(73,length);
      EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto byte){return byte==0x5a;}));
    }
    SQLBIGINT neighbor{};ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,4,SQL_C_SBIGINT,&neighbor,sizeof(neighbor),&length));EXPECT_EQ(42,neighbor);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));
    for (SQLUSMALLINT col=1;col<=3;++col) {
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,col,target,output,sizeof(output),&length));
      const SQLUINTEGER fractions[]{0,123000000,123456000};
      expect_datetime_output(target,output,length,*seen->date_result->rows[1][col-1],SQL_TIMESTAMP_STRUCT{2000,2,29,12,34,56,fractions[col-1]});
    }
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));EXPECT_EQ(0,seen->disconnects);
  }
}
TEST_F(BackendContractTest, BoundDatetimeErrorsPreserveFailingStructAndRecoverIncludingNull) {
  connect();SQLUSMALLINT row_status=SQL_ROW_SUCCESS;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_ROW_STATUS_PTR,&row_status,0));
  for (bool marked:{false,true}) for (SQLSMALLINT target:{SQLSMALLINT(SQL_C_TYPE_TIMESTAMP),SQLSMALLINT(SQL_C_TIMESTAMP),SQLSMALLINT(SQL_C_DEFAULT)}) {
    SCOPED_TRACE(marked);
    SCOPED_TRACE(target);seen->date_result=owning_datetime_contract_rows();
    seen->date_result->rows={{"2024-02-29 01:02:03","2024-02-29 01:02:03.001",marked?"":"2023-02-29 01:02:03.123456","42"},
        {"2000-02-29 12:34:56","2000-02-29 12:34:56.123","2000-02-29 12:34:56.123456","42"},
        {std::nullopt,std::nullopt,std::nullopt,"42"}};
    if (marked) seen->date_result->cell_errors={{0,2}};
    SQL_TIMESTAMP_STRUCT output{73,74,75,76,77,78,79};SQLLEN length=80;
    ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt,3,target,&output,sizeof(output),&length));ASSERT_EQ(SQL_SUCCESS,execute("rows"));
    EXPECT_EQ(SQL_ERROR,SQLFetch(stmt));EXPECT_EQ(marked?"22018":"22007",state());EXPECT_EQ(SQL_ROW_ERROR,row_status);
    EXPECT_EQ(73,output.year);EXPECT_EQ(74,output.month);EXPECT_EQ(75,output.day);EXPECT_EQ(76,output.hour);EXPECT_EQ(77,output.minute);EXPECT_EQ(78,output.second);EXPECT_EQ(79u,output.fraction);EXPECT_EQ(80,length);
    SQLBIGINT neighbor{};SQLLEN neighbor_length{};ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,4,SQL_C_SBIGINT,&neighbor,sizeof(neighbor),&neighbor_length));EXPECT_EQ(42,neighbor);
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));EXPECT_EQ(SQL_ROW_SUCCESS,row_status);
    expect_datetime_output(target,reinterpret_cast<unsigned char*>(&output),length,"2000-02-29 12:34:56.123456",SQL_TIMESTAMP_STRUCT{2000,2,29,12,34,56,123456000});
    output={73,74,75,76,77,78,79};length=80;ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));EXPECT_EQ(SQL_NULL_DATA,length);EXPECT_EQ(SQL_ROW_SUCCESS,row_status);
    EXPECT_EQ(73,output.year);EXPECT_EQ(74,output.month);EXPECT_EQ(75,output.day);EXPECT_EQ(76,output.hour);EXPECT_EQ(77,output.minute);EXPECT_EQ(78,output.second);EXPECT_EQ(79u,output.fraction);
    EXPECT_EQ(SQL_NO_DATA,SQLFetch(stmt));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_UNBIND));EXPECT_EQ(0,seen->disconnects);
  }
}

// Fake adapter contract only. These tests never route ODBC to native MySQL;
// its stricter year1000 boundary and native receipt proof remain separate.
TEST_F(BackendContractTest, DateInputStructAliasesDefaultAndOwningHintAgree) {
  connect();ASSERT_EQ(SQL_SUCCESS,SQLPrepare(stmt,(SQLCHAR*)"rows ?",SQL_NTS));
  SQLULEN processed=99;SQLUSMALLINT status=SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAM_STATUS_PTR,&status,0));
  for (SQLSMALLINT c_type:{SQLSMALLINT(SQL_C_TYPE_DATE),SQLSMALLINT(SQL_C_DATE),SQLSMALLINT(SQL_C_DEFAULT)}) {
    for (const SQL_DATE_STRUCT date:{SQL_DATE_STRUCT{1000,1,1},SQL_DATE_STRUCT{9999,12,31},SQL_DATE_STRUCT{2000,2,29}}) {
      alignas(SQL_DATE_STRUCT) std::array<std::byte,sizeof(SQL_DATE_STRUCT)+1> bytes{};
      std::memcpy(bytes.data()+1,&date,sizeof(date));SQLLEN indicator=73;
      ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,c_type,SQL_TYPE_DATE,10,0,bytes.data()+1,sizeof(date),&indicator));
      ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(1u,seen->parameters.size());
      const auto expected=date.year==1000?"1000-01-01":(date.year==9999?"9999-12-31":"2000-02-29");
      EXPECT_EQ(QueryParameterType::Date,seen->parameters[0].type);EXPECT_FALSE(seen->parameters[0].binary_input);
      EXPECT_EQ(std::optional<std::string>{expected},seen->parameters[0].value);
      EXPECT_EQ(73,indicator);EXPECT_EQ(0,std::memcmp(bytes.data()+1,&date,sizeof(date)));
      EXPECT_EQ(1u,processed);EXPECT_EQ(SQL_PARAM_SUCCESS,status);
      std::fill(bytes.begin(),bytes.end(),std::byte{0});
      EXPECT_EQ(std::optional<std::string>{expected},seen->parameters[0].value);
      ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
    }
  }
  SQL_DATE_STRUCT date{2024,2,29};
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_VARCHAR,10,0,&date,sizeof(date),nullptr));
  ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(1u,seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Text,seen->parameters[0].type);EXPECT_EQ(std::optional<std::string>{"2024-02-29"},seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));const auto queries=seen->queries;
  EXPECT_EQ(SQL_ERROR,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_DATE,10,0,&date,sizeof(date),nullptr));
  EXPECT_EQ("HYC00",state());EXPECT_EQ(queries,seen->queries);EXPECT_EQ(0u,seen->disconnects);
}

TEST_F(BackendContractTest, DateInputCharactersWideAndTypedNullNormalizeWithOwningValues) {
  connect();ASSERT_EQ(SQL_SUCCESS,SQLPrepare(stmt,(SQLCHAR*)"rows ?",SQL_NTS));
  for (bool wide:{false,true}) for (bool nts:{false,true}) {
    char narrow[]=" 2024-02-29 ";SQLWCHAR text[]{' ','2','0','2','4','-','0','2','-','2','9',' ',0};
    SQLLEN indicator=nts?SQL_NTS:SQLLEN(12*(wide?sizeof(SQLWCHAR):1));const auto original=indicator;
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,wide?SQL_C_WCHAR:SQL_C_CHAR,SQL_TYPE_DATE,10,0,
        wide?static_cast<void*>(text):static_cast<void*>(narrow),wide?sizeof(text):sizeof(narrow),&indicator));
    ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(1u,seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Date,seen->parameters[0].type);EXPECT_FALSE(seen->parameters[0].binary_input);
    EXPECT_EQ(std::optional<std::string>{"2024-02-29"},seen->parameters[0].value);EXPECT_EQ(original,indicator);
    narrow[1]='x';text[1]='x';EXPECT_EQ(std::optional<std::string>{"2024-02-29"},seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
  SQLLEN indicator=SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,nullptr,0,&indicator));
  ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(1u,seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Date,seen->parameters[0].type);EXPECT_FALSE(seen->parameters[0].value);
  EXPECT_FALSE(seen->parameters[0].binary_input);EXPECT_EQ(SQL_NULL_DATA,indicator);ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  char empty[]="";indicator=0;const auto queries=seen->queries;
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_TYPE_DATE,10,0,empty,sizeof(empty),&indicator));
  EXPECT_EQ(SQL_ERROR,SQLExecute(stmt));EXPECT_EQ("22018",state());EXPECT_EQ(queries,seen->queries);EXPECT_EQ(0,indicator);
  EXPECT_FALSE(seen->parameters[0].value); // Prior owning NULL observation survives local rejection.
  SQL_DATE_STRUCT repaired{2024,2,29};indicator=sizeof(repaired);
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,&repaired,sizeof(repaired),&indicator));
  ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));EXPECT_EQ(std::optional<std::string>{"2024-02-29"},seen->parameters[0].value);
}

TEST_F(BackendContractTest, DateInputLocalCalendarErrorsPreserveStatusInputsAndRecover) {
  connect();ASSERT_EQ(SQL_SUCCESS,SQLPrepare(stmt,(SQLCHAR*)"rows ?",SQL_NTS));
  SQLULEN processed=99;SQLUSMALLINT status=SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAM_STATUS_PTR,&status,0));
  SQL_DATE_STRUCT input{2000,2,29};SQLLEN indicator=73;
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,&input,sizeof(input),&indicator));
  ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  for (const SQL_DATE_STRUCT invalid:{SQL_DATE_STRUCT{2023,2,29},SQL_DATE_STRUCT{0,1,1},SQL_DATE_STRUCT{2024,0,1}}) {
    input=invalid;const auto queries=seen->queries;const auto previous=seen->parameters[0].value;
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,&input,sizeof(input),&indicator));
    EXPECT_EQ(SQL_ERROR,SQLExecute(stmt));EXPECT_EQ("22007",state());EXPECT_EQ(queries,seen->queries);
    EXPECT_EQ(0,std::memcmp(&input,&invalid,sizeof(input)));EXPECT_EQ(73,indicator);
    EXPECT_EQ(previous,seen->parameters[0].value);EXPECT_EQ(1u,processed);EXPECT_EQ(SQL_PARAM_ERROR,status);EXPECT_EQ(0u,seen->disconnects);
    input={2024,2,29};ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));EXPECT_EQ(SQL_PARAM_SUCCESS,status);
    EXPECT_EQ(std::optional<std::string>{"2024-02-29"},seen->parameters[0].value);ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
  for (auto text:{"2023-02-29","2024-02-29T00:00:00","2024-02-29 00:00:00+02:00"}) {
    std::string value{text};SQLLEN length=static_cast<SQLLEN>(value.size());const auto queries=seen->queries;
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_TYPE_DATE,10,0,value.data(),value.size(),&length));
    EXPECT_EQ(SQL_ERROR,SQLExecute(stmt));EXPECT_EQ("22018",state());EXPECT_EQ(queries,seen->queries);
    EXPECT_EQ(text,value);EXPECT_EQ(static_cast<SQLLEN>(value.size()),length);EXPECT_EQ(SQL_PARAM_ERROR,status);
  }
  SQL_TIMESTAMP_STRUCT timestamp{2024,2,29,1,0,0,0};const auto queries=seen->queries;
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_TIMESTAMP,SQL_TYPE_DATE,10,0,&timestamp,sizeof(timestamp),nullptr));
  EXPECT_EQ(SQL_ERROR,SQLExecute(stmt));EXPECT_EQ("22008",state());EXPECT_EQ(queries,seen->queries);EXPECT_EQ(1,timestamp.hour);
  timestamp.hour=0;ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));EXPECT_EQ(QueryParameterType::Date,seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"2024-02-29"},seen->parameters[0].value);ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  // Generic ODBC calendar is wider than private MySQL parameter admission.
  // This fake forwarding assertion does not claim native year0001 support.
  input={1,1,1};
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,&input,sizeof(input),nullptr));
  ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));EXPECT_EQ(QueryParameterType::Date,seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"0001-01-01"},seen->parameters[0].value);EXPECT_EQ(SQL_PARAM_SUCCESS,status);EXPECT_EQ(0u,seen->disconnects);
}

TEST_F(BackendContractTest, DateInputExecutionReceiptOwnsIpdDespiteAbsentPreDescription) {
  seen->absent_description=true;seen->date_result=owning_date_contract_rows();
  seen->date_result->normalized_parameter_types={{ScalarType::Date,10,0,true}};connect();
  ASSERT_EQ(SQL_SUCCESS,SQLPrepare(stmt,(SQLCHAR*)"rows ?",SQL_NTS));
  SQL_DATE_STRUCT input{2000,2,29};
  ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_TYPE_DATE,SQL_TYPE_DATE,10,0,&input,sizeof(input),nullptr));
  SQLSMALLINT type=73,digits=74,nullable=75;SQLULEN size=76;
  EXPECT_EQ(SQL_ERROR,SQLDescribeParam(stmt,1,&type,&size,&digits,&nullable));EXPECT_EQ("HYC00",state());
  EXPECT_EQ(73,type);EXPECT_EQ(74,digits);EXPECT_EQ(75,nullable);EXPECT_EQ(76u,size);EXPECT_EQ(0,seen->descriptions);EXPECT_EQ(0,seen->queries);
  ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(1u,seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Date,seen->parameters[0].type);EXPECT_EQ(std::optional<std::string>{"2000-02-29"},seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS,SQLDescribeParam(stmt,1,&type,&size,&digits,&nullable));
  EXPECT_EQ(SQL_TYPE_DATE,type);EXPECT_EQ(10u,size);EXPECT_EQ(0,digits);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);EXPECT_EQ(0,seen->descriptions);
  SQLHDESC ipd=SQL_NULL_HDESC;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(stmt,SQL_ATTR_IMP_PARAM_DESC,&ipd,0,nullptr));
  for (const auto& [field,expected]:{std::pair{SQL_DESC_CONCISE_TYPE,SQL_TYPE_DATE},std::pair{SQL_DESC_TYPE,SQL_DATETIME},
      std::pair{SQL_DESC_DATETIME_INTERVAL_CODE,SQL_CODE_DATE},std::pair{SQL_DESC_SCALE,0}}) {
    SQLSMALLINT observed=77;ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ipd,1,static_cast<SQLSMALLINT>(field),&observed,0,nullptr));EXPECT_EQ(expected,observed);
  }
  SQLULEN length=78;ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ipd,1,SQL_DESC_LENGTH,&length,0,nullptr));EXPECT_EQ(10u,length);
  seen->date_result->normalized_parameter_types[0]={ScalarType::BigInt,19,0,true};input={9999,12,31};
  ASSERT_EQ(SQL_SUCCESS,SQLDescribeParam(stmt,1,&type,&size,&digits,&nullable));EXPECT_EQ(SQL_TYPE_DATE,type);EXPECT_EQ(10u,size);
  EXPECT_EQ(std::optional<std::string>{"2000-02-29"},seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));ASSERT_EQ(SQL_SUCCESS,SQLPrepare(stmt,(SQLCHAR*)"rows ?",SQL_NTS));
  type=73;digits=74;nullable=75;size=76;
  EXPECT_EQ(SQL_ERROR,SQLDescribeParam(stmt,1,&type,&size,&digits,&nullable));EXPECT_EQ("HYC00",state());
  EXPECT_EQ(73,type);EXPECT_EQ(74,digits);EXPECT_EQ(75,nullable);EXPECT_EQ(76u,size);EXPECT_EQ(0,seen->descriptions);
}

// Offline ODBC forwarding policy, not native Redshift-driver equivalence.
TEST_F(BackendContractTest, TimestampInputPrecisionBoundariesOwnValuesAndRecover) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(2), 0));
  SQLULEN processed = 99;
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  for (const SQLSMALLINT precision : {SQLSMALLINT{6}, SQLSMALLINT{3}}) {
    SCOPED_TRACE(precision);
    const SQLUINTEGER fraction = precision == 6 ? 123456000u : 123000000u;
    const std::string expected = precision == 6
        ? "2024-02-29 12:34:56.123456" : "2024-02-29 12:34:56.123000";
    SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 12, 34, 56, fraction};
    SQLLEN indicator = sizeof(input);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, precision,
        &input, sizeof(input), &indicator));
    const auto before = rs::util::Clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    const auto after = rs::util::Clock::now();
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Timestamp, seen->parameters[0].type);
    EXPECT_FALSE(seen->parameters[0].binary_input);
    EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
    EXPECT_GE(seen->deadline, before + std::chrono::seconds{2});
    EXPECT_LE(seen->deadline, after + std::chrono::seconds{2});
    EXPECT_EQ(SQL_PARAM_SUCCESS, status);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    const auto queries = seen->queries;
    const auto deadline = seen->deadline;
    input.fraction = fraction + 1; // One nanosecond beyond the declared quantum.
    const auto invalid = input;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, precision,
        &input, sizeof(input), &indicator));
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
    EXPECT_EQ("22008", state());
    EXPECT_EQ(queries, seen->queries);
    EXPECT_EQ(deadline, seen->deadline);
    EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
    EXPECT_EQ(0, std::memcmp(&input, &invalid, sizeof(input)));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
    EXPECT_EQ(1u, processed);
    EXPECT_EQ(SQL_PARAM_ERROR, status);
    EXPECT_EQ(0, seen->disconnects);
    input.fraction = fraction;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(queries + 1, seen->queries);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status);
    ASSERT_EQ(1u, seen->parameters.size());
    input = {}; // Backend observation owns the value, not the application struct.
    EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
    EXPECT_EQ(QueryParameterType::Timestamp, seen->parameters[0].type);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(BackendContractTest, TimestampInputTypedNullRetainsTimestampHintAndRecovers) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLLEN indicator = SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
      SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, 6, nullptr, 0, &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Timestamp, seen->parameters[0].type);
  EXPECT_FALSE(seen->parameters[0].value);
  EXPECT_FALSE(seen->parameters[0].binary_input);
  EXPECT_EQ(SQL_NULL_DATA, indicator);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 12, 34, 56, 123456000};
  indicator = sizeof(input);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
      SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, 6, &input, sizeof(input), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Timestamp, seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"2024-02-29 12:34:56.123456"}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, TimestampInputDateRejectsFractionOnlyLossWithoutDispatch) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQL_TIMESTAMP_STRUCT input{2024, 2, 29, 0, 0, 0, 0};
  SQLLEN indicator = sizeof(input);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
      SQL_C_TYPE_TIMESTAMP, SQL_TYPE_DATE, 10, 0, &input, sizeof(input), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  const auto queries = seen->queries;
  input.fraction = 1;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
      SQL_C_TYPE_TIMESTAMP, SQL_TYPE_DATE, 10, 0, &input, sizeof(input), &indicator));
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
  EXPECT_EQ("22008", state());
  EXPECT_EQ(queries, seen->queries);
  EXPECT_EQ(1u, input.fraction);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Date, seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"2024-02-29"}, seen->parameters[0].value);
  input.fraction = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(queries + 1, seen->queries);
  EXPECT_EQ(0, seen->disconnects);
  EXPECT_EQ(QueryParameterType::Date, seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"2024-02-29"}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

// Offline public ODBC input policy. C unsigned integers do not imply a native
// Redshift unsigned SQL family or prove native prepared protocol acceptance.
TEST_F(BackendContractTest, PreparedIntegerBoundariesOwnValuesAndRejectUnsignedBigintOverflow) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99;
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(2), 0));
  SQLLEN indicator = sizeof(SQLBIGINT);
  for (const SQLBIGINT boundary : {std::numeric_limits<SQLBIGINT>::min(),
                                   std::numeric_limits<SQLBIGINT>::max()}) {
    SQLBIGINT input = boundary;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        SQL_C_SBIGINT, SQL_BIGINT, 19, 0, &input, sizeof(input), &indicator));
    const auto before = rs::util::Clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    const auto after = rs::util::Clock::now();
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Int64, seen->parameters[0].type);
    EXPECT_FALSE(seen->parameters[0].binary_input);
    EXPECT_EQ(std::optional<std::string>{std::to_string(boundary)}, seen->parameters[0].value);
    EXPECT_GE(seen->deadline, before + std::chrono::seconds{2});
    EXPECT_LE(seen->deadline, after + std::chrono::seconds{2});
    EXPECT_EQ(1u, processed);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
    input = 0;
    EXPECT_EQ(std::optional<std::string>{std::to_string(boundary)}, seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  const auto ceiling = static_cast<SQLUBIGINT>(std::numeric_limits<SQLBIGINT>::max());
  SQLUBIGINT input = ceiling;
  indicator = sizeof(input);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
      SQL_C_UBIGINT, SQL_BIGINT, 19, 0, &input, sizeof(input), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Int64, seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"9223372036854775807"}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  for (const SQLUBIGINT invalid : {ceiling + 1, std::numeric_limits<SQLUBIGINT>::max()}) {
    input = invalid;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        SQL_C_UBIGINT, SQL_BIGINT, 19, 0, &input, sizeof(input), &indicator));
    const auto queries = seen->queries;
    const auto deadline = seen->deadline;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
    EXPECT_EQ("22003", state());
    EXPECT_EQ(queries, seen->queries);
    EXPECT_EQ(deadline, seen->deadline);
    EXPECT_EQ(invalid, input);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
    EXPECT_EQ(1u, processed);
    EXPECT_EQ(SQL_PARAM_ERROR, status);
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Int64, seen->parameters[0].type);
    EXPECT_EQ(std::optional<std::string>{"9223372036854775807"}, seen->parameters[0].value);
    input = ceiling;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(queries + 1, seen->queries);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status);
    EXPECT_EQ(1u, processed);
    EXPECT_EQ(0, seen->disconnects);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  input = std::numeric_limits<SQLUBIGINT>::max();
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
      SQL_C_UBIGINT, SQL_NUMERIC, 20, 0, &input, sizeof(input), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Numeric, seen->parameters[0].type);
  EXPECT_FALSE(seen->parameters[0].binary_input);
  EXPECT_EQ(std::optional<std::string>{"18446744073709551615"}, seen->parameters[0].value);
  EXPECT_EQ(1u, processed);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status);
  input = 0;
  EXPECT_EQ(std::optional<std::string>{"18446744073709551615"}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, PreparedIntegerTypedNullRetainsInt64HintAndRecovers) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99;
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  for (const SQLSMALLINT c_type : {SQLSMALLINT{SQL_C_SBIGINT}, SQLSMALLINT{SQL_C_UBIGINT}}) {
    SCOPED_TRACE(c_type);
    SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        c_type, SQL_BIGINT, 19, 0, nullptr, 0, &indicator));
    const auto queries = seen->queries;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(queries + 1, seen->queries);
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Int64, seen->parameters[0].type);
    EXPECT_FALSE(seen->parameters[0].value);
    EXPECT_FALSE(seen->parameters[0].binary_input);
    EXPECT_EQ(SQL_NULL_DATA, indicator);
    EXPECT_EQ(1u, processed);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    SQLBIGINT signed_input = 42;
    SQLUBIGINT unsigned_input = 42;
    indicator = sizeof(SQLBIGINT);
    void* input = c_type == SQL_C_SBIGINT ? static_cast<void*>(&signed_input)
                                         : static_cast<void*>(&unsigned_input);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        c_type, SQL_BIGINT, 19, 0, input, sizeof(SQLBIGINT), &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Int64, seen->parameters[0].type);
    EXPECT_EQ(std::optional<std::string>{"42"}, seen->parameters[0].value);
    EXPECT_FALSE(seen->parameters[0].binary_input);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status);
    EXPECT_EQ(1u, processed);
    EXPECT_EQ(0, seen->disconnects);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

// Historical negative-scale safety motivates strict local refusal, not native parity.
TEST_F(BackendContractTest, NumericInputInvalidDescriptorsAndSignRefuseLocallyAndRecover) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_QUERY_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(2), 0));
  SQLULEN processed = 99;
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  SQL_NUMERIC_STRUCT input{};
  input.precision = 5; input.scale = 2; input.sign = 0;
  input.val[0] = 0x39; input.val[1] = 0x30; // Exact magnitude 12345.
  SQLLEN indicator = sizeof(input);
  SQLHDESC apd = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, &apd, 0, nullptr));
  const auto bind = [&](SQLSMALLINT precision, SQLSMALLINT scale) {
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        SQL_C_NUMERIC, SQL_NUMERIC, 5, 2, &input, sizeof(input), &indicator));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(precision)), 0));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(scale)), 0));
    // Descriptor metadata edits deliberately invalidate the pointer.
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, &input, 0));
  };
  for (const auto& [precision, scale, sign] : {
      std::tuple{SQLSMALLINT{5}, SQLSMALLINT{-1}, SQLCHAR{0}},
      std::tuple{SQLSMALLINT{0}, SQLSMALLINT{0}, SQLCHAR{0}},
      std::tuple{SQLSMALLINT{39}, SQLSMALLINT{2}, SQLCHAR{0}},
      std::tuple{SQLSMALLINT{2}, SQLSMALLINT{3}, SQLCHAR{0}},
      std::tuple{SQLSMALLINT{5}, SQLSMALLINT{2}, SQLCHAR{2}}}) {
    SCOPED_TRACE(precision);
    SCOPED_TRACE(scale);
    SCOPED_TRACE(sign);
    input.sign = sign;
    const auto original = input;
    bind(precision, scale);
    const auto queries = seen->queries;
    const auto previous = seen->parameters;
    const auto deadline = seen->deadline;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
    EXPECT_EQ("22003", state());
    EXPECT_EQ(queries, seen->queries);
    EXPECT_EQ(deadline, seen->deadline);
    ASSERT_EQ(previous.size(), seen->parameters.size());
    if (!previous.empty()) {
      EXPECT_EQ(previous[0].value, seen->parameters[0].value);
    }
    EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
    EXPECT_EQ(1u, processed); EXPECT_EQ(SQL_PARAM_ERROR, status);
    EXPECT_EQ(0, seen->disconnects);
    input.sign = 0;
    bind(5, 2);
    const auto before = rs::util::Clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    const auto after = rs::util::Clock::now();
    EXPECT_EQ(queries + 1, seen->queries);
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Numeric, seen->parameters[0].type);
    EXPECT_FALSE(seen->parameters[0].binary_input);
    EXPECT_EQ(std::optional<std::string>{"-123.45"}, seen->parameters[0].value);
    EXPECT_GE(seen->deadline, before + std::chrono::seconds{2});
    EXPECT_LE(seen->deadline, after + std::chrono::seconds{2});
    EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    input.val[0] = 0;
    EXPECT_EQ(std::optional<std::string>{"-123.45"}, seen->parameters[0].value);
    input.val[0] = 0x39;
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  input.sign = 2; indicator = SQL_NULL_DATA;
  const auto original = input;
  bind(5, -1); // NULL bypasses even invalid material/formatter metadata.
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Numeric, seen->parameters[0].type);
  EXPECT_FALSE(seen->parameters[0].value); EXPECT_FALSE(seen->parameters[0].binary_input);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
  EXPECT_EQ(SQL_NULL_DATA, indicator);
  EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
}

TEST_F(BackendContractTest, NumericInputExact38DigitMagnitudeSignsAndTargetLimits) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99; SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  SQL_NUMERIC_STRUCT input{};
  SQLLEN indicator = sizeof(input);
  SQLHDESC apd = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, SQL_ATTR_APP_PARAM_DESC, &apd, 0, nullptr));
  const auto bind = [&](SQLSMALLINT apd_precision, SQLSMALLINT apd_scale,
                        SQLULEN sql_precision, SQLSMALLINT sql_scale) {
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        SQL_C_NUMERIC, SQL_NUMERIC, sql_precision, sql_scale, &input, sizeof(input), &indicator));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(apd_precision)), 0));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(apd_scale)), 0));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, &input, 0));
  };
  // Independent predetermined base-256 little-endian oracles: 10^38-1 and 10^38.
  const SQLCHAR maximum[]{255,255,255,255,63,34,138,9,122,196,134,90,168,76,59,75};
  const SQLCHAR overflow[]{0,0,0,0,64,34,138,9,122,196,134,90,168,76,59,75};
  const std::string digits(38, '9');
  for (const SQLCHAR sign : {SQLCHAR{0}, SQLCHAR{1}}) {
    input = {}; input.precision = 38; input.sign = sign;
    std::copy(std::begin(maximum), std::end(maximum), std::begin(input.val));
    bind(38, 0, 38, 0);
    const auto queries = seen->queries;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(queries + 1, seen->queries);
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Numeric, seen->parameters[0].type);
    EXPECT_FALSE(seen->parameters[0].binary_input);
    const auto expected = sign ? digits : "-" + digits;
    EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    input = {};
    EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  input = {}; input.precision = 38; input.sign = 1;
  std::copy(std::begin(overflow), std::end(overflow), std::begin(input.val));
  const auto original = input;
  bind(38, 0, 38, 0);
  const auto queries = seen->queries;
  ASSERT_EQ(1u, seen->parameters.size());
  const auto previous = seen->parameters[0].value;
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(queries, seen->queries); EXPECT_EQ(previous, seen->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
  EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
  input = {}; input.sign = 0;
  bind(38, 0, 38, 0);
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"0"}, seen->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  input = {}; input.sign = 1; input.precision = 5; input.scale = 2;
  input.val[0] = 0x39; input.val[1] = 0x30;
  bind(5, 2, 4, 2); // Valid APD material; independent SQL target precision refusal.
  const auto before = seen->queries;
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
  EXPECT_EQ(before, seen->queries); EXPECT_EQ(SQL_PARAM_ERROR, status);
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"0"}, seen->parameters[0].value);
  bind(5, 2, 5, 2);
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(before + 1, seen->queries);
  EXPECT_EQ(std::optional<std::string>{"123.45"}, seen->parameters[0].value);
  EXPECT_EQ(QueryParameterType::Numeric, seen->parameters[0].type);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
  EXPECT_EQ(0, seen->disconnects);
}

TEST_F(BackendContractTest, NumericGetDataARDRefusalAndExplicitDefaultAreDistinct) {
  QueryResult rows;
  rows.columns = {{"ard", NativeTypeInfo{ScalarType::Decimal, 5, 2, true}},
      {"default", NativeTypeInfo{ScalarType::Decimal, 5, 2, true}},
      {"maximum", NativeTypeInfo{ScalarType::Decimal, 38, 0, true}},
      {"overflow", NativeTypeInfo{ScalarType::Decimal, 38, 0, true}},
      {"null", NativeTypeInfo{ScalarType::Decimal, 5, 2, true}},
      {"neighbor", NativeTypeInfo{ScalarType::BigInt, 19, 0, true}}};
  rows.rows = {{"-123.45", "-123.45", std::string(38, '9'), std::string(39, '9'), std::nullopt, "42"},
      {"12.34", "12.34", "1", "1", "0", "42"}};
  seen->date_result = rows; connect();
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  seen->date_result->rows[0][0] = "changed after execution";
  SQLHDESC ard = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_NUMERIC), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
      reinterpret_cast<SQLPOINTER>(5), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
      reinterpret_cast<SQLPOINTER>(std::intptr_t{-1}), 0));
  SQL_NUMERIC_STRUCT output;
  std::memset(&output, 0x5a, sizeof(output));
  const auto canary = output; SQLLEN length = 73;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_ARD_TYPE, &output, sizeof(output), &length));
  EXPECT_EQ("22003", state()); EXPECT_EQ(73, length);
  EXPECT_EQ(0, std::memcmp(&output, &canary, sizeof(output)));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
      reinterpret_cast<SQLPOINTER>(2), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_ARD_TYPE, &output, sizeof(output), &length));
  EXPECT_EQ(5, output.precision); EXPECT_EQ(2, output.scale); EXPECT_EQ(0, output.sign);
  EXPECT_EQ(0x39, output.val[0]); EXPECT_EQ(0x30, output.val[1]);
  EXPECT_TRUE(std::all_of(std::begin(output.val) + 2, std::end(output.val), [](SQLCHAR byte) { return byte == 0; }));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
  EXPECT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, SQL_ARD_TYPE, &output, sizeof(output), &length));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 2, SQL_C_NUMERIC, &output, sizeof(output), &length));
  EXPECT_EQ("01S07", state()); EXPECT_EQ(38, output.precision); EXPECT_EQ(0, output.scale);
  EXPECT_EQ(0, output.sign); EXPECT_EQ(123, output.val[0]);
  EXPECT_TRUE(std::all_of(std::begin(output.val) + 1, std::end(output.val), [](SQLCHAR byte) { return byte == 0; }));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
  const SQLCHAR maximum[]{255,255,255,255,63,34,138,9,122,196,134,90,168,76,59,75};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_NUMERIC, &output, sizeof(output), &length));
  EXPECT_EQ(38, output.precision); EXPECT_EQ(0, output.scale); EXPECT_EQ(1, output.sign);
  EXPECT_EQ(0, std::memcmp(output.val, maximum, sizeof(maximum)));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
  output = canary; length = 73;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 4, SQL_C_NUMERIC, &output, sizeof(output), &length));
  EXPECT_EQ("22003", state()); EXPECT_EQ(73, length);
  EXPECT_EQ(0, std::memcmp(&output, &canary, sizeof(output)));
  char text[64]{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 4, SQL_C_CHAR, text, sizeof(text), &length));
  EXPECT_EQ(std::string(39, '9'), text); EXPECT_EQ(39, length);
  output = canary; length = 73;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 5, SQL_C_NUMERIC, &output, sizeof(output), nullptr));
  EXPECT_EQ("22002", state()); EXPECT_EQ(0, std::memcmp(&output, &canary, sizeof(output)));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 5, SQL_C_NUMERIC, &output, sizeof(output), &length));
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(0, std::memcmp(&output, &canary, sizeof(output)));
  SQLBIGINT neighbor = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 6, SQL_C_SBIGINT, &neighbor, sizeof(neighbor), &length));
  EXPECT_EQ(42, neighbor);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); // ARD has no DATA_PTR: no bound conversion.
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_ARD_TYPE, &output, sizeof(output), &length));
  EXPECT_EQ(5, output.precision); EXPECT_EQ(2, output.scale); EXPECT_EQ(1, output.sign);
  EXPECT_EQ(0xd2, output.val[0]); EXPECT_EQ(4, output.val[1]);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length); EXPECT_EQ(0, seen->disconnects);
}

TEST_F(BackendContractTest, NumericBoundFetchPreservesFailingOutputAndReportsRowStatus) {
  QueryResult rows;
  rows.columns = {{"numeric", NativeTypeInfo{ScalarType::Decimal, 38, 2, true}},
      {"neighbor", NativeTypeInfo{ScalarType::BigInt, 19, 0, true}}};
  rows.rows = {{"-123.45", "42"}, {"-123.45", "42"}, {"12.349", "42"},
      {std::string(39, '9'), "42"}, {std::string(38, '9'), "42"}, {std::nullopt, "42"}, {"0", "42"}};
  seen->date_result = rows; connect();
  SQLUSMALLINT status = SQL_ROW_NOROW; SQLULEN fetched = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0));
  SQL_NUMERIC_STRUCT output; std::memset(&output, 0x5a, sizeof(output));
  const auto canary = output; SQLLEN length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_NUMERIC, &output, sizeof(output), &length));
  SQLHDESC ard = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr));
  const auto set_fields = [&](SQLSMALLINT precision, SQLSMALLINT scale) {
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(precision)), 0));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(scale)), 0));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_DATA_PTR, &output, 0));
  };
  set_fields(5, -1);
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  seen->date_result->rows[1][0] = "changed after execution";
  EXPECT_EQ(SQL_ERROR, SQLFetch(stmt)); EXPECT_EQ("22003", state());
  EXPECT_EQ(SQL_ROW_ERROR, status); EXPECT_EQ(1u, fetched); EXPECT_EQ(73, length);
  EXPECT_EQ(0, std::memcmp(&output, &canary, sizeof(output)));
  SQLBIGINT neighbor = 0; SQLLEN neighbor_length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_SBIGINT, &neighbor, sizeof(neighbor), &neighbor_length));
  EXPECT_EQ(42, neighbor); // Failed bound conversion retains a positioned row.
  set_fields(5, 2);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_SUCCESS, status); EXPECT_EQ(1u, fetched);
  EXPECT_EQ(5, output.precision); EXPECT_EQ(2, output.scale); EXPECT_EQ(0, output.sign);
  EXPECT_EQ(0x39, output.val[0]); EXPECT_EQ(0x30, output.val[1]);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt)); EXPECT_EQ("01S07", state());
  EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO, status); EXPECT_EQ(1u, fetched);
  EXPECT_EQ(5, output.precision); EXPECT_EQ(2, output.scale); EXPECT_EQ(1, output.sign);
  EXPECT_EQ(0xd2, output.val[0]); EXPECT_EQ(4, output.val[1]);
  EXPECT_TRUE(std::all_of(std::begin(output.val) + 2, std::end(output.val), [](SQLCHAR byte) { return byte == 0; }));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
  set_fields(38, 0); output = canary; length = 73;
  EXPECT_EQ(SQL_ERROR, SQLFetch(stmt)); EXPECT_EQ("22003", state());
  EXPECT_EQ(SQL_ROW_ERROR, status); EXPECT_EQ(1u, fetched); EXPECT_EQ(73, length);
  EXPECT_EQ(0, std::memcmp(&output, &canary, sizeof(output)));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_SUCCESS, status); EXPECT_EQ(1u, fetched);
  const SQLCHAR maximum[]{255,255,255,255,63,34,138,9,122,196,134,90,168,76,59,75};
  EXPECT_EQ(38, output.precision); EXPECT_EQ(0, output.scale); EXPECT_EQ(1, output.sign);
  EXPECT_EQ(0, std::memcmp(output.val, maximum, sizeof(maximum)));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
  output = canary; length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_SUCCESS, status); EXPECT_EQ(1u, fetched);
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(0, std::memcmp(&output, &canary, sizeof(output)));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_SUCCESS, status); EXPECT_EQ(1u, fetched);
  EXPECT_EQ(1, output.sign); EXPECT_EQ(38, output.precision); EXPECT_EQ(0, output.scale);
  EXPECT_TRUE(std::all_of(std::begin(output.val), std::end(output.val), [](SQLCHAR byte) { return byte == 0; }));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_NOROW, status); EXPECT_EQ(0u, fetched);
  EXPECT_EQ(0, seen->disconnects);
}

#include <cmath>
#include <limits>

TEST_F(BackendContractTest, FloatingGetDataTargetRangeAndSpecialsPreserveOutputs) {
  connect();
  const auto check = [&]<typename T>(SQLSMALLINT target, ScalarType family) {
    const bool real = target == SQL_C_FLOAT;
    const std::string maximum = real ? "3.4028234663852886e38" : "1.7976931348623157e308";
    const std::string overflow = real ? "3.5e38" : "1e309";
    const std::string underflow = real ? "1e-50" : "1e-400";
    const std::string subnormal = real ? "1e-45" : "5e-324";
    QueryResult rows;
    rows.columns = {{"value", NativeTypeInfo{family, real ? 7u : 15u, static_cast<std::int16_t>(real ? 6 : 15), true}},
        {"neighbor", NativeTypeInfo{ScalarType::BigInt, 19, 0, true}}};
    rows.rows = {{"1.25", "42"}, {"-0", "42"}, {maximum, "42"}, {subnormal, "42"},
        {"NaN", "42"}, {"Infinity", "42"}, {"-Infinity", "42"},
        {overflow, "42"}, {underflow, "42"}, {"1.25 junk", "42"}, {std::nullopt, "42"}, {"1.25", "42"}};
    seen->date_result = rows;
    ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    seen->date_result->rows[0][0] = "mutated source";
    SQLCHAR name[32]{}; SQLSMALLINT name_length = 0, type = 0, digits = 0, nullable = 0; SQLULEN size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, 1, name, sizeof(name), &name_length, &type, &size, &digits, &nullable));
    EXPECT_EQ(real ? SQL_REAL : SQL_DOUBLE, type); EXPECT_EQ(real ? 7u : 15u, size);
    T output = T{17}; SQLLEN length = 73;
    for (unsigned row = 0; row != 12; ++row) {
      SCOPED_TRACE(target);
      SCOPED_TRACE(row);
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      output = T{17}; length = 73; const auto original = output;
      if (row >= 7 && row <= 9) {
        EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, target, &output, sizeof(output), &length));
        EXPECT_EQ(row == 9 ? "22018" : "22003", state());
        EXPECT_EQ(0, std::memcmp(&output, &original, sizeof(output))); EXPECT_EQ(73, length);
        char text[64]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, text, sizeof(text), &length));
        EXPECT_EQ(*rows.rows[row][0], text);
      } else if (row == 10) {
        EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, target, &output, sizeof(output), nullptr));
        EXPECT_EQ("22002", state()); EXPECT_EQ(0, std::memcmp(&output, &original, sizeof(output)));
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, target, &output, sizeof(output), &length));
        EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(0, std::memcmp(&output, &original, sizeof(output)));
      } else {
        if (row >= 4 && row <= 6) {
          SQL_NUMERIC_STRUCT numeric; std::memset(&numeric, 0x5a, sizeof(numeric));
          const auto canary = numeric; SQLLEN numeric_length = 73;
          EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_NUMERIC, &numeric, sizeof(numeric), &numeric_length));
          EXPECT_EQ("22003", state()); EXPECT_EQ(73, numeric_length);
          EXPECT_EQ(0, std::memcmp(&numeric, &canary, sizeof(numeric)));
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, target, &output, sizeof(output), &length));
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
        if (row == 0 || row == 11) {
          EXPECT_EQ(T{1.25}, output);
        } else if (row == 1) {
          EXPECT_EQ(T{0}, output); EXPECT_TRUE(std::signbit(output));
        } else if (row == 2) {
          EXPECT_EQ(std::numeric_limits<T>::max(), output);
        } else if (row == 3) {
          EXPECT_NE(T{0}, output); EXPECT_TRUE(std::isfinite(output));
          EXPECT_EQ(FP_SUBNORMAL, std::fpclassify(output));
          EXPECT_EQ(std::numeric_limits<T>::denorm_min(), output);
        } else if (row == 4) {
          EXPECT_TRUE(std::isnan(output));
        } else {
          EXPECT_TRUE(std::isinf(output)); EXPECT_EQ(row == 6, std::signbit(output));
        }
      }
      SQLBIGINT neighbor = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_SBIGINT, &neighbor, sizeof(neighbor), &length));
      EXPECT_EQ(42, neighbor);
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    EXPECT_EQ(0, seen->disconnects);
  };
  check.template operator()<SQLREAL>(SQL_C_FLOAT, ScalarType::Real);
  check.template operator()<SQLDOUBLE>(SQL_C_DOUBLE, ScalarType::Double);
}

TEST_F(BackendContractTest, FloatingBoundFetchReportsRangeErrorAndPreservesFailingCell) {
  connect();
  SQLUSMALLINT status = SQL_ROW_NOROW; SQLULEN fetched = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0));
  const auto check = [&]<typename T>(SQLSMALLINT target, ScalarType family) {
    const bool real = target == SQL_C_FLOAT;
    QueryResult rows;
    rows.columns = {{"value", NativeTypeInfo{family, real ? 7u : 15u, static_cast<std::int16_t>(real ? 6 : 15), true}}};
    rows.rows = {{real ? "3.5e38" : "1e309"}, {"1.25"}, {"-0"}, {"NaN"},
        {std::nullopt}, {real ? "1e-50" : "1e-400"}, {"1.25"}};
    seen->date_result = rows;
    T output = T{17}; SQLLEN length = 73;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, target, &output, sizeof(output), &length));
    ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    seen->date_result->rows[1][0] = "mutated source";
    for (unsigned row = 0; row != 7; ++row) {
      SCOPED_TRACE(target);
      SCOPED_TRACE(row);
      output = T{17}; length = 73; const auto original = output;
      if (row == 0 || row == 5) {
        EXPECT_EQ(SQL_ERROR, SQLFetch(stmt)); EXPECT_EQ("22003", state());
        EXPECT_EQ(SQL_ROW_ERROR, status); EXPECT_EQ(1u, fetched); EXPECT_EQ(73, length);
        EXPECT_EQ(0, std::memcmp(&output, &original, sizeof(output)));
      } else {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_SUCCESS, status); EXPECT_EQ(1u, fetched);
        if (row == 4) {
          EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(0, std::memcmp(&output, &original, sizeof(output)));
        } else {
          EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
          if (row == 2) {
            EXPECT_EQ(T{0}, output); EXPECT_TRUE(std::signbit(output));
          } else if (row == 3) {
            EXPECT_TRUE(std::isnan(output));
          } else {
            EXPECT_EQ(T{1.25}, output);
          }
        }
      }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_NOROW, status); EXPECT_EQ(0u, fetched);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
    EXPECT_EQ(0, seen->disconnects);
  };
  check.template operator()<SQLREAL>(SQL_C_FLOAT, ScalarType::Real);
  check.template operator()<SQLDOUBLE>(SQL_C_DOUBLE, ScalarType::Double);
}

TEST_F(BackendContractTest, FloatingPreparedSameWidthOwnsValuesAndRefusesNonfiniteNumericTarget) {
  connect(); ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99; SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  const auto check = [&]<typename T>(SQLSMALLINT c_type, SQLSMALLINT sql_type, QueryParameterType hint) {
    for (unsigned index = 0; index != 5; ++index) {
      SCOPED_TRACE(c_type);
      SCOPED_TRACE(index);
      T input = index == 0 ? T{1.25} : index == 1 ? -T{0} : index == 2
          ? std::numeric_limits<T>::quiet_NaN() : index == 3
          ? std::numeric_limits<T>::infinity() : -std::numeric_limits<T>::infinity();
      const auto original = input; SQLLEN indicator = sizeof(input);
      const std::string expected = index == 0 ? "1.25" : index == 1 ? "-0" : index == 2 ? "NaN" : index == 3 ? "Infinity" : "-Infinity";
      ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, c_type, sql_type,
          sizeof(T) == sizeof(SQLREAL) ? 7 : 15, 0, &input, sizeof(input), &indicator));
      const auto queries = seen->queries;
      ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); EXPECT_EQ(queries + 1, seen->queries);
      ASSERT_EQ(1u, seen->parameters.size());
      EXPECT_EQ(hint, seen->parameters[0].type); EXPECT_FALSE(seen->parameters[0].binary_input);
      EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
      EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
      EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
      input = T{17}; EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
      if (index >= 2) {
        input = original;
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, c_type, SQL_NUMERIC,
            3, 2, &input, sizeof(input), &indicator));
        const auto before = seen->queries;
        EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
        EXPECT_EQ(before, seen->queries); EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
        EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
        ASSERT_EQ(1u, seen->parameters.size()); EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
        input = T{1.25};
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); EXPECT_EQ(before + 1, seen->queries);
        ASSERT_EQ(1u, seen->parameters.size()); EXPECT_EQ(QueryParameterType::Numeric, seen->parameters[0].type);
        EXPECT_EQ(std::optional<std::string>{"1.25"}, seen->parameters[0].value);
        EXPECT_EQ(SQL_PARAM_SUCCESS, status); ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
      }
    }
    SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, c_type, sql_type,
        sizeof(T) == sizeof(SQLREAL) ? 7 : 15, 0, nullptr, 0, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(hint, seen->parameters[0].type); EXPECT_FALSE(seen->parameters[0].value);
    EXPECT_FALSE(seen->parameters[0].binary_input); EXPECT_EQ(SQL_NULL_DATA, indicator);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt)); EXPECT_EQ(0, seen->disconnects);
  };
  check.template operator()<SQLREAL>(SQL_C_FLOAT, SQL_REAL, QueryParameterType::Float32);
  check.template operator()<SQLDOUBLE>(SQL_C_DOUBLE, SQL_DOUBLE, QueryParameterType::Float64);
}

#include <bit>
#include <sstream>
#include <locale>
#include <cfenv>
#include <cmath>
#include <limits>

// Deliberately red until the separately reviewed shared ODBC narrowing repair.
TEST_F(BackendContractTest, DoubleRealRangeRefusesLocallyAndRecovers) {
  ASSERT_EQ(FE_TONEAREST, std::fegetround()); // Observe; do not alter process environment.
  static_assert(sizeof(SQLREAL) == 4 && sizeof(SQLDOUBLE) == 8);
  static_assert(std::numeric_limits<SQLREAL>::is_iec559 && std::numeric_limits<SQLDOUBLE>::is_iec559);
  connect(); ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99; SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  SQLDOUBLE input = 1.25; SQLLEN indicator = sizeof(input);
  const auto bind = [&] {
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        SQL_C_DOUBLE, SQL_REAL, 7, 0, &input, sizeof(input), &indicator));
  };
  bind(); ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"1.25"}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  const SQLDOUBLE maximum = 0x1.fffffep+127; // IEEE binary32 max promoted exactly.
  for (const SQLDOUBLE invalid : {
      std::nextafter(maximum, std::numeric_limits<SQLDOUBLE>::infinity()),
      std::nextafter(-maximum, -std::numeric_limits<SQLDOUBLE>::infinity()),
      std::numeric_limits<SQLDOUBLE>::max(), -std::numeric_limits<SQLDOUBLE>::max(),
      SQLDOUBLE{1e-50}, SQLDOUBLE{-1e-50}}) {
    SCOPED_TRACE(invalid);
    input = invalid; const auto original = input;
    bind(); const auto queries = seen->queries; const auto deadline = seen->deadline;
    ASSERT_EQ(1u, seen->parameters.size()); const auto previous = seen->parameters[0].value;
    const auto result = SQLExecute(stmt);
    EXPECT_EQ(SQL_ERROR, result); EXPECT_EQ("22003", state());
    EXPECT_EQ(queries, seen->queries); EXPECT_EQ(deadline, seen->deadline);
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
    ASSERT_EQ(1u, seen->parameters.size()); EXPECT_EQ(previous, seen->parameters[0].value);
    if (result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO) {
      // Cleanup an unexpected baseline dispatch so later controls still execute.
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    }
    input = 1.25; bind(); const auto before_recovery = seen->queries;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); EXPECT_EQ(before_recovery + 1, seen->queries);
    ASSERT_EQ(1u, seen->parameters.size()); EXPECT_EQ(QueryParameterType::Float32, seen->parameters[0].type);
    EXPECT_EQ(std::optional<std::string>{"1.25"}, seen->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  input = maximum; bind(); ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Float32, seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"3.4028235e+38"}, seen->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(0, seen->disconnects);
}

TEST_F(BackendContractTest, DoubleRealRoundingMidpointsSpecialsAndNullOwnFloat32Intent) {
  ASSERT_EQ(FE_TONEAREST, std::fegetround());
  static_assert(sizeof(SQLREAL) == 4 && sizeof(SQLDOUBLE) == 8);
  static_assert(std::numeric_limits<SQLREAL>::is_iec559 && std::numeric_limits<SQLDOUBLE>::is_iec559);
  connect(); ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99; SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  const SQLDOUBLE midpoint_even_low = 0x1.000001p+0; // Halfway between float bits 0x3f800000/1.
  const SQLDOUBLE midpoint_even_high = 0x1.000003p+0; // Halfway between float bits 0x3f800001/2.
  struct Case { SQLDOUBLE input; std::uint32_t rounded_bits; const char* text; };
  const Case cases[]{
      {1.25, 0x3fa00000u, "1.25"},
      {1.00000006, 0x3f800001u, "1.0000001"},
      {midpoint_even_low, 0x3f800000u, "1"},
      {std::nextafter(midpoint_even_low, 0.0), 0x3f800000u, "1"},
      {std::nextafter(midpoint_even_low, 2.0), 0x3f800001u, "1.0000001"},
      {midpoint_even_high, 0x3f800002u, "1.0000002"},
      {std::nextafter(midpoint_even_high, 0.0), 0x3f800001u, "1.0000001"},
      {std::nextafter(midpoint_even_high, 2.0), 0x3f800002u, "1.0000002"},
      {-midpoint_even_low, 0xbf800000u, "-1"},
      {-std::nextafter(midpoint_even_low, 0.0), 0xbf800000u, "-1"},
      {-std::nextafter(midpoint_even_low, 2.0), 0xbf800001u, "-1.0000001"},
      {-midpoint_even_high, 0xbf800002u, "-1.0000002"},
      {-std::nextafter(midpoint_even_high, 0.0), 0xbf800001u, "-1.0000001"},
      {-std::nextafter(midpoint_even_high, 2.0), 0xbf800002u, "-1.0000002"},
      {0x1p-149, 1u, "1e-45"},
      {-0x1p-149, 0x80000001u, "-1e-45"},
      {0.0, 0u, "0"}, {-0.0, 0x80000000u, "-0"}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item.text);
    const SQLREAL expected = std::bit_cast<SQLREAL>(item.rounded_bits);
    if (item.rounded_bits == 1u || item.rounded_bits == 0x80000001u) {
      EXPECT_NE(0.0f, expected); EXPECT_TRUE(std::isfinite(expected));
      EXPECT_EQ(FP_SUBNORMAL, std::fpclassify(expected));
    }
    // Literal binary32 bit oracle is independent of the future narrowing helper.
    SQLDOUBLE input = item.input; const auto original = input; SQLLEN indicator = sizeof(input);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DOUBLE,
        SQL_REAL, 7, 0, &input, sizeof(input), &indicator));
    const auto queries = seen->queries;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); EXPECT_EQ(queries + 1, seen->queries);
    ASSERT_EQ(1u, seen->parameters.size()); EXPECT_EQ(QueryParameterType::Float32, seen->parameters[0].type);
    EXPECT_FALSE(seen->parameters[0].binary_input);
    EXPECT_EQ(std::optional<std::string>{item.text}, seen->parameters[0].value);
    ASSERT_TRUE(seen->parameters[0].value.has_value());
    const auto& captured = *seen->parameters[0].value;
    SQLDOUBLE decoded = 17.0;
    std::istringstream parser(captured);
    parser.imbue(std::locale::classic());
    parser >> decoded;
    ASSERT_FALSE(parser.fail());
    EXPECT_EQ(std::char_traits<char>::eof(), parser.peek());
    ASSERT_TRUE(std::isfinite(decoded));
    ASSERT_LE(std::fabs(decoded), 0x1.fffffep+127);
    const SQLREAL encoded = static_cast<SQLREAL>(decoded);
    EXPECT_EQ(item.rounded_bits, std::bit_cast<std::uint32_t>(encoded));
    EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), indicator);
    input = 17.0;
    EXPECT_EQ(std::optional<std::string>{item.text}, seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  for (const auto& [special, text] : {
      std::pair{std::numeric_limits<SQLDOUBLE>::quiet_NaN(), "NaN"},
      std::pair{std::numeric_limits<SQLDOUBLE>::infinity(), "Infinity"},
      std::pair{-std::numeric_limits<SQLDOUBLE>::infinity(), "-Infinity"}}) {
    SQLDOUBLE input = special; SQLLEN indicator = sizeof(input);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DOUBLE,
        SQL_REAL, 7, 0, &input, sizeof(input), &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(QueryParameterType::Float32, seen->parameters[0].type);
    EXPECT_EQ(std::optional<std::string>{text}, seen->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    input = 17.0; EXPECT_EQ(std::optional<std::string>{text}, seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  SQLLEN indicator = SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DOUBLE,
      SQL_REAL, 7, 0, nullptr, 0, &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Float32, seen->parameters[0].type);
  EXPECT_FALSE(seen->parameters[0].value); EXPECT_FALSE(seen->parameters[0].binary_input);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed); EXPECT_EQ(SQL_NULL_DATA, indicator);
  EXPECT_EQ(0, seen->disconnects);
}

// Public adapter observations only: no native embedded-NUL or VARBYTE parameter claim.
TEST_F(BackendContractTest, PreparedTextLengthsOwnUnicodeNulAndRecoverFromLocalRefusals) {
  connect();
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99; SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  const std::string expected("A\0\xe2\x82\xac\xf0\x9f\x98\x80", 9);
  std::string narrow = expected; SQLLEN length = 9;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 9, 0, narrow.data(), narrow.size(), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Text, seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
  narrow[0] = 'Z';
  EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  narrow = expected;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 8, 0, narrow.data(), narrow.size(), &length));
  const auto queries = seen->queries; const auto deadline = seen->deadline;
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22001", state());
  EXPECT_EQ(queries, seen->queries); EXPECT_EQ(deadline, seen->deadline);
  EXPECT_EQ(1u, processed); EXPECT_EQ(SQL_PARAM_ERROR, status);
  EXPECT_EQ(expected, narrow); EXPECT_EQ(9, length);
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 9, 0, narrow.data(), narrow.size(), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); EXPECT_EQ(SQL_PARAM_SUCCESS, status);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  length = SQL_NTS;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_VARCHAR, 9, 0, narrow.data(), narrow.size(), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"A"}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));

  std::vector<SQLWCHAR> wide;
  if constexpr (sizeof(SQLWCHAR) == 2) { wide = {'A', 0, 0x20ac, 0xd83d, 0xde00}; }
  else { wide = {'A', 0, 0x20ac, static_cast<SQLWCHAR>(0x1f600)}; }
  length = static_cast<SQLLEN>(wide.size() * sizeof(SQLWCHAR));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
      SQL_WVARCHAR, 4, 0, wide.data(), length, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
  wide[0] = 'Z'; EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
  wide[0] = 'A'; ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  const auto valid_length = length;
  length = SQL_NTS;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
      SQL_WVARCHAR, 4, 0, wide.data(), valid_length, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"A"}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  length = valid_length;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
      SQL_WVARCHAR, 4, 0, wide.data(), valid_length, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  for (const int error : {0, 1, 2}) {
    SCOPED_TRACE(error);
    length = error == 1 ? valid_length - 1 : valid_length;
    if (error == 2) { wide[0] = static_cast<SQLWCHAR>(0xd800); }
    const auto saved = wide;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
        SQL_WVARCHAR, error == 0 ? 3 : 4, 0, wide.data(), valid_length, &length));
    const auto count = seen->queries; const auto prior_deadline = seen->deadline;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
    EXPECT_EQ(error == 0 ? "22001" : error == 1 ? "HY090" : "22018", state());
    EXPECT_EQ(count, seen->queries); EXPECT_EQ(prior_deadline, seen->deadline);
    EXPECT_EQ(saved, wide); EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(std::optional<std::string>{expected}, seen->parameters[0].value);
    wide[0] = 'A'; length = valid_length;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
        SQL_WVARCHAR, 4, 0, wide.data(), valid_length, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); EXPECT_EQ(SQL_PARAM_SUCCESS, status);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  wide[0] = static_cast<SQLWCHAR>(0xd800); length = SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
      SQL_WVARCHAR, 4, 0, wide.data(), valid_length, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size()); EXPECT_FALSE(seen->parameters[0].value);
  EXPECT_EQ(static_cast<SQLWCHAR>(0xd800), wide[0]);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
      SQL_WVARCHAR, 4, 0, wide.data(), valid_length, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(std::optional<std::string>{""}, seen->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed); EXPECT_EQ(0, seen->disconnects);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, TextGetDataChunksCountBytesAndPreserveWideScalarBoundaries) {
  QueryResult result;
  result.columns = {{"text", NativeTypeInfo{ScalarType::VarChar, 32, 0, true}}};
  const std::string text("A\xe2\x82\xac\xf0\x9f\x98\x80\0B", 10);
  result.rows = {{text}, {"ok"}}; seen->date_result = result; connect();
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  char bytes[3]{'x','x','x'}; SQLLEN length = -1; std::string reconstructed;
  for (int part = 0; part < 5; ++part) {
    ASSERT_EQ(part == 4 ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO,
        SQLGetData(stmt, 1, SQL_C_CHAR, bytes, sizeof(bytes), &length));
    EXPECT_EQ(10 - part * 2, length); EXPECT_EQ(0, bytes[2]);
    reconstructed.append(bytes, 2);
    if (part < 4) { EXPECT_EQ("01004", state()); }
    if (part == 0) { EXPECT_EQ(static_cast<unsigned char>(0xe2), static_cast<unsigned char>(bytes[1])); }
  }
  EXPECT_EQ(text, reconstructed);
  EXPECT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, SQL_C_CHAR, bytes, sizeof(bytes), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  SQLWCHAR out[4]{'x','x','x','x'};
  const SQLLEN total = (sizeof(SQLWCHAR) == 2 ? 6 : 5) * sizeof(SQLWCHAR);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_WCHAR, out, 2*sizeof(SQLWCHAR), &length));
  EXPECT_EQ(total, length); EXPECT_EQ('A', out[0]); EXPECT_EQ(0, out[1]); EXPECT_EQ('x', out[2]);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_WCHAR, out, 2*sizeof(SQLWCHAR), &length));
  EXPECT_EQ(total - sizeof(SQLWCHAR), length); EXPECT_EQ(0x20ac, out[0]); EXPECT_EQ(0, out[1]);
  // Euro is consumed; the next scalar is now the supplementary smile.
  const SQLLEN remaining = total - 2*sizeof(SQLWCHAR);
  out[0] = 'x'; out[1] = 'x';
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_WCHAR, out, sizeof(SQLWCHAR), &length));
  EXPECT_EQ(remaining, length); EXPECT_EQ(0, out[0]); EXPECT_EQ('x', out[1]);
  if constexpr (sizeof(SQLWCHAR) == 2) {
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_WCHAR, out, 2*sizeof(SQLWCHAR), &length));
    EXPECT_EQ(remaining, length); EXPECT_EQ(0, out[0]); EXPECT_EQ('x', out[1]);
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_WCHAR, out, 3*sizeof(SQLWCHAR), &length));
    EXPECT_EQ(remaining, length); EXPECT_EQ(0xd83d, out[0]); EXPECT_EQ(0xde00, out[1]); EXPECT_EQ(0, out[2]);
  } else {
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_WCHAR, out, 2*sizeof(SQLWCHAR), &length));
    EXPECT_EQ(remaining, length); EXPECT_EQ(0x1f600, out[0]); EXPECT_EQ(0, out[1]);
  }
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_WCHAR, out, 3*sizeof(SQLWCHAR), &length));
  const SQLWCHAR tail[]{0,'B',0}; EXPECT_EQ(2*sizeof(SQLWCHAR), length);
  EXPECT_EQ(0, std::memcmp(out, tail, sizeof(tail)));
  EXPECT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, SQL_C_WCHAR, out, sizeof(out), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_WCHAR, out, sizeof(out), &length));
  EXPECT_EQ(2*sizeof(SQLWCHAR), length); EXPECT_EQ('o', out[0]); EXPECT_EQ('k', out[1]); EXPECT_EQ(0, out[2]);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, BoundTextAndLongBinaryKeepFullLengthsPairBoundariesAndNull) {
  QueryResult result;
  result.columns = {{"text", NativeTypeInfo{ScalarType::VarChar, 32, 0, true}}};
  result.rows = {{std::string("A\xf0\x9f\x98\x80\0B", 7)}, {""}, {std::nullopt}};
  seen->date_result = result; connect();
  SQLUSMALLINT status = SQL_ROW_NOROW;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &status, 0));
  SQLWCHAR output[6]{'x','x','x','x','x','x'}; SQLLEN length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_WCHAR, output, 3*sizeof(SQLWCHAR), &length));
  ASSERT_EQ(SQL_SUCCESS, execute("rows"));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt)); EXPECT_EQ("01004", state()); EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO, status);
  EXPECT_EQ((sizeof(SQLWCHAR) == 2 ? 5 : 4)*sizeof(SQLWCHAR), length);
  EXPECT_EQ('A', output[0]);
  if constexpr (sizeof(SQLWCHAR) == 2) { EXPECT_EQ(0, output[1]); EXPECT_EQ('x', output[2]); }
  else { EXPECT_EQ(0x1f600, output[1]); EXPECT_EQ(0, output[2]); }
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(0, length); EXPECT_EQ(0, output[0]); EXPECT_EQ(SQL_ROW_SUCCESS, status);
  std::fill(std::begin(output), std::end(output), SQLWCHAR('x'));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(SQL_ROW_SUCCESS, status);
  EXPECT_TRUE(std::all_of(std::begin(output), std::end(output), [](auto unit) { return unit == 'x'; }));
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt)); EXPECT_EQ(SQL_ROW_NOROW, status);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_WCHAR, output, sizeof(output), &length));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  std::vector<SQLWCHAR> expected;
  if constexpr (sizeof(SQLWCHAR) == 2) { expected = {'A',0xd83d,0xde00,0,'B',0}; }
  else { expected = {'A',static_cast<SQLWCHAR>(0x1f600),0,'B',0}; }
  EXPECT_EQ(0, std::memcmp(output, expected.data(), expected.size()*sizeof(SQLWCHAR)));
  EXPECT_EQ(SQL_ROW_SUCCESS, status);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
  seen->date_result.reset(); seen->long_binary = true;
  char narrow[8]{'x','x','x','x','x','x','x','x'};
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_CHAR, narrow, 4, &length));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt));
  EXPECT_EQ("01004", state()); EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO, status); EXPECT_EQ(6, length);
  EXPECT_EQ(0, std::memcmp(narrow, "00\0", 3)); EXPECT_EQ('x', narrow[3]);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
  std::fill(std::begin(output), std::end(output), SQLWCHAR('x'));
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_WCHAR, output, 4*sizeof(SQLWCHAR), &length));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt));
  const SQLWCHAR hex_prefix[]{'0','0',0}; EXPECT_EQ(0, std::memcmp(output, hex_prefix, sizeof(hex_prefix)));
  EXPECT_EQ('x', output[3]); EXPECT_EQ(6*sizeof(SQLWCHAR), length); EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO, status);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_CHAR, narrow, sizeof(narrow), &length));
  ASSERT_EQ(SQL_SUCCESS, execute("rows")); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(0, std::memcmp(narrow, "00ff5c\0", 7)); EXPECT_EQ(6, length); EXPECT_EQ(SQL_ROW_SUCCESS, status);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, PreparedBitNumericRefusalsPreserveOwningValuesAndRecover) {
  connect(); ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"rows ?", SQL_NTS));
  SQLULEN processed = 99; SQLUSMALLINT status = SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  std::string text = "1"; SQLLEN length = 1;
  const auto bind_text = [&] { return SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_BIT, 1, 0, text.data(), static_cast<SQLLEN>(text.size()), &length); };
  ASSERT_EQ(SQL_SUCCESS, bind_text()); ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size());
  EXPECT_EQ(QueryParameterType::Boolean, seen->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"1"}, seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  for (const auto& invalid : std::vector<std::pair<std::string, std::string>>{
      {"0.5","22001"},{"1.5","22001"},{"1e-100","22001"},
      {"-1","22003"},{"2","22003"},{"maybe","22018"}}) {
    SCOPED_TRACE(invalid.first); text = invalid.first; length = static_cast<SQLLEN>(text.size());
    ASSERT_EQ(SQL_SUCCESS, bind_text());
    const auto queries = seen->queries; const auto deadline = seen->deadline;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ(invalid.second, state());
    EXPECT_EQ(queries, seen->queries); EXPECT_EQ(deadline, seen->deadline);
    EXPECT_EQ(invalid.first, text); EXPECT_EQ(static_cast<SQLLEN>(text.size()), length);
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    ASSERT_EQ(1u, seen->parameters.size()); EXPECT_EQ(std::optional<std::string>{"1"}, seen->parameters[0].value);
    text = "1"; length = 1; ASSERT_EQ(SQL_SUCCESS, bind_text());
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); EXPECT_EQ(SQL_PARAM_SUCCESS, status);
    text[0] = '0'; EXPECT_EQ(std::optional<std::string>{"1"}, seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  SQLDOUBLE number = 0; length = sizeof(number);
  const auto bind_number = [&] { return SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DOUBLE,
      SQL_BIT, 1, 0, &number, sizeof(number), &length); };
  for (const SQLDOUBLE valid : {0.0,1.0}) {
    number = valid; ASSERT_EQ(SQL_SUCCESS, bind_number()); ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, seen->parameters.size());
    EXPECT_EQ(std::optional<std::string>{valid == 0 ? "0" : "1"}, seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  for (const SQLDOUBLE invalid : {0.5,-1.0,2.0,std::numeric_limits<SQLDOUBLE>::infinity(),
                                 std::numeric_limits<SQLDOUBLE>::quiet_NaN()}) {
    number = invalid; const auto bytes = std::bit_cast<std::uint64_t>(number);
    ASSERT_EQ(SQL_SUCCESS, bind_number());
    const auto queries = seen->queries; const auto deadline = seen->deadline;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ(invalid == 0.5 ? "22001" : "22003", state());
    EXPECT_EQ(queries, seen->queries); EXPECT_EQ(deadline, seen->deadline);
    EXPECT_EQ(bytes, std::bit_cast<std::uint64_t>(number)); EXPECT_EQ(static_cast<SQLLEN>(sizeof(number)), length);
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    ASSERT_EQ(1u, seen->parameters.size()); EXPECT_EQ(std::optional<std::string>{"1"}, seen->parameters[0].value);
    number = 1; ASSERT_EQ(SQL_SUCCESS, bind_number()); ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(SQL_PARAM_SUCCESS, status); ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  number = std::numeric_limits<SQLDOUBLE>::quiet_NaN(); length = SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS, bind_number()); ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, seen->parameters.size()); EXPECT_EQ(QueryParameterType::Boolean, seen->parameters[0].type);
  EXPECT_FALSE(seen->parameters[0].value); EXPECT_TRUE(std::isnan(number)); EXPECT_EQ(SQL_NULL_DATA, length);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(0, seen->disconnects); ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, BooleanResultTinyBuffersPreserveValuesAndNullRetry) {
  QueryResult result; result.columns = {{"flag",NativeTypeInfo{ScalarType::Boolean,1,0,true}}};
  result.rows = {{"1"},{"0"},{std::nullopt}}; seen->date_result = result; connect();
  for (const SQLSMALLINT target : {SQLSMALLINT(SQL_C_BINARY),SQLSMALLINT(SQL_C_CHAR),SQLSMALLINT(SQL_C_WCHAR)}) {
    SCOPED_TRACE(target); ASSERT_EQ(SQL_SUCCESS, execute("rows"));
    SQLSMALLINT type = 0; SQLULEN size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt,1,nullptr,0,nullptr,&type,&size,nullptr,nullptr));
    EXPECT_EQ(SQL_BIT,type); EXPECT_EQ(1u,size);
    for (int row = 0; row < 2; ++row) {
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      alignas(SQLWCHAR) unsigned char output[3*sizeof(SQLWCHAR)]; std::fill(std::begin(output),std::end(output),0x5a);
      SQLLEN length = 73; const SQLLEN tiny = target == SQL_C_BINARY ? 0 : target == SQL_C_CHAR ? 1 : sizeof(SQLWCHAR);
      EXPECT_EQ(SQL_ERROR,SQLGetData(stmt,1,target,output,tiny,&length)); EXPECT_EQ("22003",state()); EXPECT_EQ(73,length);
      EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto b){return b==0x5a;}));
      ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,target,output,sizeof(output),&length));
      if (target == SQL_C_BINARY) { EXPECT_EQ(row == 0 ? 1 : 0,output[0]); EXPECT_EQ(1,length); EXPECT_EQ(0x5a,output[1]); }
      else if (target == SQL_C_CHAR) { EXPECT_EQ(row == 0 ? '1' : '0',output[0]); EXPECT_EQ(0,output[1]); EXPECT_EQ(1,length); }
      else { SQLWCHAR units[2];std::memcpy(units,output,sizeof(units));EXPECT_EQ(row == 0 ? '1' : '0',units[0]);EXPECT_EQ(0,units[1]);EXPECT_EQ(sizeof(SQLWCHAR),length); }
    }
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt)); unsigned char output[16];std::fill(std::begin(output),std::end(output),0x5a);
    EXPECT_EQ(SQL_ERROR,SQLGetData(stmt,1,target,output,sizeof(output),nullptr));EXPECT_EQ("22002",state());
    SQLLEN length = 73;ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,target,output,sizeof(output),&length));EXPECT_EQ(SQL_NULL_DATA,length);
    EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto b){return b==0x5a;}));
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
  SQLCHAR output = 73; SQLLEN length = 74;SQLUSMALLINT status = SQL_ROW_NOROW;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_ROW_STATUS_PTR,&status,0));
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt,1,SQL_C_BINARY,&output,0,&length));ASSERT_EQ(SQL_SUCCESS,execute("rows"));
  EXPECT_EQ(SQL_ERROR,SQLFetch(stmt));EXPECT_EQ("22003",state());EXPECT_EQ(SQL_ROW_ERROR,status);EXPECT_EQ(73,output);EXPECT_EQ(74,length);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt,1,SQL_C_BINARY,&output,1,&length));
  ASSERT_EQ(SQL_SUCCESS,execute("rows"));ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));EXPECT_EQ(1,output);EXPECT_EQ(1,length);EXPECT_EQ(SQL_ROW_SUCCESS,status);
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));EXPECT_EQ(0,output);output=73;
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));EXPECT_EQ(SQL_NULL_DATA,length);EXPECT_EQ(73,output);EXPECT_EQ(SQL_ROW_SUCCESS,status);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, UnsupportedIntervalGuidInterfacesPreserveSupportedTextBindings) {
  QueryResult result;result.columns={{"text",NativeTypeInfo{ScalarType::VarChar,16,0,true}}};result.rows={{"ok"}};
  seen->date_result=result;connect();
  std::vector<SQLSMALLINT> ctypes{SQL_C_INTERVAL_YEAR_TO_MONTH,SQL_C_INTERVAL_DAY_TO_SECOND};
#ifdef SQL_C_GUID
  ctypes.push_back(SQL_C_GUID);
#endif
  char output[16]="untouched";SQLLEN length=73;
  ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt,1,SQL_C_CHAR,output,sizeof(output),&length));
  for (const auto unsupported : ctypes) {
    SCOPED_TRACE(unsupported);std::memset(output,'x',sizeof(output));length=73;
    EXPECT_EQ(SQL_ERROR,SQLBindCol(stmt,1,unsupported,output,sizeof(output),&length));EXPECT_EQ("HYC00",state());
    ASSERT_EQ(SQL_SUCCESS,execute("rows"));ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt));
    EXPECT_EQ(0,std::memcmp(output,"ok\0",3));EXPECT_EQ(2,length); // prior CHAR binding survived
    char canary[16];std::fill(std::begin(canary),std::end(canary),'x');SQLLEN indicator=74;
    EXPECT_EQ(SQL_ERROR,SQLGetData(stmt,1,unsupported,canary,sizeof(canary),&indicator));EXPECT_EQ("HYC00",state());EXPECT_EQ(74,indicator);
    EXPECT_TRUE(std::all_of(std::begin(canary),std::end(canary),[](auto b){return b=='x';}));
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt,1,SQL_C_CHAR,canary,sizeof(canary),&indicator));EXPECT_EQ(0,std::memcmp(canary,"ok\0",3));EXPECT_EQ(2,indicator);
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_UNBIND));ASSERT_EQ(SQL_SUCCESS,SQLPrepare(stmt,(SQLCHAR*)"rows ?",SQL_NTS));
  char input[]="ok";SQLLEN input_length=2;SQLULEN processed=99;SQLUSMALLINT status=SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAM_STATUS_PTR,&status,0));
  std::vector<SQLSMALLINT> sqltypes{SQL_INTERVAL_YEAR_TO_MONTH,SQL_INTERVAL_DAY_TO_SECOND};
#ifdef SQL_GUID
  sqltypes.push_back(SQL_GUID);
#endif
  for (std::size_t i=0;i<ctypes.size()+sqltypes.size();++i) {
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,16,0,input,sizeof(input),&input_length));
    processed=99;status=SQL_PARAM_UNUSED;const auto queries=seen->queries;
    const auto ctype=i<ctypes.size()?ctypes[i]:SQLSMALLINT(SQL_C_CHAR);
    const auto sqltype=i<ctypes.size()?SQLSMALLINT(SQL_VARCHAR):sqltypes[i-ctypes.size()];
    EXPECT_EQ(SQL_ERROR,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,ctype,sqltype,16,0,input,sizeof(input),&input_length));EXPECT_EQ("HYC00",state());
    EXPECT_EQ(queries,seen->queries);EXPECT_EQ(99u,processed);EXPECT_EQ(SQL_PARAM_UNUSED,status);EXPECT_EQ(2,input_length);EXPECT_EQ(0,std::memcmp(input,"ok\0",3));
    ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(1u,seen->parameters.size());EXPECT_EQ(std::optional<std::string>{"ok"},seen->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_SUCCESS,status);EXPECT_EQ(1u,processed);ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
  EXPECT_EQ(0,seen->disconnects);
}

// Deliberate precode regressions: canonical typed BIT input is a local policy.
// Current nonzero normalization is expected to fail the invalid-byte assertions.
TEST_F(BackendContractTest, CanonicalTypedBitRejectsInvalidBytesBeforeDispatchAndRecovers) {
  connect(); ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt,(SQLCHAR*)"rows ?",SQL_NTS));
  SQLULEN processed=99;SQLUSMALLINT status=SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAM_STATUS_PTR,&status,0));
  SQLCHAR input[]{0x5a,1,0xa5};SQLLEN length=1;
  const auto bind = [&] { return SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_BIT,
      SQL_BIT,1,0,&input[1],1,&length); };
  ASSERT_EQ(SQL_SUCCESS,bind());ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));
  ASSERT_EQ(1u,seen->parameters.size());EXPECT_EQ(std::optional<std::string>{"1"},seen->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  for (const SQLCHAR invalid : {SQLCHAR(2),SQLCHAR(255),SQLCHAR('1'),SQLCHAR('t'),SQLCHAR('T')}) {
    SCOPED_TRACE(static_cast<unsigned>(invalid)); input[1]=invalid;length=1;
    ASSERT_EQ(SQL_SUCCESS,bind());const auto queries=seen->queries;const auto deadline=seen->deadline;
    EXPECT_EQ(SQL_ERROR,SQLExecute(stmt));EXPECT_EQ("22003",state());
    EXPECT_EQ(queries,seen->queries);EXPECT_EQ(deadline,seen->deadline);
    EXPECT_EQ(SQL_PARAM_ERROR,status);EXPECT_EQ(1u,processed);
    EXPECT_EQ(invalid,input[1]);EXPECT_EQ(0x5a,input[0]);EXPECT_EQ(0xa5,input[2]);EXPECT_EQ(1,length);
    ASSERT_EQ(1u,seen->parameters.size());EXPECT_EQ(QueryParameterType::Boolean,seen->parameters[0].type);
    EXPECT_EQ(std::optional<std::string>{"1"},seen->parameters[0].value);
    // Close a baseline's wrongly successful cursor as well as a repaired error.
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
    input[1]=0;ASSERT_EQ(SQL_SUCCESS,bind());ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));
    ASSERT_EQ(1u,seen->parameters.size());EXPECT_EQ(std::optional<std::string>{"0"},seen->parameters[0].value);
    input[1]=1;EXPECT_EQ(std::optional<std::string>{"0"},seen->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_SUCCESS,status);ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
    ASSERT_EQ(SQL_SUCCESS,bind());ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));
    ASSERT_EQ(1u,seen->parameters.size());EXPECT_EQ(std::optional<std::string>{"1"},seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
  input[1]=255;length=SQL_NULL_DATA;ASSERT_EQ(SQL_SUCCESS,bind());ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));
  ASSERT_EQ(1u,seen->parameters.size());EXPECT_EQ(QueryParameterType::Boolean,seen->parameters[0].type);
  EXPECT_FALSE(seen->parameters[0].value);EXPECT_EQ(255,input[1]);EXPECT_EQ(SQL_NULL_DATA,length);
  EXPECT_EQ(SQL_PARAM_SUCCESS,status);EXPECT_EQ(1u,processed);EXPECT_EQ(0,seen->disconnects);
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, CanonicalTypedBitTargetsKeepFixedSizeNullAndBindingSemantics) {
  connect();ASSERT_EQ(SQL_SUCCESS,SQLPrepare(stmt,(SQLCHAR*)"rows ?",SQL_NTS));
  SQLULEN processed=99;SQLUSMALLINT status=SQL_PARAM_UNUSED;
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed,0));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_PARAM_STATUS_PTR,&status,0));
  SQLCHAR input[]{0x5a,0,0xa5};SQLLEN length=0;
  for (const SQLSMALLINT target : {SQLSMALLINT(SQL_BIT),SQLSMALLINT(SQL_VARCHAR),SQLSMALLINT(SQL_INTEGER),
                                  SQLSMALLINT(SQL_DOUBLE),SQLSMALLINT(SQL_NUMERIC),SQLSMALLINT(SQL_WVARCHAR)}) {
    SCOPED_TRACE(target);
    const auto hint=target==SQL_BIT?QueryParameterType::Boolean:
        (target==SQL_VARCHAR || target==SQL_WVARCHAR)?QueryParameterType::Text:
        target==SQL_DOUBLE?QueryParameterType::Float64:
        target==SQL_NUMERIC?QueryParameterType::Numeric:QueryParameterType::Int32;
    const auto bind = [&] { return SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_BIT,target,
        target==SQL_INTEGER?10:1,0,&input[1],length==0?0:1,&length); };
    for (const SQLCHAR valid : {SQLCHAR(0),SQLCHAR(1)}) {
      for (const SQLLEN fixed_length : {SQLLEN(0),SQLLEN(1),SQLLEN(99)}) {
        input[1]=valid;length=fixed_length;ASSERT_EQ(SQL_SUCCESS,bind());
        ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(1u,seen->parameters.size());
        EXPECT_EQ(hint,seen->parameters[0].type);EXPECT_FALSE(seen->parameters[0].binary_input);
        EXPECT_EQ(std::optional<std::string>{valid==0?"0":"1"},seen->parameters[0].value);
        input[1]=valid==0?1:0;EXPECT_EQ(std::optional<std::string>{valid==0?"0":"1"},seen->parameters[0].value);
        EXPECT_EQ(fixed_length,length);EXPECT_EQ(0x5a,input[0]);EXPECT_EQ(0xa5,input[2]);
        EXPECT_EQ(SQL_PARAM_SUCCESS,status);EXPECT_EQ(1u,processed);ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
      }
    }
    // Material must be checked before target-specific coercion for every selected SQL target.
    input[1]=2;length=1;ASSERT_EQ(SQL_SUCCESS,bind());
    const auto queries=seen->queries;const auto deadline=seen->deadline;
    EXPECT_EQ(SQL_ERROR,SQLExecute(stmt));EXPECT_EQ("22003",state());EXPECT_EQ(queries,seen->queries);EXPECT_EQ(deadline,seen->deadline);
    EXPECT_EQ(SQL_PARAM_ERROR,status);EXPECT_EQ(1u,processed);EXPECT_EQ(2,input[1]);EXPECT_EQ(1,length);
    ASSERT_EQ(1u,seen->parameters.size());EXPECT_EQ(std::optional<std::string>{"1"},seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
    input[1]=1;ASSERT_EQ(SQL_SUCCESS,bind());processed=99;status=SQL_PARAM_UNUSED;
    const auto before_bind=seen->queries;
    EXPECT_EQ(SQL_ERROR,SQLBindParameter(stmt,1,SQL_PARAM_INPUT,SQL_C_BIT,
        static_cast<SQLSMALLINT>(32767),1,0,&input[1],1,&length));EXPECT_EQ("HY004",state());
    EXPECT_EQ(before_bind,seen->queries);EXPECT_EQ(99u,processed);EXPECT_EQ(SQL_PARAM_UNUSED,status);
    EXPECT_EQ(0x5a,input[0]);EXPECT_EQ(1,input[1]);EXPECT_EQ(0xa5,input[2]);EXPECT_EQ(1,length);
    ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));ASSERT_EQ(1u,seen->parameters.size());
    EXPECT_EQ(hint,seen->parameters[0].type);EXPECT_EQ(std::optional<std::string>{"1"},seen->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
    input[1]=2;length=SQL_NULL_DATA;ASSERT_EQ(SQL_SUCCESS,bind());ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt));
    ASSERT_EQ(1u,seen->parameters.size());EXPECT_EQ(hint,seen->parameters[0].type);EXPECT_FALSE(seen->parameters[0].value);
    EXPECT_EQ(2,input[1]);EXPECT_EQ(SQL_NULL_DATA,length);EXPECT_EQ(SQL_PARAM_SUCCESS,status);
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
  }
  EXPECT_EQ(0,seen->disconnects);
}

TEST_F(BackendContractTest, NumericMetadataKeepsDigitsBitsBytesAndLegacySizesDistinct) {
  ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,dbc));dbc=nullptr;
  auto profile=std::make_shared<postgres::PgBackendProvider>(BackendIdentity{"redshift","Amazon Redshift","ODBCPP Redshift"},
      BackendConnectionDefaults{"localhost",5439,std::nullopt,true},std::nullopt,postgres::PgCatalogProfile::Redshift);
  auto connection=std::make_unique<rs::odbc::ODBCConnection>(nullptr,std::make_shared<FakeProvider>(seen,profile));
  dbc=reinterpret_cast<SQLHDBC>(connection.get());rs::odbc::HandleRegistry::instance().register_handle(dbc,std::move(connection),env);
  struct Expected { ScalarType family;SQLSMALLINT type;SQLULEN size;SQLSMALLINT scale,precision;SQLLEN octets,display,radix;const char* name; };
  const Expected cases[]{
      {ScalarType::SmallInt,SQL_SMALLINT,5,0,5,2,6,10,"smallint"},
      {ScalarType::Integer,SQL_INTEGER,10,0,10,4,11,10,"integer"},
      {ScalarType::BigInt,SQL_BIGINT,19,0,19,8,20,10,"bigint"},
      {ScalarType::Decimal,SQL_DECIMAL,5,2,5,7,7,10,"decimal"},
      {ScalarType::Numeric,SQL_NUMERIC,38,0,38,40,40,10,"numeric"},
      {ScalarType::Numeric,SQL_NUMERIC,0,0,0,SQL_NO_TOTAL,SQL_NO_TOTAL,10,"numeric"},
      {ScalarType::Real,SQL_REAL,7,6,24,4,14,2,"real"},
      {ScalarType::Double,SQL_DOUBLE,15,15,53,8,24,2,"double precision"}};
  QueryResult result;
  for (const auto& item:cases) { result.columns.push_back({item.name,NativeTypeInfo{item.family,item.size,item.scale,true}}); }
  result.rows.emplace_back(std::size(cases),std::nullopt);seen->date_result=result;connect();ASSERT_EQ(SQL_SUCCESS,execute("rows"));
  SQLHDESC ird=nullptr;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(stmt,SQL_ATTR_IMP_ROW_DESC,&ird,0,nullptr));ASSERT_NE(nullptr,ird);
  SQLSMALLINT count=0;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(stmt,&count));ASSERT_EQ(std::size(cases),static_cast<std::size_t>(count));
  for (std::size_t i=0;i<std::size(cases);++i) {
    SCOPED_TRACE(i);const auto& item=cases[i];const auto column=static_cast<SQLUSMALLINT>(i+1);
    SQLSMALLINT type=-1,scale=-1;SQLULEN size=99;
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(stmt,column,nullptr,0,nullptr,&type,&size,&scale,nullptr));
    EXPECT_EQ(item.type,type);EXPECT_EQ(item.size,size);
    // Decimal digits are observed forwarding; REAL/DOUBLE scale6/15 is not asserted normative.
    if (item.family!=ScalarType::Real && item.family!=ScalarType::Double) { EXPECT_EQ(item.scale,scale); }
    const std::pair<SQLUSMALLINT,SQLLEN> fields[]{
        {SQL_DESC_CONCISE_TYPE,item.type},{SQL_DESC_TYPE,item.type},
        {SQL_DESC_PRECISION,item.precision},{SQL_COLUMN_PRECISION,static_cast<SQLLEN>(item.size)},
        {SQL_DESC_OCTET_LENGTH,item.octets},{SQL_DESC_DISPLAY_SIZE,item.display},{SQL_DESC_NUM_PREC_RADIX,item.radix}};
    for (const auto& [field,expected]:fields) {
      SQLLEN value=99;ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(stmt,column,field,nullptr,0,nullptr,&value));EXPECT_EQ(expected,value);
    }
    SQLSMALLINT precision=-1;ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,column,SQL_DESC_PRECISION,&precision,0,nullptr));EXPECT_EQ(item.precision,precision);
    SQLLEN octets=-9;ASSERT_EQ(SQL_SUCCESS,SQLGetDescField(ird,column,SQL_DESC_OCTET_LENGTH,&octets,0,nullptr));EXPECT_EQ(item.octets,octets);
    char name[32]{};SQLSMALLINT length=-1;ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(stmt,column,SQL_DESC_TYPE_NAME,name,sizeof(name),&length,nullptr));
    ASSERT_EQ(static_cast<SQLSMALLINT>(std::strlen(item.name)),length);EXPECT_EQ(0,std::memcmp(name,item.name,static_cast<std::size_t>(length)+1));
  }
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, OwningTypeMetadataKeepsWideNameUnitsAndAttributeBytesSeparate) {
  ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,dbc));dbc=nullptr;
  auto profile=std::make_shared<postgres::PgBackendProvider>(BackendIdentity{"redshift","Amazon Redshift","ODBCPP Redshift"},
      BackendConnectionDefaults{"localhost",5439,std::nullopt,true},std::nullopt,postgres::PgCatalogProfile::Redshift);
  auto connection=std::make_unique<rs::odbc::ODBCConnection>(nullptr,std::make_shared<FakeProvider>(seen,profile));
  dbc=reinterpret_cast<SQLHDBC>(connection.get());rs::odbc::HandleRegistry::instance().register_handle(dbc,std::move(connection),env);
  const std::string utf8="A\xe2\x82\xac\xf0\x9f\x98\x80";
  QueryResult result;result.columns={{utf8,NativeTypeInfo{ScalarType::VarChar,4,0,true}}};result.rows={{"ok"}};
  seen->date_result=result;connect();ASSERT_EQ(SQL_SUCCESS,execute("rows"));
  seen->date_result->columns[0].name="poison";seen->date_result->columns[0].normalized_type=NativeTypeInfo{ScalarType::Binary,999,0,true};
  SQLSMALLINT type=-1,units=-1;SQLULEN size=99;SQLWCHAR name[8];std::fill(std::begin(name),std::end(name),SQLWCHAR('x'));
  std::vector<SQLWCHAR> literal;
  if constexpr(sizeof(SQLWCHAR)==2) { literal={'A',0x20ac,0xd83d,0xde00,0}; }
  else { literal={'A',0x20ac,static_cast<SQLWCHAR>(0x1f600),0}; }
  const auto expected_units=static_cast<SQLSMALLINT>(literal.size()-1);
  ASSERT_EQ(SQL_SUCCESS,SQLDescribeColW(stmt,1,name,8,&units,&type,&size,nullptr,nullptr));
  EXPECT_EQ(expected_units,units);EXPECT_EQ(SQL_VARCHAR,type);EXPECT_EQ(4u,size);
  EXPECT_EQ(0,std::memcmp(name,literal.data(),literal.size()*sizeof(SQLWCHAR)));
  std::fill(std::begin(name),std::end(name),SQLWCHAR('x'));
  const SQLSMALLINT truncated_units=sizeof(SQLWCHAR)==2?4:3;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLDescribeColW(stmt,1,name,truncated_units,&units,&type,&size,nullptr,nullptr));
  EXPECT_EQ("01004",state());EXPECT_EQ(expected_units,units);
  const SQLWCHAR prefix[]{'A',0x20ac,0};EXPECT_EQ(0,std::memcmp(name,prefix,sizeof(prefix)));EXPECT_EQ('x',name[3]);
  SQLHDESC ird=nullptr;ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(stmt,SQL_ATTR_IMP_ROW_DESC,&ird,0,nullptr));ASSERT_NE(nullptr,ird);
  SQLSMALLINT subtype=-1,precision=-1,scale=-1,nullable=-1;SQLLEN descriptor_length=-1;
  std::fill(std::begin(name),std::end(name),SQLWCHAR('x'));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLGetDescRecW(ird,1,name,truncated_units,&units,&type,&subtype,&descriptor_length,&precision,&scale,&nullable));
  EXPECT_EQ("01004",state(SQL_HANDLE_DESC,ird));EXPECT_EQ(expected_units,units);EXPECT_EQ(SQL_VARCHAR,type);EXPECT_EQ(0,subtype);EXPECT_EQ(4,descriptor_length);EXPECT_EQ(0,precision);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
  EXPECT_EQ(0,std::memcmp(name,prefix,sizeof(prefix)));EXPECT_EQ('x',name[3]);
  std::fill(std::begin(name),std::end(name),SQLWCHAR('x'));
  ASSERT_EQ(SQL_SUCCESS,SQLGetDescRecW(ird,1,name,8,&units,&type,&subtype,&descriptor_length,&precision,&scale,&nullable));
  EXPECT_EQ(expected_units,units);EXPECT_EQ(0,std::memcmp(name,literal.data(),literal.size()*sizeof(SQLWCHAR)));EXPECT_EQ('x',name[literal.size()]);
  // DescribeColW capacity/result lengths count units; ColAttributeW uses bytes.
  SQLSMALLINT bytes=-1;std::fill(std::begin(name),std::end(name),SQLWCHAR('x'));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLColAttributeW(stmt,1,SQL_DESC_NAME,name,2*sizeof(SQLWCHAR),&bytes,nullptr));
  EXPECT_EQ("01004",state());EXPECT_EQ(expected_units*sizeof(SQLWCHAR),bytes);EXPECT_EQ('A',name[0]);EXPECT_EQ(0,name[1]);EXPECT_EQ('x',name[2]);
  std::fill(std::begin(name),std::end(name),SQLWCHAR('x'));bytes=73;
  EXPECT_EQ(SQL_ERROR,SQLColAttributeW(stmt,1,SQL_DESC_NAME,name,2*sizeof(SQLWCHAR)-1,&bytes,nullptr));
  EXPECT_EQ("HY090",state());EXPECT_EQ(73,bytes);EXPECT_TRUE(std::all_of(std::begin(name),std::end(name),[](auto v){return v=='x';}));
  ASSERT_EQ(SQL_SUCCESS,SQLColAttributeW(stmt,1,SQL_DESC_NAME,name,sizeof(name),&bytes,nullptr));
  EXPECT_EQ(expected_units*sizeof(SQLWCHAR),bytes);EXPECT_EQ(0,std::memcmp(name,literal.data(),literal.size()*sizeof(SQLWCHAR)));
  SQLWCHAR type_name[16]{};ASSERT_EQ(SQL_SUCCESS,SQLColAttributeW(stmt,1,SQL_DESC_TYPE_NAME,type_name,sizeof(type_name),&bytes,nullptr));
  const SQLWCHAR varchar_name[]{'v','a','r','c','h','a','r',0};ASSERT_EQ(7*sizeof(SQLWCHAR),bytes);EXPECT_EQ(0,std::memcmp(type_name,varchar_name,sizeof(varchar_name)));
  const std::pair<SQLUSMALLINT,SQLLEN> fields[]{{SQL_DESC_LENGTH,4},{SQL_DESC_OCTET_LENGTH,4},{SQL_DESC_DISPLAY_SIZE,4},{SQL_DESC_CONCISE_TYPE,SQL_VARCHAR}};
  for(const auto& [field,expected]:fields) {
    SQLLEN ansi=-1,wide=-2;ASSERT_EQ(SQL_SUCCESS,SQLColAttribute(stmt,1,field,nullptr,0,nullptr,&ansi));
    ASSERT_EQ(SQL_SUCCESS,SQLColAttributeW(stmt,1,field,nullptr,0,nullptr,&wide));EXPECT_EQ(expected,ansi);EXPECT_EQ(ansi,wide);
  }
  // Normalized size4 is adapter plumbing, not native Unicode storage qualification.
  ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(stmt));
}

TEST_F(BackendContractTest, WideNativeSqlAndDiagnosticsPreserveScalarBoundariesAndRequiredUnits) {
  connect();const auto queries=seen->queries;
  std::vector<SQLWCHAR> smile;
  if constexpr(sizeof(SQLWCHAR)==2) { smile={0xd83d,0xde00,0}; }
  else { smile={static_cast<SQLWCHAR>(0x1f600),0}; }
  const SQLWCHAR native_prefix[]{'n','a','t','i','v','e',':',0};
  const SQLINTEGER full_units=static_cast<SQLINTEGER>(7+smile.size()-1);
  SQLWCHAR output[32];std::fill(std::begin(output),std::end(output),SQLWCHAR('x'));SQLINTEGER length=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLNativeSqlW(dbc,smile.data(),SQL_NTS,nullptr,0,&length));EXPECT_EQ(full_units,length);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLNativeSqlW(dbc,smile.data(),SQL_NTS,output,0,&length));
  EXPECT_EQ("01004",state(SQL_HANDLE_DBC,dbc));EXPECT_EQ(full_units,length);EXPECT_EQ('x',output[0]);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLNativeSqlW(dbc,smile.data(),SQL_NTS,output,1,&length));
  EXPECT_EQ("01004",state(SQL_HANDLE_DBC,dbc));EXPECT_EQ(0,output[0]);EXPECT_EQ('x',output[1]);EXPECT_EQ(full_units,length);
  std::fill(std::begin(output),std::end(output),SQLWCHAR('x'));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLNativeSqlW(dbc,smile.data(),SQL_NTS,output,full_units,&length));
  EXPECT_EQ("01004",state(SQL_HANDLE_DBC,dbc));EXPECT_EQ(full_units,length);
  EXPECT_EQ(0,std::memcmp(output,native_prefix,sizeof(native_prefix)));EXPECT_EQ('x',output[8]);
  std::vector<SQLWCHAR> full(native_prefix,native_prefix+7);full.insert(full.end(),smile.begin(),smile.end());
  ASSERT_EQ(SQL_SUCCESS,SQLNativeSqlW(dbc,smile.data(),SQL_NTS,output,full_units+1,&length));
  EXPECT_EQ(full_units,length);EXPECT_EQ(0,std::memcmp(output,full.data(),full.size()*sizeof(SQLWCHAR)));
  std::fill(std::begin(output),std::end(output),SQLWCHAR('x'));length=73;
  EXPECT_EQ(SQL_ERROR,SQLNativeSqlW(dbc,smile.data(),SQL_NTS,output,-1,&length));EXPECT_EQ("HY090",state(SQL_HANDLE_DBC,dbc));
  EXPECT_EQ(73,length);EXPECT_TRUE(std::all_of(std::begin(output),std::end(output),[](auto v){return v=='x';}));
  SQLWCHAR bmp[]{'X',0};const SQLWCHAR bmp_expected[]{'n','a','t','i','v','e',':','X',0};
  ASSERT_EQ(SQL_SUCCESS,SQLNativeSqlW(dbc,bmp,SQL_NTS,output,9,&length));EXPECT_EQ(8,length);EXPECT_EQ(0,std::memcmp(output,bmp_expected,sizeof(bmp_expected)));
  SQLWCHAR empty[]{0};ASSERT_EQ(SQL_SUCCESS,SQLNativeSqlW(dbc,empty,0,output,8,&length));EXPECT_EQ(7,length);EXPECT_EQ(0,std::memcmp(output,native_prefix,sizeof(native_prefix)));
  EXPECT_EQ(queries,seen->queries);

  const auto connection=rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(dbc);ASSERT_NE(nullptr,connection);
  const std::string message="A\xe2\x82\xac\xf0\x9f\x98\x80";connection->clear_diagnostics();connection->add_diagnostic("HY000",91,message);
  const SQLSMALLINT required=sizeof(SQLWCHAR)==2?4:3;SQLSMALLINT text_length=-1;SQLWCHAR sqlstate[6]{};SQLINTEGER native_error=-1;
  ASSERT_EQ(SQL_SUCCESS,SQLGetDiagRecW(SQL_HANDLE_DBC,dbc,1,sqlstate,&native_error,nullptr,0,&text_length));
  EXPECT_EQ(required,text_length);EXPECT_EQ(91,native_error);const SQLWCHAR expected_state[]{'H','Y','0','0','0',0};EXPECT_EQ(0,std::memcmp(sqlstate,expected_state,sizeof(expected_state)));
  for(const SQLSMALLINT capacity:{SQLSMALLINT{0},SQLSMALLINT{1},required}) {
    std::fill(std::begin(output),std::end(output),SQLWCHAR('x'));
    const auto expected_result=capacity==0?SQL_SUCCESS:SQL_SUCCESS_WITH_INFO;
    ASSERT_EQ(expected_result,SQLGetDiagRecW(SQL_HANDLE_DBC,dbc,1,sqlstate,&native_error,output,capacity,&text_length));
    EXPECT_EQ(required,text_length);EXPECT_EQ(91,native_error);EXPECT_EQ(1u,connection->get_diagnostic_count());
    const auto record=connection->get_diagnostic_record(1);ASSERT_TRUE(record);EXPECT_EQ("HY000",record->sqlstate);EXPECT_EQ(message,record->message_text);
    if(capacity==0) { EXPECT_EQ('x',output[0]); }
    else if(capacity==1) { EXPECT_EQ(0,output[0]);EXPECT_EQ('x',output[1]); }
    else { const SQLWCHAR prefix[]{'A',0x20ac,0};EXPECT_EQ(0,std::memcmp(output,prefix,sizeof(prefix)));EXPECT_EQ('x',output[3]); }
  }
  std::vector<SQLWCHAR> expected_message;
  if constexpr(sizeof(SQLWCHAR)==2) { expected_message={'A',0x20ac,0xd83d,0xde00,0}; }
  else { expected_message={'A',0x20ac,static_cast<SQLWCHAR>(0x1f600),0}; }
  ASSERT_EQ(SQL_SUCCESS,SQLGetDiagRecW(SQL_HANDLE_DBC,dbc,1,sqlstate,&native_error,output,required+1,&text_length));
  EXPECT_EQ(required,text_length);EXPECT_EQ(0,std::memcmp(output,expected_message.data(),expected_message.size()*sizeof(SQLWCHAR)));
  std::fill(std::begin(output),std::end(output),SQLWCHAR('x'));text_length=73;
  EXPECT_EQ(SQL_ERROR,SQLGetDiagRecW(SQL_HANDLE_DBC,dbc,1,nullptr,nullptr,output,-1,&text_length));
  EXPECT_EQ(73,text_length);EXPECT_EQ('x',output[0]);EXPECT_EQ(1u,connection->get_diagnostic_count());
  connection->clear_diagnostics();connection->add_diagnostic("HY000",91,"\xf0\x9f\x98\x80");
  std::fill(std::begin(output),std::end(output),SQLWCHAR('x'));
  const SQLSMALLINT first_capacity=sizeof(SQLWCHAR)==2?2:1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLGetDiagRecW(SQL_HANDLE_DBC,dbc,1,nullptr,nullptr,output,first_capacity,&text_length));
  EXPECT_EQ(static_cast<SQLSMALLINT>(smile.size()-1),text_length);EXPECT_EQ(0,output[0]);EXPECT_EQ('x',output[1]);
}

TEST_F(BackendContractTest, WideDriverConnectScalarTruncationPreservesConnectionAndKeywordDiagnostics) {
  const std::string ascii="SSL=0;DESCRIPTION=";std::vector<SQLWCHAR> input(ascii.begin(),ascii.end());
  const auto prefix_units=input.size();input.push_back('A');input.push_back(0x20ac);
  if constexpr(sizeof(SQLWCHAR)==2) { input.push_back(0xd83d);input.push_back(0xde00); }
  else { input.push_back(static_cast<SQLWCHAR>(0x1f600)); }
  const std::string suffix=";BOGUS=1";input.insert(input.end(),suffix.begin(),suffix.end());input.push_back(0);
  std::vector<SQLWCHAR> output(input.size()+3,SQLWCHAR('x'));SQLSMALLINT length=-1;
  const SQLSMALLINT capacity=static_cast<SQLSMALLINT>(prefix_units+(sizeof(SQLWCHAR)==2?4:3));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLDriverConnectW(dbc,nullptr,input.data(),SQL_NTS,output.data(),capacity,&length,SQL_DRIVER_NOPROMPT));
  EXPECT_EQ(static_cast<SQLSMALLINT>(input.size()-1),length);EXPECT_EQ(1,seen->created);EXPECT_EQ("01004",state(SQL_HANDLE_DBC,dbc));
  EXPECT_EQ(0,std::memcmp(output.data(),input.data(),(prefix_units+2)*sizeof(SQLWCHAR)));
  EXPECT_EQ(0,output[prefix_units+2]);EXPECT_EQ('x',output[prefix_units+3]);
  SQLCHAR second_state[6]{};ASSERT_EQ(SQL_SUCCESS,SQLGetDiagRec(SQL_HANDLE_DBC,dbc,2,second_state,nullptr,nullptr,0,nullptr));EXPECT_STREQ("01S00",reinterpret_cast<char*>(second_state));
  ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,dbc,&stmt));ASSERT_EQ(SQL_SUCCESS,execute("rows"));ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,stmt));stmt=nullptr;
  ASSERT_EQ(SQL_SUCCESS,SQLDisconnect(dbc));std::fill(output.begin(),output.end(),SQLWCHAR('x'));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,SQLDriverConnectW(dbc,nullptr,input.data(),SQL_NTS,output.data(),static_cast<SQLSMALLINT>(input.size()),&length,SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("01S00",state(SQL_HANDLE_DBC,dbc));EXPECT_EQ(2,seen->created);EXPECT_EQ(static_cast<SQLSMALLINT>(input.size()-1),length);
  EXPECT_EQ(0,std::memcmp(output.data(),input.data(),input.size()*sizeof(SQLWCHAR)));EXPECT_EQ('x',output[input.size()]);
}
