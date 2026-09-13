#include <gtest/gtest.h>

#include "core/transport/socket_transport.h"
#include "core/transport/tls_io.h"
#include "core/transport/tls_peer_identity.h"
#include "core/transport/tls_transport.h"
#include "core/util/deadline.h"
#include "core/util/platform.h"
#include "odbc/odbc_handles.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <pthread.h>
#endif

namespace {

using namespace std::chrono_literals;
using rs::core::transport::DeadlineModel;
using rs::core::transport::SocketTransport;
using rs::core::transport::TLSTransport;

#ifdef _WIN32
using test_socket_t = SOCKET;
constexpr test_socket_t invalid_test_socket = INVALID_SOCKET;
#else
using test_socket_t = int;
constexpr test_socket_t invalid_test_socket = -1;
#endif

void close_test_socket(test_socket_t socket) noexcept {
#ifdef _WIN32
  if (socket != INVALID_SOCKET) ::closesocket(socket);
#else
  if (socket >= 0) ::close(socket);
#endif
}

class SleepingServer {
public:
  explicit SleepingServer(std::chrono::milliseconds sleep_for)
      : sleep_for_(sleep_for) {
    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ == invalid_test_socket) {
      throw std::runtime_error("failed to create test listener");
    }

    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0) {
      close_test_socket(listener_);
      throw std::runtime_error("failed to bind test listener");
    }

#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      close_test_socket(listener_);
      throw std::runtime_error("failed to inspect test listener");
    }
    port_ = ntohs(address.sin_port);

    worker_ = std::thread([this] {
      auto client = ::accept(listener_, nullptr, nullptr);
      if (client != invalid_test_socket) {
        std::this_thread::sleep_for(sleep_for_);
        close_test_socket(client);
      }
    });
  }

  ~SleepingServer() {
    close_test_socket(listener_);
    if (worker_.joinable()) worker_.join();
  }

  uint16_t port() const noexcept { return port_; }

private:
  rs::platform::WSAInit wsa_{};
  test_socket_t listener_{invalid_test_socket};
  uint16_t port_{};
  std::chrono::milliseconds sleep_for_;
  std::thread worker_;
};

class TLSSleepingServer {
public:
  explicit TLSSleepingServer(std::chrono::milliseconds sleep_for)
      : sleep_for_(sleep_for), context_(SSL_CTX_new(TLS_server_method())) {
    if (!context_) throw std::runtime_error("failed to create TLS test context");
    configure_certificate();

    listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ == invalid_test_socket) {
      throw std::runtime_error("failed to create TLS test listener");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0) {
      close_test_socket(listener_);
      throw std::runtime_error("failed to bind TLS test listener");
    }

#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      close_test_socket(listener_);
      throw std::runtime_error("failed to inspect TLS test listener");
    }
    port_ = ntohs(address.sin_port);

    worker_ = std::thread([this] {
      auto client = ::accept(listener_, nullptr, nullptr);
      if (client == invalid_test_socket) return;

      SSL* ssl = SSL_new(context_);
      if (ssl) {
        SSL_set_fd(ssl, static_cast<int>(client));
        if (SSL_accept(ssl) == 1) {
          {
            std::lock_guard lock(handshake_mutex_);
            const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
            observed_sni_ = name ? name : "";
            handshake_complete_ = true;
          }
          handshake_ready_.notify_all();
          std::this_thread::sleep_for(sleep_for_);
        }
        SSL_free(ssl);
      }
      close_test_socket(client);
    });
  }

  ~TLSSleepingServer() {
    close_test_socket(listener_);
    if (worker_.joinable()) worker_.join();
    SSL_CTX_free(context_);
  }

  uint16_t port() const noexcept { return port_; }

  bool wait_for_handshake() {
    std::unique_lock lock(handshake_mutex_);
    return handshake_ready_.wait_for(lock, 1s, [this] {
      return handshake_complete_;
    });
  }

  std::string observed_sni() {
    std::lock_guard lock(handshake_mutex_);
    return observed_sni_;
  }

