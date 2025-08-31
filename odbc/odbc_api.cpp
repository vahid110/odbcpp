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
      default:
        return SQL_ERROR;
    }
    
    // Use the object pointer as the handle
    SQLHANDLE handle = reinterpret_cast<SQLHANDLE>(new_handle.get());
    HandleRegistry::instance().register_handle(handle, std::move(new_handle));
    *output_handle = handle;
    
    return SQL_SUCCESS;
    
  } catch (...) {
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
  if (rec_number != 1) return SQL_NO_DATA; // Only support first error record
  
  auto* obj = HandleRegistry::instance().get_handle(handle);
  if (!obj) return SQL_INVALID_HANDLE;
  
  const std::string& state = obj->get_sqlstate();
  const std::string& message = obj->get_error_message();
  
  if (sqlstate) {
    std::strncpy(reinterpret_cast<char*>(sqlstate), state.c_str(), 6);
  }
  
  if (native_error) {
    *native_error = 0; // No native error codes for now
  }
  
  if (message_text && buffer_length > 0) {
    size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), message.length());
    std::strncpy(reinterpret_cast<char*>(message_text), message.c_str(), copy_len);
    message_text[copy_len] = '\0';
    
    if (text_length) {
      *text_length = static_cast<SQLSMALLINT>(message.length());
    }
  }
  
  return SQL_SUCCESS;
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
      return SQL_ERROR;
  }
}

} // extern "C"