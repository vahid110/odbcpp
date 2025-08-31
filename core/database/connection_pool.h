#pragma once
#include "i_database_connection.h"
#include "core/util/result.h"
#include <memory>
#include <vector>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

namespace rs::core::database {

// Thread-safe wrapper for database connections
class ThreadSafeConnection {
public:
  explicit ThreadSafeConnection(std::unique_ptr<IDatabaseConnection> conn);
  
  rs::util::Result<void> connect(const ConnectionSettings& settings);
  void disconnect();
  bool is_connected() const;
  
  rs::util::Result<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline);
  rs::util::Result<QueryResult> execute_prepared(std::string_view sql, 
                                                std::span<const std::string> params,
                                                rs::util::Deadline deadline);
  
  std::string get_parameter(std::string_view key) const;
  std::string get_last_error() const;

private:
  std::unique_ptr<IDatabaseConnection> conn_;
  mutable std::shared_mutex mutex_;
};

// Connection pool for managing multiple database connections
class ConnectionPool {
public:
  struct Config {
    size_t min_connections = 1;
    size_t max_connections = 10;
    std::chrono::milliseconds acquire_timeout{5000};
    std::chrono::milliseconds idle_timeout{300000}; // 5 minutes
    ConnectionSettings connection_settings;
  };
  
  explicit ConnectionPool(Config config);
  ~ConnectionPool();
  
  // Acquire a connection from the pool
  rs::util::Result<std::shared_ptr<ThreadSafeConnection>> acquire();
  
  // Release a connection back to the pool
  void release(std::shared_ptr<ThreadSafeConnection> conn);
  
  // Get pool statistics
  struct Stats {
    size_t total_connections;
    size_t available_connections;
    size_t active_connections;
  };
  Stats get_stats() const;
  
  // Shutdown the pool
  void shutdown();

private:
  Config config_;
  std::queue<std::shared_ptr<ThreadSafeConnection>> available_;
  std::vector<std::weak_ptr<ThreadSafeConnection>> all_connections_;
  
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> shutdown_{false};
  
  rs::util::Result<std::shared_ptr<ThreadSafeConnection>> create_connection();
  void cleanup_expired_connections();
};

} // namespace rs::core::database