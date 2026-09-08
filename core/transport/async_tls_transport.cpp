#include "async_tls_transport.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

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
    std::lock_guard lock(mutex_);
    if (complete_) return false;
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

std::string openssl_error_text(const char* operation) {
  const unsigned long error = ::ERR_get_error();
  if (error == 0) return operation;
  std::array<char, 256> message{};
  ::ERR_error_string_n(error, message.data(), message.size());
  return std::string(operation) + ": " + message.data();
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
    if (context_) ::SSL_CTX_free(context_);
  }

  void start() {
    worker_ = std::thread([self = shared_from_this()] { self->worker_loop(); });
  }

  bool submit(const std::shared_ptr<Task>& task) {
    {
      std::lock_guard lock(queue_mutex_);
      const std::size_t active_count = active_ ? 1 : 0;
      if (stopping_ || tasks_.size() + active_count >= queue_depth_) {
        return false;
      }
      if ((task->kind == TaskKind::ConnectTls ||
           task->kind == TaskKind::ConnectPlain) && has_pending_connect_locked()) {
        return false;
      }
      tasks_.push_back(task);
    }
    queue_ready_.notify_one();
    return true;
  }

  void request_cancel(const std::shared_ptr<OperationState>& state) noexcept {
    bool interrupt = false;
    {
      std::lock_guard lock(queue_mutex_);
      interrupt = active_ && active_->state == state;
    }
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
    reset_context_if_disconnected_locked();
  }

  void set_verify(bool enabled) {
    std::lock_guard lock(tls_mutex_);
    verify_ = enabled;
    reset_context_if_disconnected_locked();
  }

  void set_hostname_verification(bool enabled) {
    std::lock_guard lock(tls_mutex_);
    verify_hostname_ = enabled;
  }

  void set_ca_locations(std::string file, std::string directory) {
    std::lock_guard lock(tls_mutex_);
    ca_file_ = std::move(file);
    ca_directory_ = std::move(directory);
    reset_context_if_disconnected_locked();
  }

  std::size_t max_inflight() const noexcept { return max_inflight_; }
  std::size_t queue_depth() const noexcept { return queue_depth_; }

