#include "odbcpp/security/tls_client.h"

#include "odbcpp/security/platform_trust.h"
#include "odbcpp/util/platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <atomic>
#include <map>
#include <mutex>
#endif

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <signal.h>
#endif

namespace rs::core::security {
namespace {

// One policy for fresh and frozen sibling contexts. Do not rely on a provider
// build's defaults: AWS-LC's OpenSSL security-level functions are no-ops.
bool configure_strength_floor(SSL_CTX* context) {
#if !defined(OPENSSL_IS_AWSLC)
  if (::SSL_CTX_get_security_level(context) < 2) {
    ::SSL_CTX_set_security_level(context, 2);
  }
#endif
  constexpr char tls12[] =
      "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
      "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
      "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";
  constexpr char tls13[] =
      "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
  constexpr char signatures[] =
      "ecdsa_secp256r1_sha256:ecdsa_secp384r1_sha384:ecdsa_secp521r1_sha512:"
      "rsa_pss_rsae_sha256:rsa_pss_rsae_sha384:rsa_pss_rsae_sha512:ed25519:"
      "rsa_pkcs1_sha256:rsa_pkcs1_sha384:rsa_pkcs1_sha512"
#if !defined(OPENSSL_IS_AWSLC)
      ":rsa_pss_pss_sha256:rsa_pss_pss_sha384:rsa_pss_pss_sha512:ed448"
#endif
      ;
  return ::SSL_CTX_set_cipher_list(context, tls12) == 1 &&
      ::SSL_CTX_set_ciphersuites(context, tls13) == 1 &&
      ::SSL_CTX_set1_sigalgs_list(context, signatures) == 1 &&
      ::SSL_CTX_set1_groups_list(context, "X25519:P-256:P-384:P-521") == 1;
}

bool certificate_key_meets_strength_floor(X509* certificate) {
  if (!certificate) return false;
  std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)> key(
      ::X509_get_pubkey(certificate), &::EVP_PKEY_free);
  if (!key) return false;
  const int kind = ::EVP_PKEY_base_id(key.get());
  const int bits = ::EVP_PKEY_bits(key.get());
  bool strong_key = false;
  switch (kind) {
    case EVP_PKEY_RSA:
    case EVP_PKEY_RSA_PSS: strong_key = bits >= 2048; break;
    case EVP_PKEY_EC: strong_key = bits >= 224; break;
    case EVP_PKEY_ED25519:
    case EVP_PKEY_ED448: strong_key = true; break;
    default: break;  // No strength claim for unknown certificate key types.
  }
  return strong_key;
}

int strong_digest_nid(const X509_ALGOR* algorithm) {
  if (!algorithm) return NID_undef;  // PSS omission defaults to weak SHA-1.
  const ASN1_OBJECT* object = nullptr;
  int parameter_type = V_ASN1_UNDEF;
  ::X509_ALGOR_get0(&object, &parameter_type, nullptr, algorithm);
  if (!object || (parameter_type != V_ASN1_UNDEF && parameter_type != V_ASN1_NULL)) {
    return NID_undef;
  }
  const int nid = ::OBJ_obj2nid(object);
  switch (nid) {
    case NID_sha224:
    case NID_sha256:
    case NID_sha384:
    case NID_sha512: return nid;
    default: return NID_undef;
  }
}

const ASN1_STRING* sequence_parameters(const X509_ALGOR* algorithm, int expected_nid) {
  if (!algorithm) return nullptr;
  const ASN1_OBJECT* object = nullptr;
  const void* parameters = nullptr;
  int type = V_ASN1_UNDEF;
  ::X509_ALGOR_get0(&object, &type, &parameters, algorithm);
  if (!object || ::OBJ_obj2nid(object) != expected_nid ||
      type != V_ASN1_SEQUENCE || !parameters) return nullptr;
  return static_cast<const ASN1_STRING*>(parameters);
}

bool pss_signature_meets_strength_floor(const X509_ALGOR* algorithm) {
  const auto* encoded = sequence_parameters(algorithm, NID_rsassaPss);
  const int length = encoded ? ::ASN1_STRING_length(encoded) : 0;
  if (length <= 0 || length > 1024) return false;
  const auto* begin = ::ASN1_STRING_get0_data(encoded);
  if (!begin) return false;
  const auto* cursor = begin;
  std::unique_ptr<RSA_PSS_PARAMS, decltype(&::RSA_PSS_PARAMS_free)> parameters(
      ::d2i_RSA_PSS_PARAMS(nullptr, &cursor, length), &::RSA_PSS_PARAMS_free);
  if (!parameters || cursor != begin + length) return false;
  const int digest = strong_digest_nid(parameters->hashAlgorithm);
  if (digest == NID_undef) return false;
  const auto* mgf = sequence_parameters(parameters->maskGenAlgorithm, NID_mgf1);
  const int mgf_length = mgf ? ::ASN1_STRING_length(mgf) : 0;
  if (mgf_length <= 0 || mgf_length > 128) return false;
  const auto* mgf_begin = ::ASN1_STRING_get0_data(mgf);
  if (!mgf_begin) return false;
  const auto* mgf_cursor = mgf_begin;
  std::unique_ptr<X509_ALGOR, decltype(&::X509_ALGOR_free)> mgf_digest(
      ::d2i_X509_ALGOR(nullptr, &mgf_cursor, mgf_length), &::X509_ALGOR_free);
  if (!mgf_digest || mgf_cursor != mgf_begin + mgf_length ||
      strong_digest_nid(mgf_digest.get()) != digest) return false;
  // RFC4055 defaults are saltLength20/trailerField1. Salt length is not a
  // symmetric key length: provider/native signature verification also proves
  // the declared nonnegative salt fits the actual issuer RSA modulus.
  std::int64_t salt_length = 20;
  std::int64_t trailer = 1;
  if ((parameters->saltLength &&
       ::ASN1_INTEGER_get_int64(&salt_length, parameters->saltLength) != 1) ||
      (parameters->trailerField &&
       ::ASN1_INTEGER_get_int64(&trailer, parameters->trailerField) != 1)) return false;
  return salt_length >= 0 && salt_length <= std::numeric_limits<int>::max() && trailer == 1;
}

bool certificate_meets_strength_floor(X509* certificate, bool trust_anchor) {
  if (!certificate_key_meets_strength_floor(certificate)) return false;
  if (trust_anchor) return true;
  const X509_ALGOR* signature = nullptr;
  ::X509_get0_signature(nullptr, &signature, certificate);
  if (!signature) return false;
  const ASN1_OBJECT* object = nullptr;
  int parameter_type = V_ASN1_UNDEF;
  ::X509_ALGOR_get0(&object, &parameter_type, nullptr, signature);
  const int nid = object ? ::OBJ_obj2nid(object) : NID_undef;
  // AWS-LC reports success with sec_bits=-1 for these algorithms. Their
  // intrinsic/parameterized strength must be established explicitly; a PSS OID
  // alone says nothing about its digest. Cryptographic verification already
  // succeeded before this supplemental policy check on either trust path.
  if (nid == NID_ED25519 || nid == NID_ED448) return parameter_type == V_ASN1_UNDEF;
  if (nid == NID_rsassaPss) return pss_signature_meets_strength_floor(signature);
  int security_bits = 0;
  return ::X509_get_signature_info(
      certificate, nullptr, nullptr, &security_bits, nullptr) == 1 && security_bits >= 112;
}

bool verified_chain_meets_strength_floor(X509_STORE_CTX* context) {
  auto* chain = ::X509_STORE_CTX_get0_chain(context);
  const auto count = chain ? sk_X509_num(chain) : 0;
  if (count <= 0 || static_cast<std::size_t>(count) > platform::max_peer_chain_certificates) return false;
  for (decltype(sk_X509_num(chain)) i = 0; i < count; ++i) {
    if (!certificate_meets_strength_floor(sk_X509_value(chain, i), i == count - 1)) return false;
  }
  return true;
}

int current_socket_error() noexcept {
#if defined(_WIN32)
  return ::WSAGetLastError();
#else
  return errno;
#endif
}

class ScopedSigpipeBlock {
public:
  ScopedSigpipeBlock() noexcept {
#if !defined(_WIN32)
    sigemptyset(&blocked_);
    sigaddset(&blocked_, SIGPIPE);
    active_ = pthread_sigmask(SIG_BLOCK, &blocked_, &previous_) == 0;
    if (active_) {
      sigset_t pending{};
      pending_before_ = sigpending(&pending) == 0 &&
          sigismember(&pending, SIGPIPE) == 1;
    }
#endif
  }

