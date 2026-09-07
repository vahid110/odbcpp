#pragma once

#include "async_transport.h"

#ifdef _WIN32

#include <cstddef>
#include <memory>

namespace rs::core::transport {

// Windows-native asynchronous socket transport. Overlapped Winsock requests
// are completed by a dedicated IOCP reactor thread.
class IocpTransport : public IAsyncTransport {
public:
  explicit IocpTransport(std::size_t max_inflight = 64,
                         std::size_t queue_depth = 256);
  ~IocpTransport() override;

  IocpTransport(const IocpTransport&) = delete;
  IocpTransport& operator=(const IocpTransport&) = delete;

  rs::util::Result<void> connect(std::string_view host, uint16_t port,
                                 rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> send(std::span<const std::byte> buf,
                                  rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> recv(std::span<std::byte> buf,
                                  rs::util::Deadline deadline) override;
  void close() noexcept override;

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

  std::size_t max_inflight() const noexcept;
  std::size_t queue_depth() const noexcept;

private:
  class Core;
  std::shared_ptr<Core> core_;
};

} // namespace rs::core::transport

#endif // _WIN32
