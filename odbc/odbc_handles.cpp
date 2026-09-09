#include "odbc_handles.h"
#include "connection_string.h"
#include "result_types.h"
#include "text_data_converter.h"
#include "unicode.h"
#include "core/database/generic_database_connection.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/transport_factory.h"
#include "core/transport/transport_options.h"
#include "core/util/deadline.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>

namespace rs::odbc {
namespace {

std::atomic<std::uint64_t> next_connection_id{1};

std::string elapsed_milliseconds(std::chrono::steady_clock::time_point start) {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count());
}

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

std::chrono::milliseconds timeout_duration(SQLULEN seconds) {
  if (seconds == 0) return std::chrono::milliseconds::max();
  constexpr auto maximum = std::chrono::milliseconds::max().count();
  constexpr auto scale = std::chrono::milliseconds::period::den /
      std::chrono::seconds::period::den;
  if (seconds > static_cast<SQLULEN>(maximum / scale)) {
    return std::chrono::milliseconds::max();
  }
  return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(
      seconds * scale));
}

struct DynamicFunction {
  std::string name;
  SQLINTEGER code{SQL_DIAG_UNKNOWN_STATEMENT};
};

DynamicFunction classify_dynamic_function(std::string_view statement) {
  const auto first = statement.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  std::string normalized(statement.substr(first));
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::toupper(character));
                 });
  const auto matches = [&](std::string_view prefix) {
    return normalized.starts_with(prefix) &&
        (normalized.size() == prefix.size() ||
         std::isspace(static_cast<unsigned char>(normalized[prefix.size()])));
  };
  struct PrefixMapping {
    std::string_view prefix;
    std::string_view name;
    SQLINTEGER code;
  };
  static constexpr std::array<PrefixMapping, 27> mappings{{
      {"SELECT", "SELECT CURSOR", SQL_DIAG_SELECT_CURSOR},
      {"INSERT", "INSERT", SQL_DIAG_INSERT},
      {"UPDATE", "UPDATE WHERE", SQL_DIAG_UPDATE_WHERE},
      {"DELETE", "DELETE WHERE", SQL_DIAG_DELETE_WHERE},
      {"CALL", "CALL", SQL_DIAG_CALL},
      {"GRANT", "GRANT", SQL_DIAG_GRANT},
      {"REVOKE", "REVOKE", SQL_DIAG_REVOKE},
      {"ALTER DOMAIN", "ALTER DOMAIN", SQL_DIAG_ALTER_DOMAIN},
      {"ALTER TABLE", "ALTER TABLE", SQL_DIAG_ALTER_TABLE},
      {"CREATE ASSERTION", "CREATE ASSERTION", SQL_DIAG_CREATE_ASSERTION},
      {"CREATE CHARACTER SET", "CREATE CHARACTER SET",
       SQL_DIAG_CREATE_CHARACTER_SET},
      {"CREATE COLLATION", "CREATE COLLATION", SQL_DIAG_CREATE_COLLATION},
      {"CREATE DOMAIN", "CREATE DOMAIN", SQL_DIAG_CREATE_DOMAIN},
      {"CREATE INDEX", "CREATE INDEX", SQL_DIAG_CREATE_INDEX},
      {"CREATE SCHEMA", "CREATE SCHEMA", SQL_DIAG_CREATE_SCHEMA},
      {"CREATE TABLE", "CREATE TABLE", SQL_DIAG_CREATE_TABLE},
      {"CREATE TRANSLATION", "CREATE TRANSLATION",
       SQL_DIAG_CREATE_TRANSLATION},
      {"CREATE VIEW", "CREATE VIEW", SQL_DIAG_CREATE_VIEW},
      {"DROP ASSERTION", "DROP ASSERTION", SQL_DIAG_DROP_ASSERTION},
      {"DROP CHARACTER SET", "DROP CHARACTER SET",
       SQL_DIAG_DROP_CHARACTER_SET},
      {"DROP COLLATION", "DROP COLLATION", SQL_DIAG_DROP_COLLATION},
      {"DROP DOMAIN", "DROP DOMAIN", SQL_DIAG_DROP_DOMAIN},
      {"DROP INDEX", "DROP INDEX", SQL_DIAG_DROP_INDEX},
      {"DROP SCHEMA", "DROP SCHEMA", SQL_DIAG_DROP_SCHEMA},
      {"DROP TABLE", "DROP TABLE", SQL_DIAG_DROP_TABLE},
      {"DROP TRANSLATION", "DROP TRANSLATION",
       SQL_DIAG_DROP_TRANSLATION},
      {"DROP VIEW", "DROP VIEW", SQL_DIAG_DROP_VIEW}}};
  for (const auto& mapping : mappings) {
    if (matches(mapping.prefix)) {
      return {std::string(mapping.name), mapping.code};
    }
  }
  return {};
}

const char* transaction_isolation_name(SQLULEN value) {
  switch (value) {
    case SQL_TXN_READ_UNCOMMITTED: return "READ UNCOMMITTED";
    case SQL_TXN_READ_COMMITTED: return "READ COMMITTED";
    case SQL_TXN_REPEATABLE_READ: return "REPEATABLE READ";
    case SQL_TXN_SERIALIZABLE: return "SERIALIZABLE";
    default: return nullptr;
  }
}

rs::core::database::QueryParameterType parameter_type_for(
    SQLSMALLINT parameter_type, SQLSMALLINT value_type) {
  using rs::core::database::QueryParameterType;
  switch (parameter_type) {
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
    case SQL_WCHAR:
    case SQL_WVARCHAR:
    case SQL_WLONGVARCHAR:
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

  switch (value_type) {
    case SQL_C_SLONG: return QueryParameterType::Int32;
    case SQL_C_SBIGINT: return QueryParameterType::Int64;
    case SQL_C_DOUBLE: return QueryParameterType::Float64;
    case SQL_C_BIT: return QueryParameterType::Boolean;
    case SQL_C_BINARY: return QueryParameterType::Binary;
    case SQL_C_CHAR:
    case SQL_C_WCHAR: return QueryParameterType::Text;
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

DescriptorRecord descriptor_record_for(const ColumnInfo& column) {
  DescriptorRecord record;
  record.type = column.sql_type;
  record.concise_type = column.sql_type;
  record.length = column.column_size;
  record.precision = static_cast<SQLSMALLINT>(std::min<SQLULEN>(
      column.column_size,
      static_cast<SQLULEN>(std::numeric_limits<SQLSMALLINT>::max())));
  record.scale = column.decimal_digits;
  record.nullable = column.nullable;
  record.name = column.name;
  return record;
}

DescriptorRecord descriptor_record_for(const ParameterMetadata& parameter) {
  DescriptorRecord record;
  record.type = parameter.sql_type;
  record.concise_type = parameter.sql_type;
  record.length = parameter.column_size;
  record.precision = static_cast<SQLSMALLINT>(std::min<SQLULEN>(
      parameter.column_size,
      static_cast<SQLULEN>(std::numeric_limits<SQLSMALLINT>::max())));
  record.scale = parameter.decimal_digits;
  record.nullable = parameter.nullable;
  record.parameter_type = SQL_PARAM_INPUT;
  record.name = parameter.name;
  return record;
}

struct TypeInfoDefinition {
  const char* name;
  SQLSMALLINT data_type;
  SQLINTEGER column_size;
  const char* literal_prefix;
  const char* literal_suffix;
  const char* create_params;
  SQLSMALLINT case_sensitive;
  SQLSMALLINT unsigned_attribute;
  SQLSMALLINT minimum_scale;
  SQLSMALLINT maximum_scale;
  SQLSMALLINT sql_data_type;
  SQLSMALLINT datetime_sub;
  SQLINTEGER numeric_radix;
};

const TypeInfoDefinition type_info_definitions[] = {
    {"boolean", SQL_BIT, 1, nullptr, nullptr, nullptr, SQL_FALSE, -1,
     -1, -1, SQL_BIT, 0, 0},
    {"bigint", SQL_BIGINT, 19, nullptr, nullptr, nullptr, SQL_FALSE, SQL_FALSE,
     0, 0, SQL_BIGINT, 0, 10},
    {"bytea", SQL_VARBINARY, 1073741824, "'", "'", nullptr, SQL_FALSE, -1,
     -1, -1, SQL_VARBINARY, 0, 0},
    {"text", SQL_LONGVARCHAR, 1073741824, "'", "'", nullptr, SQL_TRUE, -1,
     -1, -1, SQL_LONGVARCHAR, 0, 0},
    {"char", SQL_CHAR, 10485760, "'", "'", "length", SQL_TRUE, -1,
     -1, -1, SQL_CHAR, 0, 0},
    {"numeric", SQL_NUMERIC, 1000, nullptr, nullptr, "precision,scale",
     SQL_FALSE, SQL_FALSE, 0, 1000, SQL_NUMERIC, 0, 10},
    {"decimal", SQL_DECIMAL, 1000, nullptr, nullptr, "precision,scale",
     SQL_FALSE, SQL_FALSE, 0, 1000, SQL_DECIMAL, 0, 10},
    {"integer", SQL_INTEGER, 10, nullptr, nullptr, nullptr, SQL_FALSE,
     SQL_FALSE, 0, 0, SQL_INTEGER, 0, 10},
    {"smallint", SQL_SMALLINT, 5, nullptr, nullptr, nullptr, SQL_FALSE,
     SQL_FALSE, 0, 0, SQL_SMALLINT, 0, 10},
    {"real", SQL_REAL, 7, nullptr, nullptr, nullptr, SQL_FALSE, SQL_FALSE,
     -1, -1, SQL_REAL, 0, 2},
    {"double precision", SQL_DOUBLE, 15, nullptr, nullptr, nullptr, SQL_FALSE,
     SQL_FALSE, -1, -1, SQL_DOUBLE, 0, 2},
    {"varchar", SQL_VARCHAR, 10485760, "'", "'", "length", SQL_TRUE,
     -1, -1, -1, SQL_VARCHAR, 0, 0},
    {"date", SQL_TYPE_DATE, 10, "'", "'", nullptr, SQL_FALSE, -1,
     -1, -1, SQL_DATETIME, SQL_CODE_DATE, 0},
    {"time", SQL_TYPE_TIME, 15, "'", "'", "precision", SQL_FALSE,
     -1, 0, 6, SQL_DATETIME, SQL_CODE_TIME, 0},
    {"timestamp", SQL_TYPE_TIMESTAMP, 29, "'", "'", "precision", SQL_FALSE,
     -1, 0, 6, SQL_DATETIME, SQL_CODE_TIMESTAMP, 0},
};

rs::core::database::ResultCell type_info_text(const char* value) {
  if (!value) return std::nullopt;
  return std::string(value);
}

rs::core::database::ResultCell type_info_number(long long value) {
  return std::to_string(value);
}

std::string quote_catalog_literal(const std::string& value) {
  std::string quoted{"'"};
  quoted.reserve(value.size() + 2);
  for (const char ch : value) {
    if (ch == '\'') quoted.push_back('\'');
    quoted.push_back(ch);
  }
  quoted.push_back('\'');
  return quoted;
}

std::vector<std::string> parse_table_types(const std::string& value) {
  std::vector<std::string> types;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto comma = value.find(',', start);
    auto type = ConnectionString::trim(value.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start));
    if (type.size() >= 2 && type.front() == '\'' && type.back() == '\'') {
      type = type.substr(1, type.size() - 2);
    }
    type = ConnectionString::to_upper(type);
    if (type == "TABLE" || type == "VIEW" || type == "SYSTEM TABLE" ||
        type == "FOREIGN TABLE") {
      types.push_back(std::move(type));
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return types;
}

} // namespace

ODBCConnection::ODBCConnection(ODBCEnvironment*)
    : ODBCHandle(HandleType::Connection),
      connection_id_(next_connection_id.fetch_add(1)) {}

void ODBCConnection::log(
    rs::core::logging::LogLevel level, std::string_view event,
    std::string_view message,
    std::initializer_list<rs::core::logging::LogField> fields) const noexcept {
  if (logger_) logger_->log(level, event, message, fields);
}

bool ODBCConnection::logs_queries() const noexcept {
  return logger_ && logger_->logs_queries();
}

bool ODBCConnection::logging_enabled(
    rs::core::logging::LogLevel level) const noexcept {
  return logger_ && logger_->enabled(level);
}

ODBCStatement::ODBCStatement(std::shared_ptr<ODBCConnection> conn)
    : ODBCHandle(HandleType::Statement), conn_(std::move(conn)) {
  try {
    automatic_app_row_descriptor_ = create_implicit_descriptor(
        DescriptorKind::Application);
    app_row_descriptor_ = automatic_app_row_descriptor_;
    automatic_app_param_descriptor_ = create_implicit_descriptor(
        DescriptorKind::Application);
    app_param_descriptor_ = automatic_app_param_descriptor_;
    imp_row_descriptor_ = create_implicit_descriptor(
        DescriptorKind::ImplementationRow);
    imp_param_descriptor_ = create_implicit_descriptor(
        DescriptorKind::ImplementationParameter);
  } catch (...) {
    for (const auto descriptor : {
             automatic_app_row_descriptor_, automatic_app_param_descriptor_,
             imp_row_descriptor_, imp_param_descriptor_}) {
      if (descriptor) HandleRegistry::instance().unregister_handle(descriptor);
    }
    throw;
  }
}

ODBCStatement::~ODBCStatement() {
  for (const auto descriptor : {
           automatic_app_row_descriptor_, automatic_app_param_descriptor_,
           imp_row_descriptor_, imp_param_descriptor_}) {
    if (descriptor) HandleRegistry::instance().unregister_handle(descriptor);
  }
}

SQLHDESC ODBCStatement::create_implicit_descriptor(DescriptorKind kind) {
  auto descriptor = std::make_unique<ODBCDescriptor>(conn_.get(), true, kind);
  const auto handle = reinterpret_cast<SQLHDESC>(descriptor.get());
  HandleRegistry::instance().register_handle(
      handle, std::move(descriptor), reinterpret_cast<SQLHANDLE>(this));
  return handle;
}

// Connection implementation
SQLRETURN ODBCConnection::connect(const std::string& dsn, const std::string& user, const std::string& password) {
  const auto started = std::chrono::steady_clock::now();
  if (connected_) {
    set_error(SQLSTATE_CONNECTION_IN_USE, "Connection is already open");
    return SQL_ERROR;
  }
  try {
    const auto resolved = ConnectionString::resolve(dsn, default_driver_name());
    const auto logging_options = rs::core::logging::LoggingOptions::resolve(
        resolved.driver_parameters, resolved.dsn_parameters,
        resolved.connection_parameters);
    logger_ = rs::core::logging::DriverLogger::create(
        logging_options, connection_id_);
    const auto& params = resolved.effective_parameters;
    if (!resolved.dsn_name.empty() && resolved.dsn_parameters.empty()) {
      set_error(SQLSTATE_CONNECTION_FAILURE,
                "DSN '" + resolved.dsn_name + "' not found");
      log(rs::core::logging::LogLevel::Error, "connection_failed",
          get_error_message(), {{"sqlstate", get_sqlstate()},
                                {"duration_ms", elapsed_milliseconds(started)}});
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
    if (requested_catalog_) settings.database = *requested_catalog_;
    settings.user = params.count("UID") ? params.at("UID") :
                    (params.count("USER") ? params.at("USER") : user);
    settings.password = params.count("PWD") ? params.at("PWD") :
                        (params.count("PASSWORD") ? params.at("PASSWORD") : password);
    settings.use_ssl = params.count("SSL") && enabled(params.at("SSL"));
    settings.timeout = timeout_duration(login_timeout_seconds_);

    const auto transport_options = rs::core::transport::TransportOptions::resolve(
        resolved.driver_parameters, resolved.dsn_parameters,
        resolved.connection_parameters);
    log(rs::core::logging::LogLevel::Info, "connection_start",
        "Opening database connection",
        {{"host", settings.host},
         {"port", std::to_string(settings.port)},
         {"database", settings.database},
         {"tls", settings.use_ssl ? "true" : "false"},
         {"transport_mode", std::string(rs::core::transport::to_string(
                                rs::core::transport::TransportFactory::resolve_mode(
                                    transport_options)))},
         {"async_engine", std::string(rs::core::transport::to_string(
                               transport_options.async_engine))},
         {"deadline_model", std::string(rs::core::transport::to_string(
                                 transport_options.deadline_model))}});
    auto transport = rs::core::transport::TransportFactory::create(
        transport_options, settings.use_ssl);
    auto parser = std::make_unique<rs::core::database::postgres::PgProtocolParser>();
    db_conn_ = std::make_unique<rs::core::database::GenericDatabaseConnection>(
        std::move(parser), std::move(transport));
    
    // Connect synchronously for ODBC compatibility
    auto result = db_conn_->connect(settings);
    if (result.has_error()) {
      const auto timeout = result.error() ==
          rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
      set_error(timeout ? SQLSTATE_CONNECTION_TIMEOUT : SQLSTATE_CONNECTION_FAILURE,
                result.error_message());
      log(rs::core::logging::LogLevel::Error, "connection_failed",
          result.error_message(),
          {{"sqlstate", get_sqlstate()},
           {"duration_ms", elapsed_milliseconds(started)}});
      return SQL_ERROR;
    }

    if (transaction_isolation_ != SQL_TXN_READ_COMMITTED) {
      const std::string command =
          "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL " +
          std::string(transaction_isolation_name(transaction_isolation_));
      auto isolation_result = db_conn_->execute_query(
          command, rs::util::make_deadline(settings.timeout));
      if (isolation_result.has_error()) {
        const auto timeout = isolation_result.error() ==
            rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
        set_error(timeout ? SQLSTATE_CONNECTION_TIMEOUT : SQLSTATE_CONNECTION_FAILURE,
                  isolation_result.error_message());
        log(rs::core::logging::LogLevel::Error, "connection_failed",
            isolation_result.error_message(),
            {{"sqlstate", get_sqlstate()},
             {"duration_ms", elapsed_milliseconds(started)}});
        db_conn_->disconnect();
        return SQL_ERROR;
      }
    }
    
    connected_ = true;
    transaction_active_ = false;
    current_catalog_ = settings.database;
    log(rs::core::logging::LogLevel::Info, "connection_opened",
        "Database connection established",
        {{"duration_ms", elapsed_milliseconds(started)}});
    return SQL_SUCCESS;
    
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    log(rs::core::logging::LogLevel::Error, "connection_failed", e.what(),
        {{"sqlstate", get_sqlstate()},
         {"duration_ms", elapsed_milliseconds(started)}});
    return SQL_ERROR;
  }
}

SQLRETURN ODBCConnection::set_attribute(SQLINTEGER attribute, SQLULEN value) {
  if (attribute == IODBC_ATTR_APP_WCHAR_TYPE) {
    if (value != NATIVE_SQLWCHAR_ENCODING) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "The requested SQLWCHAR encoding is not native to this "
                "driver build");
      return SQL_ERROR;
    }
    clear_diagnostics();
    return SQL_SUCCESS;
  }
  if (attribute == SQL_ATTR_AUTOCOMMIT) {
    if (value != SQL_AUTOCOMMIT_ON && value != SQL_AUTOCOMMIT_OFF) {
      set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                "Autocommit must be SQL_AUTOCOMMIT_ON or SQL_AUTOCOMMIT_OFF");
      return SQL_ERROR;
    }
    if (autocommit_ == value) return SQL_SUCCESS;
    if (value == SQL_AUTOCOMMIT_ON && connected_ && transaction_active_) {
      const auto result = end_transaction(SQL_COMMIT);
      if (result != SQL_SUCCESS) return result;
    }
    autocommit_ = static_cast<SQLUINTEGER>(value);
    return SQL_SUCCESS;
  }
  if (attribute == SQL_ATTR_ACCESS_MODE) {
    if (value == SQL_MODE_READ_WRITE) return SQL_SUCCESS;
    if (value == SQL_MODE_READ_ONLY) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Read-only connection mode is not implemented");
      return SQL_ERROR;
    }
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Invalid connection access mode");
    return SQL_ERROR;
  }
  if (attribute == SQL_ATTR_ASYNC_ENABLE) {
    if (value == SQL_ASYNC_ENABLE_OFF) return SQL_SUCCESS;
    if (value == SQL_ASYNC_ENABLE_ON) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Asynchronous ODBC function execution is not implemented");
      return SQL_ERROR;
    }
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Invalid asynchronous execution mode");
    return SQL_ERROR;
  }
  if (attribute == SQL_ATTR_METADATA_ID) {
    if (value == SQL_FALSE) return SQL_SUCCESS;
    if (value == SQL_TRUE) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "Metadata identifier semantics are not implemented");
      return SQL_ERROR;
    }
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Invalid metadata identifier mode");
    return SQL_ERROR;
  }
  if (attribute == SQL_ATTR_TXN_ISOLATION) {
    const auto* isolation_name = transaction_isolation_name(value);
    if (!isolation_name) {
      set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                "Unsupported transaction isolation level");
      return SQL_ERROR;
    }
    if (transaction_active_) {
      set_error(SQLSTATE_ATTRIBUTE_CANNOT_BE_SET,
                "Transaction isolation cannot change during a transaction");
      return SQL_ERROR;
    }
    if (connected_) {
      const std::string command =
          "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL " +
          std::string(isolation_name);
      auto result = db_conn_->execute_query(
          command, rs::util::make_deadline(std::chrono::seconds(30)));
      if (result.has_error()) {
        const auto timeout = result.error() ==
            rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
        set_error(timeout ? SQLSTATE_TIMEOUT : SQLSTATE_GENERAL_ERROR,
                  result.error_message());
        if (timeout) close_connection();
        return SQL_ERROR;
      }
    }
    transaction_isolation_ = static_cast<SQLUINTEGER>(value);
    return SQL_SUCCESS;
  }
  if (attribute != SQL_ATTR_LOGIN_TIMEOUT) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE,
              "Unsupported connection attribute");
    return SQL_ERROR;
  }
  if (connected_) {
    set_error(SQLSTATE_ATTRIBUTE_CANNOT_BE_SET,
              "Login timeout cannot be changed while connected");
    return SQL_ERROR;
  }
  if (value > std::numeric_limits<SQLUINTEGER>::max()) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Login timeout is outside the supported range");
    return SQL_ERROR;
  }
  login_timeout_seconds_ = static_cast<SQLUINTEGER>(value);
  return SQL_SUCCESS;
}

