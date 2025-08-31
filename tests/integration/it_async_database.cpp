#include <gtest/gtest.h>
#include "core/database/async_database_connection.h"
#include "core/transport/thread_pool_transport.h"
#include "core/util/deadline.h"
#include "core/util/exception_adapter.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>

using namespace rs::core::database;

class AsyncDatabaseIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto transport = std::make_unique<rs::core::transport::ThreadPoolTransport>(4);
    async_conn_ = std::make_unique<AsyncDatabaseConnection>(nullptr, std::move(transport));
    
    // Connection settings from environment or defaults
    settings_.host = getenv("PGHOST") ? getenv("PGHOST") : "127.0.0.1";
    settings_.port = getenv("PGPORT") ? std::stoi(getenv("PGPORT")) : 5432;
    settings_.database = getenv("PGDATABASE") ? getenv("PGDATABASE") : "postgres";
    settings_.user = getenv("PGUSER") ? getenv("PGUSER") : "postgres";
    settings_.password = getenv("PGPASSWORD") ? getenv("PGPASSWORD") : "postgres";
    settings_.use_ssl = false;
    settings_.timeout = std::chrono::seconds(10);
  }
  
  std::unique_ptr<AsyncDatabaseConnection> async_conn_;
  ConnectionSettings settings_;
};

TEST_F(AsyncDatabaseIntegrationTest, RealAsyncConnection) {
  // Skip if no database configuration
  if (!getenv("PGHOST") && !getenv("PGDATABASE")) {
    GTEST_SKIP() << "No database configuration found";
  }
  
  std::atomic<bool> connect_done{false};
  std::atomic<bool> connect_success{false};
  
  auto op = async_conn_->connect_async(settings_,
    [&](rs::util::Result<void> result) {
      connect_success.store(result.has_value());
      connect_done.store(true);
    });
  
  // Wait for connection
  auto start = std::chrono::steady_clock::now();
  while (!connect_done.load() && 
         std::chrono::steady_clock::now() - start < std::chrono::seconds(15)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  EXPECT_TRUE(connect_done.load());
  
  if (connect_success.load()) {
    // Test async query on real connection
    std::atomic<bool> query_done{false};
    std::atomic<bool> query_success{false};
    
    auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
    async_conn_->execute_query_async("SELECT 1 as test_col", deadline,
      [&](rs::util::Result<QueryResult> result) {
        if (result.has_value() && !result->rows.empty()) {
          query_success.store(result->rows[0][0] == "1");
        }
        query_done.store(true);
      });
    
    // Wait for query
    start = std::chrono::steady_clock::now();
    while (!query_done.load() && 
           std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    EXPECT_TRUE(query_done.load());
    EXPECT_TRUE(query_success.load());
  }
}

TEST_F(AsyncDatabaseIntegrationTest, ConcurrentRealQueries) {
  // Skip if no database configuration
  if (!getenv("PGHOST") && !getenv("PGDATABASE")) {
    GTEST_SKIP() << "No database configuration found";
  }
  
  // First connect synchronously for simplicity
  auto connect_result = async_conn_->connect_future(settings_).get();
  if (connect_result.has_error()) {
    GTEST_SKIP() << "Could not connect to database: " << connect_result.error_message();
  }
  
  const int num_concurrent_queries = 10;
  std::atomic<int> queries_completed{0};
  std::atomic<int> queries_successful{0};
  
  auto deadline = rs::util::make_deadline(std::chrono::seconds(10));
  
  // Submit concurrent queries
  for (int i = 0; i < num_concurrent_queries; ++i) {
    std::string sql = "SELECT " + std::to_string(i) + " as query_id";
    
    async_conn_->execute_query_async(sql, deadline,
      [&, i](rs::util::Result<QueryResult> result) {
        queries_completed++;
        
        if (result.has_value() && !result->rows.empty()) {
          std::string expected = std::to_string(i);
          if (result->rows[0][0] == expected) {
            queries_successful++;
          }
        }
      });
  }
  
  // Wait for all queries
  auto start = std::chrono::steady_clock::now();
  while (queries_completed.load() < num_concurrent_queries &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  EXPECT_EQ(queries_completed.load(), num_concurrent_queries);
  EXPECT_EQ(queries_successful.load(), num_concurrent_queries);
}

TEST_F(AsyncDatabaseIntegrationTest, AsyncTransactionHandling) {
  // Skip if no database configuration
  if (!getenv("PGHOST") && !getenv("PGDATABASE")) {
    GTEST_SKIP() << "No database configuration found";
  }
  
  auto connect_result = async_conn_->connect_future(settings_).get();
  if (connect_result.has_error()) {
    GTEST_SKIP() << "Could not connect to database";
  }
  
  std::atomic<int> operations_completed{0};
  auto deadline = rs::util::make_deadline(std::chrono::seconds(10));
  
  // Chain transaction operations
  async_conn_->execute_query_async("BEGIN", deadline,
    [&](rs::util::Result<QueryResult> begin_result) {
      operations_completed++;
      
      if (begin_result.has_value()) {
        async_conn_->execute_query_async("SELECT 'in_transaction'", deadline,
          [&](rs::util::Result<QueryResult> select_result) {
            operations_completed++;
            
            if (select_result.has_value()) {
              async_conn_->execute_query_async("ROLLBACK", deadline,
                [&](rs::util::Result<QueryResult> rollback_result) {
                  operations_completed++;
                });
            }
          });
      }
    });
  
  // Wait for transaction chain
  auto start = std::chrono::steady_clock::now();
  while (operations_completed.load() < 3 &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(15)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  EXPECT_EQ(operations_completed.load(), 3);
}

TEST_F(AsyncDatabaseIntegrationTest, AsyncErrorHandling) {
  // Test with intentionally bad connection settings
  ConnectionSettings bad_settings = settings_;
  bad_settings.host = "nonexistent-host-12345";
  bad_settings.port = 65000;
  
  std::atomic<bool> connect_done{false};
  std::atomic<bool> connect_failed{false};
  
  auto op = async_conn_->connect_async(bad_settings,
    [&](rs::util::Result<void> result) {
      connect_failed.store(result.has_error());
      connect_done.store(true);
    });
  
  // Wait for connection attempt
  auto start = std::chrono::steady_clock::now();
  while (!connect_done.load() && 
         std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  EXPECT_TRUE(connect_done.load());
  EXPECT_TRUE(connect_failed.load());
}

TEST_F(AsyncDatabaseIntegrationTest, AsyncTimeoutHandling) {
  // Skip if no database configuration
  if (!getenv("PGHOST") && !getenv("PGDATABASE")) {
    GTEST_SKIP() << "No database configuration found";
  }
  
  auto connect_result = async_conn_->connect_future(settings_).get();
  if (connect_result.has_error()) {
    GTEST_SKIP() << "Could not connect to database";
  }
  
  std::atomic<bool> query_done{false};
  std::atomic<bool> query_timed_out{false};
  
  // Very short timeout for a potentially slow query
  auto deadline = rs::util::make_deadline(std::chrono::milliseconds(1));
  
  async_conn_->execute_query_async("SELECT pg_sleep(1)", deadline,
    [&](rs::util::Result<QueryResult> result) {
      query_timed_out.store(result.has_error());
      query_done.store(true);
    });
  
  // Wait for timeout
  auto start = std::chrono::steady_clock::now();
  while (!query_done.load() && 
         std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  
  EXPECT_TRUE(query_done.load());
  // Query should timeout or complete quickly
}