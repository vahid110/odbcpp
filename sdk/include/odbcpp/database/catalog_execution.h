#pragma once

#include "odbcpp/database/catalog_request.h"
#include "odbcpp/database/query_result.h"
#include "odbcpp/util/deadline.h"

namespace rs::core::database {

// Optional session-owned execution facet, stable for the session lifetime.
// Presence proves neither connection nor support for any request kind. Inputs
// are borrowed only until return; every nested operation reuses the caller's
// absolute deadline. Results own normalized catalog rows/metadata and their
// final session snapshot, rather than exposing backend-specific raw discovery.
// Unsupported requests and native failures never select a SQL fallback/replay.
// Disconnected sessions return NotConnected. Capability selection belongs to
// each backend; this boundary advertises no PostgreSQL or Redshift behavior.
class ICatalogExecution {
 public:
  virtual ~ICatalogExecution() = default;
  // Passive backend-owned routing; selection is independent of whether the
  // authenticated server can fulfill the request. Execution failures never
  // switch to generated SQL. Existing facets retain primary-key selection.
  virtual bool selects_catalog_request(const CatalogRequest& request) const noexcept {
    return std::holds_alternative<PrimaryKeysCatalogRequest>(request);
  }
  virtual BackendResult<QueryResult> execute_catalog(
      const CatalogRequest& request, rs::util::Deadline deadline) = 0;
};

} // namespace rs::core::database
