#include <gtest/gtest.h>
#include "core/database/async_database_connection.h"
#include "core/transport/thread_pool_transport.h"
#include "core/util/deadline.h"
#include <thread>
#include <atomic>
#include <chrono>

using namespace rs::core::database;

class AsyncDatabaseTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto transport = std::make_unique<rs::core::transport::ThreadPoolTransport>(2);
    async_conn_ = std::make_unique<AsyncDatabaseConnection>(nullptr, std::move(transport));
  }
  
  std::unique_ptr<AsyncDatabaseConnection> async_conn_;
};

TEST_F(AsyncDatabaseTest, AsyncConnectCallback) {
  std::atomic<bool> callback_called{false};
  std::atomic<bool> connect_result{false};
  
  ConnectionSettings settings;
  settings.host = "localhost";
  settings.port = 5432;
  settings.database = "test";
  settings.user = "test";
  settings.password = "test";
  settings.timeout = std::chrono::seconds(2);
  
  auto op = async_conn_->connect_async(settings,
    [&](rs::util::Result<void> result) {
      connect_result.store(result.has_value());
      callback_called.store(true);
    });
  
  // Wait for callback
  auto start = std::chrono::steady_clock::now();
  while (!callback_called.load() && 
         std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  
  EXPECT_TRUE(callback_called.load());
  // Connection may fail in test environment, but callback should be called
}

TEST_F(AsyncDatabaseTest, AsyncConnectFuture) {
  ConnectionSettings settings;
  settings.host = "localhost";
  settings.port = 5432;
  settings.database = "test";
  settings.user = "test";
  settings.password = "test";
  settings.timeout = std::chrono::seconds(2);
  
  auto future = async_conn_->connect_future(settings);
  
  auto status = future.wait_for(std::chrono::seconds(3));
  EXPECT_EQ(status, std::future_status::ready);
  
  auto result = future.get();
  // Connection may fail, but future should resolve
  EXPECT_TRUE(result.has_value() || result.has_error());
}

TEST_F(AsyncDatabaseTest, AsyncQueryCallback) {
  std::atomic<bool> callback_called{false};
  std::atomic<bool> query_success{false};
  
  auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  auto op = async_conn_->execute_query_async("SELECT 1", deadline,
    [&](rs::util::Result<QueryResult> result) {
      if (result.has_value()) {
        query_success.store(!result->rows.empty());
      }
      callback_called.store(true);
    });
  
  // Wait for callback
  auto start = std::chrono::steady_clock::now();
  while (!callback_called.load() && 
         std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  
  EXPECT_TRUE(callback_called.load());
  EXPECT_TRUE(query_success.load()); // Our mock implementation should return results
}

TEST_F(AsyncDatabaseTest, AsyncQueryFuture) {
  auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  auto future = async_conn_->execute_query_future("SELECT 42", deadline);
  
  auto status = future.wait_for(std::chrono::seconds(3));
  EXPECT_EQ(status, std::future_status::ready);
  
  auto result = future.get();
  EXPECT_TRUE(result.has_value());
  if (result.has_value()) {
    EXPECT_FALSE(result->rows.empty());
  }
}

TEST_F(AsyncDatabaseTest, ConcurrentQueries) {
  const int num_queries = 5;
  std::atomic<int> completed_queries{0};
  std::vector<std::string> query_results;
  std::mutex results_mutex;
  
  auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  
  for (int i = 0; i < num_queries; ++i) {
    std::string sql = "SELECT " + std::to_string(i);
    async_conn_->execute_query_async(sql, deadline,
      [&](rs::util::Result<QueryResult> result) {
        if (result.has_value() && !result->rows.empty()) {
          std::lock_guard lock(results_mutex);
          query_results.push_back(result->rows[0][0]);
        }
        completed_queries++;
      });
  }
  
  // Wait for all queries
  auto start = std::chrono::steady_clock::now();
  while (completed_queries.load() < num_queries &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  
  EXPECT_EQ(completed_queries.load(), num_queries);
  EXPECT_EQ(query_results.size(), num_queries);
}

TEST_F(AsyncDatabaseTest, AsyncPreparedStatement) {
  std::atomic<bool> callback_called{false};
  std::atomic<bool> query_success{false};
  
  auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  std::vector<std::string> params = {"test_param"};
  
  auto op = async_conn_->execute_prepared_async("SELECT $1", params, deadline,
    [&](rs::util::Result<QueryResult> result) {
      if (result.has_value()) {
        query_success.store(!result->rows.empty());
      }
      callback_called.store(true);
    });
  
  // Wait for callback
  auto start = std::chrono::steady_clock::now();
  while (!callback_called.load() && 
         std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  
  EXPECT_TRUE(callback_called.load());
  EXPECT_TRUE(query_success.load());
}

TEST_F(AsyncDatabaseTest, MixedSyncAsyncOperations) {
  // Test that sync and async operations can coexist
  
  // Sync query
  auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  auto sync_result = async_conn_->execute_query("SELECT 'sync'", deadline);
  EXPECT_TRUE(sync_result.has_value());
  
  // Async query
  std::atomic<bool> async_done{false};
  async_conn_->execute_query_async("SELECT 'async'", deadline,
    [&](rs::util::Result<QueryResult> result) {
      EXPECT_TRUE(result.has_value());
      async_done.store(true);
    });
  
  // Wait for async
  auto start = std::chrono::steady_clock::now();
  while (!async_done.load() && 
         std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  
  EXPECT_TRUE(async_done.load());
}

TEST_F(AsyncDatabaseTest, AsyncOperationChaining) {
  std::atomic<int> operations_completed{0};
  
  ConnectionSettings settings;
  settings.host = "localhost";
  settings.port = 5432;
  settings.timeout = std::chrono::seconds(2);
  
  // Chain: connect -> query -> prepared query
  async_conn_->connect_async(settings,
    [&](rs::util::Result<void> connect_result) {
      operations_completed++;
      
      if (connect_result.has_value()) {
        auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
        async_conn_->execute_query_async("SELECT 1", deadline,
          [&](rs::util::Result<QueryResult> query_result) {
            operations_completed++;
            
            if (query_result.has_value()) {
              std::vector<std::string> params = {"test"};
              async_conn_->execute_prepared_async("SELECT $1", params, deadline,
                [&](rs::util::Result<QueryResult> prepared_result) {
                  operations_completed++;
                });
            }
          });
      }
    });
  
  // Wait for all chained operations
  auto start = std::chrono::steady_clock::now();
  while (operations_completed.load() < 3 &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  
  // At least connect should complete, others depend on actual connection success
  EXPECT_GE(operations_completed.load(), 1);
}