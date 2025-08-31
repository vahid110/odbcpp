#include "database_factory.h"
#include "generic_database_connection.h"
#include "postgres/pg_protocol_parser.h"
#include "core/transport/socket_transport.h"
#include "core/transport/tls_transport.h"

namespace rs::core::database {

std::unique_ptr<IDatabaseConnection> DatabaseFactory::create_connection(DatabaseType type) {
  switch (type) {
    case DatabaseType::PostgreSQL:
    case DatabaseType::Redshift:
      return std::make_unique<GenericDatabaseConnection>(
        std::make_unique<postgres::PgProtocolParser>());
    
    case DatabaseType::MySQL:
      // Future: return MySQL-specific parser
      throw std::runtime_error("MySQL support not implemented yet");
    
    case DatabaseType::SQLServer:
      // Future: return SQL Server-specific parser  
      throw std::runtime_error("SQL Server support not implemented yet");
    
    default:
      throw std::runtime_error("Unknown database type");
  }
}

DatabaseType DatabaseFactory::detect_from_port(uint16_t port) {
  switch (port) {
    case 5432: return DatabaseType::PostgreSQL;
    case 5439: return DatabaseType::Redshift;
    case 3306: return DatabaseType::MySQL;
    case 1433: return DatabaseType::SQLServer;
    default: return DatabaseType::PostgreSQL; // Default fallback
  }
}

DatabaseType DatabaseFactory::detect_from_url(const std::string& connection_url) {
  if (connection_url.starts_with("postgresql://") || 
      connection_url.starts_with("postgres://")) {
    return DatabaseType::PostgreSQL;
  }
  if (connection_url.starts_with("redshift://")) {
    return DatabaseType::Redshift;
  }
  if (connection_url.starts_with("mysql://")) {
    return DatabaseType::MySQL;
  }
  if (connection_url.starts_with("sqlserver://")) {
    return DatabaseType::SQLServer;
  }
  return DatabaseType::PostgreSQL; // Default
}

} // namespace rs::core::database