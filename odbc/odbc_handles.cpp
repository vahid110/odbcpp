#include "odbc_handles.h"
#include "connection_string.h"
#include "result_types.h"
#include "text_data_converter.h"
#include "core/database/generic_database_connection.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/transport_factory.h"
#include "core/transport/transport_options.h"
#include "core/util/deadline.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

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

rs::core::database::QueryParameterType parameter_type_for(
    const ParameterInfo& parameter) {
  using rs::core::database::QueryParameterType;
  switch (parameter.parameter_type) {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
      return QueryParameterType::Text;
    case SQL_TINYINT:
    case SQL_SMALLINT:
    case SQL_INTEGER:
      return QueryParameterType::Int32;
    case SQL_BIGINT:
      return QueryParameterType::Int64;
    case SQL_REAL:
    case SQL_FLOAT:
    case SQL_DOUBLE:
      return QueryParameterType::Float64;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
      return QueryParameterType::Numeric;
    case SQL_BIT:
      return QueryParameterType::Boolean;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
      return QueryParameterType::Binary;
    default:
      break;
  }

  switch (parameter.value_type) {
    case SQL_C_SLONG: return QueryParameterType::Int32;
    case SQL_C_SBIGINT: return QueryParameterType::Int64;
    case SQL_C_DOUBLE: return QueryParameterType::Float64;
    case SQL_C_BIT: return QueryParameterType::Boolean;
    case SQL_C_CHAR: return QueryParameterType::Text;
    default: return QueryParameterType::Unspecified;
  }
}

struct OdbcTypeInfo {
  SQLSMALLINT sql_type{SQL_VARCHAR};
  SQLULEN column_size{255};
  SQLSMALLINT decimal_digits{0};
};

OdbcTypeInfo postgres_type_info(std::uint32_t oid, std::int16_t type_size,
                                std::int32_t type_modifier) {
  switch (oid) {
    case 16: return {SQL_BIT, 1, 0};
    case 17: return {SQL_VARBINARY, type_size > 0 ? static_cast<SQLULEN>(type_size) : 0, 0};
    case 18: return {SQL_CHAR, 1, 0};
    case 19: return {SQL_VARCHAR, 63, 0};
    case 20: return {SQL_BIGINT, 19, 0};
    case 21: return {SQL_SMALLINT, 5, 0};
    case 23: return {SQL_INTEGER, 10, 0};
    case 25: return {SQL_VARCHAR, 0, 0};
    case 26: return {SQL_BIGINT, 10, 0};
    case 700: return {SQL_REAL, 7, 6};
    case 701: return {SQL_DOUBLE, 15, 15};
    case 1042:
    case 1043: {
      const auto length = type_modifier >= 4
          ? static_cast<SQLULEN>(type_modifier - 4) : 0;
      return {static_cast<SQLSMALLINT>(
                  oid == 1042 ? SQL_CHAR : SQL_VARCHAR), length, 0};
    }
    case 1082: return {SQL_TYPE_DATE, 10, 0};
    case 1083:
    case 1266: return {SQL_TYPE_TIME, 15,
                       static_cast<SQLSMALLINT>(type_modifier >= 0 ? type_modifier : 6)};
    case 1114:
    case 1184: return {SQL_TYPE_TIMESTAMP, 29,
                       static_cast<SQLSMALLINT>(type_modifier >= 0 ? type_modifier : 6)};
    case 114:
    case 2950:
    case 3802: return {SQL_VARCHAR, 0, 0};
    case 1700: {
      if (type_modifier < 4) return {SQL_NUMERIC, 0, 0};
      const auto modifier = static_cast<std::uint32_t>(type_modifier - 4);
      const auto precision = static_cast<SQLULEN>((modifier >> 16) & 0xffff);
      const auto scale = static_cast<SQLSMALLINT>(modifier & 0xffff);
      return {SQL_NUMERIC, precision, scale};
    }
    default:
      return {SQL_VARCHAR,
              type_size > 0 ? static_cast<SQLULEN>(type_size) : 0, 0};
  }
}

ColumnInfo column_info_for(
    const rs::core::database::ResultColumnMetadata& metadata) {
  const auto type = postgres_type_info(
      metadata.type_id, metadata.type_size, metadata.type_modifier);
  return ColumnInfo{metadata.name, type.sql_type, type.column_size,
                    type.decimal_digits, SQL_NULLABLE_UNKNOWN};
}

ParameterMetadata parameter_metadata_for(std::uint32_t oid) {
  const auto type = postgres_type_info(oid, -1, -1);
  return ParameterMetadata{type.sql_type, type.column_size,
                           type.decimal_digits, SQL_NULLABLE_UNKNOWN, {}};
}

} // namespace

