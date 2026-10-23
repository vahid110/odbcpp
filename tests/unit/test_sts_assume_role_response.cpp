#include <type_traits>
#include "odbcpp/auth/sts_assume_role_response.h"
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>

namespace {
using namespace rs::core::auth;
using rs::util::Clock;
using rs::util::Deadline;
using namespace std::chrono_literals;
Deadline at(long long s) { return Deadline{std::chrono::seconds{s}}; }
StsAssumeRoleTarget target(StsAssumeRolePartition part = StsAssumeRolePartition::Aws) {
  return {part, "123456789012", "us-east-1", "MyRole", "session", 900};
}
StsAssumeRoleAcquisition acquisition(Deadline d = at(150), Clock::duration h = 6s,
    StsAssumeRoleTarget t = target()) {
  auto a = StsAssumeRoleAcquisition::create(d, h, std::move(t));
  if (!a) { throw std::runtime_error("bad test acquisition"); }
  return std::move(a).value();
}
SecretBytes secret(std::string value) {
  auto s = SecretBytes::create(std::as_bytes(std::span{value.data(), value.size()}));
  if (!s) { throw std::runtime_error("bad test secret"); }
  return std::move(s).value();
}
StsAssumeRoleResponse response(std::optional<std::int64_t> expiry = 1060000) {
  return {secret("ASIA_SENTINEL"), secret("SECRET_SENTINEL"), secret("TOKEN_SENTINEL"), expiry,
      "arn:aws:sts::123456789012:assumed-role/MyRole/session"};
}
StsAssumeRoleResponseUtc utc_response(std::optional<UtcInstant> expiry = UtcInstant{1060000001}) {
  return {secret("ASIA_SENTINEL"), secret("SECRET_SENTINEL"), secret("TOKEN_SENTINEL"), expiry,
      "arn:aws:sts::123456789012:assumed-role/MyRole/session"};
}
TemporaryClockSample sample() { return {{1000000000}, at(100), at(102), at(103), at(103), 3s, 0us, 0us, at(99)}; }
TemporaryTimePolicy policy() { return {10s, 2s, 10s, 3600s}; }
void failure(const StsOutcome<StsAssumedCredential>& r, StsAssumeRoleFailure expected) {
  ASSERT_FALSE(r);
  EXPECT_EQ(expected, r.error().failure);
  EXPECT_EQ(std::string_view::npos, r.error().safe_message().find("SECRET_SENTINEL"));
  EXPECT_EQ(std::string_view::npos, r.error().safe_message().find("ASIA_SENTINEL"));
  EXPECT_EQ(std::string_view::npos, r.error().safe_message().find("TOKEN_SENTINEL"));
}
TEST(StsAssumeRoleResponseTest, OperationAndLocalSyntaxDoNotFabricateServiceAuthority) {
  EXPECT_EQ("AssumeRole", StsAssumeRoleAcquisition::operation);
  auto a = acquisition();
  auto retained = std::move(a);
  EXPECT_FALSE(a.matches(retained));
  EXPECT_TRUE(retained.matches(acquisition()));
  EXPECT_FALSE(retained.matches(acquisition(at(149))));
  EXPECT_FALSE(retained.matches(acquisition(at(150), 5s)));
  auto t = target();
  t.role_name = "ForeignRole";
  EXPECT_FALSE(retained.matches(acquisition(at(150), 6s, std::move(t))));
}
TEST(StsAssumeRoleResponseTest, ProjectionOwnsCredentialAndConservativeValidity) {
  auto a = acquisition();
  auto input = response();
  auto r = project_sts_assume_role_response(a, std::move(input), sample(), policy());
  ASSERT_TRUE(r);
  EXPECT_TRUE(input.access_key_id.empty());
  EXPECT_EQ(at(103), r.value().validity.issued_at);
  EXPECT_EQ(at(157), r.value().validity.expires_at);
  r.value().secret_access_key.with_bytes([](auto bytes) {
    EXPECT_EQ("SECRET_SENTINEL", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  });
  r.value().access_key_id.with_bytes([](auto bytes) {
    EXPECT_EQ("ASIA_SENTINEL", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  });
  r.value().session_token.with_bytes([](auto bytes) {
    EXPECT_EQ("TOKEN_SENTINEL", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  });
  EXPECT_EQ("arn:aws:sts::123456789012:assumed-role/MyRole/session", r.value().assumed_role_arn);
}
TEST(StsAssumeRoleResponseTest, TargetRejectsForeignComponentsAndLocalBounds) {
  const StsAssumeRolePartition parts[] = {StsAssumeRolePartition::Aws, StsAssumeRolePartition::AwsCn, StsAssumeRolePartition::AwsUsGov};
  for (auto part : parts) {
    EXPECT_TRUE(StsAssumeRoleAcquisition::create(at(150), 6s, target(part)));
  }
  auto bad_partition = target();
  bad_partition.partition = static_cast<StsAssumeRolePartition>(99);
  auto r = StsAssumeRoleAcquisition::create(at(150), 6s, bad_partition);
  ASSERT_FALSE(r);
  EXPECT_EQ(StsAssumeRoleFailure::UnsupportedPartition, r.error().failure);
  for (int field = 0; field != 5; ++field) {
    auto t = target();
    if (field == 0) { t.account = "12345678901x"; }
    if (field == 1) { t.region = "us--east-1"; }
    if (field == 2) { t.role_name = ""; }
    if (field == 3) { t.role_name = "a//b"; }
    if (field == 4) { t.role_session_name = "x"; }
    EXPECT_FALSE(StsAssumeRoleAcquisition::create(at(150), 6s, t));
  }
}
TEST(StsAssumeRoleResponseTest, RoleNameWithPathIsAccepted){
  auto t = target();
  t.role_name = "odbcpp-redshift-test/odbcpp-redshift-test-runtime";
  EXPECT_TRUE(StsAssumeRoleAcquisition::create(at(150), 6s, t));
}
TEST(StsAssumeRoleResponseTest, DurationAndHeadroomBoundsAreClosed) {
  for (unsigned duration : {900u, 3600u}) {
    auto t = target();
    t.duration_seconds = duration;
    EXPECT_TRUE(StsAssumeRoleAcquisition::create(at(150), 6s, t));
  }
  for (unsigned duration : {0u, 899u, 3601u, (std::numeric_limits<unsigned>::max)()}) {
    auto t = target();
    t.duration_seconds = duration;
    EXPECT_FALSE(StsAssumeRoleAcquisition::create(at(150), 6s, t));
  }
  EXPECT_FALSE(StsAssumeRoleAcquisition::create(Deadline::min(), 6s, target()));
  EXPECT_FALSE(StsAssumeRoleAcquisition::create(Deadline::max(), 6s, target()));
  EXPECT_FALSE(StsAssumeRoleAcquisition::create(at(150), -1s, target()));
  EXPECT_FALSE(StsAssumeRoleAcquisition::create(at(150), 25h, target()));
}
TEST(StsAssumeRoleResponseTest, MissingEmptyAndNulSecretsCannotPublish) {
  auto a = acquisition();
  auto input = response();
  input.access_key_id = secret("");
  failure(project_sts_assume_role_response(a, std::move(input), sample(), policy()), StsAssumeRoleFailure::InvalidResponse);
  input = response();
  input.secret_access_key = secret(std::string("a\0b", 3));
  failure(project_sts_assume_role_response(a, std::move(input), sample(), policy()), StsAssumeRoleFailure::InvalidResponse);
  input = response();
  input.session_token = secret("");
  failure(project_sts_assume_role_response(a, std::move(input), sample(), policy()), StsAssumeRoleFailure::InvalidResponse);
  input = response();
  auto kept = std::move(input);
  failure(project_sts_assume_role_response(a, std::move(input), sample(), policy()), StsAssumeRoleFailure::InvalidResponse);
  EXPECT_TRUE(project_sts_assume_role_response(a, std::move(kept), sample(), policy()));
  input = response();
  input.assumed_role_arn = "";
  failure(project_sts_assume_role_response(a, std::move(input), sample(), policy()), StsAssumeRoleFailure::InvalidResponse);
}
TEST(StsAssumeRoleResponseTest, MissingNonpositiveAndOverflowExpiryConsumeSecrets) {
  const std::optional<std::int64_t> values[] = {std::nullopt, 0, -1, (std::numeric_limits<std::int64_t>::max)()};
  for (auto expiry : values) {
    auto input = response(expiry);
    failure(project_sts_assume_role_response(acquisition(), std::move(input), sample(), policy()), StsAssumeRoleFailure::ExpiryRange);
    EXPECT_TRUE(input.access_key_id.empty());
  }
}
TEST(StsAssumeRoleResponseTest, OriginalDeadlineAndQualityRemainClosed) {
  auto r = project_sts_assume_role_response(acquisition(at(150), 7s), response(), sample(), policy());
  failure(r, StsAssumeRoleFailure::TimeConversionFailed);
  EXPECT_EQ(TimeFailure::Expired, r.error().time_failure);
  auto s = sample();
  s.utc_uncertainty.reset();
  auto unknown = project_sts_assume_role_response(acquisition(), response(), s, policy());
  failure(unknown, StsAssumeRoleFailure::TimeConversionFailed);
  EXPECT_EQ(TimeFailure::UnknownClockQuality, unknown.error().time_failure);
  s = sample();
  s.acquisition_completed = at(101);
  auto rollback = project_sts_assume_role_response(acquisition(), response(), s, policy());
  ASSERT_FALSE(rollback);
  EXPECT_EQ(TimeFailure::ClockRollback, rollback.error().time_failure);
  s = sample();
  s.now = at(111);
  auto stale = project_sts_assume_role_response(acquisition(), response(), s, policy());
  ASSERT_FALSE(stale);
  EXPECT_EQ(TimeFailure::StaleSample, stale.error().time_failure);
}
struct Stop : Cancellation { bool stopped = false; bool stop_requested() const noexcept override { return stopped; } };
TEST(StsAssumeRoleResponseTest, CancellationConsumesBeforeEarlyValidation) {
  Stop stop;
  stop.stopped = true;
  auto input = response();
  auto a = acquisition();
  auto kept = std::move(a);
  failure(project_sts_assume_role_response(a, std::move(input), sample(), policy(), &stop), StsAssumeRoleFailure::Cancelled);
  EXPECT_TRUE(input.access_key_id.empty());
  EXPECT_FALSE(kept.invariant_error());
}
TEST(StsAssumeRoleResponseTest, UtcTypesHaveNoImplicitMillisecondOrOperationConversions) {
  static_assert(!std::is_convertible_v<StsAssumeRoleResponse, StsAssumeRoleResponseUtc>);
  static_assert(!std::is_convertible_v<StsAssumeRoleResponseUtc, StsAssumeRoleResponse>);
  static_assert(!std::is_copy_constructible_v<StsAssumeRoleResponse>);
  static_assert(!std::is_copy_constructible_v<StsAssumeRoleResponseUtc>);
  static_assert(!std::is_copy_constructible_v<StsAssumedCredential>);
}
TEST(StsAssumeRoleResponseTest, ExactOneMicrosecondChangesExclusiveBoundaryWithoutRounding) {
  auto a = acquisition(at(150), 7s);
  auto utc = project_sts_assume_role_response_utc(a, utc_response(), sample(), policy());
  ASSERT_TRUE(utc);
  EXPECT_EQ(at(157) + 1us, utc.value().validity.expires_at);
  auto ms = project_sts_assume_role_response(a, response(), sample(), policy());
  failure(ms, StsAssumeRoleFailure::TimeConversionFailed);
  EXPECT_EQ(TimeFailure::Expired, ms.error().time_failure);
}
TEST(StsAssumeRoleResponseTest, MillisecondCompatibilityAndCheckedRangeStayUnchanged) {
  auto a = acquisition();
  auto ms = project_sts_assume_role_response(a, response(), sample(), policy());
  auto utc = project_sts_assume_role_response_utc(a, utc_response(UtcInstant{1060000000}), sample(), policy());
  ASSERT_TRUE(ms);
  ASSERT_TRUE(utc);
  EXPECT_EQ(ms.value().validity.expires_at, utc.value().validity.expires_at);
  const auto maximum = (std::numeric_limits<std::int64_t>::max)();
  auto edge_ms = project_sts_assume_role_response(a, response(maximum / 1000), sample(), policy());
  auto edge_utc = project_sts_assume_role_response_utc(a, utc_response(UtcInstant{(maximum / 1000) * 1000}), sample(), policy());
  failure(edge_ms, StsAssumeRoleFailure::TimeConversionFailed);
  failure(edge_utc, StsAssumeRoleFailure::TimeConversionFailed);
  failure(project_sts_assume_role_response(a, response(maximum / 1000 + 1), sample(), policy()), StsAssumeRoleFailure::ExpiryRange);
  const std::optional<UtcInstant> bad_utc[] = {std::optional<UtcInstant>{}, std::optional<UtcInstant>{UtcInstant{0}}, std::optional<UtcInstant>{UtcInstant{-1}}};
  for (auto expiry : bad_utc) {
    auto input = utc_response(expiry);
    failure(project_sts_assume_role_response_utc(a, std::move(input), sample(), policy()), StsAssumeRoleFailure::ExpiryRange);
    EXPECT_TRUE(input.access_key_id.empty());
  }
}
TEST(StsAssumeRoleResponseTest, MultifaultPrecedenceAndEarlyOwnershipAreConsistent) {
  auto a = acquisition();
  auto invalid = acquisition();
  auto kept = std::move(invalid);
  failure(project_sts_assume_role_response(invalid, response((std::numeric_limits<std::int64_t>::max)()), sample(), policy()), StsAssumeRoleFailure::InvalidAcquisition);
  failure(project_sts_assume_role_response_utc(invalid, utc_response(UtcInstant{1060000000}), sample(), policy()), StsAssumeRoleFailure::InvalidAcquisition);
  failure(project_sts_assume_role_response(kept, response(std::nullopt), sample(), policy()), StsAssumeRoleFailure::ExpiryRange);
  failure(project_sts_assume_role_response_utc(kept, utc_response(std::nullopt), sample(), policy()), StsAssumeRoleFailure::ExpiryRange);
}
TEST(StsAssumeRoleResponseTest, SafeErrorsAreClosedWithoutSourcePayloads) {
  const StsAssumeRoleFailure all[] = {StsAssumeRoleFailure::UnsupportedPartition, StsAssumeRoleFailure::InvalidAcquisition,
      StsAssumeRoleFailure::InvalidResponse, StsAssumeRoleFailure::ExpiryRange,
      StsAssumeRoleFailure::TimeConversionFailed, StsAssumeRoleFailure::Cancelled,
      StsAssumeRoleFailure::AllocationFailed};
  for (auto f : all) {
    StsAssumeRoleError e{f};
    EXPECT_FALSE(e.safe_message().empty());
    EXPECT_EQ(std::string_view::npos, e.safe_message().find("SECRET_SENTINEL"));
  }
}
} // namespace
