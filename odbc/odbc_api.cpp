#include "odbc_api.h"
#include "odbc_handles.h"
#include "unicode.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

using namespace rs::odbc;

// String conversion helpers (Unicode-ready architecture)
namespace {
  // ANSI string conversion
  std::string sqlchar_to_string(SQLCHAR* str, SQLINTEGER length) {
    if (!str) return "";
    if (length == SQL_NTS) return reinterpret_cast<char*>(str);
    return std::string(reinterpret_cast<char*>(str), length);
  }
  
  // Internal UTF-8 string handling (ready for Wide function support)
  std::string normalize_string(const std::string& input) {
    // Currently pass-through, but ready for UTF-8 validation/conversion
    return input;
  }

  template <typename Length>
  SQLRETURN write_wide_output(
      ODBCHandle* handle, std::string_view utf8, SQLWCHAR* output,
      SQLINTEGER buffer_length, Length* output_length,
      std::string_view truncation_message, bool set_truncation_diagnostic = true) {
    const auto wide = utf8_to_wide(utf8);
    if (!wide) {
      if (handle) {
        handle->set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                          "Invalid UTF-8 output text");
      }
      return SQL_ERROR;
    }
    if (output_length) {
      *output_length = static_cast<Length>(std::min<std::size_t>(
          wide->size(),
          static_cast<std::size_t>(std::numeric_limits<Length>::max())));
    }
    if (!output || buffer_length <= 0) return SQL_SUCCESS;

    const auto copied = std::min<std::size_t>(
        wide->size(), static_cast<std::size_t>(buffer_length - 1));
    std::copy_n(wide->begin(), copied, output);
    output[copied] = 0;
    if (copied < wide->size()) {
      if (handle && set_truncation_diagnostic) {
        handle->set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                          std::string(truncation_message));
      }
      return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  }

  template <typename Length>
  SQLRETURN write_wide_bytes_output(
      ODBCHandle* handle, std::string_view utf8, SQLWCHAR* output,
      SQLINTEGER buffer_length, Length* output_length,
      std::string_view truncation_message,
      bool set_truncation_diagnostic = true) {
    const auto wide = utf8_to_wide(utf8);
    if (!wide) {
      if (handle) {
        handle->set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                          "Invalid UTF-8 output text");
      }
      return SQL_ERROR;
    }
    const auto required_bytes = wide->size() * sizeof(SQLWCHAR);
    if (output_length) {
      *output_length = static_cast<Length>(std::min<std::size_t>(
          required_bytes,
          static_cast<std::size_t>(std::numeric_limits<Length>::max())));
    }
    if (!output || buffer_length <= 0) return SQL_SUCCESS;

    const auto buffer_units =
        static_cast<std::size_t>(buffer_length) / sizeof(SQLWCHAR);
    const auto capacity = buffer_units > 0 ? buffer_units - 1 : 0;
    auto copied = std::min(capacity, wide->size());
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (copied < wide->size() && copied > 0 &&
          (*wide)[copied - 1] >= 0xd800 &&
          (*wide)[copied - 1] <= 0xdbff) {
        --copied;
      }
    }
    if (copied > 0) std::copy_n(wide->begin(), copied, output);
    if (buffer_units > 0) output[copied] = 0;
    if (copied < wide->size()) {
      if (handle && set_truncation_diagnostic) {
        handle->set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                          std::string(truncation_message));
      }
      return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  }

  bool read_wide_argument(ODBCHandle* handle, SQLWCHAR* value,
                          SQLSMALLINT length,
                          std::optional<std::string>& output,
                          std::string_view function_name) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) {
      handle->set_error(
          SQLSTATE_INVALID_STRING_LENGTH,
          "Invalid " + std::string(function_name) + " argument length");
      return false;
    }
    const auto converted = sqlwchar_to_utf8(value, length);
    if (!converted) {
      handle->set_error(
          SQLSTATE_INVALID_CHARACTER_VALUE,
          "Invalid wide-character " + std::string(function_name) +
              " argument");
      return false;
    }
    output = normalize_string(*converted);
    return true;
  }

  std::optional<std::string_view> string_info_value(
      SQLUSMALLINT info_type) {
    switch (info_type) {
      case SQL_DRIVER_NAME: return "ODBCPP Driver";
      case SQL_DRIVER_VER: return "01.00.0000";
      case SQL_DRIVER_ODBC_VER:
      case SQL_ODBC_VER: return "03.80";
      case SQL_DBMS_NAME:
#ifdef ODBCPP_ENABLE_REDSHIFT
        return "Amazon Redshift";
#else
        return "PostgreSQL";
#endif
      case SQL_IDENTIFIER_QUOTE_CHAR: return "\"";
      case SQL_CATALOG_NAME_SEPARATOR: return ".";
      case SQL_CATALOG_TERM: return "database";
      case SQL_SCHEMA_TERM: return "schema";
      case SQL_TABLE_TERM: return "table";
      case SQL_PROCEDURE_TERM: return "procedure";
      case SQL_SEARCH_PATTERN_ESCAPE: return "\\";
      case SQL_CATALOG_NAME:
      case SQL_COLUMN_ALIAS:
      case SQL_ACCESSIBLE_TABLES:
      case SQL_ACCESSIBLE_PROCEDURES:
      case SQL_MULT_RESULT_SETS: return "Y";
      case SQL_DATA_SOURCE_READ_ONLY:
      case SQL_MULTIPLE_ACTIVE_TXN:
      case SQL_NEED_LONG_DATA_LEN:
      case SQL_ORDER_BY_COLUMNS_IN_SELECT: return "N";
      default: return std::nullopt;
    }
  }

  SQLRETURN write_narrow_output(
      ODBCHandle* handle, std::string_view value, SQLCHAR* output,
      SQLSMALLINT buffer_length, SQLSMALLINT* output_length,
      std::string_view truncation_message) {
    if (buffer_length < 0) {
      handle->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                        "Invalid output buffer length");
      return SQL_ERROR;
    }
    if (output_length) {
      *output_length = static_cast<SQLSMALLINT>(std::min<std::size_t>(
          value.size(), static_cast<std::size_t>(
                            std::numeric_limits<SQLSMALLINT>::max())));
    }
    if (!output || buffer_length <= 0) return SQL_SUCCESS;
    const auto copied = std::min<std::size_t>(
        value.size(), static_cast<std::size_t>(buffer_length - 1));
    std::memcpy(output, value.data(), copied);
    output[copied] = 0;
    if (copied < value.size()) {
      handle->set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                        std::string(truncation_message));
      return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  }

  bool is_supported_function(SQLUSMALLINT function_id) {
    switch (function_id) {
      case SQL_API_SQLALLOCHANDLE:
      case SQL_API_SQLBINDCOL:
      case SQL_API_SQLBINDPARAMETER:
      case SQL_API_SQLCOLATTRIBUTE:
      case SQL_API_SQLCLOSECURSOR:
      case SQL_API_SQLCOLUMNS:
      case SQL_API_SQLCONNECT:
      case SQL_API_SQLCOPYDESC:
      case SQL_API_SQLDESCRIBECOL:
      case SQL_API_SQLDESCRIBEPARAM:
      case SQL_API_SQLDISCONNECT:
      case SQL_API_SQLDRIVERCONNECT:
      case SQL_API_SQLENDTRAN:
      case SQL_API_SQLERROR:
      case SQL_API_SQLEXECDIRECT:
      case SQL_API_SQLEXECUTE:
      case SQL_API_SQLFETCH:
      case SQL_API_SQLFETCHSCROLL:
      case SQL_API_SQLFOREIGNKEYS:
      case SQL_API_SQLFREEHANDLE:
      case SQL_API_SQLFREESTMT:
      case SQL_API_SQLGETCONNECTATTR:
      case SQL_API_SQLGETDATA:
      case SQL_API_SQLGETDESCFIELD:
      case SQL_API_SQLGETDIAGFIELD:
      case SQL_API_SQLGETDIAGREC:
      case SQL_API_SQLGETENVATTR:
      case SQL_API_SQLGETFUNCTIONS:
      case SQL_API_SQLGETINFO:
      case SQL_API_SQLGETSTMTATTR:
      case SQL_API_SQLGETTYPEINFO:
      case SQL_API_SQLNUMRESULTCOLS:
      case SQL_API_SQLNUMPARAMS:
      case SQL_API_SQLNATIVESQL:
      case SQL_API_SQLMORERESULTS:
      case SQL_API_SQLPREPARE:
      case SQL_API_SQLPRIMARYKEYS:
      case SQL_API_SQLPROCEDURECOLUMNS:
      case SQL_API_SQLPROCEDURES:
      case SQL_API_SQLROWCOUNT:
      case SQL_API_SQLSETCONNECTATTR:
      case SQL_API_SQLSETDESCFIELD:
      case SQL_API_SQLSETENVATTR:
      case SQL_API_SQLSETSTMTATTR:
      case SQL_API_SQLSPECIALCOLUMNS:
      case SQL_API_SQLSTATISTICS:
      case SQL_API_SQLTABLES:
        return true;
      default:
        return false;
    }
  }
}

// Helper to validate handle
template<typename T>
T* get_valid_handle(SQLHANDLE handle) {
  return HandleRegistry::instance().get_handle_as<T>(handle);
}