// Connection implementation
SQLRETURN ODBCConnection::connect(const std::string& dsn, const std::string& user, const std::string& password) {
  try {
    const auto resolved = ConnectionString::resolve(dsn, default_driver_name());
    const auto& params = resolved.effective_parameters;
    if (!resolved.dsn_name.empty() && resolved.dsn_parameters.empty()) {
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
    
    apply_query_result(std::move(*result), false);
    
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
  get_data_offsets_.assign(result_rows_[current_row_ - 1].size(), 0);
  SQLRETURN fetch_result = SQL_SUCCESS;
  
  // Auto-populate bound columns from ARD
  const auto& row = result_rows_[current_row_ - 1];
  for (size_t i = 0; i < column_bindings_.size() && i < row.size(); ++i) {
    const auto& binding = column_bindings_[i];
    if (binding.bound && binding.target_value) {
      const auto& cell = row[i];

      if (!cell) {
        if (!binding.strlen_or_indicator) {
          set_error(SQLSTATE_INDICATOR_VARIABLE_REQUIRED,
                    "NULL column requires an indicator variable");
          return SQL_ERROR;
        }
        *binding.strlen_or_indicator = SQL_NULL_DATA;
        continue;
      }

      const SQLSMALLINT sql_type = i < column_info_.size()
          ? column_info_[i].sql_type : static_cast<SQLSMALLINT>(SQL_VARCHAR);
      const SQLSMALLINT target_type = binding.target_type == SQL_C_DEFAULT
          ? ResultTypes::default_c_type(sql_type) : binding.target_type;
      if (!ResultTypes::is_conversion_supported(sql_type, target_type)) {
        set_error(SQLSTATE_RESTRICTED_DATA_TYPE,
                  "Unsupported result data type conversion");
        return SQL_ERROR;
      }
      SQLRETURN conv_result = TextDataConverter::convert_data(
          *cell, target_type, binding.target_value, binding.buffer_length,
          binding.strlen_or_indicator);

      if (conv_result == SQL_ERROR) {
        set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                  "Result value could not be converted to the requested C type");
        return SQL_ERROR;
      }
      if (conv_result == SQL_SUCCESS_WITH_INFO) {
        set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                  "Result value was truncated to fit the application buffer");
        fetch_result = SQL_SUCCESS_WITH_INFO;
      }
    }
  }

  return fetch_result;
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

  constexpr auto complete = std::numeric_limits<std::size_t>::max();
  if (get_data_offsets_.size() != row.size()) {
    get_data_offsets_.assign(row.size(), 0);
  }
  auto& offset = get_data_offsets_[col - 1];
  if (offset == complete) return SQL_NO_DATA;

  if (!buffer) {
    set_error(SQLSTATE_INVALID_NULL_POINTER, "Null result buffer");
    return SQL_ERROR;
  }
  if (buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH, "Invalid result buffer length");
    return SQL_ERROR;
  }

  const auto& cell = row[col - 1];
  if (!cell) {
    if (!indicator) {
      set_error(SQLSTATE_INDICATOR_VARIABLE_REQUIRED,
                "NULL column requires an indicator variable");
      return SQL_ERROR;
    }
    *indicator = SQL_NULL_DATA;
    offset = complete;
    return SQL_SUCCESS;
  }
  
  const SQLSMALLINT sql_type = col <= column_info_.size()
      ? column_info_[col - 1].sql_type : static_cast<SQLSMALLINT>(SQL_VARCHAR);
  const SQLSMALLINT effective_target_type = target_type == SQL_C_DEFAULT
      ? ResultTypes::default_c_type(sql_type) : target_type;

  if (!ResultTypes::is_conversion_supported(sql_type, effective_target_type)) {
    set_error(SQLSTATE_RESTRICTED_DATA_TYPE,
              "Unsupported result data type conversion");
    return SQL_ERROR;
  }

  if (effective_target_type == SQL_C_CHAR) {
    const auto remaining = cell->size() - offset;
    if (indicator) *indicator = static_cast<SQLLEN>(remaining);
    const auto capacity = buffer_length > 0
        ? static_cast<std::size_t>(buffer_length - 1) : 0;
    const auto copy_length = std::min(capacity, remaining);
    if (copy_length > 0) {
      std::memcpy(buffer, cell->data() + offset, copy_length);
    }
    if (buffer_length > 0) {
      static_cast<char*>(buffer)[copy_length] = '\0';
    }
    offset += copy_length;
    if (offset < cell->size()) {
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Result value was truncated to fit the application buffer");
      return SQL_SUCCESS_WITH_INFO;
    }
    offset = complete;
    return SQL_SUCCESS;
  }

  SQLRETURN result = TextDataConverter::convert_data(
      *cell, effective_target_type, buffer, buffer_length, indicator);

  if (result == SQL_ERROR) {
    set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
              "Result value could not be converted to the requested C type");
  } else if (result == SQL_SUCCESS_WITH_INFO) {
    set_error(SQLSTATE_STRING_DATA_TRUNCATED,
              "Result value was truncated to fit the application buffer");
  }
  if (result != SQL_ERROR) offset = complete;
  
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
  param_metadata_.clear();
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
    std::vector<rs::core::database::QueryParameter> param_values;
    param_values.reserve(parameter_info_.size());
    
    for (const auto& param : parameter_info_) {
      if (!param.bound) {
        set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
                  "Not all statement parameters are bound");
        return SQL_ERROR;
      }
      if (param.input_output_type != SQL_PARAM_INPUT) {
        set_error(SQLSTATE_GENERAL_ERROR,
                  "Only input parameters are currently supported");
        return SQL_ERROR;
      }

      rs::core::database::QueryParameter query_param;
      query_param.type = parameter_type_for(param);
      if (param.strlen_or_indicator &&
          *param.strlen_or_indicator == SQL_NULL_DATA) {
        query_param.value = std::nullopt;
        param_values.push_back(std::move(query_param));
        continue;
      }
      if (!param.parameter_value) {
        set_error(SQLSTATE_GENERAL_ERROR, "Bound parameter has no value buffer");
        return SQL_ERROR;
      }

      std::string value;
      if (param.value_type == SQL_C_CHAR) {
        const auto* text = static_cast<const char*>(param.parameter_value);
        SQLLEN length = param.buffer_length;
        if (param.strlen_or_indicator) length = *param.strlen_or_indicator;
        if (length == SQL_NTS || length == 0) {
          value.assign(text);
        } else if (length >= 0) {
          value.assign(text, static_cast<std::size_t>(length));
        } else {
          set_error(SQLSTATE_GENERAL_ERROR,
                    "Data-at-execution parameters are not supported yet");
          return SQL_ERROR;
        }
      } else if (param.value_type == SQL_C_SLONG) {
        value = std::to_string(*static_cast<SQLINTEGER*>(param.parameter_value));
      } else if (param.value_type == SQL_C_SBIGINT) {
        value = std::to_string(*static_cast<SQLBIGINT*>(param.parameter_value));
      } else if (param.value_type == SQL_C_DOUBLE) {
        value = std::to_string(*static_cast<SQLDOUBLE*>(param.parameter_value));
      } else if (param.value_type == SQL_C_BIT) {
        value = *static_cast<unsigned char*>(param.parameter_value) ? "1" : "0";
      } else {
        set_error(SQLSTATE_GENERAL_ERROR,
                  "Unsupported C parameter type");
        return SQL_ERROR;
      }

      query_param.value = std::move(value);
      param_values.push_back(std::move(query_param));
    }
    
    // Use PostgreSQL Parse/Bind/Execute protocol
    auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
    auto result = conn_->get_db_connection()->execute_prepared(prepared_sql_, param_values, deadline);
    
    if (result.has_error()) {
      set_error(SQLSTATE_SYNTAX_ERROR, result.error_message());
      return SQL_ERROR;
    }
    
    apply_query_result(std::move(*result), true);
    
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
  param.bound = true;

  if (parameter_number > param_metadata_.size()) {
    param_metadata_.resize(parameter_number);
  }
  auto& metadata = param_metadata_[parameter_number - 1];
  metadata.sql_type = parameter_type;
  metadata.column_size = column_size;
  metadata.decimal_digits = decimal_digits;
  metadata.nullable = SQL_NULLABLE_UNKNOWN;
  
  return SQL_SUCCESS;
}

