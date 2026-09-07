#include <gtest/gtest.h>
#include "core/database/connection_pool.h"
#include "core/database/database_factory.h"
#include "core/util/exception_adapter.h"
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <random>

using namespace rs::core::database;

class ThreadSafetyTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Create a mock connection for testing
    auto raw_conn = DatabaseFactory::create_connection();
    thread_safe_conn_ = std::make_unique<ThreadSafeConnection>(std::move(raw_conn));
  }
  
  std::unique_ptr<ThreadSafeConnection> thread_safe_conn_;
};

TEST_F(ThreadSafetyTest, ConcurrentReads) {
  const int num_threads = 8;
  const int reads_per_thread = 100;
  std::vector<std::thread> threads;
  std::atomic<int> successful_reads{0};
  std::atomic<int> failed_reads{0};
  
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < reads_per_thread; ++j) {
        try {
          // These should be safe concurrent reads
          bool connected = thread_safe_conn_->is_connected();
          std::string error = thread_safe_conn_->get_last_error();
          std::string param = thread_safe_conn_->get_parameter("application_name");
          
          successful_reads++;
        } catch (...) {
          failed_reads++;
        }
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  EXPECT_EQ(failed_reads.load(), 0);
  EXPECT_EQ(successful_reads.load(), num_threads * reads_per_thread);
}

TEST_F(ThreadSafetyTest, ConcurrentWrites) {
  const int num_threads = 4;
  const int writes_per_thread = 10;
  std::vector<std::thread> threads;
  std::atomic<int> successful_writes{0};
  std::atomic<int> failed_writes{0};
  
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&]() {
      for (int j = 0; j < writes_per_thread; ++j) {
        try {
          // These operations require exclusive access
          ConnectionSettings settings;
          settings.host = "127.0.0.1";
          settings.port = 1;
          settings.database = "test";
          settings.user = "test";
          settings.password = "test";
          settings.timeout = std::chrono::milliseconds(5);
          settings.use_ssl = false;
          
          auto result = thread_safe_conn_->connect(settings);
          if (result.has_value()) {
            successful_writes++;
          } else {
            failed_writes++;
          }
          
          // Disconnect to allow other threads
          thread_safe_conn_->disconnect();
          
        } catch (...) {
          failed_writes++;
        }
      }
    });
  }
  
  for (auto& thread : threads) {
    thread.join();
  }
  
  // No crashes or data races should occur
  EXPECT_EQ(successful_writes.load() + failed_writes.load(), num_threads * writes_per_thread);
}