extern "C" {

SQLRETURN SQLAllocHandle(SQLSMALLINT handle_type, SQLHANDLE input_handle, SQLHANDLE* output_handle) {
  if (!output_handle) return SQL_INVALID_HANDLE;
  
  try {
    std::unique_ptr<ODBCHandle> new_handle;
    
    switch (handle_type) {
      case SQL_HANDLE_ENV: {
        new_handle = std::make_unique<ODBCEnvironment>();
        break;
      }
      case SQL_HANDLE_DBC: {
        auto* env = get_valid_handle<ODBCEnvironment>(input_handle);
        if (!env) return SQL_INVALID_HANDLE;
        new_handle = std::make_unique<ODBCConnection>(env);
        break;
      }
      case SQL_HANDLE_STMT: {
        auto* conn = get_valid_handle<ODBCConnection>(input_handle);
        if (!conn) return SQL_INVALID_HANDLE;
        new_handle = std::make_unique<ODBCStatement>(conn);
        break;
      }
      case SQL_HANDLE_DESC: {
        auto* conn = get_valid_handle<ODBCConnection>(input_handle);
        if (!conn) return SQL_INVALID_HANDLE;
        new_handle = std::make_unique<ODBCDescriptor>(conn);
        break;
      }
      default: {
        // Set diagnostic for invalid handle type
        if (input_handle) {
          auto* parent = HandleRegistry::instance().get_handle(input_handle);
          if (parent) {
            parent->set_error(SQLSTATE_GENERAL_ERROR, "Invalid handle type");
          }
        }
        return SQL_ERROR;
      }
    }
    
    // Use the object pointer as the handle
    SQLHANDLE handle = reinterpret_cast<SQLHANDLE>(new_handle.get());
    HandleRegistry::instance().register_handle(handle, std::move(new_handle));
    *output_handle = handle;
    
    return SQL_SUCCESS;
    
  } catch (const std::exception& e) {
    // Set diagnostic on parent handle if available
    if (input_handle) {
      auto* parent = HandleRegistry::instance().get_handle(input_handle);
      if (parent) {
        parent->set_error(SQLSTATE_GENERAL_ERROR, std::string("Handle allocation failed: ") + e.what());
      }
    }
    return SQL_ERROR;
  } catch (...) {
    // Set diagnostic on parent handle if available
    if (input_handle) {
      auto* parent = HandleRegistry::instance().get_handle(input_handle);
      if (parent) {
        parent->set_error(SQLSTATE_GENERAL_ERROR, "Handle allocation failed: unknown error");
      }
    }
    return SQL_ERROR;
  }
}

SQLRETURN SQLFreeHandle(SQLSMALLINT handle_type, SQLHANDLE handle) {
  if (!handle) return SQL_INVALID_HANDLE;
  
  auto* obj = HandleRegistry::instance().get_handle(handle);
  if (!obj || static_cast<int>(obj->get_type()) != handle_type) {
    return SQL_INVALID_HANDLE;
  }
  if (handle_type == SQL_HANDLE_DESC) {
    auto* descriptor = static_cast<ODBCDescriptor*>(obj);
    if (descriptor->is_automatically_allocated()) {
      descriptor->set_error(SQLSTATE_INVALID_AUTO_DESCRIPTOR_USE,
                            "Implicit descriptor handles cannot be freed");
      return SQL_ERROR;
    }
  }
  
  HandleRegistry::instance().unregister_handle(handle);
  return SQL_SUCCESS;
}

SQLRETURN SQLConnect(SQLHDBC connection_handle, 
                    SQLCHAR* server_name, SQLSMALLINT name_length1,
                    SQLCHAR* user_name, SQLSMALLINT name_length2, 
                    SQLCHAR* authentication, SQLSMALLINT name_length3) {
  
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  
  std::string dsn = normalize_string(sqlchar_to_string(server_name, name_length1));
  std::string user = normalize_string(sqlchar_to_string(user_name, name_length2));
  std::string password = normalize_string(sqlchar_to_string(authentication, name_length3));
  
  return conn->connect(dsn, user, password);
}

SQLRETURN SQLConnectW(SQLHDBC connection_handle,
                      SQLWCHAR* server_name, SQLSMALLINT name_length1,
                      SQLWCHAR* user_name, SQLSMALLINT name_length2,
                      SQLWCHAR* authentication, SQLSMALLINT name_length3) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  if ((name_length1 < 0 && name_length1 != SQL_NTS) ||
      (name_length2 < 0 && name_length2 != SQL_NTS) ||
      (name_length3 < 0 && name_length3 != SQL_NTS)) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid wide-character connection input length");
    return SQL_ERROR;
  }

  const auto dsn = sqlwchar_to_utf8(server_name, name_length1);
  const auto user = sqlwchar_to_utf8(user_name, name_length2);
  const auto password = sqlwchar_to_utf8(authentication, name_length3);
  if (!dsn || !user || !password) {
    conn->set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid wide-character connection input");
    return SQL_ERROR;
  }
  return conn->connect(normalize_string(*dsn), normalize_string(*user),
                       normalize_string(*password));
}

SQLRETURN SQLDriverConnect(
    SQLHDBC connection_handle, SQLHWND, SQLCHAR* connection_string_in,
    SQLSMALLINT string_length1, SQLCHAR* connection_string_out,
    SQLSMALLINT buffer_length, SQLSMALLINT* string_length2,
    SQLUSMALLINT driver_completion) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  if (!connection_string_in) {
    conn->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "Input connection string is null");
    return SQL_ERROR;
  }
  if (string_length1 < 0 && string_length1 != SQL_NTS) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid input connection string length");
    return SQL_ERROR;
  }
  if (buffer_length < 0) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid output connection string buffer length");
    return SQL_ERROR;
  }
  if (driver_completion != SQL_DRIVER_NOPROMPT &&
      driver_completion != SQL_DRIVER_COMPLETE &&
      driver_completion != SQL_DRIVER_COMPLETE_REQUIRED &&
      driver_completion != SQL_DRIVER_PROMPT) {
    conn->set_error(SQLSTATE_INVALID_DRIVER_COMPLETION,
                    "Invalid driver completion mode");
    return SQL_ERROR;
  }
  if (driver_completion == SQL_DRIVER_PROMPT) {
    conn->set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                    "Interactive connection prompting is not implemented");
    return SQL_ERROR;
  }

  const auto connection_string = normalize_string(
      sqlchar_to_string(connection_string_in, string_length1));
  const auto connect_result = conn->connect(connection_string, {}, {});
  if (connect_result != SQL_SUCCESS) return connect_result;

  const auto output_length = connection_string.size();
  if (string_length2) {
    *string_length2 = static_cast<SQLSMALLINT>(
        std::min(output_length,
                 static_cast<std::size_t>(std::numeric_limits<SQLSMALLINT>::max())));
  }
  if (!connection_string_out || buffer_length <= 0) return SQL_SUCCESS;

  const auto copied_length = std::min(
      output_length, static_cast<std::size_t>(buffer_length - 1));
  std::memcpy(connection_string_out, connection_string.data(), copied_length);
  connection_string_out[copied_length] = '\0';
  if (copied_length < output_length) {
    conn->set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                    "Output connection string was truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

SQLRETURN SQLDriverConnectW(
    SQLHDBC connection_handle, SQLHWND, SQLWCHAR* connection_string_in,
    SQLSMALLINT string_length1, SQLWCHAR* connection_string_out,
    SQLSMALLINT buffer_length, SQLSMALLINT* string_length2,
    SQLUSMALLINT driver_completion) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  if (!connection_string_in) {
    conn->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "Input connection string is null");
    return SQL_ERROR;
  }
  if (string_length1 < 0 && string_length1 != SQL_NTS) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid input connection string length");
    return SQL_ERROR;
  }
  if (buffer_length < 0) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid output connection string buffer length");
    return SQL_ERROR;
  }
  if (driver_completion != SQL_DRIVER_NOPROMPT &&
      driver_completion != SQL_DRIVER_COMPLETE &&
      driver_completion != SQL_DRIVER_COMPLETE_REQUIRED &&
      driver_completion != SQL_DRIVER_PROMPT) {
    conn->set_error(SQLSTATE_INVALID_DRIVER_COMPLETION,
                    "Invalid driver completion mode");
    return SQL_ERROR;
  }
  if (driver_completion == SQL_DRIVER_PROMPT) {
    conn->set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                    "Interactive connection prompting is not implemented");
    return SQL_ERROR;
  }

  const auto connection_string =
      sqlwchar_to_utf8(connection_string_in, string_length1);
  if (!connection_string) {
    conn->set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid wide-character connection string");
    return SQL_ERROR;
  }
  const auto normalized = normalize_string(*connection_string);
  const auto connect_result = conn->connect(normalized, {}, {});
  if (connect_result != SQL_SUCCESS) return connect_result;

  return write_wide_output(
      conn, normalized, connection_string_out, buffer_length, string_length2,
      "Output connection string was truncated");
}

SQLRETURN SQLDisconnect(SQLHDBC connection_handle) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  
  return conn->disconnect();
}

SQLRETURN SQLSetConnectAttr(SQLHDBC connection_handle, SQLINTEGER attribute,
                            SQLPOINTER value, SQLINTEGER) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  return conn->set_attribute(
      attribute, static_cast<SQLULEN>(reinterpret_cast<std::uintptr_t>(value)));
}

