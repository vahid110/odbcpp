#include <gtest/gtest.h>
#include "core/database/async_database_connection.h"
#include "core/transport/thread_pool_transport.h"
#include "core/util/deadline.h"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>

using namespace rs::core::database;

class AsyncConcurrencyTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create multiple connections for concurrency testing
    for (int i = 0; i < 3; ++i) {
      auto transport = std::make_unique<rs::core::transport::ThreadPoolTransport>(2);
      connections_.emplace_back(
        std::make_unique<AsyncDatabaseConnection>(nullptr, std::move(transport))
      );
    }
  }
  
  std::vector<std::unique_ptr<AsyncDatabaseConnection>> connections_;
};

TEST_F(AsyncConcurrencyTest, MultipleConnectionsConcurrentQueries) {
  const int queries_per_connection = 10;
  std::atomic<int> total_completed{0};
  std::atomic<int> total_successful{0};
  
  std::vector<std::thread> workers;
  
  for (size_t conn_idx = 0; conn_idx < connections_.size(); ++conn_idx) {
    workers.emplace_back([&, conn_idx]() {
      auto& conn = connections_[conn_idx];
      
      for (int i = 0; i < queries_per_connection; ++i) {
        auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
        std::string sql = "SELECT " + std::to_string(conn_idx * 100 + i);
        
        conn->execute_query_async(sql, deadline,
          [&](rs::util::Result<QueryResult> result) {
            total_completed++;
            if (result.has_value()) {
              total_successful++;
            }
          });
      }
    });
  }
  
  // Wait for all workers to submit queries
  for (auto& worker : workers) {
    worker.join();
  }
  
  // Wait for all queries to complete
  const int expected_total =
      static_cast<int>(connections_.size()) * queries_per_connection;
  auto start = std::chrono::steady_clock::now();
  while (total_completed.load() < expected_total &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  
  EXPECT_EQ(total_completed.load(), expected_total);
  EXPECT_EQ(total_successful.load(), expected_total); // Mock implementation should succeed
}

TEST_F(AsyncConcurrencyTest, ThreadSafetyStressTest) {
  const int num_threads = 8;
  const int operations_per_thread = 25;
  std::atomic<int> operations_completed{0};
  std::atomic<int> exceptions_caught{0};
  
  std::vector<std::thread> threads;
  
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&, t]() {
      std::random_device rd;
      std::mt19937 gen(rd());
      std::uniform_int_distribution<> conn_dist(
          0, static_cast<int>(connections_.size() - 1));
      std::uniform_int_distribution<> delay_dist(1, 50);
      
      for (int i = 0; i < operations_per_thread; ++i) {
        try {
          // Random connection
          auto& conn = connections_[conn_dist(gen)];
          
          // Random operation type
          auto deadline = rs::util::make_deadline(std::chrono::seconds(3));
          
          if (i % 3 == 0) {
            // Query
            std::string sql = "SELECT " + std::to_string(t * 1000 + i);
            conn->execute_query_async(sql, deadline,
              [&](rs::util::Result<QueryResult> result) {
                operations_completed++;
              });
          } else if (i % 3 == 1) {
            // Prepared query
            std::vector<std::string> params = {std::to_string(t), std::to_string(i)};
            conn->execute_prepared_async("SELECT $1, $2", params, deadline,
              [&](rs::util::Result<QueryResult> result) {
                operations_completed++;
              });
          } else {
            // Future-based query
            auto future = conn->execute_query_future("SELECT 'future'", deadline);
            std::thread([future = std::move(future), &operations_completed]() mutable {
              try {
                auto result = future.get();
                operations_completed++;
              } catch (...) {
                operations_completed++; // Count as completed even if failed
              }
            }).detach();
          }
          
          // Random small delay
          std::this_thread::sleep_for(std::chrono::microseconds(delay_dist(gen)));
          
        } catch (...) {
          exceptions_caught++;
        }
      }
    });
  }
  
  // Wait for all threads
  for (auto& thread : threads) {
    thread.join();
  }
  
  // Wait for all operations to complete
  const int expected_operations = num_threads * operations_per_thread;
  auto start = std::chrono::steady_clock::now();
  while (operations_completed.load() < expected_operations &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(15)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  EXPECT_EQ(exceptions_caught.load(), 0);
  EXPECT_EQ(operations_completed.load(), expected_operations);
}