  ~ScopedSigpipeBlock() noexcept {
#if !defined(_WIN32)
    if (!active_) return;
    sigset_t pending{};
    const bool pending_after = sigpending(&pending) == 0 &&
        sigismember(&pending, SIGPIPE) == 1;
    if (!pending_before_ && pending_after) {
      int received = 0;
      (void)sigwait(&blocked_, &received);
    }
    (void)pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
#endif
  }

private:
#if !defined(_WIN32)
  sigset_t blocked_{};
  sigset_t previous_{};
  bool active_{false};
  bool pending_before_{false};
#endif
};

std::string error_text(std::string operation) {
  const unsigned long error = ::ERR_get_error();
  if (error != 0) {
    std::array<char, 256> detail{};
    ::ERR_error_string_n(error, detail.data(), detail.size());
    operation += ": ";
    operation += detail.data();
  }
  while (::ERR_get_error() != 0) {}
  return operation;
}

bool host_uses_sni(std::string_view host) {
  if (host.empty() || host.find('\0') != std::string_view::npos) return false;
  const std::string name(host);
  ASN1_OCTET_STRING* address = ::a2i_IPADDRESS(name.c_str());
  if (!address) return true;
  ::ASN1_OCTET_STRING_free(address);
  return false;
}

constexpr std::uintmax_t max_ca_file_bytes = 16 * 1024 * 1024;

std::filesystem::path utf8_path(const std::string& path) {
  // Connection inputs are UTF-8; Windows needs the wide spelling, not ANSI.
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(path.data()), path.size()));
}

// Loads every PEM certificate/CRL (including TRUSTED CERTIFICATE blocks) from
// one file into a per-context store. Returns an empty string or the reason.
// The provider opens the file so its UTF-8 path handling and error detail are
// preserved; an existing non-regular path is rejected before opening.
extern "C" int odbcpp_trust_no_password(char*, int, int, void*) noexcept {
  return 0;
}