void ODBCStatement::apply_query_result(
    rs::core::database::QueryResult result,
    bool include_parameter_metadata) {
  result_rows_ = std::move(result.rows);
  current_row_ = 0;
  get_data_offsets_.clear();
  executed_ = true;

  const auto max_rows = static_cast<std::size_t>(
      std::numeric_limits<SQLLEN>::max());
  affected_rows_ = result.affected_rows > max_rows
      ? std::numeric_limits<SQLLEN>::max()
      : static_cast<SQLLEN>(result.affected_rows);

  column_info_.clear();
  column_info_.reserve(result.columns.size());
  for (const auto& column : result.columns) {
    column_info_.push_back(column_info_for(column));
  }
  if (column_info_.empty() && !result_rows_.empty()) {
    column_info_.reserve(result_rows_.front().size());
    for (std::size_t i = 0; i < result_rows_.front().size(); ++i) {
      column_info_.push_back(ColumnInfo{
          "column" + std::to_string(i + 1), SQL_VARCHAR, 255, 0,
          SQL_NULLABLE_UNKNOWN});
    }
  }

  if (!include_parameter_metadata) {
    param_metadata_.clear();
    return;
  }
  if (!result.parameter_type_ids.empty()) {
    param_metadata_.clear();
    param_metadata_.reserve(result.parameter_type_ids.size());
    for (const auto oid : result.parameter_type_ids) {
      param_metadata_.push_back(parameter_metadata_for(oid));
    }
  }
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

SQLRETURN ODBCStatement::row_count(SQLLEN* row_count_value) {
  if (!row_count_value) {
    set_error(SQLSTATE_GENERAL_ERROR, "Null pointer for row count");
    return SQL_ERROR;
  }
  if (!executed_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR, "No statement executed");
    return SQL_ERROR;
  }
  *row_count_value = affected_rows_;
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
