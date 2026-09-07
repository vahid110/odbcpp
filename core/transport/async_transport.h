#pragma once
#include "i_transport.h"
#include "core/util/result.h"
#include <future>
#include <memory>
#include <functional>

namespace rs::core::transport {

// Async operation handle for cancellation and status
class AsyncOperation {
public:
  virtual ~AsyncOperation() = default;
  virtual void cancel() = 0;
  virtual bool is_complete() const = 0;
  virtual bool is_cancelled() const = 0;
};

// Async transport interface
class IAsyncTransport : public ITransport {
public:
  using ConnectCallback = std::function<void(rs::util::Result<void>)>;
  using SendCallback = std::function<void(rs::util::Result<IOResult>)>;
  using RecvCallback = std::function<void(rs::util::Result<IOResult>)>;
  
  // Async operations with callbacks
  virtual std::unique_ptr<AsyncOperation> connect_async(
    std::string_view host, uint16_t port, rs::util::Deadline deadline,
    ConnectCallback callback) = 0;
    
  virtual std::unique_ptr<AsyncOperation> send_async(
    std::span<const std::byte> buf, rs::util::Deadline deadline,
    SendCallback callback) = 0;
    
  virtual std::unique_ptr<AsyncOperation> recv_async(
    std::span<std::byte> buf, rs::util::Deadline deadline,
    RecvCallback callback) = 0;
  
  // Future-based interface
  virtual std::future<rs::util::Result<void>> connect_future(
    std::string_view host, uint16_t port, rs::util::Deadline deadline) = 0;
    
  virtual std::future<rs::util::Result<IOResult>> send_future(
    std::span<const std::byte> buf, rs::util::Deadline deadline) = 0;
    
  virtual std::future<rs::util::Result<IOResult>> recv_future(
    std::span<std::byte> buf, rs::util::Deadline deadline) = 0;
};

// Platform-specific async transport implementations
#ifdef __linux__
class EpollTransport;
using PlatformAsyncTransport = EpollTransport;
#elif defined(_WIN32)
class IocpTransport;
using PlatformAsyncTransport = IocpTransport;
#else
class ThreadPoolTransport;
using PlatformAsyncTransport = ThreadPoolTransport;
#endif

} // namespace rs::core::transport
