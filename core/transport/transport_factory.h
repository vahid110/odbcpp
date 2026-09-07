#pragma once

#include "i_transport.h"
#include "transport_options.h"

#include <memory>

namespace rs::core::transport {

class TransportFactory {
public:
  // Auto currently selects Sync. It will prefer the native async backend once
  // the matching IOCP/epoll implementation is available and validated.
  static TransportMode resolve_mode(const TransportOptions& options) noexcept;

  static std::unique_ptr<ITransport> create(const TransportOptions& options,
                                            bool use_tls);
};

} // namespace rs::core::transport