SQLRETURN ODBCConnection::get_attribute(SQLINTEGER attribute,
                                        SQLUINTEGER* value) {
  switch (attribute) {
    case IODBC_ATTR_APP_WCHAR_TYPE:
      *value = NATIVE_SQLWCHAR_ENCODING;
      return SQL_SUCCESS;
    case SQL_ATTR_LOGIN_TIMEOUT:
      *value = login_timeout_seconds_;
      return SQL_SUCCESS;
    case SQL_ATTR_ACCESS_MODE:
      *value = SQL_MODE_READ_WRITE;
      return SQL_SUCCESS;
    case SQL_ATTR_ASYNC_ENABLE:
      *value = SQL_ASYNC_ENABLE_OFF;
      return SQL_SUCCESS;
    case SQL_ATTR_AUTO_IPD:
      *value = SQL_FALSE;
      return SQL_SUCCESS;
    case SQL_ATTR_CONNECTION_DEAD:
      if (!connected_) {
        set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
        return SQL_ERROR;
      }
      *value = SQL_CD_FALSE;
      return SQL_SUCCESS;
    case SQL_ATTR_METADATA_ID:
      *value = SQL_FALSE;
      return SQL_SUCCESS;
    case SQL_ATTR_AUTOCOMMIT:
      *value = autocommit_;
      return SQL_SUCCESS;
    case SQL_ATTR_TXN_ISOLATION:
      *value = transaction_isolation_;
      return SQL_SUCCESS;
    default:
      set_error(SQLSTATE_INVALID_ATTRIBUTE,
                "Unsupported connection attribute");
      return SQL_ERROR;
  }
}

SQLRETURN ODBCConnection::set_current_catalog(std::string catalog) {
  if (catalog.empty()) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Current catalog cannot be empty");
    return SQL_ERROR;
  }
  if (connected_) {
    if (catalog != current_catalog_) {
      set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
                "PostgreSQL cannot change databases on an open connection");
      return SQL_ERROR;
    }
  }
  requested_catalog_ = std::move(catalog);
  return SQL_SUCCESS;
}

std::string ODBCConnection::get_current_catalog() const {
  if (requested_catalog_) return *requested_catalog_;
  return current_catalog_;
}

rs::util::Result<void> ODBCConnection::begin_transaction_if_needed(
    rs::util::Deadline deadline) {
  if (autocommit_ == SQL_AUTOCOMMIT_ON || transaction_active_) {
    return {};
  }
  auto result = db_conn_->execute_query("BEGIN", deadline);
  if (result.has_error()) {
    return {result.error(), result.error_message()};
  }
  transaction_active_ = true;
  return {};
}

SQLRETURN ODBCConnection::end_transaction(SQLSMALLINT completion_type) {
  if (completion_type != SQL_COMMIT && completion_type != SQL_ROLLBACK) {
    set_error(SQLSTATE_INVALID_TRANSACTION_OPERATION,
              "Completion type must be SQL_COMMIT or SQL_ROLLBACK");
    return SQL_ERROR;
  }
  if (!connected_) {
    set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
    return SQL_ERROR;
  }
  if (autocommit_ == SQL_AUTOCOMMIT_ON) return SQL_SUCCESS;
  if (!transaction_active_) return SQL_SUCCESS;

  const auto command = completion_type == SQL_COMMIT ? "COMMIT" : "ROLLBACK";
  auto result = db_conn_->execute_query(
      command, rs::util::make_deadline(std::chrono::seconds(30)));
  if (result.has_error()) {
    const auto timeout = result.error() ==
        rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
    set_error(timeout ? SQLSTATE_TIMEOUT : SQLSTATE_GENERAL_ERROR,
              result.error_message());
    if (timeout) close_connection();
    return SQL_ERROR;
  }
  transaction_active_ = false;
  return SQL_SUCCESS;
}

SQLRETURN ODBCConnection::disconnect() {
  if (!connected_) {
    set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
    return SQL_ERROR;
  }
  if (transaction_active_) {
    set_error(SQLSTATE_INVALID_TRANSACTION_STATE,
              "An active transaction must be committed or rolled back before disconnecting");
    return SQL_ERROR;
  }
  close_connection();
  return SQL_SUCCESS;
}

void ODBCConnection::close_connection() {
  if (db_conn_) {
    db_conn_->disconnect();
    connected_ = false;
    transaction_active_ = false;
  }
  log(rs::core::logging::LogLevel::Info, "connection_closed",
      "Database connection closed");
  if (logger_) logger_->flush();
}

SQLRETURN ODBCDescriptor::get_field(
    SQLSMALLINT record_number, SQLSMALLINT field_identifier,
    SQLPOINTER value, SQLINTEGER buffer_length, SQLINTEGER* string_length) {
  if (buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Invalid descriptor output buffer length");
    return SQL_ERROR;
  }
  const auto require_output = [&]() {
    if (value) return true;
    set_error(SQLSTATE_INVALID_NULL_POINTER,
              "Descriptor output pointer is null");
    return false;
  };
  switch (field_identifier) {
    case SQL_DESC_COUNT:
      if (!require_output()) return SQL_ERROR;
      *static_cast<SQLSMALLINT*>(value) =
          static_cast<SQLSMALLINT>(records_.size());
      return SQL_SUCCESS;
    case SQL_DESC_ARRAY_SIZE:
      if (!require_output()) return SQL_ERROR;
      *static_cast<SQLULEN*>(value) = array_size_;
      return SQL_SUCCESS;
    case SQL_DESC_ARRAY_STATUS_PTR:
      if (!require_output()) return SQL_ERROR;
      *static_cast<SQLUSMALLINT**>(value) = array_status_ptr_;
      return SQL_SUCCESS;
    case SQL_DESC_BIND_OFFSET_PTR:
      if (!require_output()) return SQL_ERROR;
      *static_cast<SQLLEN**>(value) = bind_offset_ptr_;
      return SQL_SUCCESS;
    case SQL_DESC_BIND_TYPE:
      if (!require_output()) return SQL_ERROR;
      *static_cast<SQLULEN*>(value) = bind_type_;
      return SQL_SUCCESS;
    case SQL_DESC_ROWS_PROCESSED_PTR:
      if (!require_output()) return SQL_ERROR;
      *static_cast<SQLULEN**>(value) = rows_processed_ptr_;
      return SQL_SUCCESS;
    default:
      break;
  }

  if (record_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
              "Invalid descriptor record number");
    return SQL_ERROR;
  }
  if (static_cast<std::size_t>(record_number) > records_.size()) {
    return SQL_NO_DATA;
  }
  const auto& record = records_[static_cast<std::size_t>(record_number - 1)];
  if (field_identifier == SQL_DESC_NAME) {
    if (string_length) {
      *string_length = static_cast<SQLINTEGER>(record.name.size());
    }
    if (!value || buffer_length == 0) return SQL_SUCCESS;
    const auto copied = std::min<std::size_t>(
        record.name.size(), static_cast<std::size_t>(buffer_length - 1));
    std::memcpy(value, record.name.data(), copied);
    static_cast<char*>(value)[copied] = 0;
    if (copied < record.name.size()) {
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Descriptor name was truncated");
      return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  }
  if (!require_output()) return SQL_ERROR;
  switch (field_identifier) {
    case SQL_DESC_TYPE:
      *static_cast<SQLSMALLINT*>(value) = record.type; break;
    case SQL_DESC_CONCISE_TYPE:
      *static_cast<SQLSMALLINT*>(value) = record.concise_type; break;
    case SQL_DESC_LENGTH:
      *static_cast<SQLULEN*>(value) = record.length; break;
    case SQL_DESC_PRECISION:
      *static_cast<SQLSMALLINT*>(value) = record.precision; break;
    case SQL_DESC_SCALE:
      *static_cast<SQLSMALLINT*>(value) = record.scale; break;
    case SQL_DESC_NULLABLE:
      *static_cast<SQLSMALLINT*>(value) = record.nullable; break;
    case SQL_DESC_PARAMETER_TYPE:
      *static_cast<SQLSMALLINT*>(value) = record.parameter_type; break;
    case SQL_DESC_DATA_PTR:
      *static_cast<SQLPOINTER*>(value) = record.data_ptr; break;
    case SQL_DESC_INDICATOR_PTR:
      *static_cast<SQLLEN**>(value) = record.indicator_ptr; break;
    case SQL_DESC_OCTET_LENGTH_PTR:
      *static_cast<SQLLEN**>(value) = record.octet_length_ptr; break;
    case SQL_DESC_OCTET_LENGTH:
      *static_cast<SQLLEN*>(value) = record.octet_length; break;
    default:
      set_error(SQLSTATE_INVALID_ATTRIBUTE,
                "Unsupported descriptor field");
      return SQL_ERROR;
  }
  return SQL_SUCCESS;
}

