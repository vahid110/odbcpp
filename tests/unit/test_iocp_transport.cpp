#include <gtest/gtest.h>

#ifdef _WIN32

#include "core/transport/iocp_transport.h"
#include "core/util/deadline.h"
#include "core/util/platform.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using rs::core::transport::IOResult;
using rs::core::transport::IocpTransport;

class LoopbackServer {
public:
  enum class Behavior { Echo, Silent };

  explicit LoopbackServer(Behavior behavior) : behavior_(behavior) {
    listener_ = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                             WSA_FLAG_OVERLAPPED);
    if (listener_ == INVALID_SOCKET) {
      throw std::runtime_error("failed to create listener");
    }

    BOOL reuse = TRUE;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse),
                       static_cast<int>(sizeof(reuse)));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address),
               static_cast<int>(sizeof(address))) != 0 ||
        ::listen(listener_, 1) != 0) {
      ::closesocket(listener_);
      listener_ = INVALID_SOCKET;
      throw std::runtime_error("failed to bind listener");
    }

    int length = static_cast<int>(sizeof(address));
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                      &length) != 0) {
      ::closesocket(listener_);
      listener_ = INVALID_SOCKET;
      throw std::runtime_error("failed to inspect listener");
    }
    port_ = ntohs(address.sin_port);

    worker_ = std::thread([this] {
      const SOCKET client = ::accept(listener_, nullptr, nullptr);
      if (client == INVALID_SOCKET) return;
      if (behavior_ == Behavior::Echo) {
        std::array<char, 4> request{};
        std::size_t received = 0;
        while (received < request.size()) {
          const int count = ::recv(
              client, request.data() + received,
              static_cast<int>(request.size() - received), 0);
          if (count <= 0) break;
          received += static_cast<std::size_t>(count);
        }
        if (received == request.size()) {
          constexpr std::string_view response = "pong";
          (void)::send(client, response.data(),
                       static_cast<int>(response.size()), 0);
        }
      } else {
        std::this_thread::sleep_for(300ms);
      }
      ::closesocket(client);
    });
  }

  ~LoopbackServer() {
    if (listener_ != INVALID_SOCKET) {
      (void)::shutdown(listener_, SD_BOTH);
      (void)::closesocket(listener_);
    }
    if (worker_.joinable()) worker_.join();
  }

  uint16_t port() const noexcept { return port_; }

private:
  rs::platform::WSAInit winsock_{};
  Behavior behavior_;
  SOCKET listener_{INVALID_SOCKET};
  uint16_t port_{};
  std::thread worker_;
};

TEST(IocpTransportTest, ValidatesCapacitySettings) {
  EXPECT_THROW(IocpTransport(0, 1), std::invalid_argument);
  EXPECT_THROW(IocpTransport(1, 0), std::invalid_argument);
  EXPECT_THROW(IocpTransport(2, 1), std::invalid_argument);
}

TEST(IocpTransportTest, SupportsSynchronousRoundTripThroughCompletionPort) {
  LoopbackServer server(LoopbackServer::Behavior::Echo);
  IocpTransport transport;

  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  constexpr std::string_view request = "ping";
  const auto request_bytes = std::as_bytes(
      std::span<const char>(request.data(), request.size()));
  auto sent = transport.send(request_bytes, rs::util::make_deadline(1s));
  ASSERT_TRUE(sent.has_value()) << sent.error_message();
  EXPECT_EQ(sent->n, request.size());

  std::array<std::byte, 4> response{};
  auto received = transport.recv(response, rs::util::make_deadline(1s));
  ASSERT_TRUE(received.has_value()) << received.error_message();
  EXPECT_EQ(received->n, response.size());
  EXPECT_EQ(std::memcmp(response.data(), "pong", response.size()), 0);
}

TEST(IocpTransportTest, RunsSendAndReceiveConcurrently) {
  LoopbackServer server(LoopbackServer::Behavior::Echo);
  IocpTransport transport(2, 8);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 4> response{};
  std::mutex mutex;
  std::condition_variable ready;
  std::atomic<int> callback_count{0};
  std::atomic<bool> send_succeeded{false};
  std::atomic<bool> recv_succeeded{false};

  auto receive = transport.recv_async(
      response, rs::util::make_deadline(1s),
      [&](rs::util::Result<IOResult> result) {
        recv_succeeded.store(result.has_value() && result->n == response.size());
        callback_count.fetch_add(1);
        ready.notify_one();
      });

  constexpr std::string_view request = "ping";
  const auto request_bytes = std::as_bytes(
      std::span<const char>(request.data(), request.size()));
  auto send = transport.send_async(
      request_bytes, rs::util::make_deadline(1s),
      [&](rs::util::Result<IOResult> result) {
        send_succeeded.store(result.has_value() && result->n == request.size());
        callback_count.fetch_add(1);
        ready.notify_one();
      });

  std::unique_lock lock(mutex);
  ASSERT_TRUE(ready.wait_for(lock, 2s, [&] {
    return callback_count.load() == 2;
  }));
  lock.unlock();
  EXPECT_TRUE(send_succeeded.load());
  EXPECT_TRUE(recv_succeeded.load());
  EXPECT_EQ(std::memcmp(response.data(), "pong", response.size()), 0);
  EXPECT_TRUE(send->is_complete());
  EXPECT_TRUE(receive->is_complete());
}

TEST(IocpTransportTest, ReceiveDeadlineCompletesExactlyOnce) {
  LoopbackServer server(LoopbackServer::Behavior::Silent);
  IocpTransport transport;
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> buffer{};
  std::mutex mutex;
  std::condition_variable ready;
  std::atomic<int> callback_count{0};
  rs::util::DbErrorCode error = rs::util::DbErrorCode::Success;
  auto operation = transport.recv_async(
      buffer, rs::util::make_deadline(60ms),
      [&](rs::util::Result<IOResult> result) {
        if (result.has_error()) {
          error = static_cast<rs::util::DbErrorCode>(result.error().value());
        }
        callback_count.fetch_add(1);
        ready.notify_one();
      });

  std::unique_lock lock(mutex);
  ASSERT_TRUE(ready.wait_for(lock, 1s, [&] { return callback_count.load() > 0; }));
  lock.unlock();
  EXPECT_EQ(error, rs::util::DbErrorCode::Timeout);
  EXPECT_TRUE(operation->is_complete());
  EXPECT_FALSE(operation->is_cancelled());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(callback_count.load(), 1);
}

TEST(IocpTransportTest, CancellationCompletesExactlyOnce) {
  LoopbackServer server(LoopbackServer::Behavior::Silent);
  IocpTransport transport;
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> buffer{};
  std::mutex mutex;
  std::condition_variable ready;
  std::atomic<int> callback_count{0};
  auto operation = transport.recv_async(
      buffer, rs::util::make_deadline(1s),
      [&](rs::util::Result<IOResult> result) {
        EXPECT_TRUE(result.has_error());
        callback_count.fetch_add(1);
        ready.notify_one();
      });

  operation->cancel();
  EXPECT_TRUE(operation->is_cancelled());
  std::unique_lock lock(mutex);
  ASSERT_TRUE(ready.wait_for(lock, 1s, [&] { return callback_count.load() > 0; }));
  lock.unlock();
  EXPECT_TRUE(operation->is_complete());
  operation->cancel();
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(callback_count.load(), 1);
}

TEST(IocpTransportTest, RejectsWorkBeyondConfiguredQueueDepth) {
  LoopbackServer server(LoopbackServer::Behavior::Silent);
  IocpTransport transport(1, 2);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> first_buffer{};
  std::array<std::byte, 1> second_buffer{};
  std::array<std::byte, 1> rejected_buffer{};
  auto first = transport.recv_async(
      first_buffer, rs::util::make_deadline(1s),
      [](rs::util::Result<IOResult>) {});
  auto second = transport.recv_async(
      second_buffer, rs::util::make_deadline(1s),
      [](rs::util::Result<IOResult>) {});

  std::atomic<int> rejected_callbacks{0};
  std::string rejection_message;
  auto rejected = transport.recv_async(
      rejected_buffer, rs::util::make_deadline(1s),
      [&](rs::util::Result<IOResult> result) {
        ASSERT_TRUE(result.has_error());
        rejection_message = result.error_message();
        rejected_callbacks.fetch_add(1);
      });

  EXPECT_TRUE(rejected->is_complete());
  EXPECT_FALSE(rejected->is_cancelled());
  EXPECT_EQ(rejected_callbacks.load(), 1);
  EXPECT_NE(rejection_message.find("queue is full"), std::string::npos);

  first->cancel();
  second->cancel();
}

} // namespace

#endif // _WIN32
