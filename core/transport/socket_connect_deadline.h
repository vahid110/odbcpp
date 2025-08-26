// socket_connect_deadline.h
#pragma once
#include "transport_deadline.h"
#include "transport_poll.h"

namespace rs::core::transport {
// Expects fd already created (AF_INET/AF_INET6), non-blocking set by caller.
inline Error connect_with_deadline(int fd, const sockaddr* sa, socklen_t slen, Deadline dl) {
#if defined(_WIN32)
  u_long nb = 1; ioctlsocket(fd, FIONBIO, &nb);
  int r = ::connect(fd, sa, slen);
  if (r == 0) return {}; // connected immediately
  int werr = WSAGetLastError();
  if (werr != WSAEWOULDBLOCK && werr != WSAEINPROGRESS) {
    return {Errc::SyscallFailed, werr, 0, "connect", "immediate fail"};
  }
  auto ec = wait_fd_ready(fd, /*write*/0x02 | /*error*/0x04, dl);
  if (ec == Errc::Timeout) return {Errc::Timeout, 0, 0, "connect", "deadline"};
  if (ec != Errc::Ok)      return {ec, WSAGetLastError(), 0, "connect", "poll"};
  // Check SO_ERROR
  int soerr=0; int len=sizeof(soerr);
  getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
  if (soerr != 0) return {Errc::SyscallFailed, soerr, 0, "connect", "SO_ERROR"};
  return {};
#else
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int r = ::connect(fd, sa, slen);
  if (r == 0) return {};
  if (errno != EINPROGRESS) {
    return {Errc::SyscallFailed, errno, 0, "connect", "immediate fail"};
  }
  auto ec = wait_fd_ready(fd, /*write*/0x02 | /*error*/0x04, dl);
  if (ec == Errc::Timeout) return {Errc::Timeout, errno, 0, "connect", "deadline"};
  if (ec != Errc::Ok)      return {ec, errno, 0, "connect", "poll"};
  int soerr=0; socklen_t len=sizeof(soerr);
  ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len);
  if (soerr != 0) return {Errc::SyscallFailed, soerr, 0, "connect", "SO_ERROR"};
  return {};
#endif
}
} // namespace rs::core::transport
