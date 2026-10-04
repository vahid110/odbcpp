#include "core/auth/temporary_db_validity.h"
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace rs::core::auth;
using rs::util::Clock;
using rs::util::Deadline;
using namespace std::chrono_literals;
Deadline at(long long seconds) { return Deadline{std::chrono::seconds{seconds}}; }
UtcInstant utc(long long seconds) { return {seconds * 1000000}; }
Binding binding(bool temporary = true, std::string resource = "resource", std::string generation = "generation") {
  auto r = Binding::create({Service::Redshift, "db.example", 5439, "pilot", "exact_user", std::move(resource), "db.example", "trust"},
      {temporary ? SourceKind::TrustedTemporaryDbIssuer : SourceKind::ExternalPassword, "source", std::move(generation)},
      temporary ? Method::TemporaryDatabasePassword : Method::OrdinaryPassword);
  if (!r) throw std::runtime_error("bad test binding");
  return std::move(r).value();
}
Request request(Deadline deadline = at(150), Clock::duration headroom = 6s) {
  auto r = Request::create(binding(), deadline, headroom);
  if (!r) throw std::runtime_error("bad test request");
  return std::move(r).value();
}
TemporaryClockSample sample() { return {utc(1000), at(100), at(102), at(103), at(103), 3s, 0us, 0us, at(99)}; }
TemporaryTimePolicy policy() { return {10s, 2s, 10s, 3600s}; }
void fails(const TimeConversion& result, TimeFailure expected) {
  ASSERT_FALSE(result); EXPECT_EQ(expected, result.failure());
  EXPECT_FALSE(TimeConversion::safe_message(result.failure()).empty());
}
TEST(TemporaryDbTimeTest, EarliestBracketUncertaintyAndExplicitReadinessAreConservative) {
  auto result = convert_temporary_db_validity(request(), utc(1060), sample(), policy());
  ASSERT_TRUE(result);
  EXPECT_EQ(at(157), result.value().expires_at);
  EXPECT_EQ(at(103), result.value().issued_at); // Not bracket completion 102.
  auto later = sample(); later.acquisition_completed = at(104); later.now = at(104);
  auto delayed = convert_temporary_db_validity(request(), utc(1060), later, policy()); ASSERT_TRUE(delayed);
  EXPECT_EQ(at(157), delayed.value().expires_at); EXPECT_EQ(at(104), delayed.value().issued_at);
}
TEST(TemporaryDbTimeTest, ExclusiveHeadroomAndOneTickNeighbors) {
  fails(convert_temporary_db_validity(request(at(150), 7s), utc(1060), sample(), policy()), TimeFailure::Expired);
  auto before = convert_temporary_db_validity(request(at(157) - Clock::duration{1}, Clock::duration::zero()), utc(1060), sample(), policy());
  ASSERT_TRUE(before); EXPECT_EQ(at(157), before.value().expires_at);
  fails(convert_temporary_db_validity(request(at(157), Clock::duration::zero()), utc(1060), sample(), policy()), TimeFailure::Expired);
  fails(convert_temporary_db_validity(request(at(157) + Clock::duration{1}, Clock::duration::zero()), utc(1060), sample(), policy()), TimeFailure::Expired);
  auto elapsed = sample(); elapsed.now = at(150); elapsed.acquisition_completed = at(150);
  auto p = policy(); p.max_age = 60s;
  fails(convert_temporary_db_validity(request(), utc(1060), elapsed, p), TimeFailure::Expired);
}
TEST(TemporaryDbTimeTest, FreshnessUsesBeforeRatherThanAfterAndLimitsAreInclusive) {
  auto p = policy(); p.max_age = 3s;
  ASSERT_TRUE(convert_temporary_db_validity(request(), utc(1060), sample(), p));
  auto older = sample(); older.now += Clock::duration{1};
  fails(convert_temporary_db_validity(request(), utc(1060), older, p), TimeFailure::StaleSample);
  p.max_age = 2s; // now-after is only 1s, but actual observation age may be 3s.
  fails(convert_temporary_db_validity(request(), utc(1060), sample(), p), TimeFailure::StaleSample);
  p = policy(); auto wide = sample(); wide.after += Clock::duration{1};
  fails(convert_temporary_db_validity(request(), utc(1060), wide, p), TimeFailure::StaleSample);
}
TEST(TemporaryDbTimeTest, UnknownQualityRefusesAndEveryQuantitativeErrorConsumesValidity) {
  for (int missing = 0; missing != 3; ++missing) {
    auto s = sample();
    if (missing == 0) s.utc_uncertainty.reset();
    if (missing == 1) s.drift_error.reset();
    if (missing == 2) s.suspend_error.reset();
    fails(convert_temporary_db_validity(request(), utc(1060), s, policy()), TimeFailure::UnknownClockQuality);
  }
  auto s = sample(); s.drift_error = 2s; s.suspend_error = 1s;
  auto shorter = convert_temporary_db_validity(request(at(150), 3s), utc(1060), s, policy()); ASSERT_TRUE(shorter);
  EXPECT_EQ(at(154), shorter.value().expires_at);
  fails(convert_temporary_db_validity(request(at(150), 4s), utc(1060), s, policy()), TimeFailure::Expired);
  s = sample(); s.utc_uncertainty = 0us;
  auto explicit_zero = convert_temporary_db_validity(request(), utc(1060), s, policy()); ASSERT_TRUE(explicit_zero);
  EXPECT_EQ(at(160), explicit_zero.value().expires_at);
  s.utc_uncertainty = -1us;
  fails(convert_temporary_db_validity(request(), utc(1060), s, policy()), TimeFailure::InvalidInput);
  s.utc_uncertainty = 11s;
  fails(convert_temporary_db_validity(request(), utc(1060), s, policy()), TimeFailure::UnknownClockQuality);
  s.utc_uncertainty = 7s; s.drift_error = 4s;
  fails(convert_temporary_db_validity(request(), utc(1060), s, policy()), TimeFailure::UnknownClockQuality);
}
TEST(TemporaryDbTimeTest, OrderingReadinessRollbackAndSentinelsRefuse) {
  for (int malformed = 0; malformed != 5; ++malformed) {
    auto s = sample();
    if (malformed == 0) s.before = at(103);
    if (malformed == 1) s.acquisition_completed = at(101);
    if (malformed == 2) s.acquisition_completed = at(104);
    if (malformed == 3) s.now = at(101);
    if (malformed == 4) s.previous_monotonic_sample = at(101);
    fails(convert_temporary_db_validity(request(), utc(1060), s, policy()), TimeFailure::ClockRollback);
  }
  auto s = sample(); s.before = Deadline::min();
  fails(convert_temporary_db_validity(request(), utc(1060), s, policy()), TimeFailure::InvalidInput);
  s = sample(); s.previous_monotonic_sample = Deadline::max();
  fails(convert_temporary_db_validity(request(), utc(1060), s, policy()), TimeFailure::InvalidInput);
}
TEST(TemporaryDbTimeTest, ExpiryHorizonAndPolicyLimitsNeverBecomeUnbounded) {
  fails(convert_temporary_db_validity(request(), utc(1000), sample(), policy()), TimeFailure::Expired);
  fails(convert_temporary_db_validity(request(), utc(1003), sample(), policy()), TimeFailure::Expired);
  auto p = policy(); p.max_horizon = 57s;
  ASSERT_TRUE(convert_temporary_db_validity(request(), utc(1060), sample(), p));
  fails(convert_temporary_db_validity(request(), {1060000001}, sample(), p), TimeFailure::InvalidInput);
  p = policy(); p.max_horizon = 0us;
  fails(convert_temporary_db_validity(request(), utc(1060), sample(), p), TimeFailure::InvalidInput);
  p = policy(); p.max_age = -1us;
  fails(convert_temporary_db_validity(request(), utc(1060), sample(), p), TimeFailure::InvalidInput);
  p = policy(); p.max_horizon = 25h;
  fails(convert_temporary_db_validity(request(), utc(1060), sample(), p), TimeFailure::InvalidInput);
  auto s = sample(); s.now = at(157); s.acquisition_completed = at(157); p = policy(); p.max_age = 60s;
  fails(convert_temporary_db_validity(request(at(158), Clock::duration::zero()), utc(1060), s, p), TimeFailure::Expired);
}
TEST(TemporaryDbTimeTest, CheckedUtcAndMonotonicExtremesDoNotOverflow) {
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  auto s = sample(); s.utc = {minimum};
  fails(convert_temporary_db_validity(request(), {maximum}, s, policy()), TimeFailure::Overflow);
  s.utc = {maximum};
  fails(convert_temporary_db_validity(request(), {minimum}, s, policy()), TimeFailure::Overflow);
  s.utc = {0};
  fails(convert_temporary_db_validity(request(), {minimum}, s, policy()), TimeFailure::Overflow); // subtract error after E-u
  s = sample(); s.previous_monotonic_sample.reset(); s.before = Deadline::min() + Clock::duration{1};
  fails(convert_temporary_db_validity(request(), utc(1060), s, policy()), TimeFailure::Overflow);
  s = sample(); s.previous_monotonic_sample.reset();
  s.before = s.after = s.now = s.acquisition_completed = Deadline::max() - 2s;
  auto r = request(Deadline::max() - 1s, Clock::duration::zero());
  fails(convert_temporary_db_validity(r, utc(1060), s, policy()), TimeFailure::Overflow);
}
TEST(TemporaryDbTimeTest, FixedMicrosecondPrecisionPreservesSubsecondBoundAndOriginalRequest) {
  auto s = sample(); s.utc_uncertainty = 3000001us;
  auto result = convert_temporary_db_validity(request(at(150), 6s), utc(1060), s, policy()); ASSERT_TRUE(result);
  EXPECT_EQ(at(157) - 1us, result.value().expires_at);
  auto original = request(); auto retained = std::move(original);
  fails(convert_temporary_db_validity(original, utc(1060), sample(), policy()), TimeFailure::InvalidInput);
  ASSERT_TRUE(convert_temporary_db_validity(retained, utc(1060), sample(), policy()));
  auto ordinary = Request::create(binding(false), at(150), 6s); ASSERT_TRUE(ordinary);
  fails(convert_temporary_db_validity(ordinary.value(), utc(1060), sample(), policy()), TimeFailure::UnsupportedMethod);
}
class FixedClock final : public MonotonicClock {
 public: Deadline instant = at(103); Deadline now() noexcept override { return instant; }
};
class Cancel final : public Cancellation {
 public: bool requested = true; bool stop_requested() const noexcept override { return requested; }
};
class ConvertedIssuer final : public TrustedIssuer {
 public:
  ConvertedIssuer(Binding candidate, std::string user = "exact_user") : candidate_(std::move(candidate)), user_(std::move(user)) {}
  TemporaryClockSample clock_sample = sample();
  std::optional<Validity> published;
  int calls = 0;
  Deadline observed_deadline{};
  std::function<void()> on_candidate;
  Outcome<Material> acquire(const Request& r, const Cancellation* cancel) override {
    ++calls; observed_deadline = r.deadline();
    auto converted = convert_temporary_db_validity(r, utc(1060), clock_sample, policy(), cancel);
    if (!converted) return Error{converted.failure() == TimeFailure::Cancelled ? Reason::Cancelled : Reason::IssuerFailed};
    published = converted.value();
    if (on_candidate) on_candidate();
    const std::string_view opaque = "opaque-password";
    auto bytes = SecretBytes::create(std::as_bytes(std::span{opaque.data(), opaque.size()}));
    if (!bytes) return bytes.error();
    return Material::create(candidate_, MaterialKind::TemporaryDatabasePassword, user_, std::move(bytes).value(), *published);
  }
 private: Binding candidate_; std::string user_;
};
TEST(TemporaryDbTimeCompositionTest, AuthorityRetainsOriginalBoundAndDoesNotResampleAtTake) {
  auto b = binding(); ConvertedIssuer issuer(b); FixedClock clock;
  auto a = Authority::create(b, issuer, clock); ASSERT_TRUE(a);
  auto admitted = a.value()->acquire(request()); ASSERT_TRUE(admitted);
  auto receipt = std::move(admitted).value();
  ASSERT_TRUE(issuer.published); EXPECT_EQ(at(157), issuer.published->expires_at);
  // No converter call occurs during take. A later fake UTC rollback cannot alter
  // the material already inside the receipt. It is not permission to reconvert.
  issuer.clock_sample.utc = utc(900); clock.instant = at(104);
  auto taken = a.value()->take(std::move(receipt)); ASSERT_TRUE(taken);
  EXPECT_EQ(at(157), taken.value().validity().expires_at); EXPECT_EQ(1, issuer.calls);
  EXPECT_EQ(at(150), issuer.observed_deadline);
}
TEST(TemporaryDbTimeCompositionTest, BindingAndPrincipalChecksRemainAuthorityOwned) {
  for (int mismatch = 0; mismatch != 3; ++mismatch) {
    auto b = binding(); FixedClock clock;
    ConvertedIssuer issuer(binding(true, mismatch == 0 ? "foreign" : "resource", mismatch == 1 ? "foreign" : "generation"),
        mismatch == 2 ? "unexpected_user" : "exact_user");
    auto a = Authority::create(b, issuer, clock); ASSERT_TRUE(a);
    auto result = a.value()->acquire(request()); ASSERT_FALSE(result);
    EXPECT_EQ(mismatch == 0 ? Reason::TargetMismatch : mismatch == 1 ? Reason::SourceMismatch : Reason::PrincipalMismatch, result.error().reason());
  }
}
TEST(TemporaryDbTimeCompositionTest, LateOrCancelledCandidateCannotPublishAndDeadlineStillControlsTake) {
  auto b = binding(); FixedClock clock; ConvertedIssuer issuer(b);
  auto a = Authority::create(b, issuer, clock); ASSERT_TRUE(a);
  issuer.on_candidate = [&] { clock.instant = at(150); };
  auto late = a.value()->acquire(request()); ASSERT_FALSE(late);
  EXPECT_EQ(Reason::DeadlineElapsed, late.error().reason());
  EXPECT_EQ(1, issuer.calls); ASSERT_TRUE(issuer.published); // Converted candidate is not a Receipt.
  FixedClock second_clock; ConvertedIssuer second_issuer(b); Cancel cancel; cancel.requested = false;
  auto second = Authority::create(b, second_issuer, second_clock); ASSERT_TRUE(second);
  second_issuer.on_candidate = [&] { cancel.requested = true; };
  auto cancelled = second.value()->acquire(request(), &cancel); ASSERT_FALSE(cancelled);
  EXPECT_EQ(Reason::Cancelled, cancelled.error().reason()); EXPECT_EQ(1, second_issuer.calls);
  FixedClock third_clock; ConvertedIssuer third_issuer(b);
  auto third = Authority::create(b, third_issuer, third_clock); ASSERT_TRUE(third);
  auto admitted = third.value()->acquire(request()); ASSERT_TRUE(admitted);
  auto receipt = std::move(admitted).value(); third_clock.instant = at(150);
  auto expired_take = third.value()->take(std::move(receipt)); ASSERT_FALSE(expired_take);
  EXPECT_EQ(Reason::DeadlineElapsed, expired_take.error().reason()); EXPECT_EQ(1, third_issuer.calls);
}
TEST(TemporaryDbTimeCompositionTest, CancelledLateAndRolledBackClockCannotPublishOrTake) {
  Cancel cancel;
  fails(convert_temporary_db_validity(request(), utc(1060), sample(), policy(), &cancel), TimeFailure::Cancelled);
  auto b = binding(); ConvertedIssuer issuer(b); FixedClock clock;
  auto a = Authority::create(b, issuer, clock); ASSERT_TRUE(a);
  auto cancelled = a.value()->acquire(request(), &cancel); ASSERT_FALSE(cancelled); EXPECT_EQ(0, issuer.calls);
  auto admitted = a.value()->acquire(request()); ASSERT_TRUE(admitted);
  auto receipt = std::move(admitted).value(); clock.instant = at(102);
  auto rollback = a.value()->take(std::move(receipt)); ASSERT_FALSE(rollback); EXPECT_EQ(Reason::ClockRollback, rollback.error().reason());
  ConvertedIssuer late_issuer(b); FixedClock late_clock; late_clock.instant = at(150);
  auto late_a = Authority::create(b, late_issuer, late_clock); ASSERT_TRUE(late_a);
  auto late = late_a.value()->acquire(request()); ASSERT_FALSE(late); EXPECT_EQ(0, late_issuer.calls);
}
} // namespace
