#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Private operating-system trust-store adapter used only by the TLS client.
// Certificates cross this boundary as owned DER bytes; no provider or OS
// handle escapes. See docs/trust-store-design.md.
namespace rs::core::security::platform {

using DerCertificate = std::vector<unsigned char>;

inline constexpr std::size_t max_peer_chain_certificates = 16;
inline constexpr std::size_t max_peer_certificate_bytes = 64 * 1024;

// True when this build evaluates server chains with the operating system's
// verifier: CryptoAPI on Windows, Security.framework on macOS. Elsewhere the
// system trust store is the distribution CA bundle selected below. Native
// evaluation never uses the network: only peer-supplied certificates and
// locally installed or cached OS trust data (no AIA issuer fetch, no root
// auto-update), so a missing intermediate or uninstalled root fails closed.
bool native_verifier_available() noexcept;

struct VerifiedCertificate {
  DerCertificate der;
  // Anchor self-signatures do not establish trust; their public keys still
  // have to meet the TLS strength floor.
  bool trust_anchor{false};
};

struct ChainEvaluation {
  bool trusted{false};
  // Bounded, secret-free reason for a rejection.
  std::string reason;
  // On success, the actual OS-selected trust path, including locally found
  // issuers/anchors. The TLS adapter applies its key/signature floor to these
  // owned certificates rather than only the untrusted peer-supplied list.
  std::vector<VerifiedCertificate> verified_chain{};
};

struct EvaluationOptions {
  // Test-only anchor replacement for proving native chain evaluation without
  // modifying an OS store. Production callers always leave this empty.
  std::vector<DerCertificate> test_anchors;
};

// chain[0] is the server leaf; later entries are untrusted peer-supplied
// intermediates. Evaluates trust and the TLS server-authentication purpose
// under OS policy; host name matching remains the TLS client's own check.
// The call is synchronous and not preemptible; callers enforce their own
// deadline on the returned verdict.
ChainEvaluation evaluate_server_chain(const std::vector<DerCertificate>& chain,
                                      const EvaluationOptions& options);

struct UnixTrustLocation {
  enum class Kind { File, Directory };
  Kind kind{Kind::File};
  std::string path;
  bool from_environment{false};
  bool operator==(const UnixTrustLocation&) const = default;
};

// Injectable file-system/environment view so selection is testable anywhere.
struct UnixTrustProbe {
  std::function<bool(const std::string&)> is_file;
  std::function<bool(const std::string&)> is_directory;
  std::function<std::optional<std::string>(const char*)> environment;
};

const std::vector<std::string>& unix_trust_bundle_files();
const std::vector<std::string>& unix_trust_bundle_directories();
UnixTrustProbe default_unix_trust_probe();

// Non-empty SSL_CERT_FILE/SSL_CERT_DIR are selected verbatim, existing or not,
// so a broken administrator override fails rather than falling through.
// Otherwise the first existing distribution bundle file, else directory.
// Empty means no operating-system trust store was found.
std::vector<UnixTrustLocation> select_unix_trust_locations(
    const UnixTrustProbe& probe);

}  // namespace rs::core::security::platform