SQLRETURN SQLSetConnectAttrW(SQLHDBC connection_handle, SQLINTEGER attribute,
                             SQLPOINTER value, SQLINTEGER string_length) {
  return SQLSetConnectAttr(
      connection_handle, attribute, value, string_length);
}

SQLRETURN SQLGetConnectAttr(SQLHDBC connection_handle, SQLINTEGER attribute,
                            SQLPOINTER value, SQLINTEGER,
                            SQLINTEGER* string_length) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  if (!value) {
    conn->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "Null connection attribute output pointer");
    return SQL_ERROR;
  }
  const auto result = conn->get_attribute(
      attribute, static_cast<SQLUINTEGER*>(value));
  if (result == SQL_SUCCESS && string_length) {
    *string_length = static_cast<SQLINTEGER>(sizeof(SQLUINTEGER));
  }
  return result;
}

SQLRETURN SQLGetConnectAttrW(SQLHDBC connection_handle, SQLINTEGER attribute,
                             SQLPOINTER value, SQLINTEGER buffer_length,
                             SQLINTEGER* string_length) {
  return SQLGetConnectAttr(
      connection_handle, attribute, value, buffer_length, string_length);
}

SQLRETURN SQLEndTran(SQLSMALLINT handle_type, SQLHANDLE handle,
                     SQLSMALLINT completion_type) {
  if (handle_type == SQL_HANDLE_DBC) {
    auto* conn = get_valid_handle<ODBCConnection>(handle);
    if (!conn) return SQL_INVALID_HANDLE;
    return conn->end_transaction(completion_type);
  }
  if (handle_type == SQL_HANDLE_ENV) {
    auto* environment = get_valid_handle<ODBCEnvironment>(handle);
    if (!environment) return SQL_INVALID_HANDLE;
    environment->set_error(
        SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
        "Environment-wide transaction completion is not implemented");
    return SQL_ERROR;
  }
  return SQL_INVALID_HANDLE;
}

SQLRETURN SQLExecDirect(SQLHSTMT statement_handle, SQLCHAR* statement_text, SQLINTEGER text_length) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  std::string sql = normalize_string(sqlchar_to_string(statement_text, text_length));
  return stmt->execute_direct(sql);
}

SQLRETURN SQLExecDirectW(SQLHSTMT statement_handle,
                         SQLWCHAR* statement_text, SQLINTEGER text_length) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  if (!statement_text) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER, "SQL statement is null");
    return SQL_ERROR;
  }
  if (text_length < 0 && text_length != SQL_NTS) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQL statement length");
    return SQL_ERROR;
  }

  const auto sql = sqlwchar_to_utf8(statement_text, text_length);
  if (!sql) {
    stmt->set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid wide-character SQL statement");
    return SQL_ERROR;
  }
  return stmt->execute_direct(normalize_string(*sql));
}

SQLRETURN SQLFetch(SQLHSTMT statement_handle) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->fetch();
}

SQLRETURN SQLFetchScroll(SQLHSTMT statement_handle,
                         SQLSMALLINT fetch_orientation, SQLLEN) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  if (fetch_orientation == SQL_FETCH_NEXT) return stmt->fetch();

  switch (fetch_orientation) {
    case SQL_FETCH_PRIOR:
    case SQL_FETCH_FIRST:
    case SQL_FETCH_LAST:
    case SQL_FETCH_ABSOLUTE:
    case SQL_FETCH_RELATIVE:
    case SQL_FETCH_BOOKMARK:
      stmt->set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                      "Scrollable fetch orientation is not supported");
      break;
    default:
      stmt->set_error(SQLSTATE_FETCH_TYPE_OUT_OF_RANGE,
                      "Invalid fetch orientation");
      break;
  }
  return SQL_ERROR;
}

SQLRETURN SQLMoreResults(SQLHSTMT statement_handle) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  return stmt->more_results();
}

SQLRETURN SQLGetData(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLSMALLINT target_type,
                    void* target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->get_data(column_number, target_type, target_value, buffer_length, strlen_or_indicator);
}

SQLRETURN SQLSetStmtAttr(SQLHSTMT statement_handle, SQLINTEGER attribute,
                         SQLPOINTER value, SQLINTEGER) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  return stmt->set_attribute(
      attribute, static_cast<SQLULEN>(reinterpret_cast<std::uintptr_t>(value)));
}

SQLRETURN SQLSetStmtAttrW(SQLHSTMT statement_handle, SQLINTEGER attribute,
                          SQLPOINTER value, SQLINTEGER string_length) {
  return SQLSetStmtAttr(statement_handle, attribute, value, string_length);
}

SQLRETURN SQLGetStmtAttr(SQLHSTMT statement_handle, SQLINTEGER attribute,
                         SQLPOINTER value, SQLINTEGER,
                         SQLINTEGER* string_length) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  if (!value) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "Null statement attribute output pointer");
    return SQL_ERROR;
  }
  const auto result = stmt->get_attribute(
      attribute, static_cast<SQLULEN*>(value));
  if (result == SQL_SUCCESS && string_length) {
    *string_length = static_cast<SQLINTEGER>(sizeof(SQLULEN));
  }
  return result;
}

SQLRETURN SQLGetStmtAttrW(SQLHSTMT statement_handle, SQLINTEGER attribute,
                          SQLPOINTER value, SQLINTEGER buffer_length,
                          SQLINTEGER* string_length) {
  return SQLGetStmtAttr(
      statement_handle, attribute, value, buffer_length, string_length);
}

SQLRETURN SQLCloseCursor(SQLHSTMT statement_handle) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  return stmt->close_cursor(true);
}

SQLRETURN SQLFreeStmt(SQLHSTMT statement_handle, SQLUSMALLINT option) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  switch (option) {
    case SQL_CLOSE:
      return stmt->close_cursor(false);
    case SQL_UNBIND:
      stmt->unbind_columns();
      return SQL_SUCCESS;
    case SQL_RESET_PARAMS:
      stmt->reset_parameters();
      return SQL_SUCCESS;
    case SQL_DROP:
      HandleRegistry::instance().unregister_handle(statement_handle);
      return SQL_SUCCESS;
    default:
      stmt->set_error(SQLSTATE_INVALID_ATTRIBUTE,
                      "Invalid SQLFreeStmt option");
      return SQL_ERROR;
  }
}

SQLRETURN SQLGetDiagRec(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
                       SQLCHAR* sqlstate, SQLINTEGER* native_error, SQLCHAR* message_text,
                       SQLSMALLINT buffer_length, SQLSMALLINT* text_length) {
  if (rec_number < 1) {
    // Invalid record number - set diagnostic on handle if possible
    auto* obj = HandleRegistry::instance().get_handle(handle);
    if (obj) {
      obj->set_error(SQLSTATE_GENERAL_ERROR, "Invalid diagnostic record number");
    }
    return SQL_ERROR;
  }
  
  auto* obj = HandleRegistry::instance().get_handle(handle);
  if (!obj) return SQL_INVALID_HANDLE;
  
  const auto* record = obj->get_diagnostic_record(rec_number);
  if (!record) return SQL_NO_DATA;
  
  // Copy SQLSTATE (always 5 characters + null terminator)
  if (sqlstate) {
    const auto state_length = std::min<std::size_t>(5, record->sqlstate.size());
    std::memcpy(sqlstate, record->sqlstate.data(), state_length);
    sqlstate[state_length] = '\0';
  }
  
  // Set native error code
  if (native_error) {
    *native_error = record->native_error;
  }
  
  // Copy message text with proper truncation handling
  if (message_text && buffer_length > 0) {
    size_t msg_len = record->message_text.length();
    size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), msg_len);
    
    std::memcpy(message_text, record->message_text.data(), copy_len);
    message_text[copy_len] = '\0';
    
    if (text_length) {
      *text_length = static_cast<SQLSMALLINT>(msg_len);
    }
    
    // Return SQL_SUCCESS_WITH_INFO if message was truncated
    if (copy_len < msg_len) {
      return SQL_SUCCESS_WITH_INFO;
    }
  } else if (text_length) {
    *text_length = static_cast<SQLSMALLINT>(record->message_text.length());
  }
  
  return SQL_SUCCESS;
}

SQLRETURN SQLGetDiagRecW(SQLSMALLINT, SQLHANDLE handle,
                         SQLSMALLINT rec_number, SQLWCHAR* sqlstate,
                         SQLINTEGER* native_error, SQLWCHAR* message_text,
                         SQLSMALLINT buffer_length,
                         SQLSMALLINT* text_length) {
  if (rec_number < 1) {
    auto* obj = HandleRegistry::instance().get_handle(handle);
    if (obj) {
      obj->set_error(SQLSTATE_GENERAL_ERROR,
                     "Invalid diagnostic record number");
    }
    return SQL_ERROR;
  }

  auto* obj = HandleRegistry::instance().get_handle(handle);
  if (!obj) return SQL_INVALID_HANDLE;
  if (buffer_length < 0) return SQL_ERROR;

  const auto* record = obj->get_diagnostic_record(rec_number);
  if (!record) return SQL_NO_DATA;

  if (sqlstate) {
    const auto state_length = std::min<std::size_t>(5, record->sqlstate.size());
    for (std::size_t i = 0; i < state_length; ++i) {
      sqlstate[i] = static_cast<SQLWCHAR>(
          static_cast<unsigned char>(record->sqlstate[i]));
    }
    sqlstate[state_length] = 0;
  }
  if (native_error) *native_error = record->native_error;

  return write_wide_output(
      obj, record->message_text, message_text, buffer_length, text_length,
      "Diagnostic message was truncated", false);
}

SQLRETURN SQLGetDiagField(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
                         SQLSMALLINT diag_identifier, SQLPOINTER diag_info_ptr, SQLSMALLINT buffer_length,
                         SQLSMALLINT* string_length_ptr) {
  auto* obj = HandleRegistry::instance().get_handle(handle);
  if (!obj) return SQL_INVALID_HANDLE;
  
  // Header fields (rec_number = 0)
  if (rec_number == 0) {
    switch (diag_identifier) {
      case SQL_DIAG_NUMBER: {
        if (diag_info_ptr) {
          *static_cast<SQLINTEGER*>(diag_info_ptr) = static_cast<SQLINTEGER>(obj->get_diagnostic_count());
        }
        return SQL_SUCCESS;
      }
      case SQL_DIAG_RETURNCODE: {
        if (diag_info_ptr) {
          // Return the last return code (simplified - would need to track per operation)
          *static_cast<SQLRETURN*>(diag_info_ptr) = obj->get_diagnostic_count() > 0 ? SQL_ERROR : SQL_SUCCESS;
        }
        return SQL_SUCCESS;
      }
      default:
        obj->set_error(SQLSTATE_GENERAL_ERROR, "Invalid diagnostic field identifier");
        return SQL_ERROR;
    }
  }
  
  // Record fields (rec_number > 0)
  const auto* record = obj->get_diagnostic_record(rec_number);
  if (!record) return SQL_NO_DATA;
  
  auto copy_string = [&](const std::string& str) -> SQLRETURN {
    if (string_length_ptr) {
      *string_length_ptr = static_cast<SQLSMALLINT>(str.length());
    }
    
    if (diag_info_ptr && buffer_length > 0) {
      size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), str.length());
      std::memcpy(diag_info_ptr, str.data(), copy_len);
      static_cast<char*>(diag_info_ptr)[copy_len] = '\0';
      
      return (copy_len < str.length()) ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    }
    
    return SQL_SUCCESS;
  };
  
  switch (diag_identifier) {
    case SQL_DIAG_SQLSTATE:
      return copy_string(record->sqlstate);
    case SQL_DIAG_NATIVE:
      if (diag_info_ptr) {
        *static_cast<SQLINTEGER*>(diag_info_ptr) = record->native_error;
      }
      return SQL_SUCCESS;
    case SQL_DIAG_MESSAGE_TEXT:
      return copy_string(record->message_text);
    case SQL_DIAG_CLASS_ORIGIN:
      return copy_string(record->class_origin);
    case SQL_DIAG_SUBCLASS_ORIGIN:
      return copy_string(record->subclass_origin);
    case SQL_DIAG_CONNECTION_NAME:
      return copy_string(record->connection_name);
    case SQL_DIAG_SERVER_NAME:
      return copy_string(record->server_name);
    default:
      return SQL_ERROR;
  }
}

SQLRETURN SQLGetDiagFieldW(
    SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
    SQLSMALLINT diag_identifier, SQLPOINTER diag_info_ptr,
    SQLSMALLINT buffer_length, SQLSMALLINT* string_length_ptr) {
  auto* obj = HandleRegistry::instance().get_handle(handle);
  if (!obj) return SQL_INVALID_HANDLE;
  if (buffer_length < 0) return SQL_ERROR;
  if (rec_number == 0 || diag_identifier == SQL_DIAG_NATIVE) {
    return SQLGetDiagField(handle_type, handle, rec_number, diag_identifier,
                           diag_info_ptr, buffer_length, string_length_ptr);
  }

  const auto* record = obj->get_diagnostic_record(rec_number);
  if (!record) return SQL_NO_DATA;
  const std::string* value = nullptr;
  switch (diag_identifier) {
    case SQL_DIAG_SQLSTATE: value = &record->sqlstate; break;
    case SQL_DIAG_MESSAGE_TEXT: value = &record->message_text; break;
    case SQL_DIAG_CLASS_ORIGIN: value = &record->class_origin; break;
    case SQL_DIAG_SUBCLASS_ORIGIN: value = &record->subclass_origin; break;
    case SQL_DIAG_CONNECTION_NAME: value = &record->connection_name; break;
    case SQL_DIAG_SERVER_NAME: value = &record->server_name; break;
    default: return SQL_ERROR;
  }
  return write_wide_bytes_output(
      obj, *value, static_cast<SQLWCHAR*>(diag_info_ptr), buffer_length,
      string_length_ptr, "Diagnostic field was truncated", false);
}

SQLRETURN SQLError(SQLHENV environment_handle, SQLHDBC connection_handle, SQLHSTMT statement_handle,
                  SQLCHAR* sqlstate, SQLINTEGER* native_error, SQLCHAR* message_text,
                  SQLSMALLINT buffer_length, SQLSMALLINT* text_length) {
  // ODBC 2.x compatibility function - check handles in order of precedence
  SQLHANDLE handle = nullptr;
  SQLSMALLINT handle_type;
  
  if (statement_handle) {
    handle = statement_handle;
    handle_type = SQL_HANDLE_STMT;
  } else if (connection_handle) {
    handle = connection_handle;
    handle_type = SQL_HANDLE_DBC;
  } else if (environment_handle) {
    handle = environment_handle;
    handle_type = SQL_HANDLE_ENV;
  } else {
    return SQL_INVALID_HANDLE;
  }
  
  // Use SQLGetDiagRec to get the first error record
  SQLRETURN result = SQLGetDiagRec(handle_type, handle, 1, sqlstate, native_error, 
                                  message_text, buffer_length, text_length);
  
  // SQLError should clear the diagnostic after retrieving it (ODBC 2.x behavior)
  if (result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO) {
    auto* obj = HandleRegistry::instance().get_handle(handle);
    if (obj) {
      obj->clear_diagnostics();
    }
  }
  
  return result;
}

SQLRETURN SQLErrorW(
    SQLHENV environment_handle, SQLHDBC connection_handle,
    SQLHSTMT statement_handle, SQLWCHAR* sqlstate,
    SQLINTEGER* native_error, SQLWCHAR* message_text,
    SQLSMALLINT buffer_length, SQLSMALLINT* text_length) {
  SQLHANDLE handle = nullptr;
  SQLSMALLINT handle_type = 0;
  if (statement_handle) {
    handle = statement_handle;
    handle_type = SQL_HANDLE_STMT;
  } else if (connection_handle) {
    handle = connection_handle;
    handle_type = SQL_HANDLE_DBC;
  } else if (environment_handle) {
    handle = environment_handle;
    handle_type = SQL_HANDLE_ENV;
  } else {
    return SQL_INVALID_HANDLE;
  }

  const auto result = SQLGetDiagRecW(
      handle_type, handle, 1, sqlstate, native_error, message_text,
      buffer_length, text_length);
  if (result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO) {
    auto* obj = HandleRegistry::instance().get_handle(handle);
    if (obj) obj->clear_diagnostics();
  }
  return result;
}

SQLRETURN SQLGetInfo(SQLHDBC connection_handle, SQLUSMALLINT info_type, 
                    void* info_value, SQLSMALLINT buffer_length, SQLSMALLINT* string_length) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;

  if (const auto value = string_info_value(info_type)) {
    return write_narrow_output(
        conn, *value, static_cast<SQLCHAR*>(info_value), buffer_length,
        string_length, "Driver information was truncated");
  }

  const auto write_usmallint = [&](SQLUSMALLINT value) -> SQLRETURN {
    if (!info_value) {
      conn->set_error(SQLSTATE_INVALID_NULL_POINTER,
                      "Information output pointer is null");
      return SQL_ERROR;
    }
    *static_cast<SQLUSMALLINT*>(info_value) = value;
    if (string_length) {
      *string_length = static_cast<SQLSMALLINT>(sizeof(value));
    }
    return SQL_SUCCESS;
  };
  const auto write_uinteger = [&](SQLUINTEGER value) -> SQLRETURN {
    if (!info_value) {
      conn->set_error(SQLSTATE_INVALID_NULL_POINTER,
                      "Information output pointer is null");
      return SQL_ERROR;
    }
    *static_cast<SQLUINTEGER*>(info_value) = value;
    if (string_length) {
      *string_length = static_cast<SQLSMALLINT>(sizeof(value));
    }
    return SQL_SUCCESS;
  };
  
  switch (info_type) {
    case SQL_TXN_CAPABLE:
      return write_usmallint(static_cast<SQLUSMALLINT>(SQL_TC_ALL));
    case SQL_CURSOR_COMMIT_BEHAVIOR:
    case SQL_CURSOR_ROLLBACK_BEHAVIOR:
      return write_usmallint(static_cast<SQLUSMALLINT>(SQL_CB_PRESERVE));
    case SQL_DEFAULT_TXN_ISOLATION:
      return write_uinteger(
          static_cast<SQLUINTEGER>(SQL_TXN_READ_COMMITTED));
    case SQL_TXN_ISOLATION_OPTION:
      return write_uinteger(static_cast<SQLUINTEGER>(
          SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED |
          SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE));
    case SQL_MAX_IDENTIFIER_LEN:
    case SQL_MAX_COLUMN_NAME_LEN:
    case SQL_MAX_TABLE_NAME_LEN:
    case SQL_MAX_SCHEMA_NAME_LEN:
    case SQL_MAX_CATALOG_NAME_LEN:
    case SQL_MAX_PROCEDURE_NAME_LEN:
    case SQL_MAX_USER_NAME_LEN:
      return write_usmallint(63);
    case SQL_IDENTIFIER_CASE:
      return write_usmallint(static_cast<SQLUSMALLINT>(SQL_IC_LOWER));
    case SQL_QUOTED_IDENTIFIER_CASE:
      return write_usmallint(static_cast<SQLUSMALLINT>(SQL_IC_SENSITIVE));
    case SQL_CATALOG_LOCATION:
      return write_usmallint(static_cast<SQLUSMALLINT>(SQL_CL_START));
    case SQL_NULL_COLLATION:
      return write_usmallint(static_cast<SQLUSMALLINT>(SQL_NC_HIGH));
    case SQL_CONCAT_NULL_BEHAVIOR:
      return write_usmallint(static_cast<SQLUSMALLINT>(SQL_CB_NULL));
    case SQL_NON_NULLABLE_COLUMNS:
      return write_usmallint(static_cast<SQLUSMALLINT>(SQL_NNC_NON_NULL));
    case SQL_SCROLL_OPTIONS:
      return write_uinteger(static_cast<SQLUINTEGER>(SQL_SO_FORWARD_ONLY));
    case SQL_GETDATA_EXTENSIONS:
      return write_uinteger(static_cast<SQLUINTEGER>(
          SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER));
    case SQL_CATALOG_USAGE:
      return write_uinteger(0);
    case SQL_SCHEMA_USAGE:
      return write_uinteger(static_cast<SQLUINTEGER>(
          SQL_SU_DML_STATEMENTS | SQL_SU_PROCEDURE_INVOCATION |
          SQL_SU_TABLE_DEFINITION | SQL_SU_INDEX_DEFINITION |
          SQL_SU_PRIVILEGE_DEFINITION));
    default:
      conn->set_error(SQLSTATE_GENERAL_ERROR, "Unsupported SQLGetInfo type");
      return SQL_ERROR;
  }
}

SQLRETURN SQLGetInfoW(SQLHDBC connection_handle, SQLUSMALLINT info_type,
                      void* info_value, SQLSMALLINT buffer_length,
                      SQLSMALLINT* string_length) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  if (buffer_length < 0) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid information buffer length");
    return SQL_ERROR;
  }
  if (const auto value = string_info_value(info_type)) {
    return write_wide_bytes_output(
        conn, *value, static_cast<SQLWCHAR*>(info_value),
        buffer_length, string_length, "Driver information was truncated");
  }
  return SQLGetInfo(connection_handle, info_type, info_value, buffer_length,
                    string_length);
}

SQLRETURN SQLGetFunctions(SQLHDBC connection_handle, SQLUSMALLINT function_id,
                          SQLUSMALLINT* supported) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  if (!supported) {
    conn->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "Function support output pointer is null");
    return SQL_ERROR;
  }

  if (function_id == SQL_API_ODBC3_ALL_FUNCTIONS) {
    std::fill_n(supported, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE,
                static_cast<SQLUSMALLINT>(0));
    for (SQLUSMALLINT id = 0; id < 4000; ++id) {
      if (!is_supported_function(id)) continue;
      supported[id >> 4] |= static_cast<SQLUSMALLINT>(
          1u << (id & 0x000f));
    }
    return SQL_SUCCESS;
  }
  if (function_id == SQL_API_ALL_FUNCTIONS) {
    constexpr std::size_t odbc2_function_count = 100;
    std::fill_n(supported, odbc2_function_count,
                static_cast<SQLUSMALLINT>(SQL_FALSE));
    for (SQLUSMALLINT id = 0; id < odbc2_function_count; ++id) {
      supported[id] = static_cast<SQLUSMALLINT>(
          is_supported_function(id) ? SQL_TRUE : SQL_FALSE);
    }
    return SQL_SUCCESS;
  }

  *supported = static_cast<SQLUSMALLINT>(
      is_supported_function(function_id) ? SQL_TRUE : SQL_FALSE);
  return SQL_SUCCESS;
}

SQLRETURN SQLNativeSql(
    SQLHDBC connection_handle, SQLCHAR* input_statement,
    SQLINTEGER text_length1, SQLCHAR* output_statement,
    SQLINTEGER buffer_length, SQLINTEGER* text_length2) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  if (!input_statement) {
    conn->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "Input SQL statement is null");
    return SQL_ERROR;
  }
  if (text_length1 < 0 && text_length1 != SQL_NTS) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid input SQL statement length");
    return SQL_ERROR;
  }
  if (output_statement && buffer_length < 0) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid output SQL buffer length");
    return SQL_ERROR;
  }
  if (!conn->is_connected()) {
    conn->set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
    return SQL_ERROR;
  }

  const auto native_sql = normalize_string(
      sqlchar_to_string(input_statement, text_length1));
  if (text_length2) {
    *text_length2 = static_cast<SQLINTEGER>(std::min(
        native_sql.size(),
        static_cast<std::size_t>(std::numeric_limits<SQLINTEGER>::max())));
  }
  if (!output_statement || buffer_length <= 0) return SQL_SUCCESS;

  const auto copied_length = std::min(
      native_sql.size(), static_cast<std::size_t>(buffer_length - 1));
  std::memcpy(output_statement, native_sql.data(), copied_length);
  output_statement[copied_length] = '\0';
  if (copied_length < native_sql.size()) {
    conn->set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                    "Output SQL statement was truncated");
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

SQLRETURN SQLNativeSqlW(
    SQLHDBC connection_handle, SQLWCHAR* input_statement,
    SQLINTEGER text_length1, SQLWCHAR* output_statement,
    SQLINTEGER buffer_length, SQLINTEGER* text_length2) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  if (!input_statement) {
    conn->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "Input SQL statement is null");
    return SQL_ERROR;
  }
  if (text_length1 < 0 && text_length1 != SQL_NTS) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid input SQL statement length");
    return SQL_ERROR;
  }
  if (output_statement && buffer_length < 0) {
    conn->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid output SQL buffer length");
    return SQL_ERROR;
  }
  if (!conn->is_connected()) {
    conn->set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
    return SQL_ERROR;
  }

  const auto native_sql = sqlwchar_to_utf8(input_statement, text_length1);
  if (!native_sql) {
    conn->set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid wide-character SQL statement");
    return SQL_ERROR;
  }
  return write_wide_output(
      conn, normalize_string(*native_sql), output_statement, buffer_length,
      text_length2, "Output SQL statement was truncated");
}

SQLRETURN SQLSetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute, 
                       void* value, SQLINTEGER string_length) {
  auto* env = get_valid_handle<ODBCEnvironment>(environment_handle);
  if (!env) return SQL_INVALID_HANDLE;
  
  switch (attribute) {
    case SQL_ATTR_ODBC_VERSION: {
      const auto version = static_cast<SQLINTEGER>(
          reinterpret_cast<std::uintptr_t>(value));
      if (version != SQL_OV_ODBC2 && version != SQL_OV_ODBC3
#ifdef SQL_OV_ODBC3_80
          && version != SQL_OV_ODBC3_80
#endif
      ) {
        env->set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                       "Unsupported ODBC version");
        return SQL_ERROR;
      }
      env->set_odbc_version(version);
      return SQL_SUCCESS;
    }
    default:
      env->set_error(SQLSTATE_INVALID_ATTRIBUTE,
                     "Unsupported environment attribute");
      return SQL_ERROR;
  }
}

SQLRETURN SQLGetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute,
                        SQLPOINTER value, SQLINTEGER,
                        SQLINTEGER* string_length) {
  auto* env = get_valid_handle<ODBCEnvironment>(environment_handle);
  if (!env) return SQL_INVALID_HANDLE;
  if (!value) {
    env->set_error(SQLSTATE_INVALID_NULL_POINTER,
                   "Environment attribute output pointer is null");
    return SQL_ERROR;
  }
  if (attribute == IODBC_ATTR_DRIVER_UNICODE_TYPE) {
    *static_cast<SQLINTEGER*>(value) =
        static_cast<SQLINTEGER>(NATIVE_SQLWCHAR_ENCODING);
    if (string_length) {
      *string_length = static_cast<SQLINTEGER>(sizeof(SQLINTEGER));
    }
    return SQL_SUCCESS;
  }
  if (attribute != SQL_ATTR_ODBC_VERSION) {
    env->set_error(SQLSTATE_INVALID_ATTRIBUTE,
                   "Unsupported environment attribute");
    return SQL_ERROR;
  }
  *static_cast<SQLINTEGER*>(value) = env->get_odbc_version();
  if (string_length) {
    *string_length = static_cast<SQLINTEGER>(sizeof(SQLINTEGER));
  }
  return SQL_SUCCESS;
}

SQLRETURN SQLNumResultCols(SQLHSTMT statement_handle, SQLSMALLINT* column_count) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->get_num_result_cols(column_count);
}

SQLRETURN SQLRowCount(SQLHSTMT statement_handle, SQLLEN* row_count) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  return stmt->row_count(row_count);
}

SQLRETURN SQLGetTypeInfo(SQLHSTMT statement_handle, SQLSMALLINT data_type) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  return stmt->get_type_info(data_type);
}

SQLRETURN SQLGetTypeInfoW(SQLHSTMT statement_handle, SQLSMALLINT data_type) {
  return SQLGetTypeInfo(statement_handle, data_type);
}

SQLRETURN SQLColumns(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* table_name,
    SQLSMALLINT name_length3, SQLCHAR* column_name,
    SQLSMALLINT name_length4) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  const auto read_argument = [&](SQLCHAR* value, SQLSMALLINT length,
                                 std::optional<std::string>& output) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) return false;
    output = normalize_string(sqlchar_to_string(value, length));
    return true;
  };
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  std::optional<std::string> column;
  if (!read_argument(catalog_name, name_length1, catalog) ||
      !read_argument(schema_name, name_length2, schema) ||
      !read_argument(table_name, name_length3, table) ||
      !read_argument(column_name, name_length4, column)) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQLColumns argument length");
    return SQL_ERROR;
  }
  return stmt->columns(catalog, schema, table, column);
}

SQLRETURN SQLPrimaryKeys(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* table_name,
    SQLSMALLINT name_length3) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  const auto read_argument = [&](SQLCHAR* value, SQLSMALLINT length,
                                 std::optional<std::string>& output) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) return false;
    output = normalize_string(sqlchar_to_string(value, length));
    return true;
  };
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  if (!read_argument(catalog_name, name_length1, catalog) ||
      !read_argument(schema_name, name_length2, schema) ||
      !read_argument(table_name, name_length3, table)) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQLPrimaryKeys argument length");
    return SQL_ERROR;
  }
  if (!table) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "SQLPrimaryKeys requires a table name");
    return SQL_ERROR;
  }
  return stmt->primary_keys(catalog, schema, *table);
}

SQLRETURN SQLForeignKeys(
    SQLHSTMT statement_handle, SQLCHAR* pk_catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* pk_schema_name,
    SQLSMALLINT name_length2, SQLCHAR* pk_table_name,
    SQLSMALLINT name_length3, SQLCHAR* fk_catalog_name,
    SQLSMALLINT name_length4, SQLCHAR* fk_schema_name,
    SQLSMALLINT name_length5, SQLCHAR* fk_table_name,
    SQLSMALLINT name_length6) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  const auto read_argument = [&](SQLCHAR* value, SQLSMALLINT length,
                                 std::optional<std::string>& output) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) return false;
    output = normalize_string(sqlchar_to_string(value, length));
    return true;
  };
  std::optional<std::string> pk_catalog;
  std::optional<std::string> pk_schema;
  std::optional<std::string> pk_table;
  std::optional<std::string> fk_catalog;
  std::optional<std::string> fk_schema;
  std::optional<std::string> fk_table;
  if (!read_argument(pk_catalog_name, name_length1, pk_catalog) ||
      !read_argument(pk_schema_name, name_length2, pk_schema) ||
      !read_argument(pk_table_name, name_length3, pk_table) ||
      !read_argument(fk_catalog_name, name_length4, fk_catalog) ||
      !read_argument(fk_schema_name, name_length5, fk_schema) ||
      !read_argument(fk_table_name, name_length6, fk_table)) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQLForeignKeys argument length");
    return SQL_ERROR;
  }
  if (!pk_table && !fk_table) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "SQLForeignKeys requires a primary or foreign table");
    return SQL_ERROR;
  }
  return stmt->foreign_keys(pk_catalog, pk_schema, pk_table, fk_catalog,
                            fk_schema, fk_table);
}

SQLRETURN SQLStatistics(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* table_name,
    SQLSMALLINT name_length3, SQLUSMALLINT unique,
    SQLUSMALLINT reserved) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  const auto read_argument = [&](SQLCHAR* value, SQLSMALLINT length,
                                 std::optional<std::string>& output) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) return false;
    output = normalize_string(sqlchar_to_string(value, length));
    return true;
  };
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  if (!read_argument(catalog_name, name_length1, catalog) ||
      !read_argument(schema_name, name_length2, schema) ||
      !read_argument(table_name, name_length3, table)) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQLStatistics argument length");
    return SQL_ERROR;
  }
  if (!table) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "SQLStatistics requires a table name");
    return SQL_ERROR;
  }
  if (unique != SQL_INDEX_UNIQUE && unique != SQL_INDEX_ALL) {
    stmt->set_error(SQLSTATE_INVALID_OPTION_VALUE,
                    "Invalid SQLStatistics uniqueness option");
    return SQL_ERROR;
  }
  if (reserved != SQL_QUICK && reserved != SQL_ENSURE) {
    stmt->set_error(SQLSTATE_INVALID_OPTION_VALUE,
                    "Invalid SQLStatistics accuracy option");
    return SQL_ERROR;
  }
  return stmt->statistics(catalog, schema, *table,
                          unique == SQL_INDEX_UNIQUE);
}

SQLRETURN SQLProcedures(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* procedure_name,
    SQLSMALLINT name_length3) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  const auto read_argument = [&](SQLCHAR* value, SQLSMALLINT length,
                                 std::optional<std::string>& output) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) return false;
    output = normalize_string(sqlchar_to_string(value, length));
    return true;
  };
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> procedure;
  if (!read_argument(catalog_name, name_length1, catalog) ||
      !read_argument(schema_name, name_length2, schema) ||
      !read_argument(procedure_name, name_length3, procedure)) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQLProcedures argument length");
    return SQL_ERROR;
  }
  return stmt->procedures(catalog, schema, procedure);
}

SQLRETURN SQLProcedureColumns(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* procedure_name,
    SQLSMALLINT name_length3, SQLCHAR* column_name,
    SQLSMALLINT name_length4) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  const auto read_argument = [&](SQLCHAR* value, SQLSMALLINT length,
                                 std::optional<std::string>& output) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) return false;
    output = normalize_string(sqlchar_to_string(value, length));
    return true;
  };
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> procedure;
  std::optional<std::string> column;
  if (!read_argument(catalog_name, name_length1, catalog) ||
      !read_argument(schema_name, name_length2, schema) ||
      !read_argument(procedure_name, name_length3, procedure) ||
      !read_argument(column_name, name_length4, column)) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQLProcedureColumns argument length");
    return SQL_ERROR;
  }
  return stmt->procedure_columns(catalog, schema, procedure, column);
}

SQLRETURN SQLSpecialColumns(
    SQLHSTMT statement_handle, SQLUSMALLINT identifier_type,
    SQLCHAR* catalog_name, SQLSMALLINT name_length1,
    SQLCHAR* schema_name, SQLSMALLINT name_length2,
    SQLCHAR* table_name, SQLSMALLINT name_length3,
    SQLUSMALLINT scope, SQLUSMALLINT nullable) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  const auto read_argument = [&](SQLCHAR* value, SQLSMALLINT length,
                                 std::optional<std::string>& output) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) return false;
    output = normalize_string(sqlchar_to_string(value, length));
    return true;
  };
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  if (!read_argument(catalog_name, name_length1, catalog) ||
      !read_argument(schema_name, name_length2, schema) ||
      !read_argument(table_name, name_length3, table)) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQLSpecialColumns argument length");
    return SQL_ERROR;
  }
  if (!table) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "SQLSpecialColumns requires a table name");
    return SQL_ERROR;
  }
  if (identifier_type != SQL_BEST_ROWID && identifier_type != SQL_ROWVER) {
    stmt->set_error(SQLSTATE_COLUMN_TYPE_OUT_OF_RANGE,
                    "Invalid SQLSpecialColumns identifier type");
    return SQL_ERROR;
  }
  if (scope != SQL_SCOPE_CURROW && scope != SQL_SCOPE_TRANSACTION &&
      scope != SQL_SCOPE_SESSION) {
    stmt->set_error(SQLSTATE_SCOPE_OUT_OF_RANGE,
                    "Invalid SQLSpecialColumns scope");
    return SQL_ERROR;
  }
  if (nullable != SQL_NO_NULLS && nullable != SQL_NULLABLE) {
    stmt->set_error(SQLSTATE_NULLABLE_TYPE_OUT_OF_RANGE,
                    "Invalid SQLSpecialColumns nullable option");
    return SQL_ERROR;
  }
  return stmt->special_columns(identifier_type, catalog, schema, *table,
                               nullable == SQL_NO_NULLS);
}

SQLRETURN SQLTables(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* table_name,
    SQLSMALLINT name_length3, SQLCHAR* table_type,
    SQLSMALLINT name_length4) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;

  const auto read_argument = [&](SQLCHAR* value, SQLSMALLINT length,
                                 std::optional<std::string>& output) {
    if (!value) {
      output.reset();
      return true;
    }
    if (length < 0 && length != SQL_NTS) return false;
    output = normalize_string(sqlchar_to_string(value, length));
    return true;
  };
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  std::optional<std::string> type;
  if (!read_argument(catalog_name, name_length1, catalog) ||
      !read_argument(schema_name, name_length2, schema) ||
      !read_argument(table_name, name_length3, table) ||
      !read_argument(table_type, name_length4, type)) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQLTables argument length");
    return SQL_ERROR;
  }
  return stmt->tables(catalog, schema, table, type);
}

SQLRETURN SQLColumnsW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* table_name,
    SQLSMALLINT name_length3, SQLWCHAR* column_name,
    SQLSMALLINT name_length4) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  std::optional<std::string> column;
  if (!read_wide_argument(stmt, catalog_name, name_length1, catalog,
                          "SQLColumnsW") ||
      !read_wide_argument(stmt, schema_name, name_length2, schema,
                          "SQLColumnsW") ||
      !read_wide_argument(stmt, table_name, name_length3, table,
                          "SQLColumnsW") ||
      !read_wide_argument(stmt, column_name, name_length4, column,
                          "SQLColumnsW")) {
    return SQL_ERROR;
  }
  return stmt->columns(catalog, schema, table, column);
}

SQLRETURN SQLPrimaryKeysW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* table_name,
    SQLSMALLINT name_length3) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  if (!read_wide_argument(stmt, catalog_name, name_length1, catalog,
                          "SQLPrimaryKeysW") ||
      !read_wide_argument(stmt, schema_name, name_length2, schema,
                          "SQLPrimaryKeysW") ||
      !read_wide_argument(stmt, table_name, name_length3, table,
                          "SQLPrimaryKeysW")) {
    return SQL_ERROR;
  }
  if (!table) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "SQLPrimaryKeysW requires a table name");
    return SQL_ERROR;
  }
  return stmt->primary_keys(catalog, schema, *table);
}

SQLRETURN SQLForeignKeysW(
    SQLHSTMT statement_handle, SQLWCHAR* pk_catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* pk_schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* pk_table_name,
    SQLSMALLINT name_length3, SQLWCHAR* fk_catalog_name,
    SQLSMALLINT name_length4, SQLWCHAR* fk_schema_name,
    SQLSMALLINT name_length5, SQLWCHAR* fk_table_name,
    SQLSMALLINT name_length6) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  std::optional<std::string> pk_catalog;
  std::optional<std::string> pk_schema;
  std::optional<std::string> pk_table;
  std::optional<std::string> fk_catalog;
  std::optional<std::string> fk_schema;
  std::optional<std::string> fk_table;
  if (!read_wide_argument(stmt, pk_catalog_name, name_length1, pk_catalog,
                          "SQLForeignKeysW") ||
      !read_wide_argument(stmt, pk_schema_name, name_length2, pk_schema,
                          "SQLForeignKeysW") ||
      !read_wide_argument(stmt, pk_table_name, name_length3, pk_table,
                          "SQLForeignKeysW") ||
      !read_wide_argument(stmt, fk_catalog_name, name_length4, fk_catalog,
                          "SQLForeignKeysW") ||
      !read_wide_argument(stmt, fk_schema_name, name_length5, fk_schema,
                          "SQLForeignKeysW") ||
      !read_wide_argument(stmt, fk_table_name, name_length6, fk_table,
                          "SQLForeignKeysW")) {
    return SQL_ERROR;
  }
  if (!pk_table && !fk_table) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "SQLForeignKeysW requires a primary or foreign table");
    return SQL_ERROR;
  }
  return stmt->foreign_keys(pk_catalog, pk_schema, pk_table, fk_catalog,
                            fk_schema, fk_table);
}

SQLRETURN SQLStatisticsW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* table_name,
    SQLSMALLINT name_length3, SQLUSMALLINT unique,
    SQLUSMALLINT reserved) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  if (!read_wide_argument(stmt, catalog_name, name_length1, catalog,
                          "SQLStatisticsW") ||
      !read_wide_argument(stmt, schema_name, name_length2, schema,
                          "SQLStatisticsW") ||
      !read_wide_argument(stmt, table_name, name_length3, table,
                          "SQLStatisticsW")) {
    return SQL_ERROR;
  }
  if (!table) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "SQLStatisticsW requires a table name");
    return SQL_ERROR;
  }
  if (unique != SQL_INDEX_UNIQUE && unique != SQL_INDEX_ALL) {
    stmt->set_error(SQLSTATE_INVALID_OPTION_VALUE,
                    "Invalid SQLStatisticsW uniqueness option");
    return SQL_ERROR;
  }
  if (reserved != SQL_QUICK && reserved != SQL_ENSURE) {
    stmt->set_error(SQLSTATE_INVALID_OPTION_VALUE,
                    "Invalid SQLStatisticsW accuracy option");
    return SQL_ERROR;
  }
  return stmt->statistics(catalog, schema, *table,
                          unique == SQL_INDEX_UNIQUE);
}

SQLRETURN SQLProceduresW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* procedure_name,
    SQLSMALLINT name_length3) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> procedure;
  if (!read_wide_argument(stmt, catalog_name, name_length1, catalog,
                          "SQLProceduresW") ||
      !read_wide_argument(stmt, schema_name, name_length2, schema,
                          "SQLProceduresW") ||
      !read_wide_argument(stmt, procedure_name, name_length3, procedure,
                          "SQLProceduresW")) {
    return SQL_ERROR;
  }
  return stmt->procedures(catalog, schema, procedure);
}

SQLRETURN SQLProcedureColumnsW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* procedure_name,
    SQLSMALLINT name_length3, SQLWCHAR* column_name,
    SQLSMALLINT name_length4) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> procedure;
  std::optional<std::string> column;
  if (!read_wide_argument(stmt, catalog_name, name_length1, catalog,
                          "SQLProcedureColumnsW") ||
      !read_wide_argument(stmt, schema_name, name_length2, schema,
                          "SQLProcedureColumnsW") ||
      !read_wide_argument(stmt, procedure_name, name_length3, procedure,
                          "SQLProcedureColumnsW") ||
      !read_wide_argument(stmt, column_name, name_length4, column,
                          "SQLProcedureColumnsW")) {
    return SQL_ERROR;
  }
  return stmt->procedure_columns(catalog, schema, procedure, column);
}

SQLRETURN SQLSpecialColumnsW(
    SQLHSTMT statement_handle, SQLUSMALLINT identifier_type,
    SQLWCHAR* catalog_name, SQLSMALLINT name_length1,
    SQLWCHAR* schema_name, SQLSMALLINT name_length2,
    SQLWCHAR* table_name, SQLSMALLINT name_length3,
    SQLUSMALLINT scope, SQLUSMALLINT nullable) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  if (!read_wide_argument(stmt, catalog_name, name_length1, catalog,
                          "SQLSpecialColumnsW") ||
      !read_wide_argument(stmt, schema_name, name_length2, schema,
                          "SQLSpecialColumnsW") ||
      !read_wide_argument(stmt, table_name, name_length3, table,
                          "SQLSpecialColumnsW")) {
    return SQL_ERROR;
  }
  if (!table) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER,
                    "SQLSpecialColumnsW requires a table name");
    return SQL_ERROR;
  }
  if (identifier_type != SQL_BEST_ROWID && identifier_type != SQL_ROWVER) {
    stmt->set_error(SQLSTATE_COLUMN_TYPE_OUT_OF_RANGE,
                    "Invalid SQLSpecialColumnsW identifier type");
    return SQL_ERROR;
  }
  if (scope != SQL_SCOPE_CURROW && scope != SQL_SCOPE_TRANSACTION &&
      scope != SQL_SCOPE_SESSION) {
    stmt->set_error(SQLSTATE_SCOPE_OUT_OF_RANGE,
                    "Invalid SQLSpecialColumnsW scope");
    return SQL_ERROR;
  }
  if (nullable != SQL_NO_NULLS && nullable != SQL_NULLABLE) {
    stmt->set_error(SQLSTATE_NULLABLE_TYPE_OUT_OF_RANGE,
                    "Invalid SQLSpecialColumnsW nullable option");
    return SQL_ERROR;
  }
  return stmt->special_columns(identifier_type, catalog, schema, *table,
                               nullable == SQL_NO_NULLS);
}

SQLRETURN SQLTablesW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* table_name,
    SQLSMALLINT name_length3, SQLWCHAR* table_type,
    SQLSMALLINT name_length4) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  std::optional<std::string> catalog;
  std::optional<std::string> schema;
  std::optional<std::string> table;
  std::optional<std::string> type;
  if (!read_wide_argument(stmt, catalog_name, name_length1, catalog,
                          "SQLTablesW") ||
      !read_wide_argument(stmt, schema_name, name_length2, schema,
                          "SQLTablesW") ||
      !read_wide_argument(stmt, table_name, name_length3, table,
                          "SQLTablesW") ||
      !read_wide_argument(stmt, table_type, name_length4, type,
                          "SQLTablesW")) {
    return SQL_ERROR;
  }
  return stmt->tables(catalog, schema, table, type);
}

SQLRETURN SQLDescribeCol(SQLHSTMT statement_handle, SQLUSMALLINT column_number,
                        SQLCHAR* column_name, SQLSMALLINT name_buffer_length, SQLSMALLINT* name_length,
                        SQLSMALLINT* data_type, SQLULEN* column_size, SQLSMALLINT* decimal_digits,
                        SQLSMALLINT* nullable) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->describe_col(column_number, column_name, name_buffer_length, name_length,
                           data_type, column_size, decimal_digits, nullable);
}

SQLRETURN SQLDescribeColW(
    SQLHSTMT statement_handle, SQLUSMALLINT column_number,
    SQLWCHAR* column_name, SQLSMALLINT name_buffer_length,
    SQLSMALLINT* name_length, SQLSMALLINT* data_type,
    SQLULEN* column_size, SQLSMALLINT* decimal_digits,
    SQLSMALLINT* nullable) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  if (name_buffer_length < 0) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid column-name buffer length");
    return SQL_ERROR;
  }

  SQLSMALLINT utf8_length = 0;
  const auto metadata_result = stmt->describe_col(
      column_number, nullptr, 0, &utf8_length, data_type, column_size,
      decimal_digits, nullable);
  if (metadata_result != SQL_SUCCESS) return metadata_result;
  std::vector<SQLCHAR> utf8(static_cast<std::size_t>(utf8_length) + 1);
  const auto name_result = stmt->describe_col(
      column_number, utf8.data(), static_cast<SQLSMALLINT>(utf8.size()),
      nullptr, nullptr, nullptr, nullptr, nullptr);
  if (name_result != SQL_SUCCESS) return name_result;
  return write_wide_output(
      stmt,
      std::string_view(reinterpret_cast<const char*>(utf8.data()),
                       static_cast<std::size_t>(utf8_length)),
      column_name, name_buffer_length, name_length,
      "Column name was truncated");
}

SQLRETURN SQLColAttribute(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                         SQLPOINTER character_attribute, SQLSMALLINT buffer_length, SQLSMALLINT* string_length,
                         SQLLEN* numeric_attribute) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->col_attribute(column_number, field_identifier, character_attribute,
                            buffer_length, string_length, numeric_attribute);
}

SQLRETURN SQLColAttributeW(
    SQLHSTMT statement_handle, SQLUSMALLINT column_number,
    SQLUSMALLINT field_identifier, SQLPOINTER character_attribute,
    SQLSMALLINT buffer_length, SQLSMALLINT* string_length,
    SQLLEN* numeric_attribute) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  if (buffer_length < 0) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid column-attribute buffer length");
    return SQL_ERROR;
  }
  if (field_identifier != SQL_DESC_NAME) {
    return stmt->col_attribute(column_number, field_identifier,
                               character_attribute, buffer_length,
                               string_length, numeric_attribute);
  }

  SQLSMALLINT utf8_length = 0;
  const auto length_result = stmt->col_attribute(
      column_number, field_identifier, nullptr, 0, &utf8_length,
      numeric_attribute);
  if (length_result != SQL_SUCCESS) return length_result;
  std::vector<char> utf8(static_cast<std::size_t>(utf8_length) + 1);
  const auto value_result = stmt->col_attribute(
      column_number, field_identifier, utf8.data(),
      static_cast<SQLSMALLINT>(utf8.size()), nullptr, nullptr);
  if (value_result != SQL_SUCCESS) return value_result;
  return write_wide_bytes_output(
      stmt, std::string_view(utf8.data(), static_cast<std::size_t>(utf8_length)),
      static_cast<SQLWCHAR*>(character_attribute), buffer_length,
      string_length, "Column attribute was truncated");
}

SQLRETURN SQLPrepare(SQLHSTMT statement_handle, SQLCHAR* statement_text, SQLINTEGER text_length) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  std::string sql = sqlchar_to_string(statement_text, text_length);
  return stmt->prepare(sql);
}

SQLRETURN SQLPrepareW(SQLHSTMT statement_handle,
                      SQLWCHAR* statement_text, SQLINTEGER text_length) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  if (!statement_text) {
    stmt->set_error(SQLSTATE_INVALID_NULL_POINTER, "SQL statement is null");
    return SQL_ERROR;
  }
  if (text_length < 0 && text_length != SQL_NTS) {
    stmt->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid SQL statement length");
    return SQL_ERROR;
  }

  const auto sql = sqlwchar_to_utf8(statement_text, text_length);
  if (!sql) {
    stmt->set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid wide-character SQL statement");
    return SQL_ERROR;
  }
  return stmt->prepare(normalize_string(*sql));
}

SQLRETURN SQLExecute(SQLHSTMT statement_handle) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->execute();
}

SQLRETURN SQLNumParams(SQLHSTMT statement_handle,
                       SQLSMALLINT* parameter_count) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  return stmt->num_params(parameter_count);
}

SQLRETURN SQLBindParameter(SQLHSTMT statement_handle, SQLUSMALLINT parameter_number, SQLSMALLINT input_output_type,
                          SQLSMALLINT value_type, SQLSMALLINT parameter_type, SQLULEN column_size,
                          SQLSMALLINT decimal_digits, SQLPOINTER parameter_value, SQLLEN buffer_length,
                          SQLLEN* strlen_or_indicator) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->bind_parameter(parameter_number, input_output_type, value_type, parameter_type,
                             column_size, decimal_digits, parameter_value, buffer_length, strlen_or_indicator);
}

// Column binding
SQLRETURN SQLBindCol(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLSMALLINT target_type,
                    SQLPOINTER target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->bind_col(column_number, target_type, target_value, buffer_length, strlen_or_indicator);
}

// Parameter metadata
SQLRETURN SQLDescribeParam(SQLHSTMT statement_handle, SQLUSMALLINT parameter_number, SQLSMALLINT* data_type,
                          SQLULEN* parameter_size, SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->describe_param(parameter_number, data_type, parameter_size, decimal_digits, nullable);
}

SQLRETURN SQLGetDescField(
    SQLHDESC descriptor_handle, SQLSMALLINT record_number,
    SQLSMALLINT field_identifier, SQLPOINTER value,
    SQLINTEGER buffer_length, SQLINTEGER* string_length) {
  auto* descriptor = get_valid_handle<ODBCDescriptor>(descriptor_handle);
  if (!descriptor) return SQL_INVALID_HANDLE;
  return descriptor->get_field(record_number, field_identifier, value,
                               buffer_length, string_length);
}

SQLRETURN SQLGetDescFieldW(
    SQLHDESC descriptor_handle, SQLSMALLINT record_number,
    SQLSMALLINT field_identifier, SQLPOINTER value,
    SQLINTEGER buffer_length, SQLINTEGER* string_length) {
  auto* descriptor = get_valid_handle<ODBCDescriptor>(descriptor_handle);
  if (!descriptor) return SQL_INVALID_HANDLE;
  if (field_identifier != SQL_DESC_NAME) {
    return descriptor->get_field(record_number, field_identifier, value,
                                 buffer_length, string_length);
  }

  SQLINTEGER utf8_length = 0;
  auto result = descriptor->get_field(
      record_number, field_identifier, nullptr, 0, &utf8_length);
  if (result != SQL_SUCCESS) return result;
  std::vector<char> utf8(static_cast<std::size_t>(utf8_length) + 1);
  result = descriptor->get_field(
      record_number, field_identifier, utf8.data(),
      static_cast<SQLINTEGER>(utf8.size()), nullptr);
  if (result != SQL_SUCCESS) return result;
  return write_wide_bytes_output(
      descriptor,
      std::string_view(utf8.data(), static_cast<std::size_t>(utf8_length)),
      static_cast<SQLWCHAR*>(value), buffer_length, string_length,
      "Descriptor name was truncated");
}

SQLRETURN SQLSetDescField(
    SQLHDESC descriptor_handle, SQLSMALLINT record_number,
    SQLSMALLINT field_identifier, SQLPOINTER value,
    SQLINTEGER buffer_length) {
  auto* descriptor = get_valid_handle<ODBCDescriptor>(descriptor_handle);
  if (!descriptor) return SQL_INVALID_HANDLE;
  return descriptor->set_field(record_number, field_identifier, value,
                               buffer_length);
}

SQLRETURN SQLSetDescFieldW(
    SQLHDESC descriptor_handle, SQLSMALLINT record_number,
    SQLSMALLINT field_identifier, SQLPOINTER value,
    SQLINTEGER buffer_length) {
  auto* descriptor = get_valid_handle<ODBCDescriptor>(descriptor_handle);
  if (!descriptor) return SQL_INVALID_HANDLE;
  if (field_identifier != SQL_DESC_NAME) {
    return descriptor->set_field(record_number, field_identifier, value,
                                 buffer_length);
  }
  if (!value) {
    descriptor->set_error(SQLSTATE_INVALID_NULL_POINTER,
                          "Descriptor name pointer is null");
    return SQL_ERROR;
  }
  if (buffer_length < 0 && buffer_length != SQL_NTS) {
    descriptor->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                          "Invalid wide descriptor name length");
    return SQL_ERROR;
  }
  if (buffer_length != SQL_NTS &&
      buffer_length % static_cast<SQLINTEGER>(sizeof(SQLWCHAR)) != 0) {
    descriptor->set_error(SQLSTATE_INVALID_STRING_LENGTH,
                          "Wide descriptor name length is not aligned");
    return SQL_ERROR;
  }
  const auto units = buffer_length == SQL_NTS
      ? static_cast<SQLINTEGER>(SQL_NTS)
      : buffer_length / static_cast<SQLINTEGER>(sizeof(SQLWCHAR));
  const auto utf8 = sqlwchar_to_utf8(static_cast<SQLWCHAR*>(value), units);
  if (!utf8) {
    descriptor->set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                          "Invalid wide descriptor name");
    return SQL_ERROR;
  }
  return descriptor->set_field(
      record_number, field_identifier,
      const_cast<char*>(utf8->data()), static_cast<SQLINTEGER>(utf8->size()));
}

SQLRETURN SQLCopyDesc(SQLHDESC source_desc_handle,
                      SQLHDESC target_desc_handle) {
  auto* source = get_valid_handle<ODBCDescriptor>(source_desc_handle);
  if (!source) return SQL_INVALID_HANDLE;
  auto* target = get_valid_handle<ODBCDescriptor>(target_desc_handle);
  if (!target) return SQL_INVALID_HANDLE;
  target->copy_from(*source);
  return SQL_SUCCESS;
}

} // extern "C"