// AWS-LC exposes the X509_LU_* values without OpenSSL's lookup-type typedef.
// This private parser only discriminates those values; callbacks retain their
// provider-declared signatures.
std::string load_pem_file(X509_STORE* store, const std::string& path,
                          int type = X509_LU_NONE,
                          std::uintmax_t* remaining_bytes = nullptr) {
  std::error_code error;
  std::filesystem::file_status status;
  try {
    status = std::filesystem::status(utf8_path(path), error);
  } catch (const std::exception&) {
    return "invalid path encoding";
  }
  if (std::filesystem::exists(status) && !std::filesystem::is_regular_file(status)) {
    return "not a regular file";
  }
  ::ERR_clear_error();
  BIO* file = ::BIO_new_file(path.c_str(), "rb");
  if (!file) return error_text("cannot open file");
  std::string contents;
  std::array<char, 16384> chunk{};
  for (;;) {
    const int count = ::BIO_read(file, chunk.data(), static_cast<int>(chunk.size()));
    if (count == 0) break;
    if (count < 0) {
      ::BIO_free(file);
      return error_text("cannot read file");
    }
    if (remaining_bytes) {
      if (static_cast<std::uintmax_t>(count) > *remaining_bytes) {
        ::BIO_free(file);
        return "directory exceeds 16 MiB";
      }
      *remaining_bytes -= static_cast<std::uintmax_t>(count);
    }
    contents.append(chunk.data(), static_cast<std::size_t>(count));
    if (contents.size() > max_ca_file_bytes) {
      ::BIO_free(file);
      return "file exceeds 16 MiB";
    }
  }
  ::BIO_free(file);
  if (contents.empty()) return "file is empty";
  ::ERR_clear_error();
  BIO* bio = ::BIO_new_mem_buf(contents.data(), static_cast<int>(contents.size()));
  if (!bio) return error_text("BIO_new_mem_buf");
  // A hashed certificate/CRL entry loads only that object type, as by_dir does.
  // In particular, unrelated private-key blocks never add trust or cause an
  // interactive password request. AUX preserves TRUSTED CERTIFICATE metadata.
  if (type != X509_LU_NONE) {
    std::size_t loaded = 0;
    std::string reason;
    for (;;) {
      if (type == X509_LU_X509) {
        auto* certificate = ::PEM_read_bio_X509_AUX(bio, nullptr, &odbcpp_trust_no_password, nullptr);
        if (!certificate) {
          if (loaded == 0 || ERR_GET_REASON(::ERR_peek_last_error()) != PEM_R_NO_START_LINE) {
            reason = error_text("malformed PEM certificate");
          }
          break;
        }
        const int added = ::X509_STORE_add_cert(store, certificate);
        ::X509_free(certificate);
        if (added != 1) {
          reason = error_text("X509_STORE_add_cert");
          break;
        }
      } else {
        auto* crl = ::PEM_read_bio_X509_CRL(bio, nullptr, &odbcpp_trust_no_password, nullptr);
        if (!crl) {
          if (loaded == 0 || ERR_GET_REASON(::ERR_peek_last_error()) != PEM_R_NO_START_LINE) {
            reason = error_text("malformed PEM CRL");
          }
          break;
        }
        const int added = ::X509_STORE_add_crl(store, crl);
        ::X509_CRL_free(crl);
        if (added != 1) {
          reason = error_text("X509_STORE_add_crl");
          break;
        }
      }
      ++loaded;
    }
    ::BIO_free(bio);
    ::ERR_clear_error();
    if (!reason.empty()) return reason;
    return loaded > 0 ? std::string{} : "no matching PEM objects found";
  }
  STACK_OF(X509_INFO)* entries = ::PEM_X509_INFO_read_bio(bio, nullptr, nullptr, nullptr);
  ::BIO_free(bio);
  if (!entries) return error_text("malformed PEM data");
  std::size_t certificates = 0;
  std::string failure;
  for (decltype(sk_X509_INFO_num(entries)) i = 0; i < sk_X509_INFO_num(entries); ++i) {
    const auto* entry = sk_X509_INFO_value(entries, i);
    if (entry->x509) {
      if (::X509_STORE_add_cert(store, entry->x509) != 1) {
        failure = error_text("X509_STORE_add_cert");
        break;
      }
      ++certificates;
    }
    if (entry->crl && ::X509_STORE_add_crl(store, entry->crl) != 1) {
      failure = error_text("X509_STORE_add_crl");
      break;
    }
  }
  sk_X509_INFO_pop_free(entries, ::X509_INFO_free);
  ::ERR_clear_error();
  if (!failure.empty()) return failure;
  if (certificates == 0) return "no PEM certificates found";
  return {};
}

std::string check_directory(const std::string& path) {
  std::error_code error;
  try {
    if (!std::filesystem::is_directory(utf8_path(path), error)) {
      return "not an existing directory";
    }
  } catch (const std::exception&) {
    return "invalid path encoding";
  }
  return {};
}

#if defined(_WIN32)
extern "C" int odbcpp_windows_hash_directory_subject(
    X509_LOOKUP* lookup, X509_LOOKUP_TYPE type, const X509_NAME* name,
    X509_OBJECT* result) noexcept;

// OpenSSL3 by_dir.c uses narrow _stat before its UTF-8-aware BIO reader. Own
// only the per-context directory lookup: no provider/default-store replacement,
// directory enumeration or eagerly imported trust. Non-Windows keeps by_dir.
class WindowsHashDirectoryLookup {
public:
  explicit WindowsHashDirectoryLookup(const std::string& path)
      : directory_(utf8_path(path)) {}

  bool attach(X509_STORE* store) {
    method_.reset(::X509_LOOKUP_meth_new("ODBCPP UTF-8 hashed CA directory"));
    if (!method_ || ::X509_LOOKUP_meth_set_get_by_subject(
                        method_.get(), &odbcpp_windows_hash_directory_subject) != 1) {
      return false;
    }
    auto* lookup = ::X509_STORE_add_lookup(store, method_.get());
    return lookup && ::X509_LOOKUP_set_method_data(lookup, this) == 1;
  }

  bool failed() const noexcept { return failed_.load(); }
  void fail() noexcept { failed_.store(true); }

