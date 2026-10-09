#pragma once
#include "odbcpp/transport/i_transport.h"
#include "odbcpp/transport/cancellation_wait.h"
#include <array>
#include "odbcpp/transport/deadline_model.h"
#include "odbcpp/util/errors.h"
#include "odbcpp/util/platform.h"
#include <optional>

namespace rs::core::transport {

class SocketTransport : public ITransport, public IServerCancelTransport {
public:
  explicit SocketTransport(DeadlineModel deadline_model = DeadlineModel::Strict);
  ~SocketTransport() override;

  rs::util::Result<void> connect(std::string_view host, uint16_t port, rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> send(std::span<const std::byte> buf, rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> recv(std::span<std::byte> buf, rs::util::Deadline deadline) override;
  void close() noexcept override;
  bool supports_server_cancel() const noexcept override { return deadline_model_ == DeadlineModel::Strict && !is_invalid(sock_); }
  void cancellation_wait(std::shared_ptr<CancellationWait> control) noexcept override { cancellation_wait_ = std::move(control); }
  std::unique_ptr<ITransport> cancellation_peer() const override;
  bool copy_cancellation_peer(const SocketTransport& peer) noexcept;
  rs::util::Result<void> connect_cancellation_peer(rs::util::Deadline) override;

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
  void adopt(socket_t socket);
  void prepare_for_io(rs::util::Deadline deadline);
  void set_deadline_model(DeadlineModel model);
  DeadlineModel deadline_model() const noexcept { return deadline_model_; }

private:
  socket_t sock_ { invalid_socket() };
  std::array<std::byte, sizeof(sockaddr_storage)> peer_address_{};
  int peer_address_size_{};
  std::shared_ptr<CancellationWait> cancellation_wait_;
  platform::WSAInit wsa_init_{}; // ensures WSA on Windows
  DeadlineModel deadline_model_;

  static socket_t invalid_socket();
  static bool is_invalid(socket_t s);
  static void suppress_sigpipe(socket_t s);
  static void set_nonblocking(socket_t s, bool nb);
  static void set_timeouts(socket_t s, std::chrono::milliseconds rw);
  static void do_close(socket_t s) noexcept;
};

} // namespace rs::core::transport
