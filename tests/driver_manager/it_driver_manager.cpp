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

  SQLCHAR dsn[] = "RedshiftProd";
  const auto connect_result = SQLConnect(
      connection, dsn, SQL_NTS, nullptr, 0, nullptr, 0);
  if (!succeeded(connect_result)) {
    print_diagnostic(SQL_HANDLE_DBC, connection);
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

  SQLFreeHandle(SQL_HANDLE_STMT, statement);
  SQLDisconnect(connection);
  SQLFreeHandle(SQL_HANDLE_DBC, connection);
  SQLFreeHandle(SQL_HANDLE_ENV, environment);
  return 0;
}
