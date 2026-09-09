#include <sql.h>
#include <sqlext.h>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {

void print_diagnostic(SQLSMALLINT handle_type, SQLHANDLE handle) {
  SQLCHAR state[6]{};
  SQLCHAR message[512]{};
  SQLINTEGER native_error = 0;
  SQLSMALLINT message_length = 0;
  if (SQLGetDiagRec(handle_type, handle, 1, state, &native_error, message,
                    sizeof(message), &message_length) == SQL_SUCCESS) {
    std::fprintf(stderr, "%s: %s (%d)\n", state, message, native_error);
  }
}

bool succeeded(SQLRETURN result) {
  return result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO;
}

std::vector<SQLWCHAR> wide_ascii(std::string_view input) {
  std::vector<SQLWCHAR> output;
  output.reserve(input.size() + 1);
  for (const auto character : input) {
    output.push_back(static_cast<SQLWCHAR>(
        static_cast<unsigned char>(character)));
  }
  output.push_back(0);
  return output;
}

} // namespace

int main() {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  SQLHSTMT statement = SQL_NULL_HSTMT;

  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment))) {
    return 1;
  }
  if (!succeeded(SQLSetEnvAttr(
          environment, SQL_ATTR_ODBC_VERSION,
          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0))) {
    print_diagnostic(SQL_HANDLE_ENV, environment);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLINTEGER odbc_version = 0;
  if (!succeeded(SQLGetEnvAttr(
          environment, SQL_ATTR_ODBC_VERSION, &odbc_version,
          sizeof(odbc_version), nullptr)) ||
      odbc_version != SQL_OV_ODBC3) {
    print_diagnostic(SQL_HANDLE_ENV, environment);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_DBC, environment, &connection))) {
    print_diagnostic(SQL_HANDLE_ENV, environment);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }

  SQLCHAR connection_string[] =
      "DRIVER={ODBCPP PostgreSQL};SERVER=127.0.0.1;PORT=5432;"
      "DATABASE=postgres;UID=postgres;PWD=postgres;SSL=0";
  SQLCHAR completed_connection_string[sizeof(connection_string)]{};
  SQLSMALLINT completed_length = 0;
  const auto connect_result = SQLDriverConnect(
      connection, nullptr, connection_string, SQL_NTS,
      completed_connection_string, sizeof(completed_connection_string),
      &completed_length, SQL_DRIVER_NOPROMPT);
  if (!succeeded(connect_result)) {
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (completed_length != sizeof(connection_string) - 1) {
    std::fprintf(stderr, "Unexpected completed connection string length\n");
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR input_sql[] = "SELECT 42";
  SQLCHAR native_sql[sizeof(input_sql)]{};
  SQLINTEGER native_sql_length = 0;
  if (!succeeded(SQLNativeSql(
          connection, input_sql, SQL_NTS, native_sql, sizeof(native_sql),
          &native_sql_length)) ||
      native_sql_length != sizeof(input_sql) - 1 ||
      std::strcmp(reinterpret_cast<const char*>(native_sql), "SELECT 42") != 0) {
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLUSMALLINT function_supported = SQL_FALSE;
  if (!succeeded(SQLGetFunctions(
          connection, SQL_API_SQLEXECDIRECT, &function_supported)) ||
      function_supported != SQL_TRUE) {
    std::fprintf(stderr, "Driver did not report SQLExecDirect support\n");
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_STMT, connection, &statement))) {
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }

  SQLINTEGER value = 0;
  SQLCHAR query[] = "SELECT 42";
  if (!succeeded(SQLExecDirect(
          statement, query, SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLGetData(
          statement, 1, SQL_C_SLONG, &value, 0, nullptr)) ||
      value != 42) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLCloseCursor(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  const auto wide_native_input = wide_ascii("SELECT 84");
  SQLWCHAR wide_native_output[16]{};
  SQLINTEGER wide_native_length = 0;
  if (!succeeded(SQLNativeSqlW(
          connection, const_cast<SQLWCHAR*>(wide_native_input.data()), SQL_NTS,
          wide_native_output, 16, &wide_native_length)) ||
      wide_native_length != 9 ||
      wide_native_output[7] != static_cast<SQLWCHAR>('8')) {
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  const auto wide_query = wide_ascii("SELECT 'wide'::text");
  SQLWCHAR wide_value[8]{};
  SQLLEN wide_value_length = 0;
  if (!succeeded(SQLExecDirectW(
          statement, const_cast<SQLWCHAR*>(wide_query.data()), SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLGetData(
          statement, 1, SQL_C_WCHAR, wide_value, sizeof(wide_value),
          &wide_value_length)) ||
      wide_value_length != static_cast<SQLLEN>(4 * sizeof(SQLWCHAR)) ||
      wide_value[0] != static_cast<SQLWCHAR>('w') ||
      wide_value[3] != static_cast<SQLWCHAR>('e') ||
      !succeeded(SQLCloseCursor(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLFreeStmt(statement, SQL_UNBIND)) ||
      !succeeded(SQLFreeStmt(statement, SQL_RESET_PARAMS))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLGetTypeInfo(statement, SQL_INTEGER)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR prepared_query[] = "SELECT ?, '?'::text";
  SQLSMALLINT parameter_count = 0;
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLPrepare(statement, prepared_query, SQL_NTS)) ||
      !succeeded(SQLNumParams(statement, &parameter_count)) ||
      parameter_count != 1) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR schema_pattern[] = "information_schema";
  SQLCHAR table_pattern[] = "tables";
  SQLCHAR table_type[] = "VIEW";
  if (!succeeded(SQLTables(
          statement, nullptr, 0, schema_pattern, SQL_NTS,
          table_pattern, SQL_NTS, table_type, SQL_NTS)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR column_pattern[] = "table_name";
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLColumns(
          statement, nullptr, 0, schema_pattern, SQL_NTS,
          table_pattern, SQL_NTS, column_pattern, SQL_NTS)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR create_primary_key_table[] =
      "CREATE TEMP TABLE odbcpp_dm_primary_key(id integer PRIMARY KEY)";
  SQLCHAR primary_key_table[] = "odbcpp_dm_primary_key";
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLExecDirect(
          statement, create_primary_key_table, SQL_NTS)) ||
      !succeeded(SQLPrimaryKeys(
          statement, nullptr, 0, nullptr, 0,
          primary_key_table, SQL_NTS)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR create_foreign_key_table[] =
      "CREATE TEMP TABLE odbcpp_dm_foreign_key("
      "id integer REFERENCES odbcpp_dm_primary_key(id))";
  SQLCHAR foreign_key_table[] = "odbcpp_dm_foreign_key";
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLExecDirect(
          statement, create_foreign_key_table, SQL_NTS)) ||
      !succeeded(SQLForeignKeys(
          statement, nullptr, 0, nullptr, 0, nullptr, 0,
          nullptr, 0, nullptr, 0, foreign_key_table, SQL_NTS)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLStatistics(
          statement, nullptr, 0, nullptr, 0,
          primary_key_table, SQL_NTS, SQL_INDEX_UNIQUE, SQL_QUICK)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR create_catalog_function[] =
      "CREATE FUNCTION pg_temp.odbcpp_dm_catalog_function(value integer) "
      "RETURNS integer LANGUAGE SQL AS 'SELECT value'";
  SQLCHAR catalog_function[] = "odbcpp_dm_catalog_function";
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLExecDirect(
          statement, create_catalog_function, SQL_NTS)) ||
      !succeeded(SQLProcedures(
          statement, nullptr, 0, nullptr, 0,
          catalog_function, SQL_NTS)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR catalog_function_parameter[] = "value";
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLProcedureColumns(
          statement, nullptr, 0, nullptr, 0,
          catalog_function, SQL_NTS,
          catalog_function_parameter, SQL_NTS)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLSpecialColumns(
          statement, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
          primary_key_table, SQL_NTS,
          SQL_SCOPE_SESSION, SQL_NO_NULLS)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  auto wide_schema_pattern = wide_ascii("information_schema");
  auto wide_table_pattern = wide_ascii("tables");
  auto wide_table_type = wide_ascii("VIEW");
  auto wide_column_pattern = wide_ascii("table_name");
  auto wide_primary_key_table = wide_ascii("odbcpp_dm_primary_key");
  auto wide_foreign_key_table = wide_ascii("odbcpp_dm_foreign_key");
  auto wide_catalog_function = wide_ascii("odbcpp_dm_catalog_function");
  auto wide_catalog_parameter = wide_ascii("value");
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLTablesW(
          statement, nullptr, 0, wide_schema_pattern.data(), SQL_NTS,
          wide_table_pattern.data(), SQL_NTS, wide_table_type.data(),
          SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLColumnsW(
          statement, nullptr, 0, wide_schema_pattern.data(), SQL_NTS,
          wide_table_pattern.data(), SQL_NTS, wide_column_pattern.data(),
          SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLPrimaryKeysW(
          statement, nullptr, 0, nullptr, 0,
          wide_primary_key_table.data(), SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLForeignKeysW(
          statement, nullptr, 0, nullptr, 0, nullptr, 0,
          nullptr, 0, nullptr, 0, wide_foreign_key_table.data(), SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLStatisticsW(
          statement, nullptr, 0, nullptr, 0,
          wide_primary_key_table.data(), SQL_NTS, SQL_INDEX_UNIQUE,
          SQL_QUICK)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLProceduresW(
          statement, nullptr, 0, nullptr, 0,
          wide_catalog_function.data(), SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLProcedureColumnsW(
          statement, nullptr, 0, nullptr, 0,
          wide_catalog_function.data(), SQL_NTS,
          wide_catalog_parameter.data(), SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLSpecialColumnsW(
          statement, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
          wide_primary_key_table.data(), SQL_NTS,
          SQL_SCOPE_SESSION, SQL_NO_NULLS)) ||
      !succeeded(SQLFetch(statement))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (SQLMoreResults(statement) != SQL_NO_DATA) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR fetch_scroll_query[] = "SELECT 1";
  if (!succeeded(SQLExecDirect(
          statement, fetch_scroll_query, SQL_NTS)) ||
      !succeeded(SQLFetchScroll(statement, SQL_FETCH_NEXT, 0))) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR multiple_results_query[] = "SELECT 1; SELECT 'done'::text";
  if (!succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLExecDirect(
          statement, multiple_results_query, SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLMoreResults(statement)) ||
      !succeeded(SQLFetch(statement)) ||
      SQLMoreResults(statement) != SQL_NO_DATA) {
    print_diagnostic(SQL_HANDLE_STMT, statement);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }

  SQLFreeHandle(SQL_HANDLE_STMT, statement);
  SQLDisconnect(connection);
  SQLFreeHandle(SQL_HANDLE_DBC, connection);
  SQLFreeHandle(SQL_HANDLE_ENV, environment);
  return 0;
}