  int find(X509_LOOKUP* lookup, X509_LOOKUP_TYPE type, const X509_NAME* name,
           X509_OBJECT* result) {
    if (!name || !result || (type != X509_LU_X509 && type != X509_LU_CRL)) return 0;
    std::lock_guard<std::mutex> guard(mutex_);
    if (failed()) return 0;
    int hash_ok = 0;
    const auto hash = ::X509_NAME_hash_ex(name, nullptr, nullptr, &hash_ok);
    if (!hash_ok) return 0;
    auto* store = ::X509_LOOKUP_get_store(lookup);
    if (!store) return 0;
    std::size_t suffix = 0;
    if (type == X509_LU_CRL) {
      const auto previous = crl_suffixes_.find(hash);
      if (previous != crl_suffixes_.end()) suffix = previous->second;
    }
    std::uintmax_t remaining_bytes = max_ca_file_bytes;
    for (; suffix < max_hash_entries; ++suffix) {
      const auto file = entry_path(hash, type, suffix);
      std::error_code error;
      // std::filesystem takes the native wide path on Windows. A gap, malformed
      // file or non-file terminates this hash's sequence, as with by_dir.
      if (!std::filesystem::is_regular_file(file, error)) break;
      const auto spelling = file.u8string();
      const std::string path(reinterpret_cast<const char*>(spelling.data()), spelling.size());
      const auto reason = load_pem_file(store, path, type, &remaining_bytes);
      if (reason == "directory exceeds 16 MiB") {
        fail();
        return 0;
      }
      if (!reason.empty()) break;
    }
    if (suffix == max_hash_entries) {
      std::error_code error;
      if (std::filesystem::exists(entry_path(hash, type, suffix), error)) {
        fail();
        return 0;
      }
    }
    if (type == X509_LU_CRL && suffix > 0) crl_suffixes_[hash] = suffix;
    if (::X509_STORE_lock(store) != 1) return 0;
    auto* objects = ::X509_STORE_get0_objects(store);
    sk_X509_OBJECT_sort(objects);
    auto* cached = ::X509_OBJECT_retrieve_by_subject(objects, type, name);
    int found = 0;
    if (cached && type == X509_LU_X509) {
      auto* certificate = ::X509_OBJECT_get0_X509(cached);
      found = ::X509_OBJECT_set1_X509(result, certificate);
      // Lookup returns a borrowed cache object; STORE_CTX adds its own ref.
      // Balance set1's ref, like OpenSSL's public-API by_store callback.
      if (found) ::X509_free(certificate);
    } else if (cached && type == X509_LU_CRL) {
      auto* crl = ::X509_OBJECT_get0_X509_CRL(cached);
      found = ::X509_OBJECT_set1_X509_CRL(result, crl);
      if (found) ::X509_CRL_free(crl);
    }
    ::X509_STORE_unlock(store);
    if (found) ::ERR_clear_error();
    return found;
  }

private:
  std::filesystem::path entry_path(unsigned long hash, X509_LOOKUP_TYPE type,
                                    std::size_t suffix) const {
    std::array<char, 32> name{};
    std::snprintf(name.data(), name.size(), "%08lx.%s%zu", hash,
                  type == X509_LU_CRL ? "r" : "", suffix);
    return directory_ / name.data();
  }

  static constexpr std::size_t max_hash_entries = 256;
  std::filesystem::path directory_;
  std::unique_ptr<X509_LOOKUP_METHOD, decltype(&::X509_LOOKUP_meth_free)>
      method_{nullptr, &::X509_LOOKUP_meth_free};
  std::mutex mutex_;
  std::map<unsigned long, std::size_t> crl_suffixes_;
  std::atomic<bool> failed_{false};
};

extern "C" int odbcpp_windows_hash_directory_subject(
    X509_LOOKUP* lookup, X509_LOOKUP_TYPE type, const X509_NAME* name,
    X509_OBJECT* result) noexcept {
  auto* directory = static_cast<WindowsHashDirectoryLookup*>(
      ::X509_LOOKUP_get_method_data(lookup));
  if (!directory) return 0;
  try {
    return directory->find(lookup, type, name, result);
  } catch (...) {
    // No C++ exception crosses OpenSSL's C frames, including path conversion,
    // allocation and locking failures. Cached partial loads cannot bypass this.
    directory->fail();
    ERR_raise(ERR_LIB_X509, ERR_R_INTERNAL_ERROR);
    return 0;
  }
}
#endif

bool certificate_matches_host(X509* certificate, std::string_view host) {
  if (host.find('\0') != std::string_view::npos) return false;
  const std::string name(host);
  const int ip_match = ::X509_check_ip_asc(certificate, name.c_str(), 0);
  if (ip_match != -2) return ip_match == 1;
  return ::X509_check_host(certificate, name.c_str(), name.size(), 0,
                           nullptr) == 1;
}

}  // namespace

class TlsClient::Impl {
public:
  ~Impl() {
    reset_session();
    if (context_) ::SSL_CTX_free(context_);
  }

  void configure(TlsClientConfig config) {
    if (frozen_) throw std::logic_error("Frozen TLS context cannot be configured");
    config_ = std::move(config);
    reset_context_if_inactive();
  }

  TlsStep begin_socket(std::intptr_t socket, std::string_view host) {
    auto started = begin(host);
    if (started.state != TlsStepState::Complete) return started;
    if (::SSL_set_fd(session_, static_cast<int>(socket)) != 1) {
      auto failed = failure("SSL_set_fd");
      reset_session();
      return failed;
    }
#ifdef SSL_MODE_AUTO_RETRY
    ::SSL_set_mode(session_, SSL_MODE_AUTO_RETRY);
#endif
    return {};
  }

  TlsStep begin_memory(std::string_view host) {
    auto started = begin(host);
    if (started.state != TlsStepState::Complete) return started;
    BIO* read_bio = ::BIO_new(::BIO_s_mem());
    BIO* write_bio = ::BIO_new(::BIO_s_mem());
    if (!read_bio || !write_bio) {
      if (read_bio) ::BIO_free(read_bio);
      if (write_bio) ::BIO_free(write_bio);
      auto failed = failure("BIO_new");
      reset_session();
      return failed;
    }
    ::SSL_set_bio(session_, read_bio, write_bio);
    read_bio_ = read_bio;
    write_bio_ = write_bio;
    return {};
  }

