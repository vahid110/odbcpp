#include "odbcpp/security/platform_trust.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#ifndef CERT_CHAIN_DISABLE_AIA
#define CERT_CHAIN_DISABLE_AIA 0x00002000  // documented value; older SDK headers lack it
#endif
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <system_error>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace rs::core::security::platform {
namespace {

std::optional<std::string> validate_chain(
    const std::vector<DerCertificate>& chain) {
  if (chain.empty()) return "peer certificate chain is empty";
  if (chain.size() > max_peer_chain_certificates) {
    return "peer certificate chain is too long";
  }
  for (const auto& certificate : chain) {
    if (certificate.empty() || certificate.size() > max_peer_certificate_bytes) {
      return "peer certificate has an invalid size";
    }
  }
  return std::nullopt;
}

#if defined(__APPLE__)

constexpr std::size_t max_reason_bytes = 256;

// Truncates on a UTF-8 boundary and removes control characters.
std::string bounded(std::string text) {
  if (text.size() > max_reason_bytes) {
    std::size_t end = max_reason_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    text.resize(end);
  }
  for (auto& c : text) {
    if (static_cast<unsigned char>(c) < 0x20) c = ' ';
  }
  return text;
}

template <class T>
class CfOwner {
 public:
  explicit CfOwner(T value = nullptr) noexcept : value_(value) {}
  ~CfOwner() {
    if (value_) ::CFRelease(value_);
  }
  CfOwner(const CfOwner&) = delete;
  CfOwner& operator=(const CfOwner&) = delete;
  T get() const noexcept { return value_; }
  T* out() noexcept { return &value_; }

 private:
  T value_;
};

std::string cf_error_text(CFErrorRef error) {
  if (!error) return "macOS trust evaluation failed";
  CfOwner<CFStringRef> description(::CFErrorCopyDescription(error));
  if (!description.get()) return "macOS trust evaluation failed";
  std::array<char, 2048> buffer{};
  if (!::CFStringGetCString(description.get(), buffer.data(),
                            static_cast<CFIndex>(buffer.size()),
                            kCFStringEncodingUTF8)) {
    return "macOS trust evaluation failed";
  }
  return buffer.data();
}

bool append_certificates(CFMutableArrayRef target,
                         const std::vector<DerCertificate>& certificates) {
  for (const auto& der : certificates) {
    CfOwner<CFDataRef> data(::CFDataCreate(
        kCFAllocatorDefault, der.data(), static_cast<CFIndex>(der.size())));
    if (!data.get()) return false;
    CfOwner<SecCertificateRef> certificate(
        ::SecCertificateCreateWithData(kCFAllocatorDefault, data.get()));
    if (!certificate.get()) return false;
    ::CFArrayAppendValue(target, certificate.get());
  }
  return true;
}

#endif

#if defined(_WIN32)

std::string windows_error(const char* operation, DWORD code) {
  std::array<char, 96> text{};
  std::snprintf(text.data(), text.size(), "%s failed: 0x%08lX", operation,
                static_cast<unsigned long>(code));
  return text.data();
}

#endif

}  // namespace

bool native_verifier_available() noexcept {
#if defined(_WIN32) || defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

ChainEvaluation evaluate_server_chain(const std::vector<DerCertificate>& chain,
                                      const EvaluationOptions& options) {
  if (auto invalid = validate_chain(chain)) return {false, *invalid};
  if (options.test_anchors.size() > max_peer_chain_certificates) {
    return {false, "too many test anchors"};
  }
#if defined(__APPLE__)
  CfOwner<CFMutableArrayRef> certificates(::CFArrayCreateMutable(
      kCFAllocatorDefault, static_cast<CFIndex>(chain.size()),
      &kCFTypeArrayCallBacks));
  if (!certificates.get() || !append_certificates(certificates.get(), chain)) {
    return {false, "peer certificate could not be decoded"};
  }
  // Server-authentication policy without a name: the TLS client performs one
  // uniform DNS/IP host check for every trust source after the handshake.
  CfOwner<SecPolicyRef> policy(::SecPolicyCreateSSL(true, nullptr));
  if (!policy.get()) return {false, "macOS TLS trust policy unavailable"};
  CfOwner<SecTrustRef> trust;
  if (::SecTrustCreateWithCertificates(certificates.get(), policy.get(),
                                       trust.out()) != errSecSuccess ||
      !trust.get()) {
    return {false, "macOS trust object could not be created"};
  }
  // No network fetch: evaluation stays bounded by the connection deadline.
  if (::SecTrustSetNetworkFetchAllowed(trust.get(), false) != errSecSuccess) {
    return {false, "macOS trust network policy could not be set"};
  }
  if (!options.test_anchors.empty()) {
    CfOwner<CFMutableArrayRef> anchors(::CFArrayCreateMutable(
        kCFAllocatorDefault, static_cast<CFIndex>(options.test_anchors.size()),
        &kCFTypeArrayCallBacks));
    if (!anchors.get() ||
        !append_certificates(anchors.get(), options.test_anchors) ||
        ::SecTrustSetAnchorCertificates(trust.get(), anchors.get()) != errSecSuccess ||
        ::SecTrustSetAnchorCertificatesOnly(trust.get(), true) != errSecSuccess) {
      return {false, "test anchors could not be applied"};
    }
  }
  CfOwner<CFErrorRef> error;
  if (!::SecTrustEvaluateWithError(trust.get(), error.out())) {
    return {false, bounded(cf_error_text(error.get()))};
  }
  ChainEvaluation result{true, {}};
  // Keep the existing macOS 10.14 deployment boundary. The replacement copy
  // API requires macOS 12; these deprecated accessors remain necessary here.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const CFIndex count = ::SecTrustGetCertificateCount(trust.get());
  if (count <= 0 || static_cast<std::size_t>(count) > max_peer_chain_certificates) {
    return {false, "macOS verified certificate chain has an invalid size"};
  }
  for (CFIndex i = 0; i < count; ++i) {
    SecCertificateRef certificate = ::SecTrustGetCertificateAtIndex(trust.get(), i);
    if (!certificate) return {false, "macOS verified certificate missing"};
    CfOwner<CFDataRef> data(::SecCertificateCopyData(certificate));
    if (!data.get() || ::CFDataGetLength(data.get()) <= 0 ||
        static_cast<std::size_t>(::CFDataGetLength(data.get())) > max_peer_certificate_bytes) {
      return {false, "macOS verified certificate has an invalid size"};
    }
    const auto* begin = ::CFDataGetBytePtr(data.get());
    result.verified_chain.push_back({
        DerCertificate(begin, begin + ::CFDataGetLength(data.get())), i == count - 1});
  }
#pragma clang diagnostic pop
  return result;
#elif defined(_WIN32)
  if (!options.test_anchors.empty()) {
    return {false, "test anchors are not supported by the Windows verifier"};
  }
  HCERTSTORE peer_store = ::CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                                          CERT_STORE_CREATE_NEW_FLAG, nullptr);
  if (!peer_store) return {false, windows_error("CertOpenStore", ::GetLastError())};
  struct StoreOwner {
    HCERTSTORE store;
    ~StoreOwner() { ::CertCloseStore(store, 0); }
  } store_owner{peer_store};
  PCCERT_CONTEXT leaf = nullptr;
  for (std::size_t i = 0; i < chain.size(); ++i) {
    if (!::CertAddEncodedCertificateToStore(
            peer_store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            chain[i].data(), static_cast<DWORD>(chain[i].size()),
            CERT_STORE_ADD_ALWAYS, i == 0 ? &leaf : nullptr)) {
      if (leaf) ::CertFreeCertificateContext(leaf);
      return {false, "peer certificate could not be decoded"};
    }
  }
  struct LeafOwner {
    PCCERT_CONTEXT context;
    ~LeafOwner() { ::CertFreeCertificateContext(context); }
  } leaf_owner{leaf};

  LPSTR server_usage[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
  CERT_CHAIN_PARA parameters{};
  parameters.cbSize = sizeof(parameters);
  parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
  parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
  parameters.RequestedUsage.Usage.rgpszUsageIdentifier = server_usage;
  // No network retrieval: CryptoAPI bounds URL fetches only per URL, never in
  // aggregate, so a chain build with fetching could outlive the connection
  // deadline. Use peer-supplied and locally cached data only: no AIA issuer
  // download and no third-party root auto-update. Revocation is not requested;
  // see docs/trust-store-design.md section 6.
  const DWORD flags = CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL |
                      CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE |
                      CERT_CHAIN_DISABLE_AIA;
  PCCERT_CHAIN_CONTEXT chain_context = nullptr;
  // HCCE_CURRENT_USER: CurrentUser and LocalMachine roots, enterprise/Group
  // Policy trust and the Disallowed store, as seen by this process's user.
  if (!::CertGetCertificateChain(HCCE_CURRENT_USER, leaf, nullptr, peer_store,
                                 &parameters, flags, nullptr, &chain_context) ||
      !chain_context) {
    return {false, windows_error("CertGetCertificateChain", ::GetLastError())};
  }
  struct ChainOwner {
    PCCERT_CHAIN_CONTEXT context;
    ~ChainOwner() { ::CertFreeCertificateChain(context); }
  } chain_owner{chain_context};
  if (chain_context->TrustStatus.dwErrorStatus != CERT_TRUST_NO_ERROR) {
    return {false, windows_error("Windows certificate chain trust",
                                 chain_context->TrustStatus.dwErrorStatus)};
  }
  SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_policy{};
  ssl_policy.cbStruct = sizeof(ssl_policy);
  ssl_policy.dwAuthType = AUTHTYPE_SERVER;
  CERT_CHAIN_POLICY_PARA policy{};
  policy.cbSize = sizeof(policy);
  policy.pvExtraPolicyPara = &ssl_policy;
  CERT_CHAIN_POLICY_STATUS status{};
  status.cbSize = sizeof(status);
  if (!::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain_context,
                                          &policy, &status)) {
    return {false, windows_error("CertVerifyCertificateChainPolicy", ::GetLastError())};
  }
  if (status.dwError != 0) {
    return {false, windows_error("Windows TLS chain policy", status.dwError)};
  }
  ChainEvaluation result{true, {}};
  // Include every simple chain in the accepted context (including a CTL
  // signer's chain) rather than assuming all issuers arrived from the peer.
  for (DWORD i = 0; i < chain_context->cChain; ++i) {
    const auto* simple = chain_context->rgpChain[i];
    if (!simple) return {false, "Windows verified certificate chain missing"};
    for (DWORD j = 0; j < simple->cElement; ++j) {
      const auto* element = simple->rgpElement[j];
      const auto* certificate = element ? element->pCertContext : nullptr;
      if (!certificate || !certificate->cbCertEncoded ||
          certificate->cbCertEncoded > max_peer_certificate_bytes ||
          result.verified_chain.size() >= max_peer_chain_certificates) {
        return {false, "Windows verified certificate has an invalid size"};
      }
      result.verified_chain.push_back({
          DerCertificate(certificate->pbCertEncoded,
              certificate->pbCertEncoded + certificate->cbCertEncoded),
          j == simple->cElement - 1});
    }
  }
  if (result.verified_chain.empty()) return {false, "Windows verified certificate chain is empty"};
  return result;
#else
  (void)options;
  return {false, "no native operating-system verifier on this platform"};
#endif
}

const std::vector<std::string>& unix_trust_bundle_files() {
  static const std::vector<std::string> files{
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/ssl/ca-bundle.pem",
      "/etc/pki/tls/cacert.pem",
      "/etc/ssl/cert.pem",
      "/usr/local/etc/ssl/cert.pem",
      "/usr/local/share/certs/ca-root-nss.crt",
  };
  return files;
}

const std::vector<std::string>& unix_trust_bundle_directories() {
  static const std::vector<std::string> directories{
      "/etc/ssl/certs",
      "/etc/pki/tls/certs",
  };
  return directories;
}

UnixTrustProbe default_unix_trust_probe() {
  UnixTrustProbe probe;
  probe.is_file = [](const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
  };
  probe.is_directory = [](const std::string& path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error);
  };
  probe.environment = [](const char* name) -> std::optional<std::string> {
#if defined(_WIN32)
    (void)name;
    return std::nullopt;
#else
    // Like OpenSSL's own lookup, ignore overrides in set-ID processes.
#if defined(__GLIBC__)
    const char* value = ::secure_getenv(name);
#else
    if (::getuid() != ::geteuid() || ::getgid() != ::getegid()) return std::nullopt;
    const char* value = std::getenv(name);
#endif
    if (!value || !*value) return std::nullopt;
    return std::string(value);
#endif
  };
  return probe;
}

