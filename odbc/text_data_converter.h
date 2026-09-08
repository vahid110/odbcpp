#pragma once

#include "odbc_types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace rs::odbc {

class TextDataConverter {
public:
  static SQLRETURN convert_data(const std::string& value,
                                SQLSMALLINT target_c_type,
                                void* buffer,
                                SQLLEN buffer_length,
                                SQLLEN* indicator);
  // PostgreSQL text-protocol representation, including hex and legacy escape.
  static std::optional<std::vector<std::byte>> decode_binary(
      std::string_view value);
};

} // namespace rs::odbc
