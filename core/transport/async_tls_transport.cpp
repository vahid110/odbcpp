#include "async_tls_transport.h"
#include "core/security/tls_client.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rs::core::transport {
namespace {

class OperationState {
public:
  void set_cancel_hook(std::function<void()> hook) {
    std::lock_guard lock(mutex_);
    cancel_hook_ = std::move(hook);
  }

  void cancel() {
    std::function<void()> hook;
    {
      std::lock_guard lock(mutex_);
      if (complete_ || cancellation_requested_) return;
      cancellation_requested_ = true;
      cancelled_ = true;
      hook = cancel_hook_;
    }
    if (hook) hook();
  }

  bool finish() {
    return finish_with([](bool) {});
  }

  template<typename Action>
  bool finish_with(Action action) {
    std::lock_guard lock(mutex_);
    if (complete_) return false;
    action(cancellation_requested_);
    complete_ = true;
    cancel_hook_ = {};
    return true;
  }

  bool cancellation_requested() const {
    std::lock_guard lock(mutex_);
    return cancellation_requested_;
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
  bool cancellation_requested_{false};
  bool cancelled_{false};
  bool complete_{false};
  std::function<void()> cancel_hook_;
};

class TlsOperation final : public AsyncOperation {
public:
  explicit TlsOperation(std::shared_ptr<OperationState> state)
      : state_(std::move(state)) {}

  void cancel() override { state_->cancel(); }
  bool is_complete() const override { return state_->is_complete(); }
  bool is_cancelled() const override { return state_->is_cancelled(); }

private:
  std::shared_ptr<OperationState> state_;
};

template<typename Callback, typename ResultType>
void invoke_safely(Callback& callback, ResultType result) noexcept {
  try {
    callback(std::move(result));
  } catch (...) {
    // User callbacks must not terminate the TLS worker.
  }
}

template<typename T>
rs::util::Result<T> cancelled_result() {
  return rs::util::Result<T>{rs::util::DbErrorCode::NetworkError,
                             "async TLS operation cancelled"};
}

} // namespace

class AsyncTlsTransport::Core : public std::enable_shared_from_this<Core> {
public:
  enum class TaskKind { ConnectTls, ConnectPlain, Upgrade, Send, Recv };

  struct Task {
    TaskKind kind{};
    std::shared_ptr<OperationState> state;
    rs::util::Deadline deadline{};
    std::string host;
    uint16_t port{};
    std::vector<std::byte> send_buffer;
    std::span<std::byte> recv_target;
    std::vector<std::byte> recv_storage;
    ConnectCallback connect_callback;
    SendCallback send_callback;
    RecvCallback recv_callback;
  };

  Core(std::unique_ptr<IAsyncTransport> transport,
       std::size_t max_inflight, std::size_t queue_depth)
      : transport_(std::move(transport)),
        max_inflight_(max_inflight),
        queue_depth_(queue_depth) {
    if (!transport_) {
      throw std::invalid_argument("async TLS requires an async transport");
    }
    if (max_inflight_ == 0) {
      throw std::invalid_argument("max_inflight must be greater than zero");
    }
    if (queue_depth_ == 0) {
      throw std::invalid_argument("queue_depth must be greater than zero");
    }
    if (max_inflight_ > queue_depth_) {
      throw std::invalid_argument("max_inflight cannot exceed queue_depth");
    }
  }

  ~Core() {
    shutdown();
    std::lock_guard lock(tls_mutex_);
    reset_ssl_locked();
  }

  void start() {
    worker_ = std::thread([self = shared_from_this()] { self->worker_loop(); });
  }

