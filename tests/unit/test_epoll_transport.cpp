#include <gtest/gtest.h>

#ifdef __linux__

#include "core/transport/epoll_transport.h"
#include "core/util/deadline.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using rs::core::transport::EpollTransport;
using rs::core::transport::IOResult;

class LoopbackServer {
public:
  enum class Behavior { Echo, Silent };

  explicit LoopbackServer(Behavior behavior) : behavior_(behavior) {
    listener_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener_ < 0) throw std::runtime_error("failed to create listener");

    int reuse = 1;
    (void)::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0 ||
        ::listen(listener_, 1) != 0) {
      ::close(listener_);
      throw std::runtime_error("failed to bind listener");
    }

    socklen_t length = sizeof(address);
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                      &length) != 0) {
      ::close(listener_);
      throw std::runtime_error("failed to inspect listener");
    }
    port_ = ntohs(address.sin_port);

    worker_ = std::thread([this] {
      const int client = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) return;
      if (behavior_ == Behavior::Echo) {
        std::array<char, 4> request{};
        if (::recv(client, request.data(), request.size(), MSG_WAITALL) == 4) {
          constexpr std::string_view response = "pong";
          (void)::send(client, response.data(), response.size(), MSG_NOSIGNAL);
        }
      } else {
        std::this_thread::sleep_for(300ms);
      }
      ::close(client);
    });
  }

  ~LoopbackServer() {
    ::shutdown(listener_, SHUT_RDWR);
    ::close(listener_);
    if (worker_.joinable()) worker_.join();
  }

  uint16_t port() const noexcept { return port_; }

private:
  Behavior behavior_;
  int listener_{-1};
  uint16_t port_{};
  std::thread worker_;
};

TEST(EpollTransportTest, ValidatesCapacitySettings) {
  EXPECT_THROW(EpollTransport(0, 1), std::invalid_argument);
  EXPECT_THROW(EpollTransport(1, 0), std::invalid_argument);
  EXPECT_THROW(EpollTransport(2, 1), std::invalid_argument);
}

TEST(EpollTransportTest, SupportsSynchronousRoundTripThroughReactor) {
  LoopbackServer server(LoopbackServer::Behavior::Echo);
  EpollTransport transport;

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

TEST(EpollTransportTest, ReceiveDeadlineCompletesExactlyOnce) {
  LoopbackServer server(LoopbackServer::Behavior::Silent);
  EpollTransport transport;
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

TEST(EpollTransportTest, CancellationCompletesExactlyOnce) {
  LoopbackServer server(LoopbackServer::Behavior::Silent);
  EpollTransport transport;
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 1> buffer{};
  std::atomic<int> callback_count{0};
  auto operation = transport.recv_async(
      buffer, rs::util::make_deadline(1s),
      [&](rs::util::Result<IOResult> result) {
        EXPECT_TRUE(result.has_error());
        callback_count.fetch_add(1);
      });

  operation->cancel();
  EXPECT_TRUE(operation->is_complete());
  EXPECT_TRUE(operation->is_cancelled());
  EXPECT_EQ(callback_count.load(), 1);
  operation->cancel();
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(callback_count.load(), 1);
}

TEST(EpollTransportTest, RejectsWorkBeyondConfiguredQueueDepth) {
  LoopbackServer server(LoopbackServer::Behavior::Silent);
  EpollTransport transport(1, 2);
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

#endif // __linux__
