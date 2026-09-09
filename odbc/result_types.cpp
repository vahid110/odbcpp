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
