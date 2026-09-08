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
};

} // namespace rs::core::transport
