#include "core/database/database_factory.h"
#include "core/util/deadline.h"
#include <iostream>
#include <string>

using namespace rs::core::database;

int main() {
  try {
    // Create connection for the compiled database type
    auto conn = DatabaseFactory::create_connection();
    
    // Show which database this build supports
    auto db_type = DatabaseFactory::get_compiled_database_type();
    std::cout << "This build supports: ";
    switch (db_type) {
      case DatabaseType::PostgreSQL: std::cout << "PostgreSQL"; break;
      case DatabaseType::Redshift: std::cout << "Redshift"; break;
      case DatabaseType::MySQL: std::cout << "MySQL"; break;
      case DatabaseType::SQLServer: std::cout << "SQL Server"; break;
    }
    std::cout << std::endl;
    
    // Configure connection (example for Redshift)
    ConnectionSettings settings;
    settings.host = "localhost";
    settings.port = 5439;
    settings.user = "admin";
    settings.password = "password";
    settings.database = "dev";
    
    conn->connect(settings);
    
    auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
    auto result = conn->execute_query("SELECT version()", deadline);
    
    std::cout << "Database version: " << result.rows[0][0] << std::endl;
    
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  
  return 0;
}