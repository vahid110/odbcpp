#include "odbc_handles.h"
#include "connection_string.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/thread_pool_transport.h"
#include "core/util/deadline.h"
#include <mutex>

// Include database-specific converter based on build target
#ifdef ODBCPP_ENABLE_REDSHIFT
#include "redshift/redshift_data_converter.h"
#include "redshift/redshift_types.h"
namespace db_converter = rs::odbc::redshift;
#elif defined(ODBCPP_ENABLE_POSTGRESQL)
#include "postgresql/postgresql_data_converter.h"
namespace db_converter = rs::odbc::postgresql;
#elif defined(ODBCPP_ENABLE_MYSQL)
#include "mysql/mysql_data_converter.h"
namespace db_converter = rs::odbc::mysql;
#elif defined(ODBCPP_ENABLE_SQLSERVER)
#include "sqlserver/sqlserver_data_converter.h"
namespace db_converter = rs::odbc::sqlserver;
#endif

namespace rs::odbc {

// Connection implementation
SQLRETURN ODBCConnection::connect(const std::string& dsn, const std::string& user, const std::string& password) {
  try {
    // Create async database connection with real PostgreSQL parser
    auto transport = std::make_unique<rs::core::transport::ThreadPoolTransport>(4);
    auto parser = std::make_unique<rs::core::database::postgres::PgProtocolParser>();
    db_conn_ = std::make_unique<rs::core::database::AsyncDatabaseConnection>(std::move(parser), std::move(transport));
    
    rs::core::database::ConnectionSettings settings;
    
    // Check if dsn is a connection string or DSN name
    if (dsn.find('=') != std::string::npos) {
      auto params = ConnectionString::parse(dsn);
      
      // Check if it's DSN=name format
      if (params.count("DSN") && params.size() == 1) {
        // DSN=name format - load from DSN files
        auto dsn_params = ConnectionString::load_dsn(params["DSN"]);
        
        if (dsn_params.empty()) {
          set_error(SQLSTATE_CONNECTION_FAILURE, "DSN '" + params["DSN"] + "' not found");
          return SQL_ERROR;
        }
        
        // Use DSN parameters
        settings.host = dsn_params.count("SERVER") ? dsn_params["SERVER"] : "localhost";
        settings.port = dsn_params.count("PORT") ? std::stoi(dsn_params["PORT"]) : 5432;
        settings.database = dsn_params.count("DATABASE") ? dsn_params["DATABASE"] : params["DSN"];
        settings.user = dsn_params.count("UID") ? dsn_params["UID"] : user;
        settings.password = dsn_params.count("PWD") ? dsn_params["PWD"] : password;
        settings.use_ssl = dsn_params.count("SSL") ? (dsn_params["SSL"] == "1") : false;
        
      } else {
        // Connection string format: "SERVER=host;PORT=5439;DATABASE=dev;UID=user;PWD=pass"
      
      settings.host = params.count("SERVER") ? params["SERVER"] : 
                     (params.count("HOST") ? params["HOST"] : "localhost");
      settings.port = params.count("PORT") ? std::stoi(params["PORT"]) : 5432;
      settings.database = params.count("DATABASE") ? params["DATABASE"] : 
                         (params.count("DB") ? params["DB"] : "postgres");
      settings.user = params.count("UID") ? params["UID"] : 
                     (params.count("USER") ? params["USER"] : user);
        settings.password = params.count("PWD") ? params["PWD"] : 
                           (params.count("PASSWORD") ? params["PASSWORD"] : password);
        settings.use_ssl = params.count("SSL") ? (params["SSL"] == "1" || params["SSL"] == "true") : false;
      }
      
    } else {
      // DSN name - load from DSN files
      auto dsn_params = ConnectionString::load_dsn(dsn);
      
      if (dsn_params.empty()) {
        // Fallback: treat as database name
        settings.host = "localhost";
        settings.port = 5432;
        settings.database = dsn;
        settings.user = user;
        settings.password = password;
        settings.use_ssl = false;
      } else {
        // Use DSN parameters
        settings.host = dsn_params.count("SERVER") ? dsn_params["SERVER"] : "localhost";
        settings.port = dsn_params.count("PORT") ? std::stoi(dsn_params["PORT"]) : 5432;
        settings.database = dsn_params.count("DATABASE") ? dsn_params["DATABASE"] : dsn;
        settings.user = dsn_params.count("UID") ? dsn_params["UID"] : user;
        settings.password = dsn_params.count("PWD") ? dsn_params["PWD"] : password;
        settings.use_ssl = dsn_params.count("SSL") ? (dsn_params["SSL"] == "1") : false;
      }
    }
    
    settings.timeout = std::chrono::seconds(30);
    
    // Connect synchronously for ODBC compatibility
    auto result = db_conn_->connect(settings);
    if (result.has_error()) {
      set_error(SQLSTATE_CONNECTION_FAILURE, result.error_message());
      return SQL_ERROR;
    }
    
    connected_ = true;
    return SQL_SUCCESS;
    
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    return SQL_ERROR;
  }
}

SQLRETURN ODBCConnection::disconnect() {
  if (db_conn_) {
    db_conn_->disconnect();
    connected_ = false;
  }
  return SQL_SUCCESS;
}

// Statement implementation
SQLRETURN ODBCStatement::execute_direct(const std::string& sql) {
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    return SQL_ERROR;
  }
  
