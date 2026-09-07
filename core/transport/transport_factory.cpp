#include "transport_factory.h"

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
    throw std::invalid_argument(
        "TransportMode=Async is not available yet; use Auto or Sync until "
        "the native IOCP/epoll backend is enabled");
  }

  if (use_tls) {
    return std::make_unique<TLSTransport>(options.deadline_model);
  }
  return std::make_unique<SocketTransport>(options.deadline_model);
}

} // namespace rs::core::transport
