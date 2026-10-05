#include "core/database/postgres/pg_database_connection.h"
#include <gtest/gtest.h>
#include <array>
#include <algorithm>

using namespace rs::core::database;
using namespace rs::core::database::postgres;

namespace {
PrimaryKeysCatalogRequest request() { return {"db'._%", "schema'._%", "table'._%"}; }
QueryResult primary_keys() {
  QueryResult result;
  for (const auto* name : {"database_name", "schema_name", "table_name", "pk_name", "column_name", "key_seq"})
    result.columns.push_back({name, NativeTypeInfo{std::string(name) == "key_seq" ? ScalarType::SmallInt : ScalarType::VarChar, 0, 0, true}});
  result.rows = {{"db'._%", "schema'._%", "table'._%", "pk", "key_b", "1"},
                 {"db'._%", "schema'._%", "table'._%", "pk", "key_a", "2"}};
  return result;
}
class SpySession : public PgDatabaseConnection {
 public:
  explicit SpySession(PgCatalogProfile profile = PgCatalogProfile::Redshift)
      : PgDatabaseConnection(nullptr, std::nullopt, profile) {}
  std::optional<RedshiftCatalogMode> mode;
  RedshiftCatalogMode catalog_mode() const noexcept override { return mode.value_or(PgDatabaseConnection::catalog_mode()); }
  bool connected{true};
  SessionSnapshot snapshot{SessionState::Idle, SessionDisposition::Reusable};
  std::string capability{"4"};
  QueryResult response{primary_keys()};
  std::optional<BackendError> failure;
  mutable unsigned capability_reads{0};
  mutable unsigned catalog_builds{0};
  unsigned prepared_calls{0}, direct_calls{0};
  std::string sql;
  std::vector<QueryParameter> parameters;
  rs::util::Deadline received_deadline;
  bool is_connected() const override { return connected; }
  SessionState session_state() const noexcept override { return connected ? snapshot.state : SessionState::Disconnected; }
  std::string get_parameter(std::string_view key) const override {
    ++capability_reads;
    EXPECT_EQ("show_discovery", key);
    return capability;
  }
  BackendResult<QueryResult> execute_prepared(std::string_view query,
      std::span<const QueryParameter> values, rs::util::Deadline deadline) override {
    ++prepared_calls; sql = query; parameters.assign(values.begin(), values.end());
    received_deadline = deadline;
    if (failure) return *failure;
    return BackendResult<QueryResult>{response, snapshot};
  }
  BackendResult<QueryResult> execute_query(std::string_view, rs::util::Deadline) override {
    ++direct_calls;
    return {rs::util::DbErrorCode::QueryFailed, "Unexpected direct SQL fallback"};
  }
  rs::util::Result<std::string> catalog_query(const CatalogRequest&) const override {
    ++catalog_builds;
    return {rs::util::DbErrorCode::UnsupportedFeature, "Unexpected inherited catalog builder"};
  }
};
void no_execution(const SpySession& spy) {
  EXPECT_EQ(0u, spy.prepared_calls); EXPECT_EQ(0u, spy.direct_calls); EXPECT_EQ(0u, spy.catalog_builds);
}

// Private, in-memory protocol fixture: each connection receives authenticated
// startup followed by a successful empty, known-type prepared result. No mode,
// query, parameter, or type-resolution method is overridden on the real session.
class CatalogWireTransport final : public rs::core::transport::ITransport {
 public:
  std::vector<std::vector<std::byte>> writes;
  std::vector<rs::util::Deadline> write_deadlines, read_deadlines;
  unsigned connects{0}, closes{0};
  rs::util::Result<void> connect(std::string_view, uint16_t,
      rs::util::Deadline) override {
    ++connects; input_.clear(); offset_ = 0;
    frame('R', {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    std::vector<std::byte> status;
    text(status, "show_discovery"); text(status, "4"); frame('S', status);
    frame('Z', {std::byte{'I'}});
    frame('1', {});
    std::vector<std::byte> parameters; integer(parameters, 3, 2);
    for (unsigned i = 0; i < 3; ++i) integer(parameters, 25, 4);
    frame('t', parameters); frame('2', {});
    std::vector<std::byte> description; integer(description, 6, 2);
    for (const auto* name : {"database_name", "schema_name", "table_name",
                            "pk_name", "column_name", "key_seq"}) {
      text(description, name); integer(description, 0, 4); integer(description, 0, 2);
      const bool sequence = std::string_view(name) == "key_seq";
      integer(description, sequence ? 21 : 1043, 4);
      integer(description, sequence ? 2 : 65535, 2);
      integer(description, 0xffffffffu, 4); integer(description, 0, 2);
    }
    frame('T', description);
    std::vector<std::byte> completion; text(completion, "SELECT 0"); frame('C', completion);
    frame('Z', {std::byte{'I'}});
    return {};
  }
  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> bytes, rs::util::Deadline deadline) override {
    writes.emplace_back(bytes.begin(), bytes.end()); write_deadlines.push_back(deadline);
    return rs::core::transport::IOResult{bytes.size(), false};
  }
  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> bytes, rs::util::Deadline deadline) override {
    read_deadlines.push_back(deadline);
    const auto count = std::min(bytes.size(), input_.size() - offset_);
    std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(offset_), count, bytes.begin());
    offset_ += count;
    return rs::core::transport::IOResult{count, count == 0};
  }
  void close() noexcept override { ++closes; }
 private:
  static void integer(std::vector<std::byte>& out, uint32_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
      out.push_back(static_cast<std::byte>((value >> ((i - 1) * 8)) & 255u));
  }
  static void text(std::vector<std::byte>& out, std::string_view value) {
    for (const auto byte : value) out.push_back(static_cast<std::byte>(byte));
    out.push_back(std::byte{0});
  }
  void frame(char tag, const std::vector<std::byte>& body) {
    input_.push_back(static_cast<std::byte>(tag));
    integer(input_, static_cast<uint32_t>(body.size() + 4), 4);
    input_.insert(input_.end(), body.begin(), body.end());
  }
  std::vector<std::byte> input_;
  std::size_t offset_{0};
};

