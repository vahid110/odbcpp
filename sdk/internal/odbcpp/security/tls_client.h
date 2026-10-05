#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace rs::core::security {

struct TlsClientConfig {
  long minimum_version{0x0303};
  bool verify_peer{true};
  bool verify_hostname{true};
  std::string ca_file;
  std::string ca_directory;
};

enum class TlsStepState {
  Complete,
  WantRead,
  WantWrite,
  Closed,
  TlsError,
  SystemError,
};

struct TlsStep {
  TlsStepState state{TlsStepState::Complete};
  std::size_t processed{0};
  int system_error{0};
  std::string message;
};

class TlsClient {
public:
  TlsClient();
  ~TlsClient();
  TlsClient(const TlsClient&) = delete;
  TlsClient& operator=(const TlsClient&) = delete;

  void configure(TlsClientConfig config);
  TlsStep begin_socket(std::intptr_t socket, std::string_view host);
  TlsStep begin_memory(std::string_view host);
  TlsStep handshake();
  TlsStep write(std::span<const std::byte> plaintext);
  TlsStep read(std::span<std::byte> plaintext);
  TlsStep drain_ciphertext(std::span<std::byte> output);
  TlsStep provide_ciphertext(std::span<const std::byte> input);

  bool ciphertext_pending() const noexcept;
  bool active() const noexcept;
  bool peer_identity_verified() const noexcept;
  void reset_session() noexcept;
  void reset_context_if_inactive() noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rs::core::security
