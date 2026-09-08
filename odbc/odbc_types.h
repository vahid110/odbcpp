#pragma once

// Use system ODBC headers for ABI compatibility
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#else
// Linux/macOS - requires unixODBC development headers
// Install: apt-get install unixodbc-dev (Linux) or brew install unixodbc (macOS)
#include <sql.h>
#include <sqlext.h>
#endif

namespace rs::odbc {

// ODBC handle types
enum class HandleType {
  Environment = SQL_HANDLE_ENV,
  Connection = SQL_HANDLE_DBC,
  Statement = SQL_HANDLE_STMT
};

// SQLSTATE error codes (standard ODBC error states)
constexpr const char* SQLSTATE_SUCCESS = "00000";
constexpr const char* SQLSTATE_CONNECTION_FAILURE = "08001";
constexpr const char* SQLSTATE_SYNTAX_ERROR = "42000";
constexpr const char* SQLSTATE_TIMEOUT = "HYT00";
constexpr const char* SQLSTATE_GENERAL_ERROR = "HY000";
constexpr const char* SQLSTATE_INVALID_HANDLE = "HY092";
constexpr const char* SQLSTATE_FUNCTION_SEQUENCE_ERROR = "HY010";
constexpr const char* SQLSTATE_INVALID_PARAMETER_NUMBER = "07009";
constexpr const char* SQLSTATE_INDICATOR_VARIABLE_REQUIRED = "22002";
constexpr const char* SQLSTATE_RESTRICTED_DATA_TYPE = "07006";
constexpr const char* SQLSTATE_INVALID_CHARACTER_VALUE = "22018";
constexpr const char* SQLSTATE_STRING_DATA_TRUNCATED = "01004";
constexpr const char* SQLSTATE_INVALID_STRING_LENGTH = "HY090";
constexpr const char* SQLSTATE_INVALID_NULL_POINTER = "HY009";
constexpr const char* SQLSTATE_ATTRIBUTE_CANNOT_BE_SET = "HY011";
constexpr const char* SQLSTATE_INVALID_ATTRIBUTE_VALUE = "HY024";
constexpr const char* SQLSTATE_INVALID_ATTRIBUTE = "HY092";
constexpr const char* SQLSTATE_CONNECTION_TIMEOUT = "HYT01";
constexpr const char* SQLSTATE_CONNECTION_NOT_OPEN = "08003";
constexpr const char* SQLSTATE_INVALID_TRANSACTION_OPERATION = "HY012";
constexpr const char* SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED = "HYC00";
constexpr const char* SQLSTATE_INVALID_DRIVER_COMPLETION = "HY110";
constexpr const char* SQLSTATE_INVALID_CURSOR_STATE = "24000";
constexpr const char* SQLSTATE_STATEMENT_NOT_PREPARED = "HY007";

// Note: All SQL_DIAG_* constants are already defined in system ODBC headers (sql.h, sqlext.h)
// No need to redefine them here

} // namespace rs::odbc
