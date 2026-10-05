#pragma once
#include "odbcpp/transport/i_transport.h"
#include "odbcpp/transport/socket_transport.h"
#include "odbcpp/transport/start_tls_transport.h"
#include "odbcpp/transport/tls_configurable_transport.h"
#include "odbcpp/util/errors.h"
#include <memory>

namespace rs::core::transport {

class TLSTransport : public ITransport, public IStartTlsTransport,
                     public ITlsConfigurableTransport {
public:
  explicit TLSTransport(DeadlineModel deadline_model = DeadlineModel::Strict);
  ~TLSTransport() override;

  rs::util::Result<void> connect(std::string_view host, uint16_t port, rs::util::Deadline deadline) override;
  rs::util::Result<IOResult> send(std::span<const std::byte> buf, rs::util::Deadline) override;
  rs::util::Result<IOResult> recv(std::span<std::byte> buf, rs::util::Deadline) override;
  void close() noexcept override;

  rs::util::Result<void> connect_plain(
      std::string_view host, uint16_t port,
      rs::util::Deadline deadline) override;
  rs::util::Result<void> upgrade_to_tls(
      std::string_view host, rs::util::Deadline deadline) override;
  bool peer_identity_verified() noexcept override {
    return peer_identity_verified_;
  }

  // configuration
  void set_min_tls_version(long v); // e.g., TLS1_2_VERSION
  void set_verify(bool on) {
    verify_ = on;
    if (!on) peer_identity_verified_ = false;
  }
  void set_hostname_verification(bool on) {
    verify_host_ = on;
    if (!on) peer_identity_verified_ = false;
  }
  void set_ca_locations(const std::string& file,
                        const std::string& dir) override {
    ca_file_ = file; ca_dir_ = dir;
  }
  void set_deadline_model(DeadlineModel model) { tcp_.set_deadline_model(model); }
  DeadlineModel deadline_model() const noexcept { return tcp_.deadline_model(); }
#ifdef _WIN32
  using socket_t = SOCKET;
#else
  using socket_t = int;
#endif
  void upgrade_from(socket_t s, std::string_view host,
                    rs::util::Deadline deadline);

private:
  class ProviderState;

  SocketTransport tcp_;
  std::unique_ptr<ProviderState> provider_;
  bool verify_ {true};
  bool verify_host_ {true}; 
  bool peer_identity_verified_ {false};
  long min_version_ {0x0303};  // TLS 1.2 protocol version.
  std::string ca_file_, ca_dir_;

  void ensure_ctx();
  void upgrade_impl(std::string_view host, rs::util::Deadline deadline);
};

} // namespace rs::core::transport
