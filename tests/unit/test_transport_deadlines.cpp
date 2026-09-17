#include <gtest/gtest.h>

#include "core/transport/socket_transport.h"
#include "core/transport/socket_wait.h"
#include "core/transport/thread_pool_transport.h"
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
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <future>
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
  explicit SleepingServer(std::chrono::milliseconds sleep_for,
                          bool send_after_sleep = false,
                          bool read_after_sleep = false)
      : sleep_for_(sleep_for), send_after_sleep_(send_after_sleep),
        read_after_sleep_(read_after_sleep) {
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
        if (send_after_sleep_) {
          const char reply = 'x';
          (void)::send(client, &reply, 1, 0);
        }
        if (read_after_sleep_) {
          char received = 0;
          const int count = ::recv(client, &received, 1, 0);
          received_byte_.set_value(count == 1 ? received : 0);
        }
        close_test_socket(client);
      }
    });
  }

  ~SleepingServer() {
    close_test_socket(listener_);
    if (worker_.joinable()) worker_.join();
  }

  uint16_t port() const noexcept { return port_; }
  std::future<char> received_byte() { return received_byte_.get_future(); }

private:
  rs::platform::WSAInit wsa_{};
  test_socket_t listener_{invalid_test_socket};
  uint16_t port_{};
  std::chrono::milliseconds sleep_for_;
  bool send_after_sleep_;
  bool read_after_sleep_;
  std::promise<char> received_byte_;
  std::thread worker_;
};