  bool submit(const std::shared_ptr<Task>& task) {
    std::vector<std::shared_ptr<Task>> expired;
    bool accepted = false;
    bool expired_incoming = false;
    {
      std::lock_guard lock(queue_mutex_);
      if (!stopping_) {
        if (task->deadline <= rs::util::Clock::now()) {
          expired_incoming = true;
        } else {
          const bool connecting = task->kind == TaskKind::ConnectTls ||
                                  task->kind == TaskKind::ConnectPlain;
          const std::size_t active_count = active_ ? 1 : 0;
          if (tasks_.size() + active_count >= queue_depth_ ||
              (connecting && has_pending_connect_locked())) {
            const auto now = rs::util::Clock::now();
            for (auto it = tasks_.begin(); it != tasks_.end();) {
              if ((*it)->deadline <= now) {
                expired.push_back(std::move(*it));
                it = tasks_.erase(it);
              } else {
                ++it;
              }
            }
            if (!active_ && tasks_.empty()) idle_.notify_all();
          }
          if (task->deadline <= rs::util::Clock::now()) {
            expired_incoming = true;
          } else if (tasks_.size() + active_count < queue_depth_ &&
                     (!connecting || !has_pending_connect_locked())) {
            tasks_.push_back(task);
            accepted = true;
          }
        }
      }
    }
    if (accepted) queue_ready_.notify_one();
    for (const auto& queued : expired) complete_expired_task(queued);
    if (expired_incoming) complete_expired_task(task);
    return accepted || expired_incoming;
  }

  void request_cancel(const std::shared_ptr<OperationState>& state) noexcept {
    bool interrupt = false;
    std::shared_ptr<Task> queued;
    {
      std::lock_guard lock(queue_mutex_);
      interrupt = active_ && active_->state == state;
      if (!interrupt) {
        const auto it = std::find_if(tasks_.begin(), tasks_.end(),
            [&](const auto& task) { return task->state == state; });
        if (it != tasks_.end()) {
          queued = std::move(*it);
          tasks_.erase(it);
          if (!active_ && tasks_.empty()) idle_.notify_all();
        }
      }
    }
    if (queued) complete_cancelled_task(queued);
    if (interrupt) transport_->close();
    queue_ready_.notify_one();
  }

  void close_transport() noexcept {
    std::vector<std::shared_ptr<OperationState>> states;
    bool called_from_worker = false;
    {
      std::lock_guard lock(queue_mutex_);
      called_from_worker = worker_.joinable() &&
          worker_.get_id() == std::this_thread::get_id();
      if (active_) states.push_back(active_->state);
      for (const auto& task : tasks_) states.push_back(task->state);
    }
    for (const auto& state : states) state->cancel();
    transport_->close();

    if (!called_from_worker) {
      std::unique_lock lock(queue_mutex_);
      idle_.wait(lock, [this] { return !active_ && tasks_.empty(); });
    }

    std::lock_guard lock(tls_mutex_);
    reset_ssl_locked();
    transport_->close();
  }

  void shutdown() noexcept {
    std::vector<std::shared_ptr<OperationState>> states;
    {
      std::lock_guard lock(queue_mutex_);
      if (stopping_) return;
      stopping_ = true;
      if (active_) states.push_back(active_->state);
      for (const auto& task : tasks_) states.push_back(task->state);
    }
    for (const auto& state : states) state->cancel();
    transport_->close();
    queue_ready_.notify_all();
    if (!worker_.joinable()) return;
    if (worker_.get_id() == std::this_thread::get_id()) {
      worker_.detach();
    } else {
      worker_.join();
    }
  }

  void set_min_tls_version(long version) {
    std::lock_guard lock(tls_mutex_);
    min_tls_version_ = version;
    tls_.reset_context_if_inactive();
  }

  void set_verify(bool enabled) {
    std::lock_guard lock(tls_mutex_);
    verify_ = enabled;
    if (!enabled) peer_identity_verified_ = false;
    tls_.reset_context_if_inactive();
  }

  void set_hostname_verification(bool enabled) {
    std::lock_guard lock(tls_mutex_);
    verify_hostname_ = enabled;
    if (!enabled) peer_identity_verified_ = false;
  }

  void set_ca_locations(std::string file, std::string directory) {
    std::lock_guard lock(tls_mutex_);
    ca_file_ = std::move(file);
    ca_directory_ = std::move(directory);
    tls_.reset_context_if_inactive();
  }

  bool peer_identity_verified() noexcept {
    std::lock_guard lock(tls_mutex_);
    return peer_identity_verified_;
  }

