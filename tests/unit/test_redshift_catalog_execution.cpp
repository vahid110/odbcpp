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

#include "core/database/postgres/redshift_catalog_query.h"
#include <functional>
#include <limits>

namespace {
TablesCatalogRequest schemas_request() {
  TablesCatalogRequest value; value.mode = TablesCatalogRequest::Mode::Schemas;
  return value;
}
QueryResult schemas_response(std::string database = "selected") {
  QueryResult value;
  for (const auto* name : {"database_name", "schema_name", "schema_owner", "schema_type",
                          "schema_acl", "source_database", "schema_option"})
    value.columns.push_back({name, NativeTypeInfo{
        std::string_view(name) == "schema_owner" ? ScalarType::Integer : ScalarType::VarChar,
        128, 0, true}});
  value.rows = {{database, "z", "1", "local", std::nullopt, std::nullopt, std::nullopt},
                {database, "a", "1", "local", std::nullopt, std::nullopt, std::nullopt}};
  return value;
}
class SchemaSpy final : public SpySession {
 public:
  std::string database{"selected"};
  QueryResult show{schemas_response()};
  std::optional<QueryResult> identity_override;
  std::optional<BackendError> direct_failure;
  unsigned failing_call{2};
  std::function<void(unsigned, rs::util::Deadline)> before_return;
  std::vector<std::string> queries;
  std::vector<rs::util::Deadline> deadlines;
  BackendResult<QueryResult> execute_query(std::string_view query,
      rs::util::Deadline deadline) override {
    ++direct_calls; queries.emplace_back(query); deadlines.push_back(deadline);
    if (before_return) before_return(direct_calls, deadline);
    if (direct_failure && direct_calls == failing_call) return *direct_failure;
    if (direct_calls % 2 == 1) {
      QueryResult identity;
      identity.columns = {{"database_name", NativeTypeInfo{ScalarType::VarChar, 128, 0, true}}};
      identity.rows = {{database}}; identity.affected_rows = 1;
      identity.statement_kind = StatementKind::SelectCursor;
      return BackendResult<QueryResult>{identity_override.value_or(identity), snapshot};
    }
    return BackendResult<QueryResult>{show, snapshot};
  }
};
void schema_error(const BackendResult<QueryResult>& result) {
  ASSERT_FALSE(result);
  EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
  EXPECT_EQ(BackendOperation::ExecuteCatalog, result.backend_error().operation);
  EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
}
}  // namespace

namespace { void schema_real_wire_success(); }

TEST(RedshiftShowSchemas, SelectionIsRequestProfileAndModeSpecificNotCapability) {
  SchemaSpy show; show.capability.clear();
  EXPECT_TRUE(show.selects_catalog_request(schemas_request()));
  EXPECT_TRUE(show.selects_catalog_request(request()));
  EXPECT_FALSE(show.selects_catalog_request(TablesCatalogRequest{}));
  EXPECT_FALSE(show.selects_catalog_request(ColumnsCatalogRequest{}));
  show.mode = RedshiftCatalogMode::Legacy;
  EXPECT_FALSE(show.selects_catalog_request(schemas_request()));
  EXPECT_TRUE(show.selects_catalog_request(request()));
  SpySession pg(PgCatalogProfile::PostgreSQL);
  EXPECT_FALSE(pg.selects_catalog_request(schemas_request()));
  EXPECT_FALSE(pg.selects_catalog_request(request()));
}

TEST(RedshiftShowSchemas, OwningSortedFiveFieldsAndOriginalDeadline) {
  schema_real_wire_success(); ASSERT_FALSE(HasFailure());
  SchemaSpy spy; const auto deadline = rs::util::make_deadline(std::chrono::seconds{2});
  spy.snapshot = {SessionState::Transaction, SessionDisposition::ResetRequired};
  auto input = schemas_request(); input.catalog = "ignored"; input.schema = "ignored";
  auto result = spy.execute_catalog(input, deadline);
  ASSERT_TRUE(result); ASSERT_EQ(2u, spy.direct_calls); EXPECT_EQ(0u, spy.prepared_calls);
  EXPECT_EQ(0u, spy.catalog_builds); ASSERT_EQ(2u, spy.queries.size());
  EXPECT_EQ("SELECT current_database() AS database_name", spy.queries[0]);
  EXPECT_EQ("SHOW SCHEMAS FROM DATABASE \"selected\";", spy.queries[1]);
  EXPECT_EQ((std::vector<rs::util::Deadline>{deadline, deadline}), spy.deadlines);
  EXPECT_EQ(spy.snapshot, result.session_snapshot());
  ASSERT_EQ(5u, result->columns.size());
  const std::array<const char*, 5> names{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(names[i], result->columns[i].name);
    ASSERT_TRUE(result->columns[i].normalized_type);
    EXPECT_TRUE(result->columns[i].normalized_type->known);
    EXPECT_EQ(ScalarType::VarChar, result->columns[i].normalized_type->type);
    EXPECT_EQ(i == 1 ? 128u : 0u, result->columns[i].normalized_type->column_size);
  }
  EXPECT_EQ((ResultRows{{std::nullopt, "a", std::nullopt, std::nullopt, std::nullopt},
                       {std::nullopt, "z", std::nullopt, std::nullopt, std::nullopt}}), result->rows);
  spy.show.rows.clear(); spy.database.clear(); input.schema.reset(); spy.connected = false;
  EXPECT_EQ("a", result->rows[0][1]); EXPECT_EQ("TABLE_SCHEM", result->columns[1].name);
}

TEST(RedshiftShowSchemas, EmptyResultPreservesUnknownSourceCapacityWithoutLimit) {
  SchemaSpy spy; spy.show.rows.clear(); spy.show.columns[1].normalized_type->column_size = 0;
  auto result = spy.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_TRUE(result); EXPECT_TRUE(result->rows.empty()); ASSERT_EQ(5u, result->columns.size());
  EXPECT_EQ(0u, result->columns[1].normalized_type->column_size);
  EXPECT_EQ(std::string::npos, spy.queries[1].find("LIMIT"));
}

