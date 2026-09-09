#pragma once

#include "odbc_types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <optional>
#include <span>
#include <vector>

namespace rs::odbc {

enum class ConversionIssue {
  None,
  FractionalTruncation,
  NumericValueOutOfRange,
  InvalidCharacterValue,
  InvalidDatetimeFormat,
};

class TextDataConverter {
public:
  static SQLRETURN convert_data(const std::string& value,
                                SQLSMALLINT target_c_type,
                                void* buffer,
                                SQLLEN buffer_length,
                                SQLLEN* indicator,
                                ConversionIssue* issue = nullptr);
  // PostgreSQL text-protocol representation, including hex and legacy escape.
  static std::optional<std::vector<std::byte>> decode_binary(
      std::string_view value);
  static std::string encode_binary(std::span<const std::byte> value);
};

} // namespace rs::odbc
