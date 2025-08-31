#pragma once

// Use system ODBC headers for ABI compatibility
#ifdef _WIN32
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

// SQLSTATE error codes
constexpr const char* SQLSTATE_SUCCESS = "00000";
constexpr const char* SQLSTATE_CONNECTION_FAILURE = "08001";
constexpr const char* SQLSTATE_SYNTAX_ERROR = "42000";
constexpr const char* SQLSTATE_TIMEOUT = "HYT00";
constexpr const char* SQLSTATE_GENERAL_ERROR = "HY000";

} // namespace rs::odbc