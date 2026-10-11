#pragma once

#include "odbcpp/database/query_result.h"
#include <cstdint>

namespace rs::core::database {

// Private PostgreSQL-family parser storage, never returned by an SDK session.
// Cells still contain native encodings; the session normalizes them before
// publishing QueryResult. Additional results are a flat, ordered sequence.
struct ParsedColumnMetadata {
  std::string name;
  std::uint32_t type_id{0};
  std::int16_t type_size{-1};
  std::int32_t type_modifier{-1};
  std::int16_t format_code{0};
};

struct ParsedQueryResult {
  ResultRows rows;
  std::vector<ParsedColumnMetadata> columns;
  std::vector<std::uint32_t> parameter_type_ids;
  std::optional<BackendError> error;
  std::size_t affected_rows{0};
  std::vector<ParsedQueryResult> additional_results;
  std::optional<StatementKind> statement_kind;
  std::optional<ExecutionResultShape> execution_result_shape;
  // Backend-private evidence: BindComplete followed by portal T/n and C.
  bool prepared_execution_authority{false};
};

} // namespace rs::core::database
