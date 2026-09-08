#include "core/database/async_database_connection.h"
#include "core/database/database_factory.h"
#include "core/transport/thread_pool_transport.h"
#include "core/util/deadline.h"
#include "core/util/exception_adapter.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>

using namespace rs::core::database;
using namespace rs::core::transport;

void callback_example() {
  std::cout << "\n=== Callback-based Async Example ===\n";
  
  // Create async transport and connection
  auto transport = std::make_unique<ThreadPoolTransport>(4);
  auto async_conn = std::make_unique<AsyncDatabaseConnection>(nullptr, std::move(transport));
  
  ConnectionSettings settings;
  settings.host = "localhost";
  settings.port = 5432;
  settings.database = "postgres";
  settings.user = "postgres";
  settings.password = "postgres";
  settings.use_ssl = false;
  settings.timeout = std::chrono::seconds(10);
  
  std::atomic<bool> connect_done{false};
  std::atomic<bool> query_done{false};
  
  // Async connect with callback
  auto connect_op = async_conn->connect_async(settings, 
    [&](rs::util::Result<void> result) {
      if (result.has_value()) {
        std::cout << "✅ Connected successfully!\n";
        
        // Chain async query after successful connect
        auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
        auto query_op = async_conn->execute_query_async("SELECT version()", deadline,
          [&](rs::util::Result<QueryResult> query_result) {
            if (query_result.has_value()) {
              std::cout << "✅ Query successful!\n";
              if (!query_result->rows.empty() && !query_result->rows[0].empty()) {
                const auto& version = query_result->rows[0][0];
                std::cout << "Version: " << (version ? *version : "NULL") << "\n";
              }
            } else {
              std::cout << "❌ Query failed: " << query_result.error_message() << "\n";
            }
            query_done.store(true);
          });
        
      } else {
        std::cout << "❌ Connect failed: " << result.error_message() << "\n";
        query_done.store(true); // Skip query
      }
      connect_done.store(true);
    });
  
  // Wait for operations to complete
  while (!connect_done.load() || !query_done.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  
  std::cout << "Callback example completed\n";
}

void future_example() {
  std::cout << "\n=== Future-based Async Example ===\n";
  
  auto transport = std::make_unique<ThreadPoolTransport>(4);
  auto async_conn = std::make_unique<AsyncDatabaseConnection>(nullptr, std::move(transport));
  
  ConnectionSettings settings;
  settings.host = "localhost";
  settings.port = 5432;
  settings.database = "postgres";
  settings.user = "postgres";
  settings.password = "postgres";
  settings.use_ssl = false;
  settings.timeout = std::chrono::seconds(10);
  
  try {
    // Async connect
    auto connect_future = async_conn->connect_future(settings);
    
    std::cout << "Connecting asynchronously...\n";
    auto connect_result = connect_future.get();
    
    if (connect_result.has_error()) {
      std::cout << "❌ Connect failed: " << connect_result.error_message() << "\n";
      return;
    }
    
    std::cout << "✅ Connected successfully!\n";
    
    // Multiple concurrent queries
    std::vector<std::future<rs::util::Result<QueryResult>>> query_futures;
    auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
    
    query_futures.push_back(async_conn->execute_query_future("SELECT 1", deadline));
    query_futures.push_back(async_conn->execute_query_future("SELECT 2", deadline));
    query_futures.push_back(async_conn->execute_query_future("SELECT 3", deadline));
    
    std::cout << "Executing 3 concurrent queries...\n";
    
    // Wait for all queries
    for (size_t i = 0; i < query_futures.size(); ++i) {
      auto result = query_futures[i].get();
      if (result.has_value()) {
        const auto& value = result->rows[0][0];
        std::cout << "✅ Query " << (i+1) << " result: "
                  << (value ? *value : "NULL") << "\n";
      } else {
        std::cout << "❌ Query " << (i+1) << " failed: " << result.error_message() << "\n";
      }
    }
    
  } catch (const std::exception& e) {
    std::cout << "❌ Exception: " << e.what() << "\n";
  }
  
  std::cout << "Future example completed\n";
}

void concurrent_connections_example() {
  std::cout << "\n=== Concurrent Connections Example ===\n";
  
  const int num_connections = 4;
  const int queries_per_connection = 3;
  
  std::vector<std::thread> workers;
  std::atomic<int> successful_operations{0};
  
  for (int i = 0; i < num_connections; ++i) {
    workers.emplace_back([i, &successful_operations]() {
      const int queries_per_connection = 3;
      auto transport = std::make_unique<ThreadPoolTransport>(2);
      auto async_conn = std::make_unique<AsyncDatabaseConnection>(nullptr, std::move(transport));
      
      ConnectionSettings settings;
      settings.host = "localhost";
      settings.port = 5432;
      settings.database = "postgres";
      settings.user = "postgres";
      settings.password = "postgres";
      settings.timeout = std::chrono::seconds(10);
      
      try {
        // Connect
        auto connect_result = async_conn->connect_future(settings).get();
        if (connect_result.has_error()) {
          std::cout << "Worker " << i << " connect failed\n";
          return;
        }
        
        // Execute multiple queries concurrently
        std::vector<std::future<rs::util::Result<QueryResult>>> futures;
        auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
        
        for (int j = 0; j < queries_per_connection; ++j) {
          std::string sql = "SELECT " + std::to_string(i * 100 + j);
          futures.push_back(async_conn->execute_query_future(sql, deadline));
        }
        
        // Wait for all queries
        for (auto& future : futures) {
          auto result = future.get();
          if (result.has_value()) {
            successful_operations++;
          }
        }
        
        std::cout << "Worker " << i << " completed\n";
        
      } catch (const std::exception& e) {
        std::cout << "Worker " << i << " exception: " << e.what() << "\n";
      }
    });
  }
  
  // Wait for all workers
  for (auto& worker : workers) {
    worker.join();
  }
  
  std::cout << "Concurrent example completed. Successful operations: " 
            << successful_operations.load() << "/" << (num_connections * queries_per_connection) << "\n";
}

int main(int argc, char** argv) {
  std::cout << "🚀 Async Database Connection Examples\n";
  std::cout << "=====================================\n";
  
  try {
    callback_example();
    future_example();
    concurrent_connections_example();
    
    std::cout << "\n✅ All async examples completed successfully!\n";
    return 0;
    
  } catch (const std::exception& e) {
    std::cerr << "❌ ERROR: " << e.what() << "\n";
    return 1;
  }
}