TEST_F(AsyncConcurrencyTest, CallbackThreadSafety) {
  const int num_callbacks = 100;
  std::atomic<int> callback_count{0};
  std::vector<std::size_t> callback_thread_ids;
  std::mutex thread_ids_mutex;
  
  auto& conn = connections_[0];
  auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  
  // Submit many async operations
  for (int i = 0; i < num_callbacks; ++i) {
    std::string sql = "SELECT " + std::to_string(i);
    conn->execute_query_async(sql, deadline,
      [&](rs::util::Result<QueryResult> result) {
        callback_count++;
        
        // Record which thread the callback runs on
        std::lock_guard lock(thread_ids_mutex);
        callback_thread_ids.push_back(std::hash<std::thread::id>{}(std::this_thread::get_id()));
      });
  }
  
  // Wait for all callbacks
  auto start = std::chrono::steady_clock::now();
  while (callback_count.load() < num_callbacks &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  
  EXPECT_EQ(callback_count.load(), num_callbacks);
  
  // Callbacks should run on thread pool threads (not main thread)
  std::lock_guard lock(thread_ids_mutex);
  EXPECT_EQ(callback_thread_ids.size(), num_callbacks);
  
  // Should have used multiple threads from the pool
  std::set<std::size_t> unique_thread_ids(callback_thread_ids.begin(),
                                          callback_thread_ids.end());
  EXPECT_GT(unique_thread_ids.size(), 1); // At least 2 different threads
}

TEST_F(AsyncConcurrencyTest, FutureThreadSafety) {
  const int num_futures = 50;
  std::vector<std::future<rs::util::Result<QueryResult>>> futures;
  
  auto& conn = connections_[0];
  auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  
  // Create many futures
  for (int i = 0; i < num_futures; ++i) {
    std::string sql = "SELECT " + std::to_string(i);
    futures.push_back(conn->execute_query_future(sql, deadline));
  }
  
  // Access futures from multiple threads
  std::atomic<int> results_obtained{0};
  std::vector<std::thread> threads;
  
  const int threads_per_batch = 5;
  for (int t = 0; t < threads_per_batch; ++t) {
    threads.emplace_back([&, t]() {
      int start_idx = t * (num_futures / threads_per_batch);
      int end_idx = (t + 1) * (num_futures / threads_per_batch);
      if (t == threads_per_batch - 1) end_idx = num_futures; // Handle remainder
      
      for (int i = start_idx; i < end_idx; ++i) {
        try {
          auto result = futures[i].get();
          if (result.has_value() || result.has_error()) {
            results_obtained++;
          }
        } catch (...) {
          results_obtained++; // Count as obtained even if exception
        }
      }
    });
  }
  
  // Wait for all threads
  for (auto& thread : threads) {
    thread.join();
  }
  
  EXPECT_EQ(results_obtained.load(), num_futures);
}

TEST_F(AsyncConcurrencyTest, ResourceCleanupUnderLoad) {
  const int num_operations = 200;
  std::atomic<int> operations_started{0};
  std::atomic<int> operations_completed{0};
  
  // Start many operations rapidly
  std::thread producer([&]() {
    auto& conn = connections_[0];
    
    for (int i = 0; i < num_operations; ++i) {
      auto deadline = rs::util::make_deadline(std::chrono::milliseconds(100)); // Short timeout
      std::string sql = "SELECT " + std::to_string(i);
      
      conn->execute_query_async(sql, deadline,
        [&](rs::util::Result<QueryResult> result) {
          operations_completed++;
        });
      
      operations_started++;
      
      // Small delay to create overlap
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  });
  
  producer.join();
  
  // Wait for operations to complete or timeout
  auto start = std::chrono::steady_clock::now();
  while (operations_completed.load() < operations_started.load() &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  
  EXPECT_EQ(operations_started.load(), num_operations);
  // Some operations may timeout, but no crashes should occur
  EXPECT_GT(operations_completed.load(), 0);
}