SQLRETURN ODBCDescriptor::set_field(
    SQLSMALLINT record_number, SQLSMALLINT field_identifier,
    SQLPOINTER value, SQLINTEGER buffer_length) {
  if (kind_ == DescriptorKind::ImplementationRow &&
      field_identifier != SQL_DESC_ARRAY_STATUS_PTR &&
      field_identifier != SQL_DESC_ROWS_PROCESSED_PTR) {
    set_error(SQLSTATE_CANNOT_MODIFY_IRD,
              "Implementation row descriptor fields are read-only");
    return SQL_ERROR;
  }
  const auto numeric = static_cast<SQLULEN>(
      reinterpret_cast<std::uintptr_t>(value));
  switch (field_identifier) {
    case SQL_DESC_COUNT:
      if (numeric > static_cast<SQLULEN>(
                        std::numeric_limits<SQLSMALLINT>::max())) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Descriptor record count is out of range");
        return SQL_ERROR;
      }
      records_.resize(static_cast<std::size_t>(numeric));
      return SQL_SUCCESS;
    case SQL_DESC_ARRAY_SIZE:
      if (numeric == 0) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Descriptor array size must be positive");
        return SQL_ERROR;
      }
      array_size_ = numeric;
      return SQL_SUCCESS;
    case SQL_DESC_ARRAY_STATUS_PTR:
      array_status_ptr_ = static_cast<SQLUSMALLINT*>(value);
      return SQL_SUCCESS;
    case SQL_DESC_BIND_OFFSET_PTR:
      bind_offset_ptr_ = static_cast<SQLLEN*>(value);
      return SQL_SUCCESS;
    case SQL_DESC_BIND_TYPE:
      bind_type_ = numeric;
      return SQL_SUCCESS;
    case SQL_DESC_ROWS_PROCESSED_PTR:
      rows_processed_ptr_ = static_cast<SQLULEN*>(value);
      return SQL_SUCCESS;
    default:
      break;
  }

  if (record_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
              "Invalid descriptor record number");
    return SQL_ERROR;
  }
  if (static_cast<std::size_t>(record_number) > records_.size()) {
    records_.resize(static_cast<std::size_t>(record_number));
  }
  auto& record = records_[static_cast<std::size_t>(record_number - 1)];
  switch (field_identifier) {
    case SQL_DESC_TYPE:
      record.type = static_cast<SQLSMALLINT>(numeric);
      record.concise_type = record.type;
      break;
    case SQL_DESC_CONCISE_TYPE:
      record.concise_type = static_cast<SQLSMALLINT>(numeric);
      record.type = record.concise_type;
      break;
    case SQL_DESC_LENGTH: record.length = numeric; break;
    case SQL_DESC_PRECISION:
      record.precision = static_cast<SQLSMALLINT>(numeric); break;
    case SQL_DESC_SCALE:
      record.scale = static_cast<SQLSMALLINT>(numeric); break;
    case SQL_DESC_NULLABLE:
      record.nullable = static_cast<SQLSMALLINT>(numeric); break;
    case SQL_DESC_PARAMETER_TYPE:
      record.parameter_type = static_cast<SQLSMALLINT>(numeric); break;
    case SQL_DESC_DATA_PTR: record.data_ptr = value; break;
    case SQL_DESC_INDICATOR_PTR:
      record.indicator_ptr = static_cast<SQLLEN*>(value); break;
    case SQL_DESC_OCTET_LENGTH_PTR:
      record.octet_length_ptr = static_cast<SQLLEN*>(value); break;
    case SQL_DESC_OCTET_LENGTH:
      record.octet_length = static_cast<SQLLEN>(numeric); break;
    case SQL_DESC_NAME:
      if (!value) {
        set_error(SQLSTATE_INVALID_NULL_POINTER,
                  "Descriptor name pointer is null");
        return SQL_ERROR;
      }
      if (buffer_length == SQL_NTS) {
        record.name = static_cast<const char*>(value);
      } else if (buffer_length >= 0) {
        record.name.assign(static_cast<const char*>(value),
                           static_cast<std::size_t>(buffer_length));
      } else {
        set_error(SQLSTATE_INVALID_STRING_LENGTH,
                  "Invalid descriptor name length");
        return SQL_ERROR;
      }
      break;
    default:
      set_error(SQLSTATE_INVALID_ATTRIBUTE,
                "Unsupported descriptor field");
      return SQL_ERROR;
  }
  return SQL_SUCCESS;
}

SQLRETURN ODBCDescriptor::copy_from(const ODBCDescriptor& source) {
  if (kind_ == DescriptorKind::ImplementationRow) {
    set_error(SQLSTATE_CANNOT_MODIFY_IRD,
              "An implementation row descriptor cannot be a copy target");
    return SQL_ERROR;
  }
  records_ = source.records_;
  array_size_ = source.array_size_;
  array_status_ptr_ = source.array_status_ptr_;
  bind_offset_ptr_ = source.bind_offset_ptr_;
  bind_type_ = source.bind_type_;
  rows_processed_ptr_ = source.rows_processed_ptr_;
  return SQL_SUCCESS;
}

// Statement implementation
SQLRETURN ODBCStatement::execute_direct(const std::string& sql) {
  const auto started = std::chrono::steady_clock::now();
  const auto dynamic_function = classify_dynamic_function(sql);
  set_statement_diagnostic_header(
      0, 0, dynamic_function.name, dynamic_function.code);
  if (executed_ && (!column_info_.empty() || !pending_results_.empty())) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "Cannot execute while results are pending");
    return SQL_ERROR;
  }
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
               get_error_message(), {{"sqlstate", get_sqlstate()},
                                     {"kind", "direct"}});
    return SQL_ERROR;
  }
  if (conn_->logs_queries()) {
    conn_->log(rs::core::logging::LogLevel::Debug, "query_text",
               "Executing direct SQL", {{"sql", sql}});
  }
  
  clear_current_result();
  pending_results_.clear();
  prepared_ = false;
  prepared_sql_.clear();
  parameter_count_ = 0;
  try {
    auto deadline = rs::util::make_deadline(
        timeout_duration(query_timeout_seconds_));
    auto transaction = conn_->begin_transaction_if_needed(deadline);
    if (transaction.has_error()) {
      const auto timeout = transaction.error() ==
          rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
      set_error(timeout ? SQLSTATE_TIMEOUT : SQLSTATE_GENERAL_ERROR,
                transaction.error_message());
      conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
                 transaction.error_message(),
                 {{"sqlstate", get_sqlstate()}, {"kind", "direct"},
                  {"duration_ms", elapsed_milliseconds(started)}});
      if (timeout) conn_->disconnect();
      return SQL_ERROR;
    }
    auto result = conn_->get_db_connection()->execute_query(sql, deadline);
    
    if (result.has_error()) {
      const auto timeout = result.error() ==
          rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
      set_error(timeout ? SQLSTATE_TIMEOUT : SQLSTATE_SYNTAX_ERROR,
                result.error_message());
      conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
                 result.error_message(),
                 {{"sqlstate", get_sqlstate()}, {"kind", "direct"},
                  {"duration_ms", elapsed_milliseconds(started)}});
      if (timeout) conn_->disconnect();
      return SQL_ERROR;
    }

    const auto row_count = result->rows.size();
    const auto affected_rows = result->affected_rows;
    apply_query_result(std::move(*result), false);
    conn_->log(rs::core::logging::LogLevel::Info, "query_completed",
               "Direct SQL execution completed",
               {{"kind", "direct"},
                {"duration_ms", elapsed_milliseconds(started)},
                {"rows", std::to_string(row_count)},
                {"affected_rows", std::to_string(affected_rows)}});
    return SQL_SUCCESS;
    
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed", e.what(),
               {{"sqlstate", get_sqlstate()}, {"kind", "direct"},
                {"duration_ms", elapsed_milliseconds(started)}});
    return SQL_ERROR;
  }
}

SQLRETURN ODBCStatement::set_attribute(SQLINTEGER attribute, SQLPOINTER value) {
  const auto numeric = static_cast<SQLULEN>(
      reinterpret_cast<std::uintptr_t>(value));
  switch (attribute) {
    case SQL_ATTR_APP_ROW_DESC:
    case SQL_ATTR_APP_PARAM_DESC:
      return set_application_descriptor(
          attribute, static_cast<SQLHDESC>(value));
    case SQL_ATTR_IMP_ROW_DESC:
    case SQL_ATTR_IMP_PARAM_DESC:
      set_error(SQLSTATE_INVALID_AUTO_DESCRIPTOR_USE,
                "Implementation descriptors are read-only");
      return SQL_ERROR;
    case SQL_ATTR_QUERY_TIMEOUT:
      query_timeout_seconds_ = numeric;
      return SQL_SUCCESS;
    case SQL_ATTR_MAX_ROWS:
      max_rows_ = numeric;
      return SQL_SUCCESS;
    case SQL_ATTR_CURSOR_TYPE:
      if (numeric == SQL_CURSOR_FORWARD_ONLY) return SQL_SUCCESS;
      break;
    case SQL_ATTR_CONCURRENCY:
      if (numeric == SQL_CONCUR_READ_ONLY) return SQL_SUCCESS;
      break;
    case SQL_ATTR_ROW_ARRAY_SIZE:
      if (numeric == 0) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Row array size must be positive");
        return SQL_ERROR;
      }
      if (numeric == 1) {
        return descriptor(app_row_descriptor_)->set_field(
            0, SQL_DESC_ARRAY_SIZE, value, 0);
      }
      break;
    case SQL_ATTR_ROW_BIND_TYPE:
      if (numeric == SQL_BIND_BY_COLUMN) {
        return descriptor(app_row_descriptor_)->set_field(
            0, SQL_DESC_BIND_TYPE, value, 0);
      }
      break;
    case SQL_ATTR_RETRIEVE_DATA:
      if (numeric == SQL_RD_ON) return SQL_SUCCESS;
      break;
    case SQL_ATTR_USE_BOOKMARKS:
      if (numeric == SQL_UB_OFF) return SQL_SUCCESS;
      break;
    case SQL_ATTR_ASYNC_ENABLE:
      if (numeric == SQL_ASYNC_ENABLE_OFF) return SQL_SUCCESS;
      break;
    case SQL_ATTR_PARAMSET_SIZE:
      if (numeric == 0) {
        set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
                  "Parameter-set size must be positive");
        return SQL_ERROR;
      }
      if (numeric == 1) {
        return descriptor(app_param_descriptor_)->set_field(
            0, SQL_DESC_ARRAY_SIZE, value, 0);
      }
      break;
    case SQL_ATTR_PARAM_BIND_TYPE:
      if (numeric == SQL_PARAM_BIND_BY_COLUMN) {
        return descriptor(app_param_descriptor_)->set_field(
            0, SQL_DESC_BIND_TYPE, value, 0);
      }
      break;
    case SQL_ATTR_METADATA_ID:
      if (numeric == SQL_FALSE) return SQL_SUCCESS;
      break;
    case SQL_ATTR_ROW_STATUS_PTR:
      return descriptor(imp_row_descriptor_)->set_field(
          0, SQL_DESC_ARRAY_STATUS_PTR, value, 0);
    case SQL_ATTR_ROWS_FETCHED_PTR:
      return descriptor(imp_row_descriptor_)->set_field(
          0, SQL_DESC_ROWS_PROCESSED_PTR, value, 0);
    case SQL_ATTR_PARAM_STATUS_PTR:
      return descriptor(imp_param_descriptor_)->set_field(
          0, SQL_DESC_ARRAY_STATUS_PTR, value, 0);
    case SQL_ATTR_PARAMS_PROCESSED_PTR:
      return descriptor(imp_param_descriptor_)->set_field(
          0, SQL_DESC_ROWS_PROCESSED_PTR, value, 0);
    default:
      set_error(SQLSTATE_INVALID_ATTRIBUTE,
                "Unsupported statement attribute");
      return SQL_ERROR;
  }
  set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
            "Requested statement attribute value is not supported");
  return SQL_ERROR;
}

SQLRETURN ODBCStatement::set_application_descriptor(
    SQLINTEGER attribute, SQLHDESC descriptor) {
  auto& active = attribute == SQL_ATTR_APP_ROW_DESC
      ? app_row_descriptor_ : app_param_descriptor_;
  const auto automatic = attribute == SQL_ATTR_APP_ROW_DESC
      ? automatic_app_row_descriptor_ : automatic_app_param_descriptor_;
  if (!descriptor || descriptor == automatic) {
    active = automatic;
    return SQL_SUCCESS;
  }

  auto candidate = HandleRegistry::instance().get_handle_as<ODBCDescriptor>(
      descriptor);
  if (!candidate) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Application descriptor handle is invalid");
    return SQL_ERROR;
  }
  if (candidate->is_automatically_allocated()) {
    set_error(SQLSTATE_INVALID_AUTO_DESCRIPTOR_USE,
              "A different implicit descriptor cannot be associated");
    return SQL_ERROR;
  }
  auto owner = HandleRegistry::instance().get_connection_for_handle(
      descriptor);
  if (!owner || owner.get() != conn_.get()) {
    set_error(SQLSTATE_INVALID_ATTRIBUTE_VALUE,
              "Application descriptor belongs to another connection");
    return SQL_ERROR;
  }
  active = descriptor;
  return SQL_SUCCESS;
}

