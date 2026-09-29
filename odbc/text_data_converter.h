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
                                ConversionIssue* issue = nullptr,
                                SQLSMALLINT numeric_precision = 38,
                                SQLSMALLINT numeric_scale = 0);
  static std::optional<std::string> format_numeric(
      const SQL_NUMERIC_STRUCT& numeric, SQLSMALLINT precision,
      SQLSMALLINT scale, ConversionIssue* issue = nullptr);

};

} // namespace rs::odbc
