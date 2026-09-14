#include "thread_pool_transport.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace rs::core::transport {
namespace {

class OperationState {
public:
  explicit OperationState(std::function<void()> cancel_callback)
      : cancel_callback_(std::move(cancel_callback)) {}

  void cancel() {
    std::function<void()> callback;
    {
      std::lock_guard lock(mutex_);
      if (complete_) return;
      cancelled_ = true;
      complete_ = true;
      callback = std::move(cancel_callback_);
    }
    if (callback) {
      try {
        callback();
      } catch (...) {
        // User callbacks must not unwind through cancellation or destruction.
      }
    }
  }

  bool finish() {
    return finish_with([] {});
  }

  template<typename BeforeFinish>
  bool finish_with(BeforeFinish before_finish) {
    std::lock_guard lock(mutex_);
    if (complete_) return false;
    before_finish();
    complete_ = true;
    cancel_callback_ = {};
    return true;
  }

  bool is_complete() const {
    std::lock_guard lock(mutex_);
    return complete_;
  }

  bool is_cancelled() const {
    std::lock_guard lock(mutex_);
    return cancelled_;
  }

private:
  mutable std::mutex mutex_;
  bool complete_{false};
  bool cancelled_{false};
  std::function<void()> cancel_callback_;
};

template<typename ResultType, typename Callback>
std::shared_ptr<OperationState> make_operation_state(Callback callback) {
  return std::make_shared<OperationState>([callback = std::move(callback)]() mutable {
    callback(ResultType{rs::util::DbErrorCode::NetworkError,
                        "async operation cancelled"});
  });
}

bool expired(rs::util::Deadline deadline) {
  return rs::util::remaining(deadline) <= std::chrono::milliseconds::zero();
}

} // namespace

class ThreadPoolTransport::ThreadPoolOperation : public AsyncOperation {
public:
  explicit ThreadPoolOperation(std::shared_ptr<OperationState> state)
      : state_(std::move(state)) {}

  void cancel() override { state_->cancel(); }
  bool is_complete() const override { return state_->is_complete(); }
  bool is_cancelled() const override { return state_->is_cancelled(); }

private:
  std::shared_ptr<OperationState> state_;
};

ThreadPoolTransport::ThreadPoolTransport(size_t thread_count, size_t queue_depth)
    : queue_depth_(queue_depth) {
  thread_count = std::max(size_t{1}, thread_count);
  if (queue_depth_ == 0) {
    throw std::invalid_argument("queue_depth must be greater than zero");
  }
  for (size_t i = 0; i < thread_count; ++i) {
    workers_.emplace_back(&ThreadPoolTransport::worker_thread, this);
  }
}

