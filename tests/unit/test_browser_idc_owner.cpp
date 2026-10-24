#include <gtest/gtest.h>

#include "odbcpp/auth/aws/browser_idc_owner.h"
#include "odbcpp/auth/aws/sts_serverless_fake_http.h"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

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
