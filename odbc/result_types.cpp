#include "result_types.h"

namespace rs::odbc {

SQLSMALLINT ResultTypes::default_c_type(SQLSMALLINT sql_type) {
  switch (sql_type) {
    case SQL_SMALLINT: return SQL_C_SSHORT;
    case SQL_INTEGER: return SQL_C_SLONG;
    case SQL_BIGINT: return SQL_C_SBIGINT;
    case SQL_REAL: return SQL_C_FLOAT;
    case SQL_FLOAT:
    case SQL_DOUBLE: return SQL_C_DOUBLE;
    case SQL_BIT: return SQL_C_BIT;
    case SQL_TYPE_DATE: return SQL_C_DATE;
    case SQL_TYPE_TIME: return SQL_C_TIME;
    case SQL_TYPE_TIMESTAMP: return SQL_C_TIMESTAMP;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY: return SQL_C_BINARY;
    default: return SQL_C_CHAR;
  }
}

bool ResultTypes::is_supported_c_type(SQLSMALLINT c_type) {
  return c_type == SQL_C_DEFAULT || c_type == SQL_C_CHAR ||
      c_type == SQL_C_WCHAR || c_type == SQL_C_SSHORT ||
      c_type == SQL_C_SLONG || c_type == SQL_C_SBIGINT ||
      c_type == SQL_C_FLOAT || c_type == SQL_C_DOUBLE ||
      c_type == SQL_C_BIT || c_type == SQL_C_DATE ||
      c_type == SQL_C_TIME || c_type == SQL_C_TIMESTAMP ||
      c_type == SQL_C_BINARY;
}

bool ResultTypes::is_valid_c_type(SQLSMALLINT c_type) {
  if (is_supported_c_type(c_type) || c_type == SQL_C_NUMERIC ||
      c_type == SQL_C_STINYINT || c_type == SQL_C_UTINYINT ||
      c_type == SQL_C_USHORT || c_type == SQL_C_ULONG ||
      c_type == SQL_C_UBIGINT || c_type == SQL_C_TYPE_DATE ||
      c_type == SQL_C_TYPE_TIME || c_type == SQL_C_TYPE_TIMESTAMP ||
      c_type == SQL_C_INTERVAL_YEAR || c_type == SQL_C_INTERVAL_MONTH ||
      c_type == SQL_C_INTERVAL_DAY || c_type == SQL_C_INTERVAL_HOUR ||
      c_type == SQL_C_INTERVAL_MINUTE || c_type == SQL_C_INTERVAL_SECOND ||
      c_type == SQL_C_INTERVAL_YEAR_TO_MONTH ||
      c_type == SQL_C_INTERVAL_DAY_TO_HOUR ||
      c_type == SQL_C_INTERVAL_DAY_TO_MINUTE ||
      c_type == SQL_C_INTERVAL_DAY_TO_SECOND ||
      c_type == SQL_C_INTERVAL_HOUR_TO_MINUTE ||
      c_type == SQL_C_INTERVAL_HOUR_TO_SECOND ||
      c_type == SQL_C_INTERVAL_MINUTE_TO_SECOND) {
    return true;
  }
#ifdef SQL_C_GUID
  if (c_type == SQL_C_GUID) return true;
#endif
  return false;
}

bool ResultTypes::is_supported_parameter_c_type(SQLSMALLINT c_type) {
  return c_type == SQL_C_DEFAULT || c_type == SQL_C_CHAR ||
      c_type == SQL_C_WCHAR || c_type == SQL_C_SSHORT ||
      c_type == SQL_C_SLONG || c_type == SQL_C_SBIGINT ||
      c_type == SQL_C_FLOAT || c_type == SQL_C_DOUBLE ||
      c_type == SQL_C_BIT || c_type == SQL_C_BINARY;
}

bool ResultTypes::is_supported_parameter_sql_type(SQLSMALLINT sql_type) {
  return sql_type == SQL_CHAR || sql_type == SQL_VARCHAR ||
      sql_type == SQL_LONGVARCHAR || sql_type == SQL_WCHAR ||
      sql_type == SQL_WVARCHAR || sql_type == SQL_WLONGVARCHAR ||
      sql_type == SQL_TINYINT || sql_type == SQL_SMALLINT ||
      sql_type == SQL_INTEGER || sql_type == SQL_BIGINT ||
      sql_type == SQL_REAL || sql_type == SQL_FLOAT ||
      sql_type == SQL_DOUBLE || sql_type == SQL_DECIMAL ||
      sql_type == SQL_NUMERIC || sql_type == SQL_BIT ||
      sql_type == SQL_BINARY || sql_type == SQL_VARBINARY ||
      sql_type == SQL_LONGVARBINARY;
}

bool ResultTypes::is_valid_sql_type(SQLSMALLINT sql_type) {
  if (is_supported_parameter_sql_type(sql_type) ||
      sql_type == SQL_DATE || sql_type == SQL_TIME ||
      sql_type == SQL_TIMESTAMP || sql_type == SQL_TYPE_DATE ||
      sql_type == SQL_TYPE_TIME || sql_type == SQL_TYPE_TIMESTAMP ||
      sql_type == SQL_INTERVAL_YEAR || sql_type == SQL_INTERVAL_MONTH ||
      sql_type == SQL_INTERVAL_DAY || sql_type == SQL_INTERVAL_HOUR ||
      sql_type == SQL_INTERVAL_MINUTE || sql_type == SQL_INTERVAL_SECOND ||
      sql_type == SQL_INTERVAL_YEAR_TO_MONTH ||
      sql_type == SQL_INTERVAL_DAY_TO_HOUR ||
      sql_type == SQL_INTERVAL_DAY_TO_MINUTE ||
      sql_type == SQL_INTERVAL_DAY_TO_SECOND ||
      sql_type == SQL_INTERVAL_HOUR_TO_MINUTE ||
      sql_type == SQL_INTERVAL_HOUR_TO_SECOND ||
      sql_type == SQL_INTERVAL_MINUTE_TO_SECOND) {
    return true;
  }
#ifdef SQL_GUID
  if (sql_type == SQL_GUID) return true;
#endif
  return false;
}

bool ResultTypes::is_conversion_supported(SQLSMALLINT sql_type,
                                          SQLSMALLINT c_type) {
  switch (c_type) {
    case SQL_C_CHAR:
    case SQL_C_WCHAR:
    case SQL_C_SSHORT:
    case SQL_C_SLONG:
    case SQL_C_SBIGINT:
    case SQL_C_FLOAT:
    case SQL_C_DOUBLE:
    case SQL_C_BIT:
    case SQL_C_DATE:
    case SQL_C_TIME:
    case SQL_C_TIMESTAMP:
      return true;
    case SQL_C_BINARY:
      return sql_type == SQL_BINARY || sql_type == SQL_VARBINARY ||
             sql_type == SQL_LONGVARBINARY;
    default:
      return false;
  }
}

} // namespace rs::odbc
