#include <sql.h>
#include <sqlext.h>

#include <cstdio>
#include <cstring>

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

  SQLFreeHandle(SQL_HANDLE_STMT, statement);
  SQLDisconnect(connection);
  SQLFreeHandle(SQL_HANDLE_DBC, connection);
  SQLFreeHandle(SQL_HANDLE_ENV, environment);
  return 0;
}
