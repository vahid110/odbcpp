#include <gtest/gtest.h>

#if defined(__linux__) || defined(_WIN32)

#include "core/transport/async_tls_transport.h"
#ifdef __linux__
#include "core/transport/epoll_transport.h"
#else
#include "core/transport/iocp_transport.h"
#endif
#include "core/util/deadline.h"
#include "core/util/platform.h"

#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using rs::core::transport::AsyncTlsTransport;
using rs::core::transport::IAsyncTransport;
using rs::core::transport::IOResult;

#ifdef _WIN32
using test_socket_t = SOCKET;
constexpr test_socket_t invalid_test_socket = INVALID_SOCKET;
void close_test_socket(test_socket_t socket) {
  if (socket != INVALID_SOCKET) (void)::closesocket(socket);
}
#else
using test_socket_t = int;
constexpr test_socket_t invalid_test_socket = -1;
void close_test_socket(test_socket_t socket) {
  if (socket >= 0) (void)::close(socket);
}
#endif

std::unique_ptr<IAsyncTransport> make_native_transport(
    std::size_t max_inflight = 64, std::size_t queue_depth = 256) {
#ifdef __linux__
  return std::make_unique<rs::core::transport::EpollTransport>(
      max_inflight, queue_depth);
#else
  return std::make_unique<rs::core::transport::IocpTransport>(
      max_inflight, queue_depth);
#endif
}

class TlsLoopbackServer {
public:
  enum class Behavior { DirectEcho, StartTlsEcho, SilentBeforeTls,
                        SilentAfterTls };

  explicit TlsLoopbackServer(Behavior behavior) : behavior_(behavior) {
    if (behavior_ != Behavior::SilentBeforeTls) configure_tls();

    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == invalid_test_socket) {
      throw std::runtime_error("failed to create TLS listener");
    }
    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse),
                       static_cast<int>(sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
#ifdef _WIN32
    const int address_size = static_cast<int>(sizeof(address));
#else
    const socklen_t address_size = sizeof(address);
#endif
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address),
               address_size) != 0 || ::listen(listener_, 1) != 0) {
      close_test_socket(listener_);
      listener_ = invalid_test_socket;
      throw std::runtime_error("failed to bind TLS listener");
    }

#ifdef _WIN32
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = sizeof(address);
#endif
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                      &length) != 0) {
      close_test_socket(listener_);
      listener_ = invalid_test_socket;
      throw std::runtime_error("failed to inspect TLS listener");
    }
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this] { serve(); });
  }

  ~TlsLoopbackServer() {
    if (listener_ != invalid_test_socket) {
#ifdef _WIN32
      (void)::shutdown(listener_, SD_BOTH);
#else
      (void)::shutdown(listener_, SHUT_RDWR);
#endif
      close_test_socket(listener_);
    }
    if (worker_.joinable()) worker_.join();
    if (context_) ::SSL_CTX_free(context_);
  }

  uint16_t port() const noexcept { return port_; }

private:
  void configure_tls() {
    context_ = ::SSL_CTX_new(::TLS_server_method());
    EVP_PKEY_CTX* key_context = ::EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY* key = nullptr;
    X509* certificate = ::X509_new();
    if (!context_ || !key_context || !certificate ||
        ::EVP_PKEY_keygen_init(key_context) <= 0 ||
        ::EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) <= 0 ||
        ::EVP_PKEY_keygen(key_context, &key) <= 0) {
      ::EVP_PKEY_CTX_free(key_context);
      ::EVP_PKEY_free(key);
      ::X509_free(certificate);
      throw std::runtime_error("failed to generate TLS test key");
    }
    ::EVP_PKEY_CTX_free(key_context);

    ::X509_set_version(certificate, 2);
    ::ASN1_INTEGER_set(::X509_get_serialNumber(certificate), 1);
    ::X509_gmtime_adj(::X509_get_notBefore(certificate), 0);
    ::X509_gmtime_adj(::X509_get_notAfter(certificate), 60 * 60);
    ::X509_set_pubkey(certificate, key);
    X509_NAME* name = ::X509_get_subject_name(certificate);
    ::X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    ::X509_set_issuer_name(certificate, name);

    const bool configured =
        ::X509_sign(certificate, key, EVP_sha256()) > 0 &&
        ::SSL_CTX_use_certificate(context_, certificate) == 1 &&
        ::SSL_CTX_use_PrivateKey(context_, key) == 1;
    ::EVP_PKEY_free(key);
    ::X509_free(certificate);
    if (!configured) {
      throw std::runtime_error("failed to configure TLS test certificate");
    }
  }

  static bool receive_exact(test_socket_t socket, void* data,
                            std::size_t size) {
    std::size_t offset = 0;
    auto* bytes = static_cast<char*>(data);
    while (offset < size) {
      const int count = ::recv(socket, bytes + offset,
                               static_cast<int>(size - offset), 0);
      if (count <= 0) return false;
      offset += static_cast<std::size_t>(count);
    }
    return true;
  }

  void serve() {
    const test_socket_t client = ::accept(listener_, nullptr, nullptr);
    if (client == invalid_test_socket) return;
    if (behavior_ == Behavior::SilentBeforeTls) {
      std::this_thread::sleep_for(300ms);
      close_test_socket(client);
      return;
    }
    if (behavior_ == Behavior::StartTlsEcho) {
      std::array<std::byte, 8> request{};
      if (!receive_exact(client, request.data(), request.size())) {
        close_test_socket(client);
        return;
      }
      constexpr char accepted = 'S';
      if (::send(client, &accepted, 1, 0) != 1) {
        close_test_socket(client);
        return;
      }
    }

    SSL* ssl = ::SSL_new(context_);
#ifdef _WIN32
    const int descriptor = static_cast<int>(client);
#else
    const int descriptor = client;
#endif
    if (!ssl || ::SSL_set_fd(ssl, descriptor) != 1 ||
        ::SSL_accept(ssl) != 1) {
      if (ssl) ::SSL_free(ssl);
      close_test_socket(client);
      return;
    }

    if (behavior_ == Behavior::SilentAfterTls) {
      std::this_thread::sleep_for(300ms);
    } else {
      std::array<char, 4> request{};
      if (::SSL_read(ssl, request.data(),
                     static_cast<int>(request.size())) == 4) {
        constexpr std::string_view response = "pong";
        (void)::SSL_write(ssl, response.data(),
                          static_cast<int>(response.size()));
      }
    }
    // The cancellation test deliberately closes the peer first. Avoid writing
    // close_notify from this test server after that close, which can raise
    // SIGPIPE on Unix and obscures the transport behavior under test.
    ::SSL_free(ssl);
    close_test_socket(client);
  }

  rs::platform::WSAInit winsock_{};
  Behavior behavior_;
  SSL_CTX* context_{nullptr};
  test_socket_t listener_{invalid_test_socket};
  uint16_t port_{};
  std::thread worker_;
};

TEST(AsyncTlsTransportTest, SupportsDirectTlsRoundTrip) {
  TlsLoopbackServer server(TlsLoopbackServer::Behavior::DirectEcho);
  AsyncTlsTransport transport(make_native_transport());
  transport.set_verify(false);

  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(2s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  constexpr std::string_view request = "ping";
  auto sent = transport.send(
      std::as_bytes(std::span<const char>(request.data(), request.size())),
      rs::util::make_deadline(1s));
  ASSERT_TRUE(sent.has_value()) << sent.error_message();
  EXPECT_EQ(sent->n, request.size());

  std::array<std::byte, 4> response{};
  auto received = transport.recv(response, rs::util::make_deadline(1s));
  ASSERT_TRUE(received.has_value()) << received.error_message();
  EXPECT_EQ(received->n, response.size());
  EXPECT_EQ(std::memcmp(response.data(), "pong", response.size()), 0);
}

TEST(AsyncTlsTransportTest, ValidatesCapacitySettings) {
  EXPECT_THROW(AsyncTlsTransport(make_native_transport(1, 1), 0, 1),
               std::invalid_argument);
  EXPECT_THROW(AsyncTlsTransport(make_native_transport(1, 1), 1, 0),
               std::invalid_argument);
  EXPECT_THROW(AsyncTlsTransport(make_native_transport(1, 1), 2, 1),
               std::invalid_argument);
}

TEST(AsyncTlsTransportTest, UpgradesExistingPlainConnection) {
  TlsLoopbackServer server(TlsLoopbackServer::Behavior::StartTlsEcho);
  AsyncTlsTransport transport(make_native_transport());
  transport.set_verify(false);

  auto connected = transport.connect_plain(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  constexpr std::array<std::byte, 8> ssl_request{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{8},
      std::byte{4}, std::byte{210}, std::byte{22}, std::byte{47}};
  auto sent_request = transport.send(
      ssl_request, rs::util::make_deadline(1s));
  ASSERT_TRUE(sent_request.has_value()) << sent_request.error_message();

  std::array<std::byte, 1> accepted{};
  auto received_accept = transport.recv(
      accepted, rs::util::make_deadline(1s));
  ASSERT_TRUE(received_accept.has_value()) << received_accept.error_message();
  ASSERT_EQ(received_accept->n, 1u);
  ASSERT_EQ(accepted[0], std::byte{'S'});

  auto upgraded = transport.upgrade_to_tls(
      "127.0.0.1", rs::util::make_deadline(2s));
  ASSERT_TRUE(upgraded.has_value()) << upgraded.error_message();

  constexpr std::string_view request = "ping";
  auto sent = transport.send(
      std::as_bytes(std::span<const char>(request.data(), request.size())),
      rs::util::make_deadline(1s));
  ASSERT_TRUE(sent.has_value()) << sent.error_message();

  std::array<std::byte, 4> response{};
  auto received = transport.recv(response, rs::util::make_deadline(1s));
  ASSERT_TRUE(received.has_value()) << received.error_message();
  EXPECT_EQ(std::memcmp(response.data(), "pong", response.size()), 0);
}

TEST(AsyncTlsTransportTest, HandshakeHonorsStrictDeadline) {
  TlsLoopbackServer server(TlsLoopbackServer::Behavior::SilentBeforeTls);
  AsyncTlsTransport transport(make_native_transport());
  transport.set_verify(false);

  const auto start = std::chrono::steady_clock::now();
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(60ms));
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_TRUE(connected.has_error());
  EXPECT_EQ(connected.error(),
            rs::util::make_error_code(rs::util::DbErrorCode::Timeout));
  EXPECT_LT(elapsed, 500ms);
}

TEST(AsyncTlsTransportTest, CancellationCompletesReceiveExactlyOnce) {
  TlsLoopbackServer server(TlsLoopbackServer::Behavior::SilentAfterTls);
  AsyncTlsTransport transport(make_native_transport());
  transport.set_verify(false);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(2s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> buffer{};
  std::mutex mutex;
  std::condition_variable ready;
  std::atomic<int> callbacks{0};
  auto operation = transport.recv_async(
      buffer, rs::util::make_deadline(1s),
      [&](rs::util::Result<IOResult> result) {
        EXPECT_TRUE(result.has_error());
        callbacks.fetch_add(1);
        ready.notify_one();
      });
  std::this_thread::sleep_for(20ms);
  operation->cancel();

  std::unique_lock lock(mutex);
  ASSERT_TRUE(ready.wait_for(lock, 1s, [&] { return callbacks.load() == 1; }));
  lock.unlock();
  EXPECT_TRUE(operation->is_complete());
  EXPECT_TRUE(operation->is_cancelled());
  operation->cancel();
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(callbacks.load(), 1);
}

} // namespace

#endif
