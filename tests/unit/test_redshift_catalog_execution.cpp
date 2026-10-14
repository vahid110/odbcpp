#include "core/database/postgres/pg_database_connection.h"
#include <gtest/gtest.h>
#include <array>
#include <algorithm>
#include <functional>
#include "core/database/postgres/redshift_catalog_query.h"

using namespace rs::core::database;
using namespace rs::core::database::postgres;

TEST(RedshiftMetadataDatabaseScope, AuthenticatedFlagsFollowCompleteNativeTruthTable) {
  for (const auto requested : {CatalogDatabaseScope::CurrentDatabase, CatalogDatabaseScope::VisibleDatabases}) {
    for (const auto& share : std::vector<std::optional<std::string>>{std::nullopt, "off", "on"}) {
      for (const auto& external : std::vector<std::optional<std::string>>{std::nullopt, "off", "on"}) {
        auto selected = redshift_catalog_database_scope(requested, share, external);
        ASSERT_TRUE(selected);
        const auto expected = external == std::optional<std::string>{"on"} ||
            (requested == CatalogDatabaseScope::VisibleDatabases && share == std::optional<std::string>{"on"})
            ? CatalogDatabaseScope::VisibleDatabases : CatalogDatabaseScope::CurrentDatabase;
        EXPECT_EQ(expected, *selected);
      }
    }
  }
}

TEST(RedshiftMetadataDatabaseScope, InvalidFlagsAreRedactedEvenWhenOtherFlagDeterminesScopeAndRecoveryIsPure) {
  for (const auto& invalid : std::vector<std::string>{"", "ON", "true", "1", " off", "secret", std::string("on\0", 3)}) {
    auto share = redshift_catalog_database_scope(CatalogDatabaseScope::CurrentDatabase, invalid, "on");
    auto external = redshift_catalog_database_scope(CatalogDatabaseScope::VisibleDatabases, "on", invalid);
    ASSERT_FALSE(share); ASSERT_FALSE(external);
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError), share.error());
    EXPECT_EQ("Invalid authenticated metadata scope parameter", share.error_message());
    EXPECT_EQ(share.error_message(), external.error_message());
  }
  auto invalid_scope = redshift_catalog_database_scope(static_cast<CatalogDatabaseScope>(-1), "on", "on");
  ASSERT_FALSE(invalid_scope);
  auto restored = redshift_catalog_database_scope(CatalogDatabaseScope::VisibleDatabases, "on", "off");
  ASSERT_TRUE(restored); EXPECT_EQ(CatalogDatabaseScope::VisibleDatabases, *restored);
  auto sibling = redshift_catalog_database_scope(CatalogDatabaseScope::CurrentDatabase, "on", "off");
  ASSERT_TRUE(sibling); EXPECT_EQ(CatalogDatabaseScope::CurrentDatabase, *sibling);
  EXPECT_EQ(CatalogDatabaseScope::VisibleDatabases, *restored);
}

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
  std::function<void()> prepared_before_return;
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
    if (prepared_before_return) prepared_before_return();
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
  std::optional<std::string> metadata_share, metadata_external;
  bool foreign_keys{false};
  bool sized_character_metadata{false};
  bool boolean_cells{false};
  bool malformed_boolean_cell{false};
  rs::util::Result<void> connect(std::string_view, uint16_t,
      rs::util::Deadline) override {
    ++connects; input_.clear(); offset_ = 0;
    frame('R', {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}});
    std::vector<std::byte> status;
    text(status, "show_discovery"); text(status, "4"); frame('S', status);
    for (const auto& flag : {std::pair{"datashare_enabled", metadata_share},
                             std::pair{"external_database", metadata_external}}) {
      if (flag.second) {
        status.clear(); text(status, flag.first); text(status, flag.second->c_str()); frame('S', status);
      }
    }
    frame('Z', {std::byte{'I'}});
    frame('1', {});
    std::vector<std::byte> parameters; integer(parameters, 3, 2);
    for (unsigned i = 0; i < 3; ++i) integer(parameters, boolean_cells ? 16 : 25, 4);
    frame('t', parameters); frame('2', {});
    const std::vector<const char*> fields = boolean_cells ? std::vector<const char*>{"flag"} : foreign_keys ? std::vector<const char*>{
        "deferrability", "pk_name", "fk_name", "delete_rule", "update_rule", "key_seq",
        "fk_column_name", "fk_table_name", "fk_schema_name", "fk_database_name",
        "pk_column_name", "pk_table_name", "pk_schema_name", "pk_database_name"} :
        std::vector<const char*>{"database_name", "schema_name", "table_name", "pk_name", "column_name", "key_seq"};
    std::vector<std::byte> description; integer(description, static_cast<uint32_t>(fields.size()), 2);
    for (const auto* name : fields) {
      text(description, name); integer(description, 0, 4); integer(description, 0, 2);
      const bool sequence = std::string_view(name) == "key_seq" ||
          (foreign_keys && (std::string_view(name) == "deferrability" ||
           std::string_view(name) == "delete_rule" || std::string_view(name) == "update_rule"));
      integer(description, boolean_cells ? 16 : sequence ? 21 : 1043, 4);
      integer(description, boolean_cells ? 1 : sequence ? 2 : 65535, 2);
      integer(description, !sequence && sized_character_metadata ? 260u : 0xffffffffu, 4);
      integer(description, 0, 2);
    }
    frame('T', description);
    if (boolean_cells) {
      for (const char* value : {"t", "f", static_cast<const char*>(nullptr)}) {
        std::vector<std::byte> row; integer(row, 1, 2);
        integer(row, value ? 1u : 0xffffffffu, 4);
        if (value) row.push_back(static_cast<std::byte>(*value));
        frame('D', row);
      }
      if (malformed_boolean_cell) {
        std::vector<std::byte> row; integer(row, 1, 2); integer(row, 1, 4);
        row.push_back(std::byte{'x'}); frame('D', row);
      }
    }
    std::vector<std::byte> completion; text(completion, boolean_cells ? (malformed_boolean_cell ? "SELECT 4" : "SELECT 3") : "SELECT 0"); frame('C', completion);
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
    // Omitted-schema routine discovery is now supported in modern mode;
    // retain the no-I/O refusal contract on its legacy path.
    if (std::holds_alternative<ProceduresCatalogRequest>(input) ||
        std::holds_alternative<ProcedureColumnsCatalogRequest>(input))
      spy.mode = RedshiftCatalogMode::Legacy;
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
  EXPECT_TRUE(session.selects_catalog_request(tables));
  EXPECT_TRUE(session.selects_catalog_request(columns));
  session.capability = "3"; // Selected unavailable identifiers refuse without fallback.
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

namespace {
// Literal native-shaped payloads, independent of the normalizer's output.
QueryResult routine_identity() {
  QueryResult r;r.columns={{"database_name",NativeTypeInfo{ScalarType::VarChar,2,0,true}}};
  r.rows={{"db"}};r.statement_kind=StatementKind::SelectCursor;return r;
}
QueryResult routine_discovery(bool function,ResultRows rows) {
  QueryResult r;
  for(const auto* name:{"database_name","schema_name",function?"function_name":"procedure_name",
      "number_of_arguments","argument_list","return_type","remarks"}) {
    r.columns.push_back({name,NativeTypeInfo{std::string_view(name)=="number_of_arguments"?ScalarType::Integer:ScalarType::VarChar,64,0,true}});
  }
  r.rows=std::move(rows);r.statement_kind=StatementKind::Unknown;return r;
}
QueryResult routine_parameters(bool function,ResultRows rows) {
  QueryResult r;
  for(const auto* name:{"database_name","schema_name",function?"function_name":"procedure_name","parameter_name",
      "ordinal_position","parameter_type","data_type","character_maximum_length","numeric_precision","numeric_scale"}) {
    const bool number=std::string_view(name)=="ordinal_position" || std::string_view(name)=="character_maximum_length" ||
        std::string_view(name)=="numeric_precision" || std::string_view(name)=="numeric_scale";
    r.columns.push_back({name,NativeTypeInfo{number?ScalarType::Integer:ScalarType::VarChar,64,0,true}});
  }
  r.rows=std::move(rows);r.statement_kind=StatementKind::Unknown;return r;
}
class RoutineExecutionSession : public PgDatabaseConnection {
 public:
  RoutineExecutionSession():PgDatabaseConnection(std::make_unique<CatalogWireTransport>(),std::nullopt,PgCatalogProfile::Redshift){}
  bool connected{true};SessionSnapshot snapshot{SessionState::Idle,SessionDisposition::Reusable};
  std::string capability{"4"};std::vector<BackendResult<QueryResult>> replies;
  std::vector<std::string> commands;std::vector<rs::util::Deadline> deadlines;
  std::optional<RedshiftCatalogMode> mode;
  std::function<void()> callback;
  bool is_connected() const override{return connected;}
  SessionState session_state() const override{return snapshot.state;}
  RedshiftCatalogMode catalog_mode() const noexcept override{return mode.value_or(RedshiftCatalogMode::Show);}
  std::string get_parameter(std::string_view name) const override{return name=="show_discovery"?capability:std::string{};}
  BackendResult<QueryResult> execute_query(std::string_view query,rs::util::Deadline deadline) override {
    commands.emplace_back(query);deadlines.push_back(deadline);
    if(callback)callback();
    if(replies.empty())return {rs::util::DbErrorCode::QueryFailed,"Unexpected routine fixture dispatch"};
    auto result=std::move(replies.front());replies.erase(replies.begin());return result;
  }
  BackendResult<void> budgets(ResultLimits limits,ResponseLimits response={}) {
    ConnectionSettings settings;settings.host="in-memory";settings.port=5439;settings.database="db";settings.user="fixture";
    settings.use_ssl=false;settings.result_limits=limits;settings.response_limits=response;
    // Configure the real owning budgets through a genuine disconnected startup;
    // do not bypass the production already-open guard or its limit validation.
    struct RestoreFlag {bool& flag;bool prior;~RestoreFlag(){flag=prior;}} restore{connected,connected};
    connected=false;
    return PgDatabaseConnection::connect_until(settings,rs::util::Deadline::max());
  }
  void discovery(ResultRows procedures,ResultRows functions={}) {
    replies.emplace_back(routine_identity(),snapshot);
    replies.emplace_back(routine_discovery(false,std::move(procedures)),snapshot);
    replies.emplace_back(routine_discovery(true,std::move(functions)),snapshot);
  }
};
}

TEST(RedshiftRoutineExecution, ExactSchemaTypedUnknownDimensionsRefcursorAndReturnFirst) {
  RoutineExecutionSession s;
  s.discovery({{"db","s","cursor_proc","2","integer, refcursor","refcursor","owning"}},
      {{"db","s","f","4","numeric, numeric, numeric, numeric","character varying",std::nullopt}});
  s.replies.emplace_back(routine_parameters(false,{
      {"db","s","cursor_proc","id","1","IN","integer",std::nullopt,"32","0"},
      {"db","s","cursor_proc","cursor","2","INOUT","refcursor",std::nullopt,std::nullopt,std::nullopt},
      {"db","s","cursor_proc","out","3","OUT","character varying","50",std::nullopt,std::nullopt}}),s.snapshot);
  s.replies.emplace_back(routine_parameters(true,{
      {"db","s","f","partial","2","IN","numeric",std::nullopt,"18",std::nullopt},
      {"db","s","f","unknown","1","IN","numeric",std::nullopt,std::nullopt,std::nullopt},
      {"db","s","f","scale_only","3","IN","numeric",std::nullopt,std::nullopt,"4"},
      {"db","s","f","exact","4","IN","numeric",std::nullopt,"18","0"},
      {"db","s","f","","0","RETURN","character varying","-1",std::nullopt,std::nullopt}}),s.snapshot);
  const auto d=rs::util::Deadline::max();
  auto result=s.execute_catalog(ProcedureColumnsCatalogRequest{std::nullopt,"s",std::nullopt,std::nullopt},d);
  ASSERT_TRUE(result);ASSERT_EQ(19u,result->columns.size());ASSERT_EQ(8u,result->rows.size());
  const std::vector<std::string> expected{"SELECT current_database() AS database_name",
      "SHOW PROCEDURES FROM SCHEMA \"db\".\"s\";","SHOW FUNCTIONS FROM SCHEMA \"db\".\"s\";",
      "SHOW PARAMETERS OF PROCEDURE \"db\".\"s\".\"cursor_proc\"(integer, refcursor);",
      "SHOW PARAMETERS OF FUNCTION \"db\".\"s\".\"f\"(numeric, numeric, numeric, numeric);"};
  EXPECT_EQ(expected,s.commands);for(const auto deadline:s.deadlines){EXPECT_EQ(d,deadline);}
  EXPECT_EQ(ResultCell{"10"},result->rows[0][7]);EXPECT_EQ(ResultCell{"4"},result->rows[0][8]);
  EXPECT_EQ(ResultCell{"0"},result->rows[1][5]);EXPECT_EQ(ResultCell{"refcursor"},result->rows[1][6]);EXPECT_FALSE(result->rows[1][7]);
  EXPECT_EQ(ResultCell{"4"},result->rows[2][4]);EXPECT_EQ(ResultCell{"50"},result->rows[2][7]);
  EXPECT_EQ(ResultCell{"5"},result->rows[3][4]);EXPECT_EQ(ResultCell{"0"},result->rows[3][17]);EXPECT_FALSE(result->rows[3][7]);
  EXPECT_EQ(ResultCell{"2"},result->rows[4][5]);EXPECT_EQ(ResultCell{"numeric"},result->rows[4][6]);
  EXPECT_FALSE(result->rows[4][7]);EXPECT_FALSE(result->rows[4][8]);EXPECT_FALSE(result->rows[4][9]);EXPECT_EQ(ResultCell{"10"},result->rows[4][10]);
  EXPECT_EQ(ResultCell{"18"},result->rows[5][7]);EXPECT_EQ(ResultCell{"20"},result->rows[5][8]);EXPECT_FALSE(result->rows[5][9]);
  EXPECT_FALSE(result->rows[6][7]);EXPECT_FALSE(result->rows[6][8]);EXPECT_EQ(ResultCell{"4"},result->rows[6][9]);
  EXPECT_EQ(ResultCell{"18"},result->rows[7][7]);EXPECT_EQ(ResultCell{"20"},result->rows[7][8]);EXPECT_EQ(ResultCell{"0"},result->rows[7][9]);
  EXPECT_EQ(ResultCell{"10"},result->rows[7][10]);
}

TEST(RedshiftRoutineExecution, IdentifierPatternsOverloadsEmptyAndUnsupportedPaths) {
  for(const auto& input:std::vector<CatalogRequest>{ProceduresCatalogRequest{std::nullopt,std::nullopt,std::nullopt},
      ProceduresCatalogRequest{std::nullopt,"s%",std::nullopt}}) {
    RoutineExecutionSession s;s.mode=RedshiftCatalogMode::Legacy;EXPECT_TRUE(s.selects_catalog_request(input));
    const auto result=s.execute_catalog(input,rs::util::Deadline::max());ASSERT_FALSE(result);
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature),result.error());EXPECT_TRUE(s.commands.empty());
  }
  RoutineExecutionSession empty;auto result=empty.execute_catalog(ProceduresCatalogRequest{std::nullopt,"",std::nullopt},rs::util::Deadline::max());
  ASSERT_TRUE(result);EXPECT_EQ(8u,result->columns.size());EXPECT_TRUE(result->rows.empty());EXPECT_TRUE(empty.commands.empty());
  // Explicit catalogs are direct context authority even with the default current-only setting.
  RoutineExecutionSession elsewhere;
  elsewhere.replies.emplace_back(routine_discovery(false, {}), elsewhere.snapshot);
  elsewhere.replies.emplace_back(routine_discovery(true, {}), elsewhere.snapshot);
  result=elsewhere.execute_catalog(ProceduresCatalogRequest{"elsewhere","s",std::nullopt},rs::util::Deadline::max());
  ASSERT_TRUE(result);EXPECT_TRUE(result->rows.empty());
  EXPECT_EQ((std::vector<std::string>{"SHOW PROCEDURES FROM SCHEMA \"elsewhere\".\"s\";",
      "SHOW FUNCTIONS FROM SCHEMA \"elsewhere\".\"s\";"}), elsewhere.commands);
  RoutineExecutionSession empty_catalog;
  result=empty_catalog.execute_catalog(ProceduresCatalogRequest{"","s",std::nullopt},rs::util::Deadline::max());
  ASSERT_TRUE(result);EXPECT_TRUE(result->rows.empty());EXPECT_TRUE(empty_catalog.commands.empty());
  RoutineExecutionSession s;s.discovery({{"db","s_%","p","1","integer",std::nullopt,"first"},
      {"db","s_%","p","1","numeric",std::nullopt,"second"}});
  result=s.execute_catalog(ProceduresCatalogRequest{std::nullopt,"s\\_\\%","p"},rs::util::Deadline::max());
  ASSERT_TRUE(result);ASSERT_EQ(2u,result->rows.size());EXPECT_EQ(ResultCell{"first"},result->rows[0][6]);EXPECT_EQ(ResultCell{"second"},result->rows[1][6]);
  EXPECT_EQ("SHOW PROCEDURES FROM SCHEMA \"db\".\"s_%\";",s.commands[1]);
  for(int control=0;control<3;++control) {
    RoutineExecutionSession refused;
    if(control==0)refused.mode=RedshiftCatalogMode::Legacy;
    if(control==1)refused.capability="3";
    if(control==2)refused.snapshot.state=SessionState::Transaction;
    EXPECT_FALSE(refused.execute_catalog(ProceduresCatalogRequest{std::nullopt,"s",std::nullopt},rs::util::Deadline::max()));
    EXPECT_TRUE(refused.commands.empty());
  }
}

