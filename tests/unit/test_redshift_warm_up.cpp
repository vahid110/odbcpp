#include "../redshift_warm_up.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

// Offline checks of the live-fixture warm-up deadline (it_redshift_real). A
// fake clock and a recording ODBC boundary stand in for the endpoint.
namespace {

namespace warm_up = odbcpp::test::redshift_warm_up;
using std::chrono::milliseconds;
using std::chrono::seconds;

struct FakeClock {
  warm_up::Clock::time_point now{};
  auto reader() {
    return [this] { return now; };
  }
};

// Records the seconds each phase receives and advances the fake clock by the
// configured phase durations; every phase reports success.
struct RecordingBoundary {
  explicit RecordingBoundary(FakeClock& fake_clock, milliseconds connect = milliseconds{0},
                             milliseconds execute = milliseconds{0},
                             milliseconds fetch = milliseconds{0})
      : clock(fake_clock), connect_takes(connect), execute_takes(execute), fetch_takes(fetch) {}

  FakeClock& clock;
  milliseconds connect_takes;
  milliseconds execute_takes;
  milliseconds fetch_takes;
  std::vector<std::uintptr_t> connect_budget;
  std::vector<std::uintptr_t> execute_budget;
  std::vector<std::uintptr_t> fetch_budget;

  bool connect(std::uintptr_t budget) {
    connect_budget.push_back(budget);
    clock.now += connect_takes;
    return true;
  }
  bool execute(std::uintptr_t budget) {
    execute_budget.push_back(budget);
    clock.now += execute_takes;
    return true;
  }
  bool fetch(std::uintptr_t budget) {
    fetch_budget.push_back(budget);
    clock.now += fetch_takes;
    return true;
  }
};

TEST(RedshiftWarmUpTest, PhaseSecondsNeverTurnsAnExpiredOrSubsecondBudgetIntoUnlimited) {
  const warm_up::Clock::time_point now{};
  EXPECT_EQ(0u, warm_up::phase_seconds(now - milliseconds{1}, now, 30));
  EXPECT_EQ(0u, warm_up::phase_seconds(now, now, 30));
  EXPECT_EQ(0u, warm_up::phase_seconds(now + milliseconds{999}, now, 30));
  EXPECT_EQ(1u, warm_up::phase_seconds(now + milliseconds{1000}, now, 30));
  EXPECT_EQ(29u, warm_up::phase_seconds(now + milliseconds{29999}, now, 30));
  EXPECT_EQ(30u, warm_up::phase_seconds(now + seconds{45}, now, 30));
}

// The review case: a 29 s remainder, a successful 28 s connect. The query must
// receive only the 1 s left, and a fetch with under a second left never starts.
TEST(RedshiftWarmUpTest, ConnectConsumingMostOfTheBudgetLeavesOnlyTheRemainder) {
  FakeClock clock;
  const auto deadline = clock.now + seconds{29};
  RecordingBoundary boundary{clock, seconds{28}, milliseconds{500}, milliseconds{0}};
  EXPECT_FALSE(warm_up::attempt(clock.reader(), deadline, 30, boundary));
  EXPECT_EQ(std::vector<std::uintptr_t>{29}, boundary.connect_budget);
  EXPECT_EQ(std::vector<std::uintptr_t>{1}, boundary.execute_budget);
  EXPECT_TRUE(boundary.fetch_budget.empty());
}

TEST(RedshiftWarmUpTest, SuccessCompletedAtOrAfterTheDeadlineIsNotReadiness) {
  for (const auto overrun : {milliseconds{0}, milliseconds{1}, milliseconds{20000}}) {
    SCOPED_TRACE(overrun.count());
    FakeClock clock;
    const auto deadline = clock.now + seconds{10};
    RecordingBoundary boundary{clock, seconds{4}, seconds{3}, seconds{3} + overrun};
    EXPECT_FALSE(warm_up::attempt(clock.reader(), deadline, 30, boundary));
    EXPECT_EQ(std::vector<std::uintptr_t>{10}, boundary.connect_budget);
    EXPECT_EQ(std::vector<std::uintptr_t>{6}, boundary.execute_budget);
    EXPECT_EQ(std::vector<std::uintptr_t>{3}, boundary.fetch_budget);
  }
}

TEST(RedshiftWarmUpTest, AttemptWithinTheDeadlineIsReadyAndEachPhaseSeesTheRemainder) {
  FakeClock clock;
  const auto deadline = clock.now + seconds{120};
  RecordingBoundary boundary{clock, seconds{95}, milliseconds{2500}, milliseconds{200}};
  EXPECT_TRUE(warm_up::attempt(clock.reader(), deadline, 30, boundary));
  EXPECT_EQ(std::vector<std::uintptr_t>{30}, boundary.connect_budget);
  EXPECT_EQ(std::vector<std::uintptr_t>{25}, boundary.execute_budget);
  EXPECT_EQ(std::vector<std::uintptr_t>{22}, boundary.fetch_budget);
}

TEST(RedshiftWarmUpTest, ExpiredDeadlineStartsNoPhase) {
  FakeClock clock;
  const auto deadline = clock.now + milliseconds{999};
  RecordingBoundary boundary{clock};
  EXPECT_FALSE(warm_up::attempt(clock.reader(), deadline, 30, boundary));
  EXPECT_TRUE(boundary.connect_budget.empty());
  EXPECT_TRUE(boundary.execute_budget.empty());
  EXPECT_TRUE(boundary.fetch_budget.empty());
}

struct FailingConnect : RecordingBoundary {
  using RecordingBoundary::RecordingBoundary;
  bool connect(std::uintptr_t budget) {
    connect_budget.push_back(budget);
    return false;
  }
};

// Connect and execute succeed instantly; fetch takes `fetch_takes`; destroying
// the boundary (handle cleanup and disconnect) advances the clock by `cleanup`.
struct SlowCleanupBoundary {
  SlowCleanupBoundary(FakeClock& fake_clock, milliseconds fetch, milliseconds release,
                      bool connects, int& released_count)
      : clock(fake_clock), fetch_takes(fetch), cleanup(release), connect_succeeds(connects),
        released(released_count) {}
  SlowCleanupBoundary(const SlowCleanupBoundary&) = delete;
  SlowCleanupBoundary& operator=(const SlowCleanupBoundary&) = delete;
  ~SlowCleanupBoundary() {
    clock.now += cleanup;
    ++released;
  }

