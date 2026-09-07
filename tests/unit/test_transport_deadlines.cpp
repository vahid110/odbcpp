#include <gtest/gtest.h>

#include "core/transport/socket_transport.h"
#include "core/transport/tls_transport.h"
#include "core/util/deadline.h"
#include "core/util/platform.h"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <thread>
#include <vector>

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
};

void expect_receive_timeout(DeadlineModel model) {
#ifdef _WIN32
  const auto server_delay = model == DeadlineModel::SocketTimeout
      ? 1200ms : 250ms;
#else
  const auto server_delay = 250ms;
#endif
  SleepingServer server(server_delay);
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
#ifdef _WIN32
  EXPECT_LT(elapsed, 1100ms);
#else
  EXPECT_LT(elapsed, 500ms);
#endif
}

TEST(SocketTransportDeadlineTest, StrictReceiveHonorsAbsoluteDeadline) {
  expect_receive_timeout(DeadlineModel::Strict);
}

TEST(SocketTransportDeadlineTest, SocketTimeoutIsRefreshedForReceive) {
  expect_receive_timeout(DeadlineModel::SocketTimeout);
}

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

} // namespace
