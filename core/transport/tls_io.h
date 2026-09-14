// core/transport/tls_io.h
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <limits>
#include <span>

#include <openssl/ssl.h>
#include <openssl/err.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <poll.h>
  #include <unistd.h>
  #include <errno.h>
#endif

#include "core/util/deadline.h" // rs::util::Deadline + rs::util::remaining
#include "core/transport/socket_wait.h"

namespace rs::core::transport {

enum class Errc {
  Ok = 0,
  Timeout,
  WantRetry,
  SyscallFailed,
  HandshakeFailed,
  TlsFailed,
};

struct Error {
  Errc code{Errc::Ok};
  const char* where{""};   // "handshake" | "send" | "recv"
  const char* detail{""};  // "want_read" | "want_write" | "SSL_*" | etc.
  int  sys_errno{0};
  long ssl_err{0};
};

inline int tls_io_chunk_size(std::size_t remaining) noexcept {
  return static_cast<int>(std::min(
      remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

inline bool deadline_expired(rs::util::Deadline dl) {
  using namespace std::chrono;
  return rs::util::remaining(dl) <= milliseconds{0};
}

// Wait for readiness (read=0x01, write=0x02)
inline Errc wait_fd_ready(
#if defined(_WIN32)
  SOCKET fd,
#else
  int fd,
#endif
  int events,
  rs::util::Deadline dl)
{
  const auto result = wait_for_socket(fd, (events & 0x01) != 0,
                                      (events & 0x02) != 0, dl);
  if (result == SocketWaitResult::Timeout) return Errc::Timeout;
  if (result == SocketWaitResult::Failed) return Errc::SyscallFailed;
  return Errc::Ok;
}

inline Error tls_handshake_with_deadline(SSL* ssl,
#if defined(_WIN32)
  SOCKET fd,
#else
  int fd,
#endif
  rs::util::Deadline dl)
{
  for (;;) {
    if (deadline_expired(dl)) return {Errc::Timeout, "handshake", "deadline"};
    int rc = ::SSL_connect(ssl);
    if (rc == 1) return {};
    int e = ::SSL_get_error(ssl, rc);
    if (e == SSL_ERROR_WANT_READ) {
      auto ec = wait_fd_ready(fd, 0x01, dl);
      if (ec != Errc::Ok) return {ec, "handshake", "want_read"};
      continue;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
      auto ec = wait_fd_ready(fd, 0x02, dl);
      if (ec != Errc::Ok) return {ec, "handshake", "want_write"};
      continue;
    }
    if (e == SSL_ERROR_SYSCALL && socket_error_is_timeout(last_socket_error())) {
      return {Errc::Timeout, "handshake", "socket_timeout"};
    }
    long serr = ::ERR_get_error();
    return {Errc::HandshakeFailed, "handshake", "SSL_connect", 0, serr};
  }
}

inline Error tls_write_all(SSL* ssl,
#if defined(_WIN32)
  SOCKET fd,
#else
  int fd,
#endif
  const uint8_t* buf, size_t len, rs::util::Deadline dl, size_t& written)
{
  written = 0;
  while (written < len) {
    if (deadline_expired(dl)) return {Errc::Timeout, "send", "deadline"};
    int rc = ::SSL_write(ssl, buf + written,
                         tls_io_chunk_size(len - written));
    if (rc > 0) { written += static_cast<size_t>(rc); continue; }
    int e = ::SSL_get_error(ssl, rc);
    if (e == SSL_ERROR_WANT_READ) {
      auto ec = wait_fd_ready(fd, 0x01, dl);
      if (ec != Errc::Ok) return {ec, "send", "want_read"};
      continue;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
      auto ec = wait_fd_ready(fd, 0x02, dl);
      if (ec != Errc::Ok) return {ec, "send", "want_write"};
      continue;
    }
    if (e == SSL_ERROR_SYSCALL && socket_error_is_timeout(last_socket_error())) {
      return {Errc::Timeout, "send", "socket_timeout"};
    }
    long serr = ::ERR_get_error();
    return {e == SSL_ERROR_SSL ? Errc::TlsFailed : Errc::SyscallFailed,
            "send", "SSL_write", 0, serr};
  }
  return {};
}

inline Error tls_read_some(SSL* ssl,
#if defined(_WIN32)
  SOCKET fd,
#else
  int fd,
#endif
  uint8_t* buf, size_t cap, rs::util::Deadline dl, size_t& got, bool& eof)
{
  got = 0; eof = false;
  if (cap == 0) return {};
  for (;;) {
    if (deadline_expired(dl)) return {Errc::Timeout, "recv", "deadline"};
    int rc = ::SSL_read(ssl, buf, tls_io_chunk_size(cap));
    if (rc > 0) { got = static_cast<size_t>(rc); return {}; }
    int e = ::SSL_get_error(ssl, rc);
    if (e == SSL_ERROR_ZERO_RETURN) { eof = true; return {}; }
    if (e == SSL_ERROR_WANT_READ) {
      auto ec = wait_fd_ready(fd, 0x01, dl);
      if (ec != Errc::Ok) return {ec, "recv", "want_read"};
      continue;
    }
    if (e == SSL_ERROR_WANT_WRITE) {
      auto ec = wait_fd_ready(fd, 0x02, dl);
      if (ec != Errc::Ok) return {ec, "recv", "want_write"};
      continue;
    }
    if (e == SSL_ERROR_SYSCALL && socket_error_is_timeout(last_socket_error())) {
      return {Errc::Timeout, "recv", "socket_timeout"};
    }
    long serr = ::ERR_get_error();
    // OpenSSL 1.1 reports a bare TCP EOF as SSL_ERROR_SYSCALL with rc == 0;
    // OpenSSL 3 reports it as SSL_ERROR_SSL. Neither is a clean TLS shutdown.
    const bool tls_failure = e == SSL_ERROR_SSL ||
        (e == SSL_ERROR_SYSCALL && rc == 0 && serr == 0);
    return {tls_failure ? Errc::TlsFailed : Errc::SyscallFailed,
            "recv", "SSL_read", 0, serr};
  }
}

} // namespace rs::core::transport
