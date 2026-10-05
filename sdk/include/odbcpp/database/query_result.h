#pragma once

#include "odbcpp/database/native_type_info.h"
#include "odbcpp/database/backend_result.h"
#include "odbcpp/database/statement_kind.h"
#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rs::core::database {

// Backend-neutral, owning metadata. Native descriptors remain inside the backend.
struct ResultColumnMetadata {
  // UTF-8, without embedded NULs; empty names are valid.
  std::string name;
  // Backend/session results and locally synthesized results must provide this
  // normalized metadata.
  std::optional<NativeTypeInfo> normalized_type{};
};

// Owning canonical bytes: nullopt is SQL NULL; an engaged empty string is a
// non-NULL empty value. Binary is raw bytes, Boolean is "0"/"1";
// text is valid UTF-8, including embedded NULs. Malformed native binary/Boolean
// or text cells use an empty placeholder and a cell_errors entry.
using ResultCell = std::optional<std::string>;
// Every row has exactly one cell per schema column.
using ResultRow = std::vector<ResultCell>;
using ResultRows = std::vector<ResultRow>;

// Fully owning snapshot: rows, metadata and additional results survive later
// calls, disconnect and backend destruction. Additional results form a flat,
// ordered sequence; nested additional results are invalid. A primary error uses
// BackendResult rather than the success alternative. Deferred server errors belong to
// an error-only additional result as BackendError; native_state must be normalized
// by the backend. Error-only items carry no schema, rows, parameter descriptions,
// cell errors or completion metadata.
// Deferred data errors preserve fetch/GetData timing without retaining native
// encodings. Coordinates are zero-based, sorted, unique and refer to non-NULL
// cells. The whole error snapshot owns its storage independently of the session.
struct CellEncodingError {
  std::size_t row{};
  std::size_t column{};
  auto operator<=>(const CellEncodingError&) const = default;
};

struct QueryResult {
  ResultRows rows;
  std::vector<ResultColumnMetadata> columns;
  std::optional<BackendError> error;
  std::size_t affected_rows{0};
  std::vector<QueryResult> additional_results;
  // Absent means no completion metadata; explicit Unknown clears prior dynamic
  // function diagnostics. Native completion tags never cross this boundary.
  std::optional<StatementKind> statement_kind;
  // Owning, ordered parameter descriptions supplied by the backend.
  std::vector<NativeTypeInfo> normalized_parameter_types;
  std::vector<CellEncodingError> cell_errors;

};

} // namespace rs::core::database
