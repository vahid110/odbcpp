#pragma once

#include "core/database/catalog_request.h"
#include <string>

namespace rs::core::database::postgres {

// Redshift column discovery only; SHOW/datashare/external parity is separate work.
std::string redshift_columns_query(const ColumnsCatalogRequest& request);

}  // namespace rs::core::database::postgres
