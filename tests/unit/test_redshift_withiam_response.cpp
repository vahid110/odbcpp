#include "odbcpp/auth/redshift_withiam_response.h"
#include "odbcpp/auth/redshift_provisioned_response.h"
#include "odbcpp/auth/redshift_serverless_response.h"
#include <type_traits>
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
WithIamTarget target(WithIamPartition part = WithIamPartition::Aws) {
  return {part, "123456789012", "eu-north-1", "exact-cluster", "pilot", "IAM:alice", 900};
}
Request request(std::string resource = arn(), std::string generation = "generation", Deadline d = at(150), Clock::duration h = 6s,
                Service service = Service::Redshift, Method method = Method::TemporaryDatabasePassword, std::string principal = "IAM:alice") {
  auto b = Binding::create({service, "db.example", 5439, "pilot", std::move(principal), std::move(resource), "db.example", "trust"},
      {method == Method::TemporaryDatabasePassword ? SourceKind::TrustedTemporaryDbIssuer : SourceKind::ExternalPassword,
       "source", std::move(generation)}, method);
  if (!b) throw std::runtime_error("bad test binding");
  auto r = Request::create(std::move(b).value(), d, h);
  if (!r) throw std::runtime_error("bad test request");
  return std::move(r).value();
}
WithIamAcquisition acquisition(Request r = request(), WithIamTarget t = target()) {
  auto a = WithIamAcquisition::create(std::move(r), std::move(t));
  if (!a) throw std::runtime_error("bad test acquisition");
  return std::move(a).value();
}
WithIamResponse response(std::string user = "IAM:alice", std::optional<std::int64_t> expiry = 1060000,
                            std::string password = "SECRET_SENTINEL") {
  auto secret = SecretBytes::create(std::as_bytes(std::span{password.data(), password.size()}));
  if (!secret) throw std::runtime_error("bad test secret");
  return {std::move(user), std::move(secret).value(), expiry};
}
TemporaryClockSample sample() { return {{1000000000}, at(100), at(102), at(103), at(103), 3s, 0us, 0us, at(99)}; }
TemporaryTimePolicy policy() { return {10s, 2s, 10s, 3600s}; }
void failure(const WithIamOutcome<Material>& r, WithIamFailure expected) {
  ASSERT_FALSE(r); EXPECT_EQ(expected, r.error().failure);
  EXPECT_EQ(std::string_view::npos, r.error().safe_message().find("SECRET_SENTINEL"));
  EXPECT_EQ(std::string_view::npos, r.error().safe_message().find("IAM:alice"));
}
TEST(WithIamResponseTest, DedicatedOperationDoesNotFabricateIssuingApiProof) {
  EXPECT_EQ("GetClusterCredentialsWithIAM", WithIamAcquisition::operation);
  EXPECT_NE(ProvisionedAcquisition::operation, WithIamAcquisition::operation);
  static_assert(!std::is_convertible_v<ProvisionedAcquisition, WithIamAcquisition>);
  static_assert(!std::is_convertible_v<ServerlessAcquisition, WithIamAcquisition>);
  static_assert(!std::is_convertible_v<ProvisionedResponse, WithIamResponse>);
  static_assert(!std::is_convertible_v<ServerlessResponse, WithIamResponse>);
  EXPECT_FALSE(acquisition().matches(request(arn(), "foreign-generation")));
  // There is no requested user/creation/group/method-selector slot. Equal-shaped
  // untagged results remain untrusted; the injected issuer owns API pairing.
}
TEST(WithIamResponseTest, ExactUserRoleAndOpaquePrincipalsAreNeverDerivedOrRewritten) {
  for (std::string expected : {"IAM:alice", "IAMR:worker", "Opaque:policy-user"}) {
    auto t = target(); t.expected_principal = expected;
    auto req = request(arn(), "generation", at(150), 6s, Service::Redshift,
                       Method::TemporaryDatabasePassword, expected);
    auto a = acquisition(req, t);
    auto result = project_withiam_response(a, response(expected), sample(), policy());
    ASSERT_TRUE(result); EXPECT_EQ(expected, result.value().principal());
    auto wrong = expected == "IAM:alice" ? "alice" : "IAM:alice";
    failure(project_withiam_response(a, response(wrong), sample(), policy()), WithIamFailure::PrincipalMismatch);
  }
}
TEST(WithIamResponseTest, PrincipalPolicyByteBoundaryAndClosedCoreValidation) {
  const std::string exact(127, 'a'); auto t = target(); t.expected_principal = exact;
  auto req = request(arn(), "generation", at(150), 6s, Service::Redshift, Method::TemporaryDatabasePassword, exact);
  auto allowed = WithIamAcquisition::create(req, t); ASSERT_TRUE(allowed);
  EXPECT_TRUE(project_withiam_response(allowed.value(), response(exact), sample(), policy()));
  t.expected_principal += 'a';
  auto longer = request(arn(), "generation", at(150), 6s, Service::Redshift, Method::TemporaryDatabasePassword, t.expected_principal);
  EXPECT_FALSE(WithIamAcquisition::create(longer, t));
  t = target(); t.expected_principal = "IAMR:alice";
  EXPECT_FALSE(WithIamAcquisition::create(request(), t));
  t.expected_principal.clear(); EXPECT_FALSE(WithIamAcquisition::create(request(), t));
  t.expected_principal = std::string("IAM:alice\0", 10); EXPECT_FALSE(WithIamAcquisition::create(request(), t));
}
TEST(WithIamResponseTest, ExactProjectionOwnsMaterialAndConservativeValidity) {
  auto a = acquisition(); auto input = response();
  auto r = project_withiam_response(a, std::move(input), sample(), policy());
  ASSERT_TRUE(r); EXPECT_TRUE(input.password.empty());
  EXPECT_EQ(at(103), r.value().validity().issued_at); EXPECT_EQ(at(157), r.value().validity().expires_at);
  input.user = "changed"; input.expiration_epoch_milliseconds = 1;
  r.value().with_secret([](auto bytes) {
    EXPECT_EQ("SECRET_SENTINEL", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  });
  EXPECT_EQ("IAM:alice", r.value().principal()); EXPECT_EQ(arn(), r.value().binding().target().resource_id);
}
TEST(WithIamResponseTest, FullArnRejectsEveryForeignTupleAndExtraGrammar) {
  const std::string bad[] = {"arn:aws:redshift-serverless:eu-north-1:123456789012:workgroup/exact-cluster", "exact-cluster", "arn:aws:other:eu-north-1:123456789012:cluster:exact-cluster",
    "arn:aws-cn:redshift:eu-north-1:123456789012:cluster:exact-cluster",
    "arn:aws:redshift:eu-west-1:123456789012:cluster:exact-cluster",
    "arn:aws:redshift:eu-north-1:123456789013:cluster:exact-cluster",
    "arn:aws:redshift:eu-north-1:123456789012:namespace:exact-cluster", arn()+"/tail", arn()+":tail",
    "arn:aws:redshift:eu-north-1:123456789012:cluster:*"};
  for (const auto& value : bad) { auto r = WithIamAcquisition::create(request(value), target());
    ASSERT_FALSE(r); EXPECT_EQ(WithIamFailure::InvalidAcquisition, r.error().failure); }
  for (int field = 0; field != 5; ++field) {
    auto t = target();
    if (field == 0) t.account = "123456789013";
    if (field == 1) t.region = "eu-west-1";
    if (field == 2) t.cluster_identifier = "foreign-cluster";
    if (field == 3) t.partition = WithIamPartition::AwsCn;
    if (field == 4) t.database = "foreign";
    EXPECT_FALSE(WithIamAcquisition::create(request(), t));
  }
}
TEST(WithIamResponseTest, ClosedPartitionRepresentationDoesNotInferAvailability) {
  const WithIamPartition parts[] = {WithIamPartition::Aws, WithIamPartition::AwsCn, WithIamPartition::AwsUsGov};
  const std::string texts[] = {"aws", "aws-cn", "aws-us-gov"};
  for (int i = 0; i != 3; ++i) EXPECT_TRUE(WithIamAcquisition::create(request(arn(texts[i])), target(parts[i])));
  auto t = target(); t.partition = static_cast<WithIamPartition>(99);
  auto r = WithIamAcquisition::create(request(), t); ASSERT_FALSE(r);
  EXPECT_EQ(WithIamFailure::UnsupportedPartition, r.error().failure);
}
TEST(WithIamResponseTest, ExplicitDurationAndDatabaseLocalSyntaxOnly) {
  for (unsigned duration : {900u, 3600u}) { auto t = target(); t.duration_seconds = duration;
    EXPECT_TRUE(WithIamAcquisition::create(request(), t)); }
  for (unsigned duration : {0u, 899u, 3601u, (std::numeric_limits<unsigned>::max)()}) {
    auto t = target(); t.duration_seconds = duration; EXPECT_FALSE(WithIamAcquisition::create(request(), t)); }
  for (std::string db : {"", "1pilot", "p:ilot", "p/ilot"}) {
    auto t = target(); t.database = db; EXPECT_FALSE(WithIamAcquisition::create(request(), t)); }
}
TEST(WithIamResponseTest, DelimiterAndLocalBoundsNeverBecomeArnInjection) {
  for (int field = 0; field != 6; ++field) { auto t = target();
    if (field == 0) t.account = "12345678901x";
    if (field == 1) t.region = "eu--north-1";
    if (field == 2) t.region = "eu:north-1";
    if (field == 3) t.cluster_identifier = "id/tail";
    if (field == 4) t.cluster_identifier = std::string(64, 'a');
    if (field == 5) t.cluster_identifier = std::string("id\0tail", 7);
    EXPECT_FALSE(WithIamAcquisition::create(request(), t)); }
}
TEST(WithIamResponseTest, OrdinaryAndForeignServiceAreNotProvisionedMethods) {
  auto r = WithIamAcquisition::create(request(arn(), "generation", at(150), 6s, Service::Rds, Method::OrdinaryPassword), target());
  ASSERT_FALSE(r); EXPECT_EQ(WithIamFailure::UnsupportedMethod, r.error().failure);
  auto ordinary = WithIamAcquisition::create(request(arn(), "generation", at(150), 6s, Service::Redshift, Method::OrdinaryPassword), target());
  EXPECT_FALSE(ordinary);
}
TEST(WithIamResponseTest, MovedFromAcquisitionAndRequestAreClosedAndConsumeResponse) {
  auto original = acquisition(); auto retained = std::move(original); auto input = response();
  auto r = project_withiam_response(original, std::move(input), sample(), policy());
  failure(r, WithIamFailure::InvalidAcquisition); EXPECT_TRUE(input.password.empty());
  EXPECT_TRUE(project_withiam_response(retained, response(), sample(), policy()));
  auto req = request(); auto owner = std::move(req);
  EXPECT_FALSE(WithIamAcquisition::create(req, target())); EXPECT_FALSE(retained.matches(req));
  EXPECT_TRUE(retained.matches(owner));
}
TEST(WithIamResponseTest, MissingWrongPrincipalEmptyAndMovedSecretCannotPublish) {
  for (std::string user : std::vector<std::string>{"", "alice", "IAMA:alice", "IAMR:alice", "IAM:Alice", "foreign", std::string("IAM:alice\0", 10), std::string("\xff", 1)}) {
    auto input = response(user); auto r = project_withiam_response(acquisition(), std::move(input), sample(), policy());
    ASSERT_FALSE(r); EXPECT_TRUE(input.password.empty());
  }
  auto input = response("IAM:alice", 1060000, "");
  failure(project_withiam_response(acquisition(), std::move(input), sample(), policy()), WithIamFailure::InvalidResponse);
  input = response(); auto kept = std::move(input);
  failure(project_withiam_response(acquisition(), std::move(input), sample(), policy()), WithIamFailure::InvalidResponse);
  EXPECT_TRUE(project_withiam_response(acquisition(), std::move(kept), sample(), policy()));
  input = response("IAM:alice", 1060000, std::string("a\0b", 3));
  failure(project_withiam_response(acquisition(), std::move(input), sample(), policy()), WithIamFailure::InvalidResponse);
}
TEST(WithIamResponseTest, MissingNonpositiveAndOverflowExpiryConsumePassword) {
  const std::optional<std::int64_t> values[] = {std::nullopt, 0, -1, (std::numeric_limits<std::int64_t>::max)()};
  for (auto expiry : values) { auto input = response("IAM:alice", expiry);
    failure(project_withiam_response(acquisition(), std::move(input), sample(), policy()), WithIamFailure::ExpiryRange);
    EXPECT_TRUE(input.password.empty()); }
}
TEST(WithIamResponseTest, OriginalDeadlineAndQualityRemainClosed) {
  auto r = project_withiam_response(acquisition(request(arn(), "generation", at(150), 7s)), response(), sample(), policy());
  failure(r, WithIamFailure::TimeConversionFailed); EXPECT_EQ(TimeFailure::Expired, r.error().time_failure);
  auto s = sample(); s.utc_uncertainty.reset();
  auto unknown = project_withiam_response(acquisition(), response(), s, policy());
  failure(unknown, WithIamFailure::TimeConversionFailed); EXPECT_EQ(TimeFailure::UnknownClockQuality, unknown.error().time_failure);
  s = sample(); s.acquisition_completed = at(101);
  auto rollback = project_withiam_response(acquisition(), response(), s, policy());
  ASSERT_FALSE(rollback); EXPECT_EQ(TimeFailure::ClockRollback, rollback.error().time_failure);
  s = sample(); s.now = at(111);
  auto stale = project_withiam_response(acquisition(), response(), s, policy());
  ASSERT_FALSE(stale); EXPECT_EQ(TimeFailure::StaleSample, stale.error().time_failure);
}
struct Stop : Cancellation { bool stopped = false; bool stop_requested() const noexcept override { return stopped; } };
TEST(WithIamResponseTest, CancellationConsumesBeforeEarlyValidation) {
  Stop stop; stop.stopped = true; auto input = response(); auto a = acquisition(); auto kept = std::move(a);
  failure(project_withiam_response(a, std::move(input), sample(), policy(), &stop), WithIamFailure::Cancelled);
  EXPECT_TRUE(input.password.empty()); EXPECT_FALSE(kept.invariant_error());
}
struct FakeClock : MonotonicClock { Deadline value = at(103); Deadline now() noexcept override { return value; } };
// Explicitly selected fixed-operation issuer seam; Request and untagged response
// fields cannot prove an AWS operation or authenticated response pairing.
struct WithIamTestIssuer : TrustedIssuer {
  static constexpr std::string_view operation = WithIamAcquisition::operation;
  WithIamAcquisition context = acquisition(); int calls = 0; Stop* stop_during = nullptr;
  Outcome<Material> acquire(const Request& req, const Cancellation* cancel) override {
    ++calls;
    if (!context.matches(req)) return Error{Reason::TargetMismatch};
    auto candidate = project_withiam_response(context, response(), sample(), policy(), cancel);
    if (stop_during) stop_during->stopped = true;
    if (!candidate) return Error{Reason::InvalidMaterial};
    return std::move(candidate).value();
  }
};
TEST(WithIamResponseTest, ExplicitAuthorityBridgePublishesOnlyExactRequestAndSingleUseReceipt) {
  WithIamTestIssuer issuer; FakeClock clock; auto req = request();
  EXPECT_EQ("GetClusterCredentialsWithIAM", issuer.operation);
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
TEST(WithIamResponseTest, ForeignAuthorityRollbackLateAndCancelledBridgeNeverPublish) {
  WithIamTestIssuer issuer; FakeClock clock; Stop stop; auto req = request();
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
TEST(WithIamResponseTest, SafeErrorsAreClosedWithoutSourcePayloads) {
  for (auto f : {WithIamFailure::UnsupportedMethod, WithIamFailure::UnsupportedPartition, WithIamFailure::InvalidAcquisition,
      WithIamFailure::InvalidResponse, WithIamFailure::PrincipalMismatch, WithIamFailure::ExpiryRange,
      WithIamFailure::TimeConversionFailed, WithIamFailure::Cancelled, WithIamFailure::AllocationFailed}) {
    WithIamError e{f}; EXPECT_FALSE(e.safe_message().empty());
    EXPECT_EQ(std::string_view::npos, e.safe_message().find("SECRET_SENTINEL"));
  }
}

WithIamResponseUtc utc_response(std::optional<UtcInstant> expiry = UtcInstant{1060000001},
    std::string user = "IAM:alice", std::string password = "SECRET_SENTINEL") {
  auto old = response(std::move(user), 1060000, std::move(password));
  return {std::move(old.user), std::move(old.password), expiry};
}
TEST(WithIamResponseTest, UtcTypesHaveNoImplicitMillisecondOrOperationConversions) {
  static_assert(!std::is_convertible_v<WithIamResponse, WithIamResponseUtc>);
  static_assert(!std::is_convertible_v<WithIamResponseUtc, WithIamResponse>);
  static_assert(!std::is_copy_constructible_v<WithIamResponseUtc>);
  static_assert(!std::is_convertible_v<ServerlessResponseUtc, ProvisionedResponseUtc>);
  static_assert(!std::is_convertible_v<ProvisionedResponseUtc, WithIamResponseUtc>);
  static_assert(!std::is_convertible_v<WithIamResponseUtc, ServerlessResponseUtc>);
}
TEST(WithIamResponseTest, ExactOneMicrosecondChangesExclusiveBoundaryWithoutRounding) {
  auto a = acquisition(request(arn(), "generation", at(150), 7s));
  auto utc = project_withiam_response_utc(a, utc_response(), sample(), policy()); ASSERT_TRUE(utc);
  EXPECT_EQ(at(157) + 1us, utc.value().validity().expires_at);
  EXPECT_EQ(at(103), utc.value().validity().issued_at);
  auto ms = project_withiam_response(a, response(), sample(), policy());
  failure(ms, WithIamFailure::TimeConversionFailed); EXPECT_EQ(TimeFailure::Expired, ms.error().time_failure);
  auto equality = acquisition(request(arn(), "generation", at(150), 7s + 1us));
  auto refused = project_withiam_response_utc(equality, utc_response(), sample(), policy());
  failure(refused, WithIamFailure::TimeConversionFailed); EXPECT_EQ(TimeFailure::Expired, refused.error().time_failure);
  auto adjacent = acquisition(request(arn(), "generation", at(150), 7s + 1us - Clock::duration{1}));
  EXPECT_TRUE(project_withiam_response_utc(adjacent, utc_response(), sample(), policy()));
}
TEST(WithIamResponseTest, MillisecondCompatibilityAndCheckedRangeStayUnchanged) {
  auto a = acquisition();
  auto ms = project_withiam_response(a, response(), sample(), policy());
  auto utc = project_withiam_response_utc(a, utc_response(UtcInstant{1060000000}), sample(), policy());
  ASSERT_TRUE(ms); ASSERT_TRUE(utc);
  EXPECT_EQ(ms.value().binding().target(), utc.value().binding().target());
  EXPECT_EQ(ms.value().binding().source(), utc.value().binding().source());
  EXPECT_EQ(ms.value().principal(), utc.value().principal());
  EXPECT_EQ(ms.value().validity().issued_at, utc.value().validity().issued_at);
  EXPECT_EQ(ms.value().validity().expires_at, utc.value().validity().expires_at);
  const auto maximum = (std::numeric_limits<std::int64_t>::max)();
  auto edge_ms = project_withiam_response(a, response("IAM:alice", maximum / 1000), sample(), policy());
  auto edge_utc = project_withiam_response_utc(a, utc_response(UtcInstant{(maximum / 1000) * 1000}), sample(), policy());
  failure(edge_ms, WithIamFailure::TimeConversionFailed); failure(edge_utc, WithIamFailure::TimeConversionFailed);
  EXPECT_EQ(edge_ms.error().time_failure, edge_utc.error().time_failure);
  failure(project_withiam_response(a, response("IAM:alice", maximum / 1000 + 1), sample(), policy()), WithIamFailure::ExpiryRange);
  failure(project_withiam_response_utc(a, utc_response(UtcInstant{maximum}), sample(), policy()), WithIamFailure::TimeConversionFailed);
  for (auto expiry : {std::optional<UtcInstant>{}, std::optional<UtcInstant>{UtcInstant{0}}, std::optional<UtcInstant>{UtcInstant{-1}}}) {
    auto input = utc_response(expiry);
    failure(project_withiam_response_utc(a, std::move(input), sample(), policy()), WithIamFailure::ExpiryRange);
    EXPECT_TRUE(input.password.empty());
  }
}
TEST(WithIamResponseTest, LegacyAndUtcMultifaultPrecedenceAndEarlyOwnershipAreIdentical) {
  auto a = acquisition(); const auto maximum = (std::numeric_limits<std::int64_t>::max)();
  auto ms_wrong = response("foreign", maximum); auto utc_wrong = utc_response(UtcInstant{maximum}, "foreign");
  failure(project_withiam_response(a, std::move(ms_wrong), sample(), policy()), WithIamFailure::PrincipalMismatch);
  failure(project_withiam_response_utc(a, std::move(utc_wrong), sample(), policy()), WithIamFailure::PrincipalMismatch);
  EXPECT_TRUE(ms_wrong.password.empty()); EXPECT_TRUE(utc_wrong.password.empty());
  auto invalid = acquisition(); auto kept = std::move(invalid);
  failure(project_withiam_response(invalid, response("IAM:alice", maximum), sample(), policy()), WithIamFailure::InvalidAcquisition);
  failure(project_withiam_response_utc(invalid, utc_response(UtcInstant{maximum}), sample(), policy()), WithIamFailure::InvalidAcquisition);
  failure(project_withiam_response(kept, response("IAM:alice", std::nullopt, ""), sample(), policy()), WithIamFailure::InvalidResponse);
  failure(project_withiam_response_utc(kept, utc_response(std::nullopt, "IAM:alice", ""), sample(), policy()), WithIamFailure::InvalidResponse);
  auto invalid_secret = utc_response(UtcInstant{1060000000}, "IAM:alice", std::string("a\0b", 3));
  failure(project_withiam_response_utc(kept, std::move(invalid_secret), sample(), policy()), WithIamFailure::InvalidResponse);
}
struct UtcCountCancel : Cancellation {
  mutable unsigned calls = 0; unsigned stop_at = 99;
  bool stop_requested() const noexcept override { return ++calls >= stop_at; }
};
TEST(WithIamResponseTest, DelegationAddsNoCancellationObservationOrChangedErrors) {
  for (unsigned stop_at : {1u, 2u, 3u, 4u, 99u}) {
    UtcCountCancel ms_cancel, utc_cancel; ms_cancel.stop_at = utc_cancel.stop_at = stop_at;
    auto a = acquisition(); auto ms_input = response(); auto utc_input = utc_response(UtcInstant{1060000000});
    auto ms = project_withiam_response(a, std::move(ms_input), sample(), policy(), &ms_cancel);
    auto utc = project_withiam_response_utc(a, std::move(utc_input), sample(), policy(), &utc_cancel);
    EXPECT_EQ(static_cast<bool>(ms), static_cast<bool>(utc)); EXPECT_EQ(ms_cancel.calls, utc_cancel.calls);
    EXPECT_EQ(stop_at < 4 ? stop_at : 4u, ms_cancel.calls);
    if (!ms) {
      ASSERT_FALSE(utc); EXPECT_EQ(ms.error().failure, utc.error().failure);
      EXPECT_EQ(ms.error().time_failure, utc.error().time_failure);
    }
    EXPECT_TRUE(ms_input.password.empty()); EXPECT_TRUE(utc_input.password.empty());
  }
  auto invalid = acquisition(); auto retained = std::move(invalid); UtcCountCancel cancel; cancel.stop_at = 1;
  failure(project_withiam_response_utc(invalid, utc_response(std::nullopt), sample(), policy(), &cancel), WithIamFailure::Cancelled);
  EXPECT_FALSE(retained.invariant_error());
}
TEST(WithIamResponseTest, UtcMaterialOwnsSecretAndRejectsMovedFromResponse) {
  auto input = utc_response(); auto retained = std::move(input); auto a = acquisition();
  failure(project_withiam_response_utc(a, std::move(input), sample(), policy()), WithIamFailure::InvalidResponse);
  auto output = project_withiam_response_utc(a, std::move(retained), sample(), policy()); ASSERT_TRUE(output);
  EXPECT_TRUE(retained.password.empty());
  output.value().with_secret([](auto bytes) {
    EXPECT_EQ("SECRET_SENTINEL", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  });
  EXPECT_EQ("IAM:alice", output.value().principal()); EXPECT_EQ(at(157) + 1us, output.value().validity().expires_at);
}
struct UtcIssuer : TrustedIssuer {
  WithIamAcquisition context = acquisition(request(arn(), "generation", at(150), 7s));
  unsigned calls = 0;
  Outcome<Material> acquire(const Request& original, const Cancellation* cancel) override {
    ++calls;
    if (!context.matches(original)) return Error{Reason::TargetMismatch};
    // Selected operation-owned candidate; this fake is not authenticated SDK pairing.
    auto result = project_withiam_response_utc(context, utc_response(), sample(), policy(), cancel);
    if (!result) return Error{Reason::InvalidMaterial};
    return std::move(result).value();
  }
};
TEST(WithIamResponseTest, ExplicitUtcIssuerAuthorityKeepsOriginalBindingAndSingleUseAdmission) {
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
