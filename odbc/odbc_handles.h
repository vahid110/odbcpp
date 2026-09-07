#pragma once
#include "odbc_types.h"
#include "core/database/i_database_connection.h"
#include "core/database/connection_pool.h"
#include "core/util/result.h"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace rs::odbc {

// Diagnostic record for ODBC error handling
struct DiagnosticRecord {
  std::string sqlstate;
  SQLINTEGER native_error;
  std::string message_text;
  std::string class_origin;
  std::string subclass_origin;
  std::string connection_name;
  std::string server_name;
  
  DiagnosticRecord(const std::string& state = SQLSTATE_SUCCESS, 
                   SQLINTEGER native = 0,
                   const std::string& message = "")
    : sqlstate(state), native_error(native), message_text(message),
      class_origin("ISO 9075"), subclass_origin("ODBCPP 1.0") {}
};

// Base ODBC handle with comprehensive diagnostics
class ODBCHandle {
public:
  explicit ODBCHandle(HandleType type) : type_(type) {}
  virtual ~ODBCHandle() = default;
  
  HandleType get_type() const { return type_; }
  
  // Enhanced error handling with multiple records
  void clear_diagnostics() {
    diagnostic_records_.clear();
  }
  
  void add_diagnostic(const std::string& sqlstate, SQLINTEGER native_error, const std::string& message) {
    diagnostic_records_.emplace_back(sqlstate, native_error, message);
  }
  
  void set_error(const std::string& sqlstate, const std::string& message, SQLINTEGER native_error = 0) {
    clear_diagnostics();
    add_diagnostic(sqlstate, native_error, message);
  }
  
  // Legacy compatibility
  std::string get_sqlstate() const { 
    return diagnostic_records_.empty() ? std::string(SQLSTATE_SUCCESS) : diagnostic_records_[0].sqlstate;
  }
  
  std::string get_error_message() const {
    return diagnostic_records_.empty() ? std::string() : diagnostic_records_[0].message_text;
  }
  
  // New diagnostic access
  size_t get_diagnostic_count() const { return diagnostic_records_.size(); }
  
  const DiagnosticRecord* get_diagnostic_record(SQLSMALLINT record_number) const {
    if (record_number < 1 || record_number > static_cast<SQLSMALLINT>(diagnostic_records_.size())) {
      return nullptr;
    }
    return &diagnostic_records_[record_number - 1];
  }

private:
  HandleType type_;
  std::vector<DiagnosticRecord> diagnostic_records_;
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
  
  rs::core::database::IDatabaseConnection* get_db_connection() { return db_conn_.get(); }

private:
  ODBCEnvironment* env_; // Reserved for environment-specific settings
  std::unique_ptr<rs::core::database::IDatabaseConnection> db_conn_;
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

// ARD (Application Row Descriptor) - ODBC result column binding info
struct ColumnBinding {
  SQLSMALLINT target_type;        // SQL_C_CHAR, SQL_C_LONG, etc.
  SQLPOINTER target_value;        // Application buffer
  SQLLEN buffer_length;
  SQLLEN* strlen_or_indicator;
  bool bound = false;
};

// IPD (Implementation Parameter Descriptor) - ODBC parameter metadata
struct ParameterMetadata {
  SQLSMALLINT sql_type;           // SQL_VARCHAR, SQL_INTEGER, etc.
  SQLULEN column_size;
  SQLSMALLINT decimal_digits;
  SQLSMALLINT nullable;
  std::string name;               // Parameter name (if available)
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
  
  // Column binding (ARD)
  SQLRETURN bind_col(SQLUSMALLINT column_number, SQLSMALLINT target_type,
                     SQLPOINTER target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator);
  
  // Metadata functions
  SQLRETURN get_num_result_cols(SQLSMALLINT* column_count);
  SQLRETURN describe_col(SQLUSMALLINT column_number, SQLCHAR* column_name, SQLSMALLINT name_buffer_length,
                        SQLSMALLINT* name_length, SQLSMALLINT* data_type, SQLULEN* column_size,
                        SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable);
  SQLRETURN col_attribute(SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                         SQLPOINTER character_attribute, SQLSMALLINT buffer_length,
                         SQLSMALLINT* string_length, SQLLEN* numeric_attribute);
  
  // Parameter metadata (IPD)
  SQLRETURN describe_param(SQLUSMALLINT parameter_number, SQLSMALLINT* data_type,
                          SQLULEN* parameter_size, SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable);
  
  // Descriptor field access
  SQLRETURN get_desc_field(SQLSMALLINT descriptor_type, SQLSMALLINT record_number, SQLSMALLINT field_identifier,
                           SQLPOINTER value, SQLLEN buffer_length, SQLLEN* string_length);
  SQLRETURN set_desc_field(SQLSMALLINT descriptor_type, SQLSMALLINT record_number, SQLSMALLINT field_identifier,
                           SQLPOINTER value, SQLLEN string_length);
  
  bool has_results() const { return !result_rows_.empty(); }
  size_t get_column_count() const { return result_rows_.empty() ? 0 : result_rows_[0].size(); }

private:
  ODBCConnection* conn_;
  std::vector<std::vector<std::string>> result_rows_;
  std::vector<ColumnInfo> column_info_;        // IRD storage
  std::vector<ParameterInfo> parameter_info_;  // APD storage
  std::vector<ColumnBinding> column_bindings_; // ARD storage
  std::vector<ParameterMetadata> param_metadata_; // IPD storage
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
