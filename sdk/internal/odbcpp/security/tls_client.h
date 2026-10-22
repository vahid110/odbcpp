#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rs::core::security {

// Trust source when verify_peer is set: ca_file alone, ca_directory alone, or,
// when both are empty, the operating-system trust store (Windows CryptoAPI,
// macOS Security framework, Linux/Unix distribution CA bundles). Custom
// sources never fall back to system trust; both set is rejected.
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
  // Snapshot the already loaded verified trust store. No paths are reread and
  // no provider pointer escapes; the sibling owns a separate immutable context.
  std::unique_ptr<TlsClient> verified_sibling() const;
  bool frozen_context() const noexcept;
  TlsStep begin_socket(std::intptr_t socket, std::string_view host);
  TlsStep begin_memory(std::string_view host);
  // Bounds OS verifier network retrieval for the active session's handshake.
  // Call after begin_*; a new session starts without a deadline.
  void set_verification_deadline(std::chrono::steady_clock::time_point deadline) noexcept;
  // Test-only seam: in operating-system trust mode on macOS, evaluate peers
  // against exactly these DER roots instead of the OS anchors (OS policy and
  // purpose checks still apply). Never called by production code.
  void replace_platform_anchors_for_testing(std::vector<std::vector<unsigned char>> anchors);
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
