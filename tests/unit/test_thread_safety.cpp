#include <gtest/gtest.h>
#include "core/database/connection_pool.h"
#include "core/database/database_factory.h"
#include "odbcpp/util/exception_adapter.h"
#include <algorithm>
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
          std::string param = thread_safe_conn_->server_version();
          
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

namespace {
// Holds the first protocol operation while a second caller attempts entry.
class BlockingSession final : public IDatabaseConnection {
 public:
  std::mutex mutex;
  std::condition_variable cv;
  int entries{}, in_flight{}, maximum_in_flight{};
  bool release{};
  BackendResult<void> connect(const ConnectionSettings&) override { return {}; }
  void disconnect() override {}
  bool is_connected() const override { return true; }
  std::string server_version() const override { return "fixture"; }
  BackendResult<QueryResult> operation() {
    std::unique_lock lock(mutex);
    ++entries; ++in_flight;
    maximum_in_flight = std::max(maximum_in_flight, in_flight);
    cv.notify_all();
    cv.wait(lock, [&] { return release; });
    --in_flight;
    return QueryResult{};
  }
  BackendResult<QueryResult> execute_query(std::string_view, rs::util::Deadline) override {
    return operation();
  }
  BackendResult<QueryResult> execute_prepared(std::string_view,
      std::span<const QueryParameter>, rs::util::Deadline) override {
    return operation();
  }
};
}

TEST(ThreadSafeProtocolTest, DirectAndPreparedExchangesNeverOverlap) {
  for (const bool first_prepared : {false, true}) {
    for (const bool second_prepared : {false, true}) {
      auto raw = std::make_unique<BlockingSession>();
      auto* seen = raw.get();
      ThreadSafeConnection connection(std::move(raw));
      const auto invoke = [&](bool prepared) {
        if (prepared) return connection.execute_prepared("SELECT ?", {}, rs::util::Deadline::max());
        return connection.execute_query("SELECT 1", rs::util::Deadline::max());
      };
      std::thread first([&] { EXPECT_TRUE(invoke(first_prepared)); });
      {
        std::unique_lock lock(seen->mutex);
        EXPECT_TRUE(seen->cv.wait_for(lock, std::chrono::seconds(5), [&] { return seen->entries == 1; }));
      }
      std::atomic<bool> second_attempted{false};
      std::thread second([&] {
        second_attempted.store(true);
        EXPECT_TRUE(invoke(second_prepared));
      });
      while (!second_attempted.load()) std::this_thread::yield();
      {
        std::unique_lock lock(seen->mutex);
        EXPECT_FALSE(seen->cv.wait_for(lock, std::chrono::milliseconds(250), [&] { return seen->entries > 1; }))
            << "Concurrent protocol entry";
        seen->release = true;
        seen->cv.notify_all();
      }
      first.join(); second.join();
      EXPECT_EQ(2, seen->entries);
      EXPECT_EQ(1, seen->maximum_in_flight);
    }
  }
}