  try {
    auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
    auto result = conn_->get_db_connection()->execute_query(sql, deadline);
    
    if (result.has_error()) {
      set_error(SQLSTATE_SYNTAX_ERROR, result.error_message());
      return SQL_ERROR;
    }
    
    result_rows_ = result->rows;
    current_row_ = 0;
    executed_ = true;
    
    // Populate IRD (Implementation Row Descriptor) with column metadata
    // TODO: Extract from PostgreSQL RowDescription message
    column_info_.clear();
    if (!result_rows_.empty()) {
      for (size_t i = 0; i < result_rows_[0].size(); ++i) {
        ColumnInfo col;
        col.name = "column" + std::to_string(i + 1);  // Generic names for now
        col.sql_type = SQL_VARCHAR;  // Assume all strings for now
        col.column_size = 255;       // Default size
        col.decimal_digits = 0;
        col.nullable = SQL_NULLABLE;
        column_info_.push_back(col);
      }
    }
    
    return SQL_SUCCESS;
    
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    return SQL_ERROR;
  }
}

SQLRETURN ODBCStatement::fetch() {
  if (!executed_ || current_row_ >= result_rows_.size()) {
    return SQL_NO_DATA;
  }
  
  current_row_++;
  
  // Auto-populate bound columns from ARD
  const auto& row = result_rows_[current_row_ - 1];
  for (size_t i = 0; i < column_bindings_.size() && i < row.size(); ++i) {
    const auto& binding = column_bindings_[i];
    if (binding.bound && binding.target_value) {
      // Convert and copy data to bound buffer
      const std::string& value = row[i];
      
      // Use existing data converter with VARCHAR as default SQL type
      SQLSMALLINT sql_type = SQL_VARCHAR;
      
      if (db_converter::RedshiftTypes::is_conversion_supported(sql_type, binding.target_type)) {
        SQLRETURN result = db_converter::RedshiftDataConverter::convert_data(
          value, binding.target_type, binding.target_value, binding.buffer_length, binding.strlen_or_indicator);
        
        // If conversion fails, set error indicator
        if (result != SQL_SUCCESS && binding.strlen_or_indicator) {
          *binding.strlen_or_indicator = SQL_NULL_DATA;
        }
      }
    }
  }
  
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::get_data(SQLUSMALLINT col, SQLSMALLINT target_type, 
                                 void* buffer, SQLLEN buffer_length, SQLLEN* indicator) {
  if (!executed_ || current_row_ == 0 || current_row_ > result_rows_.size()) {
    set_error(SQLSTATE_GENERAL_ERROR, "No current row");
    return SQL_ERROR;
  }
  
  const auto& row = result_rows_[current_row_ - 1];
  if (col < 1 || col > row.size()) {
    set_error(SQLSTATE_GENERAL_ERROR, "Invalid column number");
    return SQL_ERROR;
  }
  
  const std::string& value = row[col - 1];
  
  // TODO: Get actual SQL type from column metadata (Milestone 2)
  // For now, assume all data comes as VARCHAR from database
  SQLSMALLINT sql_type = SQL_VARCHAR;
  
  // Validate conversion is supported
  if (!db_converter::RedshiftTypes::is_conversion_supported(sql_type, target_type)) {
    set_error(SQLSTATE_GENERAL_ERROR, "Unsupported data type conversion");
    return SQL_ERROR;
  }
  
  // Use database-specific data converter
  SQLRETURN result = db_converter::RedshiftDataConverter::convert_data(
    value, target_type, buffer, buffer_length, indicator);
  
  if (result == SQL_ERROR) {
    set_error(SQLSTATE_GENERAL_ERROR, "Data type conversion failed");
  }
  
  return result;
}

// Prepared statement implementation
SQLRETURN ODBCStatement::prepare(const std::string& sql) {
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    return SQL_ERROR;
  }
  
  prepared_sql_ = sql;
  parameter_info_.clear();
  prepared_ = true;
  executed_ = false;
  
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::execute() {
  if (!prepared_) {
    set_error(SQLSTATE_GENERAL_ERROR, "Statement not prepared");
    return SQL_ERROR;
  }
  
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    return SQL_ERROR;
  }
  
  try {
    // Convert bound parameters to string format for PostgreSQL
    std::vector<std::string> param_values;
    param_values.reserve(parameter_info_.size());
    
    for (const auto& param : parameter_info_) {
      std::string value;
      
      // Convert parameter value to string based on C type
      if (param.value_type == SQL_C_CHAR) {
        value = std::string(static_cast<char*>(param.parameter_value));
      } else if (param.value_type == SQL_C_SLONG) {
        value = std::to_string(*static_cast<SQLINTEGER*>(param.parameter_value));
      } else if (param.value_type == SQL_C_SBIGINT) {
        value = std::to_string(*static_cast<SQLBIGINT*>(param.parameter_value));
      } else if (param.value_type == SQL_C_DOUBLE) {
        value = std::to_string(*static_cast<SQLDOUBLE*>(param.parameter_value));
      } else {
        value = "";
      }
      
      param_values.push_back(value);
    }
    
    // Use PostgreSQL Parse/Bind/Execute protocol
    auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
    auto result = conn_->get_db_connection()->execute_prepared(prepared_sql_, param_values, deadline);
    
    if (result.has_error()) {
      set_error(SQLSTATE_SYNTAX_ERROR, result.error_message());
      return SQL_ERROR;
    }
    
    result_rows_ = result->rows;
    current_row_ = 0;
    executed_ = true;
    
    // Update IRD with column metadata
    column_info_.clear();
    if (!result_rows_.empty()) {
      for (size_t i = 0; i < result_rows_[0].size(); ++i) {
        ColumnInfo col;
        col.name = "column" + std::to_string(i + 1);
        col.sql_type = SQL_VARCHAR;
        col.column_size = 255;
        col.decimal_digits = 0;
        col.nullable = SQL_NULLABLE;
        column_info_.push_back(col);
      }
    }
    
    return SQL_SUCCESS;
    
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    return SQL_ERROR;
  }
}

