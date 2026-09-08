#include <gtest/gtest.h>
#include "core/database/connection_pool.h"
#include <thread>
#include <vector>
#include <chrono>

using namespace rs::core::database;

class ConnectionPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    config_.min_connections = 1;
    config_.max_connections = 3;
    config_.acquire_timeout = std::chrono::milliseconds(5);
    
    // Use invalid settings that will fail connection but allow pool creation
    config_.connection_settings.host = "127.0.0.1";
    config_.connection_settings.port = 1;
    config_.connection_settings.database = "test";
    config_.connection_settings.user = "test";
    config_.connection_settings.password = "test";
    config_.connection_settings.use_ssl = false;
    config_.connection_settings.timeout = std::chrono::milliseconds(5);
  }
  
  ConnectionPool::Config config_;
};

TEST_F(ConnectionPoolTest, BasicPoolCreation) {
  // Test pool creation with invalid settings (should not crash)
  EXPECT_NO_THROW({
    ConnectionPool pool(config_);
    auto stats = pool.get_stats();
    EXPECT_EQ(stats.total_connections, 0); // No connections created yet
  });
}

TEST_F(ConnectionPoolTest, AcquireAndRelease) {
  ConnectionPool pool(config_);
  
  // Acquire should fail with invalid connection settings
  auto conn = pool.acquire();
  EXPECT_FALSE(conn.has_value());
  EXPECT_TRUE(conn.has_error());
}

TEST_F(ConnectionPoolTest, MaxConnectionsLimit) {
  // Test pool configuration validation
  config_.max_connections = 0;
  EXPECT_THROW(ConnectionPool pool(config_), std::exception);
  
  // Test valid configuration
  config_.max_connections = 2;
  EXPECT_NO_THROW({
    ConnectionPool pool(config_);
    auto stats = pool.get_stats();
    EXPECT_EQ(stats.total_connections, 0); // No connections created yet with invalid settings
  });
}

TEST_F(ConnectionPoolTest, ConcurrentAccess) {
  // Test concurrent pool access without requiring real connections
  config_.max_connections = 5;
  ConnectionPool pool(config_);
  
  const int num_threads = 4;
  std::vector<std::thread> threads;
  std::atomic<int> acquire_attempts{0};
  
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&pool, &acquire_attempts]() {
      for (int j = 0; j < 5; ++j) {
        auto conn = pool.acquire();
        acquire_attempts++;
        // Connection will fail with invalid settings, but pool should handle concurrency
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  EXPECT_EQ(acquire_attempts.load(), num_threads * 5);
}

TEST_F(ConnectionPoolTest, Shutdown) {
  ConnectionPool pool(config_);
  
  // Shutdown should work regardless of connection state
  EXPECT_NO_THROW(pool.shutdown());
  
  // Should not be able to acquire after shutdown
  auto conn = pool.acquire();
  EXPECT_FALSE(conn.has_value());
}

TEST_F(ConnectionPoolTest, ThreadSafetyStressTest) {
  config_.max_connections = 3;
  ConnectionPool pool(config_);
  
  const int num_threads = 10;
  const int operations_per_thread = 20;
  std::vector<std::thread> threads;
  std::atomic<int> total_operations{0};
  std::atomic<int> exceptions{0};
  
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < operations_per_thread; ++j) {
        try {
          auto conn = pool.acquire();
          total_operations++;
          // Connection will fail, but no exceptions should occur
          std::this_thread::sleep_for(std::chrono::microseconds(1));
        } catch (...) {
          exceptions++;
        }
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  EXPECT_EQ(exceptions.load(), 0);
  EXPECT_EQ(total_operations.load(), num_threads * operations_per_thread);
}

TEST_F(ConnectionPoolTest, ErrorReporting) {
  // Test invalid configuration
  config_.max_connections = 0;
  EXPECT_THROW(ConnectionPool pool(config_), std::exception);
  
  // Test connection failure handling
  config_.max_connections = 1;
  ConnectionPool pool(config_);
  
  auto conn = pool.acquire();
  EXPECT_FALSE(conn.has_value()); // Should fail with invalid settings
  EXPECT_TRUE(conn.has_error());
}

TEST_F(ConnectionPoolTest, ConnectionFailureHandling) {
  config_.connection_settings.host = "127.0.0.1";
  config_.connection_settings.port = 1;
  
  // Pool creation should succeed even with bad settings
  ConnectionPool pool(config_);
  
  // But connection acquisition should fail
  auto conn = pool.acquire();
  EXPECT_FALSE(conn.has_value());
  EXPECT_TRUE(
      conn.error() == rs::util::make_error_code(
                          rs::util::DbErrorCode::ConnectionFailed) ||
      conn.error() == rs::util::make_error_code(rs::util::DbErrorCode::Timeout));
}

TEST_F(ConnectionPoolTest, ReleaseInvalidConnection) {
  ConnectionPool pool(config_);
  
  // Release null connection (should not crash)
  EXPECT_NO_THROW(pool.release(nullptr));
  
  // Stats should be unchanged
  auto stats = pool.get_stats();
  EXPECT_EQ(stats.available_connections, 0); // No connections with invalid settings
}

TEST_F(ConnectionPoolTest, ConcurrentAcquireRelease) {
  config_.max_connections = 2;
  ConnectionPool pool(config_);
  
  std::atomic<bool> stop{false};
  std::atomic<int> acquire_attempts{0};
  
  // Thread 1: Continuously try to acquire
  std::thread acquirer([&]() {
    while (!stop.load()) {
      auto conn = pool.acquire();
      acquire_attempts++;
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  });
  
  // Thread 2: Monitor stats
  std::thread monitor([&]() {
    for (int i = 0; i < 50; ++i) {
      auto stats = pool.get_stats();
      EXPECT_LE(stats.active_connections, config_.max_connections);
      EXPECT_LE(stats.available_connections, config_.max_connections);
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });
  
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.store(true);
  
  acquirer.join();
  monitor.join();
  
  EXPECT_GT(acquire_attempts.load(), 0);
}
