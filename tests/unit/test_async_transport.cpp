#include <gtest/gtest.h>
#include "core/transport/thread_pool_transport.h"
#include "core/util/deadline.h"
#include <thread>
#include <atomic>
#include <chrono>

using namespace rs::core::transport;

class AsyncTransportTest : public ::testing::Test {
protected:
  void SetUp() override {
    transport_ = std::make_unique<ThreadPoolTransport>(2);
  }
  
  std::unique_ptr<ThreadPoolTransport> transport_;
};

TEST_F(AsyncTransportTest, CallbackConnect) {
  std::atomic<bool> callback_called{false};
  std::atomic<bool> connect_success{false};
  
  auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
  auto op = transport_->connect_async("127.0.0.1", 80, deadline,
    [&](rs::util::Result<void> result) {
      connect_success.store(result.has_value());
      callback_called.store(true);
    });
  
  // Wait for callback
  auto start = std::chrono::steady_clock::now();
  while (!callback_called.load() && 
         std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  
  EXPECT_TRUE(callback_called.load());
  // Connection may fail (no server), but callback should be called
}

TEST_F(AsyncTransportTest, FutureConnect) {
  auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
  auto future = transport_->connect_future("127.0.0.1", 80, deadline);
  
  // Should complete within reasonable time
  auto status = future.wait_for(std::chrono::seconds(2));
  EXPECT_EQ(status, std::future_status::ready);
  
  auto result = future.get();
  // Connection may fail, but future should resolve
  EXPECT_TRUE(result.has_value() || result.has_error());
}

TEST_F(AsyncTransportTest, OperationCancellation) {
  std::atomic<bool> callback_called{false};
  std::atomic<int> callback_count{0};
  std::atomic<bool> cancellation_reported{false};
  
  auto deadline = rs::util::make_deadline(std::chrono::milliseconds(500));
  auto op = transport_->connect_async("192.0.2.1", 12345, deadline, // Non-routable IP
    [&](rs::util::Result<void> result) {
      callback_called.store(true);
      callback_count.fetch_add(1);
      cancellation_reported.store(result.has_error());
    });
  
  EXPECT_FALSE(op->is_complete());
  EXPECT_FALSE(op->is_cancelled());
  
  // Cancel operation
  op->cancel();
  EXPECT_TRUE(op->is_cancelled());
  EXPECT_TRUE(op->is_complete());
  EXPECT_TRUE(callback_called.load());
  EXPECT_TRUE(cancellation_reported.load());

  // Cancellation and its callback are idempotent.
  op->cancel();
  EXPECT_EQ(callback_count.load(), 1);
  
  // Give some time for cancellation to take effect
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(AsyncTransportTest, DestroyingOperationHandleDoesNotInvalidateQueuedWork) {
  std::atomic<bool> callback_called{false};

  {
    auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
    auto op = transport_->connect_async("127.0.0.1", 9, deadline,
      [&](rs::util::Result<void> result) {
        EXPECT_TRUE(result.has_value() || result.has_error());
        callback_called.store(true);
      });
  }

  auto start = std::chrono::steady_clock::now();
  while (!callback_called.load() &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(callback_called.load());
}

TEST(ThreadPoolTransportConfigurationTest, RejectsZeroQueueDepth) {
  EXPECT_THROW(ThreadPoolTransport(1, 0), std::invalid_argument);
}

TEST_F(AsyncTransportTest, ConcurrentOperations) {
  const int num_operations = 10;
  std::atomic<int> completed_operations{0};
  std::vector<std::unique_ptr<AsyncOperation>> operations;
  
  auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  
  for (int i = 0; i < num_operations; ++i) {
    auto op = transport_->connect_async("127.0.0.1", 80 + i, deadline,
      [&](rs::util::Result<void> result) {
        completed_operations++;
      });
    operations.push_back(std::move(op));
  }
  
  // Wait for all operations to complete
  auto start = std::chrono::steady_clock::now();
  while (completed_operations.load() < num_operations &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  
  EXPECT_EQ(completed_operations.load(), num_operations);
}

TEST_F(AsyncTransportTest, SendReceiveAfterConnect) {
  // First establish a connection (this will likely fail, but test the flow)
  auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
  auto connect_result = transport_->connect("127.0.0.1", 80, deadline);
  
  if (connect_result.has_value()) {
    // Test async send
    std::string data = "GET / HTTP/1.1\r\n\r\n";
    std::vector<std::byte> buffer;
    buffer.reserve(data.size());
    for (char c : data) {
      buffer.push_back(static_cast<std::byte>(c));
    }
    
    std::atomic<bool> send_done{false};
    auto send_op = transport_->send_async(buffer, deadline,
      [&](rs::util::Result<IOResult> result) {
        send_done.store(true);
      });
    
    // Wait for send
    auto start = std::chrono::steady_clock::now();
    while (!send_done.load() && 
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    EXPECT_TRUE(send_done.load());
  }
  // If connection fails, that's expected in test environment
}

TEST_F(AsyncTransportTest, TimeoutHandling) {
  std::atomic<bool> callback_called{false};
  std::atomic<bool> operation_failed{false};
  
  // Very short timeout
  auto deadline = rs::util::make_deadline(std::chrono::milliseconds(1));
  auto op = transport_->connect_async("192.0.2.1", 12345, deadline, // Non-routable
    [&](rs::util::Result<void> result) {
      callback_called.store(true);
      operation_failed.store(result.has_error());
    });
  
  // Wait for timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  
  // Operation should timeout quickly due to expired deadline
  EXPECT_TRUE(callback_called.load() || op->is_cancelled());
}

TEST_F(AsyncTransportTest, MultipleTransportInstances) {
  auto transport1 = std::make_unique<ThreadPoolTransport>(1);
  auto transport2 = std::make_unique<ThreadPoolTransport>(1);
  
  std::atomic<int> callbacks_called{0};
  auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
  
  auto op1 = transport1->connect_async("127.0.0.1", 80, deadline,
    [&](rs::util::Result<void> result) { callbacks_called++; });
    
  auto op2 = transport2->connect_async("127.0.0.1", 81, deadline,
    [&](rs::util::Result<void> result) { callbacks_called++; });
  
  // Wait for both
  auto start = std::chrono::steady_clock::now();
  while (callbacks_called.load() < 2 &&
         std::chrono::steady_clock::now() - start < std::chrono::seconds(3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  
  EXPECT_EQ(callbacks_called.load(), 2);
}
