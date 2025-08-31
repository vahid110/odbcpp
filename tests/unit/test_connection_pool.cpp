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
    config_.acquire_timeout = std::chrono::milliseconds(1000);
    
    config_.connection_settings.host = "localhost";
    config_.connection_settings.port = 5432;
    config_.connection_settings.database = "test";
    config_.connection_settings.user = "test";
    config_.connection_settings.password = "test";
    config_.connection_settings.use_ssl = false;
    config_.connection_settings.timeout = std::chrono::seconds(5);
  }
  
  ConnectionPool::Config config_;
};

TEST_F(ConnectionPoolTest, BasicPoolCreation) {
  ConnectionPool pool(config_);
  
  auto stats = pool.get_stats();
  EXPECT_EQ(stats.total_connections, config_.min_connections);
  EXPECT_EQ(stats.available_connections, config_.min_connections);
  EXPECT_EQ(stats.active_connections, 0);
}

TEST_F(ConnectionPoolTest, AcquireAndRelease) {
  ConnectionPool pool(config_);
  
  // Acquire connection
  auto conn_result = pool.acquire();
  EXPECT_TRUE(conn_result.has_value());
  
  auto stats = pool.get_stats();
  EXPECT_EQ(stats.available_connections, 0);
  EXPECT_EQ(stats.active_connections, 1);
  
  // Release connection
  pool.release(*conn_result);
  
  stats = pool.get_stats();
  EXPECT_EQ(stats.available_connections, 1);
  EXPECT_EQ(stats.active_connections, 0);
}

TEST_F(ConnectionPoolTest, MaxConnectionsLimit) {
  config_.max_connections = 2;
  ConnectionPool pool(config_);
  
  // Acquire all available connections
  auto conn1 = pool.acquire();
  auto conn2 = pool.acquire();
  
  EXPECT_TRUE(conn1.has_value());
  EXPECT_TRUE(conn2.has_value());
  
  auto stats = pool.get_stats();
  EXPECT_EQ(stats.total_connections, 2);
  EXPECT_EQ(stats.active_connections, 2);
  EXPECT_EQ(stats.available_connections, 0);
  
  // Try to acquire one more (should timeout)
  config_.acquire_timeout = std::chrono::milliseconds(100);
  ConnectionPool pool2(config_);
  auto conn1_2 = pool2.acquire();
  auto conn2_2 = pool2.acquire();
  
  auto start = std::chrono::steady_clock::now();
  auto conn3 = pool2.acquire();
  auto end = std::chrono::steady_clock::now();
  
  EXPECT_FALSE(conn3.has_value());
  EXPECT_GE(end - start, std::chrono::milliseconds(90)); // Should have waited
}

TEST_F(ConnectionPoolTest, ConcurrentAccess) {
  config_.max_connections = 5;
  ConnectionPool pool(config_);
  
  const int num_threads = 4;
  const int operations_per_thread = 10;
  std::vector<std::thread> threads;
  std::atomic<int> successful_operations{0};
  
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&pool, &successful_operations, operations_per_thread]() {
      for (int j = 0; j < operations_per_thread; ++j) {
        auto conn = pool.acquire();
        if (conn.has_value()) {
          // Simulate some work
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          pool.release(*conn);
          successful_operations++;
        }
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  EXPECT_EQ(successful_operations.load(), num_threads * operations_per_thread);
  
  // All connections should be back in the pool
  auto stats = pool.get_stats();
  EXPECT_EQ(stats.active_connections, 0);
}

TEST_F(ConnectionPoolTest, Shutdown) {
  ConnectionPool pool(config_);
  
  auto conn = pool.acquire();
  EXPECT_TRUE(conn.has_value());
  
  pool.shutdown();
  
  // Should not be able to acquire after shutdown
  auto conn2 = pool.acquire();
  EXPECT_FALSE(conn2.has_value());
  EXPECT_EQ(conn2.error().value(), static_cast<int>(rs::util::DbErrorCode::InvalidParameter));
}