#pragma once

#include "catalog_request.h"
#include "core/util/result.h"
#include <string>

namespace rs::core::database {

// Optional session-owned facet, stable for its session lifetime, including
// disconnected state. Query construction performs no I/O or session mutation;
// requests are borrowed until return and returned SQL owns its storage. The
// adapter executes supported queries through the normal execution contract,
// retaining its timeout, diagnostics and result validation. Individual requests
// may return UnsupportedFeature even when the facet is present.
class ICatalogQueries {
 public:
  virtual ~ICatalogQueries() = default;
  virtual rs::util::Result<std::string> catalog_query(
      const CatalogRequest& request) const = 0;
};

} // namespace rs::core::database