// Read the first unnamed Parse from a combined prepared exchange. Inspecting
// actual transport bytes also catches an accidental direct-query fallback.
void expect_parse(const std::vector<std::byte>& packet, bool legacy) {
  ASSERT_GE(packet.size(), 8u); ASSERT_EQ(std::byte{'P'}, packet[0]);
  ASSERT_EQ(std::byte{0}, packet[5]);
  const auto end = std::find(packet.begin() + 6, packet.end(), std::byte{0});
  ASSERT_NE(packet.end(), end);
  std::string sql;
  for (auto it = packet.begin() + 6; it != end; ++it) sql.push_back(static_cast<char>(*it));
  if (legacy) {
    EXPECT_NE(std::string::npos, sql.find("a.attrelid=ci.oid"));
    EXPECT_NE(std::string::npos, sql.find("current_database()=$1"));
    EXPECT_NE(std::string::npos, sql.find("n.nspname=$2"));
    EXPECT_NE(std::string::npos, sql.find("ct.relname=$3"));
    EXPECT_EQ(std::string::npos, sql.find("SHOW"));
  } else {
    EXPECT_EQ("SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE $1.$2.$3;", sql);
  }
  const auto position = static_cast<std::size_t>(end - packet.begin()) + 1;
  ASSERT_GE(packet.size() - position, 14u);
  EXPECT_EQ(std::byte{0}, packet[position]); EXPECT_EQ(std::byte{3}, packet[position + 1]);
  for (std::size_t i = 0; i < 3; ++i) {
    const auto oid = position + 2 + i * 4;
    EXPECT_EQ(std::byte{0}, packet[oid]); EXPECT_EQ(std::byte{0}, packet[oid + 1]);
    EXPECT_EQ(std::byte{0}, packet[oid + 2]);
    EXPECT_EQ(static_cast<std::byte>(legacy ? 25 : 0), packet[oid + 3]);
  }
}
}  // namespace

TEST(RedshiftCatalogExecution, FacetIsExplicitProfileOnly) {
  SpySession redshift, postgres(PgCatalogProfile::PostgreSQL);
  EXPECT_NE(nullptr, redshift.catalog_execution());
  EXPECT_EQ(nullptr, postgres.catalog_execution());
  auto result = postgres.execute_catalog(request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_FALSE(result);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature), result.error());
  no_execution(postgres);
}