TEST(RedshiftShowSchemas, ReturnedUnicodeIdentifierIsQuotedAndMalformedIdentityStopsShow) {
  SchemaSpy quoted; quoted.database = "quoted\"._%数据库"; quoted.show = schemas_response(quoted.database);
  auto result = quoted.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_TRUE(result);
  EXPECT_EQ("SHOW SCHEMAS FROM DATABASE \"quoted\"\"._%数据库\";", quoted.queries[1]);
  for (const auto& bad : {std::string{}, std::string("x\0y", 3), std::string("\xff", 1)}) {
    SchemaSpy spy; spy.database = bad;
    schema_error(spy.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1})));
    EXPECT_EQ(1u, spy.direct_calls); EXPECT_EQ(0u, spy.prepared_calls);
  }
  for (unsigned variant = 0; variant < 4; ++variant) {
    SchemaSpy spy; QueryResult identity;
    identity.columns = {{"database_name", NativeTypeInfo{ScalarType::VarChar, 128, 0, true}}};
    identity.rows = {{"selected"}}; identity.affected_rows = 1;
    identity.statement_kind = StatementKind::SelectCursor;
    if (variant == 0) identity.rows[0][0].reset();
    if (variant == 1) identity.rows.push_back({"selected"});
    if (variant == 2) identity.columns[0].normalized_type->known = false;
    if (variant == 3) identity.columns[0].name = "wrong";
    spy.identity_override = identity;
    schema_error(spy.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1})));
    EXPECT_EQ(1u, spy.direct_calls);
  }
}

TEST(RedshiftShowSchemas, MalformedCapabilitySelectsRefusalWithoutAnySql) {
  for (const auto& capability : {std::string{}, std::string{"3"}, std::string{"+4"},
      std::string{"4suffix"}, std::string{"4294967296"}, std::string("4\0x", 3)}) {
    SchemaSpy spy; spy.capability = capability;
    EXPECT_TRUE(spy.selects_catalog_request(schemas_request()));
    auto result = spy.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1}));
    ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::Unsupported, result.backend_error().error_class);
    EXPECT_EQ(SessionDisposition::Reusable, result.session_snapshot().disposition); no_execution(spy);
  }
}

TEST(RedshiftShowSchemas, MalformedForeignDuplicateAndNullRowsAreNotPartialSuccess) {
  for (unsigned variant = 0; variant < 12; ++variant) {
    SchemaSpy spy;
    switch (variant) {
      case 0: spy.show.rows[0][0] = "foreign"; break;
      case 1: spy.show.rows[0][0].reset(); break;
      case 2: spy.show.rows[0][1].reset(); break;
      case 3: spy.show.rows[0][1] = ""; break;
      case 4: spy.show.rows[0][1] = std::string("x\0y", 3); break;
      case 5: spy.show.rows[1][1] = spy.show.rows[0][1]; break;
      case 6: spy.show.columns[1].name = "database_name"; break;
      case 7: spy.show.columns[1].normalized_type->type = ScalarType::Integer; break;
      case 8: spy.show.additional_results.push_back(QueryResult{}); break;
      case 9: spy.show.cell_errors.push_back({0, 1}); break;
      case 10: spy.show.rows[0].pop_back(); break;
      case 11: spy.show.rows.resize(10001, spy.show.rows[0]); break;
    }
    SCOPED_TRACE(variant);
    schema_error(spy.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1})));
    EXPECT_EQ(2u, spy.direct_calls); EXPECT_EQ(0u, spy.prepared_calls); EXPECT_EQ(0u, spy.catalog_builds);
  }
}

TEST(RedshiftShowSchemas, NativeFailureAndRetirementStayOwningWithoutReplayAndRecoveryIsExplicit) {
  SchemaSpy spy;
  BackendError failure{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "synthetic"};
  failure.native_state = "42501"; failure.operation = BackendOperation::ExecuteDirect;
  failure.session_state = SessionState::Idle; failure.disposition = SessionDisposition::Reusable;
  spy.direct_failure = failure;
  auto result = spy.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_FALSE(result); EXPECT_EQ(failure.native_state, result.backend_error().native_state);
  EXPECT_EQ(failure.operation, result.backend_error().operation); EXPECT_EQ(2u, spy.direct_calls);
  SchemaSpy deferred;
  QueryResult identity;
  identity.columns = {{"database_name", NativeTypeInfo{ScalarType::VarChar, 128, 0, true}}};
  identity.rows = {{"selected"}}; identity.affected_rows = 1;
    identity.statement_kind = StatementKind::SelectCursor;
  QueryResult extra; extra.error = failure; identity.additional_results.push_back(extra);
  deferred.identity_override = identity;
  auto deferred_error = deferred.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_FALSE(deferred_error); EXPECT_EQ("42501", deferred_error.backend_error().native_state);
  EXPECT_EQ(1u, deferred.direct_calls);
  spy.direct_failure.reset();
  auto recovered = spy.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_TRUE(recovered); EXPECT_EQ(4u, spy.direct_calls); EXPECT_EQ(0u, spy.catalog_builds);
  SchemaSpy retired; retired.before_return = [&retired](unsigned call, rs::util::Deadline) {
    if (call == 2) retired.snapshot = {SessionState::Disconnected, SessionDisposition::Retire};
  };
  auto final = retired.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{1}));
  ASSERT_TRUE(final); EXPECT_EQ(retired.snapshot, final.session_snapshot());
}

TEST(RedshiftShowSchemas, ExpiredAndLateIdentityNeverDispatchShow) {
  SchemaSpy expired;
  auto result = expired.execute_catalog(schemas_request(), rs::util::Clock::now());
  ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::Timeout, result.backend_error().error_class);
  no_execution(expired); EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
  for (const bool malformed : {false, true}) {
    SchemaSpy late;
    if (malformed) late.database.clear();
    late.before_return = [](unsigned, rs::util::Deadline deadline) {
      while (rs::util::Clock::now() < deadline) {}
    };
    auto after = late.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::milliseconds{10}));
    ASSERT_FALSE(after); EXPECT_EQ(BackendErrorClass::Timeout, after.backend_error().error_class);
    EXPECT_EQ(1u, late.direct_calls); EXPECT_EQ(0u, late.prepared_calls);
  }
}

