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
// Linux/macOS - requires ODBC development headers. The driver is normally
// built against unixODBC's two-byte SQLWCHAR ABI; iODBC can translate its
// native four-byte application buffers to that representation.
#include <sql.h>
#include <sqlext.h>
#endif

namespace rs::odbc {

static_assert(sizeof(SQLWCHAR) == 2 || sizeof(SQLWCHAR) == 4,
              "ODBCPP supports only two- or four-byte SQLWCHAR ABIs");

#ifdef ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE
static_assert(sizeof(SQLWCHAR) == ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE,
              "The selected ODBC headers have an unexpected SQLWCHAR ABI");
#endif

// iODBC/DataDirect Unicode negotiation extensions. These are intentionally
// defined as project constants because unixODBC and the Windows SDK do not
// expose iodbcext.h. iODBC uses these numeric values at the driver boundary.
inline constexpr SQLINTEGER IODBC_ATTR_APP_WCHAR_TYPE = 1061;
inline constexpr SQLINTEGER IODBC_ATTR_APP_UNICODE_TYPE = 1064;
inline constexpr SQLINTEGER IODBC_ATTR_DRIVER_UNICODE_TYPE = 1065;
inline constexpr SQLUINTEGER IODBC_CP_UTF16 = 1;
inline constexpr SQLUINTEGER IODBC_CP_UTF8 = 2;
inline constexpr SQLUINTEGER IODBC_CP_UCS4 = 3;
inline constexpr SQLUINTEGER NATIVE_SQLWCHAR_ENCODING =
    sizeof(SQLWCHAR) == 2 ? IODBC_CP_UTF16 : IODBC_CP_UCS4;

// ODBC handle types
enum class HandleType {
  Environment = SQL_HANDLE_ENV,
  Connection = SQL_HANDLE_DBC,
  Statement = SQL_HANDLE_STMT,
  Descriptor = SQL_HANDLE_DESC
};

// SQLSTATE error codes (standard ODBC error states)
constexpr const char* SQLSTATE_SUCCESS = "00000";
constexpr const char* SQLSTATE_CONNECTION_FAILURE = "08001";
constexpr const char* SQLSTATE_CONNECTION_IN_USE = "08002";
constexpr const char* SQLSTATE_SYNTAX_ERROR = "42000";
constexpr const char* SQLSTATE_TIMEOUT = "HYT00";
constexpr const char* SQLSTATE_GENERAL_ERROR = "HY000";
constexpr const char* SQLSTATE_MEMORY_ALLOCATION_ERROR = "HY001";
constexpr const char* SQLSTATE_INVALID_HANDLE = "HY092";
constexpr const char* SQLSTATE_FUNCTION_SEQUENCE_ERROR = "HY010";
constexpr const char* SQLSTATE_INVALID_PARAMETER_NUMBER = "07009";
constexpr const char* SQLSTATE_INDICATOR_VARIABLE_REQUIRED = "22002";
constexpr const char* SQLSTATE_NUMERIC_VALUE_OUT_OF_RANGE = "22003";
constexpr const char* SQLSTATE_INVALID_DATETIME_FORMAT = "22007";
constexpr const char* SQLSTATE_RESTRICTED_DATA_TYPE = "07006";
constexpr const char* SQLSTATE_INVALID_CHARACTER_VALUE = "22018";
constexpr const char* SQLSTATE_STRING_DATA_TRUNCATED = "01004";
constexpr const char* SQLSTATE_FRACTIONAL_TRUNCATION = "01S07";
constexpr const char* SQLSTATE_INVALID_STRING_LENGTH = "HY090";
constexpr const char* SQLSTATE_INVALID_NULL_POINTER = "HY009";
constexpr const char* SQLSTATE_INVALID_APPLICATION_BUFFER_TYPE = "HY003";
constexpr const char* SQLSTATE_INVALID_SQL_DATA_TYPE = "HY004";
constexpr const char* SQLSTATE_INVALID_PARAMETER_TYPE = "HY105";
constexpr const char* SQLSTATE_ATTRIBUTE_CANNOT_BE_SET = "HY011";
constexpr const char* SQLSTATE_INVALID_ATTRIBUTE_VALUE = "HY024";
constexpr const char* SQLSTATE_INVALID_ATTRIBUTE = "HY092";
constexpr const char* SQLSTATE_CONNECTION_TIMEOUT = "HYT01";
constexpr const char* SQLSTATE_CONNECTION_NOT_OPEN = "08003";
constexpr const char* SQLSTATE_INVALID_TRANSACTION_STATE = "25000";
constexpr const char* SQLSTATE_INVALID_TRANSACTION_OPERATION = "HY012";
constexpr const char* SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED = "HYC00";
constexpr const char* SQLSTATE_INVALID_DRIVER_COMPLETION = "HY110";
constexpr const char* SQLSTATE_INVALID_OPTION_VALUE = "HY100";
constexpr const char* SQLSTATE_INVALID_INFORMATION_TYPE = "HY096";
constexpr const char* SQLSTATE_COLUMN_TYPE_OUT_OF_RANGE = "HY097";
constexpr const char* SQLSTATE_SCOPE_OUT_OF_RANGE = "HY098";
constexpr const char* SQLSTATE_NULLABLE_TYPE_OUT_OF_RANGE = "HY099";
constexpr const char* SQLSTATE_FETCH_TYPE_OUT_OF_RANGE = "HY106";
constexpr const char* SQLSTATE_INVALID_CURSOR_STATE = "24000";
constexpr const char* SQLSTATE_STATEMENT_NOT_PREPARED = "HY007";
constexpr const char* SQLSTATE_CANNOT_MODIFY_IRD = "HY016";
constexpr const char* SQLSTATE_INVALID_AUTO_DESCRIPTOR_USE = "HY017";

// Note: All SQL_DIAG_* constants are already defined in system ODBC headers (sql.h, sqlext.h)
// No need to redefine them here

} // namespace rs::odbc