  TlsStep handshake() {
    if (!session_) return tls_error("TLS session is not active");
    ::ERR_clear_error();
    int result = 0;
    {
      ScopedSigpipeBlock block;
      result = ::SSL_connect(session_);
    }
#if defined(_WIN32)
    if (directory_lookup_ && directory_lookup_->failed()) {
      return tls_error("TLS CA directory lookup failed or exceeded safety limits");
    }
#endif
    auto status = classify(result, "SSL_connect");
    if (status.state == TlsStepState::TlsError && !platform_failure_.empty()) {
      status.message += " (operating system trust store: " + platform_failure_ + ")";
    }
    if (status.state != TlsStepState::Complete) return status;
    auto verified = verify_peer();
    if (verified.state != TlsStepState::Complete) {
      reset_session();
      return verified;
    }
    handshake_complete_ = true;
    return verified;
  }

  TlsStep write(std::span<const std::byte> plaintext) {
    if (!session_) return tls_error("TLS session is not active");
    if (!handshake_complete_) return tls_error("TLS handshake is not complete");
    if (plaintext.empty()) return {};
    ::ERR_clear_error();
    std::size_t written = 0;
    int result = 0;
    {
      ScopedSigpipeBlock block;
      const auto size = std::min(
          plaintext.size(),
          static_cast<std::size_t>(std::numeric_limits<int>::max()));
      result = ::SSL_write_ex(session_, plaintext.data(), size,
                              &written);
    }
    auto status = classify(result, "SSL_write_ex");
    status.processed = written;
    return status;
  }

  TlsStep read(std::span<std::byte> plaintext) {
    if (!session_) return tls_error("TLS session is not active");
    if (!handshake_complete_) return tls_error("TLS handshake is not complete");
    if (plaintext.empty()) return {};
    ::ERR_clear_error();
    std::size_t received = 0;
    int result = 0;
    {
      ScopedSigpipeBlock block;
      const auto size = std::min(
          plaintext.size(),
          static_cast<std::size_t>(std::numeric_limits<int>::max()));
      result = ::SSL_read_ex(session_, plaintext.data(), size,
                             &received);
    }
    auto status = classify(result, "SSL_read_ex");
    status.processed = received;
    return status;
  }

  TlsStep drain_ciphertext(std::span<std::byte> output) {
    if (!write_bio_) return tls_error("TLS memory output is not active");
    if (output.empty() || ::BIO_ctrl_pending(write_bio_) == 0) return {};
    const auto size = std::min(
        output.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max()));
    const int count = ::BIO_read(write_bio_, output.data(),
                                 static_cast<int>(size));
    if (count <= 0) return failure("BIO_read");
    return {TlsStepState::Complete, static_cast<std::size_t>(count), 0, {}};
  }

  TlsStep provide_ciphertext(std::span<const std::byte> input) {
    if (!read_bio_) return tls_error("TLS memory input is not active");
    if (input.empty()) return {};
    const auto size = std::min(
        input.size(),
        static_cast<std::size_t>(std::numeric_limits<int>::max()));
    const int count = ::BIO_write(read_bio_, input.data(),
                                  static_cast<int>(size));
    if (count <= 0) return failure("BIO_write");
    return {TlsStepState::Complete, static_cast<std::size_t>(count), 0, {}};
  }

  bool ciphertext_pending() const noexcept {
    return write_bio_ && ::BIO_ctrl_pending(write_bio_) > 0;
  }

  std::unique_ptr<Impl> verified_sibling() const {
    if (!session_ || !peer_verified_ || !context_ || !config_.verify_peer || !config_.verify_hostname) return nullptr;
    // Context configuration is captured when created, not from later setters.
    auto sibling = std::make_unique<Impl>();
    sibling->config_ = context_config_;
    if (!sibling->config_.verify_peer || !sibling->config_.verify_hostname) return nullptr;
    sibling->context_ = ::SSL_CTX_new(::TLS_client_method());
    if (!sibling->context_) return nullptr;
    if (!configure_strength_floor(sibling->context_)) return nullptr;
#if !defined(OPENSSL_IS_AWSLC)
    const int source_level = ::SSL_CTX_get_security_level(context_);
    if (::SSL_CTX_get_security_level(sibling->context_) < source_level) {
      ::SSL_CTX_set_security_level(sibling->context_, source_level);
    }
#endif
    if (::SSL_CTX_set_min_proto_version(sibling->context_, static_cast<int>(context_config_.minimum_version)) != 1) return nullptr;
#ifdef TLS1_3_VERSION
    if (::SSL_CTX_set_max_proto_version(sibling->context_, TLS1_3_VERSION) != 1) return nullptr;
#endif
    ::SSL_CTX_set_verify(sibling->context_, SSL_VERIFY_PEER, nullptr);
    ::SSL_CTX_set_cert_verify_callback(sibling->context_, &Impl::verify_chain, sibling.get());
    auto* source = ::SSL_CTX_get_cert_store(context_);
    auto* target = ::SSL_CTX_get_cert_store(sibling->context_);
    if (!source || !target || ::X509_VERIFY_PARAM_set1(::X509_STORE_get0_param(target), ::X509_STORE_get0_param(source)) != 1) return nullptr;
    const auto* objects = ::X509_STORE_get0_objects(source);
    for (decltype(sk_X509_OBJECT_num(objects)) i = 0; objects && i < sk_X509_OBJECT_num(objects); ++i) {
      auto* object = sk_X509_OBJECT_value(objects, i);
      const auto type = ::X509_OBJECT_get_type(object);
      if (type == X509_LU_X509) {
        if (::X509_STORE_add_cert(target, ::X509_OBJECT_get0_X509(object)) != 1) return nullptr;
      } else if (type == X509_LU_CRL) {
        if (::X509_STORE_add_crl(target, ::X509_OBJECT_get0_X509_CRL(object)) != 1) return nullptr;
      }
    }
    // No lookup methods/path names are copied: hashed CA directories cannot be
    // reopened by this sibling after the original verification established trust.
    sibling->config_.ca_file.clear(); sibling->config_.ca_directory.clear();
    sibling->context_config_ = sibling->config_;
    // Native OS trust is evaluated live; it cannot be snapshotted into a store.
    sibling->platform_test_anchors_ = platform_test_anchors_;
    if (platform_verification_) sibling->enable_platform_verification();
    sibling->frozen_ = true;
    return sibling;
  }
  bool frozen_context() const noexcept { return frozen_; }
  bool active() const noexcept { return session_ != nullptr; }
  bool peer_identity_verified() const noexcept { return peer_verified_; }

  void set_verification_deadline(std::chrono::steady_clock::time_point deadline) noexcept {
    verification_deadline_ = deadline;
  }

  void replace_platform_anchors_for_testing(std::vector<std::vector<unsigned char>> anchors) {
    platform_test_anchors_ = std::move(anchors);
  }

  void reset_session() noexcept {
    peer_verified_ = false;
    handshake_complete_ = false;
    platform_failure_.clear();
    if (session_) ::SSL_free(session_);
    session_ = nullptr;
    read_bio_ = nullptr;
    write_bio_ = nullptr;
    server_name_.clear();
  }

  void reset_context_if_inactive() noexcept {
    if (frozen_ || session_ || !context_) return;
    ::SSL_CTX_free(context_);
    context_ = nullptr;
#if defined(_WIN32)
    directory_lookup_.reset();
#endif
  }