namespace {
// Real parser/Generic path: a completed SELECT can omit its affected count.
// Identity cardinality and explicit completion remain separate. No overrides.
class SchemaWireTransport final : public rs::core::transport::ITransport {
 public:
  unsigned queries{0};
  std::vector<rs::util::Deadline> query_deadlines;
  rs::util::Result<void> connect(std::string_view, uint16_t, rs::util::Deadline) override {
    frame('R', {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    std::vector<std::byte> status; text(status, "show_discovery"); text(status, "4");
    frame('S', status); frame('Z', {std::byte{'I'}}); return {};
  }
  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> bytes, rs::util::Deadline deadline) override {
    if (!bytes.empty() && bytes[0] == std::byte{'Q'}) {
      ++queries; query_deadlines.push_back(deadline);
      if (queries > 2 || bytes.size() < 6 || bytes.back() != std::byte{0})
        return {rs::util::DbErrorCode::ProtocolError, "Invalid synthetic schema request"};
      std::string sql;
      for (std::size_t i = 5; i + 1 < bytes.size(); ++i) sql.push_back(static_cast<char>(bytes[i]));
      EXPECT_EQ(queries == 1 ? "SELECT current_database() AS database_name" :
          "SHOW SCHEMAS FROM DATABASE \"selected\";", sql);
      std::vector<std::byte> description; integer(description, queries == 1 ? 1 : 7, 2);
      const std::vector<std::string_view> names = queries == 1 ?
          std::vector<std::string_view>{"database_name"} :
          std::vector<std::string_view>{"database_name", "schema_name", "schema_owner",
              "schema_type", "schema_acl", "source_database", "schema_option"};
      for (const auto name : names) {
        text(description, name); integer(description, 0, 4); integer(description, 0, 2);
        const bool owner = name == "schema_owner";
        integer(description, owner ? 23 : 1043, 4); integer(description, owner ? 4 : 65535, 2);
        integer(description, owner ? 0xffffffffu : 132, 4); integer(description, 0, 2);
      }
      frame('T', description);
      std::vector<std::byte> row; integer(row, queries == 1 ? 1 : 7, 2);
      for (const auto& value : queries == 1 ? std::vector<ResultCell>{"selected"} :
          std::vector<ResultCell>{"selected", "a", "1", "local", std::nullopt, std::nullopt, std::nullopt}) {
        if (!value) integer(row, 0xffffffffu, 4);
        else { integer(row, static_cast<uint32_t>(value->size()), 4);
          for (char ch : *value) row.push_back(static_cast<std::byte>(ch)); }
      }
      frame('D', row); std::vector<std::byte> completion;
      text(completion, queries == 1 ? "SELECT" : "SHOW"); frame('C', completion);
      frame('Z', {std::byte{'I'}});
    }
    return rs::core::transport::IOResult{bytes.size(), false};
  }
  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> bytes, rs::util::Deadline) override {
    const auto count = std::min(bytes.size(), input.size() - offset);
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), count, bytes.begin());
    offset += count; return rs::core::transport::IOResult{count, count == 0};
  }
  void close() noexcept override {}
 private:
  static void integer(std::vector<std::byte>& out, uint32_t value, unsigned width) {
    for (unsigned i = width; i > 0; --i)
      out.push_back(static_cast<std::byte>((value >> ((i - 1) * 8)) & 255u));
  }
  static void text(std::vector<std::byte>& out, std::string_view value) {
    for (char ch : value) out.push_back(static_cast<std::byte>(ch));
    out.push_back(std::byte{0});
  }
  void frame(char tag, const std::vector<std::byte>& body) {
    input.push_back(static_cast<std::byte>(tag)); integer(input, static_cast<uint32_t>(body.size() + 4), 4);
    input.insert(input.end(), body.begin(), body.end());
  }
  std::vector<std::byte> input;
  std::size_t offset{0};
};
void schema_real_wire_success() {
  auto transport = std::make_unique<SchemaWireTransport>(); auto* wire = transport.get();
  PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
  ConnectionSettings settings; settings.host = "in-memory.invalid"; settings.port = 5439;
  settings.database = "selected"; settings.user = "synthetic"; settings.use_ssl = false;
  ASSERT_TRUE(session.connect(settings));
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{2});
  auto result = session.execute_catalog(schemas_request(), deadline);
  ASSERT_TRUE(result); EXPECT_EQ(2u, wire->queries);
  EXPECT_EQ((std::vector<rs::util::Deadline>{deadline, deadline}), wire->query_deadlines);
  ASSERT_EQ(1u, result->rows.size()); EXPECT_EQ("a", result->rows[0][1]);
  EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
  session.disconnect(); EXPECT_EQ("a", result->rows[0][1]);
}
}  // namespace

namespace {
QueryResult schema_lookup_fixture() {
  QueryResult value;
  value.columns={{"database_name",NativeTypeInfo{ScalarType::VarChar,128,0,true}}};
  value.rows={{"selected"}};value.affected_rows=1;
  value.statement_kind=StatementKind::SelectCursor;return value;
}
template<class T> void expect_schema_reason(const BackendResult<T>& result,std::string_view reason) {
  ASSERT_FALSE(result);
  EXPECT_EQ(std::string("Invalid Redshift schema metadata: ")+std::string(reason),result.error_message());
  EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);
  EXPECT_EQ(BackendOperation::ExecuteCatalog,result.backend_error().operation);
  EXPECT_EQ(SessionDisposition::Retire,result.session_snapshot().disposition);
}
}

TEST(RedshiftShowSchemasDiagnostics, LookupReasonsPreserveFirstGuardAndCompletedSelectPolicy) {
  const std::array<std::string_view,8> expected{"lookup-structure","lookup-columns","lookup-name",
      "lookup-type","lookup-rows","lookup-completion","lookup-identifier","lookup-snapshot"};
  for(unsigned variant=0;variant<expected.size();++variant) {
    auto value=schema_lookup_fixture();SessionSnapshot snapshot{SessionState::Idle,SessionDisposition::Reusable};
    switch(variant) {
      case 0:value.cell_errors.push_back({0,0});value.affected_rows=0;break;
      case 1:value.columns.push_back(value.columns[0]);value.rows[0].push_back("extra");break;
      case 2:value.columns[0].name="other";value.affected_rows=0;break;
      case 3:value.columns[0].normalized_type->known=false;break;
      case 4:value.rows.clear();break;
      case 5:value.affected_rows=2;value.rows[0][0].reset();break;
      case 6:value.rows[0][0].reset();break;
      case 7:snapshot.disposition=SessionDisposition::Retire;break;
    }
    SCOPED_TRACE(variant);
    expect_schema_reason(redshift_schema_database(BackendResult<QueryResult>{std::move(value),snapshot}),expected[variant]);
  }
  auto valid=redshift_schema_database(BackendResult<QueryResult>{schema_lookup_fixture(),
      {SessionState::Idle,SessionDisposition::Reusable}});
  ASSERT_TRUE(valid);EXPECT_EQ("selected",*valid);
}

