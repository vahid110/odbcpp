#pragma once

#include <openssl/x509v3.h>

#include <string>
#include <string_view>

namespace rs::core::transport {

inline bool tls_host_uses_sni(std::string_view host) {
  if (host.empty() || host.find('\0') != std::string_view::npos) return false;

  const std::string name(host);
  ASN1_OCTET_STRING* address = a2i_IPADDRESS(name.c_str());
  if (!address) return true;
  ASN1_OCTET_STRING_free(address);
  return false;
}

inline bool tls_certificate_matches_host(X509* certificate,
                                         std::string_view host) {
  if (host.find('\0') != std::string_view::npos) return false;

  const std::string name(host);
  const int ip_match = X509_check_ip_asc(certificate, name.c_str(), 0);
  if (ip_match != -2) return ip_match == 1;
  return X509_check_host(certificate, name.c_str(), name.size(), 0,
                         nullptr) == 1;
}

} // namespace rs::core::transport
