#include "tests/integration/mysql_datetime_q6_preflight_deadlines.h"
#include <gtest/gtest.h>
#include <cstdint>

namespace {
using namespace rs::tests::mysql::q6;
constexpr auto policy = Policy::Overall30Startup10Cleanup5;
Time at(std::int64_t seconds) {
  return Time{std::chrono::duration_cast<Duration>(std::chrono::seconds{seconds})};
}
Time tick(Rep value) { return Time{Duration{value}}; }
static_assert(!std::is_default_constructible_v<PhaseDeadlines>);
static_assert(!std::is_copy_constructible_v<PhaseDeadlines>);
static_assert(!std::is_move_constructible_v<PhaseDeadlines>);
static_assert(!PhaseDeadlines::grants_native_authority);
static_assert(!PhaseDeadlines::grants_hard_interruption);
}
TEST(MySqlQ6PreflightDeadlinesTest, LiteralOverallAndStartupCutoffsAreDistinct) {
  PhaseDeadlines early(policy, at(0)); ASSERT_TRUE(early.begin_startup(at(0)));
  EXPECT_EQ(at(30), early.view().overall); EXPECT_EQ(at(10), early.view().startup);
  ASSERT_TRUE(early.finish_startup(at(9))); EXPECT_TRUE(early.view().startup_complete);
  PhaseDeadlines late(policy, at(0)); ASSERT_TRUE(late.begin_startup(at(25)));
  EXPECT_EQ(at(30), late.view().overall); EXPECT_EQ(at(30), late.view().startup);
  ASSERT_TRUE(late.finish_startup(at(29)));
  PhaseDeadlines shifted(policy, at(7)); ASSERT_TRUE(shifted.begin_startup(at(8)));
  EXPECT_EQ(at(37), shifted.view().overall); EXPECT_EQ(at(18), shifted.view().startup);
}
TEST(MySqlQ6PreflightDeadlinesTest, EqualityRejectsBeginAndCompletionWithoutRenewal) {
  PhaseDeadlines at_end(policy, at(0)); EXPECT_FALSE(at_end.begin_startup(at(30)));
  EXPECT_EQ(Fault::Expired, at_end.view().first_fault); EXPECT_FALSE(at_end.view().startup);
  EXPECT_FALSE(at_end.begin_startup(at(1))); EXPECT_FALSE(at_end.view().startup);
  for (auto begin : {at(0), at(25)}) {
    PhaseDeadlines p(policy, at(0)); ASSERT_TRUE(p.begin_startup(begin));
    const auto cutoff = p.view().startup; ASSERT_TRUE(cutoff);
    EXPECT_FALSE(p.finish_startup(*cutoff)); EXPECT_EQ(Fault::Expired, p.view().first_fault);
    EXPECT_FALSE(p.finish_startup(tick(cutoff->time_since_epoch().count() - 1)));
    EXPECT_EQ(cutoff, p.view().startup); EXPECT_FALSE(p.view().startup_complete);
  }
}
TEST(MySqlQ6PreflightDeadlinesTest, ClosedPolicyAndSentinelsPublishNoPartialPlan) {
  for (auto origin : {Time::min(), Time::max()}) {
    PhaseDeadlines p(policy, origin); EXPECT_EQ(Fault::InvalidTime, p.view().first_fault);
    EXPECT_FALSE(p.view().origin); EXPECT_FALSE(p.view().overall); EXPECT_FALSE(p.begin_startup(at(0)));
  }
  PhaseDeadlines p(static_cast<Policy>(0), Time::max());
  EXPECT_EQ(Fault::InvalidPolicy, p.view().first_fault); EXPECT_FALSE(p.view().overall);
  for (auto value : {Time::min(), Time::max()}) {
    PhaseDeadlines q(policy, at(0)); EXPECT_FALSE(q.begin_startup(value));
    EXPECT_EQ(Fault::InvalidTime, q.view().first_fault); EXPECT_FALSE(q.view().startup);
  }
}
TEST(MySqlQ6PreflightDeadlinesTest, NearCapacityReservesFiniteCleanupAndHandlesNegativeOrigin) {
  const Rep maximum = std::numeric_limits<Rep>::max();
  const Rep thirty_five = std::chrono::duration_cast<Duration>(std::chrono::seconds{35}).count();
  const Rep five = std::chrono::duration_cast<Duration>(std::chrono::seconds{5}).count();
  PhaseDeadlines good(policy, tick(maximum - thirty_five - 1));
  ASSERT_TRUE(good.view().overall); EXPECT_EQ(tick(maximum - five - 1), good.view().overall);
  ASSERT_TRUE(good.begin_startup(*good.view().origin));
  EXPECT_EQ(tick(maximum - thirty_five - 1) + std::chrono::seconds{10}, good.view().startup);
  ASSERT_TRUE(good.begin_cleanup(*good.view().overall)); EXPECT_EQ(tick(maximum - 1), good.view().cleanup);
  EXPECT_FALSE(good.finish_cleanup(tick(maximum - 1))); EXPECT_EQ(Fault::Expired, good.view().cleanup_fault);
  for (Rep count : {maximum - thirty_five, maximum - 1}) {
    PhaseDeadlines bad(policy, tick(count)); EXPECT_EQ(Fault::Overflow, bad.view().first_fault);
    EXPECT_FALSE(bad.view().origin); EXPECT_FALSE(bad.view().overall);
  }
  PhaseDeadlines negative(policy, tick(std::numeric_limits<Rep>::min() + 1));
  ASSERT_TRUE(negative.view().overall); ASSERT_TRUE(negative.begin_startup(*negative.view().origin));
  EXPECT_EQ(tick(std::numeric_limits<Rep>::min() + 1) + std::chrono::seconds{30}, negative.view().overall);
  EXPECT_EQ(tick(std::numeric_limits<Rep>::min() + 1) + std::chrono::seconds{10}, negative.view().startup);
}
TEST(MySqlQ6PreflightDeadlinesTest, ClockOrderAndFirstFaultSurviveIndependentCleanup) {
  PhaseDeadlines p(policy, at(4)); EXPECT_FALSE(p.begin_startup(at(3)));
  EXPECT_EQ(Fault::ClockOrder, p.view().first_fault); ASSERT_TRUE(p.begin_cleanup(at(5)));
  EXPECT_EQ(at(10), p.view().cleanup); ASSERT_TRUE(p.finish_cleanup(at(9)));
  EXPECT_EQ(Fault::ClockOrder, p.view().first_fault); EXPECT_EQ(Fault::None, p.view().cleanup_fault);
  EXPECT_FALSE(p.view().cleanup_uncertain); EXPECT_TRUE(p.view().cleanup_complete);
  PhaseDeadlines q(policy, at(0)); ASSERT_TRUE(q.begin_startup(at(0)));
  EXPECT_FALSE(q.finish_startup(at(10))); ASSERT_TRUE(q.begin_cleanup(at(11)));
  EXPECT_EQ(at(16), q.view().cleanup); EXPECT_FALSE(q.finish_cleanup(at(9)));
  EXPECT_EQ(Fault::Expired, q.view().first_fault); EXPECT_EQ(Fault::ClockOrder, q.view().cleanup_fault);
  EXPECT_TRUE(q.view().cleanup_uncertain); EXPECT_EQ(at(16), q.view().cleanup);
}
TEST(MySqlQ6PreflightDeadlinesTest, FirstCleanupCutoffNeverExtendsPastOverallOrRenews) {
  for (auto first : {at(3), at(30), at(35), at(100)}) {
    PhaseDeadlines p(policy, at(0)); EXPECT_EQ(first < at(35), p.begin_cleanup(first));
    EXPECT_EQ(first == at(3) ? at(8) : at(35), p.view().cleanup);
    const auto original = p.view().cleanup;
    EXPECT_FALSE(p.begin_cleanup(at(101))); EXPECT_EQ(original, p.view().cleanup);
    EXPECT_EQ(first < at(35) ? Fault::RepeatedCleanup : Fault::Expired, p.view().cleanup_fault); EXPECT_TRUE(p.view().cleanup_uncertain);
  }
  PhaseDeadlines q(policy, at(0)); EXPECT_FALSE(q.begin_cleanup(Time::max()));
  EXPECT_TRUE(q.view().cleanup_attempted); EXPECT_FALSE(q.view().cleanup);
  EXPECT_FALSE(q.begin_cleanup(at(1))); EXPECT_FALSE(q.view().cleanup);
  EXPECT_EQ(Fault::InvalidTime, q.view().first_fault); EXPECT_EQ(Fault::InvalidTime, q.view().cleanup_fault);
}
TEST(MySqlQ6PreflightDeadlinesTest, OwningViewsCannotMutatePlanAndNoFreshStartupIsPossible) {
  PhaseDeadlines p(policy, at(0)); ASSERT_TRUE(p.begin_startup(at(2)));
  auto copy = p.view(); copy.overall = at(99); copy.startup.reset(); copy.first_fault = Fault::Expired;
  EXPECT_EQ(at(30), p.view().overall); EXPECT_EQ(at(12), p.view().startup); EXPECT_EQ(Fault::None, p.view().first_fault);
  ASSERT_TRUE(p.finish_startup(at(4))); EXPECT_FALSE(p.begin_startup(at(5)));
  EXPECT_EQ(at(12), p.view().startup); EXPECT_EQ(Fault::RepeatedStartup, p.view().first_fault);
  EXPECT_FALSE(p.finish_startup(at(6))); ASSERT_TRUE(p.begin_cleanup(at(7)));
  EXPECT_EQ(at(12), p.view().cleanup); ASSERT_TRUE(p.finish_cleanup(at(8)));
  EXPECT_EQ(Fault::RepeatedStartup, p.view().first_fault);
}
TEST(MySqlQ6PreflightDeadlinesTest, MisuseAndCleanupEqualityRetainIndependentUncertainty) {
  PhaseDeadlines missing(policy, at(0)); EXPECT_FALSE(missing.finish_startup(at(1)));
  EXPECT_EQ(Fault::WrongOrder, missing.view().first_fault); EXPECT_FALSE(missing.view().startup);
  PhaseDeadlines after(policy, at(0)); ASSERT_TRUE(after.begin_cleanup(at(1)));
  EXPECT_FALSE(after.begin_startup(at(2))); EXPECT_EQ(Fault::WrongOrder, after.view().first_fault);
  EXPECT_FALSE(after.finish_cleanup(at(6))); EXPECT_EQ(Fault::Expired, after.view().cleanup_fault);
  EXPECT_EQ(Fault::WrongOrder, after.view().first_fault); EXPECT_TRUE(after.view().cleanup_uncertain);
  PhaseDeadlines interrupted(policy, at(0)); ASSERT_TRUE(interrupted.begin_startup(at(0)));
  ASSERT_TRUE(interrupted.begin_cleanup(at(1))); EXPECT_FALSE(interrupted.finish_startup(at(2)));
  EXPECT_EQ(Fault::WrongOrder, interrupted.view().first_fault); EXPECT_FALSE(interrupted.view().startup_complete);
  PhaseDeadlines repeated(policy, at(0)); ASSERT_TRUE(repeated.begin_cleanup(at(0)));
  ASSERT_TRUE(repeated.finish_cleanup(at(4))); EXPECT_FALSE(repeated.finish_cleanup(at(4)));
  EXPECT_EQ(Fault::RepeatedCleanup, repeated.view().cleanup_fault); EXPECT_TRUE(repeated.view().cleanup_complete);
  EXPECT_TRUE(repeated.view().cleanup_uncertain); EXPECT_EQ(at(5), repeated.view().cleanup);
}