TEST(RedshiftShowSchemasDiagnostics, ShowReasonsKeepOrderedShapeAndRowRejection) {
  const std::array<std::string_view,11> expected{"show-database","show-structure","show-columns",
      "show-rows","show-completion","show-layout","show-type","show-null-identity",
      "show-foreign-database","show-identifier","show-duplicate"};
  for(unsigned variant=0;variant<expected.size();++variant) {
    auto value=schemas_response();std::string database="selected";
    switch(variant) {
      case 0:database.clear();value.columns.clear();break;
      case 1:value.cell_errors.push_back({0,1});value.columns[1].name="wrong";break;
      case 2:value.columns.pop_back();for(auto& row:value.rows)row.pop_back();break;
      case 3:value.rows.resize(10001,value.rows[0]);value.affected_rows=8;break;
      case 4:value.affected_rows=1;value.columns[1].name="wrong";break;
      case 5:value.columns[1].name="wrong";break;
      case 6:value.columns[1].normalized_type->type=ScalarType::Integer;break;
      case 7:value.rows[0][1].reset();value.rows[0][0]="foreign";break;
      case 8:value.rows[0][0]="foreign";value.rows[0][1]="";break;
      case 9:value.rows[0][1]="";break;
      case 10:value.rows[1][1]=value.rows[0][1];break;
    }
    SCOPED_TRACE(variant);
    expect_schema_reason(normalize_redshift_schemas(database,BackendResult<QueryResult>{std::move(value),
        {SessionState::Idle,SessionDisposition::Reusable}}),expected[variant]);
  }
  auto valid=normalize_redshift_schemas("selected",BackendResult<QueryResult>{schemas_response(),
      {SessionState::Idle,SessionDisposition::Reusable}});
  ASSERT_TRUE(valid);EXPECT_EQ(2u,valid->rows.size());
}

TEST(RedshiftShowSchemasDiagnostics, DeferredNativeErrorRemainsOwningWithoutLocalRelabeling) {
  auto value=schema_lookup_fixture();QueryResult extra;
  BackendError native{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),"synthetic"};
  native.native_state="42501";native.session_state=SessionState::Idle;native.disposition=SessionDisposition::Reusable;
  extra.error=native;value.additional_results.push_back(extra);
  auto result=redshift_schema_database(BackendResult<QueryResult>{value,{SessionState::Idle,SessionDisposition::Reusable}});
  ASSERT_FALSE(result);EXPECT_EQ(native.message,result.error_message());EXPECT_EQ("42501",result.backend_error().native_state);
  EXPECT_EQ(SessionDisposition::Reusable,result.session_snapshot().disposition);
}

#include "core/database/postgres/pg_protocol_parser.h"
TEST(RedshiftShowSchemasDiagnostics, LiteralCompletedSelectTagsAndInvalidCompletions) {
  PgProtocolParser parser;
  Message no_count;no_count.tag='C';
  no_count.payload={std::byte{'S'},std::byte{'E'},std::byte{'L'},std::byte{'E'},
      std::byte{'C'},std::byte{'T'},std::byte{0}};
  Message count_one;count_one.tag='C';
  count_one.payload={std::byte{'S'},std::byte{'E'},std::byte{'L'},std::byte{'E'},
      std::byte{'C'},std::byte{'T'},std::byte{' '},std::byte{'1'},std::byte{0}};
  const auto uncounted_completion=parser.extract_query_result({no_count});
  auto uncounted=schema_lookup_fixture();
  uncounted.affected_rows=uncounted_completion.affected_rows;
  uncounted.statement_kind=uncounted_completion.statement_kind;
  EXPECT_EQ(0u,uncounted.affected_rows);
  EXPECT_EQ(StatementKind::SelectCursor,uncounted.statement_kind);
  auto owning=redshift_schema_database(BackendResult<QueryResult>{uncounted,
      {SessionState::Idle,SessionDisposition::Reusable}});
  ASSERT_TRUE(owning);uncounted.rows[0][0]="poison";EXPECT_EQ("selected",*owning);
  const auto counted_completion=parser.extract_query_result({count_one});
  auto counted=schema_lookup_fixture();counted.affected_rows=counted_completion.affected_rows;
  counted.statement_kind=counted_completion.statement_kind;
  EXPECT_EQ(1u,counted.affected_rows);
  EXPECT_TRUE(redshift_schema_database(BackendResult<QueryResult>{counted,
      {SessionState::Idle,SessionDisposition::Reusable}}));
  for(unsigned variant=0;variant<5;++variant) {
    auto refused=schema_lookup_fixture();refused.affected_rows=0;
    if(variant==0)refused.statement_kind.reset();
    if(variant==1)refused.statement_kind=StatementKind::Unknown;
    if(variant==2)refused.statement_kind=StatementKind::UpdateWhere;
    if(variant==3)refused.affected_rows=2;
    if(variant==4)refused.affected_rows=std::numeric_limits<std::size_t>::max();
    SCOPED_TRACE(variant);
    expect_schema_reason(redshift_schema_database(BackendResult<QueryResult>{refused,
        {SessionState::Idle,SessionDisposition::Reusable}}),"lookup-completion");
  }
  for(const unsigned count:{0u,2u}) {
    auto refused=schema_lookup_fixture();refused.affected_rows=0;
    refused.rows.resize(count,{"selected"});
    expect_schema_reason(redshift_schema_database(BackendResult<QueryResult>{refused,
        {SessionState::Idle,SessionDisposition::Reusable}}),"lookup-rows");
  }
  for(const unsigned count:{6u,8u}) {
    auto value=schemas_response();
    value.columns.resize(count,{"extra",NativeTypeInfo{ScalarType::VarChar,128,0,true}});
    for(auto& row:value.rows)row.resize(count);
    expect_schema_reason(normalize_redshift_schemas("selected",BackendResult<QueryResult>{value,
        {SessionState::Idle,SessionDisposition::Reusable}}),"show-columns");
  }
}

TEST(RedshiftShowSchemasDiagnostics, UnicodeSchemaIdentityOwnsAndSortsExactUtf8) {
  const std::vector<std::string> names{"数据库_😀", "quote\"._%", "é_Σ_東京", "a"};
  auto source=schemas_response();source.rows.clear();
  for(const auto& name:names)
    source.rows.push_back({"selected",name,"1","local",std::nullopt,std::nullopt,std::nullopt});
  auto output=normalize_redshift_schemas("selected",BackendResult<QueryResult>{source,
      {SessionState::Idle,SessionDisposition::Reusable}});
  ASSERT_TRUE(output);source.rows.clear();
  auto expected=names;std::sort(expected.begin(),expected.end());
  ASSERT_EQ(expected.size(),output->rows.size());
  for(std::size_t i=0;i<expected.size();++i) {
    ASSERT_EQ(5u,output->rows[i].size());EXPECT_EQ(expected[i],output->rows[i][1]);
    EXPECT_FALSE(output->rows[i][0]);EXPECT_FALSE(output->rows[i][2]);
    EXPECT_FALSE(output->rows[i][3]);EXPECT_FALSE(output->rows[i][4]);
  }
  auto malformed=schemas_response();malformed.rows[0][1]=std::string("\xf0\x28\x8c\x28",4);
  expect_schema_reason(normalize_redshift_schemas("selected",BackendResult<QueryResult>{malformed,
      {SessionState::Idle,SessionDisposition::Reusable}}),"show-identifier");
}

