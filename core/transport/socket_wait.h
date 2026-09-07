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

inline bool socket_error_is_timeout(int error) noexcept {
#ifdef _WIN32
  return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
#else
  return error == EAGAIN || error == EWOULDBLOCK || error == ETIMEDOUT;
#endif
}

inline SocketWaitResult wait_for_socket(native_socket_t socket, bool read,
                                        bool write, rs::util::Deadline deadline) {
  for (;;) {
    const auto left = rs::util::remaining(deadline);
    if (left <= std::chrono::milliseconds::zero()) {
      return SocketWaitResult::Timeout;
    }

    const auto bounded = std::min<long long>(
        left.count(), std::numeric_limits<int>::max());
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
    if (result == 0) return SocketWaitResult::Timeout;

#ifdef _WIN32
    if (WSAGetLastError() == WSAEINTR) continue;
#else
    if (errno == EINTR) continue;
#endif
    return SocketWaitResult::Failed;
  }
}

} // namespace rs::core::transport
