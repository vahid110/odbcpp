#pragma once
#include "temporary_db_validity.h"
#include <string_view>

namespace rs::core::auth {
// Private representation parser only: no issuer, SDK, clock or admission proof.
// Local grammar/resource bounds are NOT advertised AWS format/maximum parity.
enum class IssuerTimestampFormat { JsonSeconds, UtcIso8601 };
enum class TimestampFailure { UnsupportedFormat, InvalidSyntax, ResourceLimit, Nonpositive, PrecisionLoss, Overflow };
class TimestampResult final {
 public:
  TimestampResult(UtcInstant value) noexcept : value_(value) {}
  TimestampResult(TimestampFailure error) noexcept : value_(error) {}
  explicit operator bool() const noexcept { return std::holds_alternative<UtcInstant>(value_); }
  UtcInstant value() const { return std::get<UtcInstant>(value_); }
  TimestampFailure failure() const { return std::get<TimestampFailure>(value_); }
  static std::string_view safe_message(TimestampFailure) noexcept;
 private:
  std::variant<UtcInstant, TimestampFailure> value_;
};
// Complete JSON numeric spelling; local64-byte/19-mantissa/18-fraction digits,
// exponent magnitude<=18 and spelling<=2digits. Exact positive int64 microseconds
// only; nonzero discarded precision refuses, no float/locale/truncation.
// ISO subset: YYYY-MM-DDTHH:MM:SS[.1..6digits]Z, Gregorian1970..9999,
// uppercase UTC only, no timezone offsets/leap-second normalization.
// Borrowed nonsecret lexeme is never retained or included in diagnostics.
TimestampResult parse_issuer_timestamp(IssuerTimestampFormat, std::string_view) noexcept;
} // namespace rs::core::auth
