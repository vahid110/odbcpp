#pragma once

#include "i_transport.h"

namespace rs::core::transport {

// Transport extension for protocols such as PostgreSQL that begin in plain
// text, negotiate TLS in-band, and then upgrade the existing connection.
class IStartTlsTransport {
public:
  virtual ~IStartTlsTransport() = default;

  virtual rs::util::Result<void> connect_plain(
      std::string_view host, uint16_t port,
      rs::util::Deadline deadline) = 0;
  virtual rs::util::Result<void> upgrade_to_tls(
      std::string_view host, rs::util::Deadline deadline) = 0;

  // True only after the active TLS session has verified both the peer's
  // certificate chain and its host identity. Unknown/custom transports fail
  // closed so authentication policy cannot infer verification from encryption
  // alone.
  virtual bool peer_identity_verified() noexcept { return false; }
};

} // namespace rs::core::transport
