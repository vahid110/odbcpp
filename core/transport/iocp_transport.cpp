#include "iocp_transport.h"

#ifdef _WIN32

#include "core/util/platform.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <mswsock.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rs::core::transport {
namespace {

using Completion = std::function<void()>;

class OperationState {
public:
  explicit OperationState(std::function<void()> wake)
      : wake_(std::move(wake)) {}

  void cancel() {
    std::function<void()> wake;
    {
      std::lock_guard lock(mutex_);
      if (complete_ || cancellation_requested_) return;
      cancellation_requested_ = true;
      cancelled_ = true;
      wake = wake_;
    }
    if (wake) wake();
  }

  bool finish() {
    std::lock_guard lock(mutex_);
    if (complete_) return false;
    complete_ = true;
    wake_ = {};
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
  bool complete_{false};
  bool cancellation_requested_{false};
  bool cancelled_{false};
  std::function<void()> wake_;
};

class IocpOperation final : public AsyncOperation {
public:
  explicit IocpOperation(std::shared_ptr<OperationState> state)
      : state_(std::move(state)) {}

  void cancel() override { state_->cancel(); }
  bool is_complete() const override { return state_->is_complete(); }
  bool is_cancelled() const override { return state_->is_cancelled(); }

private:
  std::shared_ptr<OperationState> state_;
};

std::string windows_error_text(const char* operation, DWORD error) {
  return std::string(operation) + ": Windows error " +
      std::to_string(error);
}

template<typename Callback, typename ResultType>
void invoke_safely(Callback& callback, ResultType result) noexcept {
  try {
    callback(std::move(result));
  } catch (...) {
    // A callback exception must not terminate the completion thread.
  }
}

} // namespace

class IocpTransport::Core : public std::enable_shared_from_this<Core> {
public:
  enum class RequestKind { Connect, Send, Recv };

  struct Endpoint {
    sockaddr_storage address{};
    int length{};
    int family{};
    int type{};
    int protocol{};
  };

  struct Request {
    OVERLAPPED overlapped{};
    RequestKind kind{};
    rs::util::Deadline deadline{};
    std::shared_ptr<OperationState> state;
    ConnectCallback connect_callback;
    SendCallback send_callback;
    RecvCallback recv_callback;
    std::string host;
    uint16_t port{};
    std::vector<std::byte> send_buffer;
    std::span<std::byte> recv_buffer;
    WSABUF winsock_buffer{};
    DWORD recv_flags{};
    std::vector<Endpoint> endpoints;
    std::size_t next_endpoint{};
    SOCKET native_socket{INVALID_SOCKET};
    bool cancellation_issued{false};
    bool timed_out{false};
    bool closing{false};
  };

  Core(std::size_t max_inflight, std::size_t queue_depth)
      : max_inflight_(max_inflight), queue_depth_(queue_depth) {
    if (max_inflight_ == 0) {
      throw std::invalid_argument("max_inflight must be greater than zero");
    }
    if (queue_depth_ == 0) {
      throw std::invalid_argument("queue_depth must be greater than zero");
    }
    if (max_inflight_ > queue_depth_) {
      throw std::invalid_argument("max_inflight cannot exceed queue_depth");
    }

    completion_port_ = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr,
                                                 0, 1);
    if (completion_port_ == nullptr) {
      throw std::runtime_error(windows_error_text(
          "CreateIoCompletionPort", ::GetLastError()));
    }
  }

  ~Core() {
    shutdown();
    if (socket_ != INVALID_SOCKET) ::closesocket(socket_);
    if (completion_port_ != nullptr) ::CloseHandle(completion_port_);
  }

  void start() {
    reactor_ = std::thread([self = shared_from_this()] { self->run(); });
  }

  void shutdown() noexcept {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake();
    if (!reactor_.joinable()) return;
    if (reactor_.get_id() == std::this_thread::get_id()) {
      reactor_.detach();
    } else {
      reactor_.join();
    }
  }

  bool submit(const std::shared_ptr<Request>& request) {
    {
      std::lock_guard lock(mutex_);
      if (stopping_ || outstanding_count_locked() >= queue_depth_) return false;
      if (request->kind == RequestKind::Connect && has_connect_locked()) {
        return false;
      }
      requests_.push_back(request);
    }
    wake();
    return true;
  }

  void wake() noexcept {
    if (completion_port_ == nullptr) return;
    (void)::PostQueuedCompletionStatus(completion_port_, 0, control_key_,
                                       nullptr);
  }