std::vector<UnixTrustLocation> select_unix_trust_locations(
    const UnixTrustProbe& probe) {
  using Kind = UnixTrustLocation::Kind;
  std::vector<UnixTrustLocation> selected;
  const auto environment_value = [&](const char* name) -> std::optional<std::string> {
    if (!probe.environment) return std::nullopt;
    auto value = probe.environment(name);
    if (!value || value->empty()) return std::nullopt;
    return value;
  };
  if (auto file = environment_value("SSL_CERT_FILE")) {
    selected.push_back({Kind::File, std::move(*file), true});
  }
  if (auto directories = environment_value("SSL_CERT_DIR")) {
    // OpenSSL accepts a colon-separated list; empty entries are ignored. An
    // override without any entry is kept verbatim so it fails to load rather
    // than falling through to distribution bundles.
    const auto before = selected.size();
    std::size_t start = 0;
    while (start <= directories->size()) {
      const auto end = std::min(directories->find(':', start), directories->size());
      if (end > start) {
        selected.push_back({Kind::Directory, directories->substr(start, end - start), true});
      }
      start = end + 1;
    }
    if (selected.size() == before) {
      selected.push_back({Kind::Directory, *directories, true});
    }
  }
  if (!selected.empty()) return selected;
  for (const auto& file : unix_trust_bundle_files()) {
    if (probe.is_file && probe.is_file(file)) return {{Kind::File, file, false}};
  }
  for (const auto& directory : unix_trust_bundle_directories()) {
    if (probe.is_directory && probe.is_directory(directory)) {
      return {{Kind::Directory, directory, false}};
    }
  }
  return {};
}

}  // namespace rs::core::security::platform