void ODBCStatement::detach_descriptor(SQLHDESC descriptor) noexcept {
  if (app_row_descriptor_ == descriptor) {
    app_row_descriptor_ = automatic_app_row_descriptor_;
  }
  if (app_param_descriptor_ == descriptor) {
    app_param_descriptor_ = automatic_app_param_descriptor_;
  }
}

std::shared_ptr<ODBCDescriptor> ODBCStatement::descriptor(
    SQLHDESC handle) const {
  return HandleRegistry::instance().get_handle_as<ODBCDescriptor>(handle);
}

SQLRETURN ODBCStatement::get_attribute(SQLINTEGER attribute, SQLPOINTER value) {
  switch (attribute) {
    case SQL_ATTR_APP_ROW_DESC:
      *static_cast<SQLHDESC*>(value) = app_row_descriptor_;
      break;
    case SQL_ATTR_APP_PARAM_DESC:
      *static_cast<SQLHDESC*>(value) = app_param_descriptor_;
      break;
    case SQL_ATTR_IMP_ROW_DESC:
      *static_cast<SQLHDESC*>(value) = imp_row_descriptor_;
      break;
    case SQL_ATTR_IMP_PARAM_DESC:
      *static_cast<SQLHDESC*>(value) = imp_param_descriptor_;
      break;
    case SQL_ATTR_QUERY_TIMEOUT:
      *static_cast<SQLULEN*>(value) = query_timeout_seconds_; break;
    case SQL_ATTR_MAX_ROWS:
      *static_cast<SQLULEN*>(value) = max_rows_; break;
    case SQL_ATTR_CURSOR_TYPE:
      *static_cast<SQLULEN*>(value) = SQL_CURSOR_FORWARD_ONLY; break;
    case SQL_ATTR_CONCURRENCY:
      *static_cast<SQLULEN*>(value) = SQL_CONCUR_READ_ONLY; break;
    case SQL_ATTR_ROW_ARRAY_SIZE:
      *static_cast<SQLULEN*>(value) =
          descriptor(app_row_descriptor_)->array_size(); break;
    case SQL_ATTR_ROW_BIND_TYPE:
      *static_cast<SQLULEN*>(value) =
          descriptor(app_row_descriptor_)->bind_type(); break;
    case SQL_ATTR_RETRIEVE_DATA:
      *static_cast<SQLULEN*>(value) = SQL_RD_ON; break;
    case SQL_ATTR_USE_BOOKMARKS:
      *static_cast<SQLULEN*>(value) = SQL_UB_OFF; break;
    case SQL_ATTR_ASYNC_ENABLE:
      *static_cast<SQLULEN*>(value) = SQL_ASYNC_ENABLE_OFF; break;
    case SQL_ATTR_PARAMSET_SIZE:
      *static_cast<SQLULEN*>(value) =
          descriptor(app_param_descriptor_)->array_size(); break;
    case SQL_ATTR_PARAM_BIND_TYPE:
      *static_cast<SQLULEN*>(value) =
          descriptor(app_param_descriptor_)->bind_type(); break;
    case SQL_ATTR_METADATA_ID:
      *static_cast<SQLULEN*>(value) = SQL_FALSE; break;
    case SQL_ATTR_ROW_STATUS_PTR:
      *static_cast<SQLUSMALLINT**>(value) =
          descriptor(imp_row_descriptor_)->array_status_ptr(); break;
    case SQL_ATTR_ROWS_FETCHED_PTR:
      *static_cast<SQLULEN**>(value) =
          descriptor(imp_row_descriptor_)->rows_processed_ptr(); break;
    case SQL_ATTR_PARAM_STATUS_PTR:
      *static_cast<SQLUSMALLINT**>(value) =
          descriptor(imp_param_descriptor_)->array_status_ptr(); break;
    case SQL_ATTR_PARAMS_PROCESSED_PTR:
      *static_cast<SQLULEN**>(value) =
          descriptor(imp_param_descriptor_)->rows_processed_ptr(); break;
    default:
      set_error(SQLSTATE_INVALID_ATTRIBUTE, "Unsupported statement attribute");
      return SQL_ERROR;
  }
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::close_cursor(bool report_missing_cursor) {
  const bool cursor_open = executed_ && !column_info_.empty();
  if (!cursor_open && report_missing_cursor) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE, "No cursor is open");
    return SQL_ERROR;
  }
  clear_current_result();
  pending_results_.clear();
  return SQL_SUCCESS;
}

void ODBCStatement::unbind_columns() {
  descriptor(app_row_descriptor_)->set_field(
      0, SQL_DESC_COUNT, nullptr, 0);
}

void ODBCStatement::reset_parameters() {
  descriptor(app_param_descriptor_)->set_field(
      0, SQL_DESC_COUNT, nullptr, 0);
}

SQLRETURN ODBCStatement::fetch() {
  if (!executed_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
              "Statement has not been executed");
    return SQL_ERROR;
  }
  if (column_info_.empty()) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "Executed statement did not produce a result set");
    return SQL_ERROR;
  }
  const auto application_descriptor = descriptor(app_row_descriptor_);
  if (application_descriptor->array_size() != 1 ||
      application_descriptor->bind_type() != SQL_BIND_BY_COLUMN ||
      application_descriptor->bind_offset_ptr()) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Only single-row column-wise descriptor binding is supported");
    return SQL_ERROR;
  }
  const auto implementation_descriptor = descriptor(imp_row_descriptor_);
  auto* rows_fetched = implementation_descriptor->rows_processed_ptr();
  auto* row_status = implementation_descriptor->array_status_ptr();
  if (current_row_ >= result_rows_.size()) {
    if (rows_fetched) *rows_fetched = 0;
    if (row_status) row_status[0] = SQL_ROW_NOROW;
    return SQL_NO_DATA;
  }
  
  current_row_++;
  if (rows_fetched) *rows_fetched = 1;
  get_data_offsets_.assign(result_rows_[current_row_ - 1].size(), 0);
  SQLRETURN fetch_result = SQL_SUCCESS;
  
  // Auto-populate bound columns from ARD
  const auto& row = result_rows_[current_row_ - 1];
  for (size_t i = 0;
       i < application_descriptor->record_count() && i < row.size(); ++i) {
    const auto& binding = *application_descriptor->record(i);
    if (binding.data_ptr) {
      const auto& cell = row[i];

      if (!cell) {
        if (!binding.indicator_ptr) {
          set_error(SQLSTATE_INDICATOR_VARIABLE_REQUIRED,
                    "NULL column requires an indicator variable");
          if (row_status) row_status[0] = SQL_ROW_ERROR;
          return SQL_ERROR;
        }
        *binding.indicator_ptr = SQL_NULL_DATA;
        continue;
      }

      if (binding.indicator_ptr &&
          binding.indicator_ptr != binding.octet_length_ptr) {
        *binding.indicator_ptr = 0;
      }

      const SQLSMALLINT sql_type = i < column_info_.size()
          ? column_info_[i].sql_type : static_cast<SQLSMALLINT>(SQL_VARCHAR);
      const SQLSMALLINT target_type = binding.concise_type == SQL_C_DEFAULT
          ? ResultTypes::default_c_type(sql_type) : binding.concise_type;
      if (!ResultTypes::is_conversion_supported(sql_type, target_type)) {
        set_error(SQLSTATE_RESTRICTED_DATA_TYPE,
                  "Unsupported result data type conversion");
        if (row_status) row_status[0] = SQL_ROW_ERROR;
        return SQL_ERROR;
      }
      SQLRETURN conv_result = TextDataConverter::convert_data(
          *cell, target_type, binding.data_ptr, binding.octet_length,
          binding.octet_length_ptr ? binding.octet_length_ptr
                                   : binding.indicator_ptr);

      if (conv_result == SQL_ERROR) {
        set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                  "Result value could not be converted to the requested C type");
        if (row_status) row_status[0] = SQL_ROW_ERROR;
        return SQL_ERROR;
      }
      if (conv_result == SQL_SUCCESS_WITH_INFO) {
        set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                  "Result value was truncated to fit the application buffer");
        fetch_result = SQL_SUCCESS_WITH_INFO;
      }
    }
  }

  if (row_status) {
    row_status[0] = fetch_result == SQL_SUCCESS_WITH_INFO
        ? SQL_ROW_SUCCESS_WITH_INFO : SQL_ROW_SUCCESS;
  }
  return fetch_result;
}

