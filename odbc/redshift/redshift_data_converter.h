#pragma once
#include "../odbc_types.h"
#include <string>

namespace rs::odbc::redshift {

// Redshift-specific data conversion
class RedshiftDataConverter {
public:
    // Convert Redshift string value to requested C type
    static SQLRETURN convert_data(const std::string& redshift_value, 
                                 SQLSMALLINT target_c_type,
                                 void* buffer, 
                                 SQLLEN buffer_length, 
                                 SQLLEN* indicator);

private:
    // Redshift-specific conversion helpers
    static SQLRETURN convert_to_integer(const std::string& value, void* buffer, SQLLEN* indicator);
    static SQLRETURN convert_to_bigint(const std::string& value, void* buffer, SQLLEN* indicator);
    static SQLRETURN convert_to_double(const std::string& value, void* buffer, SQLLEN* indicator);
    static SQLRETURN convert_to_boolean(const std::string& value, void* buffer, SQLLEN* indicator);
    static SQLRETURN convert_to_date(const std::string& value, void* buffer, SQLLEN* indicator);
    static SQLRETURN convert_to_timestamp(const std::string& value, void* buffer, SQLLEN* indicator);
    static SQLRETURN convert_to_string(const std::string& value, void* buffer, SQLLEN buffer_length, SQLLEN* indicator);
};

} // namespace rs::odbc::redshift