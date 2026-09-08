#include "epoll_transport.h"

#ifdef __linux__

#include "core/util/platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <thread>
#include <utility>
#include <vector>

namespace rs::core::transport {
namespace {

using Completion = std::function<void()>;

class OperationState {
public:
  explicit OperationState(std::function<void()> cancel_callback)
      : cancel_callback_(std::move(cancel_callback)) {}

  void cancel() {
    std::function<void()> callback;
    {
      std::lock_guard lock(mutex_);
      if (complete_) return;
      complete_ = true;
      cancelled_ = true;
      callback = std::move(cancel_callback_);
    }
    if (callback) {
      try {
        callback();
      } catch (...) {
        // User callbacks must not escape cancellation.
      }
    }
  }

  bool finish() {
    std::lock_guard lock(mutex_);
    if (complete_) return false;
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

class EpollOperation final : public AsyncOperation {
public:
  explicit EpollOperation(std::shared_ptr<OperationState> state)
      : state_(std::move(state)) {}

  void cancel() override { state_->cancel(); }
  bool is_complete() const override { return state_->is_complete(); }
  bool is_cancelled() const override { return state_->is_cancelled(); }

private:
  std::shared_ptr<OperationState> state_;
};

std::string socket_error_text(const char* operation, int error) {
  return std::string(operation) + ": errno " + std::to_string(error) +
      " (" + std::generic_category().message(error) + ")";
}

template<typename Callback, typename ResultType>
void invoke_safely(Callback& callback, ResultType result) noexcept {
  try {
    callback(std::move(result));
  } catch (...) {
    // A callback exception must not terminate the reactor.
  }
}

} // namespace

class EpollTransport::Core : public std::enable_shared_from_this<Core> {
public:
  enum class RequestKind { Connect, Send, Recv };

  struct Endpoint {
    sockaddr_storage address{};
    socklen_t length{};
    int family{};
    int type{};
    int protocol{};
  };

  struct Request {
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
    std::vector<Endpoint> endpoints;
    std::size_t next_endpoint{};
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

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
      throw std::runtime_error(rs::platform::last_error_text("epoll_create1"));
    }

    wake_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0) {
      const auto message = rs::platform::last_error_text("eventfd");
      ::close(epoll_fd_);
      epoll_fd_ = -1;
      throw std::runtime_error(message);
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = wake_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) < 0) {
      const auto message = rs::platform::last_error_text("epoll_ctl(wake)");
      ::close(wake_fd_);
      ::close(epoll_fd_);
      wake_fd_ = -1;
      epoll_fd_ = -1;
      throw std::runtime_error(message);
    }

  }

  ~Core() {
    shutdown();

    if (socket_fd_ >= 0) ::close(socket_fd_);
    if (wake_fd_ >= 0) ::close(wake_fd_);
    if (epoll_fd_ >= 0) ::close(epoll_fd_);
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
      if (stopping_ || requests_.size() >= queue_depth_) return false;
      if (request->kind == RequestKind::Connect &&
          std::any_of(requests_.begin(), requests_.end(), [](const auto& item) {
            return item->kind == RequestKind::Connect &&
                !item->state->is_complete();
          })) {
        return false;
      }
      requests_.push_back(request);
    }
    wake();
    return true;
  }

  void wake() noexcept {
    if (wake_fd_ < 0) return;
    const uint64_t one = 1;
    const auto ignored = ::write(wake_fd_, &one, sizeof(one));
    (void)ignored;
  }

  void close_transport() noexcept {
    std::vector<Completion> completions;
    {
      std::lock_guard lock(mutex_);
      close_socket_locked();
      connected_ = false;
      active_connect_.reset();
      for (const auto& request : requests_) {
        complete_error_locked(request, rs::util::DbErrorCode::NetworkError,
                              "transport closed", completions);
      }
      requests_.clear();
    }
    run_completions(completions);
    wake();
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
    std::array<epoll_event, 4> events{};
    for (;;) {
      const int timeout = wait_timeout();
      int count;
      do {
        count = ::epoll_wait(epoll_fd_, events.data(),
                             static_cast<int>(events.size()), timeout);
      } while (count < 0 && errno == EINTR);

      bool control_event = false;
      uint32_t socket_events = 0;
      if (count > 0) {
        for (int i = 0; i < count; ++i) {
          if (events[i].data.fd == wake_fd_) {
            control_event = true;
            drain_wake_fd();
          } else {
            socket_events |= events[i].events;
          }
        }
      }

      std::vector<Completion> completions;
      bool should_stop = false;
      {
        std::lock_guard lock(mutex_);
        should_stop = stopping_;
        if (should_stop) {
          close_socket_locked();
          for (const auto& request : requests_) {
            complete_error_locked(request,
                                  rs::util::DbErrorCode::NetworkError,
                                  "transport shutting down", completions);
          }
          requests_.clear();
          active_connect_.reset();
        } else {
          reap_cancelled_locked();
          expire_requests_locked(completions);
          start_connect_locked(completions);
          if (active_connect_ &&
              (socket_events & (EPOLLOUT | EPOLLERR | EPOLLHUP))) {
            finish_connect_locked(completions);
          }
          if (!active_connect_ &&
              (control_event || socket_events != 0 || count == 0)) {
            process_io_locked(completions);
          }
          reap_cancelled_locked();
          refresh_interest_locked();
        }
      }
      run_completions(completions);
      if (should_stop) return;
    }
  }

  int wait_timeout() const {
    std::lock_guard lock(mutex_);
    if (stopping_) return 0;
    if (requests_.empty()) return -1;

    auto nearest = rs::util::Clock::time_point::max();
    for (const auto& request : requests_) {
      if (!request->state->is_complete()) {
        nearest = std::min(nearest, request->deadline);
      }
    }
    if (nearest == rs::util::Clock::time_point::max()) return -1;
    const auto now = rs::util::Clock::now();
    if (nearest <= now) return 0;
    const auto duration = nearest - now;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        duration + std::chrono::milliseconds(1));
    return static_cast<int>(std::min<long long>(
        milliseconds.count(), std::numeric_limits<int>::max()));
  }

  void drain_wake_fd() noexcept {
    uint64_t value;
    while (::read(wake_fd_, &value, sizeof(value)) > 0) {
    }
  }

  void reap_cancelled_locked() {
    if (active_connect_ && active_connect_->state->is_complete()) {
      close_socket_locked();
      connected_ = false;
      active_connect_.reset();
    }
    std::erase_if(requests_, [](const auto& request) {
      return request->state->is_complete();
    });
  }

  void expire_requests_locked(std::vector<Completion>& completions) {
    const auto now = rs::util::Clock::now();
    for (const auto& request : requests_) {
      if (!request->state->is_complete() && request->deadline <= now) {
        if (request == active_connect_) {
          close_socket_locked();
          connected_ = false;
          active_connect_.reset();
        }
        complete_error_locked(request, rs::util::DbErrorCode::Timeout,
                              "async operation deadline expired", completions);
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
    close_socket_locked();
    connected_ = false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto port = std::to_string(active_connect_->port);
    const int result = ::getaddrinfo(active_connect_->host.c_str(), port.c_str(),
                                     &hints, &addresses);
    if (result != 0) {
      complete_connect_locked(
          active_connect_,
          rs::util::Result<void>{rs::util::DbErrorCode::ConnectionFailed,
                                 std::string("getaddrinfo: ") +
                                     ::gai_strerror(result)},
          completions);
      active_connect_.reset();
      return;
    }

    for (auto* address = addresses; address; address = address->ai_next) {
      if (address->ai_addrlen > sizeof(sockaddr_storage)) continue;
      Endpoint endpoint;
      std::memcpy(&endpoint.address, address->ai_addr, address->ai_addrlen);
      endpoint.length = static_cast<socklen_t>(address->ai_addrlen);
      endpoint.family = address->ai_family;
      endpoint.type = address->ai_socktype;
      endpoint.protocol = address->ai_protocol;
      active_connect_->endpoints.push_back(endpoint);
    }
    ::freeaddrinfo(addresses);

    try_next_endpoint_locked(completions);
  }

  void try_next_endpoint_locked(std::vector<Completion>& completions) {
    while (active_connect_ &&
           active_connect_->next_endpoint < active_connect_->endpoints.size()) {
      const auto& endpoint =
          active_connect_->endpoints[active_connect_->next_endpoint++];
      const int socket = ::socket(endpoint.family, endpoint.type | SOCK_CLOEXEC,
                                  endpoint.protocol);
      if (socket < 0) continue;

      const int flags = ::fcntl(socket, F_GETFL, 0);
      if (flags < 0 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(socket);
        continue;
      }

      socket_fd_ = socket;
      int result;
      do {
        result = ::connect(socket_fd_,
                           reinterpret_cast<const sockaddr*>(&endpoint.address),
                           endpoint.length);
      } while (result < 0 && errno == EINTR);

      if (result == 0) {
        connected_ = true;
        complete_connect_locked(active_connect_, rs::util::Result<void>{},
                                completions);
        active_connect_.reset();
        return;
      }
      if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
        refresh_interest_locked();
        return;
      }
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

  void finish_connect_locked(std::vector<Completion>& completions) {
    int error = 0;
    socklen_t length = sizeof(error);
    if (::getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
        error == 0) {
      connected_ = true;
      complete_connect_locked(active_connect_, rs::util::Result<void>{},
                              completions);
      active_connect_.reset();
      return;
    }
    close_socket_locked();
    try_next_endpoint_locked(completions);
  }

  void process_io_locked(std::vector<Completion>& completions) {
    std::size_t considered = 0;
    for (const auto& request : requests_) {
      if (considered >= max_inflight_) break;
      if (request->state->is_complete() ||
          request->kind == RequestKind::Connect) {
        continue;
      }
      ++considered;

      if (socket_fd_ < 0 || !connected_) {
        complete_error_locked(request, rs::util::DbErrorCode::NotConnected,
                              "transport is not connected", completions);
        continue;
      }

      if ((request->kind == RequestKind::Send &&
           request->send_buffer.empty()) ||
          (request->kind == RequestKind::Recv &&
           request->recv_buffer.empty())) {
        if (request->kind == RequestKind::Send) {
          complete_send_locked(request,
                               rs::util::Result<IOResult>{IOResult{0, false}},
                               completions);
        } else {
          complete_recv_locked(request,
                               rs::util::Result<IOResult>{IOResult{0, false}},
                               completions);
        }
        continue;
      }

      if (request->kind == RequestKind::Send) {
        ssize_t count;
        do {
          count = ::send(socket_fd_, request->send_buffer.data(),
                         request->send_buffer.size(), MSG_NOSIGNAL);
        } while (count < 0 && errno == EINTR);
        if (count >= 0) {
          complete_send_locked(request, rs::util::Result<IOResult>{
              IOResult{static_cast<std::size_t>(count), false}}, completions);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          const int error = errno;
          complete_send_locked(request, rs::util::Result<IOResult>{
              rs::util::DbErrorCode::NetworkError,
              socket_error_text("send", error)}, completions);
        }
      } else {
        ssize_t count;
        do {
          count = ::recv(socket_fd_, request->recv_buffer.data(),
                         request->recv_buffer.size(), 0);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
          complete_recv_locked(request, rs::util::Result<IOResult>{
              IOResult{static_cast<std::size_t>(count), false}}, completions);
        } else if (count == 0) {
          complete_recv_locked(request,
                               rs::util::Result<IOResult>{IOResult{0, true}},
                               completions);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          const int error = errno;
          complete_recv_locked(request, rs::util::Result<IOResult>{
              rs::util::DbErrorCode::NetworkError,
              socket_error_text("recv", error)}, completions);
        }
      }
    }
  }

  void refresh_interest_locked() {
    if (socket_fd_ < 0) return;

    uint32_t events = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
    if (active_connect_) {
      events |= EPOLLOUT;
    } else if (connected_) {
      std::size_t considered = 0;
      for (const auto& request : requests_) {
        if (considered >= max_inflight_) break;
        if (request->state->is_complete() ||
            request->kind == RequestKind::Connect) {
          continue;
        }
        ++considered;
        if (request->kind == RequestKind::Send) events |= EPOLLOUT;
        if (request->kind == RequestKind::Recv) events |= EPOLLIN;
      }
    }

    epoll_event event{};
    event.events = events;
    event.data.fd = socket_fd_;
    const int operation = registered_socket_ == socket_fd_
        ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    if (::epoll_ctl(epoll_fd_, operation, socket_fd_, &event) == 0) {
      registered_socket_ = socket_fd_;
    }
  }

  void close_socket_locked() noexcept {
    if (registered_socket_ >= 0) {
      ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, registered_socket_, nullptr);
      registered_socket_ = -1;
    }
    if (socket_fd_ >= 0) {
      ::close(socket_fd_);
      socket_fd_ = -1;
    }
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

  const std::size_t max_inflight_;
  const std::size_t queue_depth_;
  int epoll_fd_{-1};
  int wake_fd_{-1};
  int socket_fd_{-1};
  int registered_socket_{-1};
  bool connected_{false};
  bool stopping_{false};
  mutable std::mutex mutex_;
  std::deque<std::shared_ptr<Request>> requests_;
  std::shared_ptr<Request> active_connect_;
  std::thread reactor_;
};

EpollTransport::EpollTransport(std::size_t max_inflight,
                               std::size_t queue_depth)
    : core_(std::make_shared<Core>(max_inflight, queue_depth)) {
  core_->start();
}

EpollTransport::~EpollTransport() { core_->shutdown(); }

rs::util::Result<void> EpollTransport::connect(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  return connect_future(host, port, deadline).get();
}

rs::util::Result<IOResult> EpollTransport::send(
    std::span<const std::byte> buf, rs::util::Deadline deadline) {
  return send_future(buf, deadline).get();
}

rs::util::Result<IOResult> EpollTransport::recv(
    std::span<std::byte> buf, rs::util::Deadline deadline) {
  return recv_future(buf, deadline).get();
}

void EpollTransport::close() noexcept { core_->close_transport(); }

std::unique_ptr<AsyncOperation> EpollTransport::connect_async(
    std::string_view host, uint16_t port, rs::util::Deadline deadline,
    ConnectCallback callback) {
  auto core = core_;
  std::weak_ptr<Core> weak_core = core;
  auto cancel_callback = callback;
  auto state = std::make_shared<OperationState>(
      [weak_core, callback = std::move(cancel_callback)]() mutable {
        invoke_safely(callback, rs::util::Result<void>{
            rs::util::DbErrorCode::NetworkError,
            "async operation cancelled"});
        if (auto core = weak_core.lock()) core->wake();
      });
  auto operation = std::make_unique<EpollOperation>(state);
  auto request = std::make_shared<Core::Request>();
  request->kind = Core::RequestKind::Connect;
  request->deadline = deadline;
  request->state = state;
  request->connect_callback = std::move(callback);
  request->host = std::string(host);
  request->port = port;
  if (!core_->submit(request) && state->finish()) {
    invoke_safely(request->connect_callback, rs::util::Result<void>{
        rs::util::DbErrorCode::NetworkError,
        "epoll transport queue is full or a connect is already pending"});
  }
  return operation;
}

std::unique_ptr<AsyncOperation> EpollTransport::send_async(
    std::span<const std::byte> buf, rs::util::Deadline deadline,
    SendCallback callback) {
  auto core = core_;
  std::weak_ptr<Core> weak_core = core;
  auto cancel_callback = callback;
  auto state = std::make_shared<OperationState>(
      [weak_core, callback = std::move(cancel_callback)]() mutable {
        invoke_safely(callback, rs::util::Result<IOResult>{
            rs::util::DbErrorCode::NetworkError,
            "async operation cancelled"});
        if (auto core = weak_core.lock()) core->wake();
      });
  auto operation = std::make_unique<EpollOperation>(state);
  auto request = std::make_shared<Core::Request>();
  request->kind = Core::RequestKind::Send;
  request->deadline = deadline;
  request->state = state;
  request->send_callback = std::move(callback);
  request->send_buffer.assign(buf.begin(), buf.end());
  if (!core_->submit(request) && state->finish()) {
    invoke_safely(request->send_callback, rs::util::Result<IOResult>{
        rs::util::DbErrorCode::NetworkError,
        "epoll transport queue is full"});
  }
  return operation;
}

std::unique_ptr<AsyncOperation> EpollTransport::recv_async(
    std::span<std::byte> buf, rs::util::Deadline deadline,
    RecvCallback callback) {
  auto core = core_;
  std::weak_ptr<Core> weak_core = core;
  auto cancel_callback = callback;
  auto state = std::make_shared<OperationState>(
      [weak_core, callback = std::move(cancel_callback)]() mutable {
        invoke_safely(callback, rs::util::Result<IOResult>{
            rs::util::DbErrorCode::NetworkError,
            "async operation cancelled"});
        if (auto core = weak_core.lock()) core->wake();
      });
  auto operation = std::make_unique<EpollOperation>(state);
  auto request = std::make_shared<Core::Request>();
  request->kind = Core::RequestKind::Recv;
  request->deadline = deadline;
  request->state = state;
  request->recv_callback = std::move(callback);
  request->recv_buffer = buf;
  if (!core_->submit(request) && state->finish()) {
    invoke_safely(request->recv_callback, rs::util::Result<IOResult>{
        rs::util::DbErrorCode::NetworkError,
        "epoll transport queue is full"});
  }
  return operation;
}

std::future<rs::util::Result<void>> EpollTransport::connect_future(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) {
  auto promise = std::make_shared<std::promise<rs::util::Result<void>>>();
  auto future = promise->get_future();
  (void)connect_async(host, port, deadline,
      [promise](rs::util::Result<void> result) mutable {
        promise->set_value(std::move(result));
      });
  return future;
}

std::future<rs::util::Result<IOResult>> EpollTransport::send_future(
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

std::future<rs::util::Result<IOResult>> EpollTransport::recv_future(
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

std::size_t EpollTransport::max_inflight() const noexcept {
  return core_->max_inflight();
}

std::size_t EpollTransport::queue_depth() const noexcept {
  return core_->queue_depth();
}

} // namespace rs::core::transport

#endif // __linux__
