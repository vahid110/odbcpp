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

} // namespace rs::core::database