TEST(MySqlQ6PreflightDeadlinesTest, PrematureCleanupFinishIsTerminalWithoutNewCutoffOrProgress) {
  PhaseDeadlines fresh(policy, at(0));
  EXPECT_FALSE(fresh.finish_cleanup(at(1)));
  const auto rejected = fresh.view();
  EXPECT_EQ(Fault::WrongOrder, rejected.first_fault); EXPECT_EQ(Fault::WrongOrder, rejected.cleanup_fault);
  EXPECT_TRUE(rejected.cleanup_uncertain); EXPECT_FALSE(rejected.cleanup_attempted);
  EXPECT_FALSE(rejected.cleanup); EXPECT_FALSE(rejected.cleanup_complete); EXPECT_EQ(at(0), rejected.highwater);
  for (auto recovery : {at(2), at(3), at(0), Time::max()}) {
    EXPECT_FALSE(fresh.begin_cleanup(recovery)); EXPECT_FALSE(fresh.finish_cleanup(recovery));
    const auto after = fresh.view();
    EXPECT_FALSE(after.cleanup); EXPECT_FALSE(after.cleanup_attempted); EXPECT_FALSE(after.cleanup_complete);
    EXPECT_EQ(rejected.first_fault, after.first_fault); EXPECT_EQ(rejected.cleanup_fault, after.cleanup_fault);
    EXPECT_TRUE(after.cleanup_uncertain); EXPECT_EQ(rejected.highwater, after.highwater);
    EXPECT_EQ(at(0), after.origin); EXPECT_EQ(at(30), after.overall);
  }
  EXPECT_FALSE(fresh.begin_startup(at(4))); EXPECT_FALSE(fresh.finish_startup(at(5)));
  EXPECT_FALSE(fresh.view().startup); EXPECT_FALSE(fresh.view().cleanup);

  PhaseDeadlines active(policy, at(0)); ASSERT_TRUE(active.begin_startup(at(0)));
  EXPECT_FALSE(active.finish_startup(at(10))); EXPECT_EQ(Fault::Expired, active.view().first_fault);
  EXPECT_FALSE(active.finish_cleanup(at(11))); EXPECT_EQ(Fault::WrongOrder, active.view().cleanup_fault);
  EXPECT_FALSE(active.begin_cleanup(at(12))); EXPECT_FALSE(active.finish_cleanup(at(13)));
  EXPECT_EQ(Fault::Expired, active.view().first_fault); EXPECT_EQ(Fault::WrongOrder, active.view().cleanup_fault);
  EXPECT_TRUE(active.view().cleanup_uncertain); EXPECT_FALSE(active.view().cleanup);
  EXPECT_FALSE(active.view().cleanup_attempted); EXPECT_FALSE(active.view().cleanup_complete);
  EXPECT_EQ(at(10), active.view().highwater); EXPECT_EQ(at(10), active.view().startup);

  // An ACTIVE fault alone still permits independent cleanup under frozen C.
  PhaseDeadlines allowed(policy, at(0)); ASSERT_TRUE(allowed.begin_startup(at(0)));
  EXPECT_FALSE(allowed.finish_startup(at(10))); ASSERT_TRUE(allowed.begin_cleanup(at(11)));
  EXPECT_EQ(at(16), allowed.view().cleanup); ASSERT_TRUE(allowed.finish_cleanup(at(15)));
  EXPECT_EQ(Fault::Expired, allowed.view().first_fault); EXPECT_EQ(Fault::None, allowed.view().cleanup_fault);
  EXPECT_TRUE(allowed.view().cleanup_complete); EXPECT_FALSE(allowed.view().cleanup_uncertain);
}