TEST(RedshiftCatalogExecution, DisconnectedReturnsNotConnectedWithoutCapabilityOrSql) {
  SpySession spy; spy.connected = false;
  auto result = spy.execute_catalog(request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_FALSE(result);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::NotConnected), result.error());
  EXPECT_EQ(SessionState::Disconnected, result.session_snapshot().state);
  EXPECT_EQ(0u, spy.capability_reads); no_execution(spy);
}

TEST(RedshiftCatalogExecution, CapabilityRequiresEntireBoundedModernUnsignedValue) {
  for (const auto& value : {std::string{}, std::string{"0"}, std::string{"3"}, std::string{"-4"}, std::string{"+4"},
      std::string{" 4"}, std::string{"4 "}, std::string{"4suffix"}, std::string{"4.0"}, std::string{"4294967296"},
      std::string{"00000000004"}, std::string("4\0suffix", 8)}) {
    SCOPED_TRACE(value.size());
    SpySession spy; spy.capability = value;
    auto result = spy.execute_catalog(request(), rs::util::make_deadline(std::chrono::seconds{1}));
    ASSERT_FALSE(result); no_execution(spy);
  }
  for (const auto* value : {"4", "5", "0004", "4294967295"}) {
    SpySession spy; spy.capability = value;
    EXPECT_TRUE(spy.execute_catalog(request(), rs::util::make_deadline(std::chrono::seconds{1})));
    EXPECT_EQ(1u, spy.prepared_calls);
  }
}

TEST(RedshiftCatalogExecution, IncompleteAndMalformedExactNamesPerformNoSql) {
  for (const auto& bad : {std::optional<std::string>{}, std::optional<std::string>{""},
      std::optional<std::string>{std::string("x\0secret", 8)}, std::optional<std::string>{std::string("\xff", 1)}}) {
    for (int field = 0; field < 3; ++field) {
      SpySession spy; auto input = request();
      if (field == 0) input.catalog = bad;
      if (field == 1) input.schema = bad;
      if (field == 2) input.table = bad.value_or("");
      EXPECT_FALSE(spy.execute_catalog(input, rs::util::make_deadline(std::chrono::seconds{1})));
      no_execution(spy);
    }
  }
}

TEST(RedshiftCatalogExecution, OtherCatalogKindsAreUnsupportedWithoutIo) {
  const std::vector<CatalogRequest> unsupported{TablesCatalogRequest{}, ColumnsCatalogRequest{},
      ForeignKeysCatalogRequest{}, StatisticsCatalogRequest{}, ProceduresCatalogRequest{},
      ProcedureColumnsCatalogRequest{}, SpecialColumnsCatalogRequest{}};
  for (const auto& input : unsupported) {
    SpySession spy;
    auto result = spy.execute_catalog(input, rs::util::make_deadline(std::chrono::seconds{1}));
    ASSERT_FALSE(result);
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature), result.error());
    no_execution(spy);
  }
}

TEST(RedshiftCatalogExecution, OneFixedShowPreservesDeadlineValuesSnapshotAndOwnership) {
  SpySession spy;
  spy.snapshot = {SessionState::Transaction, SessionDisposition::ResetRequired};
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{7});
  auto input = request();
  auto result = spy.execute_catalog(input, deadline);
  ASSERT_TRUE(result);
  EXPECT_EQ(1u, spy.prepared_calls); EXPECT_EQ(0u, spy.direct_calls); EXPECT_EQ(0u, spy.catalog_builds);
  EXPECT_EQ("SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;", spy.sql);
  EXPECT_EQ(deadline, spy.received_deadline);
  ASSERT_EQ(3u, spy.parameters.size());
  EXPECT_EQ(input.catalog, spy.parameters[0].value); EXPECT_EQ(input.schema, spy.parameters[1].value);
  EXPECT_EQ(input.table, spy.parameters[2].value);
  for (const auto& parameter : spy.parameters) {
    EXPECT_EQ(QueryParameterType::Unspecified, parameter.type); EXPECT_FALSE(parameter.binary_input);
  }
  EXPECT_EQ(spy.snapshot, result.session_snapshot());
  spy.response.rows.clear(); spy.response.columns.clear(); input.table.clear(); spy.connected = false;
  ASSERT_EQ(2u, result->rows.size()); ASSERT_EQ(6u, result->columns.size());
  EXPECT_EQ((ResultRow{"db'._%", "schema'._%", "table'._%", "key_b", "1", "pk"}), result->rows[0]);
  EXPECT_EQ("key_a", result->rows[1][3]);
  constexpr std::array<const char*, 6> names{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(names[i], result->columns[i].name); ASSERT_TRUE(result->columns[i].normalized_type);
    EXPECT_TRUE(result->columns[i].normalized_type->known);
    EXPECT_EQ(i == 4 ? ScalarType::SmallInt : ScalarType::VarChar, result->columns[i].normalized_type->type);
  }
}

