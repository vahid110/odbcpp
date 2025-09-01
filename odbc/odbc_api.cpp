#include "odbc_api.h"
#include "odbc_handles.h"
#include <cstring>

using namespace rs::odbc;

// String conversion helpers (Unicode-ready architecture)
namespace {
  // ANSI string conversion
  std::string sqlchar_to_string(SQLCHAR* str, SQLSMALLINT length) {
    if (!str) return "";
    if (length == SQL_NTS) return reinterpret_cast<char*>(str);
    return std::string(reinterpret_cast<char*>(str), length);
  }
  
  // Internal UTF-8 string handling (ready for Wide function support)
  std::string normalize_string(const std::string& input) {
    // Currently pass-through, but ready for UTF-8 validation/conversion
    return input;
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

SQLRETURN SQLDisconnect(SQLHDBC connection_handle) {
  auto* conn = get_valid_handle<ODBCConnection>(connection_handle);
  if (!conn) return SQL_INVALID_HANDLE;
  
  return conn->disconnect();
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
    std::strncpy(reinterpret_cast<char*>(sqlstate), record->sqlstate.c_str(), 6);
    sqlstate[5] = '\0'; // Ensure null termination
  }
  
  // Set native error code
  if (native_error) {
    *native_error = record->native_error;
  }
  
  // Copy message text with proper truncation handling
  if (message_text && buffer_length > 0) {
    size_t msg_len = record->message_text.length();
    size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), msg_len);
    
    std::strncpy(reinterpret_cast<char*>(message_text), record->message_text.c_str(), copy_len);
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
      std::strncpy(static_cast<char*>(diag_info_ptr), str.c_str(), copy_len);
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
  
  // Return basic driver information
  switch (info_type) {
    case 6: // SQL_DRIVER_NAME
      if (info_value && buffer_length > 0) {
        const char* name = "ODBCPP Driver";
        size_t len = std::min(static_cast<size_t>(buffer_length - 1), std::strlen(name));
        std::strncpy(static_cast<char*>(info_value), name, len);
        static_cast<char*>(info_value)[len] = '\0';
        if (string_length) *string_length = static_cast<SQLSMALLINT>(std::strlen(name));
      }
      return SQL_SUCCESS;
    default:
      conn->set_error(SQLSTATE_GENERAL_ERROR, "Unsupported SQLGetInfo type");
      return SQL_ERROR;
  }
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