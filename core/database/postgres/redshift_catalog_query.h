#pragma once

#include "odbcpp/database/catalog_request.h"
#include "odbcpp/database/query_result.h"
#include "odbcpp/database/i_database_connection.h"
#include <string>
#include <utility>

namespace rs::core::database::postgres {

// Exact authenticated ParameterStatus policy. Missing flags mean off; invalid
// values are refused, including flags whose value cannot change the outcome.
rs::util::Result<CatalogDatabaseScope> redshift_catalog_database_scope(
    CatalogDatabaseScope requested, const std::optional<std::string>& datashare_enabled,
    const std::optional<std::string>& external_database);
BackendResult<std::vector<std::string>> normalize_redshift_databases(
    BackendResult<QueryResult> input);
rs::util::Result<bool> redshift_database_matches(std::string_view database,
    const std::optional<std::string>& requested, bool pattern);

// UTF8 native-profile matching: ASCII native fold for unquoted identifiers,
// exact quoted bytes, and checked request-specific semantic slots.
std::optional<CatalogRequest> native_catalog_request(const CatalogRequest& request);
// Private lowering of validated identifier names to escaped literal LIKE carriers.
// Other request families and enumeration modes are not lowered.
std::optional<CatalogRequest> redshift_identifier_show_request(const CatalogRequest& validated_native);
// Explicit Redshift catalog queries; broader parity remains separate work.
std::string redshift_schemas_query(bool visible_databases = false);
// Fixed current-database discovery and owning SHOW normalization. No SQL fallback.
BackendResult<std::string> redshift_schema_database(BackendResult<QueryResult> input);
// Borrowed validated lookup, for admission before allocating its owning copy.
BackendResult<std::string_view> redshift_schema_database_view(const BackendResult<QueryResult>& input);
std::string redshift_show_schemas_command(std::string_view validated_database);
BackendResult<QueryResult> normalize_redshift_schemas(
    std::string_view database, BackendResult<QueryResult> input);
// Narrow SHOW TABLES eligibility: schema LIKE pattern denotes one literal.
bool redshift_table_schema_is_literal(const TablesCatalogRequest& request) noexcept;
std::optional<std::string> redshift_table_schema(const TablesCatalogRequest& request);
// Ordinary SQLTables nonliteral-schema SHOW fanout; exact and enumeration paths stay separate.
bool redshift_tables_schema_wide(const TablesCatalogRequest& request) noexcept;
bool redshift_tables_patterns_valid(const TablesCatalogRequest& request);
std::string redshift_show_tables_command(std::string_view database, std::string_view schema);
BackendResult<QueryResult> normalize_redshift_tables(std::string_view database,
    std::string_view schema, const TablesCatalogRequest& request, BackendResult<QueryResult> input,
    bool detailed_types = true);
// Scoped per-table SHOW COLUMNS; schema/table LIKE patterns must be literals.
bool redshift_column_names_are_literal(const ColumnsCatalogRequest& request) noexcept;
std::optional<std::pair<std::string,std::string>> redshift_column_names(const ColumnsCatalogRequest& request);
std::string redshift_show_columns_command(std::string_view database,std::string_view schema,std::string_view table);
BackendResult<QueryResult> normalize_redshift_columns(std::string_view database,std::string_view schema,
    std::string_view table,const ColumnsCatalogRequest& request,BackendResult<QueryResult> input);
// Ordinary schema/table-pattern discovery. A literal schema/table pair retains
// its direct path; metadata-ID requests are lowered separately.
bool redshift_columns_schema_wide(const ColumnsCatalogRequest& request) noexcept;
bool redshift_columns_patterns_valid(const ColumnsCatalogRequest& request);
std::optional<std::string> redshift_columns_schema(const ColumnsCatalogRequest& request);
// Backend-owned routine identities and budget accounting, not a new SDK facet.
struct RedshiftRoutine {
  std::string name;
  bool function{false};
  std::vector<std::string> arguments;
  std::string signature;
  ResultCell return_type, remarks;
};
struct RedshiftRoutineUsage {
  std::size_t rows{0}, cells{0}, bytes{0}, metadata{0}, name_bytes{0};
};
class RedshiftRoutineBudget {
 public:
  RedshiftRoutineBudget(const ResponseLimits& response, const ResultLimits& result)
      : response_(response), result_(result) {}
  bool retain(RedshiftRoutineUsage extra) noexcept;
  void release(RedshiftRoutineUsage used) noexcept;
  bool exchange() noexcept;
  bool exchanges_available(std::size_t count) const noexcept;
  bool name(std::string_view value) const noexcept;
  bool description(std::size_t count) const noexcept;
  bool arguments(std::size_t count) const noexcept;
 private:
  ResponseLimits response_;
  ResultLimits result_;
  RedshiftRoutineUsage used_;
  std::size_t exchanges_{0};
};
RedshiftRoutineUsage redshift_routine_usage(const QueryResult& result);
// Current-database SQL_ALL_SCHEMAS only. Existing pure and Visible callers stay
// on their unchanged entry; raw storage is released by its actual owner.
BackendError redshift_current_schema_limit(SessionSnapshot snapshot);
std::optional<RedshiftRoutineUsage> redshift_current_schema_raw_storage(const QueryResult& source);
std::optional<RedshiftRoutineUsage> redshift_current_schema_database_storage(std::string_view database);
BackendResult<QueryResult> normalize_redshift_current_schemas(std::string_view database,
    const BackendResult<QueryResult>& input, RedshiftRoutineBudget& budget,
    RedshiftRoutineUsage& raw_retained, rs::util::Deadline deadline);

// Exact native identity/signature/token charges retained by the routine
// normalizer. Release only after the owning routine vector is destroyed.
RedshiftRoutineUsage redshift_routine_storage_usage(const std::vector<RedshiftRoutine>& routines);
BackendError redshift_routine_limit();
std::optional<std::string> redshift_routine_schema(const CatalogRequest& request);
// Backend-private SHOW schema selection. The caller keeps the raw response
// charged until its owning storage is destroyed, and keeps selected identities
// charged through routine/parameter fanout. Failure aborts the request budget.
struct RedshiftRoutineSchemas {
  std::vector<std::string> names;
  RedshiftRoutineUsage retained;
};
RedshiftRoutineUsage redshift_routine_schema_usage(const QueryResult& source);
BackendResult<RedshiftRoutineSchemas> normalize_redshift_routine_schemas(
    std::string_view database, const CatalogRequest& request,
    const BackendResult<QueryResult>& input, RedshiftRoutineBudget& budget,
    rs::util::Deadline deadline);
// Closed SHOW TABLES identity selection. Raw storage stays charged until the
// caller destroys it; selected exact identifiers remain charged during fanout.
struct RedshiftColumnTables {
  std::vector<std::string> names;
  RedshiftRoutineUsage retained, raw_retained;
};
// Closed native SHOW SCHEMAS identities; raw and selected names have separate
// destruction boundaries. No routine-request lowering or shared matcher repair.
struct RedshiftColumnSchemas {
  std::vector<std::string> names;
  RedshiftRoutineUsage retained, raw_retained;
};
BackendResult<RedshiftColumnSchemas> redshift_column_schemas(std::string_view database,
    const std::optional<std::string>& schema_pattern,
    const BackendResult<QueryResult>& input,
    RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
BackendResult<RedshiftColumnTables> redshift_column_tables(std::string_view database,
    std::string_view schema, const std::optional<std::string>& table_pattern,
    const BackendResult<QueryResult>& input,
    RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
BackendResult<QueryResult> redshift_columns_headers(RedshiftRoutineBudget& budget);
BackendResult<QueryResult> redshift_columns_part(std::string_view database, std::string_view schema,
    std::string_view table, const ColumnsCatalogRequest& request,
    BackendResult<QueryResult> input, RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
RedshiftRoutineUsage redshift_columns_output_usage(const QueryResult& result);
// Closed SQLTables SHOW framing/owning projection, using existing native budget primitives.
BackendResult<RedshiftColumnSchemas> redshift_table_databases(bool visible,
    const BackendResult<QueryResult>& input, RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
BackendResult<QueryResult> redshift_tables_headers(RedshiftRoutineBudget& budget);
BackendResult<QueryResult> redshift_tables_part(std::string_view database, std::string_view schema,
    const TablesCatalogRequest& request, BackendResult<QueryResult> input,
    RedshiftRoutineBudget& budget, rs::util::Deadline deadline, bool detailed_types);
RedshiftRoutineUsage redshift_tables_output_usage(const QueryResult& result);
std::string redshift_show_routines_command(std::string_view database, std::string_view schema, bool function);
std::string redshift_show_parameters_command(std::string_view database, std::string_view schema,
    const RedshiftRoutine& routine);
BackendResult<std::vector<RedshiftRoutine>> normalize_redshift_routines(std::string_view database,
    std::string_view schema, bool function, const CatalogRequest& request,
    BackendResult<QueryResult> input, RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
BackendResult<QueryResult> redshift_routine_result(std::string_view database, std::string_view schema,
    const std::vector<RedshiftRoutine>& routines, bool columns, RedshiftRoutineBudget& budget);
BackendResult<QueryResult> normalize_redshift_parameters(std::string_view database, std::string_view schema,
    const RedshiftRoutine& routine, const ProcedureColumnsCatalogRequest& request,
    BackendResult<QueryResult> input, RedshiftRoutineBudget& budget, rs::util::Deadline deadline);
void sort_redshift_parameters(QueryResult& result);
std::string redshift_statistics_query(const StatisticsCatalogRequest& request);
std::string redshift_row_version_query();
std::string redshift_columns_query(const ColumnsCatalogRequest& request, bool visible_databases = false);

}  // namespace rs::core::database::postgres
