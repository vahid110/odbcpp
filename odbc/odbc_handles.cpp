#include "odbc_handles.h"
#include "connection_string.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/thread_pool_transport.h"
#include "core/util/deadline.h"
#include <mutex>

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
  
  // Essential Redshift data type conversions
  switch (target_type) {
    case SQL_C_CHAR: {
      size_t copy_len = std::min(static_cast<size_t>(buffer_length - 1), value.length());
      std::memcpy(buffer, value.c_str(), copy_len);
      static_cast<char*>(buffer)[copy_len] = '\0';
      if (indicator) *indicator = static_cast<SQLLEN>(value.length());
      return SQL_SUCCESS;
    }
    
    case SQL_C_SLONG: {
      try {
        SQLINTEGER result = static_cast<SQLINTEGER>(std::stol(value));
        *static_cast<SQLINTEGER*>(buffer) = result;
        if (indicator) *indicator = sizeof(SQLINTEGER);
        return SQL_SUCCESS;
      } catch (...) {
        set_error(SQLSTATE_GENERAL_ERROR, "Invalid integer value");
        return SQL_ERROR;
      }
    }
    
    case SQL_C_SBIGINT: {
      try {
        SQLBIGINT result = static_cast<SQLBIGINT>(std::stoll(value));
        *static_cast<SQLBIGINT*>(buffer) = result;
        if (indicator) *indicator = sizeof(SQLBIGINT);
        return SQL_SUCCESS;
      } catch (...) {
        set_error(SQLSTATE_GENERAL_ERROR, "Invalid bigint value");
        return SQL_ERROR;
      }
    }
    
    case SQL_C_DOUBLE: {
      try {
        SQLDOUBLE result = std::stod(value);
        *static_cast<SQLDOUBLE*>(buffer) = result;
        if (indicator) *indicator = sizeof(SQLDOUBLE);
        return SQL_SUCCESS;
      } catch (...) {
        set_error(SQLSTATE_GENERAL_ERROR, "Invalid double value");
        return SQL_ERROR;
      }
    }
    
    case SQL_C_BIT: {
      SQLCHAR result = (value == "t" || value == "true" || value == "1") ? 1 : 0;
      *static_cast<SQLCHAR*>(buffer) = result;
      if (indicator) *indicator = sizeof(SQLCHAR);
      return SQL_SUCCESS;
    }
    
    default:
      set_error(SQLSTATE_GENERAL_ERROR, "Unsupported data type conversion");
      return SQL_ERROR;
  }
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