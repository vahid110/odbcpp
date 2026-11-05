#include "odbcpp/auth/trusted_clock_sample.h"
#include <gtest/gtest.h>
#include <limits>
using namespace std::chrono_literals;
namespace {
namespace a = rs::core::auth;
using rs::util::Deadline;
using rs::util::Clock;
struct Elapsed final : a::SuspendAwareClock {
  std::optional<std::chrono::microseconds> now{100s};
  std::optional<std::chrono::microseconds> read() noexcept override { return now; }
};
struct Source final : a::TrustedUtcSource {
  std::shared_ptr<Elapsed> elapsed = std::make_shared<Elapsed>();
  std::optional<a::TrustedUtcReading> value{{{1000000000}, 3s, 1s, 3600s}};
  std::function<void()> during;
  std::optional<a::TrustedUtcReading> read(std::chrono::microseconds) override {
    if (during) during();
    return value;
  }
  std::shared_ptr<a::SuspendAwareClock> elapsed_clock() override { return elapsed; }
};
a::TemporaryTimePolicy policy() { return {10s, 2s, 10s, 3600s}; }
a::TrustedClockResult sample(Source& s) {
  return a::sample_trusted_clock(s, policy(), [] { return Deadline{100s}; });
}
TEST(TrustedClock, CapturesUtcQualityAndConservativelyRetainsExclusiveExpiry) {
  Source s;
  auto result = sample(s);
  ASSERT_TRUE(std::holds_alternative<a::TrustedClockSample>(result));
  auto selected = std::get<a::TrustedClockSample>(result);
  auto validity = a::convert_temporary_validity(Deadline{120s}, 1s, {1060000000}, selected.time, policy());
  ASSERT_TRUE(validity);
  EXPECT_EQ(validity.value().expires_at, Deadline{156s});
  auto guard = a::retain_trusted_clock(selected, validity.value().expires_at);
  ASSERT_TRUE(guard);
  s.elapsed->now = 155999999us;
  EXPECT_TRUE(guard->valid());
  s.elapsed->now = 156s;
  EXPECT_FALSE(guard->valid());
  s.elapsed->now = 101s;
  EXPECT_FALSE(guard->valid()); // Expiry cannot heal or be resampled.
}
TEST(TrustedClock, MissingQualityHorizonAndExcessiveBoundsRefuse) {
  for (unsigned kind = 0; kind != 7; ++kind) {
    SCOPED_TRACE(kind);
    Source s;
    if (kind == 0) s.value.reset();
    if (kind == 1) s.value->utc_uncertainty.reset();
    if (kind == 2) s.value->drift_error.reset();
    if (kind == 3) s.value->quality_horizon = 3599s;
    if (kind == 4) s.value->utc_uncertainty = 10s;
    if (kind == 5) s.elapsed.reset();
    if (kind == 6) s.elapsed->now.reset();
    const auto result = sample(s);
    ASSERT_TRUE(std::holds_alternative<a::TimeFailure>(result));
    EXPECT_EQ(std::get<a::TimeFailure>(result), a::TimeFailure::UnknownClockQuality);
  }
}
TEST(TrustedClock, SlowUtcWithKnownBoundDoesNotExtendExpiry) {
  Source s;
  s.during = [&] { s.elapsed->now = 102s; };
  unsigned reads{};
  auto result = a::sample_trusted_clock(s, policy(), [&] { return Deadline{reads++ ? 102s : 100s}; });
  ASSERT_TRUE(std::holds_alternative<a::TrustedClockSample>(result));
  auto selected = std::get<a::TrustedClockSample>(result);
  auto validity = a::convert_temporary_validity(Deadline{120s}, 1s, {1060000000}, selected.time, policy());
  ASSERT_TRUE(validity);
  EXPECT_EQ(validity.value().expires_at, Deadline{156s});
}
TEST(TrustedClock, SlowReadSuspendRollbackAndRangeRefuse) {
  for (unsigned kind = 0; kind != 6; ++kind) {
    SCOPED_TRACE(kind);
    Source s;
    unsigned reads{};
    if (kind == 1) s.during = [&] { s.elapsed->now = 103s; };
    if (kind == 2) s.during = [&] { s.elapsed->now = 99s; };
    if (kind == 5) s.value->utc_uncertainty = -1us;
    auto result = a::sample_trusted_clock(s, policy(), [&] {
      if (kind == 3) return Deadline::max();
      if (kind == 4) return Deadline{Clock::duration{std::numeric_limits<Clock::duration::rep>::max() - 1}};
      return Deadline{kind == 0 && reads++ ? 103s : 100s};
    });
    ASSERT_TRUE(std::holds_alternative<a::TimeFailure>(result));
    const auto expected = kind < 2 ? a::TimeFailure::StaleSample : kind == 2 ? a::TimeFailure::ClockRollback :
        kind == 4 ? a::TimeFailure::Overflow : a::TimeFailure::InvalidInput;
    EXPECT_EQ(std::get<a::TimeFailure>(result), expected);
  }
  Source s;
  unsigned reads{};
  auto rollback = a::sample_trusted_clock(s, policy(), [&] { return Deadline{reads++ ? 99s : 100s}; });
  EXPECT_EQ(std::get<a::TimeFailure>(rollback), a::TimeFailure::ClockRollback);
}
TEST(TrustedClock, SuspendGuardRejectsElapsedExpiryWhileSteadyClockHasNotAdvanced) {
  Source s;
  auto selected = std::get<a::TrustedClockSample>(sample(s));
  auto guard = a::retain_trusted_clock(selected, Deadline{150s});
  ASSERT_TRUE(guard);
  s.elapsed->now = 200s; // Simulated sleep: steady sample remains at 100s.
  EXPECT_FALSE(guard->valid());
}
TEST(TrustedClock, GuardMissingReadRollbackAndOverflowDoNotHeal) {
  for (unsigned kind = 0; kind != 2; ++kind) {
    Source s;
    auto guard = a::retain_trusted_clock(std::get<a::TrustedClockSample>(sample(s)), Deadline{150s});
    ASSERT_TRUE(guard);
    if (kind == 0) s.elapsed->now.reset();
    else s.elapsed->now = 99s;
    EXPECT_FALSE(guard->valid());
    s.elapsed->now = 101s;
    EXPECT_FALSE(guard->valid());
  }
  Source s;
  auto selected = std::get<a::TrustedClockSample>(sample(s));
  selected.elapsed_before = std::chrono::microseconds::max();
  EXPECT_FALSE(a::retain_trusted_clock(selected, Deadline{150s}));
  selected.elapsed_before = 100s;
  EXPECT_FALSE(a::retain_trusted_clock(selected, Deadline::max()));
  EXPECT_FALSE(a::retain_trusted_clock(selected, Deadline{100s}));
  EXPECT_FALSE(a::retain_trusted_clock(selected, Deadline{4000s}));
}
TEST(TrustedClock, UtcRangeOverflowAndStaleAcquisitionStayRefused) {
  Source s;
  auto selected = std::get<a::TrustedClockSample>(sample(s));
  selected.time.utc = {std::numeric_limits<std::int64_t>::min()};
  auto overflow = a::convert_temporary_validity(Deadline{120s}, 1s,
      {std::numeric_limits<std::int64_t>::max()}, selected.time, policy());
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.failure(), a::TimeFailure::Overflow);
  selected = std::get<a::TrustedClockSample>(sample(s));
  selected.time.acquisition_completed = selected.time.now = Deadline{111s};
  auto stale = a::convert_temporary_validity(Deadline{120s}, 1s, {1060000000}, selected.time, policy());
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.failure(), a::TimeFailure::StaleSample);
}
TEST(TrustedClock, SystemProviderIsExplicitAndFailClosed) {
  auto source = a::system_trusted_utc_source();
#if defined(__APPLE__)
  ASSERT_TRUE(source);
  auto result = a::sample_trusted_clock(*source, policy());
  if (std::holds_alternative<a::TimeFailure>(result)) {
    GTEST_SKIP() << "Host clock quality unavailable; no synthetic or live UTC proof inferred";
  }
  const auto& selected = std::get<a::TrustedClockSample>(result);
  EXPECT_GT(selected.time.utc.microseconds_since_epoch, 0);
  ASSERT_TRUE(selected.time.utc_uncertainty);
  ASSERT_TRUE(selected.time.drift_error);
  EXPECT_GE(*selected.time.utc_uncertainty, 1us);
  EXPECT_GT(*selected.time.drift_error, 0us);
  EXPECT_TRUE(a::retain_trusted_clock(selected, selected.time.before + 60s));
#else
  EXPECT_FALSE(source);
#endif
}
}
