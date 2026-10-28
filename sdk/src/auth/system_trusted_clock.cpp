#include "odbcpp/auth/trusted_clock_sample.h"
#include <algorithm>
#include <limits>
#if defined(__APPLE__)
#include <sys/timex.h>
#include <time.h>
#endif
namespace rs::core::auth {
#if defined(__APPLE__)
namespace {
using namespace std::chrono;
class DarwinElapsed final : public SuspendAwareClock {
 public:
  std::optional<microseconds> read() noexcept override {
    timespec time{};
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &time) || time.tv_sec < 0 ||
        time.tv_nsec < 0 || time.tv_nsec >= 1000000000 ||
        time.tv_sec > (std::numeric_limits<std::int64_t>::max() - 999999) / 1000000)
      return std::nullopt;
    return microseconds{time.tv_sec * 1000000 + time.tv_nsec / 1000};
  }
};
class DarwinUtc final : public TrustedUtcSource {
 public:
  std::shared_ptr<SuspendAwareClock> elapsed_clock() override { return elapsed_; }
  std::optional<TrustedUtcReading> read(microseconds horizon) override {
    // modes=0 is a read-only query. Reject leap/unsynchronized states, hardware
    // faults and missing discipline; no service startup or privileged update.
    timex quality{};
    ntptimeval value{};
    if (::ntp_adjtime(&quality) != TIME_OK || ::ntp_gettime(&value) != TIME_OK ||
        value.time_state != TIME_OK || !(quality.status & (STA_PLL | STA_FLL)) ||
        (quality.status & (STA_UNSYNC | STA_CLOCKERR | STA_INS | STA_DEL | STA_PPSERROR |
                           STA_PPSJITTER | STA_PPSWANDER)) ||
        quality.tolerance <= 0 || value.maxerror < 0 || value.esterror < 0 ||
        quality.maxerror < 0 || quality.esterror < 0 ||
        horizon <= microseconds::zero() || horizon > hours{24}) return std::nullopt;
    // Darwin exports MAXFREQ * SCALE_PPM, where SCALE_PPM is deliberately
    // truncated (65, not 65.536). Invert that actual kernel ABI, rather than
    // underestimating its 500us/s error-growth bound as generic scaled ppm.
    // See XNU kern_ntptime.c ntp_adjtime/ntp_gettime1 and sys/timex.h.
    constexpr std::int64_t denominator = static_cast<std::int64_t>(SCALE_PPM) * 1000000000;
    const auto tolerance = static_cast<std::int64_t>(quality.tolerance);
    if (tolerance > (std::numeric_limits<std::int64_t>::max() - denominator + 1) / horizon.count())
      return std::nullopt;
    const auto drift = (horizon.count() * tolerance + denominator - 1) / denominator;
    const auto uncertainty = std::max({value.maxerror, value.esterror, quality.maxerror, quality.esterror});
    if (uncertainty == std::numeric_limits<std::int64_t>::max() || value.time.tv_sec < 0 ||
        value.time.tv_nsec < 0 || value.time.tv_nsec >= 1000000000 ||
        value.time.tv_sec > (std::numeric_limits<std::int64_t>::max() - 999999) / 1000000)
      return std::nullopt;
    return TrustedUtcReading{{value.time.tv_sec * 1000000 + value.time.tv_nsec / 1000},
        microseconds{uncertainty + 1}, microseconds{drift}, horizon};
  }
 private:
  std::shared_ptr<SuspendAwareClock> elapsed_ = std::make_shared<DarwinElapsed>();
};
}
#endif
std::shared_ptr<TrustedUtcSource> system_trusted_utc_source() {
#if defined(__APPLE__)
  return std::make_shared<DarwinUtc>();
#else
  return {};
#endif
}
} // namespace rs::core::auth