TEST(RedshiftShowSchemasDiagnostics, ActualParserRowsWithoutCompletionCannotSupplyIdentity) {
  PgProtocolParser parser;
  const auto integer=[](std::vector<std::byte>& out,std::uint32_t value,unsigned width) {
    for(unsigned i=width;i>0;--i)out.push_back(static_cast<std::byte>((value>>((i-1)*8))&255u));
  };
  const auto text=[](std::vector<std::byte>& out,std::string_view value) {
    for(char c:value)out.push_back(static_cast<std::byte>(c));
    out.push_back(std::byte{0});
  };
  Message description;description.tag='T';integer(description.payload,1,2);
  text(description.payload,"database_name");integer(description.payload,0,4);
  integer(description.payload,0,2);integer(description.payload,1043,4);
  integer(description.payload,65535,2);integer(description.payload,132,4);
  integer(description.payload,0,2);
  Message row;row.tag='D';integer(row.payload,1,2);integer(row.payload,8,4);
  for(char c:std::string_view("selected"))row.payload.push_back(static_cast<std::byte>(c));
  const auto parsed=parser.extract_query_result({description,row});
  ASSERT_EQ(1u,parsed.rows.size());ASSERT_EQ(1u,parsed.columns.size());
  EXPECT_EQ(0u,parsed.affected_rows);EXPECT_FALSE(parsed.statement_kind);
  auto identity=schema_lookup_fixture();identity.rows=parsed.rows;
  identity.affected_rows=parsed.affected_rows;identity.statement_kind=parsed.statement_kind;
  expect_schema_reason(redshift_schema_database(BackendResult<QueryResult>{identity,
      {SessionState::Idle,SessionDisposition::Reusable}}),"lookup-completion");
  Message completion;completion.tag='C';text(completion.payload,"SELECT 2");
  const auto contradictory=parser.extract_query_result({description,row,completion});
  identity.rows=contradictory.rows;identity.affected_rows=contradictory.affected_rows;
  identity.statement_kind=contradictory.statement_kind;
  EXPECT_EQ(2u,identity.affected_rows);
  expect_schema_reason(redshift_schema_database(BackendResult<QueryResult>{identity,
      {SessionState::Idle,SessionDisposition::Reusable}}),"lookup-completion");
}

#include <thread>

namespace {
TablesCatalogRequest table_request() {
  TablesCatalogRequest value; value.schema = "fixture"; return value;
}
QueryResult tables_response(std::string database="selected",std::string schema="fixture") {
  QueryResult value;
  for (const auto* name : {"database_name","schema_name","table_name","table_type","remarks","table_acl"})
    value.columns.push_back({name,NativeTypeInfo{ScalarType::VarChar,128,0,true}});
  value.rows={{database,schema,"z","VIEW",std::nullopt,std::nullopt},
              {database,schema,"a","TABLE","owning remark",std::nullopt}};
  value.statement_kind=StatementKind::Unknown; // Literal SHOW completion.
  return value;
}
}

TEST(RedshiftShowTables, ScopedSelectionOwningSortAndDeadlineWithoutLegacyFallback) {
  SchemaSpy spy;spy.show=tables_response();auto input=table_request();
  const auto deadline=rs::util::make_deadline(std::chrono::seconds{2});
  EXPECT_TRUE(spy.selects_catalog_request(input));
  auto result=spy.execute_catalog(input,deadline);ASSERT_TRUE(result);
  EXPECT_EQ((std::vector<std::string>{"SELECT current_database() AS database_name",
      "SHOW TABLES FROM SCHEMA \"selected\".\"fixture\";"}),spy.queries);
  EXPECT_EQ((std::vector<rs::util::Deadline>{deadline,deadline}),spy.deadlines);
  EXPECT_EQ(0u,spy.prepared_calls);EXPECT_EQ(0u,spy.catalog_builds);
  ASSERT_EQ(5u,result->columns.size());
  const std::array<const char*,5> names{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","TABLE_TYPE","REMARKS"};
  for(std::size_t i=0;i<5;++i){EXPECT_EQ(names[i],result->columns[i].name);ASSERT_TRUE(result->columns[i].normalized_type);EXPECT_EQ(128u,result->columns[i].normalized_type->column_size);}
  EXPECT_EQ((ResultRows{{"selected","fixture","a","TABLE","owning remark"},
      {"selected","fixture","z","VIEW",std::nullopt}}),result->rows);
  spy.show.rows.clear();input.schema.reset();spy.connected=false;
  EXPECT_EQ("owning remark",result->rows[0][4]);EXPECT_EQ(SessionState::Idle,result.session_snapshot().state);
  SchemaSpy empty;empty.show=tables_response();empty.show.rows.clear();
  auto zero=empty.execute_catalog(table_request(),deadline);ASSERT_TRUE(zero);EXPECT_TRUE(zero->rows.empty());EXPECT_EQ(5u,zero->columns.size());
}

TEST(RedshiftShowTables, LiteralSchemaEscapesUnicodePatternsAndDetailedTypes) {
  SchemaSpy spy;spy.database="db\"é";auto request=table_request();request.schema="s\\_\\%\"表";
  spy.show=tables_response(spy.database,"s_%\"表");
  spy.show.rows={{spy.database,"s_%\"表","é表😀","EXTERNAL TABLE","remark",std::nullopt},
      {spy.database,"s_%\"表","é_%","SYSTEM VIEW",std::nullopt,std::nullopt}};
  request.catalog="db\"_";request.table="___";request.types=std::vector<std::string>{"EXTERNAL TABLE"};
  auto result=spy.execute_catalog(request,rs::util::make_deadline(std::chrono::seconds{1}));ASSERT_TRUE(result);
  EXPECT_EQ("SHOW TABLES FROM SCHEMA \"db\"\"é\".\"s_%\"\"表\";",spy.queries[1]);
  ASSERT_EQ(1u,result->rows.size());EXPECT_EQ("é表😀",result->rows[0][2]);
  request.table="é\\_\\%";request.types.reset();
  auto escaped=normalize_redshift_tables(spy.database,"s_%\"表",request,
      BackendResult<QueryResult>{spy.show,spy.snapshot});ASSERT_TRUE(escaped);ASSERT_EQ(1u,escaped->rows.size());EXPECT_EQ("SYSTEM VIEW",escaped->rows[0][3]);
  for(const auto& pattern:{std::string("no match"),std::string(""),std::string("é%z")}){
    request.table=pattern;auto none=normalize_redshift_tables(spy.database,"s_%\"表",request,BackendResult<QueryResult>{spy.show,spy.snapshot});ASSERT_TRUE(none);EXPECT_TRUE(none->rows.empty());
  }
  request.table="%";request.types=std::vector<std::string>{};
  auto none=normalize_redshift_tables(spy.database,"s_%\"表",request,BackendResult<QueryResult>{spy.show,spy.snapshot});ASSERT_TRUE(none);EXPECT_TRUE(none->rows.empty());
  // Optional documented fields may be reordered; no field-count/version inference.
  auto extended=tables_response();for(const auto* name:{"owner","last_altered_time","last_modified_time","dist_style","table_subtype"}){
    extended.columns.push_back({name,NativeTypeInfo{ScalarType::VarChar,0,0,true}});for(auto& row:extended.rows)row.push_back(std::nullopt);
  }
  std::reverse(extended.columns.begin(),extended.columns.end());for(auto& row:extended.rows)std::reverse(row.begin(),row.end());
  auto full=normalize_redshift_tables("selected","fixture",table_request(),BackendResult<QueryResult>{extended,spy.snapshot});ASSERT_TRUE(full);EXPECT_EQ(2u,full->rows.size());
}

TEST(RedshiftShowTables, InvalidMetadataBeforeFilteringAndOwningNativeFailureRecover) {
  for(unsigned variant=0;variant<14;++variant){
    auto source=tables_response();auto input=table_request();input.table="never-match";
    switch(variant){
      case 0:source.columns[0].name="unknown";break;
      case 1:source.columns[1].name="database_name";break;
      case 2:source.columns[2].normalized_type.reset();break;
      case 3:source.rows[0][0]="foreign";break;
      case 4:source.rows[0][1]="foreign";break;
      case 5:source.rows[0][2].reset();break;
      case 6:source.rows[1][2]=source.rows[0][2];break;
      case 7:source.rows[0][3]="invented type";break;
      case 8:source.rows[0][4]=std::string("\xf0\x28\x8c\x28",4);break;
      case 9:source.affected_rows=3;break;
      case 10:source.rows.resize(10001,source.rows[0]);break;
      case 11:source.rows[0].pop_back();break;
      case 12:source.statement_kind.reset();break;
      case 13:source.statement_kind=StatementKind::UpdateWhere;break;
    }
    auto result=normalize_redshift_tables("selected","fixture",input,BackendResult<QueryResult>{source,{SessionState::Idle,SessionDisposition::Reusable}});schema_error(result);
  }
  SchemaSpy failed;failed.show=tables_response();BackendError native{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),"synthetic"};native.native_state="42501";
  failed.direct_failure=native;auto error=failed.execute_catalog(table_request(),rs::util::make_deadline(std::chrono::seconds{1}));ASSERT_FALSE(error);EXPECT_EQ("42501",error.backend_error().native_state);EXPECT_EQ(0u,failed.catalog_builds);
  failed.direct_failure.reset();failed.direct_calls=0;
  auto recovered=failed.execute_catalog(table_request(),rs::util::make_deadline(std::chrono::seconds{1}));EXPECT_TRUE(recovered);
  auto deferred=tables_response();QueryResult extra;extra.error=native;deferred.additional_results.push_back(extra);
  auto owning=normalize_redshift_tables("selected","fixture",table_request(),BackendResult<QueryResult>{deferred,failed.snapshot});ASSERT_FALSE(owning);EXPECT_EQ("42501",owning.backend_error().native_state);
}