  std::size_t max_inflight() const noexcept { return max_inflight_; }
  std::size_t queue_depth() const noexcept { return queue_depth_; }

private:
  static void complete_cancelled_task(const std::shared_ptr<Task>& task) noexcept {
    if (!task->state->finish()) return;
    try {
      if (task->kind == TaskKind::ConnectTls ||
          task->kind == TaskKind::ConnectPlain ||
          task->kind == TaskKind::Upgrade) {
        invoke_safely(task->connect_callback, cancelled_result<void>());
      } else if (task->kind == TaskKind::Send) {
        invoke_safely(task->send_callback, cancelled_result<IOResult>());
      } else {
        invoke_safely(task->recv_callback, cancelled_result<IOResult>());
      }
    } catch (...) {
      // Cancellation cannot unwind through shutdown.
    }
  }

  static void complete_expired_task(const std::shared_ptr<Task>& task) noexcept {
    bool cancelled = false;
    if (!task->state->finish_with([&](bool requested) {
          cancelled = requested;
        })) return;
    try {
      if (task->kind == TaskKind::ConnectTls ||
          task->kind == TaskKind::ConnectPlain ||
          task->kind == TaskKind::Upgrade) {
        invoke_safely(task->connect_callback, cancelled
            ? cancelled_result<void>()
            : rs::util::Result<void>{rs::util::DbErrorCode::Timeout,
                "async TLS operation deadline expired in queue"});
      } else if (task->kind == TaskKind::Send) {
        invoke_safely(task->send_callback, cancelled
            ? cancelled_result<IOResult>()
            : rs::util::Result<IOResult>{rs::util::DbErrorCode::Timeout,
                "async TLS operation deadline expired in queue"});
      } else {
        invoke_safely(task->recv_callback, cancelled
            ? cancelled_result<IOResult>()
            : rs::util::Result<IOResult>{rs::util::DbErrorCode::Timeout,
                "async TLS operation deadline expired in queue"});
      }
    } catch (...) {
      // Queue reclamation cannot unwind through submission.
    }
  }

  bool has_pending_connect_locked() const {
    if (active_ && (active_->kind == TaskKind::ConnectTls ||
                    active_->kind == TaskKind::ConnectPlain)) {
      return true;
    }
    return std::any_of(tasks_.begin(), tasks_.end(), [](const auto& task) {
      return task->kind == TaskKind::ConnectTls ||
          task->kind == TaskKind::ConnectPlain;
    });
  }

  void worker_loop() {
    for (;;) {
      std::shared_ptr<Task> task;
      {
        std::unique_lock lock(queue_mutex_);
        queue_ready_.wait(lock, [this] {
          return stopping_ || !tasks_.empty();
        });
        if (tasks_.empty()) {
          if (stopping_) return;
          continue;
        }
        task = tasks_.front();
        tasks_.pop_front();
        active_ = task;
      }

      if (task->kind == TaskKind::ConnectTls ||
          task->kind == TaskKind::ConnectPlain ||
          task->kind == TaskKind::Upgrade) {
        auto result = execute_connect_task(*task);
        if (task->state->cancellation_requested()) {
          result = cancelled_result<void>();
        }
        finish_active(task);
        if (task->state->finish()) {
          invoke_safely(task->connect_callback, std::move(result));
        }
      } else {
        auto result = task->kind == TaskKind::Send
            ? execute_send_task(*task) : execute_recv_task(*task);
        if (task->kind == TaskKind::Recv) {
          const bool completed = task->state->finish_with([&](bool cancelled) {
            if (cancelled) {
              result = cancelled_result<IOResult>();
            } else if (result.has_value() && result->n != 0 &&
                       result->n <= task->recv_target.size()) {
              std::memcpy(task->recv_target.data(), task->recv_storage.data(),
                          result->n);
            }
          });
          finish_active(task);
          if (completed) invoke_safely(task->recv_callback, std::move(result));
        } else {
          if (task->state->cancellation_requested()) {
            result = cancelled_result<IOResult>();
          }
          finish_active(task);
          if (task->state->finish()) {
            invoke_safely(task->send_callback, std::move(result));
          }
        }
      }
    }
  }

  void finish_active(const std::shared_ptr<Task>& task) {
    std::lock_guard lock(queue_mutex_);
    if (active_ == task) active_.reset();
    if (tasks_.empty()) idle_.notify_all();
  }

