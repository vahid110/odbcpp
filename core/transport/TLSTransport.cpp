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
  if (!ctx_) throw TLSError("SSL_CTX_new failed");
  SSL_CTX_set_min_proto_version(ctx_, (int)min_version_);
  // Load system default CAs
  if (SSL_CTX_set_default_verify_paths(ctx_) != 1) {
    throw TLSError("Failed to load default CA paths");
  }
}

void TLSTransport::connect(std::string_view host, uint16_t port, Deadline deadline) {
  ensure_ctx();
  sni_host_ = std::string(host);
  tcp_.connect(host, port, deadline);

  ssl_ = SSL_new(ctx_);
  if (!ssl_) throw TLSError("SSL_new failed");
  // Bind to underlying socket
#ifdef _WIN32
  SSL_set_fd(ssl_, (int)tcp_.native());
#else
  SSL_set_fd(ssl_, tcp_.native());
#endif
  // SNI
  SSL_set_tlsext_host_name(ssl_, sni_host_.c_str());

  // Perform handshake
  int rc = SSL_connect(ssl_);
  if (rc != 1) {
    int err = SSL_get_error(ssl_, rc);
    // Print more detailed OpenSSL errors
    ERR_print_errors_fp(stderr);
    throw TLSError(std::string("SSL_connect failed: ") + std::to_string(err));
  }

  if (verify_) {
    X509* cert = SSL_get1_peer_certificate(ssl_);
    if (!cert) throw TLSError("No peer certificate");
    long v = SSL_get_verify_result(ssl_);
    if (v != X509_V_OK) {
      X509_free(cert);
      throw TLSError("Certificate verify failed: " + std::to_string(v));
    }
    try { verify_hostname(cert); } catch (...) { X509_free(cert); throw; }
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