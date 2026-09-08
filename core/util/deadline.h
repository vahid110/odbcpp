#pragma once
#include <chrono>

namespace rs::util {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

inline Deadline make_deadline(std::chrono::milliseconds from_now) {
  const auto now = Clock::now();
  const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
      Deadline::max() - now);
  return from_now >= maximum ? Deadline::max() : now + from_now;
}

inline std::chrono::milliseconds remaining(Deadline dl) {
  auto now = Clock::now();
  if (dl <= now) return std::chrono::milliseconds(0);
  return std::chrono::duration_cast<std::chrono::milliseconds>(dl - now);
}

} // namespace rs::util
