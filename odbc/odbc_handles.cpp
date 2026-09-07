#include "odbc_handles.h"
#include "connection_string.h"
#include "core/database/generic_database_connection.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/transport_factory.h"
#include "core/transport/transport_options.h"
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
namespace {

std::string default_driver_name() {
#ifdef ODBCPP_ENABLE_REDSHIFT
  return "ODBCPP Redshift";
#elif defined(ODBCPP_ENABLE_POSTGRESQL)
  return "ODBCPP PostgreSQL";
#elif defined(ODBCPP_ENABLE_MYSQL)
  return "ODBCPP MySQL";
#elif defined(ODBCPP_ENABLE_SQLSERVER)
  return "ODBCPP SQLServer";
#else
  return "ODBCPP";
#endif
}

bool enabled(const std::string& value) {
  const auto normalized = ConnectionString::to_upper(ConnectionString::trim(value));
  return normalized == "1" || normalized == "TRUE" || normalized == "YES" ||
         normalized == "ON";
}

} // namespace

// Connection implementation
SQLRETURN ODBCConnection::connect(const std::string& dsn, const std::string& user, const std::string& password) {
  try {
    const auto resolved = ConnectionString::resolve(dsn, default_driver_name());
    const auto& params = resolved.effective_parameters;
    if (resolved.connection_parameters.count("DSN") &&
        resolved.dsn_parameters.empty()) {
      set_error(SQLSTATE_CONNECTION_FAILURE,
                "DSN '" + resolved.dsn_name + "' not found");
      return SQL_ERROR;
    }

    rs::core::database::ConnectionSettings settings;
    settings.host = params.count("SERVER") ? params.at("SERVER") :
                    (params.count("HOST") ? params.at("HOST") : "localhost");
    settings.port = params.count("PORT")
        ? static_cast<uint16_t>(std::stoul(params.at("PORT")))
        : 5432;
    settings.database = params.count("DATABASE") ? params.at("DATABASE") :
                        (params.count("DB") ? params.at("DB") :
                         (!resolved.dsn_name.empty() ? resolved.dsn_name : "postgres"));
    settings.user = params.count("UID") ? params.at("UID") :
                    (params.count("USER") ? params.at("USER") : user);
    settings.password = params.count("PWD") ? params.at("PWD") :
                        (params.count("PASSWORD") ? params.at("PASSWORD") : password);
    settings.use_ssl = params.count("SSL") && enabled(params.at("SSL"));
    settings.timeout = std::chrono::seconds(30);

    const auto transport_options = rs::core::transport::TransportOptions::resolve(
        resolved.driver_parameters, resolved.dsn_parameters,
        resolved.connection_parameters);
    auto transport = rs::core::transport::TransportFactory::create(
        transport_options, settings.use_ssl);
    auto parser = std::make_unique<rs::core::database::postgres::PgProtocolParser>();
    db_conn_ = std::make_unique<rs::core::database::GenericDatabaseConnection>(
        std::move(parser), std::move(transport));
    
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
      const std::string& value = row[i];
      
      // Simple NULL detection: if value is empty, treat as NULL for bound columns
      // This is a simplified approach - proper implementation would use protocol-level NULL indicators
      if (value.empty() && binding.strlen_or_indicator) {
        *binding.strlen_or_indicator = SQL_NULL_DATA;
        continue;
      }
      
      // Use database-specific data converter
      SQLRETURN conv_result = db_converter::RedshiftDataConverter::convert_data(
        value, binding.target_type, binding.target_value, binding.buffer_length, binding.strlen_or_indicator);
      
      // If conversion fails, set error indicator but don't fail the entire fetch
      if (conv_result == SQL_ERROR && binding.strlen_or_indicator) {
        *binding.strlen_or_indicator = SQL_NULL_DATA;
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
  
  // Note: NULL detection should be done at protocol level
  // For now, we don't treat empty strings as NULL in get_data
  // NULL handling is done in fetch() for bound columns only
  
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
  
  // Check if column number is valid (after execution)
  if (executed_ && column_number > column_info_.size()) {
    set_error(SQLSTATE_GENERAL_ERROR, "Column number out of range");
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
  if (!column_count) {
    set_error(SQLSTATE_GENERAL_ERROR, "Null pointer for column count");
    return SQL_ERROR;
  }
  
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
