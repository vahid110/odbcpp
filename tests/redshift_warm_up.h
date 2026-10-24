#pragma once

// Live-fixture warm-up deadline arithmetic (test-only). One absolute deadline
// governs every blocking phase of an attempt. The ODBC boundary and the clock
// are injected so the arithmetic is checked offline (test_redshift_warm_up).

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace odbcpp::test::redshift_warm_up {

using Clock = std::chrono::steady_clock;

// Whole seconds a phase may use before `deadline`, at most `cap`. Zero means
// "do not start": ODBC reads a zero timeout as unlimited, so an expired or
// subsecond remainder must never become one.
constexpr std::uintptr_t phase_seconds(Clock::time_point deadline, Clock::time_point now,
                                       std::uintptr_t cap) {
  if (now >= deadline) return 0;
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(deadline - now).count();
  return seconds > 0 ? std::min<std::uintptr_t>(static_cast<std::uintptr_t>(seconds), cap) : 0;
}

// One attempt: connect, execute SELECT 1, fetch. Each phase receives only the
// time left before the original deadline, and a success completed at or after
// the deadline is not readiness. `boundary` owns its handles and releases them
// when destroyed, so every return path cleans up.
template <class Now, class Boundary>
bool attempt(Now now, Clock::time_point deadline, std::uintptr_t cap, Boundary& boundary) {
  auto seconds = phase_seconds(deadline, now(), cap);
  if (seconds == 0 || !boundary.connect(seconds)) return false;
  seconds = phase_seconds(deadline, now(), cap);
  if (seconds == 0 || !boundary.execute(seconds)) return false;
  seconds = phase_seconds(deadline, now(), cap);
  if (seconds == 0 || !boundary.fetch(seconds)) return false;
  return now() < deadline;
}

}  // namespace odbcpp::test::redshift_warm_up
