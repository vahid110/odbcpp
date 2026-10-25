#pragma once

// Disposable in-memory PKI and in-process TLS peer for trust-store tests.
// Test-only: uses OpenSSL directly; production code reaches it only through
// the private security adapter.

#include "odbcpp/security/platform_trust.h"
#include "odbcpp/security/tls_client.h"

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace odbcpp_test::pki {

struct KeyFree { void operator()(EVP_PKEY* key) const { ::EVP_PKEY_free(key); } };
struct CertFree { void operator()(X509* cert) const { ::X509_free(cert); } };
using Key = std::unique_ptr<EVP_PKEY, KeyFree>;
using Cert = std::unique_ptr<X509, CertFree>;

struct Identity {
  Key key;
  Cert cert;
};

struct Spec {
  std::string common_name;
  bool ca{false};
  std::vector<std::string> dns;
  std::vector<std::string> ips;
  // "serverAuth", "clientAuth" or empty for no EKU extension.
  std::string extended_usage{"serverAuth"};
  long not_before_seconds{-3600};
  long not_after_seconds{86400};
  // Zero keeps the normal P-256 fixture; weak RSA is used only in rejection
  // controls, never by the production adapter.
  int rsa_bits{0};
  const EVP_MD* signature_digest{nullptr};
};

inline Key generate_key(int rsa_bits = 0) {
  if (rsa_bits != 0) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&::EVP_PKEY_CTX_free)> context(
        ::EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), &::EVP_PKEY_CTX_free);
    EVP_PKEY* result = nullptr;
    if (!context || ::EVP_PKEY_keygen_init(context.get()) <= 0 ||
        ::EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), rsa_bits) <= 0 ||
        ::EVP_PKEY_keygen(context.get(), &result) <= 0) {
      ::EVP_PKEY_free(result);
      throw std::runtime_error("RSA key generation failed");
    }
    return Key(result);
  }
  Key key(::EVP_EC_gen("P-256"));
  if (!key) throw std::runtime_error("EC key generation failed");
  return key;
}

inline void add_extension(X509* cert, X509* issuer, int nid, const std::string& value) {
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer, cert, nullptr, nullptr, 0);
  X509_EXTENSION* extension = ::X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
  if (!extension) throw std::runtime_error("extension creation failed: " + value);
  ::X509_add_ext(cert, extension, -1);
  ::X509_EXTENSION_free(extension);
}

inline Identity issue(const Spec& spec, const Identity* issuer) {
  static std::atomic<long> serial{1};
  Identity result{generate_key(spec.rsa_bits), Cert(::X509_new())};
  X509* cert = result.cert.get();
  ::X509_set_version(cert, 2);
  std::mt19937_64 random(std::random_device{}());
  ::ASN1_INTEGER_set_int64(::X509_get_serialNumber(cert),
                           static_cast<std::int64_t>((random() >> 2) + serial++));
  ::X509_gmtime_adj(::X509_getm_notBefore(cert), spec.not_before_seconds);
  ::X509_gmtime_adj(::X509_getm_notAfter(cert), spec.not_after_seconds);
  X509_NAME* name = ::X509_get_subject_name(cert);
  ::X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("ODBCPP trust test"), -1, -1, 0);
  ::X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(spec.common_name.c_str()), -1, -1, 0);
  X509* issuer_cert = issuer ? issuer->cert.get() : cert;
  ::X509_set_issuer_name(cert, ::X509_get_subject_name(issuer_cert));
  ::X509_set_pubkey(cert, result.key.get());
  add_extension(cert, issuer_cert, NID_basic_constraints,
                spec.ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
  add_extension(cert, issuer_cert, NID_key_usage,
                spec.ca ? "critical,keyCertSign,cRLSign" : spec.rsa_bits != 0
                    ? "critical,digitalSignature,keyEncipherment" : "critical,digitalSignature");
  add_extension(cert, issuer_cert, NID_subject_key_identifier, "hash");
  if (issuer) add_extension(cert, issuer_cert, NID_authority_key_identifier, "keyid:always");
  if (!spec.ca && !spec.extended_usage.empty()) {
    add_extension(cert, issuer_cert, NID_ext_key_usage, spec.extended_usage);
  }
  std::string alternatives;
  for (const auto& dns : spec.dns) alternatives += (alternatives.empty() ? "" : ",") + ("DNS:" + dns);
  for (const auto& ip : spec.ips) alternatives += (alternatives.empty() ? "" : ",") + ("IP:" + ip);
  if (!alternatives.empty()) add_extension(cert, issuer_cert, NID_subject_alt_name, alternatives);
  EVP_PKEY* signer = issuer ? issuer->key.get() : result.key.get();
  if (::X509_sign(cert, signer, spec.signature_digest ? spec.signature_digest : ::EVP_sha256()) <= 0) {
    throw std::runtime_error("X509_sign failed");
  }
  return result;
}

