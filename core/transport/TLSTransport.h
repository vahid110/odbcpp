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
  void set_hostname_verification(bool on) { verify_host_ = on; }
  void set_ca_locations(const std::string& file, const std::string& dir) {
    ca_file_ = file; ca_dir_ = dir;
  }
#ifdef _WIN32
  using socket_t = SOCKET;
#else
  using socket_t = int;
#endif
  void upgrade_from(socket_t s, std::string_view host, rs::util::Deadline deadline);

private:
  SocketTransport tcp_;
  SSL_CTX* ctx_ {nullptr};
  SSL* ssl_ {nullptr};
  std::string sni_host_;
  bool verify_ {true};
  bool verify_host_ {true}; 
  long min_version_ {TLS1_2_VERSION};
  std::string ca_file_, ca_dir_;

  void ensure_ctx();
  void verify_hostname(X509* cert);
};

} // namespace rs::core::transport