TEST(RedshiftRoutineExecution, SignatureDuplicateForeignBudgetAndTimeoutFailUnpublished) {
  for(int malformed=0;malformed<3;++malformed) {
    RoutineExecutionSession s;
    ResultRows rows{{"db","s","p","1","integer",std::nullopt,"same"}};
    if(malformed==0)rows[0][4]="integer); SELECT secret";
    if(malformed==1)rows.push_back({"db","s","p","1","integer",std::nullopt,"different"});
    if(malformed==2)rows[0][0]="foreign";
    s.discovery(std::move(rows));
    const auto result=s.execute_catalog(ProcedureColumnsCatalogRequest{std::nullopt,"s",std::nullopt,std::nullopt},rs::util::Deadline::max());
    ASSERT_FALSE(result);EXPECT_EQ(2u,s.commands.size());EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);
  }
  RoutineExecutionSession limited;ResultLimits limits;limits.max_results=2;ASSERT_TRUE(limited.budgets(limits));
  EXPECT_FALSE(limited.execute_catalog(ProceduresCatalogRequest{std::nullopt,"s",std::nullopt},rs::util::Deadline::max()));EXPECT_TRUE(limited.commands.empty());
  RoutineExecutionSession fanout;limits.max_results=3;ASSERT_TRUE(fanout.budgets(limits));
  fanout.discovery({{"db","s","p","0","",std::nullopt,std::nullopt}});
  const auto result=fanout.execute_catalog(ProcedureColumnsCatalogRequest{std::nullopt,"s",std::nullopt,std::nullopt},rs::util::Deadline::max());
  ASSERT_FALSE(result);EXPECT_EQ(3u,fanout.commands.size());EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),result.error());
  for(int bound=0;bound<3;++bound) {
    RoutineExecutionSession capped;ResultLimits local;ResponseLimits response;
    if(bound==0)local.max_rows=1;
    if(bound==1)local.max_cells=7;
    if(bound==2)response.max_wire_bytes=32;
    ASSERT_TRUE(capped.budgets(local,response));
    capped.discovery({{"db","s","p","0","",std::nullopt,"retained"}});
    const auto refusal=capped.execute_catalog(ProceduresCatalogRequest{std::nullopt,"s",std::nullopt},rs::util::Deadline::max());
    ASSERT_FALSE(refusal);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),refusal.error());
    EXPECT_LE(capped.commands.size(),2u);
  }
  RoutineExecutionSession expired;const auto timeout=expired.execute_catalog(ProceduresCatalogRequest{std::nullopt,"s",std::nullopt},rs::util::Clock::now()-std::chrono::seconds{1});
  ASSERT_FALSE(timeout);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout),timeout.error());EXPECT_TRUE(expired.commands.empty());
}

TEST(RedshiftRoutineExecution, OwningFailureCloseRefillAndParameterValidationPreserveOriginalDeadline) {
  RoutineExecutionSession s;s.discovery({{"db","s","p","0","",std::nullopt,"original"}});
  auto result=s.execute_catalog(ProceduresCatalogRequest{std::nullopt,"s",std::nullopt},rs::util::Deadline::max());
  ASSERT_TRUE(result);ASSERT_EQ(1u,result->rows.size());const auto snapshot=*result;
  BackendError denied{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),"owned native denial"};
  denied.native_state="42501";denied.operation=BackendOperation::ExecuteCatalog;denied.session_state=SessionState::Idle;denied.disposition=SessionDisposition::Reusable;
  s.replies.clear();s.replies.emplace_back(denied);
  result=s.execute_catalog(ProceduresCatalogRequest{std::nullopt,"s",std::nullopt},rs::util::Deadline::max());
  ASSERT_FALSE(result);EXPECT_EQ("42501",result.backend_error().native_state);EXPECT_EQ("owned native denial",result.error_message());
  EXPECT_EQ(ResultCell{"original"},snapshot.rows[0][6]);
  s.replies.clear();s.discovery({{"db","s","p","0","",std::nullopt,"refilled"}});
  result=s.execute_catalog(ProceduresCatalogRequest{std::nullopt,"s",std::nullopt},rs::util::Deadline::max());ASSERT_TRUE(result);
  EXPECT_EQ(ResultCell{"refilled"},result->rows[0][6]);EXPECT_EQ(ResultCell{"original"},snapshot.rows[0][6]);
  RoutineExecutionSession dimensions;dimensions.discovery({},{{"db","s","f","1","numeric","numeric",std::nullopt}});
  dimensions.replies.emplace_back(routine_parameters(true,{{"db","s","f","x","1","IN","numeric",std::nullopt,"2","3"}}),dimensions.snapshot);
  const auto finite=rs::util::make_deadline(std::chrono::seconds{5});
  const auto invalid=dimensions.execute_catalog(ProcedureColumnsCatalogRequest{std::nullopt,"s",std::nullopt,std::nullopt},finite);
  for(const auto d:dimensions.deadlines){EXPECT_EQ(finite,d);}
  ASSERT_FALSE(invalid);EXPECT_EQ(BackendErrorClass::InvalidMetadata,invalid.backend_error().error_class);
  for(int malformed=0;malformed<4;++malformed) {
    RoutineExecutionSession bad;bad.discovery({},{{"db","s","f","1","integer","integer",std::nullopt}});
    auto parameters=routine_parameters(true,{{"db","s","f","x","1","IN","integer",std::nullopt,"32","0"}});
    if(malformed==0)parameters.rows[0][4]="0";
    if(malformed==1)parameters.rows[0][5]="INVALID";
    if(malformed==2)parameters.columns[0].name="wrong_header";
    if(malformed==3)parameters.rows[0][2]="foreign";
    bad.replies.emplace_back(std::move(parameters),bad.snapshot);
    const auto rejected=bad.execute_catalog(ProcedureColumnsCatalogRequest{std::nullopt,"s",std::nullopt,std::nullopt},finite);
    ASSERT_FALSE(rejected);EXPECT_EQ(BackendErrorClass::InvalidMetadata,rejected.backend_error().error_class);EXPECT_EQ(4u,bad.commands.size());
  }
}


namespace {
ForeignKeysCatalogRequest imported_request() {
  return {std::nullopt, std::nullopt, std::nullopt, "db", "s", "child"};
}
QueryResult foreign_key_response() {
  QueryResult r;
  const std::array<const char*,14> names{"pk_database_name","pk_schema_name","pk_table_name","pk_column_name",
      "fk_database_name","fk_schema_name","fk_table_name","fk_column_name","key_seq","update_rule",
      "delete_rule","fk_name","pk_name","deferrability"};
  for (std::size_t i=0;i<names.size();++i) {
    const bool small=i==8 || i==9 || i==10 || i==13;
    r.columns.push_back({names[i],NativeTypeInfo{small?ScalarType::SmallInt:ScalarType::VarChar,0,0,true}});
  }
  r.normalized_parameter_types.assign(3,NativeTypeInfo{ScalarType::VarChar,0,0,true});
  r.rows={{"db","s","z_parent","b","db","s","child","rb","2",std::nullopt,std::nullopt,"fk_z",std::nullopt,std::nullopt},
      {"db","s","a_parent","a","db","s","child","ra","1",std::nullopt,std::nullopt,"fk_a","pk_a",std::nullopt},
      {"db","s","z_parent","a","db","s","child","ra","1",std::nullopt,std::nullopt,"fk_z",std::nullopt,std::nullopt}};
  std::reverse(r.columns.begin(),r.columns.end());
  for (auto& row:r.rows) {std::reverse(row.begin(),row.end());}
  return r;
}
}
TEST(RedshiftForeignKeyExecution, ImportedExportedBothUseOneOwningPreparedShowAndOdbcOrder) {
  const auto deadline=rs::util::make_deadline(std::chrono::seconds{2});
  for (unsigned direction=0;direction<3;++direction) {
    SpySession spy;spy.response=foreign_key_response();auto input=imported_request();
    if (direction!=0) {input.primary_catalog="db";input.primary_schema="s";input.primary_table="z_parent";}
    if (direction==1) {
      input.foreign_catalog.reset();input.foreign_schema.reset();input.foreign_table.reset();
      // Exported response is restricted to the selected primary, but spans foreign groups.
      spy.response.rows[0][7]="z_child";spy.response.rows[2][7]="z_child";
      spy.response.rows[1][7]="a_child";spy.response.rows[1][11]="z_parent";
    }
    ASSERT_TRUE(spy.selects_catalog_request(input));
    auto result=spy.execute_catalog(input,deadline);ASSERT_TRUE(result);
    ASSERT_EQ(1u,spy.prepared_calls);EXPECT_EQ(0u,spy.direct_calls);EXPECT_EQ(0u,spy.catalog_builds);
    EXPECT_EQ(direction==1?"SHOW CONSTRAINTS FOREIGN KEYS EXPORTED FROM TABLE ?.?.?;":
        "SHOW CONSTRAINTS FOREIGN KEYS FROM TABLE ?.?.?;",spy.sql);
    ASSERT_EQ(3u,spy.parameters.size());EXPECT_EQ("db",spy.parameters[0].value);EXPECT_EQ("s",spy.parameters[1].value);
    EXPECT_EQ(direction==1?"z_parent":"child",spy.parameters[2].value);
    for (const auto& value:spy.parameters) {EXPECT_EQ(QueryParameterType::Unspecified,value.type);}
    EXPECT_EQ(deadline,spy.received_deadline);ASSERT_EQ(14u,result->columns.size());
    ASSERT_EQ(direction==2?2u:3u,result->rows.size());
    EXPECT_EQ(direction==0?"a_parent":"z_parent",result->rows[0][2]);
    EXPECT_EQ(direction==1?"a_child":"child",result->rows[0][6]);
    EXPECT_EQ("1",result->rows[0][8]);EXPECT_EQ("3",result->rows[0][9]);EXPECT_EQ("7",result->rows[0][13]);
    EXPECT_EQ("2",result->rows.back()[8]);EXPECT_TRUE(result->normalized_parameter_types.empty());
    spy.response.rows.clear();spy.connected=false;EXPECT_EQ("ra",result->rows[0][7]);
  }
}
TEST(RedshiftForeignKeyExecution, PassiveSelectionRefusesUnavailableExactInputsBeforeIoThenRecovers) {
  for (unsigned variant=0;variant<11;++variant) {
    SpySession spy;auto input=imported_request();spy.response=foreign_key_response();
    if (variant==0) spy.mode=RedshiftCatalogMode::Legacy;
    if (variant==1) spy.capability="3";
    if (variant==2) spy.capability="+4";
    if (variant==3) spy.capability.clear();
    if (variant==4) spy.snapshot={SessionState::Transaction,SessionDisposition::ResetRequired};
    if (variant==5) spy.snapshot={SessionState::Unknown,SessionDisposition::Retire};
    if (variant==6) input.foreign_catalog.reset();
    if (variant==7) input.foreign_schema="";
    if (variant==8) input.foreign_table="";
    if (variant==9) input.foreign_table=std::string("bad\0name",8);
    if (variant==10) input.name_matches.member=CatalogNameMatch::IdentifierQuoted;
    EXPECT_TRUE(spy.selects_catalog_request(input));auto refused=spy.execute_catalog(input,rs::util::Deadline::max());
    ASSERT_FALSE(refused);no_execution(spy);
    EXPECT_EQ(rs::util::make_error_code(variant>=9?rs::util::DbErrorCode::InvalidParameter:
        rs::util::DbErrorCode::UnsupportedFeature),refused.error());
    spy.mode=RedshiftCatalogMode::Show;spy.capability="4";spy.snapshot={SessionState::Idle,SessionDisposition::Reusable};
    auto repaired=spy.execute_catalog(imported_request(),rs::util::Deadline::max());ASSERT_TRUE(repaired);EXPECT_EQ(1u,spy.prepared_calls);
  }
  SpySession pg(PgCatalogProfile::PostgreSQL);EXPECT_FALSE(pg.selects_catalog_request(imported_request()));
  auto refused=pg.execute_catalog(imported_request(),rs::util::Deadline::max());ASSERT_FALSE(refused);no_execution(pg);
}
TEST(RedshiftForeignKeyExecution, AllGroupsValidateBeforeBothFilterAndNativeFailureRemainsOwning) {
  SpySession spy;spy.response=foreign_key_response();auto input=imported_request();
  input.primary_catalog="db";input.primary_schema="s";input.primary_table="never_match";
  spy.response.rows[0][5]="0"; // Reversed native header: KEY_SEQ at index5.
  auto malformed=spy.execute_catalog(input,rs::util::Deadline::max());ASSERT_FALSE(malformed);
  EXPECT_EQ(BackendErrorClass::InvalidMetadata,malformed.backend_error().error_class);EXPECT_EQ(1u,spy.prepared_calls);
  BackendError denied{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),"owning denied"};
  denied.native_state="42501";denied.operation=BackendOperation::ExecuteCatalog;
  denied.session_state=SessionState::Idle;denied.disposition=SessionDisposition::Reusable;spy.failure=denied;
  auto failed=spy.execute_catalog(imported_request(),rs::util::Deadline::max());ASSERT_FALSE(failed);
  spy.failure->message="mutated";EXPECT_EQ("owning denied",failed.error_message());EXPECT_EQ("42501",failed.backend_error().native_state);
  spy.failure.reset();spy.response=foreign_key_response();auto recovered=spy.execute_catalog(imported_request(),rs::util::Deadline::max());ASSERT_TRUE(recovered);
  SpySession expired;auto no_result=expired.execute_catalog(imported_request(),rs::util::Clock::now());ASSERT_FALSE(no_result);no_execution(expired);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout),no_result.error());
}
TEST(RedshiftForeignKeyExecution, RealPreparedWireCarriesThreeTextInputsAndReorderedFourteenColumns) {
  for (bool exported:{false,true}) {
    auto transport=std::make_unique<CatalogWireTransport>();auto* wire=transport.get();wire->foreign_keys=true;
    PgDatabaseConnection session(std::move(transport),std::nullopt,PgCatalogProfile::Redshift);
    ConnectionSettings settings;settings.host="in-memory.invalid";settings.port=5439;settings.database="db";
    settings.user="fixture";settings.use_ssl=false;ASSERT_TRUE(session.connect(settings));
    auto input=imported_request();
    if (exported) {input={"db","s","parent",std::nullopt,std::nullopt,std::nullopt};}
    const auto deadline=rs::util::make_deadline(std::chrono::seconds{2});const auto reads=wire->read_deadlines.size();
    auto result=session.execute_catalog(input,deadline);ASSERT_TRUE(result);ASSERT_EQ(14u,result->columns.size());
    EXPECT_TRUE(result->rows.empty());EXPECT_TRUE(result->normalized_parameter_types.empty());ASSERT_EQ(2u,wire->writes.size());
    const auto& packet=wire->writes[1];ASSERT_GE(packet.size(),8u);ASSERT_EQ(std::byte{'P'},packet[0]);
    const auto tags=catalog_packet_tags(packet);ASSERT_TRUE(tags);EXPECT_EQ("PDBDES",*tags);
    const auto end=std::find(packet.begin()+6,packet.end(),std::byte{0});ASSERT_NE(packet.end(),end);
    std::string sql;for(auto it=packet.begin()+6;it!=end;++it){sql.push_back(static_cast<char>(*it));}
    EXPECT_EQ(exported?"SHOW CONSTRAINTS FOREIGN KEYS EXPORTED FROM TABLE $1.$2.$3;":
        "SHOW CONSTRAINTS FOREIGN KEYS FROM TABLE $1.$2.$3;",sql);
    EXPECT_EQ(deadline,wire->write_deadlines[1]);
    for(std::size_t i=reads;i<wire->read_deadlines.size();++i){EXPECT_EQ(deadline,wire->read_deadlines[i]);}
    session.disconnect();EXPECT_EQ(1u,wire->closes);EXPECT_EQ("PKTABLE_CAT",result->columns[0].name);
  }
}
TEST(RedshiftIdentifierShow, TablesAndColumnsMatchEscapedLiteralRouteWithoutMutatingNamesOrDimensions) {
  const auto deadline=rs::util::make_deadline(std::chrono::seconds{2});
  const std::string schema="s_%\\表",table="t_%\\表",column="c_%\\表";
  TablesCatalogRequest identifier{TablesCatalogRequest::Mode::Tables,"selected",schema,table,std::nullopt};
  identifier.name_matches.schema=CatalogNameMatch::IdentifierQuoted;identifier.name_matches.object=CatalogNameMatch::IdentifierQuoted;
  SchemaSpy exact,pattern;exact.show=tables_response("selected",schema);exact.show.rows.resize(1);exact.show.rows[0][2]=table;
  pattern.show=exact.show;
  for(auto& metadata:exact.show.columns){metadata.normalized_type->column_size=65535;}
  pattern.show=exact.show;
  auto lowered=redshift_identifier_show_request(CatalogRequest{identifier});ASSERT_TRUE(lowered);
  auto left=exact.execute_catalog(identifier,deadline),right=pattern.execute_catalog(*lowered,deadline);
  ASSERT_TRUE(left);ASSERT_TRUE(right);EXPECT_EQ(left->rows,right->rows);EXPECT_EQ(5u,left->columns.size());
  for(std::size_t i=0;i<left->columns.size();++i){EXPECT_EQ(left->columns[i].normalized_type->column_size,right->columns[i].normalized_type->column_size);EXPECT_EQ(65535u,left->columns[i].normalized_type->column_size);}
  EXPECT_EQ(exact.queries,pattern.queries);EXPECT_EQ(schema,identifier.schema);EXPECT_EQ(table,identifier.table);
  EXPECT_EQ("SHOW TABLES FROM SCHEMA \"selected\".\"s_%\\表\";",exact.queries[1]);
  ColumnsCatalogRequest cols{"selected",schema,table,column};cols.name_matches.schema=CatalogNameMatch::IdentifierQuoted;
  cols.name_matches.object=CatalogNameMatch::IdentifierQuoted;cols.name_matches.member=CatalogNameMatch::IdentifierQuoted;
  SchemaSpy cexact,cpattern;cexact.show=columns_response("selected",schema,table);cexact.show.rows.resize(1);cexact.show.rows[0][3]=column;cpattern.show=cexact.show;
  auto clowered=redshift_identifier_show_request(CatalogRequest{cols});ASSERT_TRUE(clowered);
  auto cleft=cexact.execute_catalog(cols,deadline),cright=cpattern.execute_catalog(*clowered,deadline);ASSERT_TRUE(cleft);ASSERT_TRUE(cright);
  EXPECT_EQ(cleft->rows,cright->rows);ASSERT_EQ(18u,cleft->columns.size());
  for(std::size_t i=0;i<cleft->columns.size();++i){EXPECT_EQ(cleft->columns[i].normalized_type->column_size,cright->columns[i].normalized_type->column_size);}
  EXPECT_EQ(cexact.queries,cpattern.queries);EXPECT_EQ(column,cols.column);EXPECT_EQ(0u,cexact.catalog_builds);
}
TEST(RedshiftIdentifierShow, UnsupportedIdentityModesAndNonmatchingFiltersNeverFallback) {
  TablesCatalogRequest input{TablesCatalogRequest::Mode::Tables,"selected","fixture","absent",std::nullopt};
  input.name_matches.object=CatalogNameMatch::IdentifierQuoted;
  for(unsigned variant=0;variant<5;++variant){SchemaSpy spy;auto names=input;
    if (variant == 0) { spy.mode = RedshiftCatalogMode::Legacy; }
    if (variant == 1) { spy.capability = "3"; }
    if (variant == 2) { names.schema.reset(); }
    if (variant == 3) { names.schema = ""; }
    if(variant==4)spy.snapshot={SessionState::Transaction,SessionDisposition::ResetRequired};
    EXPECT_TRUE(spy.selects_catalog_request(names));auto result=spy.execute_catalog(names,rs::util::Deadline::max());ASSERT_FALSE(result);no_execution(spy);
  }
  SchemaSpy empty;empty.show=tables_response();auto result=empty.execute_catalog(input,rs::util::Deadline::max());ASSERT_TRUE(result);EXPECT_TRUE(result->rows.empty());EXPECT_EQ(5u,result->columns.size());
}