TEST(RedshiftCatalogExecution, NativeFailureIsPreservedWithoutFallbackOrRetry) {
  SpySession spy;
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owned private diagnostic"};
  error.native_state = "42501"; error.native_code = 19; error.operation = BackendOperation::ExecutePrepared;
  error.session_state = SessionState::FailedTransaction; error.disposition = SessionDisposition::ResetRequired;
  error.retry_safe = false; spy.failure = error;
  auto result = spy.execute_catalog(request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_FALSE(result); spy.failure.reset();
  EXPECT_EQ(error.message, result.error_message()); EXPECT_EQ(error.code, result.error());
  EXPECT_EQ(error.native_state, result.backend_error().native_state); EXPECT_EQ(error.native_code, result.backend_error().native_code);
  EXPECT_EQ(error.operation, result.backend_error().operation); EXPECT_EQ(error.retry_safe, result.backend_error().retry_safe);
  EXPECT_EQ((SessionSnapshot{error.session_state, error.disposition}), result.session_snapshot());
  EXPECT_EQ(1u, spy.prepared_calls); EXPECT_EQ(0u, spy.direct_calls); EXPECT_EQ(0u, spy.catalog_builds);
}

TEST(RedshiftCatalogExecution, MalformedPartialResultsBlockWithoutFallback) {
  for (int shape = 0; shape < 4; ++shape) {
    SpySession spy;
    if (shape == 0) spy.response.rows.back()[5] = "1";
    if (shape == 1) spy.response.columns.back().normalized_type.reset();
    if (shape == 2) spy.response.cell_errors.push_back({1, 4});
    if (shape == 3) spy.response.additional_results.push_back(QueryResult{});
    auto result = spy.execute_catalog(request(), rs::util::make_deadline(std::chrono::seconds{1}));
    ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
    EXPECT_EQ(1u, spy.prepared_calls); EXPECT_EQ(0u, spy.direct_calls); EXPECT_EQ(0u, spy.catalog_builds);
  }
}

TEST(RedshiftCatalogExecution, LegacyUsesExactParametersOriginalDeadlineWithoutCapabilityOrFallback) {
  SpySession spy; spy.mode = RedshiftCatalogMode::Legacy; spy.capability = "malformed";
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{1});
  auto result = spy.execute_catalog(request(), deadline);
  ASSERT_TRUE(result);
  EXPECT_EQ(0u, spy.capability_reads); EXPECT_EQ(1u, spy.prepared_calls);
  EXPECT_EQ(0u, spy.direct_calls); EXPECT_EQ(0u, spy.catalog_builds);
  EXPECT_EQ(deadline, spy.received_deadline);
  ASSERT_EQ(3u, spy.parameters.size());
  EXPECT_EQ("db'._%", spy.parameters[0].value); EXPECT_EQ("schema'._%", spy.parameters[1].value);
  EXPECT_EQ("table'._%", spy.parameters[2].value);
  for (const auto& value : spy.parameters) EXPECT_EQ(QueryParameterType::Text, value.type);
  EXPECT_NE(std::string::npos, spy.sql.find("a.attrelid=ci.oid"));
  EXPECT_NE(std::string::npos, spy.sql.find("a.attnum AS key_seq"));
  EXPECT_NE(std::string::npos, spy.sql.find("current_database()=?"));
  EXPECT_EQ(std::string::npos, spy.sql.find("SHOW"));
  EXPECT_EQ(std::string::npos, spy.sql.find("information_schema"));
  EXPECT_EQ(std::string::npos, spy.sql.find("table'._%"));
  spy.failure = local_backend_error(LocalFailure::Unsupported, "native failure",
      BackendOperation::ExecuteCatalog, SessionState::Idle);
  auto failed = spy.execute_catalog(request(), deadline);
  ASSERT_FALSE(failed); EXPECT_EQ(spy.failure->code, failed.error());
  EXPECT_EQ(2u, spy.prepared_calls); EXPECT_EQ(0u, spy.capability_reads);
}