TEST(RedshiftShowTables, PreDispatchRefusalAndAbsoluteDeadline) {
  for(const auto& capability:{"","3","-4","4x","4294967296"}){
    SchemaSpy spy;spy.capability=capability;EXPECT_TRUE(spy.selects_catalog_request(table_request()));
    auto result=spy.execute_catalog(table_request(),rs::util::make_deadline(std::chrono::seconds{1}));ASSERT_FALSE(result);no_execution(spy);
  }
  for(const auto& pattern:{std::string("bad\\"),std::string("bad\0name",8),std::string("\xff",1)}){
    SchemaSpy spy;auto input=table_request();input.table=pattern;
    auto refused=spy.execute_catalog(input,rs::util::make_deadline(std::chrono::seconds{1}));ASSERT_FALSE(refused);no_execution(spy);
  }
  SchemaSpy expired;auto refused=expired.execute_catalog(table_request(),rs::util::Clock::now());ASSERT_FALSE(refused);no_execution(expired);
  for(unsigned call:{1u,2u}){
    SchemaSpy late;late.show=tables_response();late.before_return=[call](unsigned current,rs::util::Deadline d){if(current==call)while(rs::util::Clock::now()<d)std::this_thread::yield();};
    auto result=late.execute_catalog(table_request(),rs::util::make_deadline(std::chrono::milliseconds{5}));ASSERT_FALSE(result);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout),result.error());EXPECT_EQ(call,late.direct_calls);
  }
}