  void close_transport() noexcept {
    std::unique_lock lock(mutex_);
    if (stopping_) return;
    close_requested_ = true;
    const bool called_from_reactor =
        reactor_.joinable() && reactor_.get_id() == std::this_thread::get_id();
    lock.unlock();
    wake();
    if (called_from_reactor) return;
    lock.lock();
    close_finished_.wait(lock, [this] {
      return !close_requested_ || stopping_;
    });
  }

  std::size_t max_inflight() const noexcept { return max_inflight_; }
  std::size_t queue_depth() const noexcept { return queue_depth_; }

private:
  static void run_completions(std::vector<Completion>& completions) noexcept {
    for (auto& completion : completions) {
      try {
        completion();
      } catch (...) {
      }
    }
  }

  void run() {
    for (;;) {
      DWORD transferred = 0;
      ULONG_PTR key = 0;
      OVERLAPPED* overlapped = nullptr;
      const DWORD timeout = wait_timeout();
      const BOOL succeeded = ::GetQueuedCompletionStatus(
          completion_port_, &transferred, &key, &overlapped, timeout);
      const DWORD error = succeeded ? ERROR_SUCCESS : ::GetLastError();

      std::vector<Completion> completions;
      bool should_stop = false;
      {
        std::lock_guard lock(mutex_);
        if (overlapped != nullptr) {
          handle_native_completion_locked(overlapped, transferred, error,
                                          completions);
        }

        if (close_requested_) process_close_locked(completions);
        process_cancellations_locked(completions);
        expire_requests_locked(completions);

        if (stopping_) {
          if (!shutdown_started_) {
            shutdown_started_ = true;
            process_close_locked(completions);
          }
          should_stop = requests_.empty() && active_.empty();
        } else {
          start_connect_locked(completions);
          issue_io_locked(completions);
        }
      }

      run_completions(completions);
      if (should_stop) return;
    }
  }

