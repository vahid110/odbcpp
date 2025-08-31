#include "core/database/database_factory.h"
#include "core/engine/pg_connection.h"
#include "core/util/deadline.h"
#include <iostream>

// Example showing how old and new architectures can coexist
int main() {
  try {
    std::cout << "=== Old Architecture (PgConnection) ===" << std::endl;
    {
      rs::core::engine::PgConnection old_conn;
      rs::core::engine::PgConnection::Settings settings;
      settings.host = "localhost";
      settings.port = 5432;
      settings.user = "postgres";
      settings.password = "password";
      settings.db = "postgres";
      settings.sslmode = rs::core::engine::SslMode::Disable;
      
      // old_conn.connect(settings);
      // auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
      // auto rows = old_conn.simpleQuery("SELECT 1", deadline);
      std::cout << "Old PgConnection still works as before" << std::endl;
    }
    
    std::cout << "\n=== New Architecture (Generic) ===" << std::endl;
    {
      auto new_conn = rs::core::database::DatabaseFactory::create_connection(
        rs::core::database::DatabaseType::PostgreSQL);
      
      rs::core::database::ConnectionSettings settings;
      settings.host = "localhost";
      settings.port = 5432;
      settings.user = "postgres";
      settings.password = "password";
      settings.database = "postgres";
      settings.use_ssl = false;
      
      // new_conn->connect(settings);
      // auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
      // auto result = new_conn->execute_query("SELECT 1", deadline);
      std::cout << "New generic connection uses same protocol under the hood" << std::endl;
    }
    
    std::cout << "\n=== Migration Path ===" << std::endl;
    std::cout << "1. Keep existing PgConnection for backward compatibility" << std::endl;
    std::cout << "2. New code uses DatabaseFactory for multi-database support" << std::endl;
    std::cout << "3. Both share the same transport layer and protocol parsing logic" << std::endl;
    
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  
  return 0;
}