namespace {
ColumnsCatalogRequest column_request() {return {std::nullopt,"fixture","rows",std::nullopt};}
QueryResult columns_response(std::string database="selected",std::string schema="fixture",std::string table="rows") {
  QueryResult result;
  for(const auto* name:{"database_name","schema_name","table_name","column_name","ordinal_position","column_default",
      "is_nullable","data_type","character_maximum_length","numeric_precision","numeric_scale","remarks"}) {
    const std::string_view field=name;const bool integer=field=="ordinal_position" || field=="character_maximum_length" || field=="numeric_precision" || field=="numeric_scale";
    result.columns.push_back({name,NativeTypeInfo{integer?ScalarType::Integer:ScalarType::VarChar,integer?10u:128u,0,true}});
  }
  result.rows={{database,schema,table,"value","2",std::nullopt,"YES","character varying","32",std::nullopt,std::nullopt,"owning remark"},
      {database,schema,table,"id","1","1","NO","integer",std::nullopt,"32","0",std::nullopt}};
  Message complete;complete.tag='C';complete.payload={std::byte{'S'},std::byte{'H'},std::byte{'O'},std::byte{'W'},std::byte{0}};
  result.statement_kind=PgProtocolParser{}.extract_query_result({complete}).statement_kind;
  return result;
}
}
TEST(RedshiftShowColumns, ExactOwningEighteenFieldsAndOriginalDeadline) {
  SchemaSpy spy;spy.show=columns_response();const auto deadline=rs::util::make_deadline(std::chrono::seconds{2});
  auto result=spy.execute_catalog(column_request(),deadline);ASSERT_TRUE(result);ASSERT_EQ(2u,spy.queries.size());
  EXPECT_EQ("SHOW COLUMNS FROM TABLE \"selected\".\"fixture\".\"rows\";",spy.queries[1]);
  EXPECT_EQ((std::vector<rs::util::Deadline>{deadline,deadline}),spy.deadlines);EXPECT_EQ(0u,spy.prepared_calls);EXPECT_EQ(0u,spy.catalog_builds);
  ASSERT_EQ(18u,result->columns.size());EXPECT_EQ("COLUMN_NAME",result->columns[3].name);EXPECT_EQ("IS_NULLABLE",result->columns[17].name);
  EXPECT_EQ((ResultRows{{"selected","fixture","rows","id","4","integer","10","4","0","10","0",std::nullopt,"1","4",std::nullopt,std::nullopt,"1","NO"},
      {"selected","fixture","rows","value","12","character varying","32","32",std::nullopt,std::nullopt,"1","owning remark",std::nullopt,"12",std::nullopt,"32","2","YES"}}),result->rows);
  for(const auto i:{0u,1u,2u,3u,5u,11u,12u,17u}){ASSERT_TRUE(result->columns[i].normalized_type);EXPECT_EQ(ScalarType::VarChar,result->columns[i].normalized_type->type);EXPECT_EQ(128u,result->columns[i].normalized_type->column_size);}
  spy.show.rows.clear();spy.connected=false;EXPECT_EQ("owning remark",result->rows[1][11]);EXPECT_EQ("1",result->rows[0][12]);
  SchemaSpy empty;empty.show=columns_response();empty.show.rows.clear();auto zero=empty.execute_catalog(column_request(),deadline);ASSERT_TRUE(zero);EXPECT_TRUE(zero->rows.empty());EXPECT_EQ(18u,zero->columns.size());
}
TEST(RedshiftShowColumns, CommonDimensionsUnknownNullAndExplicitTemporalPrecision) {
  auto source=columns_response();source.rows.clear();
  source.rows={{"selected","fixture","rows","amount","1",std::nullopt,"YES","numeric",std::nullopt,"38","6",std::nullopt},
      {"selected","fixture","rows","stamp","2",std::nullopt,"NO","timestamp without time zone (4)",std::nullopt,std::nullopt,std::nullopt,std::nullopt},
      {"selected","fixture","rows","other","3",std::nullopt,std::nullopt,"SUPER",std::nullopt,std::nullopt,std::nullopt,std::nullopt}};
  auto result=normalize_redshift_columns("selected","fixture","rows",column_request(),BackendResult<QueryResult>{source,{SessionState::Idle,SessionDisposition::Reusable}});ASSERT_TRUE(result);
  EXPECT_EQ("38",result->rows[0][6]);EXPECT_EQ("40",result->rows[0][7]);EXPECT_EQ("6",result->rows[0][8]);EXPECT_EQ("10",result->rows[0][9]);
  EXPECT_EQ("93",result->rows[1][4]);EXPECT_EQ("24",result->rows[1][6]);EXPECT_EQ("16",result->rows[1][7]);EXPECT_EQ("4",result->rows[1][8]);EXPECT_EQ("9",result->rows[1][13]);EXPECT_EQ("3",result->rows[1][14]);
  EXPECT_EQ("SUPER",result->rows[2][5]);EXPECT_EQ("0",result->rows[2][4]);EXPECT_FALSE(result->rows[2][6]);EXPECT_FALSE(result->rows[2][7]);EXPECT_EQ("2",result->rows[2][10]);EXPECT_EQ("",result->rows[2][17]);
  struct Sample {const char* type;const char* code;const char* size;const char* buffer;const char* scale;};
  for(const auto& sample:std::array<Sample,10>{{{"boolean","-7","1","1",nullptr},{"smallint","5","5","2","0"},
      {"bigint","-5","19","8","0"},{"real","7","7","4",nullptr},{"double precision","8","15","8",nullptr},
      {"date","91","10","6",nullptr},{"time without time zone","92","15","6","6"},
      {"time with time zone","92","21","6","6"},{"timestamp without time zone","93","26","16","6"},
      {"timestamp with time zone","93","32","16","6"}}}) {
    auto single=columns_response();single.rows.resize(1);single.rows[0][7]=sample.type;
    auto mapped=normalize_redshift_columns("selected","fixture","rows",column_request(),BackendResult<QueryResult>{single,{SessionState::Idle,SessionDisposition::Reusable}});
    ASSERT_TRUE(mapped);EXPECT_EQ(sample.code,mapped->rows[0][4]);EXPECT_EQ(sample.size,mapped->rows[0][6]);EXPECT_EQ(sample.buffer,mapped->rows[0][7]);
    if(sample.scale)EXPECT_EQ(sample.scale,mapped->rows[0][8]);else EXPECT_FALSE(mapped->rows[0][8]);
  }
  for(const auto* name:{"sort_key_type","sort_key","dist_key","encoding","collation"}){source.columns.push_back({name,NativeTypeInfo{ScalarType::VarChar,0,0,true}});for(auto& row:source.rows)row.push_back(std::nullopt);}
  std::reverse(source.columns.begin(),source.columns.end());for(auto& row:source.rows)std::reverse(row.begin(),row.end());
  EXPECT_TRUE(normalize_redshift_columns("selected","fixture","rows",column_request(),BackendResult<QueryResult>{source,{SessionState::Transaction,SessionDisposition::ResetRequired}}));
}
TEST(RedshiftShowColumns, UnicodeQuotedIdentityColumnPatternAndLiteralCatalog) {
  SchemaSpy spy;spy.database="db\"é";auto request=column_request();request.schema="s\\_表";request.table="t\\%\"表";
  spy.show=columns_response(spy.database,"s_表","t%\"表");spy.show.rows[0][3]="é表😀";spy.show.rows[1][3]="i_%";
  request.column="___";request.catalog=spy.database;
  auto result=spy.execute_catalog(request,rs::util::make_deadline(std::chrono::seconds{1}));ASSERT_TRUE(result);EXPECT_EQ(2u,result->rows.size());
  EXPECT_EQ("SHOW COLUMNS FROM TABLE \"db\"\"é\".\"s_表\".\"t%\"\"表\";",spy.queries[1]);
  request.column="i\\_\\%";auto one=normalize_redshift_columns(spy.database,"s_表","t%\"表",request,BackendResult<QueryResult>{spy.show,spy.snapshot});ASSERT_TRUE(one);ASSERT_EQ(1u,one->rows.size());EXPECT_EQ("i_%",one->rows[0][3]);
  request.catalog="db%";auto none=normalize_redshift_columns(spy.database,"s_表","t%\"表",request,BackendResult<QueryResult>{spy.show,spy.snapshot});ASSERT_TRUE(none);EXPECT_TRUE(none->rows.empty());
  request.catalog=spy.database;request.column="";none=normalize_redshift_columns(spy.database,"s_表","t%\"表",request,BackendResult<QueryResult>{spy.show,spy.snapshot});ASSERT_TRUE(none);EXPECT_TRUE(none->rows.empty());
}
TEST(RedshiftShowColumns, MalformedHiddenRowsRefuseNativeFailureAndRecovery) {
  for(unsigned variant=0;variant<15;++variant){auto value=columns_response();auto request=column_request();request.column="hidden";
    switch(variant){case 0:value.columns[0].name="unknown";break;case 1:value.columns[1].name="database_name";break;
      case 2:value.columns[4].normalized_type->type=ScalarType::VarChar;break;case 3:value.rows[0][0]="foreign";break;
      case 4:value.rows[0][2]="foreign";break;case 5:value.rows[0][3].reset();break;case 6:value.rows[0][3]="id";break;
      case 7:value.rows[0][4]="1";break;case 8:value.rows[0][4]="2147483648";break;case 9:value.rows[0][8]="-1";break;
      case 10:value.rows[0][6]="maybe";break;case 11:value.rows[0][11]=std::string("\xff",1);break;
      case 12:value.statement_kind.reset();break;case 13:value.affected_rows=3;break;case 14:value.rows.resize(10001,value.rows[0]);break;}
    schema_error(normalize_redshift_columns("selected","fixture","rows",request,BackendResult<QueryResult>{value,{SessionState::Idle,SessionDisposition::Reusable}}));
  }
  auto bad=columns_response();bad.rows[0][7]="numeric";bad.rows[0][9]="5";bad.rows[0][10]="6";
  schema_error(normalize_redshift_columns("selected","fixture","rows",column_request(),BackendResult<QueryResult>{bad,{SessionState::Idle,SessionDisposition::Reusable}}));
  SchemaSpy denied;denied.show=columns_response();BackendError native{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),"synthetic"};native.native_state="42501";denied.direct_failure=native;
  auto result=denied.execute_catalog(column_request(),rs::util::make_deadline(std::chrono::seconds{1}));ASSERT_FALSE(result);EXPECT_EQ("42501",result.backend_error().native_state);EXPECT_EQ(0u,denied.catalog_builds);
  denied.direct_failure.reset();denied.direct_calls=0;EXPECT_TRUE(denied.execute_catalog(column_request(),rs::util::make_deadline(std::chrono::seconds{1})));
}
TEST(RedshiftShowColumns, NoIoInvalidPatternsCapabilityAndDeadline) {
  for(const auto* capability:{"","3","-4","4x"}){SchemaSpy spy;spy.capability=capability;EXPECT_TRUE(spy.selects_catalog_request(column_request()));auto refused=spy.execute_catalog(column_request(),rs::util::make_deadline(std::chrono::seconds{1}));ASSERT_FALSE(refused);no_execution(spy);}
  for(auto pattern:{std::string("dangling\\"),std::string("bad\0name",8),std::string("\xff",1)}){SchemaSpy spy;auto request=column_request();request.column=pattern;auto refused=spy.execute_catalog(request,rs::util::make_deadline(std::chrono::seconds{1}));ASSERT_FALSE(refused);no_execution(spy);}
  SchemaSpy expired;auto refused=expired.execute_catalog(column_request(),rs::util::Clock::now());ASSERT_FALSE(refused);no_execution(expired);
  for(unsigned call:{1u,2u}){SchemaSpy late;late.show=columns_response();late.before_return=[call](unsigned current,rs::util::Deadline d){if(current==call)while(rs::util::Clock::now()<d)std::this_thread::yield();};auto result=late.execute_catalog(column_request(),rs::util::make_deadline(std::chrono::milliseconds{5}));ASSERT_FALSE(result);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout),result.error());EXPECT_EQ(call,late.direct_calls);}
}


