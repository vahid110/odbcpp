#pragma once
#include "odbc_types.h"
#include "core/database/async_database_connection.h"
#include "core/database/connection_pool.h"
#include "core/util/result.h"
#include <memory>
#include <string>
#include <vector>
#include <map>

namespace rs::odbc {

// Base ODBC handle
class ODBCHandle {
public:
  explicit ODBCHandle(HandleType type) : type_(type) {}
  virtual ~ODBCHandle() = default;
  
  HandleType get_type() const { return type_; }
  
  void set_error(const std::string& sqlstate, const std::string& message) {
    sqlstate_ = sqlstate;
    error_message_ = message;
  }
  
  const std::string& get_sqlstate() const { return sqlstate_; }
  const std::string& get_error_message() const { return error_message_; }

private:
  HandleType type_;
  std::string sqlstate_ = SQLSTATE_SUCCESS;
  std::string error_message_;
};

// Environment handle
class ODBCEnvironment : public ODBCHandle {
public:
  ODBCEnvironment() : ODBCHandle(HandleType::Environment) {}
  
  void set_odbc_version(SQLINTEGER version) { odbc_version_ = version; }
  SQLINTEGER get_odbc_version() const { return odbc_version_; }

private:
  SQLINTEGER odbc_version_ = 3; // ODBC 3.x by default
};

// Connection handle
class ODBCConnection : public ODBCHandle {
public:
  explicit ODBCConnection(ODBCEnvironment* env) 
    : ODBCHandle(HandleType::Connection), env_(env) {}
  
  SQLRETURN connect(const std::string& dsn, const std::string& user, const std::string& password);
  SQLRETURN disconnect();
  bool is_connected() const { return connected_; }
  
  rs::core::database::AsyncDatabaseConnection* get_db_connection() { return db_conn_.get(); }

private:
  ODBCEnvironment* env_; // Reserved for environment-specific settings
  std::unique_ptr<rs::core::database::AsyncDatabaseConnection> db_conn_;
  bool connected_ = false;
  
  // Suppress unused warning - env_ will be used for ODBC compliance features
  void suppress_unused_warning() { (void)env_; }
};

// IRD (Implementation Row Descriptor) - ODBC result column metadata
struct ColumnInfo {
  std::string name;           // SQL_DESC_NAME
  SQLSMALLINT sql_type;       // SQL_DESC_TYPE
  SQLULEN column_size;        // SQL_DESC_LENGTH
  SQLSMALLINT decimal_digits; // SQL_DESC_PRECISION
  SQLSMALLINT nullable;       // SQL_DESC_NULLABLE
};

// APD (Application Parameter Descriptor) - ODBC parameter binding info
struct ParameterInfo {
  SQLSMALLINT input_output_type;  // SQL_PARAM_INPUT, etc.
  SQLSMALLINT value_type;         // SQL_C_CHAR, SQL_C_LONG, etc.
  SQLSMALLINT parameter_type;     // SQL_VARCHAR, SQL_INTEGER, etc.
  SQLULEN column_size;
  SQLSMALLINT decimal_digits;
  SQLPOINTER parameter_value;     // Application buffer
  SQLLEN buffer_length;
  SQLLEN* strlen_or_indicator;
};

// Statement handle
class ODBCStatement : public ODBCHandle {
public:
  explicit ODBCStatement(ODBCConnection* conn) 
    : ODBCHandle(HandleType::Statement), conn_(conn) {}
  
  SQLRETURN execute_direct(const std::string& sql);
  SQLRETURN fetch();
  SQLRETURN get_data(SQLUSMALLINT col, SQLSMALLINT target_type, 
                     void* buffer, SQLLEN buffer_length, SQLLEN* indicator);
  
  // Prepared statements
  SQLRETURN prepare(const std::string& sql);
  SQLRETURN execute();
  SQLRETURN bind_parameter(SQLUSMALLINT parameter_number, SQLSMALLINT input_output_type,
                          SQLSMALLINT value_type, SQLSMALLINT parameter_type, SQLULEN column_size,
                          SQLSMALLINT decimal_digits, SQLPOINTER parameter_value, SQLLEN buffer_length,
                          SQLLEN* strlen_or_indicator);
  
  // Metadata functions
  SQLRETURN get_num_result_cols(SQLSMALLINT* column_count);
  SQLRETURN describe_col(SQLUSMALLINT column_number, SQLCHAR* column_name, SQLSMALLINT name_buffer_length,
                        SQLSMALLINT* name_length, SQLSMALLINT* data_type, SQLULEN* column_size,
                        SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable);
  SQLRETURN col_attribute(SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                         SQLPOINTER character_attribute, SQLSMALLINT buffer_length,
                         SQLSMALLINT* string_length, SQLLEN* numeric_attribute);
  
  bool has_results() const { return !result_rows_.empty(); }
  size_t get_column_count() const { return result_rows_.empty() ? 0 : result_rows_[0].size(); }

private:
  ODBCConnection* conn_;
  std::vector<std::vector<std::string>> result_rows_;
  std::vector<ColumnInfo> column_info_;  // IRD storage
  std::vector<ParameterInfo> parameter_info_;  // APD storage
  std::string prepared_sql_;
  size_t current_row_ = 0;
  bool executed_ = false;
  bool prepared_ = false;
};

// Handle registry for validation
class HandleRegistry {
public:
  static HandleRegistry& instance();
  
  void register_handle(SQLHANDLE handle, std::unique_ptr<ODBCHandle> obj);
  void unregister_handle(SQLHANDLE handle);
  ODBCHandle* get_handle(SQLHANDLE handle);
  
  template<typename T>
  T* get_handle_as(SQLHANDLE handle) {
    auto* base = get_handle(handle);
    return base ? dynamic_cast<T*>(base) : nullptr;
  }

private:
  std::map<SQLHANDLE, std::unique_ptr<ODBCHandle>> handles_;
  std::mutex mutex_;
};

} // namespace rs::odbc