inline std::string pem(X509* cert) {
  BIO* bio = ::BIO_new(::BIO_s_mem());
  ::PEM_write_bio_X509(bio, cert);
  char* data = nullptr;
  const long size = ::BIO_get_mem_data(bio, &data);
  std::string text(data, static_cast<std::size_t>(size));
  ::BIO_free(bio);
  return text;
}

inline rs::core::security::platform::DerCertificate der(X509* cert) {
  const int size = ::i2d_X509(cert, nullptr);
  rs::core::security::platform::DerCertificate bytes(static_cast<std::size_t>(size));
  unsigned char* cursor = bytes.data();
  ::i2d_X509(cert, &cursor);
  return bytes;
}

inline std::filesystem::path utf8(const std::string& text) {
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline std::string to_utf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

inline void write_file(const std::filesystem::path& path, const std::string& contents) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
  if (!output) throw std::runtime_error("cannot write test file");
}

// OpenSSL hashed-directory entry for one CA certificate.
inline void write_hashed(const std::filesystem::path& directory, X509* cert) {
  std::array<char, 16> name{};
  std::snprintf(name.data(), name.size(), "%08lx.0", ::X509_subject_name_hash(cert));
  write_file(directory / name.data(), pem(cert));
}

class TempDirectory {
 public:
  explicit TempDirectory(const std::string& label) {
    std::mt19937_64 random(std::random_device{}());
    path_ = std::filesystem::temp_directory_path() /
        utf8("odbcpp trust \xC3\xA9 " + label + "-" + std::to_string(random()));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// Server side of an in-memory TLS 1.2/1.3 exchange with a TlsClient.
struct ServerOptions {
  int protocol{0};
  std::string ciphers;
  bool weak_fixture{false};
};

class MemoryServer {
 public:
  MemoryServer(const Identity& leaf, std::vector<const Identity*> chain,
               const ServerOptions& options = {}) {
    context_ = ::SSL_CTX_new(::TLS_server_method());
    std::unique_ptr<SSL_CTX, decltype(&::SSL_CTX_free)> cleanup(context_, &::SSL_CTX_free);
#if !defined(OPENSSL_IS_AWSLC)
    if (context_ && options.weak_fixture) ::SSL_CTX_set_security_level(context_, 0);
#endif
    if (!context_ || (options.protocol &&
        (::SSL_CTX_set_min_proto_version(context_, options.protocol) != 1 ||
         ::SSL_CTX_set_max_proto_version(context_, options.protocol) != 1)) ||
        (!options.ciphers.empty() && ::SSL_CTX_set_cipher_list(context_, options.ciphers.c_str()) != 1)) {
      throw std::runtime_error("server protocol/cipher fixture setup failed");
    }
    if (!context_ || ::SSL_CTX_use_certificate(context_, leaf.cert.get()) != 1 ||
        ::SSL_CTX_use_PrivateKey(context_, leaf.key.get()) != 1) {
      throw std::runtime_error("server context setup failed");
    }
    for (const auto* intermediate : chain) {
      if (::SSL_CTX_add1_chain_cert(context_, intermediate->cert.get()) != 1) {
        throw std::runtime_error("server chain setup failed");
      }
    }
    (void)cleanup.release();
  }
  ~MemoryServer() {
    if (session_) ::SSL_free(session_);
    ::SSL_CTX_free(context_);
  }
  MemoryServer(const MemoryServer&) = delete;
  MemoryServer& operator=(const MemoryServer&) = delete;

  std::string selected_cipher() const {
    const auto* cipher = session_ ? ::SSL_get_current_cipher(session_) : nullptr;
    return cipher ? ::SSL_CIPHER_get_name(cipher) : "";
  }

  // Raw provider-only positive control for intentionally weak peers. Never
  // changes the TlsClient policy being tested. Both endpoints must complete.
  bool unverified_control(int protocol, const char* cipher) {
    std::unique_ptr<SSL_CTX, decltype(&::SSL_CTX_free)> context(
        ::SSL_CTX_new(::TLS_client_method()), &::SSL_CTX_free);
    if (!context) return false;
#if !defined(OPENSSL_IS_AWSLC)
    ::SSL_CTX_set_security_level(context.get(), 0);
#endif
    ::SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
    if (::SSL_CTX_set_min_proto_version(context.get(), protocol) != 1 ||
        ::SSL_CTX_set_max_proto_version(context.get(), protocol) != 1 ||
        ::SSL_CTX_set_cipher_list(context.get(), cipher) != 1) return false;
    std::unique_ptr<SSL, decltype(&::SSL_free)> client(::SSL_new(context.get()), &::SSL_free);
    if (session_) ::SSL_free(session_);
    session_ = ::SSL_new(context_);
    if (!client || !session_) return false;
    ::SSL_set_bio(client.get(), ::BIO_new(::BIO_s_mem()), ::BIO_new(::BIO_s_mem()));
    ::SSL_set_bio(session_, ::BIO_new(::BIO_s_mem()), ::BIO_new(::BIO_s_mem()));
    ::SSL_set_connect_state(client.get());
    ::SSL_set_accept_state(session_);
    std::array<unsigned char, 16384> buffer{};
    const auto transfer = [&](SSL* from, SSL* to) {
      int bytes = 0;
      while ((bytes = ::BIO_read(::SSL_get_wbio(from), buffer.data(), static_cast<int>(buffer.size()))) > 0) {
        if (::BIO_write(::SSL_get_rbio(to), buffer.data(), bytes) != bytes) return false;
      }
      return true;
    };
    for (int round = 0; round < 64; ++round) {
      (void)::SSL_do_handshake(client.get());
      if (!transfer(client.get(), session_)) return false;
      (void)::SSL_do_handshake(session_);
      if (!transfer(session_, client.get())) return false;
      if (::SSL_is_init_finished(client.get()) && ::SSL_is_init_finished(session_)) return true;
    }
    return false;
  }

  // Runs one complete client handshake; returns the client's final step.
  rs::core::security::TlsStep connect(rs::core::security::TlsClient& client,
                                      const std::string& host) {
    using rs::core::security::TlsStepState;
    if (session_) ::SSL_free(session_);
    session_ = ::SSL_new(context_);
    BIO* input = ::BIO_new(::BIO_s_mem());
    BIO* output = ::BIO_new(::BIO_s_mem());
    ::SSL_set_bio(session_, input, output);
    ::SSL_set_accept_state(session_);
    auto started = client.begin_memory(host);
    if (started.state != TlsStepState::Complete) return started;
    client.set_verification_deadline(std::chrono::steady_clock::now() + std::chrono::seconds(10));
    std::array<std::byte, 16384> buffer{};
    for (int round = 0; round < 64; ++round) {
      auto step = client.handshake();
      for (;;) {
        const auto drained = client.drain_ciphertext(buffer);
        if (drained.state != TlsStepState::Complete || drained.processed == 0) break;
        ::BIO_write(input, buffer.data(), static_cast<int>(drained.processed));
      }
      if (step.state != TlsStepState::WantRead && step.state != TlsStepState::WantWrite) {
        return step;
      }
      (void)::SSL_do_handshake(session_);
      for (;;) {
        const int count = ::BIO_read(output, buffer.data(), static_cast<int>(buffer.size()));
        if (count <= 0) break;
        client.provide_ciphertext(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count)));
      }
    }
    return {TlsStepState::TlsError, 0, 0, "handshake did not converge"};
  }

 private:
  SSL_CTX* context_{nullptr};
  SSL* session_{nullptr};
};

// A root, intermediate and leaf for db.example.test / 127.0.0.1.
struct Hierarchy {
  Identity root = issue({"ODBCPP Test Root", true, {}, {}, ""}, nullptr);
  Identity intermediate = issue({"ODBCPP Test Intermediate", true, {}, {}, ""}, &root);
  Identity leaf = issue({"db.example.test", false, {"db.example.test"}, {"127.0.0.1"}}, &intermediate);
};

}  // namespace odbcpp_test::pki
