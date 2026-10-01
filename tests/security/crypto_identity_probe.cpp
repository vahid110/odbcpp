// Reports the production adapters without requiring a socket or database.
#include "core/security/crypto.h"
#include "core/security/tls_client.h"

#include <iostream>

int main() {
  const auto identity = rs::core::security::crypto_provider_identity();
  const rs::core::security::TlsClientConfig config;
  std::cout << identity.provider << '\n' << identity.compile_version << '\n'
            << identity.runtime_version << '\n' << identity.fips_enabled << '\n'
            << config.minimum_version << '\n' << config.verify_peer << '\n'
            << config.verify_hostname << '\n';
  return std::cout.good() ? 0 : 1;
}