private:
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
        if (task->state->cancellation_requested()) {
          result = cancelled_result<IOResult>();
        }
        if (task->kind == TaskKind::Recv && result.has_value() &&
            result->n <= task->recv_target.size()) {
          std::memcpy(task->recv_target.data(), task->recv_storage.data(),
                      result->n);
        }
        finish_active(task);
        if (task->state->finish()) {
          if (task->kind == TaskKind::Send) {
            invoke_safely(task->send_callback, std::move(result));
          } else {
            invoke_safely(task->recv_callback, std::move(result));
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
    if (task.state->cancellation_requested()) return cancelled_result<void>();

    if (task.kind == TaskKind::ConnectTls ||
        task.kind == TaskKind::ConnectPlain) {
      reset_ssl_locked();
      auto connected = transport_->connect(task.host, task.port, task.deadline);
      if (connected.has_error()) return connected;
      if (task.kind == TaskKind::ConnectPlain) return {};
    }
    return handshake_locked(task.host, task.deadline);
  }

  rs::util::Result<IOResult> execute_send_task(Task& task) {
    std::lock_guard lock(tls_mutex_);
    if (task.state->cancellation_requested()) {
      return cancelled_result<IOResult>();
    }
    if (!ssl_) return transport_->send(task.send_buffer, task.deadline);
    if (task.send_buffer.empty()) return IOResult{0, false};

    std::size_t written_total = 0;
    while (written_total < task.send_buffer.size()) {
      if (rs::util::remaining(task.deadline) <=
          std::chrono::milliseconds::zero()) {
        return {rs::util::DbErrorCode::Timeout, "TLS send deadline expired"};
      }

      std::size_t written = 0;
      const int result = ::SSL_write_ex(
          ssl_, task.send_buffer.data() + written_total,
          task.send_buffer.size() - written_total, &written);
      auto flushed = flush_ciphertext_locked(task.deadline);
      if (flushed.has_error()) {
        return {flushed.error(), flushed.error_message()};
      }
      if (result == 1) {
        written_total += written;
        continue;
      }

      const int error = ::SSL_get_error(ssl_, result);
      if (error == SSL_ERROR_WANT_READ) {
        auto received = receive_ciphertext_locked(task.deadline);
        if (received.has_error()) {
          return {received.error(), received.error_message()};
        }
        if (*received) {
          return {rs::util::DbErrorCode::TLSError,
                  "TLS peer closed during send"};
        }
      } else if (error != SSL_ERROR_WANT_WRITE) {
        return {rs::util::DbErrorCode::TLSError,
                openssl_error_text("SSL_write_ex")};
      }
    }
    return IOResult{written_total, false};
  }

  rs::util::Result<IOResult> execute_recv_task(Task& task) {
    std::lock_guard lock(tls_mutex_);
    if (task.state->cancellation_requested()) {
      return cancelled_result<IOResult>();
    }
    if (!ssl_) return transport_->recv(task.recv_storage, task.deadline);
    if (task.recv_storage.empty()) return IOResult{0, false};

    for (;;) {
      if (rs::util::remaining(task.deadline) <=
          std::chrono::milliseconds::zero()) {
        return {rs::util::DbErrorCode::Timeout,
                "TLS receive deadline expired"};
      }

      std::size_t received_plaintext = 0;
      const int result = ::SSL_read_ex(
          ssl_, task.recv_storage.data(), task.recv_storage.size(),
          &received_plaintext);
      auto flushed = flush_ciphertext_locked(task.deadline);
      if (flushed.has_error()) {
        return {flushed.error(), flushed.error_message()};
      }
      if (result == 1) return IOResult{received_plaintext, false};

      const int error = ::SSL_get_error(ssl_, result);
      if (error == SSL_ERROR_ZERO_RETURN) return IOResult{0, true};
      if (error == SSL_ERROR_WANT_READ) {
        auto received = receive_ciphertext_locked(task.deadline);
        if (received.has_error()) {
          return {received.error(), received.error_message()};
        }
        if (*received) return IOResult{0, true};
      } else if (error != SSL_ERROR_WANT_WRITE) {
        return {rs::util::DbErrorCode::TLSError,
                openssl_error_text("SSL_read_ex")};
      }
    }
  }

  rs::util::Result<void> ensure_context_locked() {
    if (context_) return {};
    auto fail = [this](std::string message) {
      if (context_) ::SSL_CTX_free(context_);
      context_ = nullptr;
      return rs::util::Result<void>{rs::util::DbErrorCode::TLSError,
                                    std::move(message)};
    };
    context_ = ::SSL_CTX_new(::TLS_client_method());
    if (!context_) {
      return fail(openssl_error_text("SSL_CTX_new"));
    }

    if (::SSL_CTX_set_min_proto_version(
            context_, static_cast<int>(min_tls_version_)) != 1) {
      return fail(openssl_error_text("SSL_CTX_set_min_proto_version"));
    }
#ifdef TLS1_3_VERSION
    if (::SSL_CTX_set_max_proto_version(context_, TLS1_3_VERSION) != 1) {
      return fail(openssl_error_text("SSL_CTX_set_max_proto_version"));
    }
#endif
    ::SSL_CTX_set_verify(context_, verify_ ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                         nullptr);

    if (!verify_) return {};
    int loaded = 0;
    if (!ca_file_.empty()) {
      loaded = ::SSL_CTX_load_verify_locations(context_, ca_file_.c_str(),
                                                nullptr);
    } else if (!ca_directory_.empty()) {
      loaded = ::SSL_CTX_load_verify_locations(
          context_, nullptr, ca_directory_.c_str());
    } else {
      loaded = ::SSL_CTX_set_default_verify_paths(context_);
    }
    if (loaded != 1) {
      return fail(openssl_error_text("load TLS trust store"));
    }
    return {};
  }

  rs::util::Result<void> create_ssl_locked(std::string_view host) {
    reset_ssl_locked();
    auto context_result = ensure_context_locked();
    if (context_result.has_error()) return context_result;

    ssl_ = ::SSL_new(context_);
    if (!ssl_) {
      return {rs::util::DbErrorCode::TLSError,
              openssl_error_text("SSL_new")};
    }
    BIO* read_bio = ::BIO_new(::BIO_s_mem());
    BIO* write_bio = ::BIO_new(::BIO_s_mem());
    if (!read_bio || !write_bio) {
      if (read_bio) ::BIO_free(read_bio);
      if (write_bio) ::BIO_free(write_bio);
      reset_ssl_locked();
      return {rs::util::DbErrorCode::TLSError,
              openssl_error_text("BIO_new")};
    }
    ::SSL_set_bio(ssl_, read_bio, write_bio);
    read_bio_ = read_bio;
    write_bio_ = write_bio;
    ::SSL_set_connect_state(ssl_);

    server_name_ = std::string(host);
    if (!server_name_.empty() &&
        ::SSL_set_tlsext_host_name(ssl_, server_name_.c_str()) != 1) {
      reset_ssl_locked();
      return {rs::util::DbErrorCode::TLSError,
              openssl_error_text("set TLS server name")};
    }
    return {};
  }

  rs::util::Result<void> handshake_locked(
      std::string_view host, rs::util::Deadline deadline) {
    auto created = create_ssl_locked(host);
    if (created.has_error()) return created;

    for (;;) {
      if (rs::util::remaining(deadline) <=
          std::chrono::milliseconds::zero()) {
        reset_ssl_locked();
        return {rs::util::DbErrorCode::Timeout,
                "TLS handshake deadline expired"};
      }

      const int result = ::SSL_connect(ssl_);
      auto flushed = flush_ciphertext_locked(deadline);
      if (flushed.has_error()) {
        reset_ssl_locked();
        return flushed;
      }
      if (result == 1) break;

      const int error = ::SSL_get_error(ssl_, result);
      if (error == SSL_ERROR_WANT_READ) {
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
      } else if (error != SSL_ERROR_WANT_WRITE) {
        const auto message = openssl_error_text("SSL_connect");
        reset_ssl_locked();
        return {rs::util::DbErrorCode::TLSError, message};
      }
    }

    if (!verify_) return {};
    X509* certificate = ::SSL_get1_peer_certificate(ssl_);
    if (!certificate) {
      reset_ssl_locked();
      return {rs::util::DbErrorCode::TLSError,
              "TLS peer did not provide a certificate"};
    }
    const long verification = ::SSL_get_verify_result(ssl_);
    const bool hostname_matches = !verify_hostname_ ||
        ::X509_check_host(certificate, server_name_.c_str(),
                          server_name_.size(), 0, nullptr) == 1;
    ::X509_free(certificate);
    if (verification != X509_V_OK || !hostname_matches) {
      reset_ssl_locked();
      return {rs::util::DbErrorCode::TLSError,
              verification != X509_V_OK
                  ? "TLS certificate verification failed"
                  : "TLS hostname verification failed"};
    }
    return {};
  }

  rs::util::Result<void> flush_ciphertext_locked(
      rs::util::Deadline deadline) {
    std::array<std::byte, 16 * 1024> buffer{};
    while (::BIO_ctrl_pending(write_bio_) > 0) {
      const int count = ::BIO_read(write_bio_, buffer.data(),
                                   static_cast<int>(buffer.size()));
      if (count <= 0) {
        return {rs::util::DbErrorCode::TLSError,
                openssl_error_text("BIO_read")};
      }
      std::size_t offset = 0;
      const auto size = static_cast<std::size_t>(count);
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
      const int count = ::BIO_write(
          read_bio_, buffer.data() + offset,
          static_cast<int>(received->n - offset));
      if (count <= 0) {
        return {rs::util::DbErrorCode::TLSError,
                openssl_error_text("BIO_write")};
      }
      offset += static_cast<std::size_t>(count);
    }
    return false;
  }

  void reset_ssl_locked() noexcept {
    if (ssl_) ::SSL_free(ssl_);
    ssl_ = nullptr;
    read_bio_ = nullptr;
    write_bio_ = nullptr;
    server_name_.clear();
  }

  void reset_context_if_disconnected_locked() noexcept {
    if (ssl_ || !context_) return;
    ::SSL_CTX_free(context_);
    context_ = nullptr;
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
  SSL_CTX* context_{nullptr};
  SSL* ssl_{nullptr};
  BIO* read_bio_{nullptr};
  BIO* write_bio_{nullptr};
  std::string server_name_;
  bool verify_{true};
  bool verify_hostname_{true};
  long min_tls_version_{TLS1_2_VERSION};
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
  return future.get();
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
  return future.get();
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
    std::string file, std::string directory) {
  core_->set_ca_locations(std::move(file), std::move(directory));
}

std::size_t AsyncTlsTransport::max_inflight() const noexcept {
  return core_->max_inflight();
}

std::size_t AsyncTlsTransport::queue_depth() const noexcept {
  return core_->queue_depth();
}

} // namespace rs::core::transport
