#include "temporary_db_validity.h"
#include <limits>
#include <ratio>

namespace rs::core::auth {
namespace {
using Rep = rs::util::Clock::duration::rep;
static_assert(std::numeric_limits<Rep>::is_integer && std::numeric_limits<Rep>::is_signed);
bool finite(rs::util::Deadline t) noexcept {
  return t != rs::util::Deadline::min() && t != rs::util::Deadline::max();
}
template<class T> bool add(T a, T b, T& out) noexcept {
  if ((b > 0 && a > std::numeric_limits<T>::max() - b) ||
      (b < 0 && a < std::numeric_limits<T>::min() - b)) return false;
  out = a + b; return true;
}
template<class T> bool subtract(T a, T b, T& out) noexcept {
  if ((b > 0 && a < std::numeric_limits<T>::min() + b) ||
      (b < 0 && a > std::numeric_limits<T>::max() + b)) return false;
  out = a - b; return true;
}
bool to_ticks(std::int64_t micros, Rep& result) noexcept {
  using Ratio = std::ratio_divide<std::micro, rs::util::Clock::period>;
  if (micros < 0 || micros > std::numeric_limits<std::int64_t>::max() / Ratio::num) return false;
  const auto ticks = (micros * Ratio::num) / Ratio::den; // positive: rounds down
  if (ticks > std::numeric_limits<Rep>::max()) return false;
  result = static_cast<Rep>(ticks); return true;
}
Rep ticks(rs::util::Deadline t) noexcept { return t.time_since_epoch().count(); }
bool policy_limit(std::chrono::microseconds value) noexcept {
  return value >= std::chrono::microseconds::zero() && value <= std::chrono::hours{24};
}
}
std::string_view TimeConversion::safe_message(TimeFailure failure) noexcept {
  switch (failure) {
    case TimeFailure::UnsupportedMethod: return "Temporary credential method unsupported";
    case TimeFailure::InvalidInput: return "Temporary credential time input malformed";
    case TimeFailure::UnknownClockQuality: return "Temporary credential clock quality unavailable";
    case TimeFailure::Overflow: return "Temporary credential time range invalid";
    case TimeFailure::StaleSample: return "Temporary credential time sample stale";
    case TimeFailure::ClockRollback: return "Temporary credential monotonic order invalid";
    case TimeFailure::Expired: return "Temporary credential validity expired";
    case TimeFailure::Cancelled: return "Temporary credential conversion cancelled";
  }
  return "Temporary credential time input malformed";
}
TimeConversion convert_temporary_db_validity(const Request& request, UtcInstant expiry,
    const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) noexcept {
  if (cancel && cancel->stop_requested()) return TimeFailure::Cancelled;
  if (request.binding().invariant_error()) return TimeFailure::InvalidInput;
  if (request.binding().method() != Method::TemporaryDatabasePassword ||
      request.binding().target().service != Service::Redshift) return TimeFailure::UnsupportedMethod;
  if (!policy_limit(policy.max_age) || !policy_limit(policy.max_bracket) ||
      !policy_limit(policy.max_total_error) || !policy_limit(policy.max_horizon) ||
      policy.max_horizon == std::chrono::microseconds::zero() ||
      !finite(request.deadline()) || request.headroom() < rs::util::Clock::duration::zero() ||
      request.headroom() > std::chrono::hours{24}) return TimeFailure::InvalidInput;
  if (!finite(sample.before) || !finite(sample.after) || !finite(sample.acquisition_completed) ||
      !finite(sample.now) || (sample.previous_monotonic_sample && !finite(*sample.previous_monotonic_sample)))
    return TimeFailure::InvalidInput;
  if (sample.before > sample.after || sample.after > sample.acquisition_completed ||
      sample.acquisition_completed > sample.now ||
      (sample.previous_monotonic_sample && sample.before < *sample.previous_monotonic_sample))
    return TimeFailure::ClockRollback;
  if (!sample.utc_uncertainty || !sample.drift_error || !sample.suspend_error)
    return TimeFailure::UnknownClockQuality;
  std::int64_t error = 0;
  for (auto bound : {*sample.utc_uncertainty, *sample.drift_error, *sample.suspend_error}) {
    if (bound < std::chrono::microseconds::zero()) return TimeFailure::InvalidInput;
    if (bound > policy.max_total_error) return TimeFailure::UnknownClockQuality;
    if (!add(error, bound.count(), error)) return TimeFailure::Overflow;
  }
  if (error > policy.max_total_error.count()) return TimeFailure::UnknownClockQuality;
  Rep age = 0, bracket = 0, max_age = 0, max_bracket = 0;
  if (!subtract(ticks(sample.now), ticks(sample.before), age) ||
      !subtract(ticks(sample.after), ticks(sample.before), bracket) ||
      !to_ticks(policy.max_age.count(), max_age) || !to_ticks(policy.max_bracket.count(), max_bracket))
    return TimeFailure::Overflow;
  if (age > max_age || bracket > max_bracket) return TimeFailure::StaleSample;
  std::int64_t remaining = 0;
  if (!subtract(expiry.microseconds_since_epoch, sample.utc.microseconds_since_epoch, remaining) ||
      !subtract(remaining, error, remaining)) return TimeFailure::Overflow;
  if (remaining <= 0) return TimeFailure::Expired;
  if (remaining > policy.max_horizon.count()) return TimeFailure::InvalidInput;
  Rep duration = 0, converted = 0, required = 0;
  if (!to_ticks(remaining, duration) || !add(ticks(sample.before), duration, converted) ||
      !add(ticks(request.deadline()), request.headroom().count(), required)) return TimeFailure::Overflow;
  const auto until = rs::util::Deadline{rs::util::Clock::duration{converted}};
  const auto required_until = rs::util::Deadline{rs::util::Clock::duration{required}};
  if (!finite(until) || !finite(required_until)) return TimeFailure::Overflow;
  if (duration == 0 || until <= sample.now || until <= sample.acquisition_completed ||
      sample.now >= request.deadline() || required_until >= until) return TimeFailure::Expired;
  if (cancel && cancel->stop_requested()) return TimeFailure::Cancelled;
  return Validity::monotonic(sample.acquisition_completed, until);
}
} // namespace rs::core::auth
