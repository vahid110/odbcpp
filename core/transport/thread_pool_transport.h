#pragma once
#include "async_transport.h"
#include "socket_transport.h"
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>

namespace rs::core::transport {

// Cross-platform async transport using thread pool
class ThreadPoolTransport : public IAsyncTransport {
public:
  explicit ThreadPoolTransport(
      size_t thread_count = std::thread::hardware_concurrency(),
      size_t queue_depth = 256);
  ~ThreadPoolTransport() override;
  
  // Sync interface (inherited)
  rs::util::Result<void> connect(std::string_view host, uint16_t port, rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> send(std::span<const std::byte> buf, rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> recv(std::span<std::byte> buf, rs::util::Deadline deadline) override;
  void close() noexcept override;
  
  // Async interface
  std::unique_ptr<AsyncOperation> connect_async(
    std::string_view host, uint16_t port, rs::util::Deadline deadline,
    ConnectCallback callback) override;
    
  std::unique_ptr<AsyncOperation> send_async(
    std::span<const std::byte> buf, rs::util::Deadline deadline,
    SendCallback callback) override;
    
  std::unique_ptr<AsyncOperation> recv_async(
    std::span<std::byte> buf, rs::util::Deadline deadline,
    RecvCallback callback) override;
  
  // Future interface
  std::future<rs::util::Result<void>> connect_future(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) override;
    
  std::future<rs::util::Result<IOResult>> send_future(
    std::span<const std::byte> buf, rs::util::Deadline deadline) override;
    
  std::future<rs::util::Result<IOResult>> recv_future(
    std::span<std::byte> buf, rs::util::Deadline deadline) override;

private:
  class ThreadPoolOperation;
  
  struct Task {
    std::function<void()> work;
    std::function<void()> cancel;
  };
  
  SocketTransport socket_;
  std::vector<std::thread> workers_;
  std::queue<Task> tasks_;
  std::mutex queue_mutex_;
  std::condition_variable cv_;
  std::atomic<bool> shutdown_{false};
  size_t queue_depth_;
  std::mutex socket_mutex_;
  
  void worker_thread();
  bool submit_task(Task task);
};

} // namespace rs::core::transport
