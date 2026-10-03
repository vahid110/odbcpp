#pragma once

#include "core/database/catalog_request.h"
#include <string>

namespace rs::core::database::postgres {

// Explicit Redshift catalog queries; broader parity remains separate work.
std::string redshift_statistics_query(const StatisticsCatalogRequest& request);
std::string redshift_row_version_query();
std::string redshift_columns_query(const ColumnsCatalogRequest& request);

}  // namespace rs::core::database::postgres
