#pragma once

#include "odbcpp/database/query_result.h"

namespace rs::core::database {

// Materialized execution is one primary item followed by a flat ordered list.
// Operation failures use BackendResult's error alternative. A deferred error is
// an error-only item; accepting data alongside it would silently discard data.
inline bool valid_execution_structure(const QueryResult& result) {
  if (result.error) return false;
  for (const auto& item : result.additional_results) {
    if (!item.additional_results.empty()) return false;
    if (item.error && (!item.rows.empty() || !item.columns.empty() ||
        !item.cell_errors.empty() || !item.normalized_parameter_types.empty() ||
        item.affected_rows != 0 || item.statement_kind || item.described_result_shape || item.execution_result_shape)) return false;
  }
  return true;
}

// Description publishes metadata only, never execution rows or queued items.
inline bool valid_description_structure(const QueryResult& result) {
  return valid_execution_structure(result) && !result.execution_result_shape &&
      !result.statement_kind && result.affected_rows == 0 && result.additional_results.empty() &&
      result.rows.empty() && result.cell_errors.empty() &&
      (!result.described_result_shape ||
       *result.described_result_shape == DescribedResultShape::ResultSet ||
       (*result.described_result_shape == DescribedResultShape::NoResultSet && result.columns.empty()));
}


} // namespace rs::core::database