  DWORD wait_timeout() const {
    std::lock_guard lock(mutex_);
    if (stopping_ || close_requested_) return 0;

    auto nearest = rs::util::Clock::time_point::max();
    for (const auto& request : requests_) {
      if (!request->state->is_complete()) {
        nearest = (std::min)(nearest, request->deadline);
      }
    }
    for (const auto& [overlapped, request] : active_) {
      (void)overlapped;
      if (!request->state->is_complete()) {
        nearest = (std::min)(nearest, request->deadline);
      }
    }
    if (nearest == rs::util::Clock::time_point::max()) return INFINITE;

    const auto now = rs::util::Clock::now();
    if (nearest <= now) return 0;
    const auto duration = nearest - now;
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            duration + std::chrono::milliseconds(1));
    return static_cast<DWORD>((std::min<long long>)(
        milliseconds.count(), static_cast<long long>(INFINITE - 1)));
  }

  std::size_t outstanding_count_locked() const {
    std::size_t count = requests_.size();
    for (const auto& [overlapped, request] : active_) {
      (void)overlapped;
      if (!request->state->is_complete()) ++count;
    }
    return count;
  }

  bool has_connect_locked() const {
    if (active_connect_ && !active_connect_->state->is_complete()) return true;
    return std::any_of(requests_.begin(), requests_.end(),
        [](const auto& request) {
          return request->kind == RequestKind::Connect &&
              !request->state->is_complete();
        });
  }

  void process_close_locked(std::vector<Completion>& completions) {
    close_requested_ = false;
    connected_ = false;
    active_connect_.reset();
    close_socket_locked();

    for (const auto& request : requests_) {
      complete_error_locked(request, rs::util::DbErrorCode::NetworkError,
                            stopping_ ? "transport shutting down"
                                      : "transport closed",
                            completions);
    }
    requests_.clear();

    for (auto& [overlapped, request] : active_) {
      (void)overlapped;
      request->closing = true;
      request->cancellation_issued = true;
    }
    close_finished_.notify_all();
  }

  void process_cancellations_locked(std::vector<Completion>& completions) {
    for (auto iterator = requests_.begin(); iterator != requests_.end();) {
      const auto& request = *iterator;
      if (!request->state->cancellation_requested()) {
        ++iterator;
        continue;
      }
      complete_error_locked(request, rs::util::DbErrorCode::NetworkError,
                            "async operation cancelled", completions);
      iterator = requests_.erase(iterator);
    }

    for (auto& [overlapped, request] : active_) {
      if (!request->state->cancellation_requested() ||
          request->cancellation_issued) {
        continue;
      }
      request->cancellation_issued = true;
      if (request->native_socket != INVALID_SOCKET) {
        (void)::CancelIoEx(reinterpret_cast<HANDLE>(request->native_socket),
                           overlapped);
      }
    }
  }

  void expire_requests_locked(std::vector<Completion>& completions) {
    const auto now = rs::util::Clock::now();
    for (auto iterator = requests_.begin(); iterator != requests_.end();) {
      const auto& request = *iterator;
      if (request->deadline > now || request->state->is_complete()) {
        ++iterator;
        continue;
      }
      complete_error_locked(request, rs::util::DbErrorCode::Timeout,
                            "async operation deadline expired", completions);
      iterator = requests_.erase(iterator);
    }

    for (auto& [overlapped, request] : active_) {
      if (request->deadline > now || request->state->is_complete() ||
          request->timed_out) {
        continue;
      }
      request->timed_out = true;
      request->cancellation_issued = true;
      if (request->native_socket != INVALID_SOCKET) {
        (void)::CancelIoEx(reinterpret_cast<HANDLE>(request->native_socket),
                           overlapped);
      }
    }
  }

  void start_connect_locked(std::vector<Completion>& completions) {
    if (active_connect_) return;

    auto iterator = std::find_if(requests_.begin(), requests_.end(),
        [](const auto& request) {
          return request->kind == RequestKind::Connect &&
              !request->state->is_complete();
        });
    if (iterator == requests_.end()) return;

    active_connect_ = *iterator;
    requests_.erase(iterator);
    close_socket_locked();
    connected_ = false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto port = std::to_string(active_connect_->port);
    const int result = ::getaddrinfo(active_connect_->host.c_str(),
                                     port.c_str(), &hints, &addresses);
    if (result != 0) {
      complete_connect_locked(
          active_connect_,
          rs::util::Result<void>{rs::util::DbErrorCode::ConnectionFailed,
                                 "getaddrinfo: " + std::to_string(result)},
          completions);
      active_connect_.reset();
      return;
    }

    for (auto* address = addresses; address; address = address->ai_next) {
      if (address->ai_addrlen >
          static_cast<decltype(address->ai_addrlen)>(
              sizeof(sockaddr_storage))) {
        continue;
      }
      Endpoint endpoint;
      std::memcpy(&endpoint.address, address->ai_addr, address->ai_addrlen);
      endpoint.length = static_cast<int>(address->ai_addrlen);
      endpoint.family = address->ai_family;
      endpoint.type = address->ai_socktype;
      endpoint.protocol = address->ai_protocol;
      active_connect_->endpoints.push_back(endpoint);
    }
    ::freeaddrinfo(addresses);

    try_next_endpoint_locked(completions);
  }

  bool prepare_socket_locked(const Endpoint& endpoint) {
    socket_ = ::WSASocketW(endpoint.family, endpoint.type, endpoint.protocol,
                           nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (socket_ == INVALID_SOCKET) return false;

    if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket_),
                                 completion_port_, socket_key_, 0) == nullptr) {
      close_socket_locked();
      return false;
    }

    int bind_result = SOCKET_ERROR;
    if (endpoint.family == AF_INET) {
      sockaddr_in local{};
      local.sin_family = AF_INET;
      local.sin_addr.s_addr = htonl(INADDR_ANY);
      bind_result = ::bind(socket_, reinterpret_cast<sockaddr*>(&local),
                           static_cast<int>(sizeof(local)));
    } else if (endpoint.family == AF_INET6) {
      sockaddr_in6 local{};
      local.sin6_family = AF_INET6;
      local.sin6_addr = in6addr_any;
      bind_result = ::bind(socket_, reinterpret_cast<sockaddr*>(&local),
                           static_cast<int>(sizeof(local)));
    }
    if (bind_result == SOCKET_ERROR) {
      close_socket_locked();
      return false;
    }
    return true;
  }

  LPFN_CONNECTEX load_connect_ex_locked() {
    GUID identifier = WSAID_CONNECTEX;
    LPFN_CONNECTEX function = nullptr;
    DWORD bytes = 0;
    const int result = ::WSAIoctl(
        socket_, SIO_GET_EXTENSION_FUNCTION_POINTER, &identifier,
        static_cast<DWORD>(sizeof(identifier)), &function,
        static_cast<DWORD>(sizeof(function)), &bytes, nullptr, nullptr);
    return result == 0 ? function : nullptr;
  }

  void try_next_endpoint_locked(std::vector<Completion>& completions) {
    while (active_connect_ &&
           active_connect_->next_endpoint < active_connect_->endpoints.size()) {
      const auto& endpoint =
          active_connect_->endpoints[active_connect_->next_endpoint++];
      if (!prepare_socket_locked(endpoint)) continue;

      auto connect_ex = load_connect_ex_locked();
      if (connect_ex == nullptr) {
        close_socket_locked();
        continue;
      }

      active_connect_->overlapped = OVERLAPPED{};
      active_connect_->native_socket = socket_;
      OVERLAPPED* overlapped = &active_connect_->overlapped;
      active_.emplace(overlapped, active_connect_);
      const BOOL started = connect_ex(
          socket_, reinterpret_cast<const sockaddr*>(&endpoint.address),
          endpoint.length, nullptr, 0, nullptr, overlapped);
      if (started || ::WSAGetLastError() == ERROR_IO_PENDING) return;

      active_.erase(overlapped);
      close_socket_locked();
    }

    if (active_connect_) {
      complete_connect_locked(
          active_connect_,
          rs::util::Result<void>{rs::util::DbErrorCode::ConnectionFailed,
                                 "connect failed for all resolved addresses"},
          completions);
      active_connect_.reset();
    }
  }

  void issue_io_locked(std::vector<Completion>& completions) {
    if (active_connect_) return;

    while (active_.size() < max_inflight_) {
      auto iterator = std::find_if(requests_.begin(), requests_.end(),
          [](const auto& request) {
            return request->kind != RequestKind::Connect &&
                !request->state->is_complete();
          });
      if (iterator == requests_.end()) return;

      auto request = *iterator;
      requests_.erase(iterator);
      if (socket_ == INVALID_SOCKET || !connected_) {
        complete_error_locked(request, rs::util::DbErrorCode::NotConnected,
                              "transport is not connected", completions);
        continue;
      }

      const auto size = request->kind == RequestKind::Send
          ? request->send_buffer.size() : request->recv_buffer.size();
      if (size == 0) {
        if (request->kind == RequestKind::Send) {
          complete_send_locked(request,
              rs::util::Result<IOResult>{IOResult{0, false}}, completions);
        } else {
          complete_recv_locked(request,
              rs::util::Result<IOResult>{IOResult{0, false}}, completions);
        }
        continue;
      }

      request->overlapped = OVERLAPPED{};
      request->native_socket = socket_;
      request->winsock_buffer.buf = request->kind == RequestKind::Send
          ? reinterpret_cast<char*>(request->send_buffer.data())
          : reinterpret_cast<char*>(request->recv_buffer.data());
      request->winsock_buffer.len = static_cast<ULONG>((std::min<std::size_t>)(
          size, static_cast<std::size_t>((std::numeric_limits<ULONG>::max)())));
      request->recv_flags = 0;
      OVERLAPPED* overlapped = &request->overlapped;
      active_.emplace(overlapped, request);

      DWORD transferred = 0;
      const int result = request->kind == RequestKind::Send
          ? ::WSASend(socket_, &request->winsock_buffer, 1, &transferred, 0,
                      overlapped, nullptr)
          : ::WSARecv(socket_, &request->winsock_buffer, 1, &transferred,
                      &request->recv_flags, overlapped, nullptr);
      if (result == 0 || ::WSAGetLastError() == WSA_IO_PENDING) continue;

      const DWORD error = static_cast<DWORD>(::WSAGetLastError());
      active_.erase(overlapped);
      complete_error_locked(request, rs::util::DbErrorCode::NetworkError,
                            windows_error_text(
                                request->kind == RequestKind::Send
                                    ? "WSASend" : "WSARecv",
                                error),
                            completions);
    }
  }

  void handle_native_completion_locked(OVERLAPPED* overlapped,
                                       DWORD transferred, DWORD error,
                                       std::vector<Completion>& completions) {
    const auto iterator = active_.find(overlapped);
    if (iterator == active_.end()) return;
    auto request = iterator->second;
    active_.erase(iterator);

    if (request->kind == RequestKind::Connect) {
      if (request != active_connect_) return;
      if (request->timed_out) {
        close_socket_locked();
        complete_error_locked(request, rs::util::DbErrorCode::Timeout,
                              "async operation deadline expired", completions);
        active_connect_.reset();
      } else if (request->state->cancellation_requested()) {
        close_socket_locked();
        complete_error_locked(request, rs::util::DbErrorCode::NetworkError,
                              "async operation cancelled", completions);
        active_connect_.reset();
      } else if (request->closing || stopping_) {
        complete_error_locked(request, rs::util::DbErrorCode::NetworkError,
                              stopping_ ? "transport shutting down"
                                        : "transport closed",
                              completions);
        active_connect_.reset();
      } else if (error == ERROR_SUCCESS &&
                 ::setsockopt(socket_, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT,
                              nullptr, 0) == 0) {
        connected_ = true;
        complete_connect_locked(request, rs::util::Result<void>{},
                                completions);
        active_connect_.reset();
      } else {
        close_socket_locked();
        try_next_endpoint_locked(completions);
      }
      return;
    }

    if (request->timed_out) {
      complete_error_locked(request, rs::util::DbErrorCode::Timeout,
                            "async operation deadline expired", completions);
    } else if (request->state->cancellation_requested()) {
      complete_error_locked(request, rs::util::DbErrorCode::NetworkError,
                            "async operation cancelled", completions);
    } else if (request->closing || stopping_) {
      complete_error_locked(request, rs::util::DbErrorCode::NetworkError,
                            stopping_ ? "transport shutting down"
                                      : "transport closed",
                            completions);
    } else if (error != ERROR_SUCCESS) {
      complete_error_locked(
          request, rs::util::DbErrorCode::NetworkError,
          windows_error_text(request->kind == RequestKind::Send
                                 ? "WSASend" : "WSARecv",
                             error),
          completions);
    } else if (request->kind == RequestKind::Send) {
      complete_send_locked(request,
          rs::util::Result<IOResult>{
              IOResult{static_cast<std::size_t>(transferred), false}},
          completions);
    } else {
      complete_recv_locked(request,
          rs::util::Result<IOResult>{IOResult{
              static_cast<std::size_t>(transferred), transferred == 0}},
          completions);
    }
  }

  void close_socket_locked() noexcept {
    if (socket_ == INVALID_SOCKET) return;
    (void)::closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }

  void complete_connect_locked(const std::shared_ptr<Request>& request,
                               rs::util::Result<void> result,
                               std::vector<Completion>& completions) {
    if (!request || !request->state->finish()) return;
    auto callback = request->connect_callback;
    completions.emplace_back(
        [callback = std::move(callback), result = std::move(result)]() mutable {
          invoke_safely(callback, std::move(result));
        });
  }

  void complete_send_locked(const std::shared_ptr<Request>& request,
                            rs::util::Result<IOResult> result,
                            std::vector<Completion>& completions) {
    if (!request->state->finish()) return;
    auto callback = request->send_callback;
    completions.emplace_back(
        [callback = std::move(callback), result = std::move(result)]() mutable {
          invoke_safely(callback, std::move(result));
        });
  }

  void complete_recv_locked(const std::shared_ptr<Request>& request,
                            rs::util::Result<IOResult> result,
                            std::vector<Completion>& completions) {
    if (!request->state->finish()) return;
    auto callback = request->recv_callback;
    completions.emplace_back(
        [callback = std::move(callback), result = std::move(result)]() mutable {
          invoke_safely(callback, std::move(result));
        });
  }

  void complete_error_locked(const std::shared_ptr<Request>& request,
                             rs::util::DbErrorCode code,
                             const std::string& message,
                             std::vector<Completion>& completions) {
    if (request->kind == RequestKind::Connect) {
      complete_connect_locked(request, rs::util::Result<void>{code, message},
                              completions);
    } else if (request->kind == RequestKind::Send) {
      complete_send_locked(request,
                           rs::util::Result<IOResult>{code, message},
                           completions);
    } else {
      complete_recv_locked(request,
                           rs::util::Result<IOResult>{code, message},
                           completions);
    }
  }

  static constexpr ULONG_PTR control_key_ = 1;
  static constexpr ULONG_PTR socket_key_ = 2;
  const std::size_t max_inflight_;
  const std::size_t queue_depth_;
  rs::platform::WSAInit winsock_{};
  HANDLE completion_port_{nullptr};
  SOCKET socket_{INVALID_SOCKET};
  bool connected_{false};
  bool stopping_{false};
  bool shutdown_started_{false};
  bool close_requested_{false};
  mutable std::mutex mutex_;
  std::condition_variable close_finished_;
  std::deque<std::shared_ptr<Request>> requests_;
  std::unordered_map<OVERLAPPED*, std::shared_ptr<Request>> active_;
  std::shared_ptr<Request> active_connect_;
  std::thread reactor_;
};

