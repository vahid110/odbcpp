#include "odbcpp/security/tls_client.h"

#include "odbcpp/util/platform.h"

#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <signal.h>
#endif

namespace rs::core::security {
namespace {

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
    auto status = classify(result, "SSL_connect");
    if (status.state != TlsStepState::Complete) return status;
    return verify_peer();
  }

  TlsStep write(std::span<const std::byte> plaintext) {
    if (!session_) return tls_error("TLS session is not active");
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
    if (::SSL_CTX_set_min_proto_version(sibling->context_, static_cast<int>(context_config_.minimum_version)) != 1) return nullptr;
#ifdef TLS1_3_VERSION
    if (::SSL_CTX_set_max_proto_version(sibling->context_, TLS1_3_VERSION) != 1) return nullptr;
#endif
    ::SSL_CTX_set_verify(sibling->context_, SSL_VERIFY_PEER, nullptr);
    auto* source = ::SSL_CTX_get_cert_store(context_);
    auto* target = ::SSL_CTX_get_cert_store(sibling->context_);
    if (!source || !target || ::X509_VERIFY_PARAM_set1(::X509_STORE_get0_param(target), ::X509_STORE_get0_param(source)) != 1) return nullptr;
    const auto* objects = ::X509_STORE_get0_objects(source);
    for (int i = 0; objects && i < sk_X509_OBJECT_num(objects); ++i) {
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
    sibling->frozen_ = true;
    return sibling;
  }
  bool frozen_context() const noexcept { return frozen_; }
  bool active() const noexcept { return session_ != nullptr; }
  bool peer_identity_verified() const noexcept { return peer_verified_; }

  void reset_session() noexcept {
    peer_verified_ = false;
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
  }

private:
  TlsStep begin(std::string_view host) {
    if (host.find('\0') != std::string_view::npos) {
      return tls_error("TLS host contains an embedded NUL byte");
    }
    reset_session();
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
    if (context_) return {};
    context_config_ = config_;
    context_ = ::SSL_CTX_new(::TLS_client_method());
    if (!context_) return failure("SSL_CTX_new");
    const auto fail_context = [this](std::string operation) {
      auto failed = failure(std::move(operation));
      ::SSL_CTX_free(context_);
      context_ = nullptr;
      return failed;
    };
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
    if (!config_.verify_peer) return {};
    int loaded = 0;
    if (!config_.ca_file.empty()) {
      loaded = ::SSL_CTX_load_verify_locations(
          context_, config_.ca_file.c_str(), nullptr);
    } else if (!config_.ca_directory.empty()) {
      loaded = ::SSL_CTX_load_verify_locations(
          context_, nullptr, config_.ca_directory.c_str());
    } else {
      loaded = ::SSL_CTX_set_default_verify_paths(context_);
    }
    if (loaded != 1) {
      if (!config_.ca_file.empty()) {
        return fail_context("Failed to load CA file: " + config_.ca_file);
      }
      if (!config_.ca_directory.empty()) {
        return fail_context(
            "Failed to load CA directory: " + config_.ca_directory);
      }
      return fail_context("Failed to load default CA paths");
    }
    return {};
  }

  TlsStep verify_peer() {
    if (!config_.verify_peer) return {};
    X509* certificate = ::SSL_get1_peer_certificate(session_);
    if (!certificate) return tls_error("TLS peer did not provide a certificate");
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
  SSL_CTX* context_{nullptr};
  SSL* session_{nullptr};
  BIO* read_bio_{nullptr};
  BIO* write_bio_{nullptr};
  std::string server_name_;
  bool peer_verified_{false};
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
void TlsClient::reset_context_if_inactive() noexcept {
  impl_->reset_context_if_inactive();
}

}  // namespace rs::core::security