TEST(RedshiftCatalogExecution, RejectedReconnectCannotChangeActiveMode) {
  SpySession spy;
  ConnectionSettings settings; settings.redshift_catalog_mode = RedshiftCatalogMode::Legacy;
  auto connected = spy.connect(settings);
  ASSERT_FALSE(connected);
  auto result = spy.execute_catalog(request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_TRUE(result); EXPECT_EQ(1u, spy.capability_reads);
  EXPECT_EQ("SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;", spy.sql);
}

TEST(RedshiftCatalogExecution, LegacyInvalidIdentifiersRejectBeforeSqlAndKeepShowSessionIndependent) {
  SpySession legacy, show; legacy.mode = RedshiftCatalogMode::Legacy;
  auto invalid = request(); invalid.table = std::string("bad\0name", 8);
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{1});
  ASSERT_FALSE(legacy.execute_catalog(invalid, deadline));
  no_execution(legacy); EXPECT_EQ(0u, legacy.capability_reads);
  ASSERT_TRUE(show.execute_catalog(request(), deadline));
  EXPECT_EQ("SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;", show.sql);
}

TEST(RedshiftCatalogExecution, RealSessionLegacyConnectDisconnectThenDefaultShowReconnect) {
  auto transport = std::make_unique<CatalogWireTransport>();
  auto* wire = transport.get();
  PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
  ConnectionSettings settings;
  settings.host = "in-memory.invalid"; settings.port = 5439;
  settings.database = "db'._%"; settings.user = "fixture"; settings.use_ssl = false;
  settings.redshift_catalog_mode = RedshiftCatalogMode::Legacy;
  ASSERT_TRUE(session.connect(settings)); ASSERT_TRUE(session.is_connected());
  const auto legacy_deadline = rs::util::make_deadline(std::chrono::seconds{3});
  const auto legacy_reads = wire->read_deadlines.size();
  auto legacy = session.execute_catalog(request(), legacy_deadline);
  ASSERT_TRUE(legacy); ASSERT_EQ(6u, legacy->columns.size()); EXPECT_TRUE(legacy->rows.empty());
  EXPECT_EQ(SessionState::Idle, legacy.session_snapshot().state);
  ASSERT_EQ(2u, wire->writes.size()); expect_parse(wire->writes[1], true);
  EXPECT_EQ(legacy_deadline, wire->write_deadlines[1]);
  for (auto i = legacy_reads; i < wire->read_deadlines.size(); ++i)
    EXPECT_EQ(legacy_deadline, wire->read_deadlines[i]);
  session.disconnect(); EXPECT_FALSE(session.is_connected()); EXPECT_EQ(1u, wire->closes);

  settings.redshift_catalog_mode.reset();
  ASSERT_TRUE(session.connect(settings)); ASSERT_TRUE(session.is_connected());
  const auto show_deadline = rs::util::make_deadline(std::chrono::seconds{3});
  const auto show_reads = wire->read_deadlines.size();
  auto show = session.execute_catalog(request(), show_deadline);
  ASSERT_TRUE(show); ASSERT_EQ(6u, show->columns.size()); EXPECT_TRUE(show->rows.empty());
  ASSERT_EQ(4u, wire->writes.size()); expect_parse(wire->writes[3], false);
  EXPECT_EQ(show_deadline, wire->write_deadlines[3]);
  for (auto i = show_reads; i < wire->read_deadlines.size(); ++i)
    EXPECT_EQ(show_deadline, wire->read_deadlines[i]);
  EXPECT_EQ(2u, wire->connects);
  session.disconnect(); EXPECT_FALSE(session.is_connected()); EXPECT_EQ(2u, wire->closes);
  // The first owning metadata result survives reconnect and transport teardown.
  EXPECT_EQ("TABLE_CAT", legacy->columns[0].name);
  EXPECT_EQ("KEY_SEQ", legacy->columns[4].name);
}

