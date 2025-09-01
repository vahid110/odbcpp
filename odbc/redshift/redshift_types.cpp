#include "redshift_types.h"
#include <algorithm>
#include <cctype>

namespace rs::odbc::redshift {

SQLSMALLINT RedshiftTypes::get_sql_type(const std::string& redshift_type) {
    // Normalize type name (lowercase, remove size specifiers)
    std::string normalized = redshift_type;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), ::tolower);
    
    // Remove size specifiers: varchar(50) → varchar
    size_t paren_pos = normalized.find('(');
    if (paren_pos != std::string::npos) {
        normalized = normalized.substr(0, paren_pos);
    }
    
    // Redshift type mappings
    if (normalized == "smallint") return SQL_SMALLINT;
    if (normalized == "integer" || normalized == "int") return SQL_INTEGER;
    if (normalized == "bigint") return SQL_BIGINT;
    if (normalized == "decimal" || normalized == "numeric") return SQL_DECIMAL;
    if (normalized == "real") return SQL_REAL;
    if (normalized == "double precision") return SQL_DOUBLE;
    
    if (normalized == "char" || normalized == "character") return SQL_CHAR;
    if (normalized == "varchar" || normalized == "character varying") return SQL_VARCHAR;
    if (normalized == "text") return SQL_LONGVARCHAR;
    
    if (normalized == "date") return SQL_TYPE_DATE;
    if (normalized == "timestamp" || normalized == "timestamptz") return SQL_TYPE_TIMESTAMP;
    if (normalized == "time" || normalized == "timetz") return SQL_TYPE_TIME;
    
    if (normalized == "boolean" || normalized == "bool") return SQL_BIT;
    
    // Modern Redshift types
    if (normalized == "super") return SQL_LONGVARCHAR;        // JSON → String
    if (normalized == "varbyte") return SQL_VARBINARY;        // Binary
    if (normalized == "geometry" || normalized == "geography") return SQL_LONGVARCHAR; // Spatial → WKT
    
    // Arrays → JSON strings
    if (normalized.find("[]") != std::string::npos) return SQL_LONGVARCHAR;
    
    return SQL_UNKNOWN_TYPE;
}

SQLSMALLINT RedshiftTypes::get_default_c_type(SQLSMALLINT sql_type) {
    switch (sql_type) {
        case SQL_SMALLINT:
        case SQL_INTEGER:
            return SQL_C_SLONG;
        case SQL_BIGINT:
            return SQL_C_SBIGINT;
        case SQL_REAL:
        case SQL_DOUBLE:
            return SQL_C_DOUBLE;
        case SQL_DECIMAL:
            return SQL_C_CHAR; // Return as string for precision
        case SQL_CHAR:
        case SQL_VARCHAR:
        case SQL_LONGVARCHAR:
            return SQL_C_CHAR;
        case SQL_TYPE_DATE:
            return SQL_C_DATE;
        case SQL_TYPE_TIME:
            return SQL_C_TIME;
        case SQL_TYPE_TIMESTAMP:
            return SQL_C_TIMESTAMP;
        case SQL_BIT:
            return SQL_C_BIT;
        case SQL_VARBINARY:
            return SQL_C_BINARY;
        default:
            return SQL_C_CHAR; // Fallback to string
    }
}

bool RedshiftTypes::is_conversion_supported(SQLSMALLINT sql_type, SQLSMALLINT c_type) {
    // SQL_C_CHAR is supported for ALL types (universal fallback)
    if (c_type == SQL_C_CHAR) return true;
    
    // Type-specific conversions
    switch (sql_type) {
        case SQL_SMALLINT:
        case SQL_INTEGER:
        case SQL_BIGINT:
            return (c_type == SQL_C_SLONG || c_type == SQL_C_SBIGINT || 
                   c_type == SQL_C_DOUBLE || c_type == SQL_C_CHAR);
                   
        case SQL_REAL:
        case SQL_DOUBLE:
            return (c_type == SQL_C_DOUBLE || c_type == SQL_C_CHAR);
            
        case SQL_BIT:
            return (c_type == SQL_C_BIT || c_type == SQL_C_CHAR);
            
        case SQL_TYPE_DATE:
            return (c_type == SQL_C_DATE || c_type == SQL_C_CHAR);
            
        case SQL_TYPE_TIME:
            return (c_type == SQL_C_TIME || c_type == SQL_C_CHAR);
            
        case SQL_TYPE_TIMESTAMP:
            return (c_type == SQL_C_TIMESTAMP || c_type == SQL_C_CHAR);
            
        case SQL_VARBINARY:
            return (c_type == SQL_C_BINARY || c_type == SQL_C_CHAR);
            
        default:
            return (c_type == SQL_C_CHAR); // Only string conversion for unknown types
    }
}

} // namespace rs::odbc::redshift