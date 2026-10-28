#pragma once
#include "odbcpp/auth/temporary_db_validity.h"
#include <functional>
#include <memory>
#include <mutex>

namespace rs::core::auth {
// Elapsed time must advance during suspend. Its rate error is included in the
// source's drift bound. Missing reads or rollback permanently retire a guard.
class SuspendAwareClock {
 public:
  virtual ~SuspendAwareClock() = default;
  virtual std::optional<std::chrono::microseconds> read() noexcept = 0;
};
struct TrustedUtcReading {
  UtcInstant utc;
  std::optional<std::chrono::microseconds> utc_uncertainty, drift_error;
  std::chrono::microseconds quality_horizon;
};
class TrustedUtcSource {
 public:
  virtual ~TrustedUtcSource() = default;
  // Bounds cover the requested horizon. Host/provider owns their provenance;
  // an ordinary wall-clock read cannot establish these bounds.
  virtual std::optional<TrustedUtcReading> read(std::chrono::microseconds horizon) = 0;
  virtual std::shared_ptr<SuspendAwareClock> elapsed_clock() = 0;
};
struct TrustedClockSample {
  TemporaryClockSample time;
  std::shared_ptr<SuspendAwareClock> elapsed;
  std::chrono::microseconds elapsed_before, horizon;
};
using TrustedClockResult = std::variant<TrustedClockSample, TimeFailure>;
using MonotonicReader = std::function<rs::util::Deadline()>;
TrustedClockResult sample_trusted_clock(TrustedUtcSource&, const TemporaryTimePolicy&,
    const MonotonicReader& = [] { return rs::util::Clock::now(); }) noexcept;
class TrustedClockGuard final {
 public:
  // Additional mandatory bound, not a replacement for the steady Validity.
  bool valid() noexcept;
 private:
  friend std::shared_ptr<TrustedClockGuard> retain_trusted_clock(
      const TrustedClockSample&, rs::util::Deadline) noexcept;
  TrustedClockGuard(std::shared_ptr<SuspendAwareClock>, std::chrono::microseconds,
      std::chrono::microseconds);
  std::shared_ptr<SuspendAwareClock> clock_;
  std::chrono::microseconds last_, until_;
  std::mutex mutex_;
  bool failed_{};
};
// Retain the already-converted expiry; never resample UTC or extend it.
std::shared_ptr<TrustedClockGuard> retain_trusted_clock(
    const TrustedClockSample&, rs::util::Deadline converted_expiry) noexcept;
// Darwin kernel-disciplined UTC + suspend-aware elapsed clock. Unsupported or
// unsynchronized hosts refuse; this neither starts NTP nor changes clock state.
std::shared_ptr<TrustedUtcSource> system_trusted_utc_source();
} // namespace rs::core::auth
