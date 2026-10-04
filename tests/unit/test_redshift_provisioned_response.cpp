#include <type_traits>
#include "core/auth/redshift_withiam_response.h"
#include "core/auth/redshift_serverless_response.h"
#include "core/auth/redshift_provisioned_response.h"
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>

namespace {
using namespace rs::core::auth;
using rs::util::Clock;
using rs::util::Deadline;
using namespace std::chrono_literals;
Deadline at(long long s) { return Deadline{std::chrono::seconds{s}}; }
std::string arn(std::string part = "aws") { return "arn:" + part + ":redshift:eu-north-1:123456789012:cluster:exact-cluster"; }
ProvisionedTarget target(ProvisionedPartition part = ProvisionedPartition::Aws) {
  return {part, "123456789012", "eu-north-1", "exact-cluster", "pilot", "alice", false, std::nullopt, 900};
}
Request request(std::string resource = arn(), std::string generation = "generation", Deadline d = at(150), Clock::duration h = 6s,
                Service service = Service::Redshift, Method method = Method::TemporaryDatabasePassword) {
  auto b = Binding::create({service, "db.example", 5439, "pilot", "IAM:alice", std::move(resource), "db.example", "trust"},
      {method == Method::TemporaryDatabasePassword ? SourceKind::TrustedTemporaryDbIssuer : SourceKind::ExternalPassword,
       "source", std::move(generation)}, method);
  if (!b) throw std::runtime_error("bad test binding");
  auto r = Request::create(std::move(b).value(), d, h);
  if (!r) throw std::runtime_error("bad test request");
  return std::move(r).value();
}
ProvisionedAcquisition acquisition(Request r = request(), ProvisionedTarget t = target()) {
  auto a = ProvisionedAcquisition::create(std::move(r), std::move(t));
  if (!a) throw std::runtime_error("bad test acquisition");
  return std::move(a).value();
}
ProvisionedResponse response(std::string user = "IAM:alice", std::optional<std::int64_t> expiry = 1060000,
                            std::string password = "SECRET_SENTINEL") {
  auto secret = SecretBytes::create(std::as_bytes(std::span{password.data(), password.size()}));
  if (!secret) throw std::runtime_error("bad test secret");
  return {std::move(user), std::move(secret).value(), expiry};
}
TemporaryClockSample sample() { return {{1000000000}, at(100), at(102), at(103), at(103), 3s, 0us, 0us, at(99)}; }
TemporaryTimePolicy policy() { return {10s, 2s, 10s, 3600s}; }
void failure(const ProvisionedOutcome<Material>& r, ProvisionedFailure expected) {
  ASSERT_FALSE(r); EXPECT_EQ(expected, r.error().failure);
  EXPECT_EQ(std::string_view::npos, r.error().safe_message().find("SECRET_SENTINEL"));
  EXPECT_EQ(std::string_view::npos, r.error().safe_message().find("IAM:alice"));
}
TEST(ProvisionedResponseTest, OperationAndLocalSyntaxDoNotFabricateServiceAuthority) {
  EXPECT_EQ("GetClusterCredentials", ProvisionedAcquisition::operation);
  // SELECT is a reserved SQL word. This pure local syntax slice does not claim
  // full AWS reserved-word acceptance: the eventual issuer must validate it.
  auto req = request();
  auto b = Binding::create({Service::Redshift, "db.example", 5439, "pilot", "IAM:SELECT", arn(), "db.example", "trust"},
      {SourceKind::TrustedTemporaryDbIssuer, "source", "generation"}, Method::TemporaryDatabasePassword);
  ASSERT_TRUE(b); auto named = Request::create(std::move(b).value(), req.deadline(), req.headroom()); ASSERT_TRUE(named);
  auto t = target(); t.requested_user = "SELECT";
  EXPECT_TRUE(ProvisionedAcquisition::create(std::move(named).value(), t));
  EXPECT_FALSE(acquisition().matches(request(arn(), "foreign-generation")));
}
TEST(ProvisionedResponseTest, ExactProjectionOwnsMaterialAndConservativeValidity) {
  auto a = acquisition(); auto input = response();
  auto r = project_provisioned_response(a, std::move(input), sample(), policy());
  ASSERT_TRUE(r); EXPECT_TRUE(input.password.empty());
  EXPECT_EQ(at(103), r.value().validity().issued_at); EXPECT_EQ(at(157), r.value().validity().expires_at);
  input.user = "changed"; input.expiration_epoch_milliseconds = 1;
  r.value().with_secret([](auto bytes) {
    EXPECT_EQ("SECRET_SENTINEL", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  });
  EXPECT_EQ("IAM:alice", r.value().principal()); EXPECT_EQ(arn(), r.value().binding().target().resource_id);
}
TEST(ProvisionedResponseTest, FullArnRejectsEveryForeignTupleAndExtraGrammar) {
  const std::string bad[] = {"arn:aws:redshift-serverless:eu-north-1:123456789012:workgroup/exact-cluster", "exact-cluster", "arn:aws:other:eu-north-1:123456789012:cluster:exact-cluster",
    "arn:aws-cn:redshift:eu-north-1:123456789012:cluster:exact-cluster",
    "arn:aws:redshift:eu-west-1:123456789012:cluster:exact-cluster",
    "arn:aws:redshift:eu-north-1:123456789013:cluster:exact-cluster",
    "arn:aws:redshift:eu-north-1:123456789012:namespace:exact-cluster", arn()+"/tail", arn()+":tail",
    "arn:aws:redshift:eu-north-1:123456789012:cluster:*"};
  for (const auto& value : bad) { auto r = ProvisionedAcquisition::create(request(value), target());
    ASSERT_FALSE(r); EXPECT_EQ(ProvisionedFailure::InvalidAcquisition, r.error().failure); }
  for (int field = 0; field != 5; ++field) {
    auto t = target();
    if (field == 0) t.account = "123456789013";
    if (field == 1) t.region = "eu-west-1";
    if (field == 2) t.cluster_identifier = "foreign-cluster";
    if (field == 3) t.partition = ProvisionedPartition::AwsCn;
    if (field == 4) t.database = "foreign";
    EXPECT_FALSE(ProvisionedAcquisition::create(request(), t));
  }
}
TEST(ProvisionedResponseTest, ClosedPartitionRepresentationDoesNotInferAvailability) {
  const ProvisionedPartition parts[] = {ProvisionedPartition::Aws, ProvisionedPartition::AwsCn, ProvisionedPartition::AwsUsGov};
  const std::string texts[] = {"aws", "aws-cn", "aws-us-gov"};
  for (int i = 0; i != 3; ++i) EXPECT_TRUE(ProvisionedAcquisition::create(request(arn(texts[i])), target(parts[i])));
  auto t = target(); t.partition = static_cast<ProvisionedPartition>(99);
  auto r = ProvisionedAcquisition::create(request(), t); ASSERT_FALSE(r);
  EXPECT_EQ(ProvisionedFailure::UnsupportedPartition, r.error().failure);
}
TEST(ProvisionedResponseTest, DurationBoundsAndExactRequestedToReturnedPrincipal) {
  for (unsigned duration : {900u, 3600u}) { auto t = target(); t.duration_seconds = duration;
    EXPECT_TRUE(ProvisionedAcquisition::create(request(), t)); }
  for (unsigned duration : {0u, 899u, 3601u, (std::numeric_limits<unsigned>::max)()}) {
    auto t = target(); t.duration_seconds = duration; EXPECT_FALSE(ProvisionedAcquisition::create(request(), t)); }
  auto t = target(); t.requested_user = "Alice";
  EXPECT_FALSE(ProvisionedAcquisition::create(request(), t)); // IAM:Alice is not IAM:alice.
  for (std::string name : std::vector<std::string>{"", "1alice", "a:b", "a/b", "PUBLIC", "pUbLiC", std::string(65, 'a'), std::string("ab\0c", 4)}) {
    t = target(); t.requested_user = name; EXPECT_FALSE(ProvisionedAcquisition::create(request(), t)); }
}
TEST(ProvisionedResponseTest, CreationAndGroupsMustBeExplicitlySafeOmissionPolicy) {
  for (int mode = 0; mode != 4; ++mode) {
    auto t = target();
    if (mode == 0) t.auto_create.reset();
    if (mode == 1) t.auto_create = true;
    if (mode == 2) t.groups = std::vector<std::string>{};
    if (mode == 3) t.groups = std::vector<std::string>{"group"};
    auto result = ProvisionedAcquisition::create(request(), t); ASSERT_FALSE(result);
    EXPECT_EQ(ProvisionedFailure::UnsupportedPolicy, result.error().failure);
  }
  EXPECT_TRUE(ProvisionedAcquisition::create(request(), target()));
}
TEST(ProvisionedResponseTest, DelimiterAndLocalBoundsNeverBecomeArnInjection) {
  for (int field = 0; field != 6; ++field) { auto t = target();
    if (field == 0) t.account = "12345678901x";
    if (field == 1) t.region = "eu--north-1";
    if (field == 2) t.region = "eu:north-1";
    if (field == 3) t.cluster_identifier = "id/tail";
    if (field == 4) t.cluster_identifier = std::string(64, 'a');
    if (field == 5) t.cluster_identifier = std::string("id\0tail", 7);
    EXPECT_FALSE(ProvisionedAcquisition::create(request(), t)); }
}
TEST(ProvisionedResponseTest, OrdinaryAndForeignServiceAreNotProvisionedMethods) {
  auto r = ProvisionedAcquisition::create(request(arn(), "generation", at(150), 6s, Service::Rds, Method::OrdinaryPassword), target());
  ASSERT_FALSE(r); EXPECT_EQ(ProvisionedFailure::UnsupportedMethod, r.error().failure);
  auto ordinary = ProvisionedAcquisition::create(request(arn(), "generation", at(150), 6s, Service::Redshift, Method::OrdinaryPassword), target());
  EXPECT_FALSE(ordinary);
}
TEST(ProvisionedResponseTest, MovedFromAcquisitionAndRequestAreClosedAndConsumeResponse) {
  auto original = acquisition(); auto retained = std::move(original); auto input = response();
  auto r = project_provisioned_response(original, std::move(input), sample(), policy());
  failure(r, ProvisionedFailure::InvalidAcquisition); EXPECT_TRUE(input.password.empty());
  EXPECT_TRUE(project_provisioned_response(retained, response(), sample(), policy()));
  auto req = request(); auto owner = std::move(req);
  EXPECT_FALSE(ProvisionedAcquisition::create(req, target())); EXPECT_FALSE(retained.matches(req));
  EXPECT_TRUE(retained.matches(owner));
}
TEST(ProvisionedResponseTest, MissingWrongPrincipalEmptyAndMovedSecretCannotPublish) {
  for (std::string user : std::vector<std::string>{"", "alice", "IAMA:alice", "IAM:Alice", "foreign", std::string("IAM:alice\0", 10), std::string("\xff", 1)}) {
    auto input = response(user); auto r = project_provisioned_response(acquisition(), std::move(input), sample(), policy());
    ASSERT_FALSE(r); EXPECT_TRUE(input.password.empty());
  }
  auto input = response("IAM:alice", 1060000, "");
  failure(project_provisioned_response(acquisition(), std::move(input), sample(), policy()), ProvisionedFailure::InvalidResponse);
  input = response(); auto kept = std::move(input);
  failure(project_provisioned_response(acquisition(), std::move(input), sample(), policy()), ProvisionedFailure::InvalidResponse);
  EXPECT_TRUE(project_provisioned_response(acquisition(), std::move(kept), sample(), policy()));
  input = response("IAM:alice", 1060000, std::string("a\0b", 3));
  failure(project_provisioned_response(acquisition(), std::move(input), sample(), policy()), ProvisionedFailure::InvalidResponse);
}
TEST(ProvisionedResponseTest, MissingNonpositiveAndOverflowExpiryConsumePassword) {
  const std::optional<std::int64_t> values[] = {std::nullopt, 0, -1, (std::numeric_limits<std::int64_t>::max)()};
  for (auto expiry : values) { auto input = response("IAM:alice", expiry);
    failure(project_provisioned_response(acquisition(), std::move(input), sample(), policy()), ProvisionedFailure::ExpiryRange);
    EXPECT_TRUE(input.password.empty()); }
}
TEST(ProvisionedResponseTest, OriginalDeadlineAndQualityRemainClosed) {
  auto r = project_provisioned_response(acquisition(request(arn(), "generation", at(150), 7s)), response(), sample(), policy());
  failure(r, ProvisionedFailure::TimeConversionFailed); EXPECT_EQ(TimeFailure::Expired, r.error().time_failure);
  auto s = sample(); s.utc_uncertainty.reset();
  auto unknown = project_provisioned_response(acquisition(), response(), s, policy());
  failure(unknown, ProvisionedFailure::TimeConversionFailed); EXPECT_EQ(TimeFailure::UnknownClockQuality, unknown.error().time_failure);
  s = sample(); s.acquisition_completed = at(101);
  auto rollback = project_provisioned_response(acquisition(), response(), s, policy());
  ASSERT_FALSE(rollback); EXPECT_EQ(TimeFailure::ClockRollback, rollback.error().time_failure);
  s = sample(); s.now = at(111);
  auto stale = project_provisioned_response(acquisition(), response(), s, policy());
  ASSERT_FALSE(stale); EXPECT_EQ(TimeFailure::StaleSample, stale.error().time_failure);
}
struct Stop : Cancellation { bool stopped = false; bool stop_requested() const noexcept override { return stopped; } };
TEST(ProvisionedResponseTest, CancellationConsumesBeforeEarlyValidation) {
  Stop stop; stop.stopped = true; auto input = response(); auto a = acquisition(); auto kept = std::move(a);
  failure(project_provisioned_response(a, std::move(input), sample(), policy(), &stop), ProvisionedFailure::Cancelled);
  EXPECT_TRUE(input.password.empty()); EXPECT_FALSE(kept.invariant_error());
}
struct FakeClock : MonotonicClock { Deadline value = at(103); Deadline now() noexcept override { return value; } };
// Explicitly selected fixed-operation issuer seam; Request and untagged response
// fields cannot prove an AWS operation or authenticated response pairing.
struct GetClusterCredentialsTestIssuer : TrustedIssuer {
  static constexpr std::string_view operation = ProvisionedAcquisition::operation;
  ProvisionedAcquisition context = acquisition(); int calls = 0; Stop* stop_during = nullptr;
  Outcome<Material> acquire(const Request& req, const Cancellation* cancel) override {
    ++calls;
    if (!context.matches(req)) return Error{Reason::TargetMismatch};
    auto candidate = project_provisioned_response(context, response(), sample(), policy(), cancel);
    if (stop_during) stop_during->stopped = true;
    if (!candidate) return Error{Reason::InvalidMaterial};
    return std::move(candidate).value();
  }
};
TEST(ProvisionedResponseTest, ExplicitAuthorityBridgePublishesOnlyExactRequestAndSingleUseReceipt) {
  GetClusterCredentialsTestIssuer issuer; FakeClock clock; auto req = request();
  EXPECT_EQ("GetClusterCredentials", issuer.operation);
  auto a = Authority::create(req.binding(), issuer, clock); ASSERT_TRUE(a);
  auto receipt = a.value()->acquire(req); ASSERT_TRUE(receipt); EXPECT_EQ(1, issuer.calls);
  auto material = a.value()->take(std::move(receipt).value()); ASSERT_TRUE(material);
  EXPECT_EQ(at(157), material.value().validity().expires_at);
  EXPECT_FALSE(a.value()->take(std::move(receipt).value()));
  EXPECT_FALSE(a.value()->acquire(request(arn(), "changed"))); EXPECT_EQ(1, issuer.calls);
  EXPECT_FALSE(issuer.acquire(request(arn(), "generation", at(149)), nullptr));
  EXPECT_FALSE(issuer.acquire(request(arn(), "generation", at(150), 5s), nullptr));
  EXPECT_FALSE(issuer.acquire(request(arn(), "different-generation"), nullptr));
}
TEST(ProvisionedResponseTest, ForeignAuthorityRollbackLateAndCancelledBridgeNeverPublish) {
  GetClusterCredentialsTestIssuer issuer; FakeClock clock; Stop stop; auto req = request();
  auto a = Authority::create(req.binding(), issuer, clock); auto b = Authority::create(req.binding(), issuer, clock);
  ASSERT_TRUE(a); ASSERT_TRUE(b);
  auto receipt = a.value()->acquire(req); ASSERT_TRUE(receipt);
  auto foreign = b.value()->take(std::move(receipt).value()); ASSERT_FALSE(foreign);
  EXPECT_EQ(Reason::UntrustedReceipt, foreign.error().reason());
  auto receipt_rollback = a.value()->acquire(req); ASSERT_TRUE(receipt_rollback); clock.value = at(102);
  auto rollback = a.value()->take(std::move(receipt_rollback).value()); ASSERT_FALSE(rollback);
  EXPECT_EQ(Reason::ClockRollback, rollback.error().reason());
  clock.value = at(103); issuer.stop_during = &stop;
  auto cancelled = a.value()->acquire(req, &stop); ASSERT_FALSE(cancelled);
  EXPECT_EQ(Reason::Cancelled, cancelled.error().reason());
  stop.stopped = false; issuer.stop_during = nullptr;
  auto late_receipt = a.value()->acquire(req); ASSERT_TRUE(late_receipt); clock.value = at(150);
  EXPECT_FALSE(a.value()->take(std::move(late_receipt).value()));
}
TEST(ProvisionedResponseTest, SafeErrorsAreClosedWithoutSourcePayloads) {
  for (auto f : {ProvisionedFailure::UnsupportedMethod, ProvisionedFailure::UnsupportedPartition, ProvisionedFailure::UnsupportedPolicy, ProvisionedFailure::InvalidAcquisition,
      ProvisionedFailure::InvalidResponse, ProvisionedFailure::PrincipalMismatch, ProvisionedFailure::ExpiryRange,
      ProvisionedFailure::TimeConversionFailed, ProvisionedFailure::Cancelled, ProvisionedFailure::AllocationFailed}) {
    ProvisionedError e{f}; EXPECT_FALSE(e.safe_message().empty());
    EXPECT_EQ(std::string_view::npos, e.safe_message().find("SECRET_SENTINEL"));
  }
}

ProvisionedResponseUtc utc_response(std::optional<UtcInstant> expiry = UtcInstant{1060000001},
    std::string user = "IAM:alice", std::string password = "SECRET_SENTINEL") {
  auto old = response(std::move(user), 1060000, std::move(password));
  return {std::move(old.user), std::move(old.password), expiry};
}
TEST(ProvisionedResponseTest, UtcTypesHaveNoImplicitMillisecondOrOperationConversions) {
  static_assert(!std::is_convertible_v<ProvisionedResponse, ProvisionedResponseUtc>);
  static_assert(!std::is_convertible_v<ProvisionedResponseUtc, ProvisionedResponse>);
  static_assert(!std::is_copy_constructible_v<ProvisionedResponseUtc>);
  static_assert(!std::is_convertible_v<ServerlessResponseUtc, ProvisionedResponseUtc>);
  static_assert(!std::is_convertible_v<ProvisionedResponseUtc, WithIamResponseUtc>);
  static_assert(!std::is_convertible_v<WithIamResponseUtc, ServerlessResponseUtc>);
}
TEST(ProvisionedResponseTest, ExactOneMicrosecondChangesExclusiveBoundaryWithoutRounding) {
  auto a = acquisition(request(arn(), "generation", at(150), 7s));
  auto utc = project_provisioned_response_utc(a, utc_response(), sample(), policy()); ASSERT_TRUE(utc);
  EXPECT_EQ(at(157) + 1us, utc.value().validity().expires_at);
  EXPECT_EQ(at(103), utc.value().validity().issued_at);
  auto ms = project_provisioned_response(a, response(), sample(), policy());
  failure(ms, ProvisionedFailure::TimeConversionFailed); EXPECT_EQ(TimeFailure::Expired, ms.error().time_failure);
  auto equality = acquisition(request(arn(), "generation", at(150), 7s + 1us));
  auto refused = project_provisioned_response_utc(equality, utc_response(), sample(), policy());
  failure(refused, ProvisionedFailure::TimeConversionFailed); EXPECT_EQ(TimeFailure::Expired, refused.error().time_failure);
  auto adjacent = acquisition(request(arn(), "generation", at(150), 7s + 1us - Clock::duration{1}));
  EXPECT_TRUE(project_provisioned_response_utc(adjacent, utc_response(), sample(), policy()));
}
TEST(ProvisionedResponseTest, MillisecondCompatibilityAndCheckedRangeStayUnchanged) {
  auto a = acquisition();
  auto ms = project_provisioned_response(a, response(), sample(), policy());
  auto utc = project_provisioned_response_utc(a, utc_response(UtcInstant{1060000000}), sample(), policy());
  ASSERT_TRUE(ms); ASSERT_TRUE(utc);
  EXPECT_EQ(ms.value().binding().target(), utc.value().binding().target());
  EXPECT_EQ(ms.value().binding().source(), utc.value().binding().source());
  EXPECT_EQ(ms.value().principal(), utc.value().principal());
  EXPECT_EQ(ms.value().validity().issued_at, utc.value().validity().issued_at);
  EXPECT_EQ(ms.value().validity().expires_at, utc.value().validity().expires_at);
  const auto maximum = (std::numeric_limits<std::int64_t>::max)();
  auto edge_ms = project_provisioned_response(a, response("IAM:alice", maximum / 1000), sample(), policy());
  auto edge_utc = project_provisioned_response_utc(a, utc_response(UtcInstant{(maximum / 1000) * 1000}), sample(), policy());
  failure(edge_ms, ProvisionedFailure::TimeConversionFailed); failure(edge_utc, ProvisionedFailure::TimeConversionFailed);
  EXPECT_EQ(edge_ms.error().time_failure, edge_utc.error().time_failure);
  failure(project_provisioned_response(a, response("IAM:alice", maximum / 1000 + 1), sample(), policy()), ProvisionedFailure::ExpiryRange);
  failure(project_provisioned_response_utc(a, utc_response(UtcInstant{maximum}), sample(), policy()), ProvisionedFailure::TimeConversionFailed);
  for (auto expiry : {std::optional<UtcInstant>{}, std::optional<UtcInstant>{UtcInstant{0}}, std::optional<UtcInstant>{UtcInstant{-1}}}) {
    auto input = utc_response(expiry);
    failure(project_provisioned_response_utc(a, std::move(input), sample(), policy()), ProvisionedFailure::ExpiryRange);
    EXPECT_TRUE(input.password.empty());
  }
}
TEST(ProvisionedResponseTest, LegacyAndUtcMultifaultPrecedenceAndEarlyOwnershipAreIdentical) {
  auto a = acquisition(); const auto maximum = (std::numeric_limits<std::int64_t>::max)();
  auto ms_wrong = response("foreign", maximum); auto utc_wrong = utc_response(UtcInstant{maximum}, "foreign");
  failure(project_provisioned_response(a, std::move(ms_wrong), sample(), policy()), ProvisionedFailure::PrincipalMismatch);
  failure(project_provisioned_response_utc(a, std::move(utc_wrong), sample(), policy()), ProvisionedFailure::PrincipalMismatch);
  EXPECT_TRUE(ms_wrong.password.empty()); EXPECT_TRUE(utc_wrong.password.empty());
  auto invalid = acquisition(); auto kept = std::move(invalid);
  failure(project_provisioned_response(invalid, response("IAM:alice", maximum), sample(), policy()), ProvisionedFailure::InvalidAcquisition);
  failure(project_provisioned_response_utc(invalid, utc_response(UtcInstant{maximum}), sample(), policy()), ProvisionedFailure::InvalidAcquisition);
  failure(project_provisioned_response(kept, response("IAM:alice", std::nullopt, ""), sample(), policy()), ProvisionedFailure::InvalidResponse);
  failure(project_provisioned_response_utc(kept, utc_response(std::nullopt, "IAM:alice", ""), sample(), policy()), ProvisionedFailure::InvalidResponse);
  auto invalid_secret = utc_response(UtcInstant{1060000000}, "IAM:alice", std::string("a\0b", 3));
  failure(project_provisioned_response_utc(kept, std::move(invalid_secret), sample(), policy()), ProvisionedFailure::InvalidResponse);
}
struct UtcCountCancel : Cancellation {
  mutable unsigned calls = 0; unsigned stop_at = 99;
  bool stop_requested() const noexcept override { return ++calls >= stop_at; }
};
TEST(ProvisionedResponseTest, DelegationAddsNoCancellationObservationOrChangedErrors) {
  for (unsigned stop_at : {1u, 2u, 3u, 4u, 99u}) {
    UtcCountCancel ms_cancel, utc_cancel; ms_cancel.stop_at = utc_cancel.stop_at = stop_at;
    auto a = acquisition(); auto ms_input = response(); auto utc_input = utc_response(UtcInstant{1060000000});
    auto ms = project_provisioned_response(a, std::move(ms_input), sample(), policy(), &ms_cancel);
    auto utc = project_provisioned_response_utc(a, std::move(utc_input), sample(), policy(), &utc_cancel);
    EXPECT_EQ(static_cast<bool>(ms), static_cast<bool>(utc)); EXPECT_EQ(ms_cancel.calls, utc_cancel.calls);
    EXPECT_EQ(stop_at < 4 ? stop_at : 4u, ms_cancel.calls);
    if (!ms) {
      ASSERT_FALSE(utc); EXPECT_EQ(ms.error().failure, utc.error().failure);
      EXPECT_EQ(ms.error().time_failure, utc.error().time_failure);
    }
    EXPECT_TRUE(ms_input.password.empty()); EXPECT_TRUE(utc_input.password.empty());
  }
  auto invalid = acquisition(); auto retained = std::move(invalid); UtcCountCancel cancel; cancel.stop_at = 1;
  failure(project_provisioned_response_utc(invalid, utc_response(std::nullopt), sample(), policy(), &cancel), ProvisionedFailure::Cancelled);
  EXPECT_FALSE(retained.invariant_error());
}
TEST(ProvisionedResponseTest, UtcMaterialOwnsSecretAndRejectsMovedFromResponse) {
  auto input = utc_response(); auto retained = std::move(input); auto a = acquisition();
  failure(project_provisioned_response_utc(a, std::move(input), sample(), policy()), ProvisionedFailure::InvalidResponse);
  auto output = project_provisioned_response_utc(a, std::move(retained), sample(), policy()); ASSERT_TRUE(output);
  EXPECT_TRUE(retained.password.empty());
  output.value().with_secret([](auto bytes) {
    EXPECT_EQ("SECRET_SENTINEL", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  });
  EXPECT_EQ("IAM:alice", output.value().principal()); EXPECT_EQ(at(157) + 1us, output.value().validity().expires_at);
}
struct UtcIssuer : TrustedIssuer {
  ProvisionedAcquisition context = acquisition(request(arn(), "generation", at(150), 7s));
  unsigned calls = 0;
  Outcome<Material> acquire(const Request& original, const Cancellation* cancel) override {
    ++calls;
    if (!context.matches(original)) return Error{Reason::TargetMismatch};
    // Selected operation-owned candidate; this fake is not authenticated SDK pairing.
    auto result = project_provisioned_response_utc(context, utc_response(), sample(), policy(), cancel);
    if (!result) return Error{Reason::InvalidMaterial};
    return std::move(result).value();
  }
};
TEST(ProvisionedResponseTest, ExplicitUtcIssuerAuthorityKeepsOriginalBindingAndSingleUseAdmission) {
  UtcIssuer issuer; FakeClock clock; auto req = issuer.context.request();
  auto authority = Authority::create(req.binding(), issuer, clock); ASSERT_TRUE(authority);
  auto receipt = authority.value()->acquire(req); ASSERT_TRUE(receipt); EXPECT_EQ(1u, issuer.calls);
  EXPECT_FALSE(authority.value()->acquire(request(arn(), "foreign-generation", at(150), 7s)));
  EXPECT_EQ(1u, issuer.calls);
  auto material = authority.value()->take(std::move(receipt).value()); ASSERT_TRUE(material);
  EXPECT_EQ(at(157) + 1us, material.value().validity().expires_at);
  EXPECT_EQ(req.binding().source(), material.value().binding().source());
  EXPECT_FALSE(authority.value()->take(std::move(receipt).value()));
}
} // namespace
