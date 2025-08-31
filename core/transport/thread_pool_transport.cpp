#include "thread_pool_transport.h"
#include <algorithm>

namespace rs::core::transport {

class ThreadPoolTransport::ThreadPoolOperation : public AsyncOperation {
public:
  ThreadPoolOperation() = default;
  
  void cancel() override {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
  }
  
  bool is_complete() const override {
    std::lock_guard lock(mutex_);
    return complete_;
  }
  
  bool is_cancelled() const override {
    std::lock_guard lock(mutex_);
    return cancelled_;
  }
  
  void set_complete() {
    std::lock_guard lock(mutex_);
    complete_ = true;
  }

private:
  mutable std::mutex mutex_;
  bool complete_{false};
  bool cancelled_{false};
};

ThreadPoolTransport::ThreadPoolTransport(size_t thread_count) {
  thread_count = std::max(size_t{1}, thread_count);
  
  for (size_t i = 0; i < thread_count; ++i) {
    workers_.emplace_back(&ThreadPoolTransport::worker_thread, this);
  }
}

ThreadPoolTransport::~ThreadPoolTransport() {
  shutdown_.store(true);
  cv_.notify_all();
  
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

rs::util::Result<void> ThreadPoolTransport::connect(std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  return socket_.connect(host, port, deadline);
}

rs::util::Result<IOResult> ThreadPoolTransport::send(std::span<const std::byte> buf, rs::util::Deadline deadline) {
  return socket_.send(buf, deadline);
}

rs::util::Result<IOResult> ThreadPoolTransport::recv(std::span<std::byte> buf, rs::util::Deadline deadline) {
  return socket_.recv(buf, deadline);
}

void ThreadPoolTransport::close() noexcept {
  socket_.close();
}

std::unique_ptr<AsyncOperation> ThreadPoolTransport::connect_async(
    std::string_view host, uint16_t port, rs::util::Deadline deadline,
    ConnectCallback callback) {
  
  auto op = std::make_unique<ThreadPoolOperation>();
  auto op_ptr = op.get();
  
  std::string host_str(host);
  
  Task task{
    [this, host_str, port, deadline, callback, op_ptr]() {
      if (op_ptr->is_cancelled()) return;
      
      auto result = socket_.connect(host_str, port, deadline);
      op_ptr->set_complete();
      callback(std::move(result));
    },
    deadline
  };
  
  submit_task(std::move(task));
  return std::move(op);
}

std::unique_ptr<AsyncOperation> ThreadPoolTransport::send_async(
    std::span<const std::byte> buf, rs::util::Deadline deadline,
    SendCallback callback) {
  
  auto op = std::make_unique<ThreadPoolOperation>();
  auto op_ptr = op.get();
  
  // Copy buffer data for async operation
  std::vector<std::byte> buffer_copy(buf.begin(), buf.end());
  
  Task task{
    [this, buffer_copy = std::move(buffer_copy), deadline, callback, op_ptr]() {
      if (op_ptr->is_cancelled()) return;
      
      auto result = socket_.send(buffer_copy, deadline);
      op_ptr->set_complete();
      callback(std::move(result));
    },
    deadline
  };
  
  submit_task(std::move(task));
  return std::move(op);
}

std::unique_ptr<AsyncOperation> ThreadPoolTransport::recv_async(
    std::span<std::byte> buf, rs::util::Deadline deadline,
    RecvCallback callback) {
  
  auto op = std::make_unique<ThreadPoolOperation>();
  auto op_ptr = op.get();
  
  Task task{
    [this, buf, deadline, callback, op_ptr]() {
      if (op_ptr->is_cancelled()) return;
      
      auto result = socket_.recv(buf, deadline);
      op_ptr->set_complete();
      callback(std::move(result));
    },
    deadline
  };
  
  submit_task(std::move(task));
  return std::move(op);
}

std::future<rs::util::Result<void>> ThreadPoolTransport::connect_future(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  
  auto promise = std::make_shared<std::promise<rs::util::Result<void>>>();
  auto future = promise->get_future();
  
  connect_async(host, port, deadline, 
    [promise](rs::util::Result<void> result) {
      promise->set_value(std::move(result));
    });
  
  return future;
}

std::future<rs::util::Result<IOResult>> ThreadPoolTransport::send_future(
    std::span<const std::byte> buf, rs::util::Deadline deadline) {
  
  auto promise = std::make_shared<std::promise<rs::util::Result<IOResult>>>();
  auto future = promise->get_future();
  
  send_async(buf, deadline,
    [promise](rs::util::Result<IOResult> result) {
      promise->set_value(std::move(result));
    });
  
  return future;
}

std::future<rs::util::Result<IOResult>> ThreadPoolTransport::recv_future(
    std::span<std::byte> buf, rs::util::Deadline deadline) {
  
  auto promise = std::make_shared<std::promise<rs::util::Result<IOResult>>>();
  auto future = promise->get_future();
  
  recv_async(buf, deadline,
    [promise](rs::util::Result<IOResult> result) {
      promise->set_value(std::move(result));
    });
  
  return future;
}

void ThreadPoolTransport::worker_thread() {
  while (!shutdown_.load()) {
    std::unique_lock lock(queue_mutex_);
    
    cv_.wait(lock, [this] { return !tasks_.empty() || shutdown_.load(); });
    
    if (shutdown_.load()) break;
    
    if (tasks_.empty()) continue;
    
    auto task = std::move(tasks_.front());
    tasks_.pop();
    lock.unlock();
    
    // Check if task has expired
    auto now = std::chrono::steady_clock::now();
    if (now > task.deadline) {
      continue; // Skip expired task
    }
    
    // Execute task
    try {
      task.work();
    } catch (...) {
      // Log error in production
    }
  }
}

void ThreadPoolTransport::submit_task(Task task) {
  {
    std::lock_guard lock(queue_mutex_);
    tasks_.push(std::move(task));
  }
  cv_.notify_one();
}

} // namespace rs::core::transport