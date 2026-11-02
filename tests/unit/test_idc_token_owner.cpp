#include <gtest/gtest.h>

#include "odbcpp/auth/aws/idc_token_owner.h"
#include "odbcpp/auth/aws/sts_serverless_fake_http.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

namespace {
namespace a = rs::core::auth;
namespace idc = rs::core::auth::aws::idc;
namespace pn = rs::core::auth::aws::provisioned_native;
namespace detail = rs::core::auth::aws::detail;

a::SecretBytes secret(std::string_view text) {
  auto made = a::SecretBytes::create(std::as_bytes(std::span{text.data(), text.size()}));
  if (!made) std::terminate();
  return std::move(made).value();
}
std::shared_ptr<pn::WorkerCancellation> cancel() {
  return std::make_shared<pn::WorkerCancellation>();
}
idc::IdcTokenTarget target() { return {"eu-north-1", "fixture-workgroup"}; }
bool token_equals(const a::SecretBytes& bytes, std::string_view expected) {
  bool matches = false;
  bytes.with_bytes([&](std::span<const std::byte> view) {
    matches = view.size() == expected.size() &&
        std::string_view(reinterpret_cast<const char*>(view.data()), view.size()) == expected;
  });
  return matches;
}
rs::util::Deadline deadline() { return rs::util::Clock::now() + std::chrono::seconds{20}; }
}  // namespace

TEST(IdcTokenOwner, HappyPathReturnsTokenAndTrustedExpiry) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, cancel());
  auto made = idc::IdcTokenOwner::create(target(), secret("access-key"), secret("secret-key"),
      secret(""), deadline(), cancel());
  ASSERT_TRUE(made.owner);
  EXPECT_FALSE(made.failure);
  auto owner = std::move(made.owner);
  auto result = owner->acquire();
  ASSERT_TRUE(result);
  EXPECT_TRUE(token_equals(result.value().token, "synthetic-idc-token"));
  EXPECT_GT(result.value().expires_at.microseconds_since_epoch, 0);
  EXPECT_EQ(1U, detail::fake_http_observation().get_idc_token.load());
  EXPECT_TRUE(owner->close());
}

TEST(IdcTokenOwner, DeniedRefusesAcquisition) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Denied, 0, cancel());
  auto made = idc::IdcTokenOwner::create(target(), secret("access-key"), secret("secret-key"),
      secret(""), deadline(), cancel());
  ASSERT_TRUE(made.owner);
  auto owner = std::move(made.owner);
  auto result = owner->acquire();
  ASSERT_FALSE(result);
  EXPECT_EQ(a::FieldFailure::InvalidShape, result.error().failure);
  EXPECT_TRUE(owner->close());
}

TEST(IdcTokenOwner, InvalidTargetSourceAndDeadlineRefuseCreate) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, cancel());
  auto bad_target = idc::IdcTokenOwner::create({"eu-north-1", ""}, secret("access-key"),
      secret("secret-key"), secret(""), deadline(), cancel());
  ASSERT_FALSE(bad_target.owner);
  EXPECT_EQ(idc::OwnerFailure::InvalidTarget, *bad_target.failure);
  auto bad_source = idc::IdcTokenOwner::create(target(), secret(""), secret("secret-key"),
      secret(""), deadline(), cancel());
  ASSERT_FALSE(bad_source.owner);
  EXPECT_EQ(idc::OwnerFailure::InvalidSource, *bad_source.failure);
  auto expired = idc::IdcTokenOwner::create(target(), secret("access-key"), secret("secret-key"),
      secret(""), rs::util::Clock::now() - std::chrono::seconds{1}, cancel());
  ASSERT_FALSE(expired.owner);
  EXPECT_EQ(idc::OwnerFailure::DeadlineElapsed, *expired.failure);
}

TEST(IdcTokenOwner, AllocationFailureInitFixtureRefusesCleanly) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, cancel());
  auto made = idc::IdcTokenOwnerFixture::create_init_failure(idc::OwnerInitFixture::AllocationFailure);
  ASSERT_FALSE(made.owner);
  EXPECT_EQ(idc::OwnerFailure::LocalFailure, *made.failure);
}
