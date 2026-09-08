#pragma once

#include "odbc_types.h"

#include <string>

namespace rs::odbc {

class TextDataConverter {
public:
  static SQLRETURN convert_data(const std::string& value,
                                SQLSMALLINT target_c_type,
                                void* buffer,
                                SQLLEN buffer_length,
                                SQLLEN* indicator);
};

} // namespace rs::odbc