  rs::util::Result<void> execute_connect_task(Task& task) {
    std::lock_guard lock(tls_mutex_);
    if (task.state->cancellation_requested()) {
      if (task.kind == TaskKind::Upgrade) transport_->close();
      return cancelled_result<void>();
    }

    if (task.host.find('\0') != std::string::npos) {
      reset_ssl_locked();
      transport_->close();
      return {rs::util::DbErrorCode::InvalidParameter,
              "TLS host contains an embedded NUL byte"};
    }

    if (task.kind == TaskKind::ConnectTls ||
        task.kind == TaskKind::ConnectPlain) {
      reset_ssl_locked();
      auto connected = transport_->connect(task.host, task.port, task.deadline);
      if (connected.has_error()) return connected;
      if (task.kind == TaskKind::ConnectPlain) return {};
    }
    auto result = handshake_locked(task.host, task.deadline);
    if (result.has_error() || task.state->cancellation_requested()) {
      reset_ssl_locked();
      transport_->close();
    }
    return result;
  }

  rs::util::Result<IOResult> execute_send_task(Task& task) {
    std::lock_guard lock(tls_mutex_);
    if (task.state->cancellation_requested()) {
      return cancelled_result<IOResult>();
    }
    if (!tls_.active()) return transport_->send(task.send_buffer, task.deadline);
    if (task.send_buffer.empty()) {
      return transport_->send(task.send_buffer, task.deadline);
    }

    std::size_t written_total = 0;
    while (written_total < task.send_buffer.size()) {
      if (rs::util::remaining(task.deadline) <=
          std::chrono::milliseconds::zero()) {
        return {rs::util::DbErrorCode::Timeout, "TLS send deadline expired"};
      }

      const auto status = tls_.write(
          std::span<const std::byte>(task.send_buffer).subspan(written_total));
      auto flushed = flush_ciphertext_locked(task.deadline);
      if (flushed.has_error()) {
        return {flushed.error(), flushed.error_message()};
      }
      if (status.state == rs::core::security::TlsStepState::Complete) {
        if (status.processed == 0) {
          return {rs::util::DbErrorCode::NetworkError,
                  "TLS plaintext send made no progress"};
        }
        written_total += status.processed;
        continue;
      }

      if (status.state == rs::core::security::TlsStepState::WantRead) {
        auto received = receive_ciphertext_locked(task.deadline);
        if (received.has_error()) {
          return {received.error(), received.error_message()};
        }
        if (*received) {
          return {rs::util::DbErrorCode::TLSError,
                  "TLS peer closed during send"};
        }
      } else if (status.state != rs::core::security::TlsStepState::WantWrite) {
        return {rs::util::DbErrorCode::TLSError,
                status.message};
      }
    }
    return IOResult{written_total, false};
  }

  rs::util::Result<IOResult> execute_recv_task(Task& task) {
    std::lock_guard lock(tls_mutex_);
    if (task.state->cancellation_requested()) {
      return cancelled_result<IOResult>();
    }
    if (!tls_.active()) return transport_->recv(task.recv_storage, task.deadline);
    if (task.recv_storage.empty()) {
      return transport_->recv(task.recv_storage, task.deadline);
    }

    for (;;) {
      if (rs::util::remaining(task.deadline) <=
          std::chrono::milliseconds::zero()) {
        return {rs::util::DbErrorCode::Timeout,
                "TLS receive deadline expired"};
      }

      const auto status = tls_.read(task.recv_storage);
      auto flushed = flush_ciphertext_locked(task.deadline);
      if (flushed.has_error()) {
        return {flushed.error(), flushed.error_message()};
      }
      if (status.state == rs::core::security::TlsStepState::Complete) {
        return IOResult{status.processed, false};
      }

      if (status.state == rs::core::security::TlsStepState::Closed) {
        return IOResult{0, true};
      }
      if (status.state == rs::core::security::TlsStepState::WantRead) {
        auto received = receive_ciphertext_locked(task.deadline);
        if (received.has_error()) {
          return {received.error(), received.error_message()};
        }
        if (*received) {
          return {rs::util::DbErrorCode::TLSError,
                  "TLS peer closed without close_notify"};
        }
      } else if (status.state != rs::core::security::TlsStepState::WantWrite) {
        return {rs::util::DbErrorCode::TLSError,
                status.message};
      }
    }
  }

