#include "database_factory.h"

namespace rs::core::database {

DatabaseType DatabaseFactory::detect_from_port(uint16_t port) {
  switch (port) {
    case 5432: return DatabaseType::PostgreSQL;
    case 5439: return DatabaseType::Redshift;
    case 3306: return DatabaseType::MySQL;
    case 1433: return DatabaseType::SQLServer;
    default: return DatabaseType::PostgreSQL;
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
  return DatabaseType::PostgreSQL;
}

} // namespace rs::core::database