class TLSSleepingServer {
public:
  explicit TLSSleepingServer(std::chrono::milliseconds sleep_for,
                            bool send_close_notify = false)
      : sleep_for_(sleep_for), send_close_notify_(send_close_notify),
        context_(SSL_CTX_new(TLS_server_method())) {
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
          if (send_close_notify_) SSL_shutdown(ssl);
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
  bool send_close_notify_;
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

TEST(DeadlineTest, PositiveSubmillisecondRemainderDoesNotExpire) {
  const rs::util::Deadline origin{};
  const auto tick = rs::util::Clock::duration{1};
  if (tick >= 1ms) GTEST_SKIP() << "clock has no submillisecond resolution";

  EXPECT_EQ(0ms, rs::util::remaining_at(origin, origin));
  EXPECT_EQ(0ms, rs::util::remaining_at(origin, origin + tick));
  EXPECT_EQ(1ms, rs::util::remaining_at(origin + tick, origin));
  EXPECT_EQ(1ms, rs::util::remaining_at(origin + 1ms, origin));
  EXPECT_EQ(2ms, rs::util::remaining_at(origin + 1ms + tick, origin));
}

TEST(SocketTransportDeadlineTest, CappedPollWaitDoesNotExpireLongerDeadline) {
  SleepingServer server(100ms, true);
  SocketTransport transport(DeadlineModel::Strict);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  EXPECT_EQ(rs::core::transport::SocketWaitResult::Ready,
            rs::core::transport::wait_for_socket(
                transport.native(), true, false,
                rs::util::make_deadline(500ms), 5));
}

TEST(SocketTransportDeadlineTest, StrictReceiveHonorsAbsoluteDeadline) {
  expect_receive_timeout(DeadlineModel::Strict);
}

TEST(SocketTransportDeadlineTest, SocketTimeoutIsRefreshedForReceive) {
  expect_receive_timeout(DeadlineModel::SocketTimeout);
}

TEST(SocketTransportDeadlineTest, SocketTimeoutAcceptsMaximumDeadline) {
  SleepingServer server(100ms);
  SocketTransport transport(DeadlineModel::SocketTimeout);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::Deadline::max());
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  auto received = transport.recv(std::span<std::byte>{}, rs::util::Deadline::max());
  ASSERT_TRUE(received.has_value()) << received.error_message();
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

TEST(SocketTransportDeadlineTest, ResolvesLocalhostToIpv4Loopback) {
  SleepingServer server(100ms);
  SocketTransport transport(DeadlineModel::Strict);
  auto connected = transport.connect(
      "localhost", server.port(), rs::util::make_deadline(2s));
  EXPECT_TRUE(connected.has_value()) << connected.error_message();
}

TEST(SocketTransportDeadlineTest, RefusedConnectionDoesNotLeaveOpenSocket) {
  SocketTransport transport(DeadlineModel::Strict);
  SleepingServer prior_server(100ms);
  auto prior_connection = transport.connect(
      "127.0.0.1", prior_server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(prior_connection.has_value()) << prior_connection.error_message();
  ASSERT_NE(transport.native(), invalid_test_socket);

  const test_socket_t unused = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(unused, invalid_test_socket);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ASSERT_EQ(0, ::bind(unused, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)));
#ifdef _WIN32
  int length = sizeof(address);
#else
  socklen_t length = sizeof(address);
#endif
  ASSERT_EQ(0, ::getsockname(unused, reinterpret_cast<sockaddr*>(&address),
                            &length));
  close_test_socket(unused);

  const auto start = std::chrono::steady_clock::now();
  auto result = transport.connect(
      "127.0.0.1", ntohs(address.sin_port), rs::util::make_deadline(2s));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(transport.native(), invalid_test_socket);
#ifndef _WIN32
  EXPECT_LT(elapsed, 1s);
#else
  EXPECT_LT(elapsed, 3s);
#endif
  const std::array<std::byte, 1> data{std::byte{'x'}};
  EXPECT_TRUE(transport.send(data, rs::util::make_deadline(100ms)).has_error());
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

TEST(ThreadPoolTransportDeadlineTest, CancelledReceiveDoesNotWriteCallerBuffer) {
  SleepingServer server(100ms, true);
  rs::core::transport::ThreadPoolTransport transport(1);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> buffer{};
  std::atomic<int> callbacks{0};
  auto operation = transport.recv_async(
      buffer, rs::util::make_deadline(1s),
      [&](rs::util::Result<rs::core::transport::IOResult> result) {
        EXPECT_TRUE(result.has_error());
        callbacks.fetch_add(1);
      });
  operation->cancel();
  ASSERT_TRUE(operation->is_complete());
  ASSERT_TRUE(operation->is_cancelled());
  buffer[0] = std::byte{0x2a};

  auto drained = transport.send_future(
      std::span<const std::byte>{}, rs::util::make_deadline(1s));
  ASSERT_EQ(std::future_status::ready, drained.wait_for(2s));
  EXPECT_EQ(1, callbacks.load());
  EXPECT_EQ(std::byte{0x2a}, buffer[0]);
}

TEST(ThreadPoolTransportDeadlineTest, CancelledQueuedSendDoesNotReachPeer) {
  SleepingServer server(120ms, false, true);
  auto received_byte = server.received_byte();
  rs::core::transport::ThreadPoolTransport transport(1);
  ASSERT_TRUE(transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s)).has_value());

  std::array<std::byte, 1> blocked_buffer{};
  auto blocked = transport.recv_async(
      blocked_buffer, rs::util::make_deadline(40ms), [](auto) {});
  const std::array<std::byte, 1> abandoned{std::byte{'a'}};
  std::atomic<int> cancelled_callbacks{0};
  auto cancelled = transport.send_async(
      abandoned, rs::util::make_deadline(1s), [&](auto result) {
        EXPECT_TRUE(result.has_error());
        cancelled_callbacks.fetch_add(1);
      });
  cancelled->cancel();
  const std::array<std::byte, 1> live{std::byte{'b'}};
  auto sent = transport.send_future(live, rs::util::make_deadline(1s));

  ASSERT_EQ(std::future_status::ready, sent.wait_for(2s));
  ASSERT_TRUE(sent.get().has_value());
  ASSERT_EQ(std::future_status::ready, received_byte.wait_for(2s));
  EXPECT_EQ('b', received_byte.get());
  EXPECT_EQ(1, cancelled_callbacks.load());
}

TEST(ThreadPoolTransportDeadlineTest, CancelledQueuedTaskReleasesQueueCapacity) {
  SleepingServer server(500ms);
  rs::core::transport::ThreadPoolTransport transport(1, 1);
  ASSERT_TRUE(transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s)).has_value());

  std::promise<void> callback_entered;
  std::promise<void> release_callback;
  auto release = release_callback.get_future().share();
  auto active = transport.send_async(
      std::span<const std::byte>{}, rs::util::make_deadline(1s),
      [&](auto) {
        callback_entered.set_value();
        release.wait();
      });
  const auto entered = callback_entered.get_future().wait_for(1s);
  if (entered != std::future_status::ready) {
    release_callback.set_value();
    FAIL() << "worker did not enter the blocking callback";
  }

  std::atomic<int> cancelled_callbacks{0};
  auto cancelled = transport.send_async(
      std::span<const std::byte>{}, rs::util::make_deadline(1s),
      [&](auto result) {
        EXPECT_TRUE(result.has_error());
        cancelled_callbacks.fetch_add(1);
      });
  cancelled->cancel();
  auto replacement = transport.send_future(
      std::span<const std::byte>{}, rs::util::make_deadline(1s));
  std::atomic<int> rejected_callbacks{0};
  std::error_code rejection_error;
  std::string rejection_message;
  auto overflow = transport.send_async(
      std::span<const std::byte>{}, rs::util::make_deadline(1s),
      [&](auto result) {
        if (result.has_error()) {
          rejection_error = result.error();
          rejection_message = result.error_message();
        }
        rejected_callbacks.fetch_add(1);
      });
  EXPECT_TRUE(overflow->is_complete());
  EXPECT_FALSE(overflow->is_cancelled());
  EXPECT_EQ(rejected_callbacks.load(), 1);
  EXPECT_EQ(rejection_error, rs::util::make_error_code(
      rs::util::DbErrorCode::NetworkError));
  EXPECT_NE(rejection_message.find("queue is full"), std::string::npos);

  std::error_code connect_error;
  auto rejected_connect = transport.connect_async(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s),
      [&](auto result) {
        if (result.has_error()) connect_error = result.error();
      });
  EXPECT_TRUE(rejected_connect->is_complete());
  EXPECT_FALSE(rejected_connect->is_cancelled());
  EXPECT_EQ(connect_error, rs::util::make_error_code(
      rs::util::DbErrorCode::NetworkError));

  std::array<std::byte, 1> rejected_buffer{std::byte{0x2a}};
  std::error_code receive_error;
  auto rejected_receive = transport.recv_async(
      rejected_buffer, rs::util::make_deadline(1s),
      [&](auto result) {
        if (result.has_error()) receive_error = result.error();
      });
  EXPECT_TRUE(rejected_receive->is_complete());
  EXPECT_FALSE(rejected_receive->is_cancelled());
  EXPECT_EQ(receive_error, rs::util::make_error_code(
      rs::util::DbErrorCode::NetworkError));
  EXPECT_EQ(rejected_buffer[0], std::byte{0x2a});
  release_callback.set_value();

  ASSERT_EQ(std::future_status::ready, replacement.wait_for(2s));
  EXPECT_TRUE(replacement.get().has_value());
  EXPECT_EQ(cancelled_callbacks.load(), 1);
}

