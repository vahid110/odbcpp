#pragma once
#include "odbcpp/auth/auth_core.h"
#include <cstdint>

namespace rs::core::auth {

// Pure one-acquisition conversion only. These input values are NOT issuer/clock
// proof: the explicitly selected trusted issuer/sampler owns their provenance.
// Never resample an existing credential to extend its published monotonic bound.
struct UtcInstant { std::int64_t microseconds_since_epoch; };
struct TemporaryClockSample {
  UtcInstant utc;
  rs::util::Deadline before, after, acquisition_completed, now;
  // Quantitative upper bounds through the policy horizon, supplied by the
  // trusted sampler. Absence refuses; explicit zero is not inferred by default.
  std::optional<std::chrono::microseconds> utc_uncertainty;
  std::optional<std::chrono::microseconds> drift_error;
  std::optional<std::chrono::microseconds> suspend_error;
  std::optional<rs::util::Deadline> previous_monotonic_sample;
};
struct TemporaryTimePolicy {
  std::chrono::microseconds max_age, max_bracket, max_total_error, max_horizon;
};
enum class TimeFailure {
  UnsupportedMethod, InvalidInput, UnknownClockQuality, Overflow,
  StaleSample, ClockRollback, Expired, Cancelled
};
class TimeConversion final {
 public:
  TimeConversion(Validity value) : value_(value) {}
  TimeConversion(TimeFailure failure) : value_(failure) {}
  explicit operator bool() const noexcept { return std::holds_alternative<Validity>(value_); }
  Validity value() const { return std::get<Validity>(value_); }
  TimeFailure failure() const { return std::get<TimeFailure>(value_); }
  static std::string_view safe_message(TimeFailure) noexcept;
 private:
  std::variant<Validity, TimeFailure> value_;
};
// max policy durations are local arithmetic/resource limits, not service TTLs.
// Freshness is now-before; readiness is acquisition_completed, not after.
TimeConversion convert_temporary_db_validity(const Request&, UtcInstant expiry,
    const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* cancelled = nullptr) noexcept;
} // namespace rs::core::auth