  rs::util::Result<void> handshake_locked(
      std::string_view host, rs::util::Deadline deadline) {
    reset_ssl_locked();
    tls_.configure({min_tls_version_, verify_, verify_hostname_,
                    ca_file_, ca_directory_});
    const auto created = tls_.begin_memory(host);
    if (created.state != rs::core::security::TlsStepState::Complete) {
      return {rs::util::DbErrorCode::TLSError, created.message};
    }

    for (;;) {
      if (rs::util::remaining(deadline) <=
          std::chrono::milliseconds::zero()) {
        reset_ssl_locked();
        return {rs::util::DbErrorCode::Timeout,
                "TLS handshake deadline expired"};
      }

      const auto status = tls_.handshake();
      auto flushed = flush_ciphertext_locked(deadline);
      if (flushed.has_error()) {
        reset_ssl_locked();
        return flushed;
      }
      if (status.state == rs::core::security::TlsStepState::Complete) break;

      if (status.state == rs::core::security::TlsStepState::WantRead) {
        auto received = receive_ciphertext_locked(deadline);
        if (received.has_error()) {
          reset_ssl_locked();
          return {received.error(), received.error_message()};
        }
        if (*received) {
          reset_ssl_locked();
          return {rs::util::DbErrorCode::TLSError,
                  "TLS peer closed during handshake"};
        }
      } else if (status.state != rs::core::security::TlsStepState::WantWrite) {
        const auto message = status.message;
        reset_ssl_locked();
        return {rs::util::DbErrorCode::TLSError, message};
      }
    }

    peer_identity_verified_ = tls_.peer_identity_verified();
    return {};
  }

  rs::util::Result<void> flush_ciphertext_locked(
      rs::util::Deadline deadline) {
    std::array<std::byte, 16 * 1024> buffer{};
    while (tls_.ciphertext_pending()) {
      const auto drained = tls_.drain_ciphertext(buffer);
      if (drained.state != rs::core::security::TlsStepState::Complete ||
          drained.processed == 0) {
        return {rs::util::DbErrorCode::TLSError,
                drained.message.empty()
                    ? "TLS ciphertext drain made no progress"
                    : drained.message};
      }
      std::size_t offset = 0;
      const auto size = drained.processed;
      while (offset < size) {
        auto sent = transport_->send(
            std::span<const std::byte>(buffer.data() + offset, size - offset),
            deadline);
        if (sent.has_error()) {
          return {sent.error(), sent.error_message()};
        }
        if (sent->n == 0) {
          return {rs::util::DbErrorCode::NetworkError,
                  "TLS ciphertext send made no progress"};
        }
        offset += sent->n;
      }
    }
    return {};
  }

  rs::util::Result<bool> receive_ciphertext_locked(
      rs::util::Deadline deadline) {
    std::array<std::byte, 16 * 1024> buffer{};
    auto received = transport_->recv(buffer, deadline);
    if (received.has_error()) {
      return {received.error(), received.error_message()};
    }
    if (received->eof || received->n == 0) return true;

    std::size_t offset = 0;
    while (offset < received->n) {
      const auto supplied = tls_.provide_ciphertext(
          std::span<const std::byte>(buffer).subspan(
              offset, received->n - offset));
      if (supplied.state != rs::core::security::TlsStepState::Complete ||
          supplied.processed == 0) {
        return {rs::util::DbErrorCode::TLSError,
                supplied.message.empty()
                    ? "TLS ciphertext supply made no progress"
                    : supplied.message};
      }
      offset += supplied.processed;
    }
    return false;
  }

  void reset_ssl_locked() noexcept {
    peer_identity_verified_ = false;
    tls_.reset_session();
  }

  std::unique_ptr<IAsyncTransport> transport_;
  const std::size_t max_inflight_;
  const std::size_t queue_depth_;
  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::condition_variable idle_;
  std::deque<std::shared_ptr<Task>> tasks_;
  std::shared_ptr<Task> active_;
  bool stopping_{false};
  std::thread worker_;