TEST(ThreadPoolTransportDeadlineTest, ExpiredQueuedTaskReleasesQueueCapacity) {
  SleepingServer server(500ms);
  rs::core::transport::ThreadPoolTransport transport(1, 1);
  ASSERT_TRUE(transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s)).has_value());

  std::promise<void> callback_entered;
  std::promise<void> release_callback;
  auto release = release_callback.get_future().share();
  auto active = transport.send_async(
      std::span<const std::byte>{}, rs::util::make_deadline(1s),
      [&](auto) {
        callback_entered.set_value();
        release.wait();
      });
  const auto entered = callback_entered.get_future().wait_for(1s);
  if (entered != std::future_status::ready) {
    release_callback.set_value();
    FAIL() << "worker did not enter the blocking callback";
  }

  auto expired = transport.send_future(
      std::span<const std::byte>{}, rs::util::make_deadline(20ms));
  std::this_thread::sleep_for(50ms);
  auto replacement = transport.send_future(
      std::span<const std::byte>{}, rs::util::make_deadline(1s));
  const auto expired_status = expired.wait_for(100ms);
  EXPECT_EQ(expired_status, std::future_status::ready);
  release_callback.set_value();

  ASSERT_EQ(std::future_status::ready, expired.wait_for(2s));
  auto timed_out = expired.get();
  ASSERT_TRUE(timed_out.has_error());
  EXPECT_EQ(timed_out.error(), rs::util::make_error_code(
      rs::util::DbErrorCode::Timeout));
  ASSERT_EQ(std::future_status::ready, replacement.wait_for(2s));
  EXPECT_TRUE(replacement.get().has_value());
}

TEST(ThreadPoolTransportDeadlineTest, CancelledQueuedReceiveLeavesDataForNext) {
  SleepingServer server(120ms, true);
  rs::core::transport::ThreadPoolTransport transport(1);
  ASSERT_TRUE(transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s)).has_value());

  std::array<std::byte, 1> blocked_buffer{};
  auto blocked = transport.recv_async(
      blocked_buffer, rs::util::make_deadline(40ms), [](auto) {});
  std::array<std::byte, 1> abandoned_buffer{std::byte{0x2a}};
  std::atomic<int> cancelled_callbacks{0};
  auto cancelled = transport.recv_async(
      abandoned_buffer, rs::util::make_deadline(1s), [&](auto result) {
        EXPECT_TRUE(result.has_error());
        cancelled_callbacks.fetch_add(1);
      });
  cancelled->cancel();
  std::array<std::byte, 1> live_buffer{};
  auto received = transport.recv_future(
      live_buffer, rs::util::make_deadline(1s));

  ASSERT_EQ(std::future_status::ready, received.wait_for(2s));
  auto result = received.get();
  ASSERT_TRUE(result.has_value()) << result.error_message();
  EXPECT_EQ(result->n, 1u);
  EXPECT_EQ(live_buffer[0], std::byte{'x'});
  EXPECT_EQ(abandoned_buffer[0], std::byte{0x2a});
  EXPECT_EQ(1, cancelled_callbacks.load());
}