private:
  TlsStep begin(std::string_view host) {
    if (host.find('\0') != std::string_view::npos) {
      return tls_error("TLS host contains an embedded NUL byte");
    }
    reset_session();
    verification_deadline_ = std::chrono::steady_clock::time_point::max();
    auto context = ensure_context();
    if (context.state != TlsStepState::Complete) return context;
    session_ = ::SSL_new(context_);
    if (!session_) return failure("SSL_new");
    ::SSL_set_connect_state(session_);
    server_name_ = std::string(host);
    if (host_uses_sni(server_name_) &&
        ::SSL_set_tlsext_host_name(session_, server_name_.c_str()) != 1) {
      auto failed = failure("set TLS server name");
      reset_session();
      return failed;
    }
    return {};
  }

  TlsStep ensure_context() {
    if (context_) {
#if defined(_WIN32)
      if (directory_lookup_ && directory_lookup_->failed()) {
        return tls_error("TLS CA directory lookup failed or exceeded safety limits");
      }
#endif
      return {};
    }
    if (config_.minimum_version < 0x0303) {
      return tls_error("Minimum TLS version must be TLS 1.2 or later");
    }
    context_config_ = config_;
    platform_verification_ = false;
    context_ = ::SSL_CTX_new(::TLS_client_method());
    if (!context_) return failure("SSL_CTX_new");
    const auto fail_context = [this](std::string operation) {
      auto failed = failure(std::move(operation));
      ::SSL_CTX_free(context_);
      context_ = nullptr;
#if defined(_WIN32)
      directory_lookup_.reset();
#endif
      return failed;
    };
    if (!configure_strength_floor(context_)) {
      return fail_context("Failed to configure TLS strength policy");
    }
    if (::SSL_CTX_set_min_proto_version(
            context_, static_cast<int>(config_.minimum_version)) != 1) {
      return fail_context("Failed to set minimum TLS version");
    }
#ifdef TLS1_3_VERSION
    if (::SSL_CTX_set_max_proto_version(context_, TLS1_3_VERSION) != 1) {
      return fail_context("SSL_CTX_set_max_proto_version");
    }
#endif
    ::SSL_CTX_set_verify(
        context_, config_.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
        nullptr);
    ::SSL_CTX_set_cert_verify_callback(context_, &Impl::verify_chain, this);
    if (!config_.verify_peer) return {};
    const auto fail_trust = [this](std::string message) {
      ::SSL_CTX_free(context_);
      context_ = nullptr;
#if defined(_WIN32)
      directory_lookup_.reset();
#endif
      return tls_error(std::move(message));
    };
    if (!config_.ca_file.empty() && !config_.ca_directory.empty()) {
      return fail_trust("TLS CA file and directory cannot both be specified");
    }
    X509_STORE* store = ::SSL_CTX_get_cert_store(context_);
    if (!store) return fail_context("SSL_CTX_get_cert_store");
    // Custom sources replace system trust; failure never falls back.
    if (!config_.ca_file.empty()) {
      auto reason = load_pem_file(store, config_.ca_file);
      if (!reason.empty()) {
        return fail_trust("Failed to load CA file: " + config_.ca_file + ": " + reason);
      }
      return {};
    }
    if (!config_.ca_directory.empty()) {
      auto reason = check_directory(config_.ca_directory);
#if defined(_WIN32)
      if (reason.empty()) {
        try {
          directory_lookup_ = std::make_unique<WindowsHashDirectoryLookup>(config_.ca_directory);
          if (!directory_lookup_->attach(store)) reason = error_text("lookup setup failed");
        } catch (...) {
          reason = "lookup setup failed";
        }
      }
#else
      if (reason.empty() &&
          ::SSL_CTX_load_verify_locations(context_, nullptr, config_.ca_directory.c_str()) != 1) {
        reason = error_text("lookup setup failed");
      }
#endif
      if (!reason.empty()) {
        return fail_trust("Failed to load CA directory: " + config_.ca_directory + ": " + reason);
      }
      return {};
    }
    return load_system_trust(fail_trust);
  }

  template <class Fail>
  TlsStep load_system_trust(const Fail& fail_trust) {
    if (platform::native_verifier_available()) {
      enable_platform_verification();
      return {};
    }
    const auto locations =
        platform::select_unix_trust_locations(platform::default_unix_trust_probe());
    if (locations.empty()) {
      return fail_trust("No operating system trust store found");
    }
    X509_STORE* store = ::SSL_CTX_get_cert_store(context_);
    for (const auto& location : locations) {
      std::string reason;
      if (location.kind == platform::UnixTrustLocation::Kind::File) {
        reason = load_pem_file(store, location.path);
      } else {
        reason = check_directory(location.path);
        if (reason.empty() &&
            ::SSL_CTX_load_verify_locations(context_, nullptr, location.path.c_str()) != 1) {
          reason = error_text("lookup setup failed");
        }
      }
      if (!reason.empty()) {
        return fail_trust("Failed to load operating system trust store " +
                          location.path + ": " + reason);
      }
    }
    return {};
  }

  void enable_platform_verification() noexcept {
    platform_verification_ = true;
    ::SSL_CTX_set_cert_verify_callback(context_, &Impl::verify_chain, this);
  }

  // Keep provider path building for custom/Unix stores; only native trust
  // replaces it with the OS verifier. Both routes enforce the same floor.
  // No exception may cross the provider's C frames.
  static int verify_chain(X509_STORE_CTX* store_context, void* argument) noexcept {
    auto* self = static_cast<Impl*>(argument);
    try {
      if (self->platform_verification_) return platform_verify_chain(store_context, self);
      if (::X509_verify_cert(store_context) != 1) return 0;
      if (!verified_chain_meets_strength_floor(store_context)) {
        ::X509_STORE_CTX_set_error(store_context, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
      }
      return 1;
    } catch (...) {
      self->platform_failure_.clear();
      ::X509_STORE_CTX_set_error(store_context, X509_V_ERR_CERT_UNTRUSTED);
      return 0;
    }
  }

  static int platform_verify_chain(X509_STORE_CTX* store_context, Impl* self) {
    const auto reject = [&](std::string reason) {
      self->platform_failure_ = std::move(reason);
      ::X509_STORE_CTX_set_error(store_context, X509_V_ERR_CERT_UNTRUSTED);
      return 0;
    };
    X509* leaf = ::X509_STORE_CTX_get0_cert(store_context);
    if (!leaf) return reject("peer certificate missing");
    std::vector<platform::DerCertificate> chain;
    const auto append = [&](X509* certificate) {
      const int size = ::i2d_X509(certificate, nullptr);
      if (size <= 0 || static_cast<std::size_t>(size) > platform::max_peer_certificate_bytes) {
        return false;
      }
      platform::DerCertificate der(static_cast<std::size_t>(size));
      unsigned char* cursor = der.data();
      if (::i2d_X509(certificate, &cursor) != size) return false;
      chain.push_back(std::move(der));
      return true;
    };
    if (!append(leaf)) return reject("peer certificate could not be encoded");
    auto* untrusted = ::X509_STORE_CTX_get0_untrusted(store_context);
    for (decltype(sk_X509_num(untrusted)) i = 0; untrusted && i < sk_X509_num(untrusted); ++i) {
      X509* certificate = sk_X509_value(untrusted, i);
      if (::X509_cmp(certificate, leaf) == 0) continue;
      if (chain.size() >= platform::max_peer_chain_certificates) {
        return reject("peer certificate chain is too long");
      }
      if (!append(certificate)) return reject("peer certificate could not be encoded");
    }
    platform::EvaluationOptions options;
    options.test_anchors = self->platform_test_anchors_;
    auto evaluation = platform::evaluate_server_chain(chain, options);
    if (!evaluation.trusted) {
      return reject(evaluation.reason.empty() ? "certificate is not trusted"
                                              : std::move(evaluation.reason));
    }
    if (evaluation.verified_chain.empty()) return reject("verified certificate chain missing");
    for (const auto& verified : evaluation.verified_chain) {
      const unsigned char* cursor = verified.der.data();
      std::unique_ptr<X509, decltype(&::X509_free)> certificate(
          ::d2i_X509(nullptr, &cursor, static_cast<long>(verified.der.size())), &::X509_free);
      if (!certificate || cursor != verified.der.data() + verified.der.size() ||
          !certificate_meets_strength_floor(certificate.get(), verified.trust_anchor)) {
        return reject("verified certificate does not meet TLS strength policy");
      }
    }
    // A verdict that arrives after the caller's original deadline never
    // publishes trust; the transport reports the expired deadline.
    if (std::chrono::steady_clock::now() >= self->verification_deadline_) {
      return reject("verification completed after the connection deadline");
    }
    ::X509_STORE_CTX_set_error(store_context, X509_V_OK);
    return 1;
  }

  TlsStep verify_peer() {
    const SSL_CIPHER* cipher = ::SSL_get_current_cipher(session_);
    if (!cipher || ::SSL_CIPHER_get_bits(cipher, nullptr) < 112) {
      return tls_error("Negotiated cipher does not meet TLS strength policy");
    }
    EVP_PKEY* raw_ephemeral = nullptr;
    const int has_ephemeral = ::SSL_get_peer_tmp_key(session_, &raw_ephemeral);
    std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)> ephemeral(
        raw_ephemeral, &::EVP_PKEY_free);
    const int kind = ephemeral ? ::EVP_PKEY_base_id(ephemeral.get()) : EVP_PKEY_NONE;
    if (has_ephemeral != 1 || !ephemeral ||
        (kind != EVP_PKEY_EC && kind != EVP_PKEY_X25519 && kind != EVP_PKEY_X448) ||
        ::EVP_PKEY_bits(ephemeral.get()) < 224) {
      return tls_error("Negotiated key exchange does not meet TLS strength policy");
    }
    X509* certificate = ::SSL_get1_peer_certificate(session_);
    if (!certificate) return tls_error("TLS peer did not provide a certificate");
    if (!certificate_key_meets_strength_floor(certificate)) {
      ::X509_free(certificate);
      return tls_error("TLS peer key does not meet strength policy");
    }
    if (!config_.verify_peer) {
      ::X509_free(certificate);
      return {};
    }
    const long verification = ::SSL_get_verify_result(session_);
    const bool hostname_matches = !config_.verify_hostname ||
        certificate_matches_host(certificate, server_name_);
    ::X509_free(certificate);
    if (verification != X509_V_OK) {
      return tls_error("TLS certificate verification failed");
    }
    if (!hostname_matches) return tls_error("TLS hostname verification failed");
    peer_verified_ = config_.verify_hostname;
    return {};
  }

  TlsStep classify(int result, std::string_view operation) {
    if (result == 1) return {};
    const int ssl_error = ::SSL_get_error(session_, result);
    if (ssl_error == SSL_ERROR_WANT_READ) {
      return {TlsStepState::WantRead, 0, 0, {}};
    }
    if (ssl_error == SSL_ERROR_WANT_WRITE) {
      return {TlsStepState::WantWrite, 0, 0, {}};
    }
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
      return {TlsStepState::Closed, 0, 0, {}};
    }

    const unsigned long queued_error = ::ERR_peek_error();
    if (ssl_error == SSL_ERROR_SSL ||
        (ssl_error == SSL_ERROR_SYSCALL && result == 0 && queued_error == 0)) {
      return {TlsStepState::TlsError, 0, 0,
              error_text(std::string(operation))};
    }
    if (ssl_error == SSL_ERROR_SYSCALL) {
      const int system_error = current_socket_error();
      return {TlsStepState::SystemError, 0, system_error,
              error_text(std::string(operation))};
    }
    return {TlsStepState::SystemError, 0, 0,
            error_text(std::string(operation))};
  }

  static TlsStep tls_error(std::string message) {
    return {TlsStepState::TlsError, 0, 0, std::move(message)};
  }

  static TlsStep failure(std::string operation) {
    return {TlsStepState::TlsError, 0, 0, error_text(std::move(operation))};
  }

  TlsClientConfig config_;
  TlsClientConfig context_config_;
  bool frozen_{false};
  bool platform_verification_{false};
  std::string platform_failure_;
  std::vector<std::vector<unsigned char>> platform_test_anchors_;
  std::chrono::steady_clock::time_point verification_deadline_{
      std::chrono::steady_clock::time_point::max()};
