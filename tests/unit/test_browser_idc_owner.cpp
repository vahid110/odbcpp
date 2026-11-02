#include <gtest/gtest.h>

#include "odbcpp/auth/aws/browser_idc_owner.h"
#include "odbcpp/auth/aws/sts_serverless_fake_http.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {
namespace a = rs::core::auth;
namespace bidc = rs::core::auth::aws::browser_idc;
namespace pn = rs::core::auth::aws::provisioned_native;
namespace detail = rs::core::auth::aws::detail;

std::shared_ptr<pn::WorkerCancellation> cancel() {
  return std::make_shared<pn::WorkerCancellation>();
}
bidc::BrowserIdcTarget target() {
  return {"https://identitycenter.amazonaws.com/ssoins-fixture", "eu-north-1", 7890,
      std::chrono::seconds{120}};
}
bool token_equals(const a::SecretBytes& bytes, std::string_view expected) {
  bool matches = false;
  bytes.with_bytes([&](std::span<const std::byte> view) {
    matches = view.size() == expected.size() &&
        std::string_view(reinterpret_cast<const char*>(view.data()), view.size()) == expected;
  });
  return matches;
}
}  // namespace

TEST(BrowserIdcOwner, HappyPathRegistersClientAndExchangesCode) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, cancel());
  detail::fake_browser_auth_code() = "synthetic-auth-code";
  auto made = bidc::BrowserIdcOwner::create(target(),
      rs::util::Clock::now() + std::chrono::seconds{20}, cancel());
  ASSERT_TRUE(made.owner);
  EXPECT_FALSE(made.failure);
  auto owner = std::move(made.owner);
  auto result = owner->acquire();
  ASSERT_TRUE(result);
  EXPECT_TRUE(token_equals(result.value().token, "synthetic-idc-token"));
  EXPECT_GT(result.value().expires_at.microseconds_since_epoch, 0);
  EXPECT_TRUE(owner->close());
  // The OAuth path issues two requests: RegisterClient then CreateToken.
  EXPECT_GE(detail::fake_http_observation().sends.load(), 2U);
}

TEST(BrowserIdcOwner, InvalidTargetAndDeadlineRefuseCreate) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, cancel());
  auto bad_issuer = bidc::BrowserIdcOwner::create(
      {"not-an-https-issuer", "eu-north-1", 7890, std::chrono::seconds{120}},
      rs::util::Clock::now() + std::chrono::seconds{20}, cancel());
  ASSERT_FALSE(bad_issuer.owner);
  EXPECT_EQ(bidc::OwnerFailure::InvalidTarget, *bad_issuer.failure);
  auto bad_region = bidc::BrowserIdcOwner::create(
      {"https://identitycenter.amazonaws.com/ssoins-fixture", "bad region!", 7890,
       std::chrono::seconds{120}},
      rs::util::Clock::now() + std::chrono::seconds{20}, cancel());
  ASSERT_FALSE(bad_region.owner);
  EXPECT_EQ(bidc::OwnerFailure::InvalidTarget, *bad_region.failure);
  auto expired = bidc::BrowserIdcOwner::create(target(),
      rs::util::Clock::now() - std::chrono::seconds{1}, cancel());
  ASSERT_FALSE(expired.owner);
  EXPECT_EQ(bidc::OwnerFailure::DeadlineElapsed, *expired.failure);
}

TEST(BrowserIdcOwner, DeniedRefusesAcquisition) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Denied, 0, cancel());
  detail::fake_browser_auth_code() = "synthetic-auth-code";
  auto made = bidc::BrowserIdcOwner::create(target(),
      rs::util::Clock::now() + std::chrono::seconds{20}, cancel());
  ASSERT_TRUE(made.owner);
  auto owner = std::move(made.owner);
  auto result = owner->acquire();
  ASSERT_FALSE(result);
  EXPECT_EQ(a::FieldFailure::InvalidShape, result.error().failure);
  EXPECT_TRUE(owner->close());
}

namespace {
// RFC 7636 appendix B octets for the code verifier.
constexpr std::array<unsigned char, 32> rfc7636_octets{
    116, 24, 223, 180, 151, 153, 224, 37, 79, 250, 96, 125, 216, 173, 187, 186,
    22, 212, 37, 77, 105, 214, 191, 240, 91, 88, 5, 88, 83, 132, 141, 121};

rs::util::Deadline in_ms(int ms) {
  return rs::util::Clock::now() +
      std::chrono::duration_cast<rs::util::Clock::duration>(std::chrono::milliseconds{ms});
}
unsigned short bound_port(int listener) {
  sockaddr_in address{};
  socklen_t size = sizeof(address);
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size) != 0) return 0;
  return ntohs(address.sin_port);
}
int connect_loopback(unsigned short port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}
bool fd_closed(int fd) { return ::fcntl(fd, F_GETFD) == -1; }
}  // namespace

TEST(BrowserIdcPkce, VerifierEncodesThirtyTwoBytesAndMatchesRfc7636Challenge) {
  const auto verifier = bidc::detail::random_url_token([](std::span<unsigned char> out) {
    ASSERT_EQ(rfc7636_octets.size(), out.size());
    std::memcpy(out.data(), rfc7636_octets.data(), out.size());
  });
  ASSERT_TRUE(verifier.has_value());
  EXPECT_EQ("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", *verifier);
  EXPECT_EQ(43U, verifier->size());
  EXPECT_EQ("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", bidc::detail::pkce_challenge(*verifier));
}

TEST(BrowserIdcPkce, RandomSourceFailureFailsClosed) {
  const auto failed = bidc::detail::random_url_token([](std::span<unsigned char>) {
    throw std::runtime_error("synthetic RNG failure");
  });
  EXPECT_FALSE(failed.has_value());
  EXPECT_FALSE(bidc::detail::random_url_token(bidc::detail::RandomFill{}).has_value());
}

TEST(BrowserIdcLoopback, SilentAcceptedPeerIsBoundedByDeadline) {
  const int listener = bidc::detail::loopback_listen(0);
  ASSERT_GE(listener, 0);
  const int peer = connect_loopback(bound_port(listener));
  ASSERT_GE(peer, 0);
  const auto started = std::chrono::steady_clock::now();
  const auto result = bidc::detail::loopback_receive(listener, in_ms(300), cancel());
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_FALSE(result.ok);
  EXPECT_LT(elapsed, std::chrono::seconds{2});
  EXPECT_TRUE(fd_closed(listener));
  ::close(peer);
}

TEST(BrowserIdcLoopback, CancellationReleasesSilentPeerBeforeDeadline) {
  const int listener = bidc::detail::loopback_listen(0);
  ASSERT_GE(listener, 0);
  const int peer = connect_loopback(bound_port(listener));
  ASSERT_GE(peer, 0);
  auto token = cancel();
  std::thread canceller([token] {
    std::this_thread::sleep_for(std::chrono::milliseconds{150});
    token->cancel();
  });
  const auto started = std::chrono::steady_clock::now();
  const auto result = bidc::detail::loopback_receive(listener, in_ms(20000), token);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  canceller.join();
  EXPECT_FALSE(result.ok);
  EXPECT_LT(elapsed, std::chrono::seconds{2});
  EXPECT_TRUE(fd_closed(listener));
  ::close(peer);
}

TEST(BrowserIdcLoopback, FragmentedRequestIsReassembledAndAnswered) {
  const int listener = bidc::detail::loopback_listen(0);
  ASSERT_GE(listener, 0);
  const int peer = connect_loopback(bound_port(listener));
  ASSERT_GE(peer, 0);
  std::string reply;
  std::thread client([peer, &reply] {
    const std::string first = "GET /?code=abc&st";
    const std::string second = "ate=xyz HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    ::send(peer, first.data(), first.size(), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    ::send(peer, second.data(), second.size(), 0);
    char buffer[512];
    ssize_t got = 0;
    while ((got = ::recv(peer, buffer, sizeof(buffer), 0)) > 0) {
      reply.append(buffer, static_cast<std::size_t>(got));
    }
  });
  const auto result = bidc::detail::loopback_receive(listener, in_ms(5000), cancel());
  client.join();
  EXPECT_TRUE(result.ok);
  EXPECT_EQ("code=abc&state=xyz", result.query);
  EXPECT_EQ(0U, reply.rfind("HTTP/1.1 200 OK", 0));
  EXPECT_TRUE(fd_closed(listener));
  ::close(peer);
}

TEST(BrowserIdcLoopback, NoConnectionTimesOutAndClosesListener) {
  const int listener = bidc::detail::loopback_listen(0);
  ASSERT_GE(listener, 0);
  const auto result = bidc::detail::loopback_receive(listener, in_ms(200), cancel());
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(fd_closed(listener));
}
