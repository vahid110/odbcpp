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

TEST(EpollTransportTest, RejectsEmbeddedNulHost) {
  EpollTransport transport;
  constexpr char malformed[] = "127.0.0.1\0unexpected";
  auto result = transport.connect(
      std::string_view(malformed, sizeof(malformed) - 1), 5432,
      rs::util::make_deadline(1s));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(),
            rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter));

  LoopbackServer server(LoopbackServer::Behavior::Silent);
  auto recovered = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  EXPECT_TRUE(recovered.has_value()) << recovered.error_message();
}

TEST(EpollTransportTest, ResolvesLocalhostToIpv4Loopback) {
  LoopbackServer server(LoopbackServer::Behavior::Silent);
  EpollTransport transport;
  auto connected = transport.connect(
      "localhost", server.port(), rs::util::make_deadline(2s));
  EXPECT_TRUE(connected.has_value()) << connected.error_message();
}

TEST(EpollTransportTest, ExpiredConnectCanRecoverOnNextConnection) {
  EpollTransport transport;
  auto expired = transport.connect(
      "localhost", 1, rs::util::Clock::now());
  ASSERT_TRUE(expired.has_error());
  EXPECT_EQ(expired.error(),
            rs::util::make_error_code(rs::util::DbErrorCode::Timeout));

  LoopbackServer server(LoopbackServer::Behavior::Silent);
  auto connected = transport.connect(
      "localhost", server.port(), rs::util::make_deadline(2s));
  EXPECT_TRUE(connected.has_value()) << connected.error_message();
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

TEST(EpollTransportTest, RunsSendAndReceiveConcurrently) {
  LoopbackServer server(LoopbackServer::Behavior::Echo);
  EpollTransport transport(2, 8);
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

TEST(EpollTransportTest, CancelledReceiveLeavesCallerBufferUntouched) {
  LoopbackServer server(LoopbackServer::Behavior::Echo);
  EpollTransport transport;
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(1s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  std::array<std::byte, 4> cancelled_buffer{};
  std::atomic<int> callbacks{0};
  auto cancelled = transport.recv_async(
      cancelled_buffer, rs::util::make_deadline(1s),
      [&](rs::util::Result<IOResult> result) {
        EXPECT_TRUE(result.has_error());
        callbacks.fetch_add(1);
      });
  cancelled->cancel();
  ASSERT_TRUE(cancelled->is_complete());
  ASSERT_TRUE(cancelled->is_cancelled());
  cancelled_buffer.fill(std::byte{0x2a});

  constexpr std::string_view request = "ping";
  auto sent = transport.send(std::as_bytes(
      std::span<const char>(request.data(), request.size())),
      rs::util::make_deadline(1s));
  ASSERT_TRUE(sent.has_value()) << sent.error_message();
  std::array<std::byte, 4> response{};
  auto received = transport.recv(response, rs::util::make_deadline(1s));
  ASSERT_TRUE(received.has_value()) << received.error_message();
  EXPECT_EQ(std::memcmp(response.data(), "pong", response.size()), 0);
  EXPECT_EQ(1, callbacks.load());
  for (auto byte : cancelled_buffer) EXPECT_EQ(std::byte{0x2a}, byte);
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

TEST(EpollTransportTest, CancelledRequestReleasesQueueCapacity) {
  LoopbackServer server(LoopbackServer::Behavior::Silent);
  std::mutex mutex;
  std::condition_variable ready;
  bool callback_entered = false;
  bool release_callback = false;
  std::array<std::byte, 1> cancelled_buffer{};
  std::atomic<int> cancelled_callbacks{0};
  std::atomic<int> replacement_callbacks{0};
  std::atomic<bool> replacement_succeeded{false};
  EpollTransport transport(1, 1);
  auto connected = transport.connect(
      "127.0.0.1", server.port(), rs::util::make_deadline(2s));
  ASSERT_TRUE(connected.has_value()) << connected.error_message();

  auto blocker = transport.send_async(
      std::span<const std::byte>{}, rs::util::make_deadline(2s),
      [&](rs::util::Result<IOResult> result) {
        EXPECT_TRUE(result.has_value());
        if (result.has_error()) return;
        std::unique_lock lock(mutex);
        callback_entered = true;
        ready.notify_all();
        ready.wait(lock, [&] { return release_callback; });
      });
  {
    std::unique_lock lock(mutex);
    const bool entered = ready.wait_for(lock, 2s, [&] {
      return callback_entered;
    });
    if (!entered) {
      release_callback = true;
      lock.unlock();
      ready.notify_all();
      FAIL() << "reactor did not enter the blocking callback";
    }
  }

  auto cancelled = transport.recv_async(
      cancelled_buffer, rs::util::make_deadline(2s),
      [&](rs::util::Result<IOResult> result) {
        EXPECT_TRUE(result.has_error());
        cancelled_callbacks.fetch_add(1);
      });
  cancelled->cancel();
  auto replacement = transport.send_async(
      std::span<const std::byte>{}, rs::util::make_deadline(2s),
      [&](rs::util::Result<IOResult> result) {
        replacement_succeeded.store(result.has_value());
        replacement_callbacks.fetch_add(1);
        ready.notify_all();
      });
  {
    std::lock_guard lock(mutex);
    release_callback = true;
  }
  ready.notify_all();

  std::unique_lock lock(mutex);
  ASSERT_TRUE(ready.wait_for(lock, 2s, [&] {
    return replacement_callbacks.load() == 1;
  }));
  EXPECT_TRUE(replacement_succeeded.load());
  EXPECT_EQ(cancelled_callbacks.load(), 1);
  EXPECT_TRUE(cancelled->is_cancelled());
  EXPECT_TRUE(replacement->is_complete());
}

} // namespace

#endif // __linux__
