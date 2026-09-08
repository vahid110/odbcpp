#pragma once

#include "async_transport.h"
#include "start_tls_transport.h"

#include <cstddef>
#include <memory>
#include <string>

namespace rs::core::transport {

// TLS wrapper for native asynchronous transports. OpenSSL uses memory BIOs;
// encrypted records are pumped through the wrapped epoll/IOCP transport.
class AsyncTlsTransport : public IAsyncTransport, public IStartTlsTransport {
public:
  explicit AsyncTlsTransport(
      std::unique_ptr<IAsyncTransport> transport,
      std::size_t max_inflight = 64,
      std::size_t queue_depth = 256);
  ~AsyncTlsTransport() override;

  AsyncTlsTransport(const AsyncTlsTransport&) = delete;
  AsyncTlsTransport& operator=(const AsyncTlsTransport&) = delete;

  rs::util::Result<void> connect(std::string_view host, uint16_t port,
                                 rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> send(std::span<const std::byte> buf,
                                  rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> recv(std::span<std::byte> buf,
                                  rs::util::Deadline deadline) override;
  void close() noexcept override;

  rs::util::Result<void> connect_plain(
      std::string_view host, uint16_t port,
      rs::util::Deadline deadline) override;
  rs::util::Result<void> upgrade_to_tls(
      std::string_view host, rs::util::Deadline deadline) override;

  std::unique_ptr<AsyncOperation> connect_async(
      std::string_view host, uint16_t port, rs::util::Deadline deadline,
      ConnectCallback callback) override;
  std::unique_ptr<AsyncOperation> send_async(
      std::span<const std::byte> buf, rs::util::Deadline deadline,
      SendCallback callback) override;
  std::unique_ptr<AsyncOperation> recv_async(
      std::span<std::byte> buf, rs::util::Deadline deadline,
      RecvCallback callback) override;

  std::future<rs::util::Result<void>> connect_future(
      std::string_view host, uint16_t port,
      rs::util::Deadline deadline) override;
  std::future<rs::util::Result<IOResult>> send_future(
      std::span<const std::byte> buf, rs::util::Deadline deadline) override;
  std::future<rs::util::Result<IOResult>> recv_future(
      std::span<std::byte> buf, rs::util::Deadline deadline) override;

  void set_min_tls_version(long version);
  void set_verify(bool enabled);
  void set_hostname_verification(bool enabled);
  void set_ca_locations(std::string file, std::string directory);

  std::size_t max_inflight() const noexcept;
  std::size_t queue_depth() const noexcept;

private:
  class Core;
  std::shared_ptr<Core> core_;
};

} // namespace rs::core::transport
