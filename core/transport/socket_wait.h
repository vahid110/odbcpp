#pragma once

#include "core/util/deadline.h"
#include "core/util/platform.h"

#include <algorithm>
#include <chrono>
#include <limits>

namespace rs::core::transport {

#ifdef _WIN32
using native_socket_t = SOCKET;
#else
using native_socket_t = int;
#endif

enum class SocketWaitResult {
  Ready,
  Timeout,
  Failed,
};

inline int last_socket_error() noexcept {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

inline bool socket_error_would_block(int error) noexcept {
#ifdef _WIN32
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
  return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS;
#endif
}

inline bool socket_error_interrupted(int error) noexcept {
#ifdef _WIN32
  return error == WSAEINTR;
#else
  return error == EINTR;
#endif
}

inline bool socket_error_is_timeout(int error) noexcept {
#ifdef _WIN32
  return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
  return error == EAGAIN || error == EWOULDBLOCK || error == ETIMEDOUT;
#endif
}

inline SocketWaitResult wait_for_socket(native_socket_t socket, bool read,
                                        bool write, rs::util::Deadline deadline,
                                        int max_wait_ms = std::numeric_limits<int>::max()) {
  for (;;) {
    const auto left = rs::util::remaining(deadline);
    if (left <= std::chrono::milliseconds::zero()) {
      return SocketWaitResult::Timeout;
    }

    const auto bounded = std::min<long long>(left.count(), max_wait_ms);
    const int timeout_ms = static_cast<int>(std::max<long long>(1, bounded));

#ifdef _WIN32
    WSAPOLLFD descriptor{};
    descriptor.fd = socket;
    if (read) descriptor.events |= POLLRDNORM;
    if (write) descriptor.events |= POLLWRNORM;
    const int result = ::WSAPoll(&descriptor, 1, timeout_ms);
#else
    pollfd descriptor{};
    descriptor.fd = socket;
    if (read) descriptor.events |= POLLIN;
    if (write) descriptor.events |= POLLOUT;
    const int result = ::poll(&descriptor, 1, timeout_ms);
#endif

    if (result > 0) return SocketWaitResult::Ready;
    if (result == 0) continue;

#ifdef _WIN32
    if (WSAGetLastError() == WSAEINTR) continue;
#else
    if (errno == EINTR) continue;
#endif
    return SocketWaitResult::Failed;
  }
}

#ifdef _WIN32
inline SocketWaitResult wait_for_connect(native_socket_t socket,
                                         WSAEVENT event,
                                         rs::util::Deadline deadline) {
  for (;;) {
    const auto left = rs::util::remaining(deadline);
    if (left <= std::chrono::milliseconds::zero()) {
      return SocketWaitResult::Timeout;
    }
    const auto bounded = std::min<long long>(left.count(),
                                             std::numeric_limits<DWORD>::max() - 1);
    const auto result = ::WSAWaitForMultipleEvents(
        1, &event, FALSE, static_cast<DWORD>(bounded), FALSE);
    if (result == WSA_WAIT_TIMEOUT) continue;
    if (result == WSA_WAIT_FAILED) {
      if (::WSAGetLastError() == WSAEINTR) continue;
      return SocketWaitResult::Failed;
    }
    if (result != WSA_WAIT_EVENT_0) return SocketWaitResult::Failed;
    WSANETWORKEVENTS events{};
    if (::WSAEnumNetworkEvents(socket, event, &events) != 0) {
      return SocketWaitResult::Failed;
    }
    if (events.lNetworkEvents & FD_CONNECT) {
      return events.iErrorCode[FD_CONNECT_BIT] == 0
                 ? SocketWaitResult::Ready : SocketWaitResult::Failed;
    }
  }
}
#else
inline SocketWaitResult wait_for_connect(native_socket_t socket,
                                         rs::util::Deadline deadline) {
  return wait_for_socket(socket, false, true, deadline);
}
#endif

} // namespace rs::core::transport
