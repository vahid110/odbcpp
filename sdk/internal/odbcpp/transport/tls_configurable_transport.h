#pragma once

#include <string>

namespace rs::core::transport {

// Security-policy extension for transports whose trust store is configured by
// the backend session before opening or upgrading a connection.
class ITlsConfigurableTransport {
public:
  virtual ~ITlsConfigurableTransport() = default;

  virtual void set_ca_locations(const std::string& file,
                                const std::string& directory) = 0;
};

} // namespace rs::core::transport
