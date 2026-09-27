#pragma once

#include "native_type_info.h"
#include <optional>
#include <string_view>

namespace rs::core::database {

// Backend-advertised SQL types, independent of ODBC identifiers and native IDs.
struct TypeDefinition {
  ScalarType type;
  std::string_view name;
  std::uint64_t column_size;
  std::optional<std::string_view> literal_prefix, literal_suffix, create_params;
  bool case_sensitive;
  std::optional<bool> unsigned_attribute;
  std::optional<std::int16_t> minimum_scale, maximum_scale;
  std::int32_t numeric_radix;
};

} // namespace rs::core::database
