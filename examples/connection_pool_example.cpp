#include "core/database/connection_pool.h"
#include "core/util/deadline.h"
#include "core/util/exception_adapter.h"
#include <iostream>
#include <thread>
#include <vector>
#include <chrono>

using namespace rs::core::database;

void worker_thread(ConnectionPool& pool, int worker_id, int num_queries) {
  std::cout << "Worker " << worker_id << " starting...\n";
  
  for (int i = 0; i < num_queries; ++i) {
    try {
      // Acquire connection from pool
      auto conn_result = pool.acquire();
      if (conn_result.has_error()) {
        std::cout << "Worker " << worker_id << " failed to acquire connection: " 
                  << conn_result.error_message() << "\n";
        continue;
      }
      
      auto conn = *conn_result;
      std::cout << "Worker " << worker_id << " acquired connection for query " << i << "\n";
      
      // Execute query
      auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
      auto result = conn->execute_query("SELECT " + std::to_string(worker_id * 100 + i), deadline);
      
      if (result.has_value()) {
        std::cout << "Worker " << worker_id << " query " << i << " succeeded\n";
      } else {
        std::cout << "Worker " << worker_id << " query " << i << " failed: " 
                  << result.error_message() << "\n";
      }
      
      // Simulate some work
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      
      // Release connection back to pool
      pool.release(conn);
      std::cout << "Worker " << worker_id << " released connection\n";
      
    } catch (const std::exception& e) {
      std::cout << "Worker " << worker_id << " exception: " << e.what() << "\n";
    }
  }
  
  std::cout << "Worker " << worker_id << " finished\n";
}

int main(int argc, char** argv) {
  try {
    // Configure connection pool
    ConnectionPool::Config config;
    config.min_connections = 2;
    config.max_connections = 5;
    config.acquire_timeout = std::chrono::seconds(10);
    
    // Connection settings
    config.connection_settings.host = "localhost";
    config.connection_settings.port = 5432;
    config.connection_settings.database = "postgres";
    config.connection_settings.user = "postgres";
    config.connection_settings.password = "postgres";
    config.connection_settings.use_ssl = false;
    config.connection_settings.timeout = std::chrono::seconds(30);
    
    std::cout << "Creating connection pool...\n";
    ConnectionPool pool(config);
    
    // Show initial stats
    auto stats = pool.get_stats();
    std::cout << "Pool stats - Total: " << stats.total_connections 
              << ", Available: " << stats.available_connections
              << ", Active: " << stats.active_connections << "\n";
    
    // Create worker threads
    const int num_workers = 4;
    const int queries_per_worker = 3;
    std::vector<std::thread> workers;
    
    std::cout << "Starting " << num_workers << " worker threads...\n";
    
    for (int i = 0; i < num_workers; ++i) {
      workers.emplace_back(worker_thread, std::ref(pool), i, queries_per_worker);
    }
    
    // Monitor pool stats
    std::thread monitor([&pool]() {
      for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto stats = pool.get_stats();
        std::cout << "[Monitor] Pool stats - Total: " << stats.total_connections 
                  << ", Available: " << stats.available_connections
                  << ", Active: " << stats.active_connections << "\n";
      }
    });
    
    // Wait for all workers to complete
    for (auto& worker : workers) {
      worker.join();
    }
    
    monitor.join();
    
    // Final stats
    stats = pool.get_stats();
    std::cout << "Final pool stats - Total: " << stats.total_connections 
              << ", Available: " << stats.available_connections
              << ", Active: " << stats.active_connections << "\n";
    
    std::cout << "Connection pool example completed successfully!\n";
    return 0;
    
  } catch (const std::exception& e) {
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
  }
}