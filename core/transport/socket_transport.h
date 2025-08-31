#pragma once
#include "i_transport.h"
#include "core/util/errors.h"
#include "core/util/platform.h"
#include <optional>

namespace rs::core::transport {

class SocketTransport : public ITransport {
public:
  SocketTransport();
  ~SocketTransport() override;

  rs::util::Result<void> connect(std::string_view host, uint16_t port, rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> send(std::span<const std::byte> buf, rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> recv(std::span<std::byte> buf, rs::util::Deadline deadline) override;
  void close() noexcept override;

  // Access raw socket for TLS wrapper
#ifdef _WIN32
  using socket_t = SOCKET;
#else
  using socket_t = int;
#endif
  socket_t native() const { return sock_; }
  socket_t release() {
    socket_t tmp = sock_;
    sock_ = invalid_socket();
    return tmp;
  }

private:
  socket_t sock_ { invalid_socket() };
  platform::WSAInit wsa_init_{}; // ensures WSA on Windows

  static socket_t invalid_socket();
  static bool is_invalid(socket_t s);
  static void set_nonblocking(socket_t s, bool nb);
  static void set_timeouts(socket_t s, std::chrono::milliseconds rw);
  static void do_close(socket_t s) noexcept;
};

} // namespace rs::core::transport