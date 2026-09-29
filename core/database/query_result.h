#pragma once

#include "native_type_info.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rs::core::database {

// Names and normalized_type are common metadata. IDs, size/modifier, table
// provenance and format_code are opaque backend fields; shared callers must use
// describe_type/resolve_types rather than interpreting native numeric values.
struct ResultColumnMetadata {
  std::string name;
  std::uint32_t table_id{0};
  std::int16_t table_column{0};
  std::uint32_t type_id{0};
  std::int16_t type_size{-1};
  std::int32_t type_modifier{-1};
  std::int16_t format_code{0};
  // Locally synthesized results can provide normalized metadata without native IDs.
  std::optional<NativeTypeInfo> normalized_type{};
};

// Owning bytes: nullopt is SQL NULL; an engaged empty string is a non-NULL empty
// value. Current PostgreSQL results use text format, including native bytea and
// boolean representations (normalization of those remains an A4 work item).
using ResultCell = std::optional<std::string>;
using ResultRow = std::vector<ResultCell>;
using ResultRows = std::vector<ResultRow>;

// Fully owning snapshot: rows, metadata and additional results survive later
// calls, disconnect and backend destruction. Deferred server errors belong to
// their result; error_sqlstate is native and must be normalized by the backend.
// command_tag and parameter_type_ids are native, not portable SQL semantics.
struct QueryResult {
  ResultRows rows;
  std::vector<ResultColumnMetadata> columns;
  std::vector<std::uint32_t> parameter_type_ids;
  std::string command_tag;
  std::string error_message;
  std::string error_sqlstate;
  std::size_t affected_rows{0};
  std::vector<QueryResult> additional_results;
};

} // namespace rs::core::database
