#include "odbcpp/auth/issuer_timestamp.h"
#include <limits>

namespace rs::core::auth {
namespace {
constexpr auto maximum = (std::numeric_limits<std::int64_t>::max)();
bool digit(char c) noexcept { return c >= '0' && c <= '9'; }
TimestampResult json_seconds(std::string_view text) noexcept {
  std::size_t pos = 0, digits = 0, fractional = 0;
  bool negative = false;
  if (text[pos] == '-') { negative = true; if (++pos == text.size()) return TimestampFailure::InvalidSyntax; }
  std::int64_t coefficient = 0;
  const auto append = [&](char c) -> std::optional<TimestampFailure> {
    if (++digits > 19) return TimestampFailure::ResourceLimit;
    const auto value = static_cast<std::int64_t>(c - '0');
    if (coefficient > (maximum - value) / 10) return TimestampFailure::Overflow;
    coefficient = coefficient * 10 + value;
    return std::nullopt;
  };
  if (!digit(text[pos])) return TimestampFailure::InvalidSyntax;
  if (text[pos] == '0') {
    if (auto error = append(text[pos++])) return *error;
    if (pos < text.size() && digit(text[pos])) return TimestampFailure::InvalidSyntax;
  } else {
    while (pos < text.size() && digit(text[pos])) if (auto error = append(text[pos++])) return *error;
  }
  if (pos < text.size() && text[pos] == '.') {
    ++pos;
    if (pos == text.size() || !digit(text[pos])) return TimestampFailure::InvalidSyntax;
    while (pos < text.size() && digit(text[pos])) {
      if (++fractional > 18) return TimestampFailure::ResourceLimit;
      if (auto error = append(text[pos++])) return *error;
    }
  }
  int exponent = 0;
  if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
    ++pos; bool exponent_negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) exponent_negative = text[pos++] == '-';
    if (pos == text.size() || !digit(text[pos])) return TimestampFailure::InvalidSyntax;
    unsigned exponent_digits = 0;
    while (pos < text.size() && digit(text[pos])) {
      if (++exponent_digits > 2) return TimestampFailure::ResourceLimit;
      exponent = exponent * 10 + text[pos++] - '0';
    }
    if (exponent > 18) return TimestampFailure::ResourceLimit;
    if (exponent_negative) exponent = -exponent;
  }
  if (pos != text.size()) return TimestampFailure::InvalidSyntax;
  if (negative || coefficient == 0) return TimestampFailure::Nonpositive;
  int scale = 6 + exponent - static_cast<int>(fractional);
  while (scale > 0) {
    if (coefficient > maximum / 10) return TimestampFailure::Overflow;
    coefficient *= 10; --scale;
  }
  while (scale < 0) {
    if (coefficient % 10 != 0) return TimestampFailure::PrecisionLoss;
    coefficient /= 10; ++scale;
  }
  return UtcInstant{coefficient};
}
int number(std::string_view text, std::size_t start, std::size_t length) noexcept {
  int result = 0;
  for (std::size_t i = start; i < start + length; ++i) {
    if (!digit(text[i])) return -1;
    result = result * 10 + text[i] - '0';
  }
  return result;
}
bool leap(int year) noexcept { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
std::int64_t days_before_year(int year) noexcept {
  const auto previous = static_cast<std::int64_t>(year - 1);
  return previous * 365 + previous / 4 - previous / 100 + previous / 400;
}
TimestampResult utc_iso(std::string_view text) noexcept {
  if (text.size() < 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
      text[13] != ':' || text[16] != ':' || text.back() != 'Z') return TimestampFailure::InvalidSyntax;
  const int year = number(text, 0, 4), month = number(text, 5, 2), day = number(text, 8, 2);
  const int hour = number(text, 11, 2), minute = number(text, 14, 2), second = number(text, 17, 2);
  if (year < 1970 || month < 1 || month > 12 || day < 1 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 59) return TimestampFailure::InvalidSyntax;
  constexpr int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (day > month_days[month - 1] + (month == 2 && leap(year) ? 1 : 0)) return TimestampFailure::InvalidSyntax;
  std::int64_t fraction = 0;
  if (text.size() != 20) {
    if (text[19] != '.' || text.size() < 22) return TimestampFailure::InvalidSyntax;
    const auto count = text.size() - 21;
    if (count > 6) return TimestampFailure::PrecisionLoss;
    for (std::size_t i = 20; i < text.size() - 1; ++i) {
      if (!digit(text[i])) return TimestampFailure::InvalidSyntax;
      fraction = fraction * 10 + text[i] - '0';
    }
    for (auto i = count; i < 6; ++i) fraction *= 10;
  }
  auto days = days_before_year(year) - days_before_year(1970) + day - 1;
  for (int i = 1; i < month; ++i) days += month_days[i - 1] + (i == 2 && leap(year) ? 1 : 0);
  // Four-digit1970..9999 bounds all intermediates; final checked multiplication
  // remains explicit rather than relying on OS chrono/date normalization.
  const auto seconds = days * 86400 + hour * 3600 + minute * 60 + second;
  if (seconds > (maximum - fraction) / 1000000) return TimestampFailure::Overflow;
  const auto micros = seconds * 1000000 + fraction;
  if (micros <= 0) return TimestampFailure::Nonpositive;
  return UtcInstant{micros};
}
}
std::string_view TimestampResult::safe_message(TimestampFailure failure) noexcept {
  switch (failure) {
    case TimestampFailure::UnsupportedFormat: return "Issuer timestamp format unsupported";
    case TimestampFailure::InvalidSyntax: return "Issuer timestamp syntax malformed";
    case TimestampFailure::ResourceLimit: return "Issuer timestamp representation exceeds local limits";
    case TimestampFailure::Nonpositive: return "Issuer timestamp must be positive";
    case TimestampFailure::PrecisionLoss: return "Issuer timestamp precision unsupported";
    case TimestampFailure::Overflow: return "Issuer timestamp range invalid";
  }
  return "Issuer timestamp syntax malformed";
}
TimestampResult parse_issuer_timestamp(IssuerTimestampFormat format, std::string_view text) noexcept {
  if (text.empty()) return TimestampFailure::InvalidSyntax;
  if (text.size() > 64) return TimestampFailure::ResourceLimit;
  switch (format) {
    case IssuerTimestampFormat::JsonSeconds: return json_seconds(text);
    case IssuerTimestampFormat::UtcIso8601: return utc_iso(text);
  }
  return TimestampFailure::UnsupportedFormat;
}
} // namespace rs::core::auth
