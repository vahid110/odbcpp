// transport_poll.h
#pragma once
#include "transport_deadline.h"

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "Ws2_32.lib")
#else
  #include <poll.h>
  #include <unistd.h>
  #include <errno.h>
#endif

namespace rs::core::transport {

// Wait until fd is ready for the given events or deadline expires.
// events: bitmask — 0x01 = read, 0x02 = write, 0x04 = error.
inline Errc wait_fd_ready(int fd, int events, Deadline dl) {
  auto rem = dl.remaining_ms();
  int timeout = rem == std::chrono::milliseconds::max()
                  ? -1
                  : static_cast<int>(rem.count());
#if defined(_WIN32)
  WSAPOLLFD p{};
  p.fd = (SOCKET)fd;
  p.events = 0;
  if (events & 0x01) p.events |= POLLRDNORM;
  if (events & 0x02) p.events |= POLLWRNORM;
  if (events & 0x04) p.events |= POLLERR;

  int r = WSAPoll(&p, 1, timeout);
  if (r == 0) return Errc::Timeout;
  if (r < 0)  return Errc::SyscallFailed;
  return Errc::Ok;
#else
  struct pollfd p{};
  p.fd = fd;
  p.events = 0;
  if (events & 0x01) p.events |= POLLIN;
  if (events & 0x02) p.events |= POLLOUT;
  if (events & 0x04) p.events |= POLLERR;

  int r = ::poll(&p, 1, timeout);
  if (r == 0) return Errc::Timeout;
  if (r < 0)  return (errno == EINTR) ? Errc::WantRetry : Errc::SyscallFailed;
  return Errc::Ok;
#endif
}

} // namespace rs::core::transport