private:
  void configure_certificate() {
    EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY* key = nullptr;
    X509* certificate = X509_new();
    if (!key_context || !certificate ||
        EVP_PKEY_keygen_init(key_context) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) <= 0 ||
        EVP_PKEY_keygen(key_context, &key) <= 0) {
      EVP_PKEY_CTX_free(key_context);
      EVP_PKEY_free(key);
      X509_free(certificate);
      throw std::runtime_error("failed to generate TLS test key");
    }
    EVP_PKEY_CTX_free(key_context);

    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
    X509_gmtime_adj(X509_get_notBefore(certificate), 0);
    X509_gmtime_adj(X509_get_notAfter(certificate), 60 * 60);
    X509_set_pubkey(certificate, key);
    X509_NAME* name = X509_get_subject_name(certificate);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(certificate, name);

    const bool configured =
        X509_sign(certificate, key, EVP_sha256()) > 0 &&
        SSL_CTX_use_certificate(context_, certificate) == 1 &&
        SSL_CTX_use_PrivateKey(context_, key) == 1;
    EVP_PKEY_free(key);
    X509_free(certificate);
    if (!configured) throw std::runtime_error("failed to configure TLS test certificate");
  }

  rs::platform::WSAInit wsa_{};
  std::chrono::milliseconds sleep_for_;
  SSL_CTX* context_{};
  test_socket_t listener_{invalid_test_socket};
  uint16_t port_{};
  std::thread worker_;
  std::mutex handshake_mutex_;
  std::condition_variable handshake_ready_;
  bool handshake_complete_{false};
  std::string observed_sni_;
};

void expect_receive_timeout(DeadlineModel model) {
  SleepingServer server(250ms);
  SocketTransport transport(model);

  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> buffer{};
  const auto start = std::chrono::steady_clock::now();
  auto result = transport.recv(buffer, rs::util::make_deadline(60ms));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(), rs::util::make_error_code(rs::util::DbErrorCode::Timeout));
  EXPECT_GE(elapsed, 30ms);
  EXPECT_LT(elapsed, 500ms);
}

TEST(SocketTransportDeadlineTest, StrictReceiveHonorsAbsoluteDeadline) {
  expect_receive_timeout(DeadlineModel::Strict);
}

TEST(SocketTransportDeadlineTest, SocketTimeoutIsRefreshedForReceive) {
  expect_receive_timeout(DeadlineModel::SocketTimeout);
}

TEST(SocketTransportDeadlineTest, RejectsEmbeddedNulHostAndClosesOldSocket) {
  SleepingServer server(250ms);
  SocketTransport transport(DeadlineModel::Strict);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  constexpr char malformed[] = "127.0.0.1\0unexpected";
  auto rejected = transport.connect(
      std::string_view(malformed, sizeof(malformed) - 1), server.port(),
      rs::util::make_deadline(1s));
  ASSERT_TRUE(rejected.has_error());
  EXPECT_EQ(rejected.error(),
            rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter));

  const std::array<std::byte, 1> data{std::byte{'x'}};
  EXPECT_TRUE(transport.send(data, rs::util::make_deadline(100ms)).has_error());

  SleepingServer recovered_server(100ms);
  auto recovered = transport.connect(
      "127.0.0.1", recovered_server.port(), rs::util::make_deadline(1s));
  EXPECT_TRUE(recovered.has_value()) << recovered.error_message();
}

#ifndef _WIN32
volatile std::sig_atomic_t interrupted_receive_signals = 0;

void record_interrupted_receive(int) {
  interrupted_receive_signals = 1;
}

TEST(SocketTransportDeadlineTest, InterruptedReceiveStillHonorsDeadline) {
  SleepingServer server(250ms);
  SocketTransport transport(DeadlineModel::SocketTimeout);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  struct sigaction action{};
  action.sa_handler = record_interrupted_receive;
  sigemptyset(&action.sa_mask);
  struct sigaction previous{};
  ASSERT_EQ(0, sigaction(SIGUSR1, &action, &previous));
  interrupted_receive_signals = 0;

  const auto waiting_thread = pthread_self();
  std::thread interrupt([waiting_thread] {
    std::this_thread::sleep_for(30ms);
    (void)pthread_kill(waiting_thread, SIGUSR1);
  });

  std::array<std::byte, 1> buffer{};
  const auto start = std::chrono::steady_clock::now();
  auto result = transport.recv(buffer, rs::util::make_deadline(90ms));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  interrupt.join();
  EXPECT_EQ(0, sigaction(SIGUSR1, &previous, nullptr));

  EXPECT_EQ(1, interrupted_receive_signals);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(), rs::util::make_error_code(rs::util::DbErrorCode::Timeout));
  EXPECT_GE(elapsed, 60ms);
  EXPECT_LT(elapsed, 500ms);
}
#endif

TEST(SocketTransportDeadlineTest, StrictSendTimesOutUnderBackpressure) {
  SleepingServer server(300ms);
  SocketTransport transport(DeadlineModel::Strict);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  int send_buffer_size = 4096;
  ASSERT_EQ(::setsockopt(transport.native(), SOL_SOCKET, SO_SNDBUF,
                         reinterpret_cast<const char*>(&send_buffer_size),
                         sizeof(send_buffer_size)), 0);

  std::vector<std::byte> payload(64 * 1024, std::byte{0x5a});
  const auto deadline = rs::util::make_deadline(80ms);
  rs::util::Result<rs::core::transport::IOResult> result{
      rs::core::transport::IOResult{}};
  do {
    result = transport.send(payload, deadline);
  } while (result.has_value());

  EXPECT_EQ(result.error(), rs::util::make_error_code(rs::util::DbErrorCode::Timeout));
}