SQLRETURN ODBCStatement::bind_parameter(SQLUSMALLINT parameter_number, SQLSMALLINT input_output_type,
                                       SQLSMALLINT value_type, SQLSMALLINT parameter_type, SQLULEN column_size,
                                       SQLSMALLINT decimal_digits, SQLPOINTER parameter_value, SQLLEN buffer_length,
                                       SQLLEN* strlen_or_indicator) {
  if (parameter_number < 1) {
    set_error(SQLSTATE_GENERAL_ERROR, "Invalid parameter number");
    return SQL_ERROR;
  }
  
  // Resize parameter array if needed
  if (parameter_number > parameter_info_.size()) {
    parameter_info_.resize(parameter_number);
  }
  
  // Store parameter info in APD
  auto& param = parameter_info_[parameter_number - 1];
  param.input_output_type = input_output_type;
  param.value_type = value_type;
  param.parameter_type = parameter_type;
  param.column_size = column_size;
  param.decimal_digits = decimal_digits;
  param.parameter_value = parameter_value;
  param.buffer_length = buffer_length;
  param.strlen_or_indicator = strlen_or_indicator;
  
  return SQL_SUCCESS;
}

// Column binding implementation
SQLRETURN ODBCStatement::bind_col(SQLUSMALLINT column_number, SQLSMALLINT target_type,
                                  SQLPOINTER target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator) {
  if (column_number < 1) {
    set_error(SQLSTATE_GENERAL_ERROR, "Invalid column number");
    return SQL_ERROR;
  }
  
  // Resize binding array if needed
  if (column_number > column_bindings_.size()) {
    column_bindings_.resize(column_number);
  }
  
  // Store binding info in ARD
  auto& binding = column_bindings_[column_number - 1];
  binding.target_type = target_type;
  binding.target_value = target_value;
  binding.buffer_length = buffer_length;
  binding.strlen_or_indicator = strlen_or_indicator;
  binding.bound = true;
  
  return SQL_SUCCESS;
}

// Metadata functions implementation
SQLRETURN ODBCStatement::get_num_result_cols(SQLSMALLINT* column_count) {
  if (!column_count) return SQL_ERROR;
  
  if (!executed_) {
    set_error(SQLSTATE_GENERAL_ERROR, "No query executed");
    return SQL_ERROR;
  }
  
  *column_count = static_cast<SQLSMALLINT>(column_info_.size());
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::describe_col(SQLUSMALLINT column_number, SQLCHAR* column_name, SQLSMALLINT name_buffer_length,
                                     SQLSMALLINT* name_length, SQLSMALLINT* data_type, SQLULEN* column_size,
                                     SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable) {
  if (column_number < 1 || column_number > column_info_.size()) {
    set_error(SQLSTATE_GENERAL_ERROR, "Invalid column number");
    return SQL_ERROR;
  }
  
  const auto& col = column_info_[column_number - 1];
  
  // Copy column name
  if (column_name && name_buffer_length > 0) {
    size_t copy_len = std::min(static_cast<size_t>(name_buffer_length - 1), col.name.length());
    std::memcpy(column_name, col.name.c_str(), copy_len);
    column_name[copy_len] = '\0';
  }
  
  if (name_length) *name_length = static_cast<SQLSMALLINT>(col.name.length());
  if (data_type) *data_type = col.sql_type;
  if (column_size) *column_size = col.column_size;
  if (decimal_digits) *decimal_digits = col.decimal_digits;
  if (nullable) *nullable = col.nullable;
  
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::col_attribute(SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                                      SQLPOINTER character_attribute, SQLSMALLINT buffer_length,
                                      SQLSMALLINT* string_length, SQLLEN* numeric_attribute) {
  if (column_number < 1 || column_number > column_info_.size()) {
    set_error(SQLSTATE_GENERAL_ERROR, "Invalid column number");
    return SQL_ERROR;
  }
  
  const auto& col = column_info_[column_number - 1];
  
  switch (field_identifier) {
    case SQL_DESC_NAME:
      if (character_attribute && buffer_length > 0) {
        size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), col.name.length());
        std::memcpy(character_attribute, col.name.c_str(), copy_len);
        static_cast<char*>(character_attribute)[copy_len] = '\0';
      }
      if (string_length) *string_length = static_cast<SQLSMALLINT>(col.name.length());
      return SQL_SUCCESS;
      
    case SQL_DESC_TYPE:
      if (numeric_attribute) *numeric_attribute = col.sql_type;
      return SQL_SUCCESS;
      
    case SQL_DESC_LENGTH:
      if (numeric_attribute) *numeric_attribute = col.column_size;
      return SQL_SUCCESS;
      
    default:
      set_error(SQLSTATE_GENERAL_ERROR, "Unsupported column attribute");
      return SQL_ERROR;
  }
}

// Parameter metadata implementation
SQLRETURN ODBCStatement::describe_param(SQLUSMALLINT parameter_number, SQLSMALLINT* data_type,
                                        SQLULEN* parameter_size, SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable) {
  if (parameter_number < 1 || parameter_number > param_metadata_.size()) {
    set_error(SQLSTATE_GENERAL_ERROR, "Invalid parameter number");
    return SQL_ERROR;
  }
  
  const auto& meta = param_metadata_[parameter_number - 1];
  if (data_type) *data_type = meta.sql_type;
  if (parameter_size) *parameter_size = meta.column_size;
  if (decimal_digits) *decimal_digits = meta.decimal_digits;
  if (nullable) *nullable = meta.nullable;
  
  return SQL_SUCCESS;
}

// Descriptor field access implementation
SQLRETURN ODBCStatement::get_desc_field(SQLSMALLINT descriptor_type, SQLSMALLINT record_number, SQLSMALLINT field_identifier,
                                        SQLPOINTER value, SQLLEN buffer_length, SQLLEN* string_length) {
  // Simplified implementation - would need full descriptor type handling
  set_error(SQLSTATE_GENERAL_ERROR, "SQLGetDescField not fully implemented");
  return SQL_ERROR;
}

SQLRETURN ODBCStatement::set_desc_field(SQLSMALLINT descriptor_type, SQLSMALLINT record_number, SQLSMALLINT field_identifier,
                                        SQLPOINTER value, SQLLEN string_length) {
  // Simplified implementation - would need full descriptor type handling
  set_error(SQLSTATE_GENERAL_ERROR, "SQLSetDescField not fully implemented");
  return SQL_ERROR;
}

// Handle registry implementation
HandleRegistry& HandleRegistry::instance() {
  static HandleRegistry registry;
  return registry;
}

void HandleRegistry::register_handle(SQLHANDLE handle, std::unique_ptr<ODBCHandle> obj) {
  std::lock_guard lock(mutex_);
  handles_[handle] = std::move(obj);
}

void HandleRegistry::unregister_handle(SQLHANDLE handle) {
  std::lock_guard lock(mutex_);
  handles_.erase(handle);
}

ODBCHandle* HandleRegistry::get_handle(SQLHANDLE handle) {
  std::lock_guard lock(mutex_);
  auto it = handles_.find(handle);
  return (it != handles_.end()) ? it->second.get() : nullptr;
}

} // namespace rs::odbc