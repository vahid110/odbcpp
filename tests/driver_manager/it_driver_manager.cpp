#include <sql.h>
#include <sqlext.h>

#include <cstdio>

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

  SQLFreeHandle(SQL_HANDLE_STMT, statement);
  SQLDisconnect(connection);
  SQLFreeHandle(SQL_HANDLE_DBC, connection);
  SQLFreeHandle(SQL_HANDLE_ENV, environment);
  return 0;
}