  std::mutex tls_mutex_;
  rs::core::security::TlsClient tls_;
  bool verify_{true};
  bool verify_hostname_{true};
  bool peer_identity_verified_{false};
  long min_tls_version_{0x0303};
  std::string ca_file_;
  std::string ca_directory_;
};

AsyncTlsTransport::AsyncTlsTransport(
    std::unique_ptr<IAsyncTransport> transport, std::size_t max_inflight,
    std::size_t queue_depth)
    : core_(std::make_shared<Core>(std::move(transport), max_inflight,
                                   queue_depth)) {
  core_->start();
}

AsyncTlsTransport::~AsyncTlsTransport() { core_->shutdown(); }

rs::util::Result<void> AsyncTlsTransport::connect(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  return connect_future(host, port, deadline).get();
}

rs::util::Result<IOResult> AsyncTlsTransport::send(
    std::span<const std::byte> buf, rs::util::Deadline deadline) {
  return send_future(buf, deadline).get();
}

rs::util::Result<IOResult> AsyncTlsTransport::recv(
    std::span<std::byte> buf, rs::util::Deadline deadline) {
  return recv_future(buf, deadline).get();
}

void AsyncTlsTransport::close() noexcept { core_->close_transport(); }

rs::util::Result<void> AsyncTlsTransport::connect_plain(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  auto promise = std::make_shared<std::promise<rs::util::Result<void>>>();
  auto future = promise->get_future();
  auto state = std::make_shared<OperationState>();
  std::weak_ptr<Core> weak_core = core_;
  std::weak_ptr<OperationState> weak_state = state;
  state->set_cancel_hook([weak_core, weak_state] {
    if (auto core = weak_core.lock()) {
      if (auto locked_state = weak_state.lock()) {
        core->request_cancel(locked_state);
      }
    }
  });
  auto task = std::make_shared<Core::Task>();
  task->kind = Core::TaskKind::ConnectPlain;
  task->state = state;
  task->deadline = deadline;
  task->host = std::string(host);
  task->port = port;
  task->connect_callback = [promise](rs::util::Result<void> result) {
    promise->set_value(std::move(result));
  };
  if (!core_->submit(task) && state->finish()) {
    task->connect_callback({rs::util::DbErrorCode::NetworkError,
                            "async TLS queue is full"});
  }
  auto result = future.get();
  if (result.has_error()) core_->close_transport();
  return result;
}

rs::util::Result<void> AsyncTlsTransport::upgrade_to_tls(
    std::string_view host, rs::util::Deadline deadline) {
  auto promise = std::make_shared<std::promise<rs::util::Result<void>>>();
  auto future = promise->get_future();
  auto state = std::make_shared<OperationState>();
  std::weak_ptr<Core> weak_core = core_;
  std::weak_ptr<OperationState> weak_state = state;
  state->set_cancel_hook([weak_core, weak_state] {
    if (auto core = weak_core.lock()) {
      if (auto locked_state = weak_state.lock()) {
        core->request_cancel(locked_state);
      }
    }
  });
  auto task = std::make_shared<Core::Task>();
  task->kind = Core::TaskKind::Upgrade;
  task->state = state;
  task->deadline = deadline;
  task->host = std::string(host);
  task->connect_callback = [promise](rs::util::Result<void> result) {
    promise->set_value(std::move(result));
  };
  if (!core_->submit(task) && state->finish()) {
    task->connect_callback({rs::util::DbErrorCode::NetworkError,
                            "async TLS queue is full"});
  }
  auto result = future.get();
  if (result.has_error()) core_->close_transport();
  return result;
}

bool AsyncTlsTransport::peer_identity_verified() noexcept {
  return core_->peer_identity_verified();
}

std::unique_ptr<AsyncOperation> AsyncTlsTransport::connect_async(
    std::string_view host, uint16_t port, rs::util::Deadline deadline,
    ConnectCallback callback) {
  auto state = std::make_shared<OperationState>();
  std::weak_ptr<Core> weak_core = core_;
  std::weak_ptr<OperationState> weak_state = state;
  state->set_cancel_hook([weak_core, weak_state] {
    if (auto core = weak_core.lock()) {
      if (auto locked_state = weak_state.lock()) {
        core->request_cancel(locked_state);
      }
    }
  });
  auto operation = std::make_unique<TlsOperation>(state);
  auto task = std::make_shared<Core::Task>();
  task->kind = Core::TaskKind::ConnectTls;
  task->state = state;
  task->deadline = deadline;
  task->host = std::string(host);
  task->port = port;
  task->connect_callback = std::move(callback);
  if (!core_->submit(task) && state->finish()) {
    invoke_safely(task->connect_callback, rs::util::Result<void>{
        rs::util::DbErrorCode::NetworkError,
        "async TLS queue is full or a connect is already pending"});
  }
  return operation;
}

std::unique_ptr<AsyncOperation> AsyncTlsTransport::send_async(
    std::span<const std::byte> buf, rs::util::Deadline deadline,
    SendCallback callback) {
  auto state = std::make_shared<OperationState>();
  std::weak_ptr<Core> weak_core = core_;
  std::weak_ptr<OperationState> weak_state = state;
  state->set_cancel_hook([weak_core, weak_state] {
    if (auto core = weak_core.lock()) {
      if (auto locked_state = weak_state.lock()) {
        core->request_cancel(locked_state);
      }
    }
  });
  auto operation = std::make_unique<TlsOperation>(state);
  auto task = std::make_shared<Core::Task>();
  task->kind = Core::TaskKind::Send;
  task->state = state;
  task->deadline = deadline;
  task->send_buffer.assign(buf.begin(), buf.end());
  task->send_callback = std::move(callback);
  if (!core_->submit(task) && state->finish()) {
    invoke_safely(task->send_callback, rs::util::Result<IOResult>{
        rs::util::DbErrorCode::NetworkError, "async TLS queue is full"});
  }
  return operation;
}

std::unique_ptr<AsyncOperation> AsyncTlsTransport::recv_async(
    std::span<std::byte> buf, rs::util::Deadline deadline,
    RecvCallback callback) {
  auto state = std::make_shared<OperationState>();
  std::weak_ptr<Core> weak_core = core_;
  std::weak_ptr<OperationState> weak_state = state;
  state->set_cancel_hook([weak_core, weak_state] {
    if (auto core = weak_core.lock()) {
      if (auto locked_state = weak_state.lock()) {
        core->request_cancel(locked_state);
      }
    }
  });
  auto operation = std::make_unique<TlsOperation>(state);
  auto task = std::make_shared<Core::Task>();
  task->kind = Core::TaskKind::Recv;
  task->state = state;
  task->deadline = deadline;
  task->recv_target = buf;
  task->recv_storage.resize(buf.size());
  task->recv_callback = std::move(callback);
  if (!core_->submit(task) && state->finish()) {
    invoke_safely(task->recv_callback, rs::util::Result<IOResult>{
        rs::util::DbErrorCode::NetworkError, "async TLS queue is full"});
  }
  return operation;
}

std::future<rs::util::Result<void>> AsyncTlsTransport::connect_future(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  auto promise = std::make_shared<std::promise<rs::util::Result<void>>>();
  auto future = promise->get_future();
  (void)connect_async(host, port, deadline,
      [promise](rs::util::Result<void> result) mutable {
        promise->set_value(std::move(result));
      });
  return future;
}

std::future<rs::util::Result<IOResult>> AsyncTlsTransport::send_future(
    std::span<const std::byte> buf, rs::util::Deadline deadline) {
  auto promise =
      std::make_shared<std::promise<rs::util::Result<IOResult>>>();
  auto future = promise->get_future();
  (void)send_async(buf, deadline,
      [promise](rs::util::Result<IOResult> result) mutable {
        promise->set_value(std::move(result));
      });
  return future;
}

std::future<rs::util::Result<IOResult>> AsyncTlsTransport::recv_future(
    std::span<std::byte> buf, rs::util::Deadline deadline) {
  auto promise =
      std::make_shared<std::promise<rs::util::Result<IOResult>>>();
  auto future = promise->get_future();
  (void)recv_async(buf, deadline,
      [promise](rs::util::Result<IOResult> result) mutable {
        promise->set_value(std::move(result));
      });
  return future;
}

void AsyncTlsTransport::set_min_tls_version(long version) {
  core_->set_min_tls_version(version);
}

void AsyncTlsTransport::set_verify(bool enabled) {
  core_->set_verify(enabled);
}

void AsyncTlsTransport::set_hostname_verification(bool enabled) {
  core_->set_hostname_verification(enabled);
}

void AsyncTlsTransport::set_ca_locations(
    const std::string& file, const std::string& directory) {
  core_->set_ca_locations(file, directory);
}

std::size_t AsyncTlsTransport::max_inflight() const noexcept {
  return core_->max_inflight();
}

std::size_t AsyncTlsTransport::queue_depth() const noexcept {
  return core_->queue_depth();
}

} // namespace rs::core::transport
