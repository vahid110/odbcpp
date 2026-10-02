#pragma once

#include "odbc_types.h"

namespace rs::odbc {

class ResultTypes {
public:
  // Normalize only operational types; descriptors retain the caller's type.
  static SQLSMALLINT canonical_c_type(SQLSMALLINT c_type);
  static SQLSMALLINT default_c_type(SQLSMALLINT sql_type);
  static bool is_valid_c_type(SQLSMALLINT c_type);
  static bool is_supported_c_type(SQLSMALLINT c_type);
  static bool is_supported_parameter_c_type(SQLSMALLINT c_type);
  static bool is_valid_sql_type(SQLSMALLINT sql_type);
  static bool is_supported_parameter_sql_type(SQLSMALLINT sql_type);
  static bool is_conversion_supported(SQLSMALLINT sql_type,
                                      SQLSMALLINT c_type);
};

} // namespace rs::odbc
