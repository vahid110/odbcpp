#pragma once

#include "native_type_info.h"
#include "backend_result.h"
#include "statement_kind.h"
#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rs::core::database {

// Names and normalized_type are common metadata. IDs, size/modifier, table
// provenance and format_code remain parser/backend migration fields. Shared
// ODBC column mapping uses normalized_type exclusively, never these native IDs.
struct ResultColumnMetadata {
  // UTF-8, without embedded NULs; empty names are valid.
  std::string name;
  std::uint32_t table_id{0};
  std::int16_t table_column{0};
  std::uint32_t type_id{0};
  std::int16_t type_size{-1};
  std::int32_t type_modifier{-1};
  std::int16_t format_code{0};
  // Backend/session results and locally synthesized results must provide this
  // normalized metadata. Shared ODBC code never interprets column native IDs.
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
// calls, disconnect and backend destruction. Deferred server errors belong to
// their result as BackendError; native_state must be normalized by the backend.
// command_tag and parameter_type_ids are native, not portable SQL semantics.
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
  std::vector<std::uint32_t> parameter_type_ids;
  std::string command_tag;
  std::optional<BackendError> error;
  std::size_t affected_rows{0};
  std::vector<QueryResult> additional_results;
  // Absent means no completion metadata; explicit Unknown clears prior dynamic
  // function diagnostics. Shared consumers never parse native command_tag.
  std::optional<StatementKind> statement_kind;
  // Owning, ordered parameter descriptions supplied by the backend. Native IDs
  // above remain parser migration storage and are never consumed by ODBC.
  std::vector<NativeTypeInfo> normalized_parameter_types;
  std::vector<CellEncodingError> cell_errors;

};

} // namespace rs::core::database
