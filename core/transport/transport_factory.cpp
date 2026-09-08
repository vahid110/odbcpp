#include "transport_factory.h"
#include "async_tls_transport.h"

#ifdef __linux__
#include "epoll_transport.h"
#elif defined(_WIN32)
#include "iocp_transport.h"
#endif
#include "socket_transport.h"
#include "tls_transport.h"

#include <stdexcept>

namespace rs::core::transport {

TransportMode TransportFactory::resolve_mode(const TransportOptions& options) noexcept {
  if (options.mode == TransportMode::Auto) return TransportMode::Sync;
  return options.mode;
}

std::unique_ptr<ITransport> TransportFactory::create(
    const TransportOptions& options, bool use_tls) {
  if (resolve_mode(options) == TransportMode::Async) {
#ifdef __linux__
    if (options.async_engine == AsyncEngine::IOCP) {
      throw std::invalid_argument(
          "AsyncEngine=IOCP is only available on Windows");
    }
    if (options.deadline_model != DeadlineModel::Strict) {
      throw std::invalid_argument(
          "AsyncEngine=Epoll requires DeadlineModel=Strict");
    }
    auto native = std::make_unique<EpollTransport>(
        options.async_max_inflight, options.async_queue_depth);
    if (use_tls) {
      return std::make_unique<AsyncTlsTransport>(
          std::move(native), options.async_max_inflight,
          options.async_queue_depth);
    }
    return native;
#elif defined(_WIN32)
    if (options.async_engine == AsyncEngine::Epoll) {
      throw std::invalid_argument(
          "AsyncEngine=Epoll is only available on Linux");
    }
    if (options.deadline_model != DeadlineModel::Strict) {
      throw std::invalid_argument(
          "AsyncEngine=IOCP requires DeadlineModel=Strict");
    }
    auto native = std::make_unique<IocpTransport>(
        options.async_max_inflight, options.async_queue_depth);
    if (use_tls) {
      return std::make_unique<AsyncTlsTransport>(
          std::move(native), options.async_max_inflight,
          options.async_queue_depth);
    }
    return native;
#else
    throw std::invalid_argument(
        "AsyncEngine=" + std::string(to_string(options.async_engine)) +
        " is not available on this platform; use TransportMode=Auto or Sync");
#endif
  }

  if (use_tls) {
    return std::make_unique<TLSTransport>(options.deadline_model);
  }
  return std::make_unique<SocketTransport>(options.deadline_model);
}

} // namespace rs::core::transport
