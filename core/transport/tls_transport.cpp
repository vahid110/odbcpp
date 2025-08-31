#include "tls_transport.h"
#include "core/transport/tls_io.h"
#include "core/util/exception_adapter.h"

#include <stdexcept>
#include <vector>
#include <cassert>

#include <openssl/err.h>

using rs::util::Deadline;
using rs::util::IOError;
using rs::util::TLSError;
using rs::util::TimeoutError;

namespace rs::core::transport {

TLSTransport::TLSTransport() {
  // Lazy SSL_CTX creation in ensure_ctx()
}

TLSTransport::~TLSTransport() {
  close();
  if (ctx_) {
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
  }
}

void TLSTransport::ensure_ctx() {
  if (ctx_) return;

  const SSL_METHOD* method = TLS_client_method();
  ctx_ = SSL_CTX_new(method);
  if (!ctx_) {
    ERR_print_errors_fp(stderr);
    throw TLSError("SSL_CTX_new failed");
  }

  // Protocol bounds
  SSL_CTX_set_min_proto_version(ctx_, static_cast<int>(min_version_));
#ifdef TLS1_3_VERSION
  SSL_CTX_set_max_proto_version(ctx_, TLS1_3_VERSION);
#endif

  // Chain verification on/off
  SSL_CTX_set_verify(ctx_, verify_ ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

  // Load CA trust (file -> dir -> defaults)
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

#ifdef SSL_MODE_AUTO_RETRY
  // Avoid spurious SSL_ERROR_WANT_READ after writes on some stacks.
  SSL_CTX_set_mode(ctx_, SSL_MODE_AUTO_RETRY);
#endif
}

rs::util::Result<void> TLSTransport::connect(std::string_view host, uint16_t port, Deadline deadline) {
  return rs::util::try_catch([&]() {
  ensure_ctx();
  sni_host_ = std::string(host);

  tcp_.connect(host, port, deadline);
    upgrade_from(tcp_.native(), host, deadline);
  });
}

void TLSTransport::upgrade_from(socket_t s, std::string_view host, Deadline deadline) {
  (void)s; // We use tcp_.native(); keep signature for external callers.
  ensure_ctx();
  sni_host_ = std::string(host);

  ssl_ = SSL_new(ctx_);
  if (!ssl_) {
    ERR_print_errors_fp(stderr);
    throw TLSError("SSL_new failed");
  }

#ifdef _WIN32
  if (SSL_set_fd(ssl_, static_cast<int>(tcp_.native())) != 1)
#else
  if (SSL_set_fd(ssl_, tcp_.native()) != 1)
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

  // Deadline-aware TLS handshake with WANT_{READ,WRITE} handling
  if (auto e = tls_handshake_with_deadline(ssl_, tcp_.native(), deadline);
      e.code != Errc::Ok) {
    SSL_free(ssl_); ssl_ = nullptr;
    if (e.code == Errc::Timeout) throw TimeoutError("TLS handshake timeout");
    if (e.code == Errc::HandshakeFailed) throw TLSError("TLS handshake failed");
    throw IOError("TLS handshake I/O error");
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
      try { verify_hostname(cert); }
      catch (...) { X509_free(cert); throw; }
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
  // TODO: For OpenSSL < 1.1.0 parse SAN/CN manually if you need legacy support.
#endif
}

void TLSTransport::close() noexcept {
  if (ssl_) {
    // Best-effort shutdown; on non-blocking it may need a loop,
    // but we keep it simple and robust.
    SSL_shutdown(ssl_);
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  tcp_.close();
}

rs::util::Result<IOResult> TLSTransport::send(std::span<const std::byte> buf, Deadline dl) {
  return rs::util::try_catch([&]() {
  if (!ssl_) throw TLSError("TLS not connected");

  size_t n = 0;
  auto e = tls_write_all(
      ssl_, tcp_.native(),
      reinterpret_cast<const uint8_t*>(buf.data()),
      buf.size(),
      dl,
      n);

  if (e.code != Errc::Ok) {
    if (e.code == Errc::Timeout) throw TimeoutError("TLS send timeout");
    throw IOError("TLS send failed");
  }

    return IOResult{ n, false };
  });
}

rs::util::Result<IOResult> TLSTransport::recv(std::span<std::byte> buf, Deadline dl) {
  return rs::util::try_catch([&]() {
  if (!ssl_) throw TLSError("TLS not connected");

  size_t got = 0;
  bool eof = false;
  auto e = tls_read_some(
      ssl_, tcp_.native(),
      reinterpret_cast<uint8_t*>(buf.data()),
      buf.size(),
      dl,
      got,
      eof);

  if (e.code != Errc::Ok) {
    if (e.code == Errc::Timeout) throw TimeoutError("TLS recv timeout");
    throw IOError("TLS recv failed");
  }

    return IOResult{ got, eof };
  });
}

void TLSTransport::set_min_tls_version(long v) {
  min_version_ = v;
}

} // namespace rs::core::transport