TEST(RedshiftCatalogExecution, RealPostgresSessionRejectsForeignModeBeforeTransportIo) {
  auto transport = std::make_unique<CatalogWireTransport>();
  auto* wire = transport.get();
  PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::PostgreSQL);
  ConnectionSettings settings;
  settings.host = "in-memory.invalid"; settings.port = 5432; settings.use_ssl = false;
  for (const auto mode : {RedshiftCatalogMode::Show, RedshiftCatalogMode::Legacy}) {
    settings.redshift_catalog_mode = mode;
    auto result = session.connect(settings);
    ASSERT_FALSE(result);
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter), result.error());
    EXPECT_FALSE(session.is_connected());
    EXPECT_EQ(0u, wire->connects); EXPECT_TRUE(wire->writes.empty());
    EXPECT_TRUE(wire->read_deadlines.empty()); EXPECT_EQ(0u, wire->closes);
  }
  EXPECT_EQ(nullptr, session.catalog_execution());
}

namespace {
// Bounded fake protocol only. Production session/parser methods are untouched.
class CatalogErrorWire final : public rs::core::transport::ITransport {
 public:
  enum class Scenario { PermissionThenSuccess, InvalidReady, PostErrorCompletion };
  explicit CatalogErrorWire(Scenario scenario) : scenario_(scenario) {}
  std::vector<std::vector<std::byte>> writes;
  std::vector<rs::util::Deadline> sends, reads;
  unsigned connects{}, closes{};
  rs::util::Result<void> connect(std::string_view, uint16_t, rs::util::Deadline) override {
    ++connects; input_.clear(); offset_ = 0; operations_ = 0;
    frame('R', {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    std::vector<std::byte> status; text(status, "show_discovery"); text(status, "4");
    frame('S', status); frame('Z', {std::byte{'I'}}); return {};
  }
  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> bytes, rs::util::Deadline deadline) override {
    if (bytes.empty() || bytes.size() > 16384 || writes.size() >= 8)
      return {rs::util::DbErrorCode::ProtocolError, "Fake catalog send bounds exceeded"};
    writes.emplace_back(bytes.begin(), bytes.end()); sends.push_back(deadline);
    if (writes.size() > 1) {
      ++operations_;
      if (operations_ == 1) {
        std::vector<std::byte> error;
        error.push_back(std::byte{'S'}); text(error, "ERROR");
        error.push_back(std::byte{'C'}); text(error, "42501");
        error.push_back(std::byte{'M'}); text(error, "owned catalog permission denial");
        error.push_back(std::byte{0}); frame('E', error);
        if (scenario_ == Scenario::PostErrorCompletion) {
          std::vector<std::byte> command; text(command, "SELECT 0"); frame('C', command);
        }
        frame('Z', {scenario_ == Scenario::InvalidReady ? std::byte{'?'} : std::byte{'I'}});
      } else if (operations_ == 2 && scenario_ == Scenario::PermissionThenSuccess) {
        success();
      } else {
        return {rs::util::DbErrorCode::ProtocolError, "Unexpected fake catalog exchange"};
      }
    }
    return rs::core::transport::IOResult{bytes.size(), false};
  }
  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> bytes, rs::util::Deadline deadline) override {
    if (reads.size() >= 128 || offset_ > input_.size() || input_.size() > 4096)
      return {rs::util::DbErrorCode::ProtocolError, "Fake catalog receive bounds exceeded"};
    reads.push_back(deadline);
    const auto count = std::min(bytes.size(), input_.size() - offset_);
    std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(offset_), count, bytes.begin());
    offset_ += count;
    return rs::core::transport::IOResult{count, count == 0};
  }
  void close() noexcept override { ++closes; }
  void poison_consumed_bytes() {
    std::fill(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(offset_), std::byte{'X'});
  }
 private:
  static void integer(std::vector<std::byte>& out, uint32_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
      out.push_back(static_cast<std::byte>((value >> ((i - 1) * 8)) & 255u));
  }
  static void text(std::vector<std::byte>& out, std::string_view value) {
    for (char byte : value) out.push_back(static_cast<std::byte>(byte));
    out.push_back(std::byte{0});
  }
  void frame(char tag, const std::vector<std::byte>& body) {
    input_.push_back(static_cast<std::byte>(tag));
    integer(input_, static_cast<uint32_t>(body.size() + 4), 4);
    input_.insert(input_.end(), body.begin(), body.end());
  }
  void success() {
    frame('1', {}); std::vector<std::byte> params; integer(params, 3, 2);
    for (unsigned i = 0; i < 3; ++i) integer(params, 25, 4);
    frame('t', params); frame('2', {});
    std::vector<std::byte> description; integer(description, 6, 2);
    for (const auto* name : {"database_name", "schema_name", "table_name", "pk_name", "column_name", "key_seq"}) {
      text(description, name); integer(description, 0, 4); integer(description, 0, 2);
      const bool sequence = std::string_view(name) == "key_seq";
      integer(description, sequence ? 21 : 1043, 4);
      integer(description, sequence ? 2 : 65535, 2);
      integer(description, 0xffffffffu, 4); integer(description, 0, 2);
    }
    frame('T', description); std::vector<std::byte> command; text(command, "SELECT 0");
    frame('C', command); frame('Z', {std::byte{'I'}});
  }
  Scenario scenario_;
  std::vector<std::byte> input_;
  std::size_t offset_{};
  unsigned operations_{};
};
std::optional<std::string> catalog_packet_tags(const std::vector<std::byte>& packet) {
  if (packet.empty() || packet.size() > 16384) return std::nullopt;
  std::string tags;
  for (std::size_t offset = 0; offset < packet.size();) {
    if (packet.size() - offset < 5 || tags.size() >= 16) return std::nullopt;
    uint32_t length = 0;
    for (std::size_t i = 1; i <= 4; ++i)
      length = (length << 8) | std::to_integer<unsigned char>(packet[offset + i]);
    if (length < 4 || length > packet.size() - offset - 1) return std::nullopt;
    tags.push_back(static_cast<char>(packet[offset])); offset += std::size_t(length) + 1;
  }
  return tags;
}
ConnectionSettings catalog_error_settings(RedshiftCatalogMode mode) {
  ConnectionSettings settings;
  settings.host = "in-memory.invalid"; settings.port = 5439;
  settings.database = "db'._%"; settings.user = "fixture"; settings.use_ssl = false;
  settings.redshift_catalog_mode = mode; return settings;
}
void catalog_error_packet(const CatalogErrorWire& wire, std::size_t index, bool legacy) {
  ASSERT_LT(index, wire.writes.size());
  const auto tags = catalog_packet_tags(wire.writes[index]); ASSERT_TRUE(tags.has_value());
  EXPECT_EQ("PDBDES", *tags); // Current combined catalog path, not private binary staged path.
  expect_parse(wire.writes[index], legacy);
}
void catalog_error_deadlines(const CatalogErrorWire& wire, std::size_t start,
    rs::util::Deadline original) {
  ASSERT_LE(start, wire.reads.size());
  for (std::size_t i = start; i < wire.reads.size(); ++i) { EXPECT_EQ(original, wire.reads[i]); }
}
}  // namespace

