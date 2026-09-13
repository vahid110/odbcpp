#include "tls_transport.h"
#include "core/transport/tls_io.h"
#include "core/transport/tls_peer_identity.h"
#include "core/util/exception_adapter.h"

#include <stdexcept>
#include <memory>
#include <vector>
#include <cassert>

#include <openssl/err.h>

using rs::util::Deadline;
using rs::util::IOError;
using rs::util::TLSError;
using rs::util::TimeoutError;

namespace rs::core::transport {

TLSTransport::TLSTransport(DeadlineModel deadline_model) : tcp_(deadline_model) {
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
  if (context_dirty_) {
    if (ctx_) SSL_CTX_free(ctx_);
    ctx_ = nullptr;
    context_dirty_ = false;
  }
  if (ctx_) return;

  const SSL_METHOD* method = TLS_client_method();
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(
      SSL_CTX_new(method), SSL_CTX_free);
  if (!context) {
    ERR_print_errors_fp(stderr);
    throw TLSError("SSL_CTX_new failed");
  }

  // Protocol bounds
  if (SSL_CTX_set_min_proto_version(
          context.get(), static_cast<int>(min_version_)) != 1) {
    throw TLSError("Failed to set minimum TLS version");
  }
#ifdef TLS1_3_VERSION
  if (SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) != 1) {
    throw TLSError("Failed to set maximum TLS version");
  }
#endif

  // Chain verification on/off
  SSL_CTX_set_verify(context.get(), verify_ ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                     nullptr);

  if (verify_) {
    // Load CA trust (file -> dir -> defaults) only when the connection will
    // actually verify its peer. This can involve filesystem or platform trust
    // store I/O, especially on Windows.
    if (!ca_file_.empty()) {
      if (SSL_CTX_load_verify_locations(
              context.get(), ca_file_.c_str(), nullptr) != 1) {
        ERR_print_errors_fp(stderr);
        throw TLSError("Failed to load CA file: " + ca_file_);
      }
    } else if (!ca_dir_.empty()) {
      if (SSL_CTX_load_verify_locations(
              context.get(), nullptr, ca_dir_.c_str()) != 1) {
        ERR_print_errors_fp(stderr);
        throw TLSError("Failed to load CA directory: " + ca_dir_);
      }
    } else {
      if (SSL_CTX_set_default_verify_paths(context.get()) != 1) {
        ERR_print_errors_fp(stderr);
        throw TLSError("Failed to load default CA paths");
      }
    }
  }

#ifdef SSL_MODE_AUTO_RETRY
  // Avoid spurious SSL_ERROR_WANT_READ after writes on some stacks.
  SSL_CTX_set_mode(context.get(), SSL_MODE_AUTO_RETRY);
#endif
  ctx_ = context.release();
}

rs::util::Result<void> TLSTransport::connect(std::string_view host, uint16_t port, Deadline deadline) {
  auto result = connect_plain(host, port, deadline);
  if (result.has_error()) return result;
  return upgrade_to_tls(host, deadline);
}

rs::util::Result<void> TLSTransport::connect_plain(
    std::string_view host, uint16_t port, Deadline deadline) {
  close();
  return tcp_.connect(host, port, deadline);
}

rs::util::Result<void> TLSTransport::upgrade_to_tls(
    std::string_view host, Deadline deadline) {
  if (host.find('\0') != std::string_view::npos) {
    close();
    return {rs::util::DbErrorCode::InvalidParameter,
            "TLS host contains an embedded NUL byte"};
  }
  auto result = rs::util::try_catch([&] { upgrade_impl(host, deadline); });
  if (result.has_error()) close();
  return result;
}

void TLSTransport::upgrade_from(socket_t s, std::string_view host, Deadline deadline) {
  close();
  tcp_.adopt(s);
  try {
    upgrade_impl(host, deadline);
  } catch (...) {
    close();
    throw;
  }
}

void TLSTransport::upgrade_impl(std::string_view host, Deadline deadline) {
  if (host.find('\0') != std::string_view::npos) {
    throw TLSError("TLS host contains an embedded NUL byte");
  }
  if (ssl_) throw TLSError("TLS session is already active");
  tcp_.prepare_for_io(deadline);
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

  if (tls_host_uses_sni(sni_host_)) {
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
  if (!tls_certificate_matches_host(cert, sni_host_)) {
    throw TLSError("Hostname verification failed for " + sni_host_);
  }
#else
  // TODO: For OpenSSL < 1.1.0 parse SAN/CN manually if you need legacy support.
#endif
}

void TLSTransport::close() noexcept {
  if (ssl_) {
    // Closing the socket ends this client session. SSL_shutdown can write a
    // close_notify to an already-closed peer and raise SIGPIPE on Unix.
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  tcp_.close();
}

rs::util::Result<IOResult> TLSTransport::send(std::span<const std::byte> buf, Deadline dl) {
  if (buf.empty()) return IOResult{0, false};
  if (!ssl_) return tcp_.send(buf, dl);
  return rs::util::try_catch([&]() {
  tcp_.prepare_for_io(dl);

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
  if (buf.empty()) return IOResult{0, false};
  if (!ssl_) return tcp_.recv(buf, dl);
  return rs::util::try_catch([&]() {
  tcp_.prepare_for_io(dl);

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
  context_dirty_ = true;
}

} // namespace rs::core::transport
