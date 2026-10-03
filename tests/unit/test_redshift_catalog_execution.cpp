#include "core/database/postgres/pg_database_connection.h"
#include <gtest/gtest.h>
#include <array>

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