  bool connect(std::uintptr_t) { return connect_succeeds; }
  bool execute(std::uintptr_t) { return true; }
  bool fetch(std::uintptr_t) {
    clock.now += fetch_takes;
    return true;
  }

  FakeClock& clock;
  milliseconds fetch_takes;
  milliseconds cleanup;
  bool connect_succeeds;
  int& released;
};

// The fetch finishes 1 ms before the deadline; the verdict is taken only after
// cleanup, so cleanup that reaches or passes the deadline refuses readiness.
TEST(RedshiftWarmUpTest, ReadinessIsJudgedAfterCleanupAgainstTheOriginalDeadline) {
  struct Case {
    milliseconds cleanup;
    bool ready;
  };
  for (const auto& expected : {Case{milliseconds{0}, true}, Case{milliseconds{1}, false},
                               Case{milliseconds{999}, false}, Case{milliseconds{5000}, false}}) {
    SCOPED_TRACE(expected.cleanup.count());
    FakeClock clock;
    const auto deadline = clock.now + seconds{10};
    int released = 0;
    EXPECT_EQ(expected.ready, warm_up::attempt_and_release(clock.reader(), deadline, 30, [&] {
      return SlowCleanupBoundary(clock, milliseconds{9999}, expected.cleanup, true, released);
    }));
    EXPECT_EQ(1, released);
  }
}

TEST(RedshiftWarmUpTest, CleanupBeforeTheDeadlinePreservesReadiness) {
  FakeClock clock;
  const auto deadline = clock.now + seconds{10};
  int released = 0;
  EXPECT_TRUE(warm_up::attempt_and_release(clock.reader(), deadline, 30, [&] {
    return SlowCleanupBoundary(clock, milliseconds{9000}, milliseconds{999}, true, released);
  }));
  EXPECT_EQ(1, released);
  EXPECT_LT(clock.now, deadline);
}

TEST(RedshiftWarmUpTest, FailedAttemptStillReleasesTheBoundary) {
  FakeClock clock;
  int released = 0;
  EXPECT_FALSE(warm_up::attempt_and_release(clock.reader(), clock.now + seconds{10}, 30, [&] {
    return SlowCleanupBoundary(clock, milliseconds{0}, milliseconds{0}, false, released);
  }));
  EXPECT_EQ(1, released);
}

TEST(RedshiftWarmUpTest, FailedConnectStopsTheAttempt) {
  FakeClock clock;
  FailingConnect boundary{clock};
  EXPECT_FALSE(warm_up::attempt(clock.reader(), clock.now + seconds{60}, 30, boundary));
  EXPECT_EQ(std::vector<std::uintptr_t>{30}, boundary.connect_budget);
  EXPECT_TRUE(boundary.execute_budget.empty());
  EXPECT_TRUE(boundary.fetch_budget.empty());
}

}  // namespace
