#include "connection_pool.h"
#include "database_factory.h"
#include <algorithm>

namespace rs::core::database {

// ThreadSafeConnection implementation
ThreadSafeConnection::ThreadSafeConnection(std::unique_ptr<IDatabaseConnection> conn)
  : conn_(std::move(conn)) {}

rs::util::Result<void> ThreadSafeConnection::connect(const ConnectionSettings& settings) {
  std::unique_lock lock(mutex_);
  return conn_->connect(settings);
}

void ThreadSafeConnection::disconnect() {
  std::unique_lock lock(mutex_);
  conn_->disconnect();
}

bool ThreadSafeConnection::is_connected() const {
  std::shared_lock lock(mutex_);
  return conn_->is_connected();
}

rs::util::Result<QueryResult> ThreadSafeConnection::execute_query(std::string_view sql, rs::util::Deadline deadline) {
  std::shared_lock lock(mutex_);
  return conn_->execute_query(sql, deadline);
}

rs::util::Result<QueryResult> ThreadSafeConnection::execute_prepared(std::string_view sql, 
                                                                    std::span<const QueryParameter> params,
                                                                    rs::util::Deadline deadline) {
  std::shared_lock lock(mutex_);
  return conn_->execute_prepared(sql, params, deadline);
}

std::string ThreadSafeConnection::get_parameter(std::string_view key) const {
  std::shared_lock lock(mutex_);
  return conn_->get_parameter(key);
}

std::string ThreadSafeConnection::get_last_error() const {
  std::shared_lock lock(mutex_);
  return conn_->get_last_error();
}

// ConnectionPool implementation
ConnectionPool::ConnectionPool(Config config) : config_(std::move(config)) {
  // Validate configuration
  if (config_.max_connections == 0) {
    throw std::invalid_argument("max_connections must be greater than 0");
  }
  if (config_.min_connections > config_.max_connections) {
    throw std::invalid_argument("min_connections cannot exceed max_connections");
  }
  
  // Create minimum connections
  for (size_t i = 0; i < config_.min_connections; ++i) {
    auto conn_result = create_connection();
    if (conn_result.has_value()) {
      available_.push(*conn_result);
      all_connections_.emplace_back(*conn_result);
    }
  }
}

ConnectionPool::~ConnectionPool() {
  shutdown();
}

rs::util::Result<std::shared_ptr<ThreadSafeConnection>> ConnectionPool::acquire() {
  std::unique_lock lock(mutex_);
  
  if (shutdown_.load()) {
    return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{
      rs::util::DbErrorCode::InvalidParameter, "Pool is shutdown"
    };
  }
  
  // Wait for available connection or timeout
  auto deadline = std::chrono::steady_clock::now() + config_.acquire_timeout;
  
  while (available_.empty() && all_connections_.size() >= config_.max_connections) {
    if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
      return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{
        rs::util::DbErrorCode::Timeout, "Timeout acquiring connection from pool"
      };
    }
    
    if (shutdown_.load()) {
      return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{
        rs::util::DbErrorCode::InvalidParameter, "Pool is shutdown"
      };
    }
  }
  
  // Return available connection
  if (!available_.empty()) {
    auto conn = available_.front();
    available_.pop();
    return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{conn};
  }
  
  // Create new connection if under max limit
  if (all_connections_.size() < config_.max_connections) {
    auto conn_result = create_connection();
    if (conn_result.has_value()) {
      all_connections_.emplace_back(*conn_result);
      return conn_result;
    }
    return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{
      conn_result.error(), conn_result.error_message()
    };
  }
  
  return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{
    rs::util::DbErrorCode::ConnectionFailed, "Unable to acquire connection"
  };
}

void ConnectionPool::release(std::shared_ptr<ThreadSafeConnection> conn) {
  if (!conn || shutdown_.load()) {
    return;
  }
  
  std::lock_guard lock(mutex_);
  
  // Only return healthy connections to pool
  if (conn->is_connected()) {
    available_.push(conn);
    cv_.notify_one();
  } else {
    // Remove unhealthy connection from tracking
    all_connections_.erase(
      std::remove_if(all_connections_.begin(), all_connections_.end(),
        [&conn](const std::weak_ptr<ThreadSafeConnection>& weak_conn) {
          return weak_conn.expired() || weak_conn.lock() == conn;
        }),
      all_connections_.end()
    );
  }
}

ConnectionPool::Stats ConnectionPool::get_stats() const {
  std::lock_guard lock(mutex_);
  
  // Count non-expired connections
  size_t valid_connections = 0;
  for (const auto& weak_conn : all_connections_) {
    if (!weak_conn.expired()) {
      ++valid_connections;
    }
  }
  
  return Stats{
    .total_connections = valid_connections,
    .available_connections = available_.size(),
    .active_connections = valid_connections - available_.size()
  };
}

void ConnectionPool::shutdown() {
  shutdown_.store(true);
  
  std::lock_guard lock(mutex_);
  cv_.notify_all();
  
  // Disconnect all connections
  while (!available_.empty()) {
    auto conn = available_.front();
    available_.pop();
    if (conn) {
      conn->disconnect();
    }
  }
  
  all_connections_.clear();
}

rs::util::Result<std::shared_ptr<ThreadSafeConnection>> ConnectionPool::create_connection() {
  auto raw_conn = DatabaseFactory::create_connection();
  if (!raw_conn) {
    return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{
      rs::util::DbErrorCode::ConnectionFailed, "Failed to create database connection"
    };
  }
  
  auto thread_safe_conn = std::make_shared<ThreadSafeConnection>(std::move(raw_conn));
  
  auto connect_result = thread_safe_conn->connect(config_.connection_settings);
  if (connect_result.has_error()) {
    return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{
      connect_result.error(), connect_result.error_message()
    };
  }
  
  return rs::util::Result<std::shared_ptr<ThreadSafeConnection>>{thread_safe_conn};
}

void ConnectionPool::cleanup_expired_connections() {
  all_connections_.erase(
    std::remove_if(all_connections_.begin(), all_connections_.end(),
      [](const std::weak_ptr<ThreadSafeConnection>& weak_conn) {
        return weak_conn.expired();
      }),
    all_connections_.end()
  );
}

} // namespace rs::core::database