TEST(RedshiftCatalogExecution, RealWireNativePermissionErrorPreservesModesDeadlineAndRecovery) {
  for (const auto mode : {RedshiftCatalogMode::Show, RedshiftCatalogMode::Legacy}) {
    SCOPED_TRACE(mode == RedshiftCatalogMode::Show ? "SHOW" : "LEGACY");
    auto transport = std::make_unique<CatalogErrorWire>(CatalogErrorWire::Scenario::PermissionThenSuccess);
    auto* wire = transport.get();
    PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
    ASSERT_TRUE(session.connect(catalog_error_settings(mode))); ASSERT_EQ(1u, wire->writes.size());
    const auto original = rs::util::make_deadline(std::chrono::seconds{3});
    const auto reads_before = wire->reads.size(); auto names = request();
    auto denied = session.execute_catalog(names, original);
    ASSERT_TRUE(denied.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), denied.error());
    EXPECT_EQ(BackendErrorClass::Server, denied.backend_error().error_class);
    EXPECT_EQ(std::optional<std::string>{"42501"}, denied.backend_error().native_state);
    EXPECT_EQ("Query error: owned catalog permission denial", denied.error_message());
    EXPECT_EQ(BackendOperation::ExecutePrepared, denied.backend_error().operation);
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), denied.session_snapshot());
    EXPECT_TRUE(session.is_connected()); EXPECT_EQ(0u, wire->closes); EXPECT_EQ(1u, wire->connects);
    ASSERT_EQ(2u, wire->writes.size()); ASSERT_EQ(2u, wire->sends.size());
    catalog_error_packet(*wire, 1, mode == RedshiftCatalogMode::Legacy);
    EXPECT_EQ(original, wire->sends[1]); catalog_error_deadlines(*wire, reads_before, original);
    names.catalog = "changed"; names.schema = "changed"; names.table.clear(); wire->poison_consumed_bytes();
    const auto recovery_deadline = rs::util::make_deadline(std::chrono::seconds{3});
    const auto recovery_reads = wire->reads.size();
    auto recovered = session.execute_catalog(request(), recovery_deadline);
    ASSERT_TRUE(recovered); ASSERT_EQ(6u, recovered->columns.size()); EXPECT_TRUE(recovered->rows.empty());
    const std::array<std::string_view, 6> fields{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"};
    for (std::size_t i = 0; i < fields.size(); ++i) {
      EXPECT_EQ(fields[i], recovered->columns[i].name);
      ASSERT_TRUE(recovered->columns[i].normalized_type.has_value());
      EXPECT_EQ(i == 4 ? ScalarType::SmallInt : ScalarType::VarChar, recovered->columns[i].normalized_type->type);
    }
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), recovered.session_snapshot());
    ASSERT_EQ(3u, wire->writes.size()); ASSERT_EQ(3u, wire->sends.size());
    catalog_error_packet(*wire, 2, mode == RedshiftCatalogMode::Legacy);
    EXPECT_EQ(recovery_deadline, wire->sends[2]); catalog_error_deadlines(*wire, recovery_reads, recovery_deadline);
    EXPECT_EQ(1u, wire->connects); EXPECT_EQ(0u, wire->closes);
    EXPECT_EQ("Query error: owned catalog permission denial", denied.error_message());
    EXPECT_EQ(std::optional<std::string>{"42501"}, denied.backend_error().native_state);
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), denied.session_snapshot());
  }
}