SQLRETURN ODBCStatement::more_results() {
  clear_current_result();
  if (pending_results_.empty()) return SQL_NO_DATA;

  auto next = std::move(pending_results_.front());
  pending_results_.erase(pending_results_.begin());
  apply_query_result(std::move(next), false);
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::get_data(SQLUSMALLINT col, SQLSMALLINT target_type, 
                                 void* buffer, SQLLEN buffer_length, SQLLEN* indicator) {
  if (!executed_ || current_row_ == 0 || current_row_ > result_rows_.size()) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE, "No current row");
    return SQL_ERROR;
  }
  
  const auto& row = result_rows_[current_row_ - 1];
  if (col < 1 || col > row.size()) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid column number");
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

  if (effective_target_type == SQL_C_WCHAR) {
    const auto wide = utf8_to_wide(*cell);
    if (!wide) {
      set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                "Result value is not valid UTF-8");
      return SQL_ERROR;
    }
    if (offset > wide->size()) {
      set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
                "SQLGetData target type changed during chunked retrieval");
      return SQL_ERROR;
    }

    const auto remaining = wide->size() - offset;
    if (indicator) {
      *indicator = static_cast<SQLLEN>(remaining * sizeof(SQLWCHAR));
    }
    const auto buffer_units = buffer_length > 0
        ? static_cast<std::size_t>(buffer_length) / sizeof(SQLWCHAR) : 0;
    const auto capacity = buffer_units > 0 ? buffer_units - 1 : 0;
    auto copy_length = std::min(capacity, remaining);
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (copy_length < remaining && copy_length > 0 &&
          (*wide)[offset + copy_length - 1] >= 0xd800 &&
          (*wide)[offset + copy_length - 1] <= 0xdbff) {
        --copy_length;
      }
    }
    if (copy_length > 0) {
      std::copy_n(wide->begin() + static_cast<std::ptrdiff_t>(offset),
                  copy_length, static_cast<SQLWCHAR*>(buffer));
    }
    if (buffer_units > 0) {
      static_cast<SQLWCHAR*>(buffer)[copy_length] = 0;
    }
    offset += copy_length;
    if (offset < wide->size()) {
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Result value was truncated to fit the application buffer");
      return SQL_SUCCESS_WITH_INFO;
    }
    offset = complete;
    return SQL_SUCCESS;
  }

  if (effective_target_type == SQL_C_BINARY) {
    const auto decoded = TextDataConverter::decode_binary(*cell);
    if (!decoded) {
      set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                "Binary result value has invalid PostgreSQL bytea encoding");
      return SQL_ERROR;
    }
    if (offset > decoded->size()) {
      set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR,
                "SQLGetData target type changed during chunked retrieval");
      return SQL_ERROR;
    }
    const auto remaining = decoded->size() - offset;
    if (indicator) *indicator = static_cast<SQLLEN>(remaining);
    const auto capacity = static_cast<std::size_t>(buffer_length);
    const auto copy_length = std::min(capacity, remaining);
    if (copy_length > 0) {
      std::memcpy(buffer, decoded->data() + offset, copy_length);
    }
    offset += copy_length;
    if (offset < decoded->size()) {
      set_error(SQLSTATE_STRING_DATA_TRUNCATED,
                "Binary result value was truncated to fit the application buffer");
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
  if (executed_ && (!column_info_.empty() || !pending_results_.empty())) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "Cannot prepare while results are pending");
    return SQL_ERROR;
  }
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    return SQL_ERROR;
  }
  
  const auto marker_count =
      rs::core::database::postgres::PgProtocolParser::parameter_marker_count(sql);
  if (marker_count > static_cast<std::size_t>(
          std::numeric_limits<SQLSMALLINT>::max())) {
    set_error(SQLSTATE_GENERAL_ERROR, "Too many parameter markers");
    return SQL_ERROR;
  }
  prepared_sql_ = sql;
  parameter_count_ = static_cast<SQLSMALLINT>(marker_count);
  param_metadata_.clear();
  clear_current_result();
  pending_results_.clear();
  prepared_ = true;
  if (conn_->logs_queries()) {
    conn_->log(rs::core::logging::LogLevel::Debug, "query_prepared",
               "Prepared SQL statement",
               {{"sql", sql}, {"parameters", std::to_string(marker_count)}});
  }
  
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::num_params(SQLSMALLINT* parameter_count) {
  if (!parameter_count) {
    set_error(SQLSTATE_INVALID_NULL_POINTER,
              "Parameter count output pointer is null");
    return SQL_ERROR;
  }
  if (!prepared_ && !executed_) {
    set_error(SQLSTATE_STATEMENT_NOT_PREPARED,
              "Statement has not been prepared or executed");
    return SQL_ERROR;
  }
  *parameter_count = prepared_ ? parameter_count_ : 0;
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::complete_parameter_set(SQLRETURN result) {
  if (parameter_count_ == 0) return result;
  const auto implementation_descriptor = descriptor(imp_param_descriptor_);
  auto* params_processed = implementation_descriptor->rows_processed_ptr();
  auto* param_status = implementation_descriptor->array_status_ptr();
  if (params_processed) *params_processed = 1;
  if (param_status) {
    if (result == SQL_SUCCESS) {
      param_status[0] = SQL_PARAM_SUCCESS;
    } else if (result == SQL_SUCCESS_WITH_INFO) {
      param_status[0] = SQL_PARAM_SUCCESS_WITH_INFO;
    } else {
      param_status[0] = SQL_PARAM_ERROR;
    }
  }
  return result;
}

SQLRETURN ODBCStatement::execute() {
  const auto started = std::chrono::steady_clock::now();
  const auto dynamic_function = classify_dynamic_function(prepared_sql_);
  set_statement_diagnostic_header(
      0, 0, dynamic_function.name, dynamic_function.code);
  if (!prepared_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR, "Statement not prepared");
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
               get_error_message(), {{"sqlstate", get_sqlstate()},
                                     {"kind", "prepared"}});
    return SQL_ERROR;
  }
  if (executed_ && (!column_info_.empty() || !pending_results_.empty())) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "Cannot re-execute while results are pending");
    return SQL_ERROR;
  }
  
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_FAILURE, "Connection not established");
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
               get_error_message(), {{"sqlstate", get_sqlstate()},
                                     {"kind", "prepared"}});
    return SQL_ERROR;
  }
  const auto application_descriptor = descriptor(app_param_descriptor_);
  if (application_descriptor->array_size() != 1 ||
      application_descriptor->bind_type() != SQL_PARAM_BIND_BY_COLUMN ||
      application_descriptor->bind_offset_ptr() ||
      application_descriptor->array_status_ptr()) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Only one column-wise parameter set is supported");
    return SQL_ERROR;
  }
  if (parameter_count_ > 0) {
    const auto implementation_descriptor = descriptor(imp_param_descriptor_);
    if (auto* processed = implementation_descriptor->rows_processed_ptr()) {
      *processed = 0;
    }
    if (auto* status = implementation_descriptor->array_status_ptr()) {
      status[0] = SQL_PARAM_UNUSED;
    }
  }
  if (conn_->logs_queries()) {
    conn_->log(rs::core::logging::LogLevel::Debug, "query_text",
               "Executing prepared SQL",
               {{"sql", prepared_sql_},
                {"parameters", std::to_string(parameter_count_)}});
  }
  
  clear_current_result();
  pending_results_.clear();
  try {
    const auto implementation_descriptor = descriptor(imp_param_descriptor_);
    if (application_descriptor->record_count() <
            static_cast<std::size_t>(parameter_count_) ||
        implementation_descriptor->record_count() <
            static_cast<std::size_t>(parameter_count_)) {
      set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
                "Not all statement parameters are bound");
      return complete_parameter_set(SQL_ERROR);
    }
    std::vector<rs::core::database::QueryParameter> param_values;
    param_values.reserve(static_cast<std::size_t>(parameter_count_));
    
    for (SQLSMALLINT index = 0; index < parameter_count_; ++index) {
      const auto& application = *application_descriptor->record(
          static_cast<std::size_t>(index));
      const auto& implementation = *implementation_descriptor->record(
          static_cast<std::size_t>(index));
      const auto* length_or_indicator = application.octet_length_ptr
          ? application.octet_length_ptr : application.indicator_ptr;
      const bool is_null = application.indicator_ptr &&
          *application.indicator_ptr == SQL_NULL_DATA;
      if (!application.data_ptr && !is_null) {
        set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
                  "Not all statement parameters are bound");
        return complete_parameter_set(SQL_ERROR);
      }
      if (implementation.parameter_type != SQL_PARAM_INPUT) {
        set_error(SQLSTATE_GENERAL_ERROR,
                  "Only input parameters are currently supported");
        return complete_parameter_set(SQL_ERROR);
      }

      const auto value_type = application.concise_type == SQL_C_DEFAULT
          ? ResultTypes::default_c_type(implementation.concise_type)
          : application.concise_type;
      rs::core::database::QueryParameter query_param;
      query_param.type = parameter_type_for(
          implementation.concise_type, value_type);
      if (is_null) {
        query_param.value = std::nullopt;
        param_values.push_back(std::move(query_param));
        continue;
      }

      std::string value;
      if (value_type == SQL_C_CHAR) {
        const auto* text = static_cast<const char*>(application.data_ptr);
        SQLLEN length = application.octet_length;
        if (length_or_indicator) length = *length_or_indicator;
        if (length == SQL_NTS || length == 0) {
          value.assign(text);
        } else if (length >= 0) {
          value.assign(text, static_cast<std::size_t>(length));
        } else {
          set_error(SQLSTATE_GENERAL_ERROR,
                    "Data-at-execution parameters are not supported yet");
          return complete_parameter_set(SQL_ERROR);
        }
      } else if (value_type == SQL_C_WCHAR) {
        const auto* text = static_cast<const SQLWCHAR*>(application.data_ptr);
        SQLLEN length = application.octet_length;
        if (length_or_indicator) length = *length_or_indicator;
        SQLINTEGER units = SQL_NTS;
        if (length != SQL_NTS && length != 0) {
          if (length < 0 || length % sizeof(SQLWCHAR) != 0 ||
              static_cast<SQLULEN>(length / sizeof(SQLWCHAR)) >
                  static_cast<SQLULEN>(
                      std::numeric_limits<SQLINTEGER>::max())) {
            set_error(SQLSTATE_INVALID_STRING_LENGTH,
                      "Invalid wide-character parameter length");
            return complete_parameter_set(SQL_ERROR);
          }
          units = static_cast<SQLINTEGER>(length / sizeof(SQLWCHAR));
        }
        const auto converted = sqlwchar_to_utf8(text, units);
        if (!converted) {
          set_error(SQLSTATE_INVALID_CHARACTER_VALUE,
                    "Invalid wide-character parameter value");
          return complete_parameter_set(SQL_ERROR);
        }
        value = *converted;
      } else if (value_type == SQL_C_SSHORT) {
        value = std::to_string(*static_cast<SQLSMALLINT*>(application.data_ptr));
      } else if (value_type == SQL_C_SLONG) {
        value = std::to_string(*static_cast<SQLINTEGER*>(application.data_ptr));
      } else if (value_type == SQL_C_SBIGINT) {
        value = std::to_string(*static_cast<SQLBIGINT*>(application.data_ptr));
      } else if (value_type == SQL_C_FLOAT) {
        value = std::to_string(*static_cast<SQLREAL*>(application.data_ptr));
      } else if (value_type == SQL_C_DOUBLE) {
        value = std::to_string(*static_cast<SQLDOUBLE*>(application.data_ptr));
      } else if (value_type == SQL_C_BIT) {
        value = *static_cast<unsigned char*>(application.data_ptr) ? "1" : "0";
      } else if (value_type == SQL_C_BINARY) {
        SQLLEN length = application.octet_length;
        if (length_or_indicator) length = *length_or_indicator;
        if (length < 0) {
          set_error(SQLSTATE_INVALID_STRING_LENGTH,
                    "Invalid binary parameter length");
          return complete_parameter_set(SQL_ERROR);
        }
        value = TextDataConverter::encode_binary(std::span<const std::byte>(
            static_cast<const std::byte*>(application.data_ptr),
            static_cast<std::size_t>(length)));
      } else {
        set_error(SQLSTATE_GENERAL_ERROR,
                  "Unsupported C parameter type");
        return complete_parameter_set(SQL_ERROR);
      }

      query_param.value = std::move(value);
      param_values.push_back(std::move(query_param));
    }
    
    // Use PostgreSQL Parse/Bind/Execute protocol
    auto deadline = rs::util::make_deadline(
        timeout_duration(query_timeout_seconds_));
    auto transaction = conn_->begin_transaction_if_needed(deadline);
    if (transaction.has_error()) {
      const auto timeout = transaction.error() ==
          rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
      set_error(timeout ? SQLSTATE_TIMEOUT : SQLSTATE_GENERAL_ERROR,
                transaction.error_message());
      conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
                 transaction.error_message(),
                 {{"sqlstate", get_sqlstate()}, {"kind", "prepared"},
                  {"duration_ms", elapsed_milliseconds(started)}});
      if (timeout) conn_->disconnect();
      return complete_parameter_set(SQL_ERROR);
    }
    auto result = conn_->get_db_connection()->execute_prepared(prepared_sql_, param_values, deadline);
    
    if (result.has_error()) {
      const auto timeout = result.error() ==
          rs::util::make_error_code(rs::util::DbErrorCode::Timeout);
      set_error(timeout ? SQLSTATE_TIMEOUT : SQLSTATE_SYNTAX_ERROR,
                result.error_message());
      conn_->log(rs::core::logging::LogLevel::Error, "query_failed",
                 result.error_message(),
                 {{"sqlstate", get_sqlstate()}, {"kind", "prepared"},
                  {"duration_ms", elapsed_milliseconds(started)}});
      if (timeout) conn_->disconnect();
      return complete_parameter_set(SQL_ERROR);
    }

    const auto row_count = result->rows.size();
    const auto affected_rows = result->affected_rows;
    apply_query_result(std::move(*result), true);
    conn_->log(rs::core::logging::LogLevel::Info, "query_completed",
               "Prepared SQL execution completed",
               {{"kind", "prepared"},
                {"duration_ms", elapsed_milliseconds(started)},
                {"rows", std::to_string(row_count)},
                {"affected_rows", std::to_string(affected_rows)},
                {"parameters", std::to_string(parameter_count_)}});
    return complete_parameter_set(SQL_SUCCESS);
    
  } catch (const std::exception& e) {
    set_error(SQLSTATE_GENERAL_ERROR, e.what());
    conn_->log(rs::core::logging::LogLevel::Error, "query_failed", e.what(),
               {{"sqlstate", get_sqlstate()}, {"kind", "prepared"},
                {"duration_ms", elapsed_milliseconds(started)}});
    return complete_parameter_set(SQL_ERROR);
  }
}

SQLRETURN ODBCStatement::bind_parameter(SQLUSMALLINT parameter_number, SQLSMALLINT input_output_type,
                                       SQLSMALLINT value_type, SQLSMALLINT parameter_type, SQLULEN column_size,
                                       SQLSMALLINT decimal_digits, SQLPOINTER parameter_value, SQLLEN buffer_length,
                                       SQLLEN* strlen_or_indicator) {
  if (parameter_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid parameter number");
    return SQL_ERROR;
  }
  const bool valid_direction = input_output_type == SQL_PARAM_INPUT ||
      input_output_type == SQL_PARAM_INPUT_OUTPUT ||
      input_output_type == SQL_PARAM_OUTPUT
#ifdef SQL_PARAM_INPUT_OUTPUT_STREAM
      || input_output_type == SQL_PARAM_INPUT_OUTPUT_STREAM
#endif
#ifdef SQL_PARAM_OUTPUT_STREAM
      || input_output_type == SQL_PARAM_OUTPUT_STREAM
#endif
      ;
  if (!valid_direction) {
    set_error(SQLSTATE_INVALID_PARAMETER_TYPE,
              "Invalid input/output parameter type");
    return SQL_ERROR;
  }
  if (input_output_type != SQL_PARAM_INPUT) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Output parameters are not supported");
    return SQL_ERROR;
  }
  if (buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Parameter buffer length cannot be negative");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_valid_c_type(value_type)) {
    set_error(SQLSTATE_INVALID_APPLICATION_BUFFER_TYPE,
              "Invalid parameter application buffer type");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_supported_parameter_c_type(value_type)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Parameter application buffer type is not supported");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_valid_sql_type(parameter_type)) {
    set_error(SQLSTATE_INVALID_SQL_DATA_TYPE,
              "Invalid parameter SQL data type");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_supported_parameter_sql_type(parameter_type)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Parameter SQL data type is not supported");
    return SQL_ERROR;
  }
  if (!parameter_value && !strlen_or_indicator) {
    set_error(SQLSTATE_INVALID_NULL_POINTER,
              "Input parameter requires a value or indicator pointer");
    return SQL_ERROR;
  }

  const auto number = [](auto numeric) {
    return reinterpret_cast<SQLPOINTER>(
        static_cast<std::uintptr_t>(numeric));
  };
  const auto application_descriptor = descriptor(app_param_descriptor_);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_CONCISE_TYPE, number(value_type), 0);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_OCTET_LENGTH, number(buffer_length), 0);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_DATA_PTR, parameter_value, 0);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_INDICATOR_PTR, strlen_or_indicator, 0);
  application_descriptor->set_field(
      parameter_number, SQL_DESC_OCTET_LENGTH_PTR, strlen_or_indicator, 0);

  const auto implementation_descriptor = descriptor(imp_param_descriptor_);
  implementation_descriptor->set_field(
      parameter_number, SQL_DESC_CONCISE_TYPE, number(parameter_type), 0);
  implementation_descriptor->set_field(
      parameter_number, SQL_DESC_LENGTH, number(column_size), 0);
  implementation_descriptor->set_field(
      parameter_number, SQL_DESC_SCALE, number(decimal_digits), 0);
  implementation_descriptor->set_field(
      parameter_number, SQL_DESC_PARAMETER_TYPE, number(input_output_type), 0);
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
  const auto command_tag = result.command_tag;
  if (!result.additional_results.empty()) {
    pending_results_.reserve(
        pending_results_.size() + result.additional_results.size());
    for (auto& additional : result.additional_results) {
      pending_results_.push_back(std::move(additional));
    }
  }
  result_rows_ = std::move(result.rows);
  if (max_rows_ > 0 && result_rows_.size() > max_rows_) {
    result_rows_.resize(static_cast<std::size_t>(max_rows_));
  }
  current_row_ = 0;
  get_data_offsets_.clear();
  executed_ = true;

  const auto max_rows = static_cast<std::size_t>(
      std::numeric_limits<SQLLEN>::max());
  affected_rows_ = result.affected_rows > max_rows
      ? std::numeric_limits<SQLLEN>::max()
      : static_cast<SQLLEN>(result.affected_rows);
  auto diagnostic_header = get_diagnostic_header();
  if (!command_tag.empty()) {
    const auto dynamic_function = classify_dynamic_function(command_tag);
    diagnostic_header.dynamic_function = dynamic_function.name;
    diagnostic_header.dynamic_function_code = dynamic_function.code;
  }
  const auto cursor_rows = result_rows_.size() > max_rows
      ? std::numeric_limits<SQLLEN>::max()
      : static_cast<SQLLEN>(result_rows_.size());
  set_statement_diagnostic_header(
      cursor_rows, affected_rows_, diagnostic_header.dynamic_function,
      diagnostic_header.dynamic_function_code);

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
  std::vector<DescriptorRecord> row_descriptor_records;
  row_descriptor_records.reserve(column_info_.size());
  for (const auto& column : column_info_) {
    row_descriptor_records.push_back(descriptor_record_for(column));
  }
  descriptor(imp_row_descriptor_)->replace_records(
      std::move(row_descriptor_records));

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
    std::vector<DescriptorRecord> parameter_descriptor_records;
    parameter_descriptor_records.reserve(param_metadata_.size());
    for (const auto& parameter : param_metadata_) {
      parameter_descriptor_records.push_back(
          descriptor_record_for(parameter));
    }
    descriptor(imp_param_descriptor_)->replace_records(
        std::move(parameter_descriptor_records));
  }
}

void ODBCStatement::clear_current_result() {
  result_rows_.clear();
  column_info_.clear();
  descriptor(imp_row_descriptor_)->replace_records({});
  get_data_offsets_.clear();
  current_row_ = 0;
  affected_rows_ = 0;
  executed_ = false;
}

