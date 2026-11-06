#include "odbcpp/auth/trusted_clock_sample.h"
#include <limits>

namespace rs::core::auth {
namespace {
using namespace std::chrono;
bool limit(microseconds value) { return value >= microseconds::zero() && value <= hours{24}; }
bool finite(rs::util::Deadline t) { return t != rs::util::Deadline::min() && t != rs::util::Deadline::max(); }
}
TrustedClockResult sample_trusted_clock(TrustedUtcSource& source,
    const TemporaryTimePolicy& policy, const MonotonicReader& monotonic) noexcept {
  using namespace std::chrono;
  if (!monotonic || !limit(policy.max_age) || !limit(policy.max_bracket) ||
      !limit(policy.max_total_error) || !limit(policy.max_horizon) ||
      policy.max_horizon == microseconds::zero()) return TimeFailure::InvalidInput;
  try {
    auto elapsed = source.elapsed_clock();
    if (!elapsed) return TimeFailure::UnknownClockQuality;
    const auto elapsed_before = elapsed->read();
    const auto before = monotonic();
    const auto reading = source.read(policy.max_horizon);
    const auto after = monotonic();
    const auto elapsed_after = elapsed->read();
    if (!reading || !reading->utc_uncertainty || !reading->drift_error ||
        !elapsed_before || !elapsed_after) return TimeFailure::UnknownClockQuality;
    if (!finite(before) || !finite(after) || elapsed_before->count() < 0)
      return TimeFailure::InvalidInput;
    if (after < before || *elapsed_after < *elapsed_before) return TimeFailure::ClockRollback;
    const auto cap = duration_cast<rs::util::Clock::duration>(policy.max_bracket);
    if (before.time_since_epoch().count() >
        std::numeric_limits<rs::util::Clock::duration::rep>::max() - cap.count() ||
        elapsed_before->count() > std::numeric_limits<std::int64_t>::max() - policy.max_bracket.count())
      return TimeFailure::Overflow;
    if (after > before + cap || *elapsed_after > *elapsed_before + policy.max_bracket)
      return TimeFailure::StaleSample;
    if (!limit(reading->quality_horizon) || reading->quality_horizon < policy.max_horizon)
      return TimeFailure::UnknownClockQuality;
    auto total = microseconds::zero();
    for (const auto bound : {*reading->utc_uncertainty, *reading->drift_error}) {
      if (bound < microseconds::zero()) return TimeFailure::InvalidInput;
      if (bound > policy.max_total_error - total) return TimeFailure::UnknownClockQuality;
      total += bound;
    }
    // Zero additional suspend error is justified ONLY in conjunction with the
    // retained elapsed guard. Consumers must enforce both bounds at every use.
    TemporaryClockSample time{reading->utc, before, after, after, after,
        reading->utc_uncertainty, reading->drift_error, microseconds::zero(), std::nullopt};
    return TrustedClockSample{time, std::move(elapsed), *elapsed_before, policy.max_horizon};
  } catch (...) { return TimeFailure::UnknownClockQuality; }
}
TrustedClockGuard::TrustedClockGuard(std::shared_ptr<SuspendAwareClock> clock,
    std::chrono::microseconds before, std::chrono::microseconds until)
    : clock_(std::move(clock)), last_(before), until_(until) {}
bool TrustedClockGuard::valid() noexcept {
  std::lock_guard lock{mutex_};
  if (failed_) return false;
  const auto now = clock_->read();
  if (!now || *now < last_ || *now >= until_) { failed_ = true; return false; }
  last_ = *now;
  return true;
}
std::shared_ptr<TrustedClockGuard> retain_trusted_clock(
    const TrustedClockSample& sample, rs::util::Deadline expiry) noexcept {
  using namespace std::chrono;
  try {
    if (!sample.elapsed || !finite(sample.time.before) || !finite(expiry) ||
        sample.elapsed_before.count() < 0 || !limit(sample.horizon) || expiry <= sample.time.before)
      return {};
    const auto cap = duration_cast<rs::util::Clock::duration>(sample.horizon);
    if (sample.time.before.time_since_epoch().count() >
        std::numeric_limits<rs::util::Clock::duration::rep>::max() - cap.count() ||
        expiry > sample.time.before + cap) return {};
    const auto remaining = duration_cast<microseconds>(expiry - sample.time.before);
    if (remaining <= microseconds::zero() || sample.elapsed_before.count() >
        std::numeric_limits<std::int64_t>::max() - remaining.count()) return {};
    auto guard = std::shared_ptr<TrustedClockGuard>(new TrustedClockGuard{
        sample.elapsed, sample.elapsed_before, sample.elapsed_before + remaining});
    return guard->valid() ? guard : nullptr;
  } catch (...) { return {}; }
}
} // namespace rs::core::auth
