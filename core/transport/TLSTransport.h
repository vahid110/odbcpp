#pragma once
#include "ITransport.h"
#include "SocketTransport.h"
#include "core/util/Errors.h"
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <memory>

namespace rs::core::transport {

class TLSTransport : public ITransport {
public:
  TLSTransport();
  ~TLSTransport() override;

  void connect(std::string_view host, uint16_t port, rs::util::Deadline deadline) override;
  IOResult send(std::span<const std::byte> buf, rs::util::Deadline) override;
  IOResult recv(std::span<std::byte> buf, rs::util::Deadline) override;
  void close() noexcept override;

  // configuration
  void set_min_tls_version(long v); // e.g., TLS1_2_VERSION
  void set_verify(bool on) { verify_ = on; }

private:
  SocketTransport tcp_;
  SSL_CTX* ctx_ {nullptr};
  SSL* ssl_ {nullptr};
  std::string sni_host_;
  bool verify_ {true};
  long min_version_ {TLS1_2_VERSION};

  void ensure_ctx();
  void verify_hostname(X509* cert);
};

} // namespace rs::core::transport