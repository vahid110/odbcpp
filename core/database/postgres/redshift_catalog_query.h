#pragma once

#include "odbcpp/database/catalog_request.h"
#include "odbcpp/database/query_result.h"
#include <string>

namespace rs::core::database::postgres {

// Explicit Redshift catalog queries; broader parity remains separate work.
std::string redshift_schemas_query();
// Fixed current-database discovery and owning SHOW normalization. No SQL fallback.
BackendResult<std::string> redshift_schema_database(BackendResult<QueryResult> input);
std::string redshift_show_schemas_command(std::string_view validated_database);
BackendResult<QueryResult> normalize_redshift_schemas(
    std::string_view database, BackendResult<QueryResult> input);
std::string redshift_statistics_query(const StatisticsCatalogRequest& request);
std::string redshift_row_version_query();
std::string redshift_columns_query(const ColumnsCatalogRequest& request);

}  // namespace rs::core::database::postgres