ThreadPoolTransport::~ThreadPoolTransport() {
  std::queue<Task> pending;
  {
    std::lock_guard lock(queue_mutex_);
    shutdown_.store(true);
    pending.swap(tasks_);
  }
  cv_.notify_all();

  while (!pending.empty()) {
    if (pending.front().cancel) pending.front().cancel();
    pending.pop();
  }

  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

rs::util::Result<void> ThreadPoolTransport::connect(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  std::lock_guard lock(socket_mutex_);
  return socket_.connect(host, port, deadline);
}

rs::util::Result<IOResult> ThreadPoolTransport::send(
    std::span<const std::byte> buf, rs::util::Deadline deadline) {
  std::lock_guard lock(socket_mutex_);
  return socket_.send(buf, deadline);
}

rs::util::Result<IOResult> ThreadPoolTransport::recv(
    std::span<std::byte> buf, rs::util::Deadline deadline) {
  std::lock_guard lock(socket_mutex_);
  return socket_.recv(buf, deadline);
}

void ThreadPoolTransport::close() noexcept {
  std::lock_guard lock(socket_mutex_);
  socket_.close();
}

std::unique_ptr<AsyncOperation> ThreadPoolTransport::connect_async(
    std::string_view host, uint16_t port, rs::util::Deadline deadline,
    ConnectCallback callback) {
  auto callback_copy = callback;
  auto state = make_operation_state<rs::util::Result<void>>(callback_copy);
  auto operation = std::make_unique<ThreadPoolOperation>(state);
  std::string host_copy(host);

  Task task{
      [this, state, host = std::move(host_copy), port, deadline,
       callback = std::move(callback)]() mutable {
        if (state->is_complete()) return;
        rs::util::Result<void> result = expired(deadline)
            ? rs::util::Result<void>{rs::util::DbErrorCode::Timeout,
                                     "async connect deadline expired in queue"}
            : connect(host, port, deadline);
        if (state->finish()) callback(std::move(result));
      },
      [state] { state->cancel(); },
      [state] { return state->is_complete(); },
      deadline};

  if (!submit_task(std::move(task))) state->cancel();
  return operation;
}

std::unique_ptr<AsyncOperation> ThreadPoolTransport::send_async(
    std::span<const std::byte> buf, rs::util::Deadline deadline,
    SendCallback callback) {
  auto callback_copy = callback;
  auto state = make_operation_state<rs::util::Result<IOResult>>(callback_copy);
  auto operation = std::make_unique<ThreadPoolOperation>(state);
  std::vector<std::byte> buffer_copy(buf.begin(), buf.end());

  Task task{
      [this, state, buffer = std::move(buffer_copy), deadline,
       callback = std::move(callback)]() mutable {
        if (state->is_complete()) return;
        rs::util::Result<IOResult> result = expired(deadline)
            ? rs::util::Result<IOResult>{rs::util::DbErrorCode::Timeout,
                                         "async send deadline expired in queue"}
            : send(buffer, deadline);
        if (state->finish()) callback(std::move(result));
      },
      [state] { state->cancel(); },
      [state] { return state->is_complete(); },
      deadline};

  if (!submit_task(std::move(task))) state->cancel();
  return operation;
}

std::unique_ptr<AsyncOperation> ThreadPoolTransport::recv_async(
    std::span<std::byte> buf, rs::util::Deadline deadline,
    RecvCallback callback) {
  auto callback_copy = callback;
  auto state = make_operation_state<rs::util::Result<IOResult>>(callback_copy);
  auto operation = std::make_unique<ThreadPoolOperation>(state);
  std::vector<std::byte> buffer(buf.size());

  Task task{
      [this, state, buf, buffer = std::move(buffer), deadline,
       callback = std::move(callback)]() mutable {
        if (state->is_complete()) return;
        rs::util::Result<IOResult> result = expired(deadline)
            ? rs::util::Result<IOResult>{rs::util::DbErrorCode::Timeout,
                                         "async receive deadline expired in queue"}
            : recv(buffer, deadline);
        if (state->finish_with([&] {
              if (result.has_value()) {
                std::copy_n(buffer.begin(), result->n, buf.begin());
              }
            })) {
          callback(std::move(result));
        }
      },
      [state] { state->cancel(); },
      [state] { return state->is_complete(); },
      deadline};

  if (!submit_task(std::move(task))) state->cancel();
  return operation;
}

std::future<rs::util::Result<void>> ThreadPoolTransport::connect_future(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  auto promise = std::make_shared<std::promise<rs::util::Result<void>>>();
  auto future = promise->get_future();
  (void)connect_async(host, port, deadline,
      [promise](rs::util::Result<void> result) mutable {
        promise->set_value(std::move(result));
      });
  return future;
}

std::future<rs::util::Result<IOResult>> ThreadPoolTransport::send_future(
    std::span<const std::byte> buf, rs::util::Deadline deadline) {
  auto promise = std::make_shared<std::promise<rs::util::Result<IOResult>>>();
  auto future = promise->get_future();
  (void)send_async(buf, deadline,
      [promise](rs::util::Result<IOResult> result) mutable {
        promise->set_value(std::move(result));
      });
  return future;
}

std::future<rs::util::Result<IOResult>> ThreadPoolTransport::recv_future(
    std::span<std::byte> buf, rs::util::Deadline deadline) {
  auto promise = std::make_shared<std::promise<rs::util::Result<IOResult>>>();
  auto future = promise->get_future();
  (void)recv_async(buf, deadline,
      [promise](rs::util::Result<IOResult> result) mutable {
        promise->set_value(std::move(result));
      });
  return future;
}

void ThreadPoolTransport::worker_thread() {
  for (;;) {
    Task task;
    {
      std::unique_lock lock(queue_mutex_);
      cv_.wait(lock, [this] { return !tasks_.empty() || shutdown_.load(); });
      if (shutdown_.load() && tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop();
    }

    try {
      task.work();
    } catch (...) {
      if (task.cancel) task.cancel();
    }
  }
}

bool ThreadPoolTransport::submit_task(Task task) {
  std::vector<Task> expired_tasks;
  bool accepted = false;
  {
    std::lock_guard lock(queue_mutex_);
    if (shutdown_.load()) return false;
    if (tasks_.size() >= queue_depth_) {
      std::queue<Task> pending;
      while (!tasks_.empty()) {
        Task queued = std::move(tasks_.front());
        tasks_.pop();
        if (queued.is_complete()) continue;
        if (expired(queued.deadline)) {
          expired_tasks.push_back(std::move(queued));
        } else {
          pending.push(std::move(queued));
        }
      }
      tasks_.swap(pending);
    }
    if (tasks_.size() < queue_depth_) {
      tasks_.push(std::move(task));
      accepted = true;
    }
  }
  if (accepted) cv_.notify_one();
  for (auto& queued : expired_tasks) {
    try {
      queued.work();
    } catch (...) {
      if (queued.cancel) queued.cancel();
    }
  }
  return accepted;
}

} // namespace rs::core::transport
