#pragma once
#include "i_database_connection.h"
#include <memory>
#include <string>

namespace rs::core::database {

enum class DatabaseType {
  PostgreSQL,
  Redshift,
  MySQL,
  SQLServer
};

class DatabaseFactory {
public:
  static std::unique_ptr<IDatabaseConnection> create_connection(DatabaseType type);
  static DatabaseType detect_from_url(const std::string& connection_url);
  static DatabaseType detect_from_port(uint16_t port);
};

} // namespace rs::core::database