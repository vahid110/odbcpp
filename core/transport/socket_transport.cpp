#include "socket_transport.h"
#include "socket_wait.h"
#include "core/util/exception_adapter.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <limits>
#include <string>

using rs::util::Deadline;
using rs::util::remaining;
using rs::util::IOError;
using rs::util::TimeoutError;

namespace rs::core::transport {

SocketTransport::socket_t SocketTransport::invalid_socket() {
#ifdef _WIN32
  return INVALID_SOCKET;
#else
  return -1;
#endif
}

bool SocketTransport::is_invalid(socket_t s) {
#ifdef _WIN32
  return s == INVALID_SOCKET;
#else
  return s < 0;
#endif
}

void SocketTransport::do_close(socket_t s) noexcept {
#ifdef _WIN32
  if (s != INVALID_SOCKET) ::closesocket(s);
#else
  if (s >= 0) ::close(s);
#endif
}

void SocketTransport::set_nonblocking(socket_t s, bool nb) {
#ifdef _WIN32
  u_long mode = nb ? 1UL : 0UL;
  if (ioctlsocket(s, FIONBIO, &mode) != 0) {
    throw IOError(platform::last_error_text("ioctlsocket(FIONBIO)"));
  }
#else
  int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0 ||
      fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) < 0) {
    throw IOError(platform::last_error_text("fcntl(O_NONBLOCK)"));
  }
#endif
}

void SocketTransport::set_timeouts(socket_t s, std::chrono::milliseconds rw) {
  const auto count = std::max<std::chrono::milliseconds::rep>(1, rw.count());
#ifdef _WIN32
  // Winsock may quantize sub-500 ms socket timeouts down to zero (meaning no
  // timeout), so retain SocketTimeout semantics by clamping to its practical
  // timer granularity. Strict deadlines use WSAPoll and are not clamped.
  constexpr std::chrono::milliseconds::rep windows_timeout_floor = 500;
  const auto windows_count = (std::max)(count, windows_timeout_floor);
  DWORD ms = static_cast<DWORD>((std::min<std::chrono::milliseconds::rep>)(
      windows_count, std::numeric_limits<DWORD>::max()));
  if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms)) != 0 ||
      setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms)) != 0) {
    throw IOError(platform::last_error_text("setsockopt(timeout)"));
  }
#else
  timeval tv{static_cast<long>(count / 1000),
             static_cast<suseconds_t>((count % 1000) * 1000)};
  if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
      setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
    throw IOError(platform::last_error_text("setsockopt(timeout)"));
  }
#endif
}

SocketTransport::SocketTransport(DeadlineModel deadline_model)
    : deadline_model_(deadline_model) {}
SocketTransport::~SocketTransport() { close(); }

void SocketTransport::adopt(socket_t socket) {
  if (socket == sock_) return;
  close();
  if (is_invalid(socket)) throw IOError("cannot adopt an invalid socket");
  sock_ = socket;
}

void SocketTransport::set_deadline_model(DeadlineModel model) {
  deadline_model_ = model;
  if (!is_invalid(sock_)) {
    set_nonblocking(sock_, model == DeadlineModel::Strict);
  }
}

void SocketTransport::prepare_for_io(Deadline deadline) {
  if (is_invalid(sock_)) throw IOError("operation on closed socket");
  const auto left = remaining(deadline);
  if (left <= std::chrono::milliseconds::zero()) {
    throw TimeoutError("operation deadline expired");
  }
  if (deadline_model_ == DeadlineModel::Strict) {
    set_nonblocking(sock_, true);
  } else {
    set_nonblocking(sock_, false);
    set_timeouts(sock_, left);
  }
}

rs::util::Result<void> SocketTransport::connect(std::string_view host, uint16_t port, Deadline deadline) {
  return rs::util::try_catch([&]() {
  close();

  if (remaining(deadline) <= std::chrono::milliseconds::zero()) {
    throw TimeoutError("connect deadline expired");
  }

  // Resolve
  addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr; std::string port_str = std::to_string(port);
  int gai = getaddrinfo(std::string(host).c_str(), port_str.c_str(), &hints, &res);
  if (gai != 0) throw IOError(std::string("getaddrinfo: ") + gai_strerror(gai));

  auto guard = std::unique_ptr<addrinfo, void(*)(addrinfo*)>(res, freeaddrinfo);

  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
#ifdef _WIN32
    sock_ = ::WSASocketW(ai->ai_family, ai->ai_socktype, ai->ai_protocol,
                         nullptr, 0, 0);
#else
    sock_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
#endif
    if (is_invalid(sock_)) continue;

    set_nonblocking(sock_, true);

    int rc = ::connect(sock_, ai->ai_addr,
#ifdef _WIN32
                       (int)ai->ai_addrlen
#else
                       (int)ai->ai_addrlen
#endif
    );
    if (rc == 0) {
      prepare_for_io(deadline);
      return;
    }
#ifdef _WIN32
    int werr = WSAGetLastError();
    if (werr != WSAEWOULDBLOCK && werr != WSAEINPROGRESS) {
      do_close(sock_); sock_ = invalid_socket(); continue;
    }
#else
    if (errno != EINPROGRESS) {
      do_close(sock_); sock_ = invalid_socket(); continue;
    }
#endif
    // Wait for connect or timeout
    const auto wait = wait_for_socket(sock_, false, true, deadline);
    if (wait == SocketWaitResult::Ready) {
      // Check for connect success
      int err = 0;
#ifdef _WIN32
      int len = sizeof(err);
#else
      socklen_t len = sizeof(err);
#endif
      getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
      if (err == 0) {
        prepare_for_io(deadline);
        return;
      }
    }
    do_close(sock_); sock_ = invalid_socket();
    if (wait == SocketWaitResult::Timeout) break;
  }

    if (remaining(deadline) <= std::chrono::milliseconds::zero()) {
      throw TimeoutError("connect timeout");
    }
    throw IOError("connect failed for all resolved addresses");
  });
}

rs::util::Result<IOResult> SocketTransport::send(std::span<const std::byte> buf, Deadline deadline) {
  return rs::util::try_catch([&]() {
    if (buf.empty()) return IOResult{0, false};
    prepare_for_io(deadline);
    for (;;) {
      if (deadline_model_ == DeadlineModel::Strict) {
        const auto wait = wait_for_socket(sock_, false, true, deadline);
        if (wait == SocketWaitResult::Timeout) throw TimeoutError("send timeout");
        if (wait == SocketWaitResult::Failed) {
          throw IOError(platform::last_error_text("poll(send)"));
        }
      }
#ifdef _WIN32
      int n = ::send(sock_, reinterpret_cast<const char*>(buf.data()),
                     static_cast<int>(std::min<std::size_t>(buf.size(), INT_MAX)), 0);
      if (n != SOCKET_ERROR) return IOResult{static_cast<std::size_t>(n), false};
#else
      int flags = 0;
#ifdef MSG_NOSIGNAL
      flags = MSG_NOSIGNAL;
#endif
      ssize_t n = ::send(sock_, buf.data(), buf.size(), flags);
      if (n >= 0) return IOResult{static_cast<std::size_t>(n), false};
#endif
      const int error = last_socket_error();
      if (deadline_model_ == DeadlineModel::Strict && socket_error_would_block(error)) {
        continue;
      }
      if (socket_error_is_timeout(error)) throw TimeoutError("send timeout");
      throw IOError(platform::last_error_text("send"));
    }
  });
}

rs::util::Result<IOResult> SocketTransport::recv(std::span<std::byte> buf, Deadline deadline) {
  return rs::util::try_catch([&]() {
    if (buf.empty()) return IOResult{0, false};
    prepare_for_io(deadline);
    for (;;) {
      if (deadline_model_ == DeadlineModel::Strict) {
        const auto wait = wait_for_socket(sock_, true, false, deadline);
        if (wait == SocketWaitResult::Timeout) throw TimeoutError("recv timeout");
        if (wait == SocketWaitResult::Failed) {
          throw IOError(platform::last_error_text("poll(recv)"));
        }
      }
#ifdef _WIN32
      int n = ::recv(sock_, reinterpret_cast<char*>(buf.data()),
                     static_cast<int>(std::min<std::size_t>(buf.size(), INT_MAX)), 0);
      if (n > 0) return IOResult{static_cast<std::size_t>(n), false};
      if (n == 0) return IOResult{0, true};
#else
      ssize_t n = ::recv(sock_, buf.data(), buf.size(), 0);
      if (n > 0) return IOResult{static_cast<std::size_t>(n), false};
      if (n == 0) return IOResult{0, true};
#endif
      const int error = last_socket_error();
      if (deadline_model_ == DeadlineModel::Strict && socket_error_would_block(error)) {
        continue;
      }
      if (socket_error_is_timeout(error)) throw TimeoutError("recv timeout");
      throw IOError(platform::last_error_text("recv"));
    }
  });
}

void SocketTransport::close() noexcept {
  if (!is_invalid(sock_)) { do_close(sock_); sock_ = invalid_socket(); }
}

} // namespace rs::core::transport
