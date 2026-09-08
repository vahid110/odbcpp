#pragma once

#include "i_transport.h"
#include "transport_options.h"

#include <memory>

namespace rs::core::transport {

class TransportFactory {
public:
  // Auto selects the platform-native engine with strict deadlines and falls
  // back to the synchronous implementation for SocketTimeout or unsupported
  // platform/engine combinations.
  static TransportMode resolve_mode(const TransportOptions& options) noexcept;

  static std::unique_ptr<ITransport> create(const TransportOptions& options,
                                            bool use_tls);
};

} // namespace rs::core::transport
