#pragma once
#include <chrono>
#include <limits>
#include <optional>
#include <type_traits>

namespace rs::tests::mysql::q6 {
// Pure caller-time arithmetic only. No clock, transport or authority source.
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
using Duration = Clock::duration;
using Rep = Duration::rep;
static_assert(std::is_integral_v<Rep> && std::is_signed_v<Rep>);
static_assert(Duration::period::num == 1 && Duration::period::den <= 1000000000);
static_assert(std::numeric_limits<Rep>::max() >= 35000000000LL);

enum class Policy { Overall30Startup10Cleanup5 = 1 };
enum class Fault { None, InvalidPolicy, InvalidTime, Overflow, ClockOrder,
                   Expired, WrongOrder, RepeatedStartup, RepeatedCleanup };
struct View {
  std::optional<Time> origin, overall, startup, cleanup, highwater;
  Fault first_fault{Fault::None}, cleanup_fault{Fault::None};
  bool startup_complete{}, cleanup_attempted{}, cleanup_complete{}, cleanup_uncertain{};
};

class PhaseDeadlines final {
 public:
  PhaseDeadlines(Policy policy, Time origin) {
    if (policy != Policy::Overall30Startup10Cleanup5) { fail(Fault::InvalidPolicy); return; }
    if (!finite(origin)) { fail(Fault::InvalidTime); return; }
    const auto overall = add(origin, active);
    // Reserve finite worst-case cleanup headroom before publishing any plan.
    if (!overall || !add(*overall, cleaning)) { fail(Fault::Overflow); return; }
    state_.origin = origin; state_.overall = *overall; state_.highwater = origin;
  }
  PhaseDeadlines() = delete;
  PhaseDeadlines(const PhaseDeadlines&) = delete;
  PhaseDeadlines& operator=(const PhaseDeadlines&) = delete;
  PhaseDeadlines(PhaseDeadlines&&) = delete;
  PhaseDeadlines& operator=(PhaseDeadlines&&) = delete;
  View view() const noexcept { return state_; }
  static constexpr bool grants_native_authority = false;
  static constexpr bool grants_hard_interruption = false;

  bool begin_startup(Time begin) noexcept {
    if (state_.first_fault != Fault::None) { return false; }
    if (state_.cleanup_attempted) { fail(Fault::WrongOrder); return false; }
    if (state_.startup) { fail(Fault::RepeatedStartup); return false; }
    if (!observe(begin, false)) { return false; }
    if (begin >= *state_.overall) { fail(Fault::Expired); return false; }
    // Compare before adding: begin+10 must never overflow, even if D is sooner.
    const auto remaining = state_.overall->time_since_epoch().count() - begin.time_since_epoch().count();
    state_.startup = remaining <= starting.count() ? *state_.overall : *add(begin, starting);
    return true;
  }
  bool finish_startup(Time completed) noexcept {
    if (state_.first_fault != Fault::None) { return false; }
    if (!state_.startup || state_.cleanup_attempted) { fail(Fault::WrongOrder); return false; }
    if (state_.startup_complete) { fail(Fault::RepeatedStartup); return false; }
    if (!observe(completed, false)) { return false; }
    if (completed >= *state_.startup || completed >= *state_.overall) {
      fail(Fault::Expired); return false;
    }
    state_.startup_complete = true;
    return true;
  }
  bool begin_cleanup(Time first) noexcept {
    if (state_.cleanup_fault != Fault::None) { return false; }
    if (state_.cleanup_attempted) { fail(Fault::RepeatedCleanup, true); return false; }
    state_.cleanup_attempted = true; // A refused attempt can never renew C.
    if (!state_.overall) { fail(Fault::WrongOrder, true); return false; }
    if (!observe(first, true)) { return false; }
    const auto base = first < *state_.overall ? first : *state_.overall;
    const auto cutoff = add(base, cleaning);
    if (!cutoff) { fail(Fault::Overflow, true); return false; }
    state_.cleanup = *cutoff;
    if (first >= *cutoff) { fail(Fault::Expired, true); return false; }
    return true;
  }
  bool finish_cleanup(Time completed) noexcept {
    if (state_.cleanup_fault != Fault::None) { return false; }
    if (!state_.cleanup) { fail(Fault::WrongOrder, true); return false; }
    if (state_.cleanup_complete) { fail(Fault::RepeatedCleanup, true); return false; }
    if (!observe(completed, true)) { return false; }
    if (completed >= *state_.cleanup) { fail(Fault::Expired, true); return false; }
    state_.cleanup_complete = true; // Independent progress never clears first_fault.
    return true;
  }
 private:
  static constexpr Duration active = std::chrono::duration_cast<Duration>(std::chrono::seconds{30});
  static constexpr Duration starting = std::chrono::duration_cast<Duration>(std::chrono::seconds{10});
  static constexpr Duration cleaning = std::chrono::duration_cast<Duration>(std::chrono::seconds{5});
  View state_;
  static bool finite(Time value) noexcept { return value != Time::min() && value != Time::max(); }
  static std::optional<Time> add(Time value, Duration amount) noexcept {
    const auto count = value.time_since_epoch().count();
    const auto maximum = std::numeric_limits<Rep>::max();
    if (!finite(value) || amount.count() <= 0 || count >= maximum - amount.count()) { return std::nullopt; }
    return Time{Duration{count + amount.count()}};
  }
  void fail(Fault fault, bool cleanup = false) noexcept {
    if (state_.first_fault == Fault::None) { state_.first_fault = fault; }
    if (cleanup) {
      state_.cleanup_uncertain = true;
      if (state_.cleanup_fault == Fault::None) { state_.cleanup_fault = fault; }
    }
  }
  bool observe(Time time, bool cleanup) noexcept {
    if (!finite(time)) { fail(Fault::InvalidTime, cleanup); return false; }
    if (!state_.origin || time < *state_.origin || time < *state_.highwater) {
      fail(Fault::ClockOrder, cleanup); return false;
    }
    state_.highwater = time;
    return true;
  }
};
} // namespace rs::tests::mysql::q6
