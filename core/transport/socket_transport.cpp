#include "socket_transport.h"
#include <cassert>
#include <cstring>
#include <string>

using rs::util::Deadline;
using rs::util::remaining;
using rs::util::IOError;
using rs::util::TimeoutError;

namespace rs::core::transport {

#ifdef _WIN32
// Wait for connect completion with a timeout (Windows)
static int poll_connect(SocketTransport::socket_t s, int timeout_ms) {
  fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
  TIMEVAL tv{ timeout_ms/1000, (timeout_ms%1000)*1000 };
  return select(0, nullptr, &wfds, nullptr, &tv);
}
#else
// Wait for connect completion with a timeout (POSIX)
static int poll_connect(int s, int timeout_ms) {
  struct pollfd p{ s, POLLOUT, 0 };
  return ::poll(&p, 1, timeout_ms);
}
#endif

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
  ioctlsocket(s, FIONBIO, &mode);
#else
  int flags = fcntl(s, F_GETFL, 0);
  if (flags < 0) flags = 0;
  fcntl(s, F_SETFL, nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

void SocketTransport::set_timeouts(socket_t s, std::chrono::milliseconds rw) {
#ifdef _WIN32
  DWORD ms = static_cast<DWORD>(rw.count());
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof(ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof(ms));
#else
  timeval tv{ (long)(rw.count()/1000), (int)((rw.count()%1000)*1000) };
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

SocketTransport::SocketTransport() = default;
SocketTransport::~SocketTransport() { close(); }

void SocketTransport::connect(std::string_view host, uint16_t port, Deadline deadline) {
  close();

  // Resolve
  addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr; std::string port_str = std::to_string(port);
  int gai = getaddrinfo(std::string(host).c_str(), port_str.c_str(), &hints, &res);
  if (gai != 0) throw IOError(std::string("getaddrinfo: ") + gai_strerror(gai));

  auto guard = std::unique_ptr<addrinfo, void(*)(addrinfo*)>(res, freeaddrinfo);

  for (addrinfo* ai = res; ai; ai = ai->ai_next) {
#ifdef _WIN32
    sock_ = ::WSASocket(ai->ai_family, ai->ai_socktype, ai->ai_protocol, nullptr, 0, 0);
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
      set_nonblocking(sock_, false);
      set_timeouts(sock_, remaining(deadline));
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
    int wait = poll_connect(sock_, (int)remaining(deadline).count());
    if (wait > 0) {
      // Check for connect success
      int err = 0; socklen_t len = sizeof(err);
      getsockopt(sock_, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
      if (err == 0) {
        set_nonblocking(sock_, false);
        set_timeouts(sock_, remaining(deadline));
        return;
      }
    }
    do_close(sock_); sock_ = invalid_socket();
  }

  throw TimeoutError("connect timeout or no route to host");
}

IOResult SocketTransport::send(std::span<const std::byte> buf, Deadline /*deadline*/) {
  if (is_invalid(sock_)) throw IOError("send on closed socket");
#ifdef _WIN32
  int n = ::send(sock_, reinterpret_cast<const char*>(buf.data()), (int)buf.size(), 0);
  if (n == SOCKET_ERROR) throw IOError(platform::last_error_text("send"));
#else
  ssize_t n = ::send(sock_, buf.data(), buf.size(), 0);
  if (n < 0) throw IOError(platform::last_error_text("send"));
#endif
  return IOResult{ static_cast<std::size_t>(n), false };
}

IOResult SocketTransport::recv(std::span<std::byte> buf, Deadline /*deadline*/) {
  if (is_invalid(sock_)) throw IOError("recv on closed socket");
#ifdef _WIN32
  int n = ::recv(sock_, reinterpret_cast<char*>(buf.data()), (int)buf.size(), 0);
  if (n == 0) return IOResult{0, true};
  if (n == SOCKET_ERROR) throw IOError(platform::last_error_text("recv"));
#else
  ssize_t n = ::recv(sock_, buf.data(), buf.size(), 0);
  if (n == 0) return IOResult{0, true};
  if (n < 0) throw IOError(platform::last_error_text("recv"));
#endif
  return IOResult{ static_cast<std::size_t>(n), false };
}

void SocketTransport::close() noexcept {
  if (!is_invalid(sock_)) { do_close(sock_); sock_ = invalid_socket(); }
}

} // namespace rs::core::transport
