#pragma once

#include <cstdint>
#include <unordered_map>

namespace rs::core::database {

// Normalized scalar families; these are not database IDs or ODBC constants.
enum class ScalarType {
  Boolean, Binary, Char, VarChar, BigInt, SmallInt, Integer, Real, Double,
  Date, Time, Timestamp, Numeric, Decimal, LongVarChar
};

struct NativeTypeInfo {
  ScalarType type{ScalarType::VarChar};
  std::uint64_t column_size{0};
  std::int16_t decimal_digits{0};
  // False means the backend used its fallback, e.g. an unresolved user type.
  bool known{false};
};

using ResolvedTypeMap = std::unordered_map<std::uint32_t, NativeTypeInfo>;

} // namespace rs::core::database
