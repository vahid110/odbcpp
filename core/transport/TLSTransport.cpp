#include "TLSTransport.h"
#include <stdexcept>
#include <vector>
#include <openssl/err.h> // Include for OpenSSL error functions

using rs::util::Deadline;
using rs::util::IOError;
using rs::util::TLSError;
using namespace rs::core::transport;

TLSTransport::TLSTransport() {
  // Lazy SSL_CTX creation in ensure_ctx()
}

TLSTransport::~TLSTransport() { close(); if (ctx_) { SSL_CTX_free(ctx_); ctx_ = nullptr; } }

void TLSTransport::ensure_ctx() {
  if (ctx_) return;

  const SSL_METHOD* method = TLS_client_method();
  ctx_ = SSL_CTX_new(method);
  if (!ctx_) {
    ERR_print_errors_fp(stderr);
    throw TLSError("SSL_CTX_new failed");
  }

  // Protocol bounds
  SSL_CTX_set_min_proto_version(ctx_, (int)min_version_);
#ifdef TLS1_3_VERSION
  SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
#endif

  // Explicit verify mode (chain verification on/off)
  SSL_CTX_set_verify(ctx_, verify_ ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

  // Load CA trust in the chosen order (file -> dir -> defaults)
  if (!ca_file_.empty()) {
    if (SSL_CTX_load_verify_locations(ctx_, ca_file_.c_str(), nullptr) != 1) {
      ERR_print_errors_fp(stderr);
      throw TLSError("Failed to load CA file: " + ca_file_);
    }
  } else if (!ca_dir_.empty()) {
    if (SSL_CTX_load_verify_locations(ctx_, nullptr, ca_dir_.c_str()) != 1) {
      ERR_print_errors_fp(stderr);
      throw TLSError("Failed to load CA directory: " + ca_dir_);
    }
  } else {
    if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
      ERR_print_errors_fp(stderr);
      throw TLSError("Failed to load default CA paths");
    }
  }

  // Optional but harmless: allow automatic retry on read after write
#ifdef SSL_MODE_AUTO_RETRY
  SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY);
#endif
}


void TLSTransport::connect(std::string_view host, uint16_t port, Deadline deadline) {
  ensure_ctx();
  sni_host_ = std::string(host);
  tcp_.connect(host, port, deadline);

  upgrade_from(tcp_.native(), host, deadline);
}

void TLSTransport::upgrade_from(socket_t s, std::string_view host, rs::util::Deadline /*deadline*/) {
  ensure_ctx();
  sni_host_ = std::string(host);

  ssl_ = SSL_new(ctx_);
  if (!ssl_) {
    ERR_print_errors_fp(stderr);
    throw TLSError("SSL_new failed");
  }

#ifdef _WIN32
  if (SSL_set_fd(ssl_, (int)s) != 1)
#else
  if (SSL_set_fd(ssl_, s) != 1)
#endif
  {
    ERR_print_errors_fp(stderr);
    SSL_free(ssl_); ssl_ = nullptr;
    throw TLSError("SSL_set_fd failed");
  }

  if (!sni_host_.empty()) {
    if (SSL_set_tlsext_host_name(ssl_, sni_host_.c_str()) != 1) {
      ERR_print_errors_fp(stderr);
      SSL_free(ssl_); ssl_ = nullptr;
      throw TLSError("Failed to set SNI");
    }
  }

  // Handshake loop (WANT_READ/WRITE)
  for (;;) {
    int rc = SSL_connect(ssl_);
    if (rc == 1) break;
    int err = SSL_get_error(ssl_, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      // With SO_RCVTIMEO/SO_SNDTIMEO set, just retry; or add select() here if you prefer.
      continue;
    }
    ERR_print_errors_fp(stderr);
    SSL_free(ssl_); ssl_ = nullptr;
    throw TLSError(std::string("SSL_connect fatal error: ") + std::to_string(err));
  }

  if (verify_) {
    X509* cert = SSL_get1_peer_certificate(ssl_);
    if (!cert) throw TLSError("No peer certificate");
    long v = SSL_get_verify_result(ssl_);
    if (v != X509_V_OK) {
      X509_free(cert);
      throw TLSError("Certificate verify failed: " + std::to_string(v));
    }
    if (verify_host_) {
      try { verify_hostname(cert); } catch (...) { X509_free(cert); throw; }
    }
    X509_free(cert);
  }
}

void TLSTransport::verify_hostname(X509* cert) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
  if (X509_check_host(cert, sni_host_.c_str(), sni_host_.size(), 0, nullptr) != 1) {
    throw TLSError("Hostname verification failed for " + sni_host_);
  }
#else
  // Older OpenSSL: TODO parse SAN/CN manually if you need pre-1.1.0
#endif
}

void TLSTransport::close() noexcept {
  if (ssl_) {
    // Note: SSL_shutdown is not reliable on non-blocking sockets.
    // For this example, we assume blocking behavior for send/recv.
    SSL_shutdown(ssl_);
    SSL_free(ssl_); ssl_ = nullptr;
  }
  tcp_.close();
}

IOResult TLSTransport::send(std::span<const std::byte> buf, Deadline) {
  if (!ssl_) throw TLSError("TLS not connected");
  // Note: For non-blocking sockets, a robust implementation would handle
  // SSL_ERROR_WANT_READ and SSL_ERROR_WANT_WRITE and re-poll.
  // This simple example assumes blocking I/O for send/recv.
  int n = SSL_write(ssl_, buf.data(), (int)buf.size());
  if (n <= 0) {
    ERR_print_errors_fp(stderr);
    throw TLSError("SSL_write failed");
  }
  return IOResult{ (std::size_t)n, false };
}

IOResult TLSTransport::recv(std::span<std::byte> buf, Deadline) {
  if (!ssl_) throw TLSError("TLS not connected");
  // Note: For non-blocking sockets, a robust implementation would handle
  // SSL_ERROR_WANT_READ and SSL_ERROR_WANT_WRITE and re-poll.
  // This simple example assumes blocking I/O for send/recv.
  int n = SSL_read(ssl_, buf.data(), (int)buf.size());
  if (n == 0) return IOResult{0, true};
  if (n < 0) {
    ERR_print_errors_fp(stderr);
    throw TLSError("SSL_read failed");
  }
  return IOResult{ (std::size_t)n, false };
}

void TLSTransport::set_min_tls_version(long v) { min_version_ = v; }