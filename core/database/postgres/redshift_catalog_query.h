#pragma once

#include "odbcpp/database/catalog_request.h"
#include "odbcpp/database/query_result.h"
#include "odbcpp/database/i_database_connection.h"
#include <string>
#include <utility>

namespace rs::core::database::postgres {

// UTF8 native-profile matching: ASCII native fold for unquoted identifiers,
// exact quoted bytes, and checked request-specific semantic slots.
std::optional<CatalogRequest> native_catalog_request(const CatalogRequest& request);
// Private lowering of validated identifier names to escaped literal LIKE carriers.
// Other request families and enumeration modes are not lowered.
std::optional<CatalogRequest> redshift_identifier_show_request(const CatalogRequest& validated_native);
// Explicit Redshift catalog queries; broader parity remains separate work.
std::string redshift_schemas_query();
// Fixed current-database discovery and owning SHOW normalization. No SQL fallback.
BackendResult<std::string> redshift_schema_database(BackendResult<QueryResult> input);
std::string redshift_show_schemas_command(std::string_view validated_database);
BackendResult<QueryResult> normalize_redshift_schemas(
    std::string_view database, BackendResult<QueryResult> input);
// Narrow SHOW TABLES eligibility: schema LIKE pattern denotes one literal.
bool redshift_table_schema_is_literal(const TablesCatalogRequest& request) noexcept;
std::optional<std::string> redshift_table_schema(const TablesCatalogRequest& request);
std::string redshift_show_tables_command(std::string_view database, std::string_view schema);
BackendResult<QueryResult> normalize_redshift_tables(std::string_view database,
    std::string_view schema, const TablesCatalogRequest& request, BackendResult<QueryResult> input);
// Scoped per-table SHOW COLUMNS; schema/table LIKE patterns must be literals.
bool redshift_column_names_are_literal(const ColumnsCatalogRequest& request) noexcept;
std::optional<std::pair<std::string,std::string>> redshift_column_names(const ColumnsCatalogRequest& request);
std::string redshift_show_columns_command(std::string_view database,std::string_view schema,std::string_view table);
BackendResult<QueryResult> normalize_redshift_columns(std::string_view database,std::string_view schema,
    std::string_view table,const ColumnsCatalogRequest& request,BackendResult<QueryResult> input);
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
BackendError redshift_routine_limit();
std::optional<std::string> redshift_routine_schema(const CatalogRequest& request);
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
std::string redshift_columns_query(const ColumnsCatalogRequest& request);

}  // namespace rs::core::database::postgres