#if defined(_WIN32)
  // Descriptor/state outlive the context/store lookup that borrows them.
  // Every context-release path frees SSL_CTX before resetting this owner.
  std::unique_ptr<WindowsHashDirectoryLookup> directory_lookup_;
#endif
  SSL_CTX* context_{nullptr};
  SSL* session_{nullptr};
  BIO* read_bio_{nullptr};
  BIO* write_bio_{nullptr};
  std::string server_name_;
  bool peer_verified_{false};
  bool handshake_complete_{false};
};

TlsClient::TlsClient() : impl_(std::make_unique<Impl>()) {}
TlsClient::~TlsClient() = default;
std::unique_ptr<TlsClient> TlsClient::verified_sibling() const {
  auto copied = impl_->verified_sibling();
  if (!copied) return nullptr;
  auto sibling = std::make_unique<TlsClient>();
  sibling->impl_ = std::move(copied);
  return sibling;
}
bool TlsClient::frozen_context() const noexcept { return impl_->frozen_context(); }
void TlsClient::configure(TlsClientConfig config) {
  impl_->configure(std::move(config));
}
TlsStep TlsClient::begin_socket(std::intptr_t socket, std::string_view host) {
  return impl_->begin_socket(socket, host);
}
TlsStep TlsClient::begin_memory(std::string_view host) {
  return impl_->begin_memory(host);
}
TlsStep TlsClient::handshake() { return impl_->handshake(); }
TlsStep TlsClient::write(std::span<const std::byte> plaintext) {
  return impl_->write(plaintext);
}
TlsStep TlsClient::read(std::span<std::byte> plaintext) {
  return impl_->read(plaintext);
}
TlsStep TlsClient::drain_ciphertext(std::span<std::byte> output) {
  return impl_->drain_ciphertext(output);
}
TlsStep TlsClient::provide_ciphertext(std::span<const std::byte> input) {
  return impl_->provide_ciphertext(input);
}
bool TlsClient::ciphertext_pending() const noexcept {
  return impl_->ciphertext_pending();
}
bool TlsClient::active() const noexcept { return impl_->active(); }
bool TlsClient::peer_identity_verified() const noexcept {
  return impl_->peer_identity_verified();
}
void TlsClient::reset_session() noexcept { impl_->reset_session(); }
void TlsClient::replace_platform_anchors_for_testing(
    std::vector<std::vector<unsigned char>> anchors) {
  impl_->replace_platform_anchors_for_testing(std::move(anchors));
}
void TlsClient::set_verification_deadline(
    std::chrono::steady_clock::time_point deadline) noexcept {
  impl_->set_verification_deadline(deadline);
}
void TlsClient::reset_context_if_inactive() noexcept {
  impl_->reset_context_if_inactive();
}

}  // namespace rs::core::security