// Column binding implementation
SQLRETURN ODBCStatement::bind_col(SQLUSMALLINT column_number, SQLSMALLINT target_type,
                                  SQLPOINTER target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator) {
  if (column_number < 1) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid column number");
    return SQL_ERROR;
  }
  if (buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Column buffer length cannot be negative");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_valid_c_type(target_type)) {
    set_error(SQLSTATE_INVALID_APPLICATION_BUFFER_TYPE,
              "Invalid column application buffer type");
    return SQL_ERROR;
  }
  if (!ResultTypes::is_supported_c_type(target_type)) {
    set_error(SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED,
              "Column application buffer type is not supported");
    return SQL_ERROR;
  }
  
  // Check if column number is valid (after execution)
  if (executed_ && column_number > column_info_.size()) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER,
              "Column number out of range");
    return SQL_ERROR;
  }

  const auto number = [](auto numeric) {
    return reinterpret_cast<SQLPOINTER>(
        static_cast<std::uintptr_t>(numeric));
  };
  const auto application_descriptor = descriptor(app_row_descriptor_);
  application_descriptor->set_field(
      column_number, SQL_DESC_CONCISE_TYPE, number(target_type), 0);
  application_descriptor->set_field(
      column_number, SQL_DESC_OCTET_LENGTH, number(buffer_length), 0);
  application_descriptor->set_field(
      column_number, SQL_DESC_DATA_PTR, target_value, 0);
  application_descriptor->set_field(
      column_number, SQL_DESC_INDICATOR_PTR, strlen_or_indicator, 0);
  application_descriptor->set_field(
      column_number, SQL_DESC_OCTET_LENGTH_PTR, strlen_or_indicator, 0);
  return SQL_SUCCESS;
}

