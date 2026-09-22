#pragma once

#include <ctime>
#include <optional>

namespace odbcpp::test {

inline std::optional<std::tm> local_calendar(std::time_t instant) {
  std::tm calendar{};
#ifdef _WIN32
  if (localtime_s(&calendar, &instant) != 0) return std::nullopt;
#else
  if (!localtime_r(&instant, &calendar)) return std::nullopt;
#endif
  return calendar;
}

}  // namespace odbcpp::test
