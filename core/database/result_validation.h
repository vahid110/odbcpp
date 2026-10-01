#pragma once

#include "query_result.h"
#include "core/util/utf8.h"

namespace rs::core::database {

// Empty names are valid; embedded NULs are not representable in ODBC names.
// Validate before publishing any result state. Cell encoding errors are separate.
template <typename Result>
inline bool valid_result_structure(const Result& result) {
  for (const auto& column : result.columns) {
    if (column.name.find('\0') != std::string::npos ||
        !rs::util::utf8_code_point_count(column.name)) return false;
  }
  for (const auto& row : result.rows) {
    if (row.size() != result.columns.size()) return false;
  }
  return true;
}

// Materialized execution is one primary item followed by a flat ordered list.
// Operation failures use BackendResult's error alternative. A deferred error is
// an error-only item; accepting data alongside it would silently discard data.
inline bool valid_execution_structure(const QueryResult& result) {
  if (result.error) return false;
  for (const auto& item : result.additional_results) {
    if (!item.additional_results.empty()) return false;
    if (item.error && (!item.rows.empty() || !item.columns.empty() ||
        !item.cell_errors.empty() || !item.normalized_parameter_types.empty() ||
        item.affected_rows != 0 || item.statement_kind)) return false;
  }
  return true;
}

// Description publishes metadata only, never execution rows or queued items.
inline bool valid_description_structure(const QueryResult& result) {
  return valid_execution_structure(result) && result.additional_results.empty() &&
      result.rows.empty() && result.cell_errors.empty();
}

} // namespace rs::core::database
