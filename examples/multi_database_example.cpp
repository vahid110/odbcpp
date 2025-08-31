#include "core/database/database_factory.h"
#include "core/util/deadline.h"
#include <iostream>
#include <string>

using namespace rs::core::database;

int main() {
  try {
    // Example 1: Connect to Redshift
    {
      auto conn = DatabaseFactory::create_connection(DatabaseType::Redshift);
      
      ConnectionSettings settings;
      settings.host = "redshift-cluster.amazonaws.com";
      settings.port = 5439;
      settings.user = "admin";
      settings.password = "password";
      settings.database = "dev";
      
      conn->connect(settings);
      
      auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
      auto result = conn->execute_query("SELECT version()", deadline);
      
      std::cout << "Redshift version: " << result.rows[0][0] << std::endl;
    }
    
    // Example 2: Connect to PostgreSQL  
    {
      auto conn = DatabaseFactory::create_connection(DatabaseType::PostgreSQL);
      
      ConnectionSettings settings;
      settings.host = "localhost";
      settings.port = 5432;
      settings.user = "postgres";
      settings.password = "password";
      settings.database = "postgres";
      
      conn->connect(settings);
      
      auto deadline = rs::util::make_deadline(std::chrono::seconds(30));
      auto result = conn->execute_query("SELECT 1", deadline);
      
      std::cout << "PostgreSQL result: " << result.rows[0][0] << std::endl;
    }
    
    // Example 3: Auto-detect database type from URL
    {
      std::string url = "postgresql://user:pass@localhost:5432/mydb";
      auto type = DatabaseFactory::detect_from_url(url);
      auto conn = DatabaseFactory::create_connection(type);
      
      std::cout << "Detected database type from URL" << std::endl;
    }
    
    // Example 4: Auto-detect from port
    {
      auto type = DatabaseFactory::detect_from_port(5439);
      auto conn = DatabaseFactory::create_connection(type);
      
      std::cout << "Detected Redshift from port 5439" << std::endl;
    }
    
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  
  return 0;
}