TEST(ThreadPoolTransportDeadlineTest, CancelledQueuedConnectKeepsCurrentSocket) {
  SleepingServer server(120ms);
  rs::core::transport::ThreadPoolTransport transport(1);
  ASSERT_TRUE(transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s)).has_value());

  std::array<std::byte, 1> blocked_buffer{};
  auto blocked = transport.recv_async(
      blocked_buffer, rs::util::make_deadline(40ms), [](auto) {});
  constexpr char malformed[] = "localhost\0unexpected";
  std::atomic<int> cancelled_callbacks{0};
  auto cancelled = transport.connect_async(
      std::string_view(malformed, sizeof(malformed) - 1), server.port(),
      rs::util::make_deadline(1s), [&](auto result) {
        EXPECT_TRUE(result.has_error());
        cancelled_callbacks.fetch_add(1);
      });
  cancelled->cancel();
  auto barrier = transport.send_future(
      std::span<const std::byte>{}, rs::util::make_deadline(1s));

  ASSERT_EQ(std::future_status::ready, barrier.wait_for(2s));
  auto result = barrier.get();
  EXPECT_TRUE(result.has_value()) << result.error_message();
  EXPECT_EQ(1, cancelled_callbacks.load());
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

TEST(SocketTransportDeadlineTest, EmptyIoStillRequiresConnectionAndDeadline) {
  for (auto model : {DeadlineModel::Strict, DeadlineModel::SocketTimeout}) {
    SocketTransport transport(model);
    std::span<const std::byte> empty_send;
    std::span<std::byte> empty_recv;
    const auto deadline = rs::util::make_deadline(1s);
    auto disconnected_send = transport.send(empty_send, deadline);
    auto disconnected_recv = transport.recv(empty_recv, deadline);
    ASSERT_TRUE(disconnected_send.has_error());
    ASSERT_TRUE(disconnected_recv.has_error());
    EXPECT_EQ(disconnected_send.error(), rs::util::make_error_code(
        rs::util::DbErrorCode::NetworkError));
    EXPECT_EQ(disconnected_recv.error(), rs::util::make_error_code(
        rs::util::DbErrorCode::NetworkError));

    SleepingServer server(200ms);
    auto connected = transport.connect(
        "127.0.0.1", server.port(), rs::util::make_deadline(1s));
    ASSERT_TRUE(connected.has_value()) << connected.error_message();
    auto expired_send = transport.send(empty_send, rs::util::Clock::now());
    auto expired_recv = transport.recv(empty_recv, rs::util::Clock::now());
    ASSERT_TRUE(expired_send.has_error());
    ASSERT_TRUE(expired_recv.has_error());
    EXPECT_EQ(expired_send.error(), rs::util::make_error_code(
        rs::util::DbErrorCode::Timeout));
    EXPECT_EQ(expired_recv.error(), rs::util::make_error_code(
        rs::util::DbErrorCode::Timeout));

    auto sent = transport.send(empty_send, rs::util::make_deadline(1s));
    auto received = transport.recv(empty_recv, rs::util::make_deadline(1s));
    ASSERT_TRUE(sent.has_value()) << sent.error_message();
    ASSERT_TRUE(received.has_value()) << received.error_message();
    EXPECT_EQ(sent->n, 0u);
    EXPECT_EQ(received->n, 0u);
    EXPECT_FALSE(received->eof);

    transport.close();
    EXPECT_TRUE(transport.send(empty_send, deadline).has_error());
    EXPECT_TRUE(transport.recv(empty_recv, deadline).has_error());
  }
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

TEST(TLSTransportDeadlineTest, RetriesFailedTrustStoreLoadOnReconnect) {
  const auto missing_ca = std::filesystem::temp_directory_path() /
      ("odbcpp-missing-ca-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".pem");
  ASSERT_FALSE(std::filesystem::exists(missing_ca));

  TLSTransport transport(DeadlineModel::Strict);
  transport.set_ca_locations(missing_ca.string(), "");
  for (int attempt = 0; attempt < 2; ++attempt) {
    SleepingServer server(100ms);
    auto result = transport.connect(
        "127.0.0.1", server.port(), rs::util::make_deadline(1s));
    ASSERT_TRUE(result.has_error());
    EXPECT_NE(result.error_message().find("Failed to load CA file"),
              std::string::npos);
    EXPECT_NE(result.error_message().find("error:"), std::string::npos)
        << "the OpenSSL failure must reach the caller's diagnostic";
  }
}

TEST(TLSTransportDeadlineTest, RejectsInvalidMinimumTlsVersion) {
  SleepingServer server(100ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);
  transport.set_min_tls_version(0x7fff);

  auto result = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(),
            rs::util::make_error_code(rs::util::DbErrorCode::TLSError));
  EXPECT_NE(result.error_message().find("minimum TLS version"),
            std::string::npos);
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

TEST(TLSTransportDeadlineTest, RejectsPeerCloseWithoutCloseNotify) {
  TLSSleepingServer server(25ms);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);

  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  ASSERT_TRUE(server.wait_for_handshake());

  std::array<std::byte, 1> buffer{};
  auto result = transport.recv(buffer, rs::util::make_deadline(1s));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(), rs::util::make_error_code(rs::util::DbErrorCode::TLSError));
}