TEST(SocketTransportDeadlineTest, ExpiredDeadlineFailsWithoutBlocking) {
  SleepingServer server(100ms);
  SocketTransport transport;
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> buffer{};
  auto result = transport.recv(buffer, rs::util::Clock::now());

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(), rs::util::make_error_code(rs::util::DbErrorCode::Timeout));
}

TEST(OdbcLoginDeadlineTest, ReportsLoginTimeoutInsteadOfConnectionTimeout) {
  SleepingServer server(1250ms);
  rs::odbc::ODBCConnection connection(nullptr);
  ASSERT_EQ(SQL_SUCCESS,
            connection.set_attribute(SQL_ATTR_LOGIN_TIMEOUT, 1));

  const auto connection_string =
      "SERVER=127.0.0.1;PORT=" + std::to_string(server.port()) +
      ";DATABASE=postgres;UID=test;PWD=test;SSL=0;"
      "TransportMode=Sync;DeadlineModel=Strict";
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(SQL_ERROR, connection.connect(connection_string));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_EQ("HYT00", connection.get_sqlstate());
  EXPECT_GE(elapsed, 750ms);
  EXPECT_LT(elapsed, 2500ms);
}

TEST(TLSTransportDeadlineTest, StrictHandshakeTimesOutAgainstSilentPeer) {
  SleepingServer server(250ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);

  const auto start = std::chrono::steady_clock::now();
  auto result = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(60ms));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(), rs::util::make_error_code(rs::util::DbErrorCode::Timeout));
  EXPECT_GE(elapsed, 30ms);
  EXPECT_LT(elapsed, 500ms);

  const std::array<std::byte, 1> plaintext{std::byte{'x'}};
  auto sent = transport.send(plaintext, rs::util::make_deadline(100ms));
  EXPECT_TRUE(sent.has_error()) << "failed TLS must not expose plaintext I/O";
}

TEST(TLSTransportDeadlineTest, CertificateFailureClosesPlainConnection) {
  TLSSleepingServer server(100ms);
  TLSTransport transport(DeadlineModel::Strict);

  auto result = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(result.has_error());

  const std::array<std::byte, 1> plaintext{std::byte{'x'}};
  auto sent = transport.send(plaintext, rs::util::make_deadline(100ms));
  EXPECT_TRUE(sent.has_error()) << "certificate failure must close the socket";
}