IocpTransport::IocpTransport(std::size_t max_inflight,
                             std::size_t queue_depth)
    : core_(std::make_shared<Core>(max_inflight, queue_depth)) {
  core_->start();
}

IocpTransport::~IocpTransport() { core_->shutdown(); }

rs::util::Result<void> IocpTransport::connect(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  return connect_future(host, port, deadline).get();
}

rs::util::Result<IOResult> IocpTransport::send(
    std::span<const std::byte> buf, rs::util::Deadline deadline) {
  return send_future(buf, deadline).get();
}

rs::util::Result<IOResult> IocpTransport::recv(
    std::span<std::byte> buf, rs::util::Deadline deadline) {
  return recv_future(buf, deadline).get();
}

void IocpTransport::close() noexcept { core_->close_transport(); }

std::unique_ptr<AsyncOperation> IocpTransport::connect_async(
    std::string_view host, uint16_t port, rs::util::Deadline deadline,
    ConnectCallback callback) {
  auto core = core_;
  std::weak_ptr<Core> weak_core = core;
  auto state = std::make_shared<OperationState>([weak_core] {
    if (auto locked = weak_core.lock()) locked->wake();
  });
  auto operation = std::make_unique<IocpOperation>(state);
  auto request = std::make_shared<Core::Request>();
  request->kind = Core::RequestKind::Connect;
  request->deadline = deadline;
  request->state = state;
  request->connect_callback = std::move(callback);
  request->host = std::string(host);
  request->port = port;
  if (!core->submit(request) && state->finish()) {
    invoke_safely(request->connect_callback, rs::util::Result<void>{
        rs::util::DbErrorCode::NetworkError,
        "IOCP transport queue is full or a connect is already pending"});
  }
  return operation;
}

std::unique_ptr<AsyncOperation> IocpTransport::send_async(
    std::span<const std::byte> buf, rs::util::Deadline deadline,
    SendCallback callback) {
  auto core = core_;
  std::weak_ptr<Core> weak_core = core;
  auto state = std::make_shared<OperationState>([weak_core] {
    if (auto locked = weak_core.lock()) locked->wake();
  });
  auto operation = std::make_unique<IocpOperation>(state);
  auto request = std::make_shared<Core::Request>();
  request->kind = Core::RequestKind::Send;
  request->deadline = deadline;
  request->state = state;
  request->send_callback = std::move(callback);
  request->send_buffer.assign(buf.begin(), buf.end());
  if (!core->submit(request) && state->finish()) {
    invoke_safely(request->send_callback, rs::util::Result<IOResult>{
        rs::util::DbErrorCode::NetworkError,
        "IOCP transport queue is full"});
  }
  return operation;
}

std::unique_ptr<AsyncOperation> IocpTransport::recv_async(
    std::span<std::byte> buf, rs::util::Deadline deadline,
    RecvCallback callback) {
  auto core = core_;
  std::weak_ptr<Core> weak_core = core;
  auto state = std::make_shared<OperationState>([weak_core] {
    if (auto locked = weak_core.lock()) locked->wake();
  });
  auto operation = std::make_unique<IocpOperation>(state);
  auto request = std::make_shared<Core::Request>();
  request->kind = Core::RequestKind::Recv;
  request->deadline = deadline;
  request->state = state;
  request->recv_callback = std::move(callback);
  request->recv_buffer = buf;
  if (!core->submit(request) && state->finish()) {
    invoke_safely(request->recv_callback, rs::util::Result<IOResult>{
        rs::util::DbErrorCode::NetworkError,
        "IOCP transport queue is full"});
  }
  return operation;
}

std::future<rs::util::Result<void>> IocpTransport::connect_future(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  auto promise = std::make_shared<std::promise<rs::util::Result<void>>>();
  auto future = promise->get_future();
  (void)connect_async(host, port, deadline,
      [promise](rs::util::Result<void> result) mutable {
        promise->set_value(std::move(result));
      });
  return future;
}

std::future<rs::util::Result<IOResult>> IocpTransport::send_future(
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

std::future<rs::util::Result<IOResult>> IocpTransport::recv_future(
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

std::size_t IocpTransport::max_inflight() const noexcept {
  return core_->max_inflight();
}

std::size_t IocpTransport::queue_depth() const noexcept {
  return core_->queue_depth();
}

} // namespace rs::core::transport

#endif // _WIN32
