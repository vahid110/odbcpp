#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace rs::util {

// An owning decimal coefficient; no C ABI layout or backend encoding is implied.
struct ScaledDecimal {
  std::string coefficient;
  int scale;
  bool negative;
};

enum class DecimalConversionIssue {
  None,
  InvalidCharacter,
  OutOfRange,
  WholeDigitsLost,
  FractionalTruncation,
};

// Caller-supplied precision/scale are representation policy, not SDK-wide limits.
// Nonzero whole-digit loss is an error; fractional loss truncates toward zero.
std::optional<ScaledDecimal> rescale_decimal(
    std::string_view text, std::size_t precision, int scale,
    DecimalConversionIssue* issue = nullptr);

// Format a coefficient without rounding, within an explicit output byte bound.
std::optional<std::string> format_scaled_decimal(
    const ScaledDecimal& value, std::size_t max_output_bytes);

}  // namespace rs::util
