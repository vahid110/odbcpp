#pragma once
#include "../odbc_types.h"
#include <string>

namespace rs::odbc::redshift {

// Redshift-specific ODBC type mappings
class RedshiftTypes {
public:
    // Convert Redshift database type to ODBC SQL type
    static SQLSMALLINT get_sql_type(const std::string& redshift_type);
    
    // Get default C type for SQL type
    static SQLSMALLINT get_default_c_type(SQLSMALLINT sql_type);
    
    // Check if conversion is supported
    static bool is_conversion_supported(SQLSMALLINT sql_type, SQLSMALLINT c_type);
};

} // namespace rs::odbc::redshift