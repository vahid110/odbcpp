#include <gtest/gtest.h>
#include "odbcpp/auth/aws/sts_assume_role_owner.h"
#include "odbcpp/auth/aws/serverless_credentials_owner.h"
#include "odbcpp/auth/aws/sts_serverless_fake_http.h"
#include <chrono>
#include <cstdlib>
#include <thread>
using namespace std::chrono_literals;
namespace a = rs::core::auth;
namespace sts = a::aws::sts;
namespace sl = a::aws::serverless;
namespace detail = a::aws::detail;
namespace pn = a::aws::provisioned_native;
namespace {
a::SecretBytes secret(std::string_view text) {
  auto made = a::SecretBytes::create(std::as_bytes(std::span{text.data(), text.size()}));
  if (!made) std::terminate();
  return std::move(made).value();
}
rs::util::Deadline deadline_ms(long long ms) { return rs::util::Clock::now() + std::chrono::milliseconds(ms); }
rs::util::Deadline deadline_20s() { return deadline_ms(20000); }
a::StsAssumeRoleTarget sts_target() {
  return {a::StsAssumeRolePartition::Aws, "123456789012", "eu-north-1", "MyRole", "session", 900};
}
sl::ServerlessTarget sl_target() { return {"eu-north-1", "fixture-workgroup", "fixture_db", 900}; }
std::shared_ptr<pn::WorkerCancellation> cancel() { return std::make_shared<pn::WorkerCancellation>(); }

struct StsMade {
  sts::OwnerOutcome outcome;
  std::shared_ptr<pn::WorkerCancellation> cancellation;
};
StsMade make_sts(rs::util::Deadline deadline, std::shared_ptr<pn::WorkerCancellation> c = {}) {
  if (!c) c = cancel();
  return {sts::StsAssumeRoleOwner::create(sts_target(), secret("synthetic-access-key"),
      secret("synthetic-secret-key"), secret(""), deadline, c), std::move(c)};
}
struct SlMade {
  sl::OwnerOutcome outcome;
  std::shared_ptr<pn::WorkerCancellation> cancellation;
};
SlMade make_sl(rs::util::Deadline deadline, std::shared_ptr<pn::WorkerCancellation> c = {}) {
  if (!c) c = cancel();
  return {sl::ServerlessCredentialsOwner::create(sl_target(), secret("synthetic-access-key"),
      secret("synthetic-secret-key"), secret(""), deadline, c), std::move(c)};
}
} // namespace

// R5: a fault-injected State allocation must not leak the process-wide SDK slot.
TEST(StsOwnerInit, AllocationFailureDoesNotLeakSdkSlot) {
  auto refused = sts::StsOwnerFixture::create_init_failure(sts::OwnerInitFixture::AllocationFailure);
  EXPECT_FALSE(refused.owner);
  EXPECT_EQ(sts::OwnerFailure::LocalFailure, refused.failure);
  auto made = make_sts(deadline_20s());
  ASSERT_TRUE(made.outcome.owner);
  EXPECT_TRUE(made.outcome.owner->close());
}

// R4: a throw after InitAPI entry has no proven rollback and must be fatal.
TEST(StsOwnerInit, AfterEntryInitFailureIsFatal) {
  EXPECT_DEATH({ auto refused = sts::StsOwnerFixture::create_init_failure(sts::OwnerInitFixture::AfterEntry); (void)refused; }, ".*");
}

// R2: the approved HTTPS endpoint succeeds with exactly one approved send.
TEST(StsOwnerAcquire, ApprovedEndpointSucceedsWithOneSend) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = make_sts(deadline_20s());
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().session_token.empty());
  EXPECT_EQ(1U, detail::fake_http_observation().sends.load());
  EXPECT_EQ(1U, detail::fake_http_observation().approved.load());
  EXPECT_EQ(1U, detail::fake_http_observation().assume_role.load());
  EXPECT_EQ(0U, detail::fake_http_observation().foreign_scheme.load());
  EXPECT_EQ(0U, detail::fake_http_observation().foreign_authority.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

// R2: a global/service-specific HTTP override set between create and acquire
// cannot redirect the signed request away from the pinned endpoint.
TEST(StsOwnerAcquire, HttpEndpointOverrideCannotRedirectSend) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = make_sts(deadline_20s());
  ASSERT_TRUE(made.outcome.owner);
  // Overrides appear after create; the owner must still pin the endpoint.
  ::setenv("AWS_ENDPOINT_URL", "http://evil.invalid", 1);
  ::setenv("AWS_ENDPOINT_URL_STS", "https://evil.invalid", 1);
  auto result = made.outcome.owner->acquire();
  ::unsetenv("AWS_ENDPOINT_URL");
  ::unsetenv("AWS_ENDPOINT_URL_STS");
  ASSERT_TRUE(result);
  EXPECT_EQ(1U, detail::fake_http_observation().approved.load());
  EXPECT_EQ(0U, detail::fake_http_observation().foreign_scheme.load());
  EXPECT_EQ(0U, detail::fake_http_observation().foreign_authority.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

// R3: an already-expired finite deadline refuses before dispatch (zero sends).
TEST(StsOwnerAcquire, ExpiredAdmissionRefusesWithZeroSends) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = make_sts(deadline_ms(-1000));
  EXPECT_FALSE(made.outcome.owner);
  EXPECT_EQ(sts::OwnerFailure::DeadlineElapsed, made.outcome.failure);
  EXPECT_EQ(0U, detail::fake_http_observation().sends.load());
}

// R3: expiry between create and acquire refuses before dispatch (zero sends).
TEST(StsOwnerAcquire, ExpiryBetweenCreateAndAcquireRefuses) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = make_sts(deadline_ms(30));
  ASSERT_TRUE(made.outcome.owner);
  std::this_thread::sleep_for(60ms);
  auto result = made.outcome.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::StsAssumeRoleFailure::DeadlineElapsed, result.error().failure);
  EXPECT_EQ(0U, detail::fake_http_observation().sends.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

// R3: a deadline that expires during preparation (client construction latency)
// is rejected before invocation with zero sends and no zero transport timeout.
TEST(StsOwnerAcquire, ExpiryDuringPreparationRefusesWithZeroSends) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  detail::fake_http_set_prep_delay(60);
  auto made = make_sts(deadline_ms(20));
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::StsAssumeRoleFailure::DeadlineElapsed, result.error().failure);
  EXPECT_EQ(0U, detail::fake_http_observation().sends.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

// R3: a late successful HTTP completion is not published under the deadline.
TEST(StsOwnerAcquire, LateCompletionRefusesPublication) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::LateCompletion, 60, nullptr);
  auto made = make_sts(deadline_ms(20));
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::StsAssumeRoleFailure::DeadlineElapsed, result.error().failure);
  EXPECT_EQ(1U, detail::fake_http_observation().late_completion.load());
  EXPECT_EQ(1U, detail::fake_http_observation().continuation_denied.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

// R3: cancellation requested during transport refuses publication.
TEST(StsOwnerAcquire, CancellationDuringTransportRefuses) {
  detail::fake_http_observation().reset();
  auto c = cancel();
  detail::fake_http_configure(detail::FakeHttpScenario::CancelDuringTransport, 0, c);
  auto made = make_sts(deadline_20s(), c);
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::StsAssumeRoleFailure::Cancelled, result.error().failure);
  EXPECT_EQ(1U, detail::fake_http_observation().continuation_denied.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

// R1: a denied assumption refuses publication; no credential is issued and no
// fallback dispatch occurs (exactly one send, denied).
TEST(StsOwnerAcquire, DeniedAssumptionRefusesWithoutFallback) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Denied, 0, nullptr);
  auto made = make_sts(deadline_20s());
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::StsAssumeRoleFailure::InvalidResponse, result.error().failure);
  EXPECT_EQ(1U, detail::fake_http_observation().sends.load());
  EXPECT_EQ(1U, detail::fake_http_observation().denied.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

// Serverless: R5 allocation fault does not leak the SDK slot.
TEST(ServerlessOwnerInit, AllocationFailureDoesNotLeakSdkSlot) {
  auto refused = sl::ServerlessOwnerFixture::create_init_failure(sl::OwnerInitFixture::AllocationFailure);
  EXPECT_FALSE(refused.owner);
  EXPECT_EQ(sl::OwnerFailure::LocalFailure, refused.failure);
  auto made = make_sl(deadline_20s());
  ASSERT_TRUE(made.outcome.owner);
  EXPECT_TRUE(made.outcome.owner->close());
}

// Serverless: R4 a throw after InitAPI entry is fatal.
TEST(ServerlessOwnerInit, AfterEntryInitFailureIsFatal) {
  EXPECT_DEATH({ auto refused = sl::ServerlessOwnerFixture::create_init_failure(sl::OwnerInitFixture::AfterEntry); (void)refused; }, ".*");
}

// Serverless: R2 pin + R3 deadline behaviors over the synthetic JSON endpoint.
TEST(ServerlessOwnerAcquire, ApprovedEndpointSucceedsWithOneSend) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = make_sl(deadline_20s());
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  ASSERT_TRUE(result);
  EXPECT_EQ(1U, detail::fake_http_observation().sends.load());
  EXPECT_EQ(1U, detail::fake_http_observation().approved.load());
  EXPECT_EQ(1U, detail::fake_http_observation().get_credentials.load());
  EXPECT_EQ(0U, detail::fake_http_observation().foreign_authority.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

TEST(ServerlessOwnerAcquire, HttpEndpointOverrideCannotRedirectSend) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = make_sl(deadline_20s());
  ASSERT_TRUE(made.outcome.owner);
  ::setenv("AWS_ENDPOINT_URL", "http://evil.invalid", 1);
  ::setenv("AWS_ENDPOINT_URL_REDSHIFT_SERVERLESS", "https://evil.invalid", 1);
  auto result = made.outcome.owner->acquire();
  ::unsetenv("AWS_ENDPOINT_URL");
  ::unsetenv("AWS_ENDPOINT_URL_REDSHIFT_SERVERLESS");
  ASSERT_TRUE(result);
  EXPECT_EQ(1U, detail::fake_http_observation().approved.load());
  EXPECT_EQ(0U, detail::fake_http_observation().foreign_scheme.load());
  EXPECT_EQ(0U, detail::fake_http_observation().foreign_authority.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

TEST(ServerlessOwnerAcquire, ExpiredAdmissionRefusesWithZeroSends) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = make_sl(deadline_ms(-1000));
  EXPECT_FALSE(made.outcome.owner);
  EXPECT_EQ(sl::OwnerFailure::DeadlineElapsed, made.outcome.failure);
  EXPECT_EQ(0U, detail::fake_http_observation().sends.load());
}

// R3: a deadline that expires during preparation is rejected with zero sends.
TEST(ServerlessOwnerAcquire, ExpiryDuringPreparationRefusesWithZeroSends) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  detail::fake_http_set_prep_delay(60);
  auto made = make_sl(deadline_ms(20));
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::FieldFailure::DeadlineElapsed, result.error().failure);
  EXPECT_EQ(0U, detail::fake_http_observation().sends.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

TEST(ServerlessOwnerAcquire, LateCompletionRefusesPublication) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::LateCompletion, 60, nullptr);
  auto made = make_sl(deadline_ms(20));
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::FieldFailure::DeadlineElapsed, result.error().failure);
  EXPECT_EQ(1U, detail::fake_http_observation().late_completion.load());
  EXPECT_EQ(1U, detail::fake_http_observation().continuation_denied.load());
  EXPECT_TRUE(made.outcome.owner->close());
}

TEST(ServerlessOwnerAcquire, CancellationDuringTransportRefuses) {
  detail::fake_http_observation().reset();
  auto c = cancel();
  detail::fake_http_configure(detail::FakeHttpScenario::CancelDuringTransport, 0, c);
  auto made = make_sl(deadline_20s(), c);
  ASSERT_TRUE(made.outcome.owner);
  auto result = made.outcome.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::FieldFailure::Cancelled, result.error().failure);
  EXPECT_EQ(1U, detail::fake_http_observation().continuation_denied.load());
  EXPECT_TRUE(made.outcome.owner->close());
}
