#include "odbcpp/util/scaled_decimal.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace rs::util {
namespace {
bool whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
      c == '\f' || c == '\v';
}
bool digit(char c) { return c >= '0' && c <= '9'; }
}  // namespace

std::optional<ScaledDecimal> rescale_decimal(
    std::string_view text, std::size_t precision, int scale,
    DecimalConversionIssue* issue) {
  if (issue) *issue = DecimalConversionIssue::None;
  const auto fail = [&](DecimalConversionIssue reason)
      -> std::optional<ScaledDecimal> {
    if (issue) *issue = reason;
    return std::nullopt;
  };
  while (!text.empty() && whitespace(text.front())) text.remove_prefix(1);
  while (!text.empty() && whitespace(text.back())) text.remove_suffix(1);
  const bool negative = !text.empty() && text.front() == '-';
  if (!text.empty() && (text.front() == '+' || text.front() == '-'))
    text.remove_prefix(1);

  // Saturated exponents still retain enough decimal positions to distinguish
  // coefficient overflow, whole-digit loss and fraction-only loss. No memory
  // allocation is proportional to an exponent supplied in the input text.
  constexpr auto safe_size = (std::numeric_limits<std::int64_t>::max)() / 8;
  const auto signed_scale = static_cast<std::int64_t>(scale);
  const auto scale_extent = signed_scale < 0 ? -signed_scale : signed_scale;
  if (precision == 0 || text.size() > static_cast<std::size_t>(safe_size) ||
      precision > static_cast<std::size_t>(safe_size) || scale_extent > safe_size)
    return fail(DecimalConversionIssue::OutOfRange);
  const auto exponent_limit = static_cast<std::int64_t>(text.size()) +
      static_cast<std::int64_t>(precision) + scale_extent + 1;

  std::size_t position = 0;
  while (position < text.size() && digit(text[position])) ++position;
  const auto whole = text.substr(0, position);
  std::string_view fraction;
  if (position < text.size() && text[position] == '.') {
    const auto start = ++position;
    while (position < text.size() && digit(text[position])) ++position;
    fraction = text.substr(start, position - start);
  }
  if (whole.empty() && fraction.empty())
    return fail(DecimalConversionIssue::InvalidCharacter);

  std::int64_t exponent = 0;
  if (position < text.size() &&
      (text[position] == 'e' || text[position] == 'E')) {
    ++position;
    bool negative_exponent = false;
    if (position < text.size() &&
        (text[position] == '+' || text[position] == '-')) {
      negative_exponent = text[position++] == '-';
    }
    const auto start = position;
    while (position < text.size() && digit(text[position])) {
      const auto next = static_cast<std::int64_t>(text[position++] - '0');
      exponent = next > exponent_limit ||
          exponent > (exponent_limit - next) / 10
          ? exponent_limit : exponent * 10 + next;
    }
    if (position == start) return fail(DecimalConversionIssue::InvalidCharacter);
    if (negative_exponent) exponent = -exponent;
  }
  if (position != text.size()) return fail(DecimalConversionIssue::InvalidCharacter);

  std::string digits(whole);
  digits.append(fraction);
  const auto first = digits.find_first_not_of('0');
  if (first == std::string::npos) return ScaledDecimal{"0", scale, false};
  const auto decimal_position = static_cast<std::int64_t>(whole.size()) + exponent;
  const auto coefficient_end = decimal_position + signed_scale;
  const auto kept = coefficient_end <= 0 ? std::size_t{0} :
      static_cast<std::size_t>(std::min(coefficient_end,
                                      static_cast<std::int64_t>(digits.size())));
  bool lost_fraction = false;
  for (auto index = kept; index < digits.size(); ++index) {
    if (digits[index] == '0') continue;
    if (static_cast<std::int64_t>(index) < decimal_position)
      return fail(DecimalConversionIssue::WholeDigitsLost);
    lost_fraction = true;
  }

  std::string coefficient = "0";
  if (coefficient_end > static_cast<std::int64_t>(first)) {
    const auto significant = coefficient_end - static_cast<std::int64_t>(first);
    if (significant > static_cast<std::int64_t>(precision))
      return fail(DecimalConversionIssue::OutOfRange);
    coefficient = digits.substr(first, kept - first);
    if (coefficient_end > static_cast<std::int64_t>(digits.size()))
      coefficient.append(static_cast<std::size_t>(coefficient_end -
                          static_cast<std::int64_t>(digits.size())), '0');
  }
  if (lost_fraction && issue) *issue = DecimalConversionIssue::FractionalTruncation;
  const bool nonzero = coefficient != "0";
  return ScaledDecimal{std::move(coefficient), scale, negative && nonzero};
}

std::optional<std::string> format_scaled_decimal(
    const ScaledDecimal& value, std::size_t max_output_bytes) {
  if (value.coefficient.empty() ||
      !std::all_of(value.coefficient.begin(), value.coefficient.end(), digit))
    return std::nullopt;
  const auto first = value.coefficient.find_first_not_of('0');
  const bool nonzero = first != std::string::npos;
  auto digits = nonzero ? std::string_view(value.coefficient).substr(first)
                       : std::string_view{"0"};
  const auto signed_scale = static_cast<std::int64_t>(value.scale);
  const auto negative_scale = signed_scale < 0
      ? static_cast<std::uint64_t>(-signed_scale) : 0;
  const auto positive_scale = signed_scale > 0
      ? static_cast<std::uint64_t>(signed_scale) : 0;
  const std::size_t sign_bytes = value.negative && nonzero ? 1 : 0;
  if (sign_bytes > max_output_bytes || digits.size() > max_output_bytes - sign_bytes)
    return std::nullopt;
  auto required = digits.size() + sign_bytes;
  if (positive_scale) {
    const auto padding = positive_scale >= digits.size()
        ? positive_scale + 1 - digits.size() : 0;
    if (padding >= max_output_bytes - required) return std::nullopt;
    required += static_cast<std::size_t>(padding) + 1; // Decimal point.
  } else if (nonzero) {
    if (negative_scale > max_output_bytes - required) return std::nullopt;
    required += static_cast<std::size_t>(negative_scale);
  }
  std::string result;
  result.reserve(required);
  if (sign_bytes) result.push_back('-');
  if (positive_scale >= digits.size() && positive_scale) {
    result.append("0.");
    result.append(static_cast<std::size_t>(positive_scale - digits.size()), '0');
    result.append(digits);
  } else if (positive_scale) {
    const auto point = digits.size() - static_cast<std::size_t>(positive_scale);
    result.append(digits.substr(0, point));
    result.push_back('.');
    result.append(digits.substr(point));
  } else {
    result.append(digits);
    if (nonzero) result.append(static_cast<std::size_t>(negative_scale), '0');
  }
  return result;
}

}  // namespace rs::util
