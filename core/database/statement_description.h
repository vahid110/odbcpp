#pragma once

#include "backend_result.h"
#include "query_parameter.h"
#include "query_result.h"
#include "core/util/deadline.h"
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
  virtual BackendResult<QueryResult> describe_statement(std::string_view sql,
      std::span<const QueryParameterType> parameter_types,
      rs::util::Deadline deadline) = 0;
};

} // namespace rs::core::database
