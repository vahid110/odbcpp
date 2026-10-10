#pragma once

#include "odbcpp/database/backend_result.h"
#include "odbcpp/database/query_parameter.h"
#include "odbcpp/database/query_result.h"
#include "odbcpp/util/deadline.h"
#include <span>
#include <string_view>

namespace rs::core::database {

// Optional session-owned facet, stable for the session lifetime, including
// disconnected state. Presence does not imply open. Calls are serialized and
// reuse the caller's absolute deadline. Inputs are borrowed only until return;
// results own metadata and the final passive session snapshot. Descriptions
// contain no rows, execution sequence or cell-error ledger.
class IStatementDescription {
 public:
  virtual ~IStatementDescription() = default;
  // Stable support declaration, not a probe. True promises a single prepared
  // statement and explicit ResultSet/NoResultSet authority even with zero fields.
  virtual bool supports_single_statement_result_shape() const noexcept { return false; }
  virtual BackendResult<QueryResult> describe_statement(std::string_view sql,
      std::span<const QueryParameterType> parameter_types,
      rs::util::Deadline deadline) = 0;
};

} // namespace rs::core::database
