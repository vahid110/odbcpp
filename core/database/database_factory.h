#pragma once
#include "i_database_connection.h"
#include "core/transport/i_transport.h"
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
  // Create connection for the compiled database type
  static std::unique_ptr<IDatabaseConnection> create_connection();
  
  // Create connection with explicit type (throws if not compiled in)
  static std::unique_ptr<IDatabaseConnection> create_connection(DatabaseType type);

  // Transfer a configured transport to the selected backend. nullptr retains
  // the backend's default transport selection at connect time.
  static std::unique_ptr<IDatabaseConnection> create_connection(
      std::unique_ptr<rs::core::transport::ITransport> transport);
  static std::unique_ptr<IDatabaseConnection> create_connection(
      DatabaseType type,
      std::unique_ptr<rs::core::transport::ITransport> transport);
  
  // Get the database type this build was compiled for
  static DatabaseType get_compiled_database_type();
  
  // Utility functions
  static DatabaseType detect_from_url(const std::string& connection_url);
  static DatabaseType detect_from_port(uint16_t port);
};

// Forward declaration
class ConnectionPool;

} // namespace rs::core::database
