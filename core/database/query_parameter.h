#pragma once

#include <optional>
#include <string>

namespace rs::core::database {

// Database-neutral parameter types. Protocol implementations translate these
// hints to native type identifiers and send value bytes separately from SQL.
enum class QueryParameterType {
  Unspecified,
  Text,
  Int32,
  Int64,
  Float64,
  Numeric,
  Boolean,
  Binary,
};

struct QueryParameter {
  std::optional<std::string> value;
  QueryParameterType type{QueryParameterType::Unspecified};
};

} // namespace rs::core::database