TEST(RedshiftForeignKeyExecution, OriginalDeadlineAlsoBoundsReturnedNormalizationWithoutRetry) {
  SpySession late;late.response=foreign_key_response();
  const auto deadline=rs::util::make_deadline(std::chrono::milliseconds{5});
  late.prepared_before_return=[deadline]{while(rs::util::Clock::now()<deadline){std::this_thread::yield();}};
  auto refused=late.execute_catalog(imported_request(),deadline);ASSERT_FALSE(refused);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout),refused.error());
  EXPECT_EQ(1u,late.prepared_calls);EXPECT_EQ(deadline,late.received_deadline);EXPECT_EQ(0u,late.direct_calls);
  SpySession native;native.response=foreign_key_response();
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),"owning native first"};
  error.native_state="42501";error.session_state=SessionState::Idle;error.disposition=SessionDisposition::Reusable;
  native.failure=error;const auto original=rs::util::make_deadline(std::chrono::milliseconds{5});
  native.prepared_before_return=[original]{while(rs::util::Clock::now()<original){std::this_thread::yield();}};
  auto failed=native.execute_catalog(imported_request(),original);ASSERT_FALSE(failed);
  EXPECT_EQ("42501",failed.backend_error().native_state);EXPECT_EQ("owning native first",failed.error_message());
  EXPECT_EQ(1u,native.prepared_calls);EXPECT_EQ(0u,native.direct_calls);
}

TEST(RedshiftForeignKeyExecution, QuotedUnicodeLiteralNamesAndUnquotedAsciiFoldAreNeverPatterns) {
  for (bool quoted:{false,true}) {
    SpySession spy;spy.response=foreign_key_response();auto input=imported_request();
    input.foreign_catalog="DB";input.foreign_schema="S_%\\表";
    input.foreign_table=quoted?"Child_%\\表":"CHILD_%\\表";
    const auto match=quoted?CatalogNameMatch::IdentifierQuoted:CatalogNameMatch::IdentifierUnquoted;
    input.name_matches.foreign_catalog=match;input.name_matches.foreign_schema=match;input.name_matches.foreign_object=match;
    const std::string database=quoted?"DB":"db",schema=quoted?"S_%\\表":"s_%\\表",table=quoted?"Child_%\\表":"child_%\\表";
    for(auto& row:spy.response.rows){row[9]=database;row[8]=schema;row[7]=table;}
    auto result=spy.execute_catalog(input,rs::util::Deadline::max());ASSERT_TRUE(result);
    ASSERT_EQ(3u,spy.parameters.size());EXPECT_EQ(database,spy.parameters[0].value);EXPECT_EQ(schema,spy.parameters[1].value);EXPECT_EQ(table,spy.parameters[2].value);
    EXPECT_EQ("DB",input.foreign_catalog);EXPECT_EQ("S_%\\表",input.foreign_schema);
    ASSERT_EQ(3u,result->rows.size());EXPECT_EQ(table,result->rows[0][6]);
  }
}

TEST(RedshiftForeignKeyExecution, NativeDescriptionLimitsRefuseBeforeAnyCatalogPublication) {
  auto transport=std::make_unique<CatalogWireTransport>();auto* wire=transport.get();wire->foreign_keys=true;
  PgDatabaseConnection session(std::move(transport),std::nullopt,PgCatalogProfile::Redshift);
  ConnectionSettings settings;settings.host="in-memory.invalid";settings.port=5439;
  settings.database="db";settings.user="fixture";settings.use_ssl=false;
  settings.result_limits.max_columns_per_description=13;
  ASSERT_TRUE(session.connect(settings));
  auto result=session.execute_catalog(imported_request(),rs::util::make_deadline(std::chrono::seconds{2}));
  ASSERT_FALSE(result);ASSERT_EQ(2u,wire->writes.size());
  EXPECT_EQ(SessionDisposition::Retire,result.session_snapshot().disposition);
  session.disconnect();EXPECT_FALSE(session.is_connected());
}

TEST(RedshiftCharacterMetadata, RealParserPreparedResultsCapturePolicyButParameterDescriptionsStayNative) {
  auto transport = std::make_unique<CatalogWireTransport>(); auto* wire = transport.get();
  wire->sized_character_metadata = true;
  PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
  ConnectionSettings settings; settings.host = "in-memory.invalid"; settings.port = 5439;
  settings.database = "fixture"; settings.user = "fixture"; settings.use_ssl = false;
  settings.character_metadata = {255, 17};
  ASSERT_TRUE(session.connect(settings)); settings.character_metadata = {};
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{3});
  const std::vector<QueryParameter> parameters{{"db", QueryParameterType::Text},
      {"schema", QueryParameterType::Text}, {"table", QueryParameterType::Text}};
  auto result = session.execute_prepared("SELECT ?, ?, ?", parameters, deadline);
  ASSERT_TRUE(result); ASSERT_EQ(6u, result->columns.size());
  for (std::size_t i = 0; i < 5; ++i) {
    ASSERT_TRUE(result->columns[i].normalized_type);
    const auto& native = *result->columns[i].normalized_type;
    EXPECT_EQ(ScalarType::LongVarChar, native.type); EXPECT_EQ(17u, native.column_size);
    EXPECT_EQ(ScalarType::VarChar, native.declared_type); EXPECT_EQ(256u, native.declared_column_size);
  }
  EXPECT_EQ(ScalarType::SmallInt, result->columns.back().normalized_type->type);
  ASSERT_EQ(3u, result->normalized_parameter_types.size());
  for (const auto& parameter : result->normalized_parameter_types) {
    EXPECT_EQ(ScalarType::VarChar, parameter.type); EXPECT_EQ(0u, parameter.column_size);
    EXPECT_FALSE(parameter.declared_type);
  }
  const auto snapshot = result->columns.front().normalized_type;
  session.disconnect(); ASSERT_TRUE(session.connect(settings));
  auto uncapped = session.execute_prepared("SELECT ?, ?, ?", parameters, deadline);
  ASSERT_TRUE(uncapped);
  EXPECT_EQ(ScalarType::VarChar, uncapped->columns.front().normalized_type->type);
  EXPECT_EQ(256u, uncapped->columns.front().normalized_type->column_size);
  EXPECT_FALSE(uncapped->columns.front().normalized_type->declared_type);
  EXPECT_EQ(17u, snapshot->column_size); EXPECT_EQ(ScalarType::LongVarChar, snapshot->type);
  session.disconnect(); EXPECT_FALSE(session.is_connected()); EXPECT_EQ(2u, wire->connects);
}

TEST(RedshiftCharacterMetadata, InvalidDisabledSizingRefusesBeforeActualTransportAndFreshSettingsRecover) {
  auto transport = std::make_unique<CatalogWireTransport>(); auto* wire = transport.get();
  PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
  ConnectionSettings settings; settings.host = "in-memory.invalid"; settings.port = 5439;
  settings.database = "fixture"; settings.user = "fixture"; settings.use_ssl = false;
  settings.character_metadata = {0, 65536};
  auto refused = session.connect(settings); ASSERT_FALSE(refused);
  EXPECT_EQ(0u, wire->connects); EXPECT_TRUE(wire->writes.empty()); EXPECT_FALSE(session.is_connected());
  settings.character_metadata = {255, 65535}; ASSERT_TRUE(session.connect(settings));
  EXPECT_EQ(1u, wire->connects); session.disconnect(); EXPECT_FALSE(session.is_connected());
}

TEST(RedshiftBooleanCharacter, RealCodecKeepsCanonicalValuesNativeParametersOwnedReconnectAndCellErrors) {
  auto transport = std::make_unique<CatalogWireTransport>(); auto* wire = transport.get();
  wire->boolean_cells = true; wire->malformed_boolean_cell = true;
  PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
  ConnectionSettings settings; settings.host = "in-memory.invalid"; settings.port = 5439;
  settings.database = "fixture"; settings.user = "fixture"; settings.use_ssl = false;
  settings.character_metadata = {0, 17, true};
  ASSERT_TRUE(session.connect(settings)); settings.character_metadata = {};
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{3});
  const std::vector<QueryParameter> parameters{{"1", QueryParameterType::Boolean},
      {"0", QueryParameterType::Boolean}, {std::nullopt, QueryParameterType::Boolean}};
  auto result = session.execute_prepared("SELECT ?, ?, ?", parameters, deadline);
  ASSERT_TRUE(result); ASSERT_EQ(1u, result->columns.size()); ASSERT_EQ(4u, result->rows.size());
  ASSERT_EQ(1u, result->cell_errors.size()); EXPECT_EQ(3u, result->cell_errors[0].row);
  EXPECT_EQ(ScalarType::VarChar, result->columns[0].normalized_type->type);
  EXPECT_EQ(ScalarType::Boolean, result->columns[0].normalized_type->declared_type);
  EXPECT_EQ(1u, result->columns[0].normalized_type->column_size);
  EXPECT_EQ("1", result->rows[0][0]); EXPECT_EQ("0", result->rows[1][0]); EXPECT_FALSE(result->rows[2][0]);
  ASSERT_EQ(3u, result->normalized_parameter_types.size());
  for (const auto& type : result->normalized_parameter_types) { EXPECT_EQ(ScalarType::Boolean, type.type); EXPECT_FALSE(type.declared_type); }
  const auto retained = *result->columns[0].normalized_type;
  session.disconnect(); wire->malformed_boolean_cell = true; ASSERT_TRUE(session.connect(settings));
  auto disabled = session.execute_prepared("SELECT ?, ?, ?", parameters, deadline); ASSERT_TRUE(disabled);
  EXPECT_EQ(ScalarType::Boolean, disabled->columns[0].normalized_type->type);
  EXPECT_EQ("1", disabled->rows[0][0]); EXPECT_EQ("0", disabled->rows[1][0]);
  ASSERT_EQ(1u, disabled->cell_errors.size()); EXPECT_EQ(3u, disabled->cell_errors[0].row);
  EXPECT_EQ(0u, disabled->cell_errors[0].column); EXPECT_EQ(ScalarType::VarChar, retained.type);
  session.disconnect(); EXPECT_EQ(2u, wire->connects); EXPECT_FALSE(session.is_connected());
}

TEST(RedshiftBooleanCharacter, DirectOrdinarySettingsRefuseBeforeTransportCreationOrWrites) {
  auto transport = std::make_unique<CatalogWireTransport>(); auto* wire = transport.get();
  PgDatabaseConnection session(std::move(transport));
  ConnectionSettings settings; settings.host = "in-memory.invalid"; settings.port = 5432;
  settings.user = "fixture"; settings.database = "fixture"; settings.use_ssl = false;
  settings.character_metadata.boolean_as_character = true;
  auto refused = session.connect(settings); ASSERT_FALSE(refused);
  EXPECT_EQ(0u, wire->connects); EXPECT_TRUE(wire->writes.empty());
  EXPECT_EQ("BoolsAsChar requires Redshift", refused.error_message());
  settings.character_metadata.boolean_as_character = false; ASSERT_TRUE(session.connect(settings));
  session.disconnect(); EXPECT_EQ(1u, wire->connects);
}

TEST(RedshiftTableTypePresentation, GenericFiltersRetainExternalSystemTemporaryTablesAndStableIdentities) {
  auto source = tables_response(); source.rows.clear();
  unsigned index = 0;
  for (const auto type : catalog_table_types())
    source.rows.push_back({"selected", "fixture", "table_" + std::to_string(index++), std::string(type), std::nullopt, std::nullopt});
  auto input = table_request(); input.types = std::vector<std::string>{"TABLE"};
  auto generic = normalize_redshift_tables("selected", "fixture", input,
      BackendResult<QueryResult>{source, {SessionState::Idle, SessionDisposition::Reusable}}, false);
  ASSERT_TRUE(generic); ASSERT_EQ(4u, generic->rows.size()); ASSERT_EQ(5u, generic->columns.size());
  for (const auto& row : generic->rows) {
    EXPECT_EQ("selected", row[0]); EXPECT_EQ("fixture", row[1]); EXPECT_EQ("TABLE", row[3]); EXPECT_FALSE(row[4]);
  }
  EXPECT_EQ("table_0", generic->rows[0][2]); // external survives the TABLE filter.
  auto detailed = normalize_redshift_tables("selected", "fixture", input,
      BackendResult<QueryResult>{source, {SessionState::Idle, SessionDisposition::Reusable}}, true);
  ASSERT_TRUE(detailed); ASSERT_EQ(1u, detailed->rows.size()); EXPECT_EQ("table_4", detailed->rows[0][2]);
  input.types = std::vector<std::string>{"VIEW"};
  auto views = normalize_redshift_tables("selected", "fixture", input,
      BackendResult<QueryResult>{source, {SessionState::Idle, SessionDisposition::Reusable}}, false);
  ASSERT_TRUE(views); ASSERT_EQ(2u, views->rows.size()); EXPECT_EQ("table_3", views->rows[0][2]);
  input.types = std::vector<std::string>{"EXTERNAL TABLE"};
  auto none = normalize_redshift_tables("selected", "fixture", input,
      BackendResult<QueryResult>{source, {SessionState::Idle, SessionDisposition::Reusable}}, false);
  ASSERT_TRUE(none); EXPECT_TRUE(none->rows.empty());
  input.types = std::vector<std::string>{"TABLE"}; source.rows.back()[3] = "secret unknown";
  auto malformed = normalize_redshift_tables("selected", "fixture", input,
      BackendResult<QueryResult>{source, {SessionState::Idle, SessionDisposition::Reusable}}, false);
  schema_error(malformed); // A nonmatching row is validated before filtering.
}

TEST(RedshiftTableTypePresentation, RealSessionCapturesPolicyAndLegacyQueryClassifiesBeforeFilterAndEnumeratesExactKinds) {
  auto transport = std::make_unique<CatalogWireTransport>(); auto* wire = transport.get();
  PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
  ConnectionSettings settings; settings.host = "in-memory.invalid"; settings.port = 5439;
  settings.database = "fixture"; settings.user = "fixture"; settings.use_ssl = false;
  settings.redshift_catalog_mode = RedshiftCatalogMode::Legacy; settings.detailed_table_types = false;
  ASSERT_TRUE(session.connect(settings)); settings.detailed_table_types = true;
  TablesCatalogRequest enumeration; enumeration.mode = TablesCatalogRequest::Mode::TableTypes;
  auto generic = session.catalog_query(enumeration); ASSERT_TRUE(generic);
  EXPECT_NE(std::string::npos, generic->find("CAST('TABLE' AS VARCHAR(15))"));
  EXPECT_NE(std::string::npos, generic->find("CAST('VIEW' AS VARCHAR(15))"));
  EXPECT_EQ(std::string::npos, generic->find("SYSTEM VIEW"));
  auto input = table_request(); input.types = std::vector<std::string>{"TABLE"};
  auto filter = session.catalog_query(input); ASSERT_TRUE(filter);
  const auto classify = filter->find("CASE WHEN table_type IN ('VIEW', 'SYSTEM VIEW')");
  const auto where = filter->find("WHERE 1=1");
  EXPECT_NE(std::string::npos, classify); EXPECT_LT(classify, where);
  EXPECT_NE(std::string::npos, filter->find("AND table_type IN ('TABLE')"));
  EXPECT_NE(std::string::npos, filter->find("ELSE table_type END::text")); // Unknowns aren't silently classified.
  const std::string retained = *generic;
  session.disconnect(); ASSERT_TRUE(session.connect(settings));
  auto full = session.catalog_query(enumeration); ASSERT_TRUE(full);
  for (const auto type : catalog_table_types()) EXPECT_NE(std::string::npos, full->find("CAST('" + std::string(type) + "' AS VARCHAR(15))"));
  EXPECT_EQ(std::string::npos, full->find("FOREIGN TABLE")); EXPECT_EQ(*generic, retained);
  session.disconnect(); EXPECT_EQ(2u, wire->connects);
}

namespace {
class MetadataScopeQuerySession final : public PgDatabaseConnection {
 public:
  MetadataScopeQuerySession() : PgDatabaseConnection(nullptr, std::nullopt, PgCatalogProfile::Redshift) {}
  std::optional<std::string> share, external;
  std::optional<std::string> get_parameter_status(std::string_view key) const override {
    if (key == "datashare_enabled") return share;
    if (key == "external_database") return external;
    ADD_FAILURE() << "Unexpected metadata scope parameter";
    return std::nullopt;
  }
};
QueryResult visible_databases() {
  QueryResult result;
  for (const auto* name : {"database_name", "database_owner", "database_type", "database_acl",
                          "parameters", "database_isolation_level"})
    result.columns.push_back({name, NativeTypeInfo{ScalarType::VarChar, 128, 0, true}});
  result.rows = {{"z_db", "1", "local", std::nullopt, std::nullopt, "Snapshot Isolation"},
                 {"a_雪", "2", "shared", std::nullopt, std::nullopt, std::nullopt}};
  result.statement_kind = StatementKind::Unknown;
  return result;
}
}