TEST(RedshiftCatalogExecution, RealWireMalformedCatalogCompletionRetiresWithoutFallback) {
  for (const auto mode : {RedshiftCatalogMode::Show, RedshiftCatalogMode::Legacy}) {
    for (const auto scenario : {CatalogErrorWire::Scenario::InvalidReady, CatalogErrorWire::Scenario::PostErrorCompletion}) {
      SCOPED_TRACE(mode == RedshiftCatalogMode::Show ? "SHOW" : "LEGACY");
      SCOPED_TRACE(scenario == CatalogErrorWire::Scenario::InvalidReady ? "invalid-ready" : "post-error-C");
      auto transport = std::make_unique<CatalogErrorWire>(scenario); auto* wire = transport.get();
      PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
      ASSERT_TRUE(session.connect(catalog_error_settings(mode))); ASSERT_EQ(1u, wire->writes.size());
      const auto original = rs::util::make_deadline(std::chrono::seconds{3});
      const auto reads_before = wire->reads.size(); auto names = request();
      auto malformed = session.execute_catalog(names, original); ASSERT_TRUE(malformed.has_error());
      EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError), malformed.error());
      EXPECT_EQ(BackendErrorClass::Protocol, malformed.backend_error().error_class);
      EXPECT_EQ(BackendOperation::ExecutePrepared, malformed.backend_error().operation);
      EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), malformed.session_snapshot());
      EXPECT_FALSE(session.is_connected()); EXPECT_EQ(1u, wire->closes); EXPECT_EQ(1u, wire->connects);
      ASSERT_EQ(2u, wire->writes.size()); ASSERT_EQ(2u, wire->sends.size());
      catalog_error_packet(*wire, 1, mode == RedshiftCatalogMode::Legacy);
      EXPECT_EQ(original, wire->sends[1]); catalog_error_deadlines(*wire, reads_before, original);
      const std::string owning_error = malformed.error_message();
      EXPECT_EQ(scenario == CatalogErrorWire::Scenario::InvalidReady ? "Invalid PostgreSQL ReadyForQuery payload" :
          "PostgreSQL result frame arrived after ErrorResponse", owning_error);
      names.catalog.reset(); names.schema.reset(); names.table.clear(); wire->poison_consumed_bytes();
      const auto reads_after = wire->reads.size();
      auto disconnected = session.execute_catalog(request(), original); ASSERT_TRUE(disconnected.has_error());
      EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::NotConnected), disconnected.error());
      EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), disconnected.session_snapshot());
      EXPECT_EQ(2u, wire->writes.size()); EXPECT_EQ(reads_after, wire->reads.size());
      EXPECT_EQ(1u, wire->closes); EXPECT_EQ(1u, wire->connects);
      EXPECT_EQ(owning_error, malformed.error_message());
      EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), malformed.session_snapshot());
    }
  }
}
