#pragma once

#include "i_transport.h"
#include "transport_options.h"

#include <memory>

namespace rs::core::transport {

class TransportFactory {
public:
  // Auto remains on the synchronous path until native TLS and Windows async
  // support have the same coverage as the synchronous implementation.
  static TransportMode resolve_mode(const TransportOptions& options) noexcept;

  static std::unique_ptr<ITransport> create(const TransportOptions& options,
                                            bool use_tls);
};

} // namespace rs::core::transport