TEST(RedshiftMetadataDatabaseScope, DatabaseDiscoveryValidatesWholeOwningNamedShapeAndKeepsNativeErrors) {
  auto source = visible_databases();
  std::swap(source.columns[0], source.columns[2]);
  for (auto& row : source.rows) std::swap(row[0], row[2]);
  auto names = normalize_redshift_databases({source, {SessionState::Idle, SessionDisposition::Reusable}});
  ASSERT_TRUE(names); ASSERT_EQ(2u, names->size());
  EXPECT_EQ("a_雪", (*names)[0]); EXPECT_EQ("z_db", (*names)[1]);
  source.rows.clear(); EXPECT_EQ(2u, names->size());
  const auto original = visible_databases();
  for (int variant = 0; variant != 7; ++variant) {
    auto bad = original;
    if (variant == 0) bad.rows.push_back(bad.rows.front());
    if (variant == 1) bad.rows.front()[0].reset();
    if (variant == 2) bad.rows.front()[0] = "";
    if (variant == 3) bad.columns[1].name = "database_name";
    if (variant == 4) bad.rows.front()[0] = std::string("bad\0db", 6);
    if (variant == 5) bad.rows.front()[0] = std::string("\xff", 1);
    if (variant == 6) bad.affected_rows = 3;
    auto refused = normalize_redshift_databases({bad, {SessionState::Idle, SessionDisposition::Reusable}});
    ASSERT_FALSE(refused); EXPECT_EQ(BackendErrorClass::InvalidMetadata, refused.backend_error().error_class);
  }
  auto retired = normalize_redshift_databases({original, {SessionState::Idle, SessionDisposition::Retire}});
  EXPECT_FALSE(retired);
  BackendError denied{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "denied"};
  denied.native_state = "42501";
  auto failed = normalize_redshift_databases(BackendResult<QueryResult>{denied});
  ASSERT_FALSE(failed); EXPECT_EQ("42501", failed.backend_error().native_state);
  EXPECT_EQ("denied", failed.error_message());
}

TEST(RedshiftMetadataDatabaseScope, NativeViewsPreserveCrossDatabaseNamesDimensionsAndOwningSelection) {
  MetadataScopeQuerySession session;
  TablesCatalogRequest catalogs; catalogs.mode = TablesCatalogRequest::Mode::Catalogs;
  auto current = session.catalog_query(catalogs); ASSERT_TRUE(current);
  EXPECT_NE(std::string::npos, current->find("current_database()"));
  session.external = "on";
  auto visible = session.catalog_query(catalogs); ASSERT_TRUE(visible);
  EXPECT_NE(std::string::npos, visible->find("svv_redshift_databases"));
  EXPECT_EQ(std::string::npos, visible->find("current_database()"));
  TablesCatalogRequest schemas; schemas.mode = TablesCatalogRequest::Mode::Schemas;
  auto all_schemas = session.catalog_query(schemas); ASSERT_TRUE(all_schemas);
  EXPECT_NE(std::string::npos, all_schemas->find("database_name::text AS table_cat"));
  EXPECT_NE(std::string::npos, all_schemas->find("svv_all_schemas"));
  auto tables = table_request(); tables.catalog = "db'_%";
  auto all_tables = session.catalog_query(tables); ASSERT_TRUE(all_tables);
  EXPECT_NE(std::string::npos, all_tables->find("svv_all_tables"));
  EXPECT_NE(std::string::npos, all_tables->find("remarks::text AS remarks"));
  EXPECT_NE(std::string::npos, all_tables->find("AND table_cat LIKE 'db''_%'"));
  tables.types = std::vector<std::string>{"TABLE"};
  auto filtered_tables = session.catalog_query(tables); ASSERT_TRUE(filtered_tables);
  EXPECT_NE(std::string::npos, filtered_tables->find("AND (table_type IN ('TABLE') OR table_type IS NULL OR table_type NOT IN"));
  tables.types = std::vector<std::string>{};
  auto empty_filter = session.catalog_query(tables); ASSERT_TRUE(empty_filter);
  EXPECT_NE(std::string::npos, empty_filter->find("AND (FALSE OR table_type IS NULL OR table_type NOT IN"));
  ColumnsCatalogRequest columns; columns.catalog = "雪'_%"; columns.schema = "same"; columns.table = "table";
  auto all_columns = session.catalog_query(columns); ASSERT_TRUE(all_columns);
  EXPECT_NE(std::string::npos, all_columns->find("svv_all_columns"));
  EXPECT_NE(std::string::npos, all_columns->find("database_name AS table_catalog"));
  EXPECT_NE(std::string::npos, all_columns->find("numeric_precision, numeric_scale"));
  EXPECT_NE(std::string::npos, all_columns->find("NULL::integer AS datetime_precision"));
  EXPECT_NE(std::string::npos, all_columns->find("AND table_cat = '雪''_%'"));
  session.external = "off";
  auto restored = session.catalog_query(columns); ASSERT_TRUE(restored);
  EXPECT_NE(std::string::npos, restored->find("FROM svv_columns AS columns"));
  EXPECT_NE(std::string::npos, all_columns->find("svv_all_columns"));
  session.external = "";
  auto malformed = session.catalog_query(columns); ASSERT_FALSE(malformed);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError), malformed.error());
  session.external.reset(); ASSERT_TRUE(session.catalog_query(columns));
}

TEST(RedshiftMetadataDatabaseScope, InvariantEmptyApisAndLocalRowIdentityHaveExplicitScopeDisposition) {
  MetadataScopeQuerySession session; session.external = "on";
  StatisticsCatalogRequest statistics; statistics.table = "table";
  auto empty_statistics = session.catalog_query(statistics); ASSERT_TRUE(empty_statistics);
  EXPECT_NE(std::string::npos, empty_statistics->find("WHERE 1=0"));
  SpecialColumnsCatalogRequest special; special.table = "table";
  special.identifier = SpecialColumnsCatalogRequest::Identifier::RowVersion;
  auto row_version = session.catalog_query(special); ASSERT_TRUE(row_version);
  EXPECT_NE(std::string::npos, row_version->find("WHERE 1=0"));
  special.identifier = SpecialColumnsCatalogRequest::Identifier::RowIdentity;
  auto partial = session.catalog_query(special); ASSERT_FALSE(partial);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature), partial.error());
  session.external = "off";
  auto current = session.catalog_query(special); ASSERT_TRUE(current);
  EXPECT_NE(std::string::npos, current->find("pg_catalog"));
}

namespace {
class MetadataDatabaseExecutionSession final : public RoutineExecutionSession {
 public:
  std::optional<std::string> external{"on"}, share;
  std::optional<std::string> get_parameter_status(std::string_view key) const override {
    return key == "external_database" ? external : share;
  }
  void schemas() {
    replies.emplace_back(visible_databases(), snapshot);
    replies.emplace_back(schemas_response("a_雪"), snapshot);
    replies.emplace_back(schemas_response("z_db"), snapshot);
  }
};
}

TEST(RedshiftMetadataDatabaseFanout, CatalogsAndCollidingSchemasStayOwningAndUseOneDeadline) {
  MetadataDatabaseExecutionSession session;
  TablesCatalogRequest catalogs; catalogs.mode = TablesCatalogRequest::Mode::Catalogs;
  session.replies.emplace_back(visible_databases(), session.snapshot);
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{2});
  EXPECT_TRUE(session.selects_catalog_request(catalogs));
  auto result = session.execute_catalog(catalogs, deadline); ASSERT_TRUE(result);
  EXPECT_EQ((ResultRows{{"a_雪", std::nullopt, std::nullopt, std::nullopt, std::nullopt},
                       {"z_db", std::nullopt, std::nullopt, std::nullopt, std::nullopt}}), result->rows);
  EXPECT_EQ((std::vector<std::string>{"SHOW DATABASES;"}), session.commands);
  session.commands.clear(); session.deadlines.clear(); session.schemas();
  auto schemas = session.execute_catalog(schemas_request(), deadline); ASSERT_TRUE(schemas);
  EXPECT_EQ((ResultRows{{"a_雪", "a", std::nullopt, std::nullopt, std::nullopt},
                       {"a_雪", "z", std::nullopt, std::nullopt, std::nullopt},
                       {"z_db", "a", std::nullopt, std::nullopt, std::nullopt},
                       {"z_db", "z", std::nullopt, std::nullopt, std::nullopt}}), schemas->rows);
  EXPECT_EQ((std::vector<std::string>{"SHOW DATABASES;", "SHOW SCHEMAS FROM DATABASE \"a_雪\";",
                                    "SHOW SCHEMAS FROM DATABASE \"z_db\";"}), session.commands);
  EXPECT_EQ((std::vector<rs::util::Deadline>{deadline, deadline, deadline}), session.deadlines);
  session.replies.clear(); session.connected = false;
  EXPECT_EQ("a_雪", result->rows[0][0]); EXPECT_EQ("z_db", schemas->rows[3][0]);
}

TEST(RedshiftMetadataDatabaseFanout, ExactIdentifierAndLiteralColumnCatalogSelectBeforeContextExchange) {
  MetadataDatabaseExecutionSession session;
  session.replies.emplace_back(visible_databases(), session.snapshot);
  session.replies.emplace_back(tables_response("z_db"), session.snapshot);
  auto tables = table_request(); tables.catalog = "z_db";
  tables.name_matches.catalog = CatalogNameMatch::IdentifierQuoted;
  tables.name_matches.schema = CatalogNameMatch::IdentifierQuoted;
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{2});
  auto selected = session.execute_catalog(tables, deadline); ASSERT_TRUE(selected);
  ASSERT_EQ(2u, selected->rows.size()); EXPECT_EQ("z_db", selected->rows[0][0]);
  EXPECT_EQ((std::vector<std::string>{"SHOW DATABASES;", "SHOW TABLES FROM SCHEMA \"z_db\".\"fixture\";"}), session.commands);
  session.commands.clear(); session.deadlines.clear();
  session.replies.emplace_back(visible_databases(), session.snapshot);
  session.replies.emplace_back(columns_response("a_雪"), session.snapshot);
  auto columns = column_request(); columns.catalog = "a_雪";
  auto described = session.execute_catalog(columns, deadline); ASSERT_TRUE(described);
  EXPECT_EQ((std::vector<std::string>{"SHOW DATABASES;", "SHOW COLUMNS FROM TABLE \"a_雪\".\"fixture\".\"rows\";"}), session.commands);
  ASSERT_EQ(2u, described->rows.size()); EXPECT_EQ("a_雪", described->rows[0][0]);
  EXPECT_EQ("10", described->rows[0][6]); EXPECT_EQ("32", described->rows[1][6]);
  EXPECT_EQ("owning remark", described->rows[1][11]);
  EXPECT_EQ("z_db", selected->rows[0][0]);
}

TEST(RedshiftMetadataDatabaseFanout, CumulativeExchangeLimitRefusesUnpublishedPartialRowsAndRetainsDeadline) {
  MetadataDatabaseExecutionSession session;
  ResultLimits limits; limits.max_results = 2; ASSERT_TRUE(session.budgets(limits));
  session.schemas();
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{2});
  auto refused = session.execute_catalog(schemas_request(), deadline); ASSERT_FALSE(refused);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit), refused.error());
  ASSERT_EQ(2u, session.commands.size());
  EXPECT_EQ((std::vector<rs::util::Deadline>{deadline, deadline}), session.deadlines);
  EXPECT_EQ(1u, session.replies.size()); // The next context was never dispatched.
}

TEST(RedshiftMetadataDatabaseFanout, LaterNativeFailureHasNoFallbackOrPartialPublicationAndFreshRequestRecovers) {
  MetadataDatabaseExecutionSession session;
  session.replies.emplace_back(visible_databases(), session.snapshot);
  session.replies.emplace_back(schemas_response("a_雪"), session.snapshot);
  BackendError denied{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owning later denial"};
  denied.native_state = "42501";
  session.replies.emplace_back(denied);
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{2});
  auto failed = session.execute_catalog(schemas_request(), deadline); ASSERT_FALSE(failed);
  EXPECT_EQ("42501", failed.backend_error().native_state);
  EXPECT_EQ("owning later denial", failed.error_message());
  EXPECT_EQ(3u, session.commands.size()); EXPECT_TRUE(session.replies.empty());
  session.schemas();
  auto recovered = session.execute_catalog(schemas_request(), deadline); ASSERT_TRUE(recovered);
  EXPECT_EQ(4u, recovered->rows.size()); EXPECT_EQ(6u, session.commands.size());
  EXPECT_EQ("owning later denial", failed.error_message());
}

TEST(RedshiftMetadataDatabaseFanout, ExpiredDeadlineAndMalformedAuthenticatedFlagDispatchNothing) {
  MetadataDatabaseExecutionSession session;
  auto expired = session.execute_catalog(schemas_request(), rs::util::Clock::now() - std::chrono::milliseconds{1});
  ASSERT_FALSE(expired); EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout), expired.error());
  EXPECT_TRUE(session.commands.empty());
  session.external = "";
  auto malformed = session.execute_catalog(schemas_request(), rs::util::make_deadline(std::chrono::seconds{2}));
  ASSERT_FALSE(malformed); EXPECT_EQ(SessionDisposition::Retire, malformed.backend_error().disposition);
  EXPECT_TRUE(session.commands.empty());
}

TEST(RedshiftMetadataDatabaseFanout, RoutineCollisionsAndParametersKeepTheirCatalogContext) {
  MetadataDatabaseExecutionSession session;
  session.replies.emplace_back(visible_databases(), session.snapshot);
  for (const auto* database : {"a_雪", "z_db"}) {
    session.replies.emplace_back(routine_discovery(false, {{database, "s", "same", "1", "integer", std::nullopt, "owning"}}), session.snapshot);
    session.replies.emplace_back(routine_discovery(true, {}), session.snapshot);
    session.replies.emplace_back(routine_parameters(false, {{database, "s", "same", "id", "1", "IN", "integer", std::nullopt, "32", "0"}}), session.snapshot);
  }
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{2});
  auto result = session.execute_catalog(ProcedureColumnsCatalogRequest{std::nullopt, "s", "same", std::nullopt}, deadline);
  ASSERT_TRUE(result); ASSERT_EQ(2u, result->rows.size());
  EXPECT_EQ("a_雪", result->rows[0][0]); EXPECT_EQ("z_db", result->rows[1][0]);
  EXPECT_EQ("same", result->rows[0][2]); EXPECT_EQ("same", result->rows[1][2]);
  EXPECT_EQ("10", result->rows[0][7]); EXPECT_EQ("integer", result->rows[1][6]);
  ASSERT_EQ(7u, session.commands.size()); EXPECT_EQ("SHOW DATABASES;", session.commands[0]);
  EXPECT_EQ("SHOW PARAMETERS OF PROCEDURE \"a_雪\".\"s\".\"same\"(integer);", session.commands[3]);
  EXPECT_EQ("SHOW PARAMETERS OF PROCEDURE \"z_db\".\"s\".\"same\"(integer);", session.commands[6]);
  for (const auto value : session.deadlines) EXPECT_EQ(deadline, value);
  session.connected = false; EXPECT_EQ("a_雪", result->rows[0][0]);
}

TEST(RedshiftMetadataDatabaseFanout, LaterRoutineFailureSuppressesTheUnpublishedFirstCatalog) {
  MetadataDatabaseExecutionSession session;
  session.replies.emplace_back(visible_databases(), session.snapshot);
  session.replies.emplace_back(routine_discovery(false, {{"a_雪", "s", "same", "0", "", std::nullopt, "owning"}}), session.snapshot);
  session.replies.emplace_back(routine_discovery(true, {}), session.snapshot);
  BackendError denied{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "second catalog denied"};
  denied.native_state = "42501"; session.replies.emplace_back(denied);
  auto result = session.execute_catalog(ProceduresCatalogRequest{std::nullopt, "s", std::nullopt}, rs::util::Deadline::max());
  ASSERT_FALSE(result); EXPECT_EQ("42501", result.backend_error().native_state);
  EXPECT_EQ("second catalog denied", result.error_message()); EXPECT_EQ(4u, session.commands.size());
}

TEST(RedshiftMetadataDatabaseScope, RealAuthenticatedParameterStoragePreservesAbsenceEmptyOwningSettingsAndReconnect) {
  auto transport = std::make_unique<CatalogWireTransport>(); auto* wire = transport.get();
  PgDatabaseConnection session(std::move(transport), std::nullopt, PgCatalogProfile::Redshift);
  ConnectionSettings settings; settings.host = "in-memory.invalid"; settings.port = 5439;
  settings.database = "fixture"; settings.user = "fixture"; settings.use_ssl = false;
  settings.redshift_catalog_mode = RedshiftCatalogMode::Legacy;
  settings.catalog_database_scope = CatalogDatabaseScope::VisibleDatabases;
  TablesCatalogRequest input; input.mode = TablesCatalogRequest::Mode::Catalogs;
  ASSERT_TRUE(session.connect(settings));
  settings.catalog_database_scope = CatalogDatabaseScope::CurrentDatabase;
  auto absent = session.catalog_query(input); ASSERT_TRUE(absent);
  EXPECT_NE(std::string::npos, absent->find("current_database()"));
  const std::string retained = *absent;
  session.disconnect();
  wire->metadata_share = "on"; wire->metadata_external = "off";
  settings.catalog_database_scope = CatalogDatabaseScope::VisibleDatabases;
  ASSERT_TRUE(session.connect(settings));
  settings.catalog_database_scope = CatalogDatabaseScope::CurrentDatabase;
  auto shared = session.catalog_query(input); ASSERT_TRUE(shared);
  EXPECT_NE(std::string::npos, shared->find("svv_redshift_databases"));
  EXPECT_EQ(retained, *absent); const auto startup_writes = wire->writes.size();
  session.disconnect(); wire->metadata_share = ""; wire->metadata_external = "on";
  ASSERT_TRUE(session.connect(settings));
  auto empty = session.catalog_query(input); ASSERT_FALSE(empty);
  EXPECT_EQ("Invalid authenticated metadata scope parameter", empty.error_message());
  session.disconnect(); wire->metadata_share.reset(); wire->metadata_external = "on";
  ASSERT_TRUE(session.connect(settings));
  auto external = session.catalog_query(input); ASSERT_TRUE(external);
  EXPECT_NE(std::string::npos, external->find("svv_redshift_databases"));
  session.disconnect(); wire->metadata_external.reset();
  ASSERT_TRUE(session.connect(settings));
  auto restored = session.catalog_query(input); ASSERT_TRUE(restored); EXPECT_EQ(retained, *restored);
  EXPECT_EQ(5u, wire->connects); EXPECT_GT(wire->writes.size(), startup_writes);
  session.disconnect();
}

