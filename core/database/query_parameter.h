#pragma once

#include <optional>
#include <string>

namespace rs::core::database {

// Database-neutral parameter types. Protocol implementations translate these
// hints to native type identifiers and send value bytes separately from SQL.
enum class QueryParameterType {
  Unspecified,
  Text,
  Int16,
  Int32,
  Int64,
  Float32,
  Float64,
  Numeric,
  Boolean,
  Binary,
  Date,
  Time,
  Timestamp,
};

// Values own their storage; the caller keeps the parameter span alive until the
// synchronous call returns. nullopt and engaged empty strings remain distinct.
// Text/numeric/temporal values use the current textual conversion contract.
// Binary currently carries PostgreSQL bytea text, not arbitrary raw bytes; moving
// that encoding below the backend boundary remains required before G9a closure.
struct QueryParameter {
  std::optional<std::string> value;
  QueryParameterType type{QueryParameterType::Unspecified};
};

} // namespace rs::core::database
