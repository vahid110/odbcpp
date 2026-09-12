#pragma once

#include "odbc/odbc_types.h"

#include <algorithm>
#include <array>

namespace odbcpp::test {

struct FunctionExport {
  const char* name;
  SQLUSMALLINT id;
};

inline constexpr std::array advertised_functions{
    FunctionExport{"SQLAllocHandle", SQL_API_SQLALLOCHANDLE},
    FunctionExport{"SQLBindCol", SQL_API_SQLBINDCOL},
    FunctionExport{"SQLBindParameter", SQL_API_SQLBINDPARAMETER},
    FunctionExport{"SQLColAttribute", SQL_API_SQLCOLATTRIBUTE},
    FunctionExport{"SQLCloseCursor", SQL_API_SQLCLOSECURSOR},
    FunctionExport{"SQLColumns", SQL_API_SQLCOLUMNS},
    FunctionExport{"SQLConnect", SQL_API_SQLCONNECT},
    FunctionExport{"SQLCopyDesc", SQL_API_SQLCOPYDESC},
    FunctionExport{"SQLDescribeCol", SQL_API_SQLDESCRIBECOL},
    FunctionExport{"SQLDescribeParam", SQL_API_SQLDESCRIBEPARAM},
    FunctionExport{"SQLDisconnect", SQL_API_SQLDISCONNECT},
    FunctionExport{"SQLDriverConnect", SQL_API_SQLDRIVERCONNECT},
    FunctionExport{"SQLEndTran", SQL_API_SQLENDTRAN},
    FunctionExport{"SQLError", SQL_API_SQLERROR},
    FunctionExport{"SQLExecDirect", SQL_API_SQLEXECDIRECT},
    FunctionExport{"SQLExecute", SQL_API_SQLEXECUTE},
    FunctionExport{"SQLFetch", SQL_API_SQLFETCH},
    FunctionExport{"SQLFetchScroll", SQL_API_SQLFETCHSCROLL},
    FunctionExport{"SQLForeignKeys", SQL_API_SQLFOREIGNKEYS},
    FunctionExport{"SQLFreeHandle", SQL_API_SQLFREEHANDLE},
    FunctionExport{"SQLFreeStmt", SQL_API_SQLFREESTMT},
    FunctionExport{"SQLGetConnectAttr", SQL_API_SQLGETCONNECTATTR},
    FunctionExport{"SQLGetData", SQL_API_SQLGETDATA},
    FunctionExport{"SQLGetDescField", SQL_API_SQLGETDESCFIELD},
    FunctionExport{"SQLGetDescRec", SQL_API_SQLGETDESCREC},
    FunctionExport{"SQLGetDiagField", SQL_API_SQLGETDIAGFIELD},
    FunctionExport{"SQLGetDiagRec", SQL_API_SQLGETDIAGREC},
    FunctionExport{"SQLGetEnvAttr", SQL_API_SQLGETENVATTR},
    FunctionExport{"SQLGetFunctions", SQL_API_SQLGETFUNCTIONS},
    FunctionExport{"SQLGetInfo", SQL_API_SQLGETINFO},
    FunctionExport{"SQLGetStmtAttr", SQL_API_SQLGETSTMTATTR},
    FunctionExport{"SQLGetTypeInfo", SQL_API_SQLGETTYPEINFO},
    FunctionExport{"SQLMoreResults", SQL_API_SQLMORERESULTS},
    FunctionExport{"SQLNativeSql", SQL_API_SQLNATIVESQL},
    FunctionExport{"SQLNumParams", SQL_API_SQLNUMPARAMS},
    FunctionExport{"SQLNumResultCols", SQL_API_SQLNUMRESULTCOLS},
    FunctionExport{"SQLPrepare", SQL_API_SQLPREPARE},
    FunctionExport{"SQLPrimaryKeys", SQL_API_SQLPRIMARYKEYS},
    FunctionExport{"SQLProcedureColumns", SQL_API_SQLPROCEDURECOLUMNS},
    FunctionExport{"SQLProcedures", SQL_API_SQLPROCEDURES},
    FunctionExport{"SQLRowCount", SQL_API_SQLROWCOUNT},
    FunctionExport{"SQLSetConnectAttr", SQL_API_SQLSETCONNECTATTR},
    FunctionExport{"SQLSetDescField", SQL_API_SQLSETDESCFIELD},
    FunctionExport{"SQLSetDescRec", SQL_API_SQLSETDESCREC},
    FunctionExport{"SQLSetEnvAttr", SQL_API_SQLSETENVATTR},
    FunctionExport{"SQLSetStmtAttr", SQL_API_SQLSETSTMTATTR},
    FunctionExport{"SQLSpecialColumns", SQL_API_SQLSPECIALCOLUMNS},
    FunctionExport{"SQLStatistics", SQL_API_SQLSTATISTICS},
    FunctionExport{"SQLTables", SQL_API_SQLTABLES},
};

inline constexpr std::array wide_exports{
    "SQLColAttributeW", "SQLColumnsW", "SQLConnectW", "SQLDescribeColW",
    "SQLDriverConnectW", "SQLErrorW", "SQLExecDirectW", "SQLForeignKeysW",
    "SQLGetConnectAttrW", "SQLGetDescFieldW", "SQLGetDescRecW",
    "SQLGetDiagFieldW", "SQLGetDiagRecW", "SQLGetInfoW",
    "SQLGetStmtAttrW", "SQLGetTypeInfoW", "SQLNativeSqlW", "SQLPrepareW",
    "SQLPrimaryKeysW", "SQLProcedureColumnsW", "SQLProceduresW",
    "SQLSetConnectAttrW", "SQLSetDescFieldW", "SQLSetStmtAttrW",
    "SQLSpecialColumnsW", "SQLStatisticsW", "SQLTablesW",
};

inline bool expected_support(SQLUSMALLINT id) {
  return std::ranges::any_of(
      advertised_functions,
      [id](const FunctionExport& function) { return function.id == id; });
}

}  // namespace odbcpp::test