TEST(RedshiftCatalogExecutionTest, IdentifierGeneratedAlternativesNeverRetargetPrimaryKeys) {
  SpySession session;
  TablesCatalogRequest tables{TablesCatalogRequest::Mode::Tables, "db","s","t",std::nullopt};
  ColumnsCatalogRequest columns{"db","s","t","c"};
  tables.name_matches.object = CatalogNameMatch::IdentifierQuoted;
  columns.name_matches.object = CatalogNameMatch::IdentifierQuoted;
  EXPECT_FALSE(session.selects_catalog_request(tables));
  EXPECT_FALSE(session.selects_catalog_request(columns));
  auto refused = session.execute_catalog(tables, rs::util::Deadline::max());
  EXPECT_TRUE(refused.has_error()); EXPECT_EQ(0u, session.direct_calls); EXPECT_EQ(0u, session.prepared_calls);
  auto keys = request(); keys.table = "TABLE'._%";
  keys.name_matches.object = CatalogNameMatch::IdentifierUnquoted;
  EXPECT_TRUE(session.selects_catalog_request(keys));
  session.capability = "3";
  refused = session.execute_catalog(keys, rs::util::Deadline::max());
  EXPECT_TRUE(refused.has_error()); EXPECT_EQ(0u, session.direct_calls); EXPECT_EQ(0u, session.prepared_calls);
  session.capability = "4";
  const auto deadline = rs::util::Clock::now() + std::chrono::seconds(3);
  auto success = session.execute_catalog(keys, deadline);
  ASSERT_FALSE(success.has_error()); ASSERT_EQ(1u, session.prepared_calls);
  EXPECT_EQ("SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;", session.sql);
  ASSERT_EQ(3u, session.parameters.size()); EXPECT_EQ("table'._%", session.parameters[2].value);
  EXPECT_EQ(deadline, session.received_deadline); EXPECT_EQ(0u, session.direct_calls); EXPECT_EQ(0u, session.catalog_builds);
}

TEST(RedshiftCatalogExecutionTest, IdentifierScopedLegacyAndDefaultShowEnumerationKeepExistingGuards) {
  SpySession session;
  auto keys = request(); keys.name_matches.object = CatalogNameMatch::IdentifierQuoted;
  session.mode = RedshiftCatalogMode::Legacy;
  auto result = session.execute_catalog(keys, rs::util::Deadline::max());
  ASSERT_FALSE(result.has_error()); EXPECT_EQ(0u, session.capability_reads);
  EXPECT_EQ(1u, session.prepared_calls); EXPECT_EQ(0u, session.direct_calls);
  EXPECT_NE(std::string::npos, session.sql.find("FROM pg_catalog.pg_namespace"));
  session.mode = RedshiftCatalogMode::Show;
  EXPECT_TRUE(session.selects_catalog_request(schemas_request()));
  keys.name_matches.foreign_catalog = CatalogNameMatch::IdentifierQuoted;
  auto bad = session.execute_catalog(keys, rs::util::Deadline::max());
  EXPECT_TRUE(bad.has_error()); EXPECT_EQ(1u, session.prepared_calls); EXPECT_EQ(0u, session.direct_calls);
}