TEST(TLSTransportDeadlineTest, AcceptsPeerCloseNotifyAsCleanEof) {
  TLSSleepingServer server(25ms, true);
  TLSTransport transport(DeadlineModel::Strict);
  transport.set_verify(false);

  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  ASSERT_TRUE(server.wait_for_handshake());

  std::array<std::byte, 1> buffer{};
  auto result = transport.recv(buffer, rs::util::make_deadline(1s));
  ASSERT_TRUE(result.has_value()) << result.error_message();
  EXPECT_EQ(result->n, 0u);
  EXPECT_TRUE(result->eof);
}

TEST(TLSTransportDeadlineTest, DistinguishesLegacyTlsEofFromSocketFailure) {
  using rs::core::transport::classify_tls_read_failure;
  using rs::core::transport::Errc;

  EXPECT_EQ(classify_tls_read_failure(SSL_ERROR_SSL, -1, 1), Errc::TlsFailed);
  EXPECT_EQ(classify_tls_read_failure(SSL_ERROR_SYSCALL, 0, 0), Errc::TlsFailed);
  EXPECT_EQ(classify_tls_read_failure(SSL_ERROR_SYSCALL, -1, 0), Errc::SyscallFailed);
  EXPECT_EQ(classify_tls_read_failure(SSL_ERROR_SYSCALL, 0, 1), Errc::SyscallFailed);
}

TEST(TLSTransportDeadlineTest, EmptyTlsIoChecksConnectionAndDeadline) {
  TLSTransport transport(DeadlineModel::Strict);
  auto disconnected_send = transport.send(
      std::span<const std::byte>{}, rs::util::make_deadline(1s));
  auto disconnected_recv = transport.recv(
      std::span<std::byte>{}, rs::util::make_deadline(1s));
  ASSERT_TRUE(disconnected_send.has_error());
  ASSERT_TRUE(disconnected_recv.has_error());
  EXPECT_EQ(disconnected_send.error(), rs::util::make_error_code(
      rs::util::DbErrorCode::NetworkError));
  EXPECT_EQ(disconnected_recv.error(), rs::util::make_error_code(
      rs::util::DbErrorCode::NetworkError));

  TLSSleepingServer server(100ms);
  transport.set_verify(false);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  ASSERT_TRUE(server.wait_for_handshake());

  auto sent = transport.send(
      std::span<const std::byte>{}, rs::util::make_deadline(1s));
  ASSERT_TRUE(sent.has_value()) << sent.error_message();
  EXPECT_EQ(sent->n, 0u);
  EXPECT_FALSE(sent->eof);

  auto received = transport.recv(
      std::span<std::byte>{}, rs::util::make_deadline(1s));
  ASSERT_TRUE(received.has_value()) << received.error_message();
  EXPECT_EQ(received->n, 0u);
  EXPECT_FALSE(received->eof);

  const auto expired = rs::util::Clock::now();
  auto expired_send = transport.send(std::span<const std::byte>{}, expired);
  auto expired_recv = transport.recv(std::span<std::byte>{}, expired);
  ASSERT_TRUE(expired_send.has_error());
  ASSERT_TRUE(expired_recv.has_error());
  EXPECT_EQ(expired_send.error(), rs::util::make_error_code(
      rs::util::DbErrorCode::Timeout));
  EXPECT_EQ(expired_recv.error(), rs::util::make_error_code(
      rs::util::DbErrorCode::Timeout));
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