// Metadata functions implementation
SQLRETURN ODBCStatement::get_num_result_cols(SQLSMALLINT* column_count) {
  if (!column_count) {
    set_error(SQLSTATE_INVALID_NULL_POINTER,
              "Null pointer for column count");
    return SQL_ERROR;
  }
  
  if (!executed_) {
    set_error(SQLSTATE_FUNCTION_SEQUENCE_ERROR, "No query executed");
    return SQL_ERROR;
  }
  
  *column_count = static_cast<SQLSMALLINT>(column_info_.size());
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::get_type_info(SQLSMALLINT data_type) {
  if (!conn_->is_connected()) {
    set_error(SQLSTATE_CONNECTION_NOT_OPEN, "Connection is not open");
    return SQL_ERROR;
  }
  if (executed_ && !column_info_.empty()) {
    set_error(SQLSTATE_INVALID_CURSOR_STATE,
              "A result cursor is already open");
    return SQL_ERROR;
  }

  using rs::core::database::QueryResult;
  using rs::core::database::ResultColumnMetadata;
  QueryResult result;
  const auto column = [](const char* name, std::uint32_t oid,
                         std::int16_t size) {
    return ResultColumnMetadata{name, 0, 0, oid, size, -1, 0};
  };
  result.columns = {
      column("TYPE_NAME", 25, -1),
      column("DATA_TYPE", 21, 2),
      column("COLUMN_SIZE", 23, 4),
      column("LITERAL_PREFIX", 25, -1),
      column("LITERAL_SUFFIX", 25, -1),
      column("CREATE_PARAMS", 25, -1),
      column("NULLABLE", 21, 2),
      column("CASE_SENSITIVE", 21, 2),
      column("SEARCHABLE", 21, 2),
      column("UNSIGNED_ATTRIBUTE", 21, 2),
      column("FIXED_PREC_SCALE", 21, 2),
      column("AUTO_UNIQUE_VALUE", 21, 2),
      column("LOCAL_TYPE_NAME", 25, -1),
      column("MINIMUM_SCALE", 21, 2),
      column("MAXIMUM_SCALE", 21, 2),
      column("SQL_DATA_TYPE", 21, 2),
      column("SQL_DATETIME_SUB", 21, 2),
      column("NUM_PREC_RADIX", 23, 4),
      column("INTERVAL_PRECISION", 21, 2),
  };

  for (const auto& type : type_info_definitions) {
    if (data_type != SQL_ALL_TYPES && data_type != type.data_type) continue;
    result.rows.push_back({
        type_info_text(type.name), type_info_number(type.data_type),
        type_info_number(type.column_size), type_info_text(type.literal_prefix),
        type_info_text(type.literal_suffix), type_info_text(type.create_params),
        type_info_number(SQL_NULLABLE), type_info_number(type.case_sensitive),
        type_info_number(SQL_SEARCHABLE),
        type.unsigned_attribute < 0 ? rs::core::database::ResultCell{}
                                    : type_info_number(type.unsigned_attribute),
        type_info_number(SQL_FALSE),
        type.unsigned_attribute < 0 ? rs::core::database::ResultCell{}
                                    : type_info_number(SQL_FALSE),
        rs::core::database::ResultCell{},
        type.minimum_scale < 0 ? rs::core::database::ResultCell{}
                               : type_info_number(type.minimum_scale),
        type.maximum_scale < 0 ? rs::core::database::ResultCell{}
                               : type_info_number(type.maximum_scale),
        type_info_number(type.sql_data_type),
        type.datetime_sub == 0 ? rs::core::database::ResultCell{}
                               : type_info_number(type.datetime_sub),
        type.numeric_radix == 0 ? rs::core::database::ResultCell{}
                                : type_info_number(type.numeric_radix),
        rs::core::database::ResultCell{},
    });
  }
  result.affected_rows = result.rows.size();
  apply_query_result(std::move(result), false);
  return SQL_SUCCESS;
}

SQLRETURN ODBCStatement::tables(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::optional<std::string>& table_name,
    const std::optional<std::string>& table_type) {
  const bool empty_schema = !schema_name || schema_name->empty();
  const bool empty_table = !table_name || table_name->empty();
  const bool empty_type = !table_type || table_type->empty();

  if (catalog_name && *catalog_name == SQL_ALL_CATALOGS && empty_schema &&
      empty_table && empty_type) {
    return execute_direct(
        "SELECT current_database()::text AS table_cat, NULL::text AS "
        "table_schem, NULL::text AS table_name, NULL::text AS table_type, "
        "NULL::text AS remarks");
  }
  if (schema_name && *schema_name == SQL_ALL_SCHEMAS &&
      (!catalog_name || catalog_name->empty()) && empty_table && empty_type) {
    return execute_direct(
        "SELECT NULL::text AS table_cat, schema_name::text AS table_schem, "
        "NULL::text AS table_name, NULL::text AS table_type, NULL::text AS "
        "remarks FROM information_schema.schemata ORDER BY table_schem");
  }
  if (table_type && *table_type == SQL_ALL_TABLE_TYPES &&
      (!catalog_name || catalog_name->empty()) && empty_schema && empty_table) {
    return execute_direct(
        "SELECT NULL::text AS table_cat, NULL::text AS table_schem, "
        "NULL::text AS table_name, table_type, NULL::text AS remarks FROM "
        "(VALUES ('TABLE'::text), ('VIEW'::text), ('SYSTEM TABLE'::text), "
        "('FOREIGN TABLE'::text)) AS supported(table_type) ORDER BY table_type");
  }

  std::string query =
      "SELECT table_cat, table_schem, table_name, table_type, NULL::text AS "
      "remarks FROM (SELECT current_database()::text AS table_cat, "
      "table_schema::text AS table_schem, table_name::text AS table_name, "
      "CASE WHEN table_type = 'VIEW' THEN 'VIEW' WHEN table_schema IN "
      "('pg_catalog', 'information_schema') THEN 'SYSTEM TABLE' WHEN "
      "table_type IN ('BASE TABLE', 'LOCAL TEMPORARY') THEN 'TABLE' WHEN "
      "table_type = 'FOREIGN' THEN 'FOREIGN TABLE' ELSE table_type END::text "
      "AS table_type FROM information_schema.tables) AS odbcpp_tables WHERE 1=1";
  if (catalog_name && !catalog_name->empty()) {
    query += " AND table_cat LIKE " + quote_catalog_literal(*catalog_name);
  }
  if (schema_name && !schema_name->empty()) {
    query += " AND table_schem LIKE " + quote_catalog_literal(*schema_name);
  }
  if (table_name && !table_name->empty()) {
    query += " AND table_name LIKE " + quote_catalog_literal(*table_name);
  }
  if (table_type && !table_type->empty()) {
    const auto types = parse_table_types(*table_type);
    if (types.empty()) {
      query += " AND FALSE";
    } else {
      query += " AND table_type IN (";
      for (std::size_t i = 0; i < types.size(); ++i) {
        if (i != 0) query += ',';
        query += quote_catalog_literal(types[i]);
      }
      query += ')';
    }
  }
  query += " ORDER BY table_type, table_cat, table_schem, table_name";
  return execute_direct(query);
}

SQLRETURN ODBCStatement::columns(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::optional<std::string>& table_name,
    const std::optional<std::string>& column_name) {
  std::string query =
      "SELECT table_cat, table_schem, table_name, column_name, data_type, "
      "type_name, column_size, buffer_length, decimal_digits, "
      "num_prec_radix, nullable, remarks, column_def, sql_data_type, "
      "sql_datetime_sub, char_octet_length, ordinal_position, is_nullable "
      "FROM (SELECT current_database()::text AS table_cat, "
      "table_schema::text AS table_schem, table_name::text AS table_name, "
      "column_name::text AS column_name, "
      "CASE data_type "
      "WHEN 'boolean' THEN -7 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 4 WHEN 'bigint' THEN -5 "
      "WHEN 'real' THEN 7 WHEN 'double precision' THEN 8 "
      "WHEN 'numeric' THEN 2 WHEN 'decimal' THEN 3 "
      "WHEN 'character' THEN 1 WHEN 'character varying' THEN 12 "
      "WHEN 'text' THEN -1 WHEN 'bytea' THEN -3 "
      "WHEN 'date' THEN 91 "
      "WHEN 'time without time zone' THEN 92 "
      "WHEN 'time with time zone' THEN 92 "
      "WHEN 'timestamp without time zone' THEN 93 "
      "WHEN 'timestamp with time zone' THEN 93 ELSE 12 END::smallint "
      "AS data_type, "
      "CASE data_type "
      "WHEN 'character' THEN 'character' "
      "WHEN 'character varying' THEN 'character varying' "
      "WHEN 'time without time zone' THEN 'time' "
      "WHEN 'timestamp without time zone' THEN 'timestamp' "
      "ELSE data_type END::text AS type_name, "
      "CASE data_type "
      "WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 10 WHEN 'bigint' THEN 19 "
      "WHEN 'real' THEN 7 WHEN 'double precision' THEN 15 "
      "WHEN 'numeric' THEN numeric_precision "
      "WHEN 'decimal' THEN numeric_precision "
      "WHEN 'character' THEN character_maximum_length "
      "WHEN 'character varying' THEN character_maximum_length "
      "WHEN 'text' THEN 1073741824 WHEN 'bytea' THEN 1073741824 "
      "WHEN 'date' THEN 10 "
      "WHEN 'time without time zone' THEN 15 "
      "WHEN 'time with time zone' THEN 21 "
      "WHEN 'timestamp without time zone' THEN 29 "
      "WHEN 'timestamp with time zone' THEN 35 "
      "ELSE character_maximum_length END::integer AS column_size, "
      "CASE data_type "
      "WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 2 "
      "WHEN 'integer' THEN 4 WHEN 'bigint' THEN 8 "
      "WHEN 'real' THEN 4 WHEN 'double precision' THEN 8 "
      "WHEN 'numeric' THEN numeric_precision + 2 "
      "WHEN 'decimal' THEN numeric_precision + 2 "
      "WHEN 'character' THEN character_octet_length "
      "WHEN 'character varying' THEN character_octet_length "
      "WHEN 'text' THEN 1073741824 WHEN 'bytea' THEN 1073741824 "
      "WHEN 'date' THEN 10 "
      "WHEN 'time without time zone' THEN 15 "
      "WHEN 'time with time zone' THEN 21 "
      "WHEN 'timestamp without time zone' THEN 29 "
      "WHEN 'timestamp with time zone' THEN 35 ELSE NULL END::integer "
      "AS buffer_length, "
      "CASE WHEN data_type IN ('numeric', 'decimal') THEN numeric_scale "
      "WHEN data_type IN ('time without time zone', 'time with time zone', "
      "'timestamp without time zone', 'timestamp with time zone') "
      "THEN datetime_precision WHEN data_type IN ('smallint', 'integer', "
      "'bigint') THEN 0 ELSE NULL END::smallint AS decimal_digits, "
      "CASE WHEN data_type IN ('smallint', 'integer', 'bigint', 'real', "
      "'double precision', 'numeric', 'decimal') THEN numeric_precision_radix "
      "ELSE NULL END::smallint AS num_prec_radix, "
      "CASE is_nullable WHEN 'YES' THEN 1 ELSE 0 END::smallint AS nullable, "
      "NULL::text AS remarks, column_default::text AS column_def, "
      "CASE WHEN data_type IN ('date', 'time without time zone', "
      "'time with time zone', 'timestamp without time zone', "
      "'timestamp with time zone') THEN 9 ELSE CASE data_type "
      "WHEN 'boolean' THEN -7 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 4 WHEN 'bigint' THEN -5 "
      "WHEN 'real' THEN 7 WHEN 'double precision' THEN 8 "
      "WHEN 'numeric' THEN 2 WHEN 'decimal' THEN 3 "
      "WHEN 'character' THEN 1 WHEN 'character varying' THEN 12 "
      "WHEN 'text' THEN -1 WHEN 'bytea' THEN -3 ELSE 12 END "
      "END::smallint AS sql_data_type, "
      "CASE data_type WHEN 'date' THEN 1 "
      "WHEN 'time without time zone' THEN 2 "
      "WHEN 'time with time zone' THEN 2 "
      "WHEN 'timestamp without time zone' THEN 3 "
      "WHEN 'timestamp with time zone' THEN 3 ELSE NULL END::smallint "
      "AS sql_datetime_sub, "
      "CASE WHEN data_type IN ('character', 'character varying', 'text') "
      "THEN character_octet_length WHEN data_type = 'bytea' "
      "THEN 1073741824 ELSE NULL END::integer AS char_octet_length, "
      "ordinal_position::integer AS ordinal_position, "
      "is_nullable::text AS is_nullable FROM information_schema.columns) "
      "AS odbcpp_columns WHERE 1=1";
  if (catalog_name && !catalog_name->empty()) {
    query += " AND table_cat LIKE " + quote_catalog_literal(*catalog_name);
  }
  if (schema_name && !schema_name->empty()) {
    query += " AND table_schem LIKE " + quote_catalog_literal(*schema_name);
  }
  if (table_name && !table_name->empty()) {
    query += " AND table_name LIKE " + quote_catalog_literal(*table_name);
  }
  if (column_name && !column_name->empty()) {
    query += " AND column_name LIKE " + quote_catalog_literal(*column_name);
  }
  query += " ORDER BY table_cat, table_schem, table_name, ordinal_position";
  return execute_direct(query);
}

SQLRETURN ODBCStatement::primary_keys(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::string& table_name) {
  std::string query =
      "SELECT current_database()::text AS table_cat, "
      "keys.table_schema::text AS table_schem, "
      "keys.table_name::text AS table_name, "
      "keys.column_name::text AS column_name, "
      "keys.ordinal_position::smallint AS key_seq, "
      "constraints.constraint_name::text AS pk_name "
      "FROM information_schema.table_constraints AS constraints "
      "JOIN information_schema.key_column_usage AS keys "
      "ON constraints.constraint_catalog = keys.constraint_catalog "
      "AND constraints.constraint_schema = keys.constraint_schema "
      "AND constraints.constraint_name = keys.constraint_name "
      "AND constraints.table_catalog = keys.table_catalog "
      "AND constraints.table_schema = keys.table_schema "
      "AND constraints.table_name = keys.table_name "
      "WHERE constraints.constraint_type = 'PRIMARY KEY' "
      "AND keys.table_name = " + quote_catalog_literal(table_name);
  if (catalog_name && !catalog_name->empty()) {
    query += " AND keys.table_catalog = " +
        quote_catalog_literal(*catalog_name);
  }
  if (schema_name && !schema_name->empty()) {
    query += " AND keys.table_schema = " +
        quote_catalog_literal(*schema_name);
  }
  query +=
      " ORDER BY table_cat, table_schem, table_name, key_seq";
  return execute_direct(query);
}

SQLRETURN ODBCStatement::foreign_keys(
    const std::optional<std::string>& pk_catalog_name,
    const std::optional<std::string>& pk_schema_name,
    const std::optional<std::string>& pk_table_name,
    const std::optional<std::string>& fk_catalog_name,
    const std::optional<std::string>& fk_schema_name,
    const std::optional<std::string>& fk_table_name) {
  std::string query =
      "SELECT primary_keys.table_catalog::text AS pktable_cat, "
      "primary_keys.table_schema::text AS pktable_schem, "
      "primary_keys.table_name::text AS pktable_name, "
      "primary_keys.column_name::text AS pkcolumn_name, "
      "foreign_keys.table_catalog::text AS fktable_cat, "
      "foreign_keys.table_schema::text AS fktable_schem, "
      "foreign_keys.table_name::text AS fktable_name, "
      "foreign_keys.column_name::text AS fkcolumn_name, "
      "foreign_keys.ordinal_position::smallint AS key_seq, "
      "CASE relations.update_rule WHEN 'CASCADE' THEN 0 "
      "WHEN 'RESTRICT' THEN 1 WHEN 'SET NULL' THEN 2 "
      "WHEN 'NO ACTION' THEN 3 WHEN 'SET DEFAULT' THEN 4 "
      "ELSE 3 END::smallint AS update_rule, "
      "CASE relations.delete_rule WHEN 'CASCADE' THEN 0 "
      "WHEN 'RESTRICT' THEN 1 WHEN 'SET NULL' THEN 2 "
      "WHEN 'NO ACTION' THEN 3 WHEN 'SET DEFAULT' THEN 4 "
      "ELSE 3 END::smallint AS delete_rule, "
      "fk_constraints.constraint_name::text AS fk_name, "
      "pk_constraints.constraint_name::text AS pk_name, "
      "CASE WHEN fk_constraints.is_deferrable = 'NO' THEN 7 "
      "WHEN fk_constraints.initially_deferred = 'YES' THEN 5 "
      "ELSE 6 END::smallint AS deferrability "
      "FROM information_schema.referential_constraints AS relations "
      "JOIN information_schema.table_constraints AS fk_constraints "
      "ON relations.constraint_catalog = fk_constraints.constraint_catalog "
      "AND relations.constraint_schema = fk_constraints.constraint_schema "
      "AND relations.constraint_name = fk_constraints.constraint_name "
      "JOIN information_schema.key_column_usage AS foreign_keys "
      "ON fk_constraints.constraint_catalog = foreign_keys.constraint_catalog "
      "AND fk_constraints.constraint_schema = foreign_keys.constraint_schema "
      "AND fk_constraints.constraint_name = foreign_keys.constraint_name "
      "JOIN information_schema.table_constraints AS pk_constraints "
      "ON relations.unique_constraint_catalog = "
      "pk_constraints.constraint_catalog "
      "AND relations.unique_constraint_schema = "
      "pk_constraints.constraint_schema "
      "AND relations.unique_constraint_name = "
      "pk_constraints.constraint_name "
      "JOIN information_schema.key_column_usage AS primary_keys "
      "ON pk_constraints.constraint_catalog = primary_keys.constraint_catalog "
      "AND pk_constraints.constraint_schema = primary_keys.constraint_schema "
      "AND pk_constraints.constraint_name = primary_keys.constraint_name "
      "AND primary_keys.ordinal_position = "
      "foreign_keys.position_in_unique_constraint WHERE 1=1";
  if (pk_catalog_name && !pk_catalog_name->empty()) {
    query += " AND primary_keys.table_catalog = " +
        quote_catalog_literal(*pk_catalog_name);
  }
  if (pk_schema_name && !pk_schema_name->empty()) {
    query += " AND primary_keys.table_schema = " +
        quote_catalog_literal(*pk_schema_name);
  }
  if (pk_table_name) {
    query += " AND primary_keys.table_name = " +
        quote_catalog_literal(*pk_table_name);
  }
  if (fk_catalog_name && !fk_catalog_name->empty()) {
    query += " AND foreign_keys.table_catalog = " +
        quote_catalog_literal(*fk_catalog_name);
  }
  if (fk_schema_name && !fk_schema_name->empty()) {
    query += " AND foreign_keys.table_schema = " +
        quote_catalog_literal(*fk_schema_name);
  }
  if (fk_table_name) {
    query += " AND foreign_keys.table_name = " +
        quote_catalog_literal(*fk_table_name);
  }
  if (pk_table_name) {
    query +=
        " ORDER BY fktable_cat, fktable_schem, fktable_name, key_seq";
  } else {
    query +=
        " ORDER BY pktable_cat, pktable_schem, pktable_name, key_seq";
  }
  return execute_direct(query);
}

SQLRETURN ODBCStatement::statistics(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::string& table_name, bool unique_only) {
  std::string query =
      "SELECT current_database()::text AS table_cat, "
      "namespaces.nspname::text AS table_schem, "
      "tables.relname::text AS table_name, "
      "CASE WHEN indexes.indisunique THEN 0 ELSE 1 END::smallint "
      "AS non_unique, NULL::text AS index_qualifier, "
      "index_names.relname::text AS index_name, 3::smallint AS type, "
      "index_columns.ordinality::smallint AS ordinal_position, "
      "columns.attname::text AS column_name, "
      "CASE WHEN pg_index_column_has_property(indexes.indexrelid, "
      "index_columns.ordinality::integer, 'desc') THEN 'D' "
      "ELSE 'A' END::text AS asc_or_desc, "
      "LEAST(GREATEST(tables.reltuples, 0), 2147483647)::integer "
      "AS cardinality, tables.relpages::integer AS pages, "
      "pg_get_expr(indexes.indpred, indexes.indrelid)::text "
      "AS filter_condition FROM pg_catalog.pg_index AS indexes "
      "JOIN pg_catalog.pg_class AS tables "
      "ON tables.oid = indexes.indrelid "
      "JOIN pg_catalog.pg_namespace AS namespaces "
      "ON namespaces.oid = tables.relnamespace "
      "JOIN pg_catalog.pg_class AS index_names "
      "ON index_names.oid = indexes.indexrelid "
      "CROSS JOIN LATERAL unnest(indexes.indkey) WITH ORDINALITY "
      "AS index_columns(attribute_number, ordinality) "
      "LEFT JOIN pg_catalog.pg_attribute AS columns "
      "ON columns.attrelid = tables.oid "
      "AND columns.attnum = index_columns.attribute_number "
      "WHERE tables.relname = " + quote_catalog_literal(table_name);
  if (catalog_name && !catalog_name->empty()) {
    query += " AND current_database() = " +
        quote_catalog_literal(*catalog_name);
  }
  if (schema_name && !schema_name->empty()) {
    query += " AND namespaces.nspname = " +
        quote_catalog_literal(*schema_name);
  }
  if (unique_only) query += " AND indexes.indisunique";
  query +=
      " ORDER BY non_unique, type, index_qualifier, index_name, "
      "ordinal_position";
  return execute_direct(query);
}

SQLRETURN ODBCStatement::procedures(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::optional<std::string>& procedure_name) {
  std::string query =
      "SELECT current_database()::text AS procedure_cat, "
      "namespaces.nspname::text AS procedure_schem, "
      "procedures.proname::text AS procedure_name, "
      "CASE WHEN procedures.proargmodes IS NULL THEN procedures.pronargs "
      "ELSE (SELECT count(*) FROM unnest(procedures.proargmodes) AS mode "
      "WHERE mode::text IN ('i', 'b', 'v')) END::smallint "
      "AS num_input_params, "
      "CASE WHEN procedures.proargmodes IS NULL THEN 0 "
      "ELSE (SELECT count(*) FROM unnest(procedures.proargmodes) AS mode "
      "WHERE mode::text IN ('o', 'b', 't')) END::smallint "
      "AS num_output_params, -1::smallint AS num_result_sets, "
      "obj_description(procedures.oid, 'pg_proc')::text AS remarks, "
      "CASE WHEN procedures.prokind = 'p' THEN 1 ELSE 2 END::smallint "
      "AS procedure_type FROM pg_catalog.pg_proc AS procedures "
      "JOIN pg_catalog.pg_namespace AS namespaces "
      "ON namespaces.oid = procedures.pronamespace "
      "WHERE procedures.prokind IN ('f', 'p')";
  if (catalog_name && !catalog_name->empty()) {
    query += " AND current_database() LIKE " +
        quote_catalog_literal(*catalog_name);
  }
  if (schema_name && !schema_name->empty()) {
    query += " AND namespaces.nspname LIKE " +
        quote_catalog_literal(*schema_name);
  }
  if (procedure_name && !procedure_name->empty()) {
    query += " AND procedures.proname LIKE " +
        quote_catalog_literal(*procedure_name);
  }
  query +=
      " ORDER BY procedure_cat, procedure_schem, procedure_name";
  return execute_direct(query);
}

SQLRETURN ODBCStatement::procedure_columns(
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::optional<std::string>& procedure_name,
    const std::optional<std::string>& column_name) {
  std::string query =
      "WITH routine_columns AS (SELECT current_database()::text "
      "AS procedure_cat, namespaces.nspname::text AS procedure_schem, "
      "procedures.proname::text AS procedure_name, "
      "procedures.proargnames[arguments.ordinality]::text AS column_name, "
      "CASE COALESCE(procedures.proargmodes[arguments.ordinality], 'i') "
      "WHEN 'i' THEN 1 WHEN 'v' THEN 1 WHEN 'b' THEN 2 "
      "WHEN 't' THEN 3 WHEN 'o' THEN 4 ELSE 0 END::smallint "
      "AS column_type, arguments.type_oid::oid AS type_oid, "
      "arguments.ordinality::integer AS ordinal_position "
      "FROM pg_catalog.pg_proc AS procedures "
      "JOIN pg_catalog.pg_namespace AS namespaces "
      "ON namespaces.oid = procedures.pronamespace "
      "CROSS JOIN LATERAL unnest(CASE WHEN procedures.proallargtypes "
      "IS NOT NULL THEN procedures.proallargtypes "
      "ELSE procedures.proargtypes::oid[] END) WITH ORDINALITY "
      "AS arguments(type_oid, ordinality) "
      "WHERE procedures.prokind IN ('f', 'p') UNION ALL "
      "SELECT current_database()::text, namespaces.nspname::text, "
      "procedures.proname::text, NULL::text, 5::smallint, "
      "procedures.prorettype, 0::integer "
      "FROM pg_catalog.pg_proc AS procedures "
      "JOIN pg_catalog.pg_namespace AS namespaces "
      "ON namespaces.oid = procedures.pronamespace "
      "WHERE procedures.prokind = 'f' AND procedures.prorettype <> 2278 "
      "AND (procedures.proargmodes IS NULL OR NOT EXISTS "
      "(SELECT 1 FROM unnest(procedures.proargmodes) AS mode "
      "WHERE mode::text IN ('o', 'b', 't')))) "
      "SELECT columns.procedure_cat, columns.procedure_schem, "
      "columns.procedure_name, columns.column_name, columns.column_type, "
      "CASE types.oid WHEN 16 THEN -7 WHEN 17 THEN -3 WHEN 18 THEN 1 "
      "WHEN 20 THEN -5 WHEN 21 THEN 5 WHEN 23 THEN 4 WHEN 25 THEN -1 "
      "WHEN 700 THEN 7 WHEN 701 THEN 8 WHEN 1042 THEN 1 "
      "WHEN 1043 THEN 12 WHEN 1082 THEN 91 "
      "WHEN 1083 THEN 92 WHEN 1266 THEN 92 "
      "WHEN 1114 THEN 93 WHEN 1184 THEN 93 "
      "WHEN 1700 THEN 2 ELSE 12 END::smallint AS data_type, "
      "CASE types.oid WHEN 16 THEN 'boolean' WHEN 17 THEN 'bytea' "
      "WHEN 18 THEN 'char' WHEN 20 THEN 'bigint' WHEN 21 THEN 'smallint' "
      "WHEN 23 THEN 'integer' WHEN 25 THEN 'text' WHEN 700 THEN 'real' "
      "WHEN 701 THEN 'double precision' WHEN 1042 THEN 'character' "
      "WHEN 1043 THEN 'character varying' WHEN 1082 THEN 'date' "
      "WHEN 1083 THEN 'time' WHEN 1266 THEN 'time with time zone' "
      "WHEN 1114 THEN 'timestamp' WHEN 1184 THEN 'timestamp with time zone' "
      "WHEN 1700 THEN 'numeric' ELSE types.typname END::text AS type_name, "
      "CASE types.oid WHEN 16 THEN 1 WHEN 17 THEN 1073741824 "
      "WHEN 18 THEN 1 WHEN 20 THEN 19 WHEN 21 THEN 5 WHEN 23 THEN 10 "
      "WHEN 25 THEN 1073741824 WHEN 700 THEN 7 WHEN 701 THEN 15 "
      "WHEN 1042 THEN 0 WHEN 1043 THEN 0 WHEN 1082 THEN 10 "
      "WHEN 1083 THEN 15 WHEN 1266 THEN 21 WHEN 1114 THEN 29 "
      "WHEN 1184 THEN 35 WHEN 1700 THEN 0 ELSE 0 END::integer "
      "AS column_size, CASE types.oid WHEN 16 THEN 1 WHEN 17 THEN 1073741824 "
      "WHEN 18 THEN 1 WHEN 20 THEN 8 WHEN 21 THEN 2 WHEN 23 THEN 4 "
      "WHEN 25 THEN 1073741824 WHEN 700 THEN 4 WHEN 701 THEN 8 "
      "WHEN 1042 THEN 0 WHEN 1043 THEN 0 WHEN 1082 THEN 10 "
      "WHEN 1083 THEN 15 WHEN 1266 THEN 21 WHEN 1114 THEN 29 "
      "WHEN 1184 THEN 35 WHEN 1700 THEN 0 ELSE 0 END::integer "
      "AS buffer_length, CASE types.oid WHEN 20 THEN 0 WHEN 21 THEN 0 "
      "WHEN 23 THEN 0 WHEN 700 THEN 6 WHEN 701 THEN 15 "
      "WHEN 1083 THEN 6 WHEN 1266 THEN 6 WHEN 1114 THEN 6 "
      "WHEN 1184 THEN 6 ELSE NULL END::smallint AS decimal_digits, "
      "CASE WHEN types.oid IN (20, 21, 23, 700, 701, 1700) THEN 10 "
      "ELSE NULL END::smallint AS num_prec_radix, 2::smallint AS nullable, "
      "NULL::text AS remarks, NULL::text AS column_def, "
      "CASE WHEN types.oid IN (1082, 1083, 1266, 1114, 1184) THEN 9 "
      "ELSE CASE types.oid WHEN 16 THEN -7 WHEN 17 THEN -3 "
      "WHEN 18 THEN 1 WHEN 20 THEN -5 WHEN 21 THEN 5 WHEN 23 THEN 4 "
      "WHEN 25 THEN -1 WHEN 700 THEN 7 WHEN 701 THEN 8 "
      "WHEN 1042 THEN 1 WHEN 1043 THEN 12 WHEN 1700 THEN 2 "
      "ELSE 12 END END::smallint AS sql_data_type, "
      "CASE types.oid WHEN 1082 THEN 1 WHEN 1083 THEN 2 WHEN 1266 THEN 2 "
      "WHEN 1114 THEN 3 WHEN 1184 THEN 3 ELSE NULL END::smallint "
      "AS sql_datetime_sub, CASE types.oid WHEN 17 THEN 1073741824 "
      "WHEN 18 THEN 1 WHEN 25 THEN 1073741824 WHEN 1042 THEN 0 "
      "WHEN 1043 THEN 0 ELSE NULL END::integer AS char_octet_length, "
      "columns.ordinal_position, ''::text AS is_nullable "
      "FROM routine_columns AS columns JOIN pg_catalog.pg_type AS types "
      "ON types.oid = columns.type_oid WHERE 1=1";
  if (catalog_name && !catalog_name->empty()) {
    query += " AND columns.procedure_cat LIKE " +
        quote_catalog_literal(*catalog_name);
  }
  if (schema_name && !schema_name->empty()) {
    query += " AND columns.procedure_schem LIKE " +
        quote_catalog_literal(*schema_name);
  }
  if (procedure_name && !procedure_name->empty()) {
    query += " AND columns.procedure_name LIKE " +
        quote_catalog_literal(*procedure_name);
  }
  if (column_name && !column_name->empty()) {
    query += " AND columns.column_name LIKE " +
        quote_catalog_literal(*column_name);
  }
  query +=
      " ORDER BY procedure_cat, procedure_schem, procedure_name, "
      "ordinal_position";
  return execute_direct(query);
}

SQLRETURN ODBCStatement::special_columns(
    SQLUSMALLINT identifier_type,
    const std::optional<std::string>& catalog_name,
    const std::optional<std::string>& schema_name,
    const std::string& table_name, bool require_non_nullable) {
  if (identifier_type == SQL_ROWVER) {
    return execute_direct(
        "SELECT 2::smallint AS scope, NULL::text AS column_name, "
        "0::smallint AS data_type, NULL::text AS type_name, "
        "0::integer AS column_size, 0::integer AS buffer_length, "
        "NULL::smallint AS decimal_digits, 2::smallint AS pseudo_column "
        "WHERE FALSE");
  }

  std::string query =
      "SELECT 2::smallint AS scope, keys.column_name::text AS column_name, "
      "CASE columns.data_type WHEN 'boolean' THEN -7 "
      "WHEN 'smallint' THEN 5 WHEN 'integer' THEN 4 WHEN 'bigint' THEN -5 "
      "WHEN 'real' THEN 7 WHEN 'double precision' THEN 8 "
      "WHEN 'numeric' THEN 2 WHEN 'decimal' THEN 3 "
      "WHEN 'character' THEN 1 WHEN 'character varying' THEN 12 "
      "WHEN 'text' THEN -1 WHEN 'bytea' THEN -3 WHEN 'date' THEN 91 "
      "WHEN 'time without time zone' THEN 92 "
      "WHEN 'time with time zone' THEN 92 "
      "WHEN 'timestamp without time zone' THEN 93 "
      "WHEN 'timestamp with time zone' THEN 93 ELSE 12 END::smallint "
      "AS data_type, CASE columns.data_type "
      "WHEN 'time without time zone' THEN 'time' "
      "WHEN 'timestamp without time zone' THEN 'timestamp' "
      "ELSE columns.data_type END::text AS type_name, "
      "CASE columns.data_type WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 10 WHEN 'bigint' THEN 19 WHEN 'real' THEN 7 "
      "WHEN 'double precision' THEN 15 "
      "WHEN 'numeric' THEN columns.numeric_precision "
      "WHEN 'decimal' THEN columns.numeric_precision "
      "WHEN 'character' THEN columns.character_maximum_length "
      "WHEN 'character varying' THEN columns.character_maximum_length "
      "WHEN 'text' THEN 1073741824 WHEN 'bytea' THEN 1073741824 "
      "WHEN 'date' THEN 10 WHEN 'time without time zone' THEN 15 "
      "WHEN 'time with time zone' THEN 21 "
      "WHEN 'timestamp without time zone' THEN 29 "
      "WHEN 'timestamp with time zone' THEN 35 ELSE 0 END::integer "
      "AS column_size, CASE columns.data_type "
      "WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 2 WHEN 'integer' THEN 4 "
      "WHEN 'bigint' THEN 8 WHEN 'real' THEN 4 "
      "WHEN 'double precision' THEN 8 "
      "WHEN 'numeric' THEN columns.numeric_precision + 2 "
      "WHEN 'decimal' THEN columns.numeric_precision + 2 "
      "WHEN 'character' THEN columns.character_octet_length "
      "WHEN 'character varying' THEN columns.character_octet_length "
      "WHEN 'text' THEN 1073741824 WHEN 'bytea' THEN 1073741824 "
      "WHEN 'date' THEN 10 WHEN 'time without time zone' THEN 15 "
      "WHEN 'time with time zone' THEN 21 "
      "WHEN 'timestamp without time zone' THEN 29 "
      "WHEN 'timestamp with time zone' THEN 35 ELSE 0 END::integer "
      "AS buffer_length, CASE WHEN columns.data_type IN "
      "('numeric', 'decimal') THEN columns.numeric_scale "
      "WHEN columns.data_type IN ('smallint', 'integer', 'bigint') THEN 0 "
      "ELSE NULL END::smallint AS decimal_digits, "
      "1::smallint AS pseudo_column "
      "FROM information_schema.table_constraints AS constraints "
      "JOIN information_schema.key_column_usage AS keys "
      "ON constraints.constraint_catalog = keys.constraint_catalog "
      "AND constraints.constraint_schema = keys.constraint_schema "
      "AND constraints.constraint_name = keys.constraint_name "
      "JOIN information_schema.columns AS columns "
      "ON columns.table_catalog = keys.table_catalog "
      "AND columns.table_schema = keys.table_schema "
      "AND columns.table_name = keys.table_name "
      "AND columns.column_name = keys.column_name "
      "WHERE constraints.constraint_type = 'PRIMARY KEY' "
      "AND keys.table_name = " + quote_catalog_literal(table_name);
  if (catalog_name && !catalog_name->empty()) {
    query += " AND keys.table_catalog = " +
        quote_catalog_literal(*catalog_name);
  }
  if (schema_name && !schema_name->empty()) {
    query += " AND keys.table_schema = " +
        quote_catalog_literal(*schema_name);
  }
  if (require_non_nullable) query += " AND columns.is_nullable = 'NO'";
  query += " ORDER BY keys.ordinal_position";
  return execute_direct(query);
}

SQLRETURN ODBCStatement::row_count(SQLLEN* row_count_value) {
  if (!row_count_value) {
    set_error(SQLSTATE_INVALID_NULL_POINTER, "Null pointer for row count");
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
  if (name_buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Invalid column-name buffer length");
    return SQL_ERROR;
  }
  if (column_number < 1 || column_number > column_info_.size()) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid column number");
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
  if (field_identifier == SQL_DESC_NAME && buffer_length < 0) {
    set_error(SQLSTATE_INVALID_STRING_LENGTH,
              "Invalid column-attribute buffer length");
    return SQL_ERROR;
  }
  if (column_number < 1 || column_number > column_info_.size()) {
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid column number");
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
    set_error(SQLSTATE_INVALID_PARAMETER_NUMBER, "Invalid parameter number");
    return SQL_ERROR;
  }
  
  const auto& meta = param_metadata_[parameter_number - 1];
  if (data_type) *data_type = meta.sql_type;
  if (parameter_size) *parameter_size = meta.column_size;
  if (decimal_digits) *decimal_digits = meta.decimal_digits;
  if (nullable) *nullable = meta.nullable;
  
  return SQL_SUCCESS;
}

// Handle registry implementation
HandleRegistry& HandleRegistry::instance() {
  static HandleRegistry registry;
  return registry;
}

void HandleRegistry::register_handle(SQLHANDLE handle,
                                     std::unique_ptr<ODBCHandle> obj,
                                     SQLHANDLE parent) {
  std::lock_guard lock(mutex_);
  handles_[handle] = {
      std::shared_ptr<ODBCHandle>(std::move(obj)), parent};
}

void HandleRegistry::unregister_handle(SQLHANDLE handle) {
  std::vector<std::shared_ptr<ODBCHandle>> removed;
  {
    std::lock_guard lock(mutex_);
    collect_subtree_locked(handle, removed);
  }
}

void HandleRegistry::unregister_children(SQLHANDLE parent) {
  std::vector<std::shared_ptr<ODBCHandle>> removed;
  {
    std::lock_guard lock(mutex_);
    std::vector<SQLHANDLE> children;
    for (const auto& [handle, entry] : handles_) {
      if (entry.parent == parent) children.push_back(handle);
    }
    for (const auto child : children) {
      collect_subtree_locked(child, removed);
    }
  }
}

bool HandleRegistry::has_children(SQLHANDLE parent) {
  std::lock_guard lock(mutex_);
  for (const auto& [handle, entry] : handles_) {
    static_cast<void>(handle);
    if (entry.parent == parent) return true;
  }
  return false;
}

void HandleRegistry::collect_subtree_locked(
    SQLHANDLE handle, std::vector<std::shared_ptr<ODBCHandle>>& removed) {
  std::vector<SQLHANDLE> children;
  for (const auto& [candidate, entry] : handles_) {
    if (entry.parent == handle) children.push_back(candidate);
  }
  for (const auto child : children) {
    collect_subtree_locked(child, removed);
  }
  const auto it = handles_.find(handle);
  if (it == handles_.end()) return;
  removed.push_back(std::move(it->second.object));
  handles_.erase(it);
}

std::shared_ptr<ODBCHandle> HandleRegistry::get_handle(SQLHANDLE handle) {
  std::lock_guard lock(mutex_);
  auto it = handles_.find(handle);
  return (it != handles_.end()) ? it->second.object : nullptr;
}

std::shared_ptr<ODBCConnection> HandleRegistry::get_connection_for_handle(
    SQLHANDLE handle) {
  std::lock_guard lock(mutex_);
  auto current = handle;
  while (current) {
    const auto it = handles_.find(current);
    if (it == handles_.end()) return nullptr;
    if (it->second.object->get_type() == HandleType::Connection) {
      return std::static_pointer_cast<ODBCConnection>(it->second.object);
    }
    current = it->second.parent;
  }
  return nullptr;
}

void HandleRegistry::detach_descriptor_from_statements(SQLHDESC descriptor) {
  std::vector<std::shared_ptr<ODBCStatement>> statements;
  {
    std::lock_guard lock(mutex_);
    const auto descriptor_entry = handles_.find(descriptor);
    if (descriptor_entry == handles_.end()) return;
    const auto connection = descriptor_entry->second.parent;
    for (const auto& [handle, entry] : handles_) {
      static_cast<void>(handle);
      if (entry.object->get_type() != HandleType::Statement ||
          entry.parent != connection) {
        continue;
      }
      statements.push_back(
          std::static_pointer_cast<ODBCStatement>(entry.object));
    }
  }
  for (const auto& statement : statements) {
    statement->detach_descriptor(descriptor);
  }
}

HandleOperationLease HandleRegistry::lock_handles(
    std::initializer_list<SQLHANDLE> handles) {
  HandleOperationLease lease;
  std::map<SQLHANDLE, std::shared_ptr<ODBCHandle>> ordered_handles;
  {
    std::lock_guard lock(mutex_);
    for (const auto requested_handle : handles) {
      auto current = requested_handle;
      while (current) {
        const auto it = handles_.find(current);
        if (it == handles_.end()) break;
        const auto [inserted, is_new] =
            ordered_handles.emplace(current, it->second.object);
        static_cast<void>(inserted);
        if (!is_new) break;
        if (it->second.object->get_type() == HandleType::Connection) break;
        current = it->second.parent;
      }
    }
  }

  lease.handles_.reserve(ordered_handles.size());
  lease.locks_.reserve(ordered_handles.size());
  for (auto& [handle, object] : ordered_handles) {
    static_cast<void>(handle);
    lease.handles_.push_back(object);
    lease.locks_.emplace_back(object->operation_mutex_);
  }
  return lease;
}

} // namespace rs::odbc
