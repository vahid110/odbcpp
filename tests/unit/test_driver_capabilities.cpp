#include "odbc/odbc_types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

struct FunctionExport {
  const char* name;
  SQLUSMALLINT id;
};

constexpr std::array advertised_functions{
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
    FunctionExport{"SQLSetEnvAttr", SQL_API_SQLSETENVATTR},
    FunctionExport{"SQLSetStmtAttr", SQL_API_SQLSETSTMTATTR},
    FunctionExport{"SQLSpecialColumns", SQL_API_SQLSPECIALCOLUMNS},
    FunctionExport{"SQLStatistics", SQL_API_SQLSTATISTICS},
    FunctionExport{"SQLTables", SQL_API_SQLTABLES},
};

constexpr std::array wide_exports{
    "SQLColAttributeW", "SQLColumnsW", "SQLConnectW", "SQLDescribeColW",
    "SQLDriverConnectW", "SQLErrorW", "SQLExecDirectW", "SQLForeignKeysW",
    "SQLGetConnectAttrW", "SQLGetDescFieldW", "SQLGetDiagFieldW",
    "SQLGetDiagRecW", "SQLGetInfoW", "SQLGetStmtAttrW", "SQLGetTypeInfoW",
    "SQLNativeSqlW", "SQLPrepareW", "SQLPrimaryKeysW",
    "SQLProcedureColumnsW", "SQLProceduresW", "SQLSetConnectAttrW",
    "SQLSetDescFieldW", "SQLSetStmtAttrW", "SQLSpecialColumnsW",
    "SQLStatisticsW", "SQLTablesW",
};

class DynamicLibrary {
 public:
#ifdef _WIN32
  using Symbol = FARPROC;
#else
  using Symbol = void*;
#endif

  explicit DynamicLibrary(const char* path) {
#ifdef _WIN32
    handle_ = LoadLibraryA(path);
#else
    handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
  }

  ~DynamicLibrary() {
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(handle_);
#else
    dlclose(handle_);
#endif
  }

  bool valid() const { return handle_ != nullptr; }

  Symbol symbol(const char* name) const {
#ifdef _WIN32
    return GetProcAddress(handle_, name);
#else
    return dlsym(handle_, name);
#endif
  }

 private:
#ifdef _WIN32
  HMODULE handle_{};
#else
  void* handle_{};
#endif
};

bool expected_support(SQLUSMALLINT id) {
  return std::ranges::any_of(
      advertised_functions,
      [id](const FunctionExport& function) { return function.id == id; });
}

TEST(DriverCapabilitiesTest, AdvertisesExactlyItsBaseExports) {
  DynamicLibrary driver(ODBCPP_DRIVER_LIBRARY_PATH);
  ASSERT_TRUE(driver.valid()) << ODBCPP_DRIVER_LIBRARY_PATH;

  for (const auto& function : advertised_functions) {
    EXPECT_NE(nullptr, driver.symbol(function.name)) << function.name;
  }
  for (const auto* name : wide_exports) {
    EXPECT_NE(nullptr, driver.symbol(name)) << name;
  }

  using AllocHandle = SQLRETURN(SQL_API*)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*);
  using FreeHandle = SQLRETURN(SQL_API*)(SQLSMALLINT, SQLHANDLE);
  using SetEnvAttr = SQLRETURN(SQL_API*)(SQLHENV, SQLINTEGER, SQLPOINTER,
                                        SQLINTEGER);
  using GetFunctions = SQLRETURN(SQL_API*)(SQLHDBC, SQLUSMALLINT,
                                          SQLUSMALLINT*);
  const auto alloc_handle = reinterpret_cast<AllocHandle>(
      driver.symbol("SQLAllocHandle"));
  const auto free_handle = reinterpret_cast<FreeHandle>(
      driver.symbol("SQLFreeHandle"));
  const auto set_env_attr = reinterpret_cast<SetEnvAttr>(
      driver.symbol("SQLSetEnvAttr"));
  const auto get_functions = reinterpret_cast<GetFunctions>(
      driver.symbol("SQLGetFunctions"));
  ASSERT_NE(nullptr, alloc_handle);
  ASSERT_NE(nullptr, free_handle);
  ASSERT_NE(nullptr, set_env_attr);
  ASSERT_NE(nullptr, get_functions);

  SQLUSMALLINT supported = SQL_TRUE;
  EXPECT_EQ(SQL_INVALID_HANDLE,
            get_functions(SQL_NULL_HDBC, SQL_API_SQLCONNECT, &supported));

  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            set_env_attr(environment, SQL_ATTR_ODBC_VERSION,
                         reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS,
            alloc_handle(SQL_HANDLE_DBC, environment, &connection));

  SQLUSMALLINT odbc3[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE]{};
  ASSERT_EQ(SQL_SUCCESS,
            get_functions(connection, SQL_API_ODBC3_ALL_FUNCTIONS, odbc3));
  for (SQLUSMALLINT id = 0; id < 4000; ++id) {
    EXPECT_EQ(expected_support(id), SQL_FUNC_EXISTS(odbc3, id) != 0) << id;
  }

  SQLUSMALLINT odbc2[100]{};
  ASSERT_EQ(SQL_SUCCESS,
            get_functions(connection, SQL_API_ALL_FUNCTIONS, odbc2));
  for (SQLUSMALLINT id = 0; id < 100; ++id) {
    EXPECT_EQ(expected_support(id), odbc2[id] == SQL_TRUE) << id;
  }

  for (const auto& function : advertised_functions) {
    SQLUSMALLINT supported = SQL_FALSE;
    ASSERT_EQ(SQL_SUCCESS,
              get_functions(connection, function.id, &supported));
    EXPECT_EQ(SQL_TRUE, supported) << function.name;
  }

  supported = SQL_TRUE;
  EXPECT_EQ(SQL_SUCCESS, get_functions(connection, 0xffff, &supported));
  EXPECT_EQ(SQL_FALSE, supported);
  EXPECT_EQ(SQL_SUCCESS, free_handle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, free_handle(SQL_HANDLE_ENV, environment));
}

}  // namespace
