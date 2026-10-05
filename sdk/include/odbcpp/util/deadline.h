#pragma once
#include <chrono>

namespace rs::util {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

inline Deadline make_deadline(std::chrono::milliseconds from_now) {
  if (from_now < std::chrono::milliseconds::zero()) {
    return Deadline::min();
  }
  const auto now = Clock::now();
  const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
      Deadline::max() - now);
  return from_now >= maximum ? Deadline::max() : now + from_now;
}

inline std::chrono::milliseconds remaining_at(
    Deadline dl, Clock::time_point now) {
  if (dl <= now) return std::chrono::milliseconds(0);
  return std::chrono::ceil<std::chrono::milliseconds>(dl - now);
}

inline std::chrono::milliseconds remaining(Deadline dl) {
  return remaining_at(dl, Clock::now());
}

} // namespace rs::util