TEST(TLSTransportDeadlineTest, PlainReconnectDiscardsPriorTlsSession) {
  TLSSleepingServer first(250ms);
  SleepingServer second(250ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);

  auto connected = transport.connect(
      "127.0.0.1", first.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  ASSERT_TRUE(first.wait_for_handshake());

  auto reconnected = transport.connect_plain(
      "127.0.0.1", second.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(reconnected.has_value()) << reconnected.error_message();

  const std::array<std::byte, 1> plaintext{std::byte{'x'}};
  auto sent = transport.send(plaintext, rs::util::make_deadline(100ms));
  ASSERT_TRUE(sent.has_value()) << sent.error_message();
  EXPECT_EQ(sent->n, plaintext.size());
}

TEST(TLSTransportDeadlineTest, ReconnectAppliesUpdatedVerification) {
  TLSSleepingServer first(250ms);
  TLSSleepingServer second(250ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);

  auto connected = transport.connect(
      "127.0.0.1", first.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  ASSERT_TRUE(first.wait_for_handshake());

  transport.set_verify(true);
  auto rejected = transport.connect(
      "127.0.0.1", second.port(), rs::util::make_deadline(1s));
  EXPECT_TRUE(rejected.has_error())
      << "updated certificate verification must apply on reconnect";

  const std::array<std::byte, 1> plaintext{std::byte{'x'}};
  EXPECT_TRUE(transport.send(plaintext, rs::util::make_deadline(100ms)).has_error());
}

TEST(TLSTransportDeadlineTest, RejectsEmbeddedNulUpgradeAndClosesPlainSocket) {
  TLSSleepingServer server(100ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);
  auto connected = transport.connect_plain(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  constexpr char malformed[] = "localhost\0unexpected";
  auto rejected = transport.upgrade_to_tls(
      std::string_view(malformed, sizeof(malformed) - 1),
      rs::util::make_deadline(1s));
  ASSERT_TRUE(rejected.has_error());
  EXPECT_EQ(rejected.error(),
            rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter));

  const std::array<std::byte, 1> plaintext{std::byte{'x'}};
  EXPECT_TRUE(transport.send(plaintext, rs::util::make_deadline(100ms)).has_error());
}

TEST(TLSPeerIdentityTest, UsesIpSanWithoutFallingBackToDnsNames) {
  std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
  ASSERT_NE(nullptr, certificate);
  char san_text[] = "IP:127.0.0.1,IP:::1,DNS:db.example.test,DNS:127.0.0.2";
  std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
      X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, san_text),
      X509_EXTENSION_free);
  ASSERT_NE(nullptr, san);
  ASSERT_EQ(1, X509_add_ext(certificate.get(), san.get(), -1));

  using rs::core::transport::tls_certificate_matches_host;
  EXPECT_TRUE(tls_certificate_matches_host(certificate.get(), "127.0.0.1"));
  EXPECT_FALSE(tls_certificate_matches_host(certificate.get(), "127.0.0.2"));
  EXPECT_TRUE(tls_certificate_matches_host(certificate.get(), "::1"));
  EXPECT_FALSE(tls_certificate_matches_host(certificate.get(), "::2"));
  EXPECT_TRUE(tls_certificate_matches_host(certificate.get(), "db.example.test"));
  EXPECT_FALSE(tls_certificate_matches_host(certificate.get(), "other.example.test"));
  EXPECT_FALSE(tls_certificate_matches_host(
      certificate.get(), std::string_view("127.0.0.1\0invalid", 17)));
}

TEST(TLSPeerIdentityTest, SendsSniOnlyForDnsNames) {
  using rs::core::transport::tls_host_uses_sni;
  EXPECT_TRUE(tls_host_uses_sni("db.example.test"));
  EXPECT_FALSE(tls_host_uses_sni("127.0.0.1"));
  EXPECT_FALSE(tls_host_uses_sni("::1"));
  EXPECT_FALSE(tls_host_uses_sni(""));
  EXPECT_FALSE(tls_host_uses_sni(std::string_view("db.example.test\0x", 17)));

  TLSSleepingServer ip_server(100ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);
  auto ip_connected = transport.connect(
      "127.0.0.1", ip_server.port(), rs::util::make_deadline(2s));
  ASSERT_TRUE(ip_connected.has_value()) << ip_connected.error_message();
  ASSERT_TRUE(ip_server.wait_for_handshake());
  EXPECT_TRUE(ip_server.observed_sni().empty());

  TLSSleepingServer dns_server(100ms);
  auto dns_connected = transport.connect_plain(
      "127.0.0.1", dns_server.port(), rs::util::make_deadline(2s));
  ASSERT_TRUE(dns_connected.has_value()) << dns_connected.error_message();
  auto upgraded = transport.upgrade_to_tls(
      "localhost", rs::util::make_deadline(2s));
  ASSERT_TRUE(upgraded.has_value()) << upgraded.error_message();
  ASSERT_TRUE(dns_server.wait_for_handshake());
  EXPECT_EQ(dns_server.observed_sni(), "localhost");
}

TEST(TLSTransportDeadlineTest, StrictReceiveTimesOutAfterHandshake) {
  TLSSleepingServer server(250ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);

  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> buffer{};
  auto result = transport.recv(buffer, rs::util::make_deadline(60ms));

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(), rs::util::make_error_code(rs::util::DbErrorCode::Timeout));
}

TEST(TLSTransportDeadlineTest, EmptyTlsIoDoesNotWaitForSocket) {
  TLSSleepingServer server(100ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  ASSERT_TRUE(server.wait_for_handshake());

  const auto expired = rs::util::Clock::now();
  auto sent = transport.send(std::span<const std::byte>{}, expired);
  ASSERT_TRUE(sent.has_value()) << sent.error_message();
  EXPECT_EQ(sent->n, 0u);
  EXPECT_FALSE(sent->eof);

  auto received = transport.recv(std::span<std::byte>{}, expired);
  ASSERT_TRUE(received.has_value()) << received.error_message();
  EXPECT_EQ(received->n, 0u);
  EXPECT_FALSE(received->eof);
}

TEST(TLSTransportDeadlineTest, OpenSslIoLengthsStayWithinSignedInt) {
  const auto maximum = std::numeric_limits<int>::max();
  EXPECT_EQ(rs::core::transport::tls_io_chunk_size(1), 1);
  EXPECT_EQ(rs::core::transport::tls_io_chunk_size(
                static_cast<std::size_t>(maximum)), maximum);
  EXPECT_EQ(rs::core::transport::tls_io_chunk_size(
                static_cast<std::size_t>(maximum) + 1), maximum);
}

} // namespace