TEST(RedshiftMetadataDatabaseScope, CurrentCatalogEnumerationKeepsGeneratedTransactionRouteVisibleAndMalformedSelectGuardedRoute) {
  MetadataScopeQuerySession session;
  TablesCatalogRequest catalogs; catalogs.mode = TablesCatalogRequest::Mode::Catalogs;
  EXPECT_FALSE(session.selects_catalog_request(catalogs));
  auto current = session.catalog_query(catalogs); ASSERT_TRUE(current);
  EXPECT_NE(std::string::npos, current->find("current_database()"));
  session.external = "on"; EXPECT_TRUE(session.selects_catalog_request(catalogs));
  session.external = "off"; EXPECT_FALSE(session.selects_catalog_request(catalogs));
  session.external = ""; EXPECT_TRUE(session.selects_catalog_request(catalogs));
  auto invalid = session.catalog_query(catalogs); EXPECT_FALSE(invalid);
  session.external.reset(); EXPECT_FALSE(session.selects_catalog_request(catalogs));
}

namespace {
// Target-contract fixtures: native field names/cells are literal inputs, never
// constructed by the production normalizers. These tests precede the repair.
QueryResult routine_schema_rows(ResultRows rows) {
  auto result = schemas_response("db");
  result.rows = std::move(rows);
  result.statement_kind = StatementKind::Unknown; // Completed SHOW, not guessed SELECT.
  return result;
}
class RoutineSchemaSession final : public RoutineExecutionSession {
 public:
  unsigned prepared_fallbacks{0};
  mutable unsigned catalog_fallbacks{0};
  BackendResult<QueryResult> execute_prepared(std::string_view,
      std::span<const QueryParameter>, rs::util::Deadline) override {
    ++prepared_fallbacks;
    return {rs::util::DbErrorCode::QueryFailed, "Unexpected prepared catalog fallback"};
  }
  rs::util::Result<std::string> catalog_query(const CatalogRequest&) const override {
    ++catalog_fallbacks;
    return {rs::util::DbErrorCode::UnsupportedFeature, "Unexpected generated catalog fallback"};
  }
};
void routine_schema_no_fallback(const RoutineSchemaSession& session) {
  EXPECT_EQ(0u, session.prepared_fallbacks);
  EXPECT_EQ(0u, session.catalog_fallbacks);
}
}

TEST(RedshiftRoutineSchemaPatterns, OmittedAndWildcardSchemasDiscoverOnlyMatchingIdentities) {
  struct Selection {
    std::optional<std::string> pattern;
    std::vector<std::string> matches;
  };
  const std::vector<Selection> selections{
      {std::nullopt, {"a", "s_%", "s_雪", "z"}},
      {"s%", {"s_%", "s_雪"}}, {"s_雪", {"s_雪"}},
      {"s\\_%", {"s_%", "s_雪"}}, {"missing%", {}}};
  for (const auto& selection : selections) {
    SCOPED_TRACE(selection.pattern.value_or("<omitted>"));
    RoutineSchemaSession session;
    session.replies.emplace_back(routine_identity(), session.snapshot);
    session.replies.emplace_back(routine_schema_rows({
        {"db", "z", "1", "local", std::nullopt, std::nullopt, std::nullopt},
        {"db", "s_雪", "1", "local", std::nullopt, std::nullopt, std::nullopt},
        {"db", "a", "1", "local", std::nullopt, std::nullopt, std::nullopt},
        {"db", "s_%", "1", "local", std::nullopt, std::nullopt, std::nullopt}}), session.snapshot);
    std::vector<std::string> expected{"SELECT current_database() AS database_name",
        "SHOW SCHEMAS FROM DATABASE \"db\";"};
    for (const auto& schema : selection.matches) {
      session.replies.emplace_back(routine_discovery(false,
          {{"db", schema, "p", "0", "", std::nullopt, "owned"}}), session.snapshot);
      session.replies.emplace_back(routine_discovery(true, {}), session.snapshot);
      expected.push_back("SHOW PROCEDURES FROM SCHEMA \"db\".\"" + schema + "\";");
      expected.push_back("SHOW FUNCTIONS FROM SCHEMA \"db\".\"" + schema + "\";");
    }
    const auto deadline = rs::util::make_deadline(std::chrono::seconds{3});
    auto result = session.execute_catalog(
        ProceduresCatalogRequest{std::nullopt, selection.pattern, "p"}, deadline);
    ASSERT_TRUE(result); // Literal baseline failure expected at c3 before repair.
    ASSERT_EQ(8u, result->columns.size());
    ASSERT_EQ(selection.matches.size(), result->rows.size());
    EXPECT_EQ(expected, session.commands);
    EXPECT_EQ(std::vector<rs::util::Deadline>(expected.size(), deadline), session.deadlines);
    EXPECT_TRUE(session.replies.empty());
    routine_schema_no_fallback(session);
    for (std::size_t row = 0; row < selection.matches.size(); ++row) {
      EXPECT_EQ(ResultCell{"db"}, result->rows[row][0]);
      EXPECT_EQ(ResultCell{selection.matches[row]}, result->rows[row][1]);
      EXPECT_EQ(ResultCell{"p"}, result->rows[row][2]);
    }
    session.connected = false; session.replies.clear();
    if (!result->rows.empty()) {
      EXPECT_EQ(ResultCell{"owned"}, result->rows[0][6]);
    }
  }
}

TEST(RedshiftRoutineSchemaPatterns, ProcedureColumnsKeepUnknownTypesAndPartialDimensionsAcrossSchemas) {
  RoutineSchemaSession session;
  // Explicit database intent skips current_database discovery.
  session.replies.emplace_back(routine_schema_rows({
      {"db", "z", "1", "local", std::nullopt, std::nullopt, std::nullopt},
      {"db", "a", "1", "local", std::nullopt, std::nullopt, std::nullopt}}), session.snapshot);
  for (const auto* schema : {"a", "z"}) {
    session.replies.emplace_back(routine_discovery(false,
        {{"db", schema, "same", "0", "", std::nullopt, std::nullopt}}), session.snapshot);
    session.replies.emplace_back(routine_discovery(true, {}), session.snapshot);
    session.replies.emplace_back(routine_parameters(false, {{"db", schema, "same", "out", "1", "OUT",
        std::string_view(schema) == "a" ? "super" : "numeric", std::nullopt,
        std::string_view(schema) == "a" ? ResultCell{} : ResultCell{"18"}, std::nullopt}}), session.snapshot);
  }
  auto result = session.execute_catalog(ProcedureColumnsCatalogRequest{"db", std::nullopt, "same", "o%"},
      rs::util::Deadline::max());
  ASSERT_TRUE(result); ASSERT_EQ(19u, result->columns.size()); ASSERT_EQ(2u, result->rows.size());
  EXPECT_EQ((std::vector<std::string>{"SHOW SCHEMAS FROM DATABASE \"db\";",
      "SHOW PROCEDURES FROM SCHEMA \"db\".\"a\";", "SHOW FUNCTIONS FROM SCHEMA \"db\".\"a\";",
      "SHOW PARAMETERS OF PROCEDURE \"db\".\"a\".\"same\"();",
      "SHOW PROCEDURES FROM SCHEMA \"db\".\"z\";", "SHOW FUNCTIONS FROM SCHEMA \"db\".\"z\";",
      "SHOW PARAMETERS OF PROCEDURE \"db\".\"z\".\"same\"();"}), session.commands);
  EXPECT_EQ(ResultCell{"a"}, result->rows[0][1]); EXPECT_EQ(ResultCell{"super"}, result->rows[0][6]);
  EXPECT_EQ(ResultCell{"0"}, result->rows[0][5]); EXPECT_FALSE(result->rows[0][7]);
  EXPECT_FALSE(result->rows[0][8]); EXPECT_FALSE(result->rows[0][9]);
  EXPECT_EQ(ResultCell{"z"}, result->rows[1][1]); EXPECT_EQ(ResultCell{"2"}, result->rows[1][5]);
  EXPECT_EQ(ResultCell{"18"}, result->rows[1][7]); EXPECT_EQ(ResultCell{"20"}, result->rows[1][8]);
  EXPECT_FALSE(result->rows[1][9]); EXPECT_EQ(ResultCell{"1"}, result->rows[1][17]);
  session.replies.clear(); session.connected = false;
  EXPECT_EQ(ResultCell{"super"}, result->rows[0][6]);
  routine_schema_no_fallback(session);
}

TEST(RedshiftRoutineSchemaPatterns, ExactEscapedAndMetadataIdentifiersKeepFastPathAndEmptyGuards) {
  for (const auto match : {CatalogNameMatch::Existing, CatalogNameMatch::IdentifierQuoted,
                          CatalogNameMatch::IdentifierUnquoted}) {
    RoutineSchemaSession session;
    ProceduresCatalogRequest input{"db", match == CatalogNameMatch::Existing ? "s\\_\\%" : "s_%", "p"};
    input.name_matches.schema = match;
    session.replies.emplace_back(routine_discovery(false, {{"db", "s_%", "p", "0", "", std::nullopt, "exact"}}), session.snapshot);
    session.replies.emplace_back(routine_discovery(true, {}), session.snapshot);
    auto result = session.execute_catalog(input, rs::util::Deadline::max());
    ASSERT_TRUE(result); ASSERT_EQ(1u, result->rows.size());
    EXPECT_EQ((std::vector<std::string>{"SHOW PROCEDURES FROM SCHEMA \"db\".\"s_%\";",
        "SHOW FUNCTIONS FROM SCHEMA \"db\".\"s_%\";"}), session.commands);
    routine_schema_no_fallback(session);
  }
  for (const auto& input : std::vector<CatalogRequest>{
      ProceduresCatalogRequest{"db", "", std::nullopt},
      ProcedureColumnsCatalogRequest{"db", "", std::nullopt, std::nullopt},
      ProceduresCatalogRequest{"", "s", std::nullopt}}) {
    RoutineSchemaSession session;
    auto result = session.execute_catalog(input, rs::util::Deadline::max());
    ASSERT_TRUE(result); EXPECT_TRUE(result->rows.empty()); EXPECT_TRUE(session.commands.empty());
    routine_schema_no_fallback(session);
  }
  for (int guard = 0; guard < 4; ++guard) {
    RoutineSchemaSession session;
    if (guard == 0) session.mode = RedshiftCatalogMode::Legacy;
    if (guard == 1) session.capability = "3";
    if (guard == 2) session.snapshot.state = SessionState::Transaction;
    if (guard == 3) session.connected = false;
    auto result = session.execute_catalog(ProceduresCatalogRequest{"db", "s%", std::nullopt}, rs::util::Deadline::max());
    ASSERT_FALSE(result);
    EXPECT_EQ(rs::util::make_error_code(guard == 3 ? rs::util::DbErrorCode::NotConnected :
        rs::util::DbErrorCode::UnsupportedFeature), result.error());
    EXPECT_TRUE(session.commands.empty()); routine_schema_no_fallback(session);
  }
}

TEST(RedshiftRoutineSchemaPatterns, MalformedSchemaAndLaterNativeErrorNeverPublishPartialRows) {
  for (int malformed = 0; malformed < 3; ++malformed) {
    RoutineSchemaSession session;
    auto schemas = routine_schema_rows({{"db", "a", "1", "local", std::nullopt, std::nullopt, std::nullopt}});
    if (malformed == 0) schemas.rows.push_back(schemas.rows.front());
    if (malformed == 1) schemas.rows[0][0] = "foreign";
    if (malformed == 2) schemas.rows[0][1].reset();
    session.replies.emplace_back(std::move(schemas), session.snapshot);
    auto result = session.execute_catalog(ProceduresCatalogRequest{"db", "%", std::nullopt}, rs::util::Deadline::max());
    ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
    EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
    EXPECT_EQ((std::vector<std::string>{"SHOW SCHEMAS FROM DATABASE \"db\";"}), session.commands);
    routine_schema_no_fallback(session);
  }
  RoutineSchemaSession session;
  session.replies.emplace_back(routine_schema_rows({
      {"db", "a", "1", "local", std::nullopt, std::nullopt, std::nullopt},
      {"db", "z", "1", "local", std::nullopt, std::nullopt, std::nullopt}}), session.snapshot);
  session.replies.emplace_back(routine_discovery(false, {{"db", "a", "p", "0", "", std::nullopt, "unpublished"}}), session.snapshot);
  session.replies.emplace_back(routine_discovery(true, {}), session.snapshot);
  BackendError denied{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owned schema denial"};
  denied.native_state = "42501"; denied.operation = BackendOperation::ExecuteDirect;
  denied.session_state = SessionState::Idle; denied.disposition = SessionDisposition::Reusable;
  session.replies.emplace_back(denied);
  auto failure = session.execute_catalog(ProceduresCatalogRequest{"db", "%", "p"}, rs::util::Deadline::max());
  ASSERT_FALSE(failure); EXPECT_EQ("42501", failure.backend_error().native_state);
  EXPECT_EQ(BackendOperation::ExecuteDirect, failure.backend_error().operation);
  EXPECT_EQ(SessionDisposition::Reusable, failure.session_snapshot().disposition);
  EXPECT_EQ(4u, session.commands.size()); routine_schema_no_fallback(session);
  session.replies.emplace_back(routine_discovery(false, {{"db", "a", "p", "0", "", std::nullopt, "recovered"}}), session.snapshot);
  session.replies.emplace_back(routine_discovery(true, {}), session.snapshot);
  auto recovered = session.execute_catalog(ProceduresCatalogRequest{"db", "a", "p"}, rs::util::Deadline::max());
  ASSERT_TRUE(recovered); ASSERT_EQ(1u, recovered->rows.size());
  EXPECT_EQ(ResultCell{"recovered"}, recovered->rows[0][6]);
  denied.native_state.reset(); denied.message.clear(); session.replies.clear();
  EXPECT_EQ("owned schema denial", failure.error_message()); EXPECT_EQ("42501", failure.backend_error().native_state);
}

TEST(RedshiftRoutineSchemaPatterns, CumulativeExchangeAndSchemaCellLimitsRefuseBeforeRoutineFanout) {
  for (const bool cells : {false, true}) {
    RoutineSchemaSession session;
    ResultLimits limits;
    if (cells) limits.max_cells = 13; // Two literal native schema rows require 14.
    else limits.max_results = 4; // One schema exchange plus four routine exchanges cannot fit.
    ASSERT_TRUE(session.budgets(limits));
    session.replies.emplace_back(routine_schema_rows({
        {"db", "a", "1", "local", std::nullopt, std::nullopt, std::nullopt},
        {"db", "z", "1", "local", std::nullopt, std::nullopt, std::nullopt}}), session.snapshot);
    auto result = session.execute_catalog(ProceduresCatalogRequest{"db", "%", std::nullopt}, rs::util::Deadline::max());
    ASSERT_FALSE(result); EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit), result.error());
    EXPECT_EQ(SessionDisposition::Reusable, result.session_snapshot().disposition);
    EXPECT_EQ((std::vector<std::string>{"SHOW SCHEMAS FROM DATABASE \"db\";"}), session.commands);
    routine_schema_no_fallback(session);
  }
}

TEST(RedshiftRoutineSchemaPatterns, ExpiredAndLateSchemaCompletionKeepOriginalDeadlineAndStopDispatch) {
  RoutineSchemaSession expired;
  auto result = expired.execute_catalog(ProceduresCatalogRequest{"db", "%", std::nullopt}, rs::util::Clock::now());
  ASSERT_FALSE(result); EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout), result.error());
  EXPECT_TRUE(expired.commands.empty()); routine_schema_no_fallback(expired);
  RoutineSchemaSession late;
  late.replies.emplace_back(routine_schema_rows({{"db", "a", "1", "local", std::nullopt, std::nullopt, std::nullopt}}), late.snapshot);
  const auto deadline = rs::util::make_deadline(std::chrono::milliseconds{10});
  late.callback = [deadline] { while (rs::util::Clock::now() < deadline) {} };
  auto timed_out = late.execute_catalog(ProcedureColumnsCatalogRequest{"db", "%", std::nullopt, std::nullopt}, deadline);
  ASSERT_FALSE(timed_out); EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout), timed_out.error());
  EXPECT_EQ((std::vector<std::string>{"SHOW SCHEMAS FROM DATABASE \"db\";"}), late.commands);
  EXPECT_EQ((std::vector<rs::util::Deadline>{deadline}), late.deadlines); routine_schema_no_fallback(late);
}

TEST(RedshiftRoutineSchemaPatterns, CompletedShowAndSelectSchemaShapesKeepUnknownClassificationExplicit) {
  for (const auto kind : {StatementKind::Unknown, StatementKind::SelectCursor}) {
    RoutineSchemaSession session;
    auto schemas = routine_schema_rows({}); schemas.statement_kind = kind;
    session.replies.emplace_back(std::move(schemas), session.snapshot);
    auto result = session.execute_catalog(ProceduresCatalogRequest{"db", "%", std::nullopt}, rs::util::Deadline::max());
    ASSERT_TRUE(result); EXPECT_TRUE(result->rows.empty()); EXPECT_EQ(8u, result->columns.size());
    EXPECT_EQ((std::vector<std::string>{"SHOW SCHEMAS FROM DATABASE \"db\";"}), session.commands);
    routine_schema_no_fallback(session);
  }
  for (int malformed = 0; malformed < 3; ++malformed) {
    RoutineSchemaSession session;
    auto schemas = routine_schema_rows({});
    if (malformed == 0) schemas.statement_kind.reset();
    if (malformed == 1) schemas.columns[1].normalized_type->known = false;
    const auto snapshot = malformed == 2 ? SessionSnapshot{SessionState::Unknown, SessionDisposition::Retire} : session.snapshot;
    session.replies.emplace_back(std::move(schemas), snapshot);
    auto result = session.execute_catalog(ProceduresCatalogRequest{"db", "%", std::nullopt}, rs::util::Deadline::max());
    ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
    EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
    EXPECT_EQ(1u, session.commands.size()); routine_schema_no_fallback(session);
  }
}

namespace {
// ODBC ordinary-character escapes are literal: \a is two characters. Keep
// schema inputs and command inventories literal to distinguish the inherited
// decoder/matcher behavior that drops the backslash. Each API/path has its own
// test, so an original-baseline fatal assertion cannot hide another path.
void routine_ordinary_escape_contract(bool columns, bool wildcard) {
  RoutineSchemaSession session;
  const std::string schema = wildcard ? R"(\alpha)" : R"(\a)";
  std::vector<std::string> expected;
  if (wildcard) {
    session.replies.emplace_back(routine_schema_rows({
        {"db", "alpha", "1", "local", std::nullopt, std::nullopt, std::nullopt},
        {"db", R"(\beta)", "1", "local", std::nullopt, std::nullopt, std::nullopt},
        {"db", R"(\alpha)", "1", "local", std::nullopt, std::nullopt, std::nullopt},
        {"db", "a", "1", "local", std::nullopt, std::nullopt, std::nullopt}}), session.snapshot);
    expected = {R"(SHOW SCHEMAS FROM DATABASE "db";)",
        R"(SHOW PROCEDURES FROM SCHEMA "db"."\alpha";)",
        R"(SHOW FUNCTIONS FROM SCHEMA "db"."\alpha";)"};
  } else {
    expected = {R"(SHOW PROCEDURES FROM SCHEMA "db"."\a";)",
        R"(SHOW FUNCTIONS FROM SCHEMA "db"."\a";)"};
  }
  session.replies.emplace_back(routine_discovery(false,
      {{"db", schema, "p", "0", "", std::nullopt, "literal-backslash"}}), session.snapshot);
  session.replies.emplace_back(routine_discovery(true, {}), session.snapshot);
  if (columns) {
    session.replies.emplace_back(routine_parameters(false,
        {{"db", schema, "p", "out", "1", "OUT", "integer", std::nullopt, "32", "0"}}), session.snapshot);
    expected.push_back(wildcard ? R"(SHOW PARAMETERS OF PROCEDURE "db"."\alpha"."p"();)" :
        R"(SHOW PARAMETERS OF PROCEDURE "db"."\a"."p"();)");
  }
  const std::string pattern = wildcard ? R"(\a%)" : R"(\a)";
  const CatalogRequest input = columns ? CatalogRequest{ProcedureColumnsCatalogRequest{"db", pattern, "p", "out"}} :
      CatalogRequest{ProceduresCatalogRequest{"db", pattern, "p"}};
  const auto deadline = rs::util::make_deadline(std::chrono::seconds{3});
  const auto result = session.execute_catalog(input, deadline);
  EXPECT_EQ(expected, session.commands); // Observe wrong/absent baseline dispatch before fatal check.
  routine_schema_no_fallback(session);
  ASSERT_TRUE(result);
  ASSERT_EQ(1u, result->rows.size()); EXPECT_EQ(columns ? 19u : 8u, result->columns.size());
  EXPECT_EQ(ResultCell{schema}, result->rows[0][1]); EXPECT_EQ(ResultCell{"p"}, result->rows[0][2]);
  EXPECT_EQ(std::vector<rs::util::Deadline>(expected.size(), deadline), session.deadlines);
  EXPECT_TRUE(session.replies.empty());
}
}

TEST(RedshiftRoutineSchemaPatterns, OrdinaryEscapeWildcardProceduresRetainBackslash) {
  routine_ordinary_escape_contract(false, true);
}
TEST(RedshiftRoutineSchemaPatterns, OrdinaryEscapeWildcardProcedureColumnsRetainBackslash) {
  routine_ordinary_escape_contract(true, true);
}
TEST(RedshiftRoutineSchemaPatterns, OrdinaryEscapeLiteralProceduresRetainBackslash) {
  routine_ordinary_escape_contract(false, false);
}
TEST(RedshiftRoutineSchemaPatterns, OrdinaryEscapeLiteralProcedureColumnsRetainBackslash) {
  routine_ordinary_escape_contract(true, false);
}

TEST(RedshiftRoutineSchemaPatterns, MalformedUnmatchedSchemasAreRejectedBeforeFiltering) {
  for (int malformed = 0; malformed < 3; ++malformed) {
    RoutineSchemaSession session;
    auto schemas = routine_schema_rows({
        {"db", "keep", "1", "local", std::nullopt, std::nullopt, std::nullopt},
        {"db", "drop", "1", "local", std::nullopt, std::nullopt, std::nullopt}});
    if (malformed == 0) schemas.rows.push_back(schemas.rows.back());
    if (malformed == 1) schemas.rows.back()[0] = "foreign";
    if (malformed == 2) schemas.rows.back()[1].reset();
    session.replies.emplace_back(std::move(schemas), session.snapshot);
    const auto result = session.execute_catalog(ProceduresCatalogRequest{"db", "keep%", std::nullopt},
        rs::util::Deadline::max());
    ASSERT_FALSE(result);
    EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
    EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
    EXPECT_EQ((std::vector<std::string>{R"(SHOW SCHEMAS FROM DATABASE "db";)"}), session.commands);
    routine_schema_no_fallback(session);
  }
}

TEST(RedshiftRoutineSchemaPatterns, InvalidIntentIsRefusedWithoutDiscovery) {
  for(const auto& schema:std::vector<std::string>{"dangling\\",std::string("bad\0name",8),std::string("\xff",1)}) {
    RoutineSchemaSession session;
    const auto result=session.execute_catalog(ProceduresCatalogRequest{"db",schema,"p"},rs::util::Deadline::max());
    ASSERT_FALSE(result);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),result.error());
    EXPECT_TRUE(session.commands.empty());routine_schema_no_fallback(session);
  }
}

TEST(RedshiftRoutineSchemaPatterns, UnmatchedMalformedShapesAndSnapshotsRetireBeforeFanout) {
  for(unsigned malformed=0;malformed<9;++malformed) {
    RoutineSchemaSession session;
    auto source=routine_schema_rows({{"db","drop","1","local",std::nullopt,std::nullopt,std::nullopt}});
    auto snapshot=session.snapshot;
    if(malformed==0)source.columns[2].name="schema_name";
    if(malformed==1)source.rows[0].pop_back();
    if(malformed==2)source.rows[0][1]=std::string("bad\0name",8);
    if(malformed==3)source.rows[0][1]=std::string("\xff",1);
    if(malformed==4)source.statement_kind=StatementKind::UpdateWhere;
    if(malformed==5)source.affected_rows=2;
    if(malformed==6)snapshot.state=SessionState::Transaction;
    if(malformed==7)snapshot.disposition=SessionDisposition::Retire;
    if(malformed==8)source.additional_results.emplace_back();
    session.replies.emplace_back(std::move(source),snapshot);
    const auto result=session.execute_catalog(ProcedureColumnsCatalogRequest{"db","keep%","p",std::nullopt},rs::util::Deadline::max());
    ASSERT_FALSE(result);EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);
    EXPECT_EQ(SessionDisposition::Retire,result.session_snapshot().disposition);
    EXPECT_EQ((std::vector<std::string>{R"(SHOW SCHEMAS FROM DATABASE "db";)"}),session.commands);
    routine_schema_no_fallback(session);
  }
}

TEST(RedshiftRoutineSchemaPatterns, NativeStorageAndOwningSelectionOverlapAreAdmittedBeforeCopy) {
  const std::string name(900,'a');
  BackendResult<QueryResult> source{routine_schema_rows({{"db",name,"1","local",std::nullopt,std::nullopt,std::nullopt}}),
      {SessionState::Idle,SessionDisposition::Reusable}};
  const auto raw=redshift_routine_schema_usage(*source);
  ResponseLimits response;response.max_wire_bytes=raw.bytes+256;
  RedshiftRoutineBudget tight{response,ResultLimits{}};
  ASSERT_TRUE(tight.retain(raw)); // The raw result fits; its owning clone cannot coexist.
  const auto refusal=normalize_redshift_routine_schemas("db",ProceduresCatalogRequest{"db","%",std::nullopt},
      source,tight,rs::util::Deadline::max());
  ASSERT_FALSE(refusal);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),refusal.error());
  EXPECT_EQ(SessionDisposition::Reusable,refusal.session_snapshot().disposition);
  RedshiftRoutineBudget room{ResponseLimits{},ResultLimits{}};ASSERT_TRUE(room.retain(raw));
  const auto result=normalize_redshift_routine_schemas("db",ProceduresCatalogRequest{"db","%",std::nullopt},
      source,room,rs::util::Deadline::max());
  ASSERT_TRUE(result);ASSERT_EQ(1u,result->names.size());
  source->rows.clear();EXPECT_EQ(name,result->names.front()); // No borrowed publication.
}

TEST(RedshiftRoutineSchemaPatterns, BorrowedAndSelectedIdentityRowMetadataNamePeaksAreCumulative) {
  for(unsigned boundary=0;boundary<3;++boundary) {
    BackendResult<QueryResult> source{routine_schema_rows({
        {"db","a","1","local",std::nullopt,std::nullopt,std::nullopt},
        {"db","z","1","local",std::nullopt,std::nullopt,std::nullopt}}),
        {SessionState::Idle,SessionDisposition::Reusable}};
    const auto raw=redshift_routine_schema_usage(*source);ResultLimits limits;
    if(boundary==0)limits.max_rows=3; // Two native rows plus two owning identities.
    if(boundary==1)limits.max_metadata_entries=10; // Seven fields, two views, two owned names.
    if(boundary==2)limits.max_metadata_name_bytes=raw.name_bytes+3; // Carrier plus owned names.
    RedshiftRoutineBudget budget{ResponseLimits{},limits};ASSERT_TRUE(budget.retain(raw));
    const auto result=normalize_redshift_routine_schemas("db",ProceduresCatalogRequest{"db","%",std::nullopt},
        source,budget,rs::util::Deadline::max());
    ASSERT_FALSE(result);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),result.error());
    EXPECT_EQ(SessionDisposition::Reusable,result.session_snapshot().disposition);
  }
}

TEST(RedshiftRoutineSchemaPatterns, PermutedFieldsKeepSortedOwningIdentitiesDuringFanout) {
  RoutineSchemaSession session;
  auto source=routine_schema_rows({{"db","z","1","local",std::nullopt,std::nullopt,std::nullopt},
      {"db","a","1","local",std::nullopt,std::nullopt,std::nullopt}});
  std::swap(source.columns[0],source.columns[1]);
  for(auto& row:source.rows)std::swap(row[0],row[1]);
  session.replies.emplace_back(std::move(source),session.snapshot);
  for(const char* schema:{"a","z"}) {
    session.replies.emplace_back(routine_discovery(false,{{"db",schema,"p","0","",std::nullopt,std::nullopt}}),session.snapshot);
    session.replies.emplace_back(routine_discovery(true,{}),session.snapshot);
  }
  const auto result=session.execute_catalog(ProceduresCatalogRequest{"db","%","p"},rs::util::Deadline::max());
  ASSERT_TRUE(result);ASSERT_EQ(2u,result->rows.size());EXPECT_EQ(ResultCell{"a"},result->rows[0][1]);EXPECT_EQ(ResultCell{"z"},result->rows[1][1]);
  routine_schema_no_fallback(session);
}

TEST(RedshiftRoutineSchemaPatterns, KnownLaterSchemaExchangesAreReservedBeforeParameterFanout) {
  RoutineSchemaSession session;ResultLimits limits;limits.max_results=5;ASSERT_TRUE(session.budgets(limits));
  session.replies.emplace_back(routine_schema_rows({{"db","a","1","local",std::nullopt,std::nullopt,std::nullopt},
      {"db","z","1","local",std::nullopt,std::nullopt,std::nullopt}}),session.snapshot);
  session.replies.emplace_back(routine_discovery(false,{{"db","a","p","0","",std::nullopt,std::nullopt}}),session.snapshot);
  session.replies.emplace_back(routine_discovery(true,{}),session.snapshot);
  const auto result=session.execute_catalog(ProcedureColumnsCatalogRequest{"db","%","p",std::nullopt},rs::util::Deadline::max());
  ASSERT_FALSE(result);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),result.error());
  EXPECT_EQ((std::vector<std::string>{R"(SHOW SCHEMAS FROM DATABASE "db";)",R"(SHOW PROCEDURES FROM SCHEMA "db"."a";)",
      R"(SHOW FUNCTIONS FROM SCHEMA "db"."a";)"}),session.commands);routine_schema_no_fallback(session);
}

TEST(RedshiftRoutineSchemaPatterns, SelectedSchemaRowsRemainChargedWhileRoutineStorageIsAllocated) {
  RoutineSchemaSession session;
  ResultLimits limits; limits.max_rows=2; ASSERT_TRUE(session.budgets(limits));
  session.replies.emplace_back(routine_schema_rows({
      {"db","a","1","local",std::nullopt,std::nullopt,std::nullopt}}),session.snapshot);
  session.replies.emplace_back(routine_discovery(false,
      {{"db","a","p","0","",std::nullopt,std::nullopt}}),session.snapshot);
  // Schema source + owned selection fits two rows. After source destruction,
  // selected identity + native routine row fits; its owning routine copy does
  // not. Releasing selected identities prematurely would let discovery pass.
  const auto result=session.execute_catalog(ProceduresCatalogRequest{"db","%","p"},rs::util::Deadline::max());
  ASSERT_FALSE(result);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),result.error());
  EXPECT_EQ(SessionDisposition::Reusable,result.session_snapshot().disposition);
  EXPECT_EQ((std::vector<std::string>{R"(SHOW SCHEMAS FROM DATABASE "db";)",
      R"(SHOW PROCEDURES FROM SCHEMA "db"."a";)"}),session.commands);
  routine_schema_no_fallback(session);
}

TEST(RedshiftRoutineSchemaPatterns, InvalidSchemaSnapshotPrecedesRawStorageOverflow) {
  for(const auto snapshot:std::array<SessionSnapshot,3>{{
      {SessionState::Unknown,SessionDisposition::Retire},
      {SessionState::Transaction,SessionDisposition::ResetRequired},
      {SessionState::Idle,SessionDisposition::Retire}}}) {
    SCOPED_TRACE(static_cast<int>(snapshot.state));
    RoutineSchemaSession session;ResponseLimits response;response.max_wire_bytes=512;
    ASSERT_TRUE(session.budgets(ResultLimits{},response));
    auto schemas=routine_schema_rows({
        {"db","a","1","local",std::nullopt,std::nullopt,std::nullopt}});
    ASSERT_GT(redshift_routine_schema_usage(schemas).bytes,response.max_wire_bytes);
    session.replies.emplace_back(std::move(schemas),snapshot);
    const auto result=session.execute_catalog(ProceduresCatalogRequest{"db","%","p"},rs::util::Deadline::max());
    ASSERT_FALSE(result);
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),result.error());
    EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);
    EXPECT_EQ(snapshot.state,result.session_snapshot().state);
    EXPECT_EQ(SessionDisposition::Retire,result.session_snapshot().disposition);
    EXPECT_EQ((std::vector<std::string>{R"(SHOW SCHEMAS FROM DATABASE "db";)"}),session.commands);
    routine_schema_no_fallback(session);
  }
}

TEST(RedshiftRoutineSchemaPatterns, DestroyedRoutineStorageDoesNotConsumeLaterSchemaPeakBudget) {
  for(const auto cap:{4u,5u}) {
    SCOPED_TRACE(cap);
    RoutineSchemaSession session;ResultLimits limits;limits.max_rows=cap;
    ASSERT_TRUE(session.budgets(limits));
    session.replies.emplace_back(routine_schema_rows({
        {"db","a","1","local",std::nullopt,std::nullopt,std::nullopt},
        {"db","z","1","local",std::nullopt,std::nullopt,std::nullopt}}),session.snapshot);
    for(const char* schema:{"a","z"}) {
      session.replies.emplace_back(routine_discovery(false,
          {{"db",schema,"p","0","",std::nullopt,"owning"}}),session.snapshot);
      session.replies.emplace_back(routine_discovery(true,{}),session.snapshot);
    }
    const auto deadline=rs::util::make_deadline(std::chrono::seconds{3});
    const auto result=session.execute_catalog(ProceduresCatalogRequest{"db","%","p"},deadline);
    const std::vector<std::string> commands{R"(SHOW SCHEMAS FROM DATABASE "db";)",
        R"(SHOW PROCEDURES FROM SCHEMA "db"."a";)",R"(SHOW FUNCTIONS FROM SCHEMA "db"."a";)",
        R"(SHOW PROCEDURES FROM SCHEMA "db"."z";)",R"(SHOW FUNCTIONS FROM SCHEMA "db"."z";)"};
    if(cap==5) {
      // True peak: two selected identities + first unpublished output +
      // second raw routine row + its owning copy. First routine is destroyed.
      ASSERT_TRUE(result);ASSERT_EQ(2u,result->rows.size());EXPECT_EQ(commands,session.commands);
      EXPECT_EQ(ResultCell{"a"},result->rows[0][1]);EXPECT_EQ(ResultCell{"z"},result->rows[1][1]);
      session.replies.clear();session.connected=false;
      EXPECT_EQ(ResultCell{"owning"},result->rows[0][6]);EXPECT_EQ(ResultCell{"owning"},result->rows[1][6]);
    } else {
      // Four cannot fit the simultaneous second-schema copy: selected names
      // and the already accumulated unpublished output must remain charged.
      ASSERT_FALSE(result);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),result.error());
      EXPECT_EQ(SessionDisposition::Reusable,result.session_snapshot().disposition);
      EXPECT_EQ(std::vector<std::string>(commands.begin(),commands.end()-1),session.commands);
      EXPECT_EQ(1u,session.replies.size());
    }
    EXPECT_EQ(std::vector<rs::util::Deadline>(session.commands.size(),deadline),session.deadlines);
    routine_schema_no_fallback(session);
  }
}

TEST(RedshiftRoutineSchemaPatterns, DestroyedRoutineSignatureAndTextStorageIsReleasedAcrossSchemas) {
  RoutineSchemaSession session;ResponseLimits response;response.max_wire_bytes=3200;
  ASSERT_TRUE(session.budgets(ResultLimits{},response));
  session.replies.emplace_back(routine_schema_rows({
      {"db","a","1","local",std::nullopt,std::nullopt,std::nullopt},
      {"db","z","1","local",std::nullopt,std::nullopt,std::nullopt}}),session.snapshot);
  const std::string remarks(900,'r');
  for(const char* schema:{"a","z"}) {
    session.replies.emplace_back(routine_discovery(false,
        {{"db",schema,"p","2","integer,numeric","void",remarks}}),session.snapshot);
    session.replies.emplace_back(routine_discovery(true,{}),session.snapshot);
  }
  // First-schema arguments, signature, return type and remarks are gone before
  // the second schema. Both owning output remarks remain charged and retained.
  const auto result=session.execute_catalog(ProceduresCatalogRequest{"db","%","p"},rs::util::Deadline::max());
  ASSERT_TRUE(result);ASSERT_EQ(2u,result->rows.size());
  EXPECT_EQ((std::vector<std::string>{R"(SHOW SCHEMAS FROM DATABASE "db";)",
      R"(SHOW PROCEDURES FROM SCHEMA "db"."a";)",R"(SHOW FUNCTIONS FROM SCHEMA "db"."a";)",
      R"(SHOW PROCEDURES FROM SCHEMA "db"."z";)",R"(SHOW FUNCTIONS FROM SCHEMA "db"."z";)"}),session.commands);
  session.replies.clear();session.connected=false;
  EXPECT_EQ(ResultCell{remarks},result->rows[0][6]);EXPECT_EQ(ResultCell{remarks},result->rows[1][6]);
  routine_schema_no_fallback(session);
}

namespace {
class WideColumnsSession final : public RoutineExecutionSession {
 public:
  std::optional<std::string> external;
  std::optional<std::string> get_parameter_status(std::string_view key) const override {
    return key=="external_database"?external:std::nullopt;
  }
  void current(std::vector<std::string> tables={"a","z"}) {
    replies.emplace_back(routine_identity(),snapshot);
    auto list=tables_response("db");list.rows.clear();
    for (const auto& table:tables) list.rows.push_back({"db","fixture",table,"TABLE",std::nullopt,std::nullopt});
    replies.emplace_back(std::move(list),snapshot);
    std::sort(tables.begin(),tables.end());
    for (const auto& table:tables) replies.emplace_back(columns_response("db","fixture",table),snapshot);
  }
};
ColumnsCatalogRequest wide_columns(std::optional<std::string> table=std::nullopt) {
  return {std::nullopt,"fixture",std::move(table),std::nullopt};
}
void wide_resource(const BackendResult<QueryResult>& result) {
  ASSERT_FALSE(result);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),result.error());
  EXPECT_EQ(SessionState::Idle,result.backend_error().session_state);
  EXPECT_EQ(SessionDisposition::Reusable,result.backend_error().disposition);
}
}
TEST(RedshiftSchemaWideColumns, NullAndUniversalTableKeepOrderedOwningEighteenFieldResults) {
  for (const auto& table:{std::optional<std::string>{},std::optional<std::string>{"%"}}) {
    WideColumnsSession session;session.current({"z","a"});
    auto request=wide_columns(table);const auto deadline=rs::util::make_deadline(std::chrono::seconds{2});
    EXPECT_TRUE(session.selects_catalog_request(request));
    auto result=session.execute_catalog(request,deadline);ASSERT_TRUE(result)<<result.error_message();
    ASSERT_EQ(18u,result->columns.size());ASSERT_EQ(4u,result->rows.size());
    EXPECT_EQ("a",result->rows[0][2]);EXPECT_EQ("id",result->rows[0][3]);EXPECT_EQ("1",result->rows[0][16]);
    EXPECT_EQ("z",result->rows[2][2]);EXPECT_EQ("owning remark",result->rows[3][11]);
    EXPECT_EQ("1",result->rows[0][12]);EXPECT_EQ("10",result->rows[0][6]);
    EXPECT_EQ((std::vector<std::string>{"SELECT current_database() AS database_name",
        "SHOW TABLES FROM SCHEMA \"db\".\"fixture\";","SHOW COLUMNS FROM TABLE \"db\".\"fixture\".\"a\";",
        "SHOW COLUMNS FROM TABLE \"db\".\"fixture\".\"z\";"}),session.commands);
    EXPECT_EQ(std::vector<rs::util::Deadline>(4,deadline),session.deadlines);
    session.replies.clear();session.connected=false;EXPECT_EQ("owning remark",result->rows[1][11]);
  }
}
TEST(RedshiftSchemaWideColumns, ReturnedSpecialNamesAreExactQuotedIdentifiersNotCallerPatterns) {
  WideColumnsSession session;session.current({"t_%\\a\"雪","plain"});
  auto request=wide_columns();request.column="i%";
  auto result=session.execute_catalog(request,rs::util::Deadline::max());ASSERT_TRUE(result)<<result.error_message();
  ASSERT_EQ(2u,result->rows.size());EXPECT_EQ("id",result->rows[1][3]);EXPECT_EQ("t_%\\a\"雪",result->rows[1][2]);
  EXPECT_EQ("SHOW COLUMNS FROM TABLE \"db\".\"fixture\".\"t_%\\a\"\"雪\";",session.commands.back());
}
TEST(RedshiftSchemaWideColumns, VisibleDatabaseCollisionsCatalogSelectionAndEmptyKeepScope) {
  for (const auto& catalog:{std::optional<std::string>{},std::optional<std::string>{"z_db"},std::optional<std::string>{"%"}}) {
    WideColumnsSession session;session.external="on";session.replies.emplace_back(visible_databases(),session.snapshot);
    for (const auto db:{"a_雪","z_db"}) {
      if (catalog && *catalog!=db) continue;
      auto list=tables_response(db);list.rows.resize(1);list.rows[0][2]="same";
      session.replies.emplace_back(list,session.snapshot);session.replies.emplace_back(columns_response(db,"fixture","same"),session.snapshot);
    }
    auto request=wide_columns();request.catalog=catalog;
    auto result=session.execute_catalog(request,rs::util::Deadline::max());ASSERT_TRUE(result)<<result.error_message();
    EXPECT_EQ(catalog?(*catalog=="%"?0u:2u):4u,result->rows.size());
    EXPECT_EQ("SHOW DATABASES;",session.commands.front());
    if (!catalog) {EXPECT_EQ("a_雪",result->rows[0][0]);EXPECT_EQ("z_db",result->rows[2][0]);}
    if (catalog && *catalog=="%") EXPECT_EQ(1u,session.commands.size()); // Catalog is a literal for SQLColumns.
  }
  WideColumnsSession empty;empty.current({});auto result=empty.execute_catalog(wide_columns(),rs::util::Deadline::max());
  ASSERT_TRUE(result);EXPECT_TRUE(result->rows.empty());EXPECT_EQ(18u,result->columns.size());EXPECT_EQ(2u,empty.commands.size());
}
TEST(RedshiftSchemaWideColumns, SelectionPrecedesAvailabilityAndKeepsExactLegacyAndMetadataIdGuards) {
  for (const auto capability:{"","3","-4","4x","4294967296"}) {
    WideColumnsSession session;session.capability=capability;EXPECT_TRUE(session.selects_catalog_request(wide_columns()));
    EXPECT_FALSE(session.execute_catalog(wide_columns(),rs::util::Deadline::max()));EXPECT_TRUE(session.commands.empty());
  }
  WideColumnsSession busy;busy.snapshot={SessionState::Transaction,SessionDisposition::ResetRequired};
  EXPECT_TRUE(busy.selects_catalog_request(wide_columns()));EXPECT_FALSE(busy.execute_catalog(wide_columns(),rs::util::Deadline::max()));EXPECT_TRUE(busy.commands.empty());
  WideColumnsSession legacy;legacy.mode=RedshiftCatalogMode::Legacy;EXPECT_FALSE(legacy.selects_catalog_request(wide_columns()));
  for (const auto table:{"","a%","_"}) {
    auto request=wide_columns(std::string(table));EXPECT_TRUE(redshift_columns_schema_wide(request));
  }
  auto exact=wide_columns(std::string{"\\%"});EXPECT_TRUE(redshift_column_names_are_literal(exact));
  auto metadata=wide_columns();metadata.name_matches.schema=CatalogNameMatch::IdentifierQuoted;
  WideColumnsSession id;EXPECT_TRUE(id.selects_catalog_request(metadata));
  EXPECT_FALSE(id.execute_catalog(metadata,rs::util::Deadline::max()));EXPECT_TRUE(id.commands.empty());
}
TEST(RedshiftSchemaWideColumns, WholeTableMetadataIsValidatedBeforeFilteringFanoutAndStorageRefusal) {
  for (unsigned variant=0;variant<10;++variant) {
    WideColumnsSession session;session.current();auto& raw=*session.replies[1];
    switch (variant) {
      case 0:raw.rows[1][0]="foreign";break;
      case 1:raw.rows[1][1]="foreign";break;
      case 2:raw.rows[1][2]=raw.rows[0][2];break;
      case 3:raw.rows[1][2]=std::string{"\xff"};break;
      case 4:raw.rows[1][3]="unclassified";break;
      case 5:raw.rows[1][4]=std::string{"bad\0text",8};break;
      case 6:raw.statement_kind.reset();break;
      case 7:raw.columns[0].name="unexpected";break;
      case 8:raw.rows[1].pop_back();break;
      case 9:raw.additional_results.emplace_back();break;
    }
    auto request=wide_columns();request.column="hidden";
    auto result=session.execute_catalog(request,rs::util::Deadline::max());ASSERT_FALSE(result);
    EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);
    EXPECT_EQ(SessionDisposition::Retire,result.backend_error().disposition);EXPECT_EQ(2u,session.commands.size());
  }
  WideColumnsSession overflow;ResultLimits limits;limits.max_rows=2;ASSERT_TRUE(overflow.budgets(limits));overflow.current();
  overflow.replies[1]->rows[1][0]="foreign";
  auto result=overflow.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_FALSE(result);
  EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);EXPECT_EQ(2u,overflow.commands.size());
}
TEST(RedshiftSchemaWideColumns, InvalidSnapshotPrecedesOverflowAndPreservesTerminalState) {
  for (const auto snapshot:{SessionSnapshot{SessionState::FailedTransaction,SessionDisposition::Retire},
      SessionSnapshot{SessionState::Transaction,SessionDisposition::ResetRequired}}) {
    WideColumnsSession session;ResponseLimits response;response.max_wire_bytes=4096;ASSERT_TRUE(session.budgets(ResultLimits{},response));
    session.replies.emplace_back(routine_identity(),session.snapshot);
    auto raw=tables_response("db");raw.rows[0][4]=std::string(8192,'x');session.replies.emplace_back(raw,snapshot);
    auto result=session.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_FALSE(result);
    EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);
    EXPECT_EQ(snapshot.state,result.backend_error().session_state);EXPECT_EQ(SessionDisposition::Retire,result.backend_error().disposition);
    EXPECT_EQ(2u,session.commands.size());
  }
}
TEST(RedshiftSchemaWideColumns, KnownFanoutAndSimultaneousSelectionPeaksAreRefusedBeforeDescriptions) {
  WideColumnsSession queries;ResultLimits limits;limits.max_results=3;ASSERT_TRUE(queries.budgets(limits));queries.current();
  wide_resource(queries.execute_catalog(wide_columns(),rs::util::Deadline::max()));EXPECT_EQ(2u,queries.commands.size());
  WideColumnsSession rows;limits=ResultLimits{};limits.max_rows=3;ASSERT_TRUE(rows.budgets(limits));rows.current();
  wide_resource(rows.execute_catalog(wide_columns(),rs::util::Deadline::max()));EXPECT_EQ(2u,rows.commands.size());
  WideColumnsSession cells;limits=ResultLimits{};limits.max_cells=13;ASSERT_TRUE(cells.budgets(limits));cells.current();
  wide_resource(cells.execute_catalog(wide_columns(),rs::util::Deadline::max()));EXPECT_EQ(2u,cells.commands.size());
}
TEST(RedshiftSchemaWideColumns, DestroyedPerTableStorageIsReleasedButOwningRowsRemainCharged) {
  for (const auto cap:{8u,9u}) {
    WideColumnsSession session;ResultLimits limits;limits.max_rows=cap;ASSERT_TRUE(session.budgets(limits));session.current();
    auto result=session.execute_catalog(wide_columns(),rs::util::Deadline::max());
    if (cap==8) wide_resource(result);else {ASSERT_TRUE(result)<<result.error_message();EXPECT_EQ(4u,result->rows.size());}
    EXPECT_EQ(4u,session.commands.size());
  }
}
TEST(RedshiftSchemaWideColumns, LateNativeErrorMalformedColumnsAndFreshRecoveryNeverPublishPrefix) {
  for (unsigned variant=0;variant<3;++variant) {
    WideColumnsSession session;session.current();
    if (variant==0) {BackendError native{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),"owning late denial"};
      native.native_state="42501";native.session_state=SessionState::Idle;native.disposition=SessionDisposition::Reusable;session.replies[3]=native;}
    if (variant==1) session.replies[3]->rows[0][2]="foreign";
    if (variant==2) session.replies[3]->additional_results.emplace_back();
    auto failed=session.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_FALSE(failed);EXPECT_EQ(4u,session.commands.size());
    if (variant==0) EXPECT_EQ("42501",failed.backend_error().native_state);
    session.replies.clear();session.commands.clear();session.current({"fresh"});
    auto recovered=session.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_TRUE(recovered);
    ASSERT_EQ(2u,recovered->rows.size());EXPECT_EQ("fresh",recovered->rows[0][2]);EXPECT_EQ(3u,session.commands.size());
  }
}
TEST(RedshiftSchemaWideColumns, OneAbsoluteDeadlineStopsLateDiscoveryAndDescription) {
  for (const auto call:{0u,2u,4u}) {
    WideColumnsSession session;session.current();const auto deadline=call?rs::util::make_deadline(std::chrono::milliseconds{5}):rs::util::Clock::now();
    session.callback=[&]{if(session.commands.size()==call)while(rs::util::Clock::now()<deadline)std::this_thread::yield();};
    auto result=session.execute_catalog(wide_columns(),deadline);ASSERT_FALSE(result);
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout),result.error());EXPECT_EQ(call,session.commands.size());
    EXPECT_EQ(std::vector<rs::util::Deadline>(call,deadline),session.deadlines);
  }
}
TEST(RedshiftSchemaWideColumns, NameBudgetDoesNotChargeArbitraryRemarksAndPartialUnknownDimensionsSurvive) {
  WideColumnsSession session;ResultLimits limits;limits.max_metadata_name_bytes=4096;ASSERT_TRUE(session.budgets(limits));session.current({"a"});
  auto& raw=*session.replies[2];raw.rows[0][11]=std::string(8192,'r');raw.rows[0][7]="unknown_native";
  auto result=session.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_TRUE(result)<<result.error_message();
  ASSERT_EQ(2u,result->rows.size());EXPECT_EQ("0",result->rows[1][4]);EXPECT_EQ("unknown_native",result->rows[1][5]);
  EXPECT_FALSE(result->rows[1][6]);EXPECT_EQ(8192u,result->rows[1][11]->size());
  WideColumnsSession empty;empty.current({"a"});empty.replies[2]->rows.clear();
  auto zero=empty.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_TRUE(zero);EXPECT_TRUE(zero->rows.empty());EXPECT_EQ(18u,zero->columns.size());
}
TEST(RedshiftSchemaWideColumns, PermutedNativeFieldsRetainExactSortedIdentitiesAndProjection) {
  WideColumnsSession session;session.current({"z","a"});
  for (std::size_t n=1;n<session.replies.size();++n) {
    auto& raw=*session.replies[n];std::reverse(raw.columns.begin(),raw.columns.end());
    for(auto& row:raw.rows)std::reverse(row.begin(),row.end());
  }
  auto result=session.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_TRUE(result)<<result.error_message();
  ASSERT_EQ(4u,result->rows.size());EXPECT_EQ("a",result->rows[0][2]);EXPECT_EQ("id",result->rows[0][3]);
  EXPECT_EQ("1",result->rows[0][12]);EXPECT_EQ("owning remark",result->rows[3][11]);
}
TEST(RedshiftSchemaWideColumns, DestroyedSelectionsAcrossDatabasesDoNotAccumulateButUnpublishedOutputDoes) {
  WideColumnsSession session;session.external="on";ResultLimits limits;limits.max_rows=9;ASSERT_TRUE(session.budgets(limits));
  session.replies.emplace_back(visible_databases(),session.snapshot);
  for (const auto db:{"a_雪","z_db"}) {
    auto raw=tables_response(db);raw.rows.resize(1);raw.rows[0][2]="same";
    session.replies.emplace_back(raw,session.snapshot);session.replies.emplace_back(columns_response(db,"fixture","same"),session.snapshot);
  }
  auto result=session.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_TRUE(result)<<result.error_message();
  EXPECT_EQ(4u,result->rows.size());EXPECT_EQ(5u,session.commands.size());
}
TEST(RedshiftSchemaWideColumns, StorageMetadataAndNameBoundsStopWithoutFallbackAndFreshBudgetsRecover) {
  for(unsigned kind=0;kind<4;++kind) {
    WideColumnsSession session;ResultLimits limits;ResponseLimits response;
    if(kind==0)response.max_wire_bytes=5000;
    if(kind==1)limits.max_metadata_entries=24;
    if(kind==2)limits.max_column_name_bytes=24;
    if(kind==3)limits.max_metadata_name_bytes=512;
    ASSERT_TRUE(session.budgets(limits,response));session.current({"a"});
    if(kind==0)session.replies[2]->rows[0][11]=std::string(6000,'r');
    if(kind==2){session.replies[1]->rows[0][2]=std::string(25,'a');}
    auto failed=session.execute_catalog(wide_columns(),rs::util::Deadline::max());wide_resource(failed);
    EXPECT_LE(session.commands.size(),3u);session.replies.clear();session.commands.clear();
    ASSERT_TRUE(session.budgets(ResultLimits{}));session.current({"fresh"});
    auto restored=session.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_TRUE(restored)<<restored.error_message();
    EXPECT_EQ(2u,restored->rows.size());
  }
}
TEST(RedshiftSchemaWideColumns, MalformedScopeAndLaterRetiredColumnSnapshotsNeverDowngradeOrMaskWithOverflow) {
  WideColumnsSession invalid;invalid.external="unknown";EXPECT_TRUE(invalid.selects_catalog_request(wide_columns()));
  auto refused=invalid.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_FALSE(refused);
  EXPECT_EQ(BackendErrorClass::InvalidMetadata,refused.backend_error().error_class);EXPECT_TRUE(invalid.commands.empty());
  WideColumnsSession later;ResponseLimits response;response.max_wire_bytes=32768;ASSERT_TRUE(later.budgets(ResultLimits{},response));later.current();
  auto raw=columns_response("db","fixture","z");raw.rows[0][11]=std::string(65536,'x');
  later.replies[3]=BackendResult<QueryResult>{raw,{SessionState::Disconnected,SessionDisposition::Retire}};
  auto result=later.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_FALSE(result);
  EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);
  EXPECT_EQ(SessionState::Disconnected,result.backend_error().session_state);EXPECT_EQ(SessionDisposition::Retire,result.backend_error().disposition);
  EXPECT_EQ(4u,later.commands.size());
}
TEST(RedshiftSchemaWideColumns, VisibleDatabaseOpaqueAttributesDoNotBecomeIdentifierBudgetCharges) {
  WideColumnsSession session;session.external="on";ResultLimits limits;limits.max_metadata_name_bytes=4096;
  ASSERT_TRUE(session.budgets(limits));auto databases=visible_databases();databases.rows[0][4]=std::string(8192,'p');
  session.replies.emplace_back(databases,session.snapshot);
  for(const auto db:{"a_雪","z_db"}) {
    auto raw=tables_response(db);raw.rows.resize(1);raw.rows[0][2]="same";
    session.replies.emplace_back(raw,session.snapshot);session.replies.emplace_back(columns_response(db,"fixture","same"),session.snapshot);
  }
  auto result=session.execute_catalog(wide_columns(),rs::util::Deadline::max());ASSERT_TRUE(result)<<result.error_message();
  EXPECT_EQ(4u,result->rows.size());EXPECT_EQ(5u,session.commands.size());
}

