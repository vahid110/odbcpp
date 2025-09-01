#include "redshift_data_converter.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>

namespace rs::odbc::redshift {

SQLRETURN RedshiftDataConverter::convert_data(const std::string& redshift_value, 
                                             SQLSMALLINT target_c_type,
                                             void* buffer, 
                                             SQLLEN buffer_length, 
                                             SQLLEN* indicator) {
    if (!buffer) return SQL_ERROR;
    
    switch (target_c_type) {
        case SQL_C_CHAR:
            return convert_to_string(redshift_value, buffer, buffer_length, indicator);
        case SQL_C_SLONG:
            return convert_to_integer(redshift_value, buffer, indicator);
        case SQL_C_SBIGINT:
            return convert_to_bigint(redshift_value, buffer, indicator);
        case SQL_C_DOUBLE:
            return convert_to_double(redshift_value, buffer, indicator);
        case SQL_C_BIT:
            return convert_to_boolean(redshift_value, buffer, indicator);
        case SQL_C_DATE:
            return convert_to_date(redshift_value, buffer, indicator);
        case SQL_C_TIMESTAMP:
            return convert_to_timestamp(redshift_value, buffer, indicator);
        default:
            return SQL_ERROR;
    }
}

SQLRETURN RedshiftDataConverter::convert_to_string(const std::string& value, void* buffer, SQLLEN buffer_length, SQLLEN* indicator) {
    if (buffer_length <= 0) return SQL_ERROR;
    
    size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), value.length());
    std::memcpy(buffer, value.c_str(), copy_len);
    static_cast<char*>(buffer)[copy_len] = '\0';
    
    if (indicator) {
        *indicator = static_cast<SQLLEN>(value.length());
    }
    
    return (value.length() >= static_cast<size_t>(buffer_length)) ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

SQLRETURN RedshiftDataConverter::convert_to_integer(const std::string& value, void* buffer, SQLLEN* indicator) {
    try {
        SQLINTEGER result = static_cast<SQLINTEGER>(std::stol(value));
        *static_cast<SQLINTEGER*>(buffer) = result;
        if (indicator) *indicator = sizeof(SQLINTEGER);
        return SQL_SUCCESS;
    } catch (const std::exception&) {
        return SQL_ERROR;
    }
}

SQLRETURN RedshiftDataConverter::convert_to_bigint(const std::string& value, void* buffer, SQLLEN* indicator) {
    try {
        SQLBIGINT result = static_cast<SQLBIGINT>(std::stoll(value));
        *static_cast<SQLBIGINT*>(buffer) = result;
        if (indicator) *indicator = sizeof(SQLBIGINT);
        return SQL_SUCCESS;
    } catch (const std::exception&) {
        return SQL_ERROR;
    }
}

SQLRETURN RedshiftDataConverter::convert_to_double(const std::string& value, void* buffer, SQLLEN* indicator) {
    try {
        SQLDOUBLE result = std::stod(value);
        *static_cast<SQLDOUBLE*>(buffer) = result;
        if (indicator) *indicator = sizeof(SQLDOUBLE);
        return SQL_SUCCESS;
    } catch (const std::exception&) {
        return SQL_ERROR;
    }
}

SQLRETURN RedshiftDataConverter::convert_to_boolean(const std::string& value, void* buffer, SQLLEN* indicator) {
    // Redshift boolean format: 't'/'f' or 'true'/'false'
    SQLCHAR result = (value == "t" || value == "true" || value == "1") ? 1 : 0;
    *static_cast<SQLCHAR*>(buffer) = result;
    if (indicator) *indicator = sizeof(SQLCHAR);
    return SQL_SUCCESS;
}

SQLRETURN RedshiftDataConverter::convert_to_date(const std::string& value, void* buffer, SQLLEN* indicator) {
    // Parse Redshift date format: "2025-08-31"
    SQL_DATE_STRUCT* date = static_cast<SQL_DATE_STRUCT*>(buffer);
    
    if (sscanf(value.c_str(), "%hd-%hd-%hd", &date->year, &date->month, &date->day) == 3) {
        if (indicator) *indicator = sizeof(SQL_DATE_STRUCT);
        return SQL_SUCCESS;
    }
    
    return SQL_ERROR;
}

SQLRETURN RedshiftDataConverter::convert_to_timestamp(const std::string& value, void* buffer, SQLLEN* indicator) {
    // Parse Redshift timestamp format: "2025-08-31 23:39:29.020257+00"
    SQL_TIMESTAMP_STRUCT* ts = static_cast<SQL_TIMESTAMP_STRUCT*>(buffer);
    
    // Parse basic timestamp: YYYY-MM-DD HH:MM:SS
    if (sscanf(value.c_str(), "%hd-%hd-%hd %hd:%hd:%hd", 
               &ts->year, &ts->month, &ts->day, 
               &ts->hour, &ts->minute, &ts->second) == 6) {
        
        // Parse fractional seconds if present
        const char* dot = strchr(value.c_str(), '.');
        if (dot) {
            // Convert microseconds to nanoseconds
            int microseconds = 0;
            sscanf(dot + 1, "%d", &microseconds);
            ts->fraction = microseconds * 1000; // Convert to nanoseconds
        } else {
            ts->fraction = 0;
        }
        
        if (indicator) *indicator = sizeof(SQL_TIMESTAMP_STRUCT);
        return SQL_SUCCESS;
    }
    
    return SQL_ERROR;
}

} // namespace rs::odbc::redshift