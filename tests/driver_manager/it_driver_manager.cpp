#include <sql.h>
#include <sqlext.h>
#ifdef ODBCPP_TEST_IODBC
#include <iodbcext.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#ifdef ODBCPP_EXPECT_DM_SQLWCHAR_SIZE
static_assert(sizeof(SQLWCHAR) == ODBCPP_EXPECT_DM_SQLWCHAR_SIZE,
              "Driver-manager test is using an unexpected SQLWCHAR ABI");
#endif

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

#ifdef ODBCPP_TEST_IODBC
constexpr bool require_nonzero_driver_handle_mapping = false;
#else
constexpr bool require_nonzero_driver_handle_mapping = true;
#endif

bool diagnostic_is(SQLSMALLINT handle_type, SQLHANDLE handle,
                   const char* expected_state) {
  SQLCHAR state[6]{};
  const auto result = SQLGetDiagRec(handle_type, handle, 1, state, nullptr,
                                    nullptr, 0, nullptr);
  const bool matches = succeeded(result) &&
      std::strcmp(reinterpret_cast<const char*>(state), expected_state) == 0;
  if (!matches) {
    std::fprintf(stderr, "Expected diagnostic %s, received %s (result %d)\n",
                 expected_state, reinterpret_cast<const char*>(state),
                 static_cast<int>(result));
  }
  return matches;
}

bool wide_diagnostic_is(SQLSMALLINT handle_type, SQLHANDLE handle,
                        const char* expected_state) {
  SQLWCHAR state[6]{};
  const auto result = SQLGetDiagRecW(handle_type, handle, 1, state, nullptr,
                                     nullptr, 0, nullptr);
  bool matches = succeeded(result);
  for (std::size_t index = 0; matches && index < 5; ++index) {
    matches = state[index] == static_cast<SQLWCHAR>(
        static_cast<unsigned char>(expected_state[index]));
  }
  matches = matches && state[5] == 0;
  if (!matches) {
    std::fprintf(stderr, "Expected wide diagnostic %s (result %d)\n",
                 expected_state, static_cast<int>(result));
  }
  return matches;
}

bool legacy_diagnostic_is(SQLHENV environment, SQLHDBC connection,
                          const char* expected_state) {
  SQLCHAR state[6]{};
  const auto result = SQLError(environment, connection, nullptr, state,
                               nullptr, nullptr, 0, nullptr);
  return succeeded(result) &&
      std::strcmp(reinterpret_cast<const char*>(state), expected_state) == 0;
}

bool wide_legacy_diagnostic_is(SQLHENV environment, SQLHDBC connection,
                               const char* expected_state) {
  SQLWCHAR state[6]{};
  const auto result = SQLErrorW(environment, connection, nullptr, state,
                                nullptr, nullptr, 0, nullptr);
  if (!succeeded(result)) return false;
  for (std::size_t index = 0; index < 5; ++index) {
    if (state[index] != static_cast<SQLWCHAR>(
                            static_cast<unsigned char>(expected_state[index]))) {
      return false;
    }
  }
  return state[5] == 0;
}

bool result_is(SQLRETURN actual, SQLRETURN expected, const char* operation) {
  if (actual == expected) return true;
  std::fprintf(stderr, "%s returned %d; expected %d\n", operation,
               static_cast<int>(actual), static_cast<int>(expected));
  return false;
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

std::vector<SQLWCHAR> wide_text(std::u32string_view input) {
  std::vector<SQLWCHAR> output;
  output.reserve(input.size() + 1);
  for (const auto character : input) {
    const auto code_point = static_cast<std::uint32_t>(character);
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (code_point > 0xffff) {
        const auto value = code_point - 0x10000;
        output.push_back(static_cast<SQLWCHAR>(0xd800 + (value >> 10)));
        output.push_back(static_cast<SQLWCHAR>(0xdc00 + (value & 0x3ff)));
        continue;
      }
    }
    output.push_back(static_cast<SQLWCHAR>(code_point));
  }
  output.push_back(0);
  return output;
}

} // namespace

