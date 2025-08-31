#include "database_factory.h"
#include "generic_database_connection.h"

#if defined(ODBCPP_ENABLE_POSTGRESQL) || defined(ODBCPP_ENABLE_REDSHIFT)
#include "postgres/pg_protocol_parser.h"
#endif

#ifdef ODBCPP_ENABLE_MYSQL
#include "mysql/mysql_protocol_parser.h"
#endif

#ifdef ODBCPP_ENABLE_SQLSERVER
#include "sqlserver/sqlserver_protocol_parser.h"
#endif

namespace rs::core::database {

std::unique_ptr<IDatabaseConnection> DatabaseFactory::create_connection(DatabaseType type) {
#ifdef ODBCPP_ENABLE_POSTGRESQL
  if (type == DatabaseType::PostgreSQL) {
    return std::make_unique<GenericDatabaseConnection>(
      std::make_unique<postgres::PgProtocolParser>());
  }
#endif

#ifdef ODBCPP_ENABLE_REDSHIFT
  if (type == DatabaseType::Redshift) {
    return std::make_unique<GenericDatabaseConnection>(
      std::make_unique<postgres::PgProtocolParser>());
  }
#endif

#ifdef ODBCPP_ENABLE_MYSQL
  if (type == DatabaseType::MySQL) {
    return std::make_unique<GenericDatabaseConnection>(
      std::make_unique<mysql::MySQLProtocolParser>());
  }
#endif

#ifdef ODBCPP_ENABLE_SQLSERVER
  if (type == DatabaseType::SQLServer) {
    return std::make_unique<GenericDatabaseConnection>(
      std::make_unique<sqlserver::SQLServerProtocolParser>());
  }
#endif

  throw std::runtime_error("Database type not supported in this build");
}

std::unique_ptr<IDatabaseConnection> DatabaseFactory::create_connection() {
  return create_connection(get_compiled_database_type());
}

DatabaseType DatabaseFactory::get_compiled_database_type() {
#ifdef ODBCPP_ENABLE_POSTGRESQL
  return DatabaseType::PostgreSQL;
#elif defined(ODBCPP_ENABLE_REDSHIFT)
  return DatabaseType::Redshift;
#elif defined(ODBCPP_ENABLE_MYSQL)
  return DatabaseType::MySQL;
#elif defined(ODBCPP_ENABLE_SQLSERVER)
  return DatabaseType::SQLServer;
#else
  #error "No database type enabled"
#endif
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