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
    // Use connection settings from DSN configuration
    settings_.host = "vahidsbr-redshift-cluster.cxzokcavspmr.us-east-1.redshift.amazonaws.com";
    settings_.port = 5439;
    settings_.database = "dev";
    settings_.user = "awsuser";
    settings_.password = "Testing1234";
    settings_.use_ssl = false;
    settings_.timeout = std::chrono::seconds(10);
    
    auto transport = std::make_unique<rs::core::transport::ThreadPoolTransport>(4);
    async_conn_ = std::make_unique<AsyncDatabaseConnection>(nullptr, std::move(transport));
  }
  
  std::unique_ptr<AsyncDatabaseConnection> async_conn_;
  ConnectionSettings settings_;
};

TEST_F(AsyncDatabaseIntegrationTest, RealAsyncConnection) {
  // Use synchronous interface for real database testing
  auto connect_result = async_conn_->connect(settings_);
  ASSERT_TRUE(connect_result.has_value()) << "Connection failed: " << connect_result.error_message();
  
  EXPECT_TRUE(async_conn_->is_connected());
  
  // Test query on real connection
  auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto query_result = async_conn_->execute_query("SELECT 1 as test_col", deadline);
  
  ASSERT_TRUE(query_result.has_value()) << "Query failed: " << query_result.error_message();
  ASSERT_FALSE(query_result->rows.empty());
  EXPECT_EQ("1", query_result->rows[0][0]);
}

TEST_F(AsyncDatabaseIntegrationTest, ConcurrentRealQueries) {
  // Connect synchronously
  auto connect_result = async_conn_->connect(settings_);
  ASSERT_TRUE(connect_result.has_value()) << "Connection failed: " << connect_result.error_message();
  
  const int num_queries = 3; // Reduced for real database testing
  auto deadline = rs::util::make_deadline(std::chrono::seconds(10));
  
  // Execute queries sequentially (simulating concurrent behavior)
  for (int i = 0; i < num_queries; ++i) {
    std::string sql = "SELECT " + std::to_string(i) + " as query_id";
    
    auto result = async_conn_->execute_query(sql, deadline);
    ASSERT_TRUE(result.has_value()) << "Query " << i << " failed: " << result.error_message();
    ASSERT_FALSE(result->rows.empty());
    
    std::string expected = std::to_string(i);
    EXPECT_EQ(expected, result->rows[0][0]);
  }
}

TEST_F(AsyncDatabaseIntegrationTest, AsyncTransactionHandling) {
  auto connect_result = async_conn_->connect(settings_);
  ASSERT_TRUE(connect_result.has_value()) << "Connection failed: " << connect_result.error_message();
  
  auto deadline = rs::util::make_deadline(std::chrono::seconds(10));
  
  // Execute transaction operations sequentially
  auto begin_result = async_conn_->execute_query("BEGIN", deadline);
  ASSERT_TRUE(begin_result.has_value()) << "BEGIN failed: " << begin_result.error_message();
  
  auto select_result = async_conn_->execute_query("SELECT 'in_transaction'", deadline);
  ASSERT_TRUE(select_result.has_value()) << "SELECT failed: " << select_result.error_message();
  ASSERT_FALSE(select_result->rows.empty());
  EXPECT_EQ("in_transaction", select_result->rows[0][0]);
  
  auto rollback_result = async_conn_->execute_query("ROLLBACK", deadline);
  ASSERT_TRUE(rollback_result.has_value()) << "ROLLBACK failed: " << rollback_result.error_message();
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
  auto connect_result = async_conn_->connect(settings_);
  ASSERT_TRUE(connect_result.has_value()) << "Connection failed: " << connect_result.error_message();
  
  // Test with reasonable timeout for a simple query
  auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  
  auto result = async_conn_->execute_query("SELECT 'timeout_test'", deadline);
  ASSERT_TRUE(result.has_value()) << "Query failed: " << result.error_message();
  ASSERT_FALSE(result->rows.empty());
  EXPECT_EQ("timeout_test", result->rows[0][0]);
}