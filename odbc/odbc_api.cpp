#include "odbc_api.h"
#include "odbc_handles.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>

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

  bool is_supported_function(SQLUSMALLINT function_id) {
    switch (function_id) {
      case SQL_API_SQLALLOCHANDLE:
      case SQL_API_SQLBINDCOL:
      case SQL_API_SQLBINDPARAMETER:
      case SQL_API_SQLCOLATTRIBUTE:
      case SQL_API_SQLCLOSECURSOR:
      case SQL_API_SQLCONNECT:
      case SQL_API_SQLDESCRIBECOL:
      case SQL_API_SQLDESCRIBEPARAM:
      case SQL_API_SQLDISCONNECT:
      case SQL_API_SQLDRIVERCONNECT:
      case SQL_API_SQLENDTRAN:
      case SQL_API_SQLERROR:
      case SQL_API_SQLEXECDIRECT:
      case SQL_API_SQLEXECUTE:
      case SQL_API_SQLFETCH:
      case SQL_API_SQLFREEHANDLE:
      case SQL_API_SQLFREESTMT:
      case SQL_API_SQLGETCONNECTATTR:
      case SQL_API_SQLGETDATA:
      case SQL_API_SQLGETDIAGFIELD:
      case SQL_API_SQLGETDIAGREC:
      case SQL_API_SQLGETFUNCTIONS:
      case SQL_API_SQLGETINFO:
      case SQL_API_SQLGETSTMTATTR:
      case SQL_API_SQLGETTYPEINFO:
      case SQL_API_SQLNUMRESULTCOLS:
      case SQL_API_SQLNUMPARAMS:
      case SQL_API_SQLPREPARE:
      case SQL_API_SQLROWCOUNT:
      case SQL_API_SQLSETCONNECTATTR:
      case SQL_API_SQLSETENVATTR:
      case SQL_API_SQLSETSTMTATTR:
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

SQLRETURN SQLFetch(SQLHSTMT statement_handle) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->fetch();
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

SQLRETURN SQLGetInfo(SQLHDBC connection_handle, SQLUSMALLINT info_type, 
                    void* info_value, SQLSMALLINT buffer_length, SQLSMALLINT* string_length) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;

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
    case SQL_DRIVER_NAME:
      if (info_value && buffer_length > 0) {
        const char* name = "ODBCPP Driver";
        size_t len = std::min(static_cast<size_t>(buffer_length - 1), std::strlen(name));
        std::memcpy(info_value, name, len);
        static_cast<char*>(info_value)[len] = '\0';
        if (string_length) *string_length = static_cast<SQLSMALLINT>(std::strlen(name));
      }
      return SQL_SUCCESS;
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
    default:
      conn->set_error(SQLSTATE_GENERAL_ERROR, "Unsupported SQLGetInfo type");
      return SQL_ERROR;
  }
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

SQLRETURN SQLSetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute, 
                       void* value, SQLINTEGER string_length) {
  auto* env = get_valid_handle<ODBCEnvironment>(environment_handle);
  if (!env) return SQL_INVALID_HANDLE;
  
  switch (attribute) {
    case 200: // SQL_ATTR_ODBC_VERSION
      env->set_odbc_version(static_cast<SQLINTEGER>(reinterpret_cast<uintptr_t>(value)));
      return SQL_SUCCESS;
    default:
      env->set_error(SQLSTATE_GENERAL_ERROR, "Unsupported environment attribute");
      return SQL_ERROR;
  }
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

SQLRETURN SQLDescribeCol(SQLHSTMT statement_handle, SQLUSMALLINT column_number,
                        SQLCHAR* column_name, SQLSMALLINT name_buffer_length, SQLSMALLINT* name_length,
                        SQLSMALLINT* data_type, SQLULEN* column_size, SQLSMALLINT* decimal_digits,
                        SQLSMALLINT* nullable) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->describe_col(column_number, column_name, name_buffer_length, name_length,
                           data_type, column_size, decimal_digits, nullable);
}

SQLRETURN SQLColAttribute(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                         SQLPOINTER character_attribute, SQLSMALLINT buffer_length, SQLSMALLINT* string_length,
                         SQLLEN* numeric_attribute) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  return stmt->col_attribute(column_number, field_identifier, character_attribute,
                            buffer_length, string_length, numeric_attribute);
}

SQLRETURN SQLPrepare(SQLHSTMT statement_handle, SQLCHAR* statement_text, SQLINTEGER text_length) {
  auto* stmt = get_valid_handle<ODBCStatement>(statement_handle);
  if (!stmt) return SQL_INVALID_HANDLE;
  
  std::string sql = sqlchar_to_string(statement_text, text_length);
  return stmt->prepare(sql);
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

// Note: SQLGetDescField and SQLSetDescField implementations would go here
// Currently using system declarations from sql.h

} // extern "C"