int main() {
  SQLHENV unconfigured_environment = SQL_NULL_HENV;
  SQLHDBC premature_connection = SQL_NULL_HDBC;
  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_ENV, SQL_NULL_HANDLE, &unconfigured_environment)) ||
      !result_is(SQLAllocHandle(
                     SQL_HANDLE_DBC, unconfigured_environment,
                     &premature_connection),
                 SQL_ERROR, "SQLAllocHandle before setting ODBC version") ||
      premature_connection != SQL_NULL_HDBC ||
      !diagnostic_is(
          SQL_HANDLE_ENV, unconfigured_environment, "HY010") ||
      !succeeded(SQLFreeHandle(
          SQL_HANDLE_ENV, unconfigured_environment))) {
    print_diagnostic(SQL_HANDLE_ENV, unconfigured_environment);
    SQLFreeHandle(SQL_HANDLE_DBC, premature_connection);
    SQLFreeHandle(SQL_HANDLE_ENV, unconfigured_environment);
    return 1;
  }

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
#ifdef ODBCPP_TEST_IODBC
  const auto application_unicode_type = sizeof(SQLWCHAR) == 2
      ? SQL_DM_CP_UTF16 : SQL_DM_CP_UCS4;
  if (!succeeded(SQLSetEnvAttr(
          environment, SQL_ATTR_APP_UNICODE_TYPE,
          reinterpret_cast<SQLPOINTER>(
              static_cast<std::uintptr_t>(application_unicode_type)),
          SQL_IS_UINTEGER))) {
    print_diagnostic(SQL_HANDLE_ENV, environment);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
#endif
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
  SQLULEN driver_connection = 0;
  SQLULEN driver_environment = 0;
  if (!succeeded(SQLGetInfo(
          connection, SQL_DRIVER_HDBC, &driver_connection,
          sizeof(driver_connection), nullptr)) ||
      !succeeded(SQLGetInfo(
          connection, SQL_DRIVER_HENV, &driver_environment,
          sizeof(driver_environment), nullptr)) ||
      (require_nonzero_driver_handle_mapping &&
       (driver_connection == 0 || driver_environment == 0))) {
    std::fprintf(stderr, "Driver Manager rejected ENV/DBC handle mapping\n");
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLWCHAR wide_driver_name[32]{};
  SQLSMALLINT wide_driver_name_bytes = 0;
  if (!succeeded(SQLGetInfoW(
          connection, SQL_DRIVER_NAME, wide_driver_name,
          sizeof(wide_driver_name), &wide_driver_name_bytes)) ||
      wide_driver_name_bytes !=
          static_cast<SQLSMALLINT>(13 * sizeof(SQLWCHAR)) ||
      wide_driver_name[0] != static_cast<SQLWCHAR>('O')) {
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLCHAR input_sql[] = "SELECT {fn UCASE('dm')}";
  SQLCHAR native_sql[sizeof(input_sql)]{};
  SQLINTEGER native_sql_length = 0;
  if (!succeeded(SQLNativeSql(
          connection, input_sql, SQL_NTS, native_sql, sizeof(native_sql),
          &native_sql_length)) ||
      native_sql_length != 18 ||
      std::strcmp(reinterpret_cast<const char*>(native_sql),
                  "SELECT UPPER('dm')") != 0) {
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
  SQLULEN driver_statement = reinterpret_cast<SQLULEN>(statement);
  if (!succeeded(SQLGetInfo(
          connection, SQL_DRIVER_HSTMT, &driver_statement,
          sizeof(driver_statement), nullptr)) ||
      (require_nonzero_driver_handle_mapping && driver_statement == 0)) {
    std::fprintf(stderr, "Driver Manager rejected statement handle mapping\n");
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLHDESC descriptor = SQL_NULL_HDESC;
  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_DESC, connection, &descriptor))) {
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
#ifndef ODBCPP_TEST_IODBC
  SQLULEN driver_descriptor = reinterpret_cast<SQLULEN>(descriptor);
  if (!succeeded(SQLGetInfo(
          connection, SQL_DRIVER_HDESC, &driver_descriptor,
          sizeof(driver_descriptor), nullptr)) ||
      (require_nonzero_driver_handle_mapping && driver_descriptor == 0)) {
    std::fprintf(stderr, "Driver Manager rejected descriptor handle mapping\n");
    print_diagnostic(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_DESC, descriptor);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
#endif
  if (!succeeded(SQLSetDescRec(
          descriptor, 1, SQL_C_CHAR, 0, 32, 0, 0,
          nullptr, nullptr, nullptr))) {
    print_diagnostic(SQL_HANDLE_DESC, descriptor);
    SQLFreeHandle(SQL_HANDLE_DESC, descriptor);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLSMALLINT descriptor_count = 0;
  SQLSMALLINT descriptor_type = 0;
  SQLLEN descriptor_length = 0;
  if (!succeeded(SQLGetDescField(
          descriptor, 0, SQL_DESC_COUNT, &descriptor_count, 0, nullptr)) ||
      descriptor_count != 1 ||
      !succeeded(SQLGetDescField(
          descriptor, 1, SQL_DESC_CONCISE_TYPE, &descriptor_type, 0,
          nullptr)) ||
      descriptor_type != SQL_C_CHAR ||
      !succeeded(SQLGetDescRec(
          descriptor, 1, nullptr, 0, nullptr, &descriptor_type, nullptr,
          &descriptor_length, nullptr, nullptr, nullptr)) ||
      descriptor_type != SQL_C_CHAR || descriptor_length != 32) {
    print_diagnostic(SQL_HANDLE_DESC, descriptor);
    std::fprintf(stderr,
                 "Descriptor record mismatch: count=%d type=%d length=%lld\n",
                 descriptor_count, descriptor_type,
                 static_cast<long long>(descriptor_length));
    SQLFreeHandle(SQL_HANDLE_DESC, descriptor);
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
    SQLDisconnect(connection);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLFreeHandle(SQL_HANDLE_DESC, descriptor))) {
    SQLFreeHandle(SQL_HANDLE_STMT, statement);
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
  SQLHDESC result_descriptor = SQL_NULL_HDESC;
  SQLWCHAR wide_column_name[16]{};
  SQLSMALLINT wide_column_name_length = 0;
  descriptor_type = 0;
#ifdef ODBCPP_TEST_IODBC
  const auto wide_descriptor_buffer_length =
      static_cast<SQLSMALLINT>(sizeof(wide_column_name));
#ifdef ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE
  // iODBC reports characters when forwarding a same-width call, but bytes
  // when its wide-character bridge converts between driver and app widths.
  const auto iodbc_converts_width =
      sizeof(SQLWCHAR) != ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE;
  constexpr bool driver_width_unknown = false;
#else
  const auto iodbc_converts_width = true;
  constexpr bool driver_width_unknown = true;
#endif
  const auto expected_wide_descriptor_length =
      static_cast<SQLSMALLINT>(iodbc_converts_width
          ? 8 * sizeof(SQLWCHAR) : 8);
#else
  const auto wide_descriptor_buffer_length =
      static_cast<SQLSMALLINT>(std::size(wide_column_name));
  constexpr SQLSMALLINT expected_wide_descriptor_length = 8;
  constexpr bool driver_width_unknown = false;
#endif
  if (!succeeded(SQLGetStmtAttr(
          statement, SQL_ATTR_IMP_ROW_DESC, &result_descriptor, 0,
          nullptr)) ||
      !succeeded(SQLGetDescRecW(
          result_descriptor, 1, wide_column_name,
          wide_descriptor_buffer_length,
          &wide_column_name_length, &descriptor_type, nullptr, nullptr,
          nullptr, nullptr, nullptr)) ||
      (wide_column_name_length != expected_wide_descriptor_length &&
       !(driver_width_unknown && wide_column_name_length == 8)) ||
      wide_column_name[0] != static_cast<SQLWCHAR>('?') ||
      wide_column_name[7] != static_cast<SQLWCHAR>('?') ||
      wide_column_name[8] != 0 ||
      descriptor_type != SQL_INTEGER) {
    print_diagnostic(SQL_HANDLE_DESC, result_descriptor);
    std::fprintf(stderr,
                 "Wide descriptor mismatch: length=%d type=%d first=%u\n",
                 wide_column_name_length, descriptor_type,
                 static_cast<unsigned>(wide_column_name[0]));
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
  const auto wide_native_input = wide_ascii("SELECT {d '2024-02-29'}");
  SQLWCHAR wide_native_output[32]{};
  SQLINTEGER wide_native_length = 0;
  if (!succeeded(SQLNativeSqlW(
          connection, const_cast<SQLWCHAR*>(wide_native_input.data()), SQL_NTS,
          wide_native_output, 32, &wide_native_length)) ||
      wide_native_length != 24 ||
      wide_native_output[7] != static_cast<SQLWCHAR>('D')) {
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
  const auto unicode_query = wide_text(U"SELECT 'Gr\u00fc\u00dfe \u4e16\u754c \U0001f642'::text");
  const auto expected_unicode = wide_text(U"Gr\u00fc\u00dfe \u4e16\u754c \U0001f642");
  SQLWCHAR unicode_value[32]{};
  SQLLEN unicode_value_length = 0;
  const auto expected_unicode_bytes = static_cast<SQLLEN>(
      (expected_unicode.size() - 1) * sizeof(SQLWCHAR));
  const bool unicode_failed = !succeeded(SQLExecDirectW(
          statement, const_cast<SQLWCHAR*>(unicode_query.data()), SQL_NTS)) ||
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLGetData(
          statement, 1, SQL_C_WCHAR, unicode_value, sizeof(unicode_value),
          &unicode_value_length)) ||
      !std::equal(expected_unicode.begin(), expected_unicode.end(),
                  unicode_value) ||
      !succeeded(SQLCloseCursor(statement));
  bool unicode_length_failed = unicode_value_length != expected_unicode_bytes;
#ifdef ODBCPP_TEST_IODBC
  // iODBC 3.52.16 correctly collapses a UTF-16 surrogate pair into one UCS-4
  // value, but its SQLGetData length adjustment still counts the pair as two
  // output units. Accept that manager-reported length while verifying every
  // returned code point above.
  if constexpr (sizeof(SQLWCHAR) == 4) {
    unicode_length_failed = unicode_length_failed &&
        unicode_value_length != expected_unicode_bytes +
            static_cast<SQLLEN>(sizeof(SQLWCHAR));
  }
#endif
  if (unicode_failed || unicode_length_failed) {
    std::fprintf(stderr,
                 "Unicode bridge mismatch: SQLWCHAR=%zu, bytes=%lld, "
                 "expected-bytes=%zu\n",
                 sizeof(SQLWCHAR), static_cast<long long>(unicode_value_length),
                 (expected_unicode.size() - 1) * sizeof(SQLWCHAR));
  }
  if (unicode_failed || unicode_length_failed) {
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
      !succeeded(SQLFetch(statement)) ||
      !succeeded(SQLCloseCursor(statement)) ||
      !succeeded(SQLGetTypeInfoW(statement, SQL_INTEGER)) ||
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
          SQL_SCOPE_CURROW, SQL_NO_NULLS)) ||
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
          SQL_SCOPE_CURROW, SQL_NO_NULLS)) ||
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

  SQLHDBC wide_connection = SQL_NULL_HDBC;
  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_DBC, environment, &wide_connection))) {
    print_diagnostic(SQL_HANDLE_ENV, environment);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  auto wide_connection_string = wide_ascii(
      "DRIVER={ODBCPP PostgreSQL};SERVER=127.0.0.1;PORT=5432;"
      "DATABASE=postgres;UID=postgres;PWD=postgres;SSL=0");
  SQLWCHAR completed_wide_connection_string[256]{};
  SQLSMALLINT completed_wide_length = 0;
  if (!succeeded(SQLDriverConnectW(
          wide_connection, nullptr, wide_connection_string.data(), SQL_NTS,
          completed_wide_connection_string, 256, &completed_wide_length,
          SQL_DRIVER_NOPROMPT)) ||
      completed_wide_length !=
          static_cast<SQLSMALLINT>(wide_connection_string.size() - 1)) {
    print_diagnostic(SQL_HANDLE_DBC, wide_connection);
    SQLFreeHandle(SQL_HANDLE_DBC, wide_connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLDisconnect(wide_connection);
  SQLFreeHandle(SQL_HANDLE_DBC, wide_connection);

  SQLHDBC dsn_connection = SQL_NULL_HDBC;
  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_DBC, environment, &dsn_connection))) {
    print_diagnostic(SQL_HANDLE_ENV, environment);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  auto wide_dsn = wide_ascii("RedshiftProd");
  if (!succeeded(SQLConnectW(
          dsn_connection, wide_dsn.data(), SQL_NTS,
          nullptr, 0, nullptr, 0))) {
    print_diagnostic(SQL_HANDLE_DBC, dsn_connection);
    SQLFreeHandle(SQL_HANDLE_DBC, dsn_connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLDisconnect(dsn_connection);
  SQLFreeHandle(SQL_HANDLE_DBC, dsn_connection);

  SQLHDBC lifecycle_connection = SQL_NULL_HDBC;
  SQLHSTMT premature_statement = SQL_NULL_HSTMT;
  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_DBC, environment, &lifecycle_connection)) ||
      !result_is(SQLDisconnect(lifecycle_connection), SQL_ERROR,
                 "SQLDisconnect before connect") ||
      !diagnostic_is(SQL_HANDLE_DBC, lifecycle_connection, "08003") ||
      !wide_diagnostic_is(SQL_HANDLE_DBC, lifecycle_connection, "08003") ||
      !legacy_diagnostic_is(environment, lifecycle_connection, "08003") ||
      !result_is(SQLAllocHandle(
                     SQL_HANDLE_STMT, lifecycle_connection,
                     &premature_statement),
                 SQL_ERROR, "SQLAllocHandle before connect") ||
      premature_statement != SQL_NULL_HSTMT ||
      !diagnostic_is(SQL_HANDLE_DBC, lifecycle_connection, "08003") ||
      !wide_legacy_diagnostic_is(
          environment, lifecycle_connection, "08003") ||
      !succeeded(SQLDriverConnect(
          lifecycle_connection, nullptr, connection_string, SQL_NTS,
          nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT))) {
    print_diagnostic(SQL_HANDLE_DBC, lifecycle_connection);
    SQLFreeHandle(SQL_HANDLE_DBC, lifecycle_connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLHSTMT lifecycle_statement = SQL_NULL_HSTMT;
  SQLHDESC lifecycle_descriptor = SQL_NULL_HDESC;
  if (!succeeded(SQLAllocHandle(
          SQL_HANDLE_STMT, lifecycle_connection, &lifecycle_statement)) ||
      !succeeded(SQLAllocHandle(
          SQL_HANDLE_DESC, lifecycle_connection, &lifecycle_descriptor))) {
    print_diagnostic(SQL_HANDLE_DBC, lifecycle_connection);
    SQLFreeHandle(SQL_HANDLE_STMT, lifecycle_statement);
    SQLFreeHandle(SQL_HANDLE_DESC, lifecycle_descriptor);
    SQLDisconnect(lifecycle_connection);
    SQLFreeHandle(SQL_HANDLE_DBC, lifecycle_connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  if (!succeeded(SQLDisconnect(lifecycle_connection))) {
    print_diagnostic(SQL_HANDLE_DBC, lifecycle_connection);
    SQLFreeHandle(SQL_HANDLE_STMT, lifecycle_statement);
    SQLFreeHandle(SQL_HANDLE_DESC, lifecycle_descriptor);
    SQLFreeHandle(SQL_HANDLE_DBC, lifecycle_connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  // SQLDisconnect releases subordinate statement and descriptor handles.
  // Calling a Driver Manager again with those stale values is undefined.
  lifecycle_statement = SQL_NULL_HSTMT;
  lifecycle_descriptor = SQL_NULL_HDESC;
  if (!succeeded(SQLFreeHandle(SQL_HANDLE_DBC, lifecycle_connection))) {
    print_diagnostic(SQL_HANDLE_DBC, lifecycle_connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return 1;
  }
  SQLFreeHandle(SQL_HANDLE_ENV, environment);
  return 0;
}