namespace {
void invalid_wide_catalog_has_no_io_and_recovers(const std::string& catalog,
    const std::optional<std::string>& table) {
  WideColumnsSession session;session.current({"fresh"});
  auto request=wide_columns(table);request.catalog=catalog;
  const auto deadline=rs::util::make_deadline(std::chrono::seconds{2});
  EXPECT_TRUE(session.selects_catalog_request(request));
  auto rejected=session.execute_catalog(request,deadline);ASSERT_FALSE(rejected);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),rejected.error());
  EXPECT_EQ(BackendErrorClass::InvalidInput,rejected.backend_error().error_class);
  EXPECT_EQ(BackendOperation::ExecuteCatalog,rejected.backend_error().operation);
  EXPECT_EQ(SessionState::Idle,rejected.session_snapshot().state);
  EXPECT_EQ(SessionDisposition::Reusable,rejected.session_snapshot().disposition);
  EXPECT_TRUE(session.commands.empty());EXPECT_TRUE(session.deadlines.empty());
  ASSERT_EQ(3u,session.replies.size()); // No exchange, fallback or discarded queued response.
  request.catalog="db"; // Nonempty valid literal makes the preflight match false, not an error.
  auto recovered=session.execute_catalog(request,deadline);ASSERT_TRUE(recovered)<<recovered.error_message();
  ASSERT_EQ(2u,recovered->rows.size());EXPECT_EQ("fresh",recovered->rows[0][2]);
  EXPECT_EQ("id",recovered->rows[0][3]);EXPECT_EQ("owning remark",recovered->rows[1][11]);
  EXPECT_EQ((std::vector<std::string>{"SELECT current_database() AS database_name",
      "SHOW TABLES FROM SCHEMA \"db\".\"fixture\";",
      "SHOW COLUMNS FROM TABLE \"db\".\"fixture\".\"fresh\";"}),session.commands);
  EXPECT_EQ(std::vector<rs::util::Deadline>(3,deadline),session.deadlines);
}
}
TEST(RedshiftSchemaWideColumns, MalformedCatalogEmbeddedNulWithNullTableRejectsBeforeIoAndRecovers) {
  invalid_wide_catalog_has_no_io_and_recovers(std::string{"bad\0name",8},std::nullopt);
}
TEST(RedshiftSchemaWideColumns, MalformedCatalogEmbeddedNulWithUniversalTableRejectsBeforeIoAndRecovers) {
  invalid_wide_catalog_has_no_io_and_recovers(std::string{"bad\0name",8},std::string{"%"});
}
TEST(RedshiftSchemaWideColumns, MalformedCatalogInvalidUtf8WithNullTableRejectsBeforeIoAndRecovers) {
  invalid_wide_catalog_has_no_io_and_recovers(std::string{"\xff",1},std::nullopt);
}
TEST(RedshiftSchemaWideColumns, MalformedCatalogInvalidUtf8WithUniversalTableRejectsBeforeIoAndRecovers) {
  invalid_wide_catalog_has_no_io_and_recovers(std::string{"\xff",1},std::string{"%"});
}

namespace {
void pattern_columns(WideColumnsSession& session, const std::vector<std::string>& discovered,
    const std::vector<std::string>& selected) {
  session.current(discovered);
  session.replies.erase(session.replies.begin()+2,session.replies.end());
  for (const auto& table:selected) {
    session.replies.emplace_back(columns_response("db","fixture",table),session.snapshot);
  }
}
}
TEST(RedshiftColumnsTablePatterns, Utf8SingleCharacterPrefixAndEscapedSpecialsSelectOnlyOwningMatches) {
  struct Case { const char* pattern; std::vector<std::string> selected; };
  const std::vector<Case> cases{{"a_",{"a1","a雪","a😀"}},
      {"a\\_%",{"a_1"}},{"a\\%%",{"a%2"}},{"a\\\\%",{"a\\3"}},
      {"a%",{"a\"q","a%2","a1","a\\3","a_1","a雪","a😀"}},
      {"a\"%",{"a\"q"}}};
  for (const auto& test:cases) {
    SCOPED_TRACE(test.pattern);
    WideColumnsSession session;
    pattern_columns(session,{"z","a😀","a_1","a\\3","a%2","a雪","a1","a\"q"},test.selected);
    const auto deadline=rs::util::make_deadline(std::chrono::seconds{2});
    auto result=session.execute_catalog(wide_columns(std::string{test.pattern}),deadline);
    ASSERT_TRUE(result)<<result.error_message();
    ASSERT_EQ(18u,result->columns.size());
    ASSERT_EQ(test.selected.size()*2,result->rows.size());
    for (std::size_t i=0;i<test.selected.size();++i) {
      EXPECT_EQ("db",result->rows[i*2][0]);EXPECT_EQ("fixture",result->rows[i*2][1]);
      EXPECT_EQ(test.selected[i],result->rows[i*2][2]);EXPECT_EQ("id",result->rows[i*2][3]);
      EXPECT_EQ("1",result->rows[i*2][16]);EXPECT_EQ("2",result->rows[i*2+1][16]);
      EXPECT_EQ("owning remark",result->rows[i*2+1][11]);
    }
    EXPECT_EQ(test.selected.size()+2,session.commands.size());
    EXPECT_EQ("SHOW TABLES FROM SCHEMA \"db\".\"fixture\";",session.commands[1]);
    EXPECT_EQ(std::vector<rs::util::Deadline>(session.commands.size(),deadline),session.deadlines);
    session.replies.clear();session.connected=false;
    EXPECT_EQ("owning remark",result->rows.back()[11]);
  }
}
TEST(RedshiftColumnsTablePatterns, ReturnedEscapedAndQuotedNamesFeedExactCommandsNotPatterns) {
  WideColumnsSession session;
  pattern_columns(session,{"a_%\\quoted\"雪","other"},{"a_%\\quoted\"雪"});
  auto result=session.execute_catalog(wide_columns(std::string{"a\\_\\%\\\\quoted\"_"}),rs::util::Deadline::max());
  // Escaped-special-only literals keep the exact-table path. The final '_' is
  // deliberately a wildcard selecting one Unicode code point via discovery.
  ASSERT_TRUE(result)<<result.error_message();ASSERT_EQ(2u,result->rows.size());
  EXPECT_EQ("a_%\\quoted\"雪",result->rows[0][2]);
  EXPECT_EQ((std::vector<std::string>{"SELECT current_database() AS database_name",
      "SHOW TABLES FROM SCHEMA \"db\".\"fixture\";",
      "SHOW COLUMNS FROM TABLE \"db\".\"fixture\".\"a_%\\quoted\"\"雪\";"}),session.commands);
}
TEST(RedshiftColumnsTablePatterns, EmptyAndNoMatchStillValidateDiscoveryWithoutDescribingOrFallback) {
  for (const auto& pattern:{"","missing%","A_"}) {
    WideColumnsSession session;pattern_columns(session,{"a雪","z"},{});
    EXPECT_TRUE(session.selects_catalog_request(wide_columns(std::string{pattern})));
    auto result=session.execute_catalog(wide_columns(std::string{pattern}),rs::util::Deadline::max());
    ASSERT_TRUE(result)<<result.error_message();EXPECT_EQ(18u,result->columns.size());EXPECT_TRUE(result->rows.empty());
    EXPECT_EQ((std::vector<std::string>{"SELECT current_database() AS database_name",
        "SHOW TABLES FROM SCHEMA \"db\".\"fixture\";"}),session.commands);
  }
}
TEST(RedshiftColumnsTablePatterns, MalformedPatternOrCatalogRejectsBeforeIoAndQueuedFreshRequestRecovers) {
  for (const auto& poison:std::vector<std::string>{"bad%\\",std::string{"bad\0%",5},std::string{"\xff%",2}}) {
    for (const bool catalog:{false,true}) {
      WideColumnsSession session;pattern_columns(session,{"a","other"},{"a"});
      auto request=wide_columns(std::string{"a%"});
      // Catalog remains a literal: a dangling escape is invalid only in the table pattern.
      if (catalog && poison=="bad%\\") continue;
      if (catalog) request.catalog=poison;else request.table=poison;
      auto result=session.execute_catalog(request,rs::util::Deadline::max());
      ASSERT_FALSE(result);EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),result.error());
      EXPECT_TRUE(session.commands.empty());EXPECT_TRUE(session.deadlines.empty());EXPECT_EQ(3u,session.replies.size());
      auto restored=session.execute_catalog(wide_columns(std::string{"a%"}),rs::util::Deadline::max());
      ASSERT_TRUE(restored)<<restored.error_message();ASSERT_EQ(2u,restored->rows.size());EXPECT_EQ("a",restored->rows[0][2]);
    }
  }
}
TEST(RedshiftColumnsTablePatterns, SelectedIntentRefusesUnavailableOrBusyWithoutDowngradeAndKeepsIdentifierModes) {
  for (const auto& capability:{"","3","4x","4294967296"}) {
    WideColumnsSession session;session.capability=capability;
    EXPECT_TRUE(session.selects_catalog_request(wide_columns(std::string{"a%"})));
    EXPECT_FALSE(session.execute_catalog(wide_columns(std::string{"a%"}),rs::util::Deadline::max()));
    EXPECT_TRUE(session.commands.empty());
  }
  WideColumnsSession busy;busy.snapshot={SessionState::Transaction,SessionDisposition::ResetRequired};
  EXPECT_FALSE(busy.execute_catalog(wide_columns(std::string{"a%"}),rs::util::Deadline::max()));EXPECT_TRUE(busy.commands.empty());
  WideColumnsSession legacy;legacy.mode=RedshiftCatalogMode::Legacy;
  EXPECT_FALSE(legacy.selects_catalog_request(wide_columns(std::string{"a%"})));
  auto metadata=wide_columns(std::string{"a%"});metadata.name_matches.schema=CatalogNameMatch::IdentifierQuoted;
  metadata.name_matches.object=CatalogNameMatch::IdentifierQuoted;
  WideColumnsSession exact;exact.replies.emplace_back(routine_identity(),exact.snapshot);
  exact.replies.emplace_back(columns_response("db","fixture","a%"),exact.snapshot);
  auto result=exact.execute_catalog(metadata,rs::util::Deadline::max());ASSERT_TRUE(result)<<result.error_message();
  EXPECT_EQ((std::vector<std::string>{"SELECT current_database() AS database_name",
      "SHOW COLUMNS FROM TABLE \"db\".\"fixture\".\"a%\";"}),exact.commands);
}
TEST(RedshiftColumnsTablePatterns, UnmatchedMalformedRowsPrecedeFilteringAndOverflowPreservesSnapshot) {
  for (unsigned variant=0;variant<4;++variant) {
    WideColumnsSession session;ResultLimits limits;limits.max_rows=2;ASSERT_TRUE(session.budgets(limits));
    pattern_columns(session,{"a","hidden"},{"a"});
    if (variant==0) session.replies[1]->rows[1][0]="foreign";
    if (variant==1) session.replies[1]->rows[1][2]=std::string{"\xff",1};
    if (variant==2) session.replies[1]->rows[1][3]="invalid-kind";
    if (variant==3) {
      auto raw=std::move(*session.replies[1]);raw.rows[1][4]=std::string(65536,'x');
      session.replies[1]=BackendResult<QueryResult>{std::move(raw),{SessionState::FailedTransaction,SessionDisposition::ResetRequired}};
    }
    auto result=session.execute_catalog(wide_columns(std::string{"a%"}),rs::util::Deadline::max());
    ASSERT_FALSE(result);EXPECT_EQ(BackendErrorClass::InvalidMetadata,result.backend_error().error_class);
    EXPECT_EQ(SessionDisposition::Retire,result.backend_error().disposition);EXPECT_EQ(2u,session.commands.size());
    if (variant==3) {
      EXPECT_EQ(SessionState::FailedTransaction,result.backend_error().session_state);
    }
  }
}
TEST(RedshiftColumnsTablePatterns, FanoutAdmitsSelectedCountButRawHiddenRowsStillConsumeBudget) {
  WideColumnsSession selected;ResultLimits limits;limits.max_results=3;ASSERT_TRUE(selected.budgets(limits));
  pattern_columns(selected,{"a","hidden1","hidden2"},{"a"});
  auto result=selected.execute_catalog(wide_columns(std::string{"a%"}),rs::util::Deadline::max());
  ASSERT_TRUE(result)<<result.error_message();EXPECT_EQ(3u,selected.commands.size());EXPECT_EQ(2u,result->rows.size());
  WideColumnsSession raw;limits=ResultLimits{};limits.max_rows=3;ASSERT_TRUE(raw.budgets(limits));
  pattern_columns(raw,{"a","hidden1","hidden2"},{"a"});
  wide_resource(raw.execute_catalog(wide_columns(std::string{"a%"}),rs::util::Deadline::max()));
  EXPECT_EQ(2u,raw.commands.size());
}
TEST(RedshiftColumnsTablePatterns, LateErrorNeverPublishesPrefixAndFreshBudgetAndDeadlineRecover) {
  for (const unsigned variant:{0u,1u,2u}) {
    WideColumnsSession session;pattern_columns(session,{"aa","az","hidden"},{"aa","az"});
    const auto deadline=rs::util::make_deadline(variant==2?std::chrono::milliseconds{100}:std::chrono::milliseconds{2000});
    if (variant==0) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),"late native denied"};
      error.native_state="42501";error.session_state=SessionState::Idle;error.disposition=SessionDisposition::Reusable;
      session.replies[3]=error;
    }
    if (variant==1) session.replies[3]->rows[0][2]="foreign";
    if (variant==2) session.callback=[&] {
      if (session.commands.size()==4) {
        while (rs::util::Clock::now()<deadline) std::this_thread::yield();
      }
    };
    auto result=session.execute_catalog(wide_columns(std::string{"a%"}),deadline);ASSERT_FALSE(result);
    EXPECT_EQ(4u,session.commands.size());EXPECT_EQ(std::vector<rs::util::Deadline>(4,deadline),session.deadlines);
    if (variant==0) {
      EXPECT_EQ("42501",result.backend_error().native_state);
    }
    if (variant==2) {
      EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::Timeout),result.error());
    }
    session.callback={};session.commands.clear();session.deadlines.clear();session.replies.clear();
    pattern_columns(session,{"fresh","other"},{"fresh"});
    auto restored=session.execute_catalog(wide_columns(std::string{"f%"}),rs::util::Deadline::max());
    ASSERT_TRUE(restored)<<restored.error_message();EXPECT_EQ(2u,restored->rows.size());EXPECT_EQ(3u,session.commands.size());
  }
}
TEST(RedshiftColumnsTablePatterns, VisibleCollisionsRetainDatabaseScopeAndLiteralCatalogSelection) {
  for (const auto& catalog:std::vector<std::optional<std::string>>{std::nullopt,"z_db","%"}) {
    WideColumnsSession session;session.external="on";
    session.replies.emplace_back(visible_databases(),session.snapshot);
    for (const auto& db:{"a_雪","z_db"}) {
      if (catalog && *catalog!=db) continue;
      auto raw=tables_response(db);raw.rows={{db,"fixture","a1","TABLE",std::nullopt,std::nullopt},
          {db,"fixture","hidden","TABLE",std::nullopt,std::nullopt}};
      session.replies.emplace_back(std::move(raw),session.snapshot);
      session.replies.emplace_back(columns_response(db,"fixture","a1"),session.snapshot);
    }
    auto request=wide_columns(std::string{"a_"});request.catalog=catalog;
    auto result=session.execute_catalog(request,rs::util::Deadline::max());ASSERT_TRUE(result)<<result.error_message();
    EXPECT_EQ("SHOW DATABASES;",session.commands.front());
    if (!catalog) {
      ASSERT_EQ(4u,result->rows.size());EXPECT_EQ("a_雪",result->rows[0][0]);EXPECT_EQ("z_db",result->rows[2][0]);
      EXPECT_EQ(5u,session.commands.size());
    } else {
      EXPECT_EQ(*catalog=="%"?0u:2u,result->rows.size());EXPECT_EQ(*catalog=="%"?1u:3u,session.commands.size());
    }
  }
}
TEST(RedshiftColumnsTablePatterns, DestroyedRawAndPerTableStorageReleaseButUnpublishedOwningRowsStayCharged) {
  for (const auto cap:{8u,9u}) {
    WideColumnsSession session;ResultLimits limits;limits.max_rows=cap;ASSERT_TRUE(session.budgets(limits));
    pattern_columns(session,{"aa","az","hidden"},{"aa","az"});
    auto result=session.execute_catalog(wide_columns(std::string{"a%"}),rs::util::Deadline::max());
    if (cap==8) {
      wide_resource(result);
    } else {
      ASSERT_TRUE(result)<<result.error_message();ASSERT_EQ(4u,result->rows.size());
      session.replies.clear();session.connected=false;EXPECT_EQ("az",result->rows[2][2]);
      EXPECT_EQ("owning remark",result->rows[3][11]);
    }
    EXPECT_EQ(4u,session.commands.size());
  }
}
TEST(RedshiftColumnsTablePatterns, CallerPatternNameAndUnmatchedNativeNameBoundsRefuseWithoutReplay) {
  WideColumnsSession caller;ResultLimits limits;limits.max_column_name_bytes=24;ASSERT_TRUE(caller.budgets(limits));
  pattern_columns(caller,{"a"},{"a"});
  wide_resource(caller.execute_catalog(wide_columns(std::string(25,'%')),rs::util::Deadline::max()));
  EXPECT_TRUE(caller.commands.empty());
  WideColumnsSession hidden;ASSERT_TRUE(hidden.budgets(limits));
  pattern_columns(hidden,{"a",std::string(25,'z')},{"a"});
  wide_resource(hidden.execute_catalog(wide_columns(std::string{"a%"}),rs::util::Deadline::max()));
  EXPECT_EQ(2u,hidden.commands.size());
}
