#pragma once

#include "native_type_info.h"
#include "backend_result.h"
#include "statement_kind.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rs::core::database {

// Names and normalized_type are common metadata. IDs, size/modifier, table
// provenance and format_code remain parser/backend migration fields. Shared
// ODBC column mapping uses normalized_type exclusively, never these native IDs.
struct ResultColumnMetadata {
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

// Owning bytes: nullopt is SQL NULL; an engaged empty string is a non-NULL empty
// value. Backend cell bytes remain opaque until normalize_result_value converts
// binary/boolean values to raw bytes or "0"/"1" for shared ODBC conversion.
using ResultCell = std::optional<std::string>;
using ResultRow = std::vector<ResultCell>;
using ResultRows = std::vector<ResultRow>;

// Fully owning snapshot: rows, metadata and additional results survive later
// calls, disconnect and backend destruction. Deferred server errors belong to
// their result as BackendError; native_state must be normalized by the backend.
// command_tag and parameter_type_ids are native, not portable SQL semantics.
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
};

} // namespace rs::core::database
