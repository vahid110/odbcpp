#include "text_data_converter.h"
#include "unicode.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace rs::odbc {
namespace {

void store_indicator(SQLLEN* indicator, SQLLEN value) {
  if (indicator) {
    std::memcpy(reinterpret_cast<std::byte*>(indicator), &value,
                sizeof(value));
  }
}

std::string_view trim_whitespace(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

std::optional<long double> parse_number(const std::string& value,
                                        ConversionIssue* issue) {
  if (value.empty()) {
    if (issue) *issue = ConversionIssue::InvalidCharacterValue;
    return std::nullopt;
  }
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtold(value.c_str(), &end);
  if (end == value.c_str()) {
    if (issue) *issue = ConversionIssue::InvalidCharacterValue;
    return std::nullopt;
  }
  while (end != value.c_str() + value.size() &&
         std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (end != value.c_str() + value.size()) {
    if (issue) *issue = ConversionIssue::InvalidCharacterValue;
    return std::nullopt;
  }
  if (errno == ERANGE || !std::isfinite(parsed)) {
    if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
    return std::nullopt;
  }
  return parsed;
}

template <typename T>
SQLRETURN convert_integral(const std::string& value, void* buffer,
                           SQLLEN* indicator, ConversionIssue* issue) {
  static_assert(std::numeric_limits<T>::is_signed);
  const auto finish = [&](T converted, bool fractional) -> SQLRETURN {
    std::memcpy(buffer, &converted, sizeof(converted));
    store_indicator(indicator, static_cast<SQLLEN>(sizeof(T)));
    if (fractional) {
      if (issue) *issue = ConversionIssue::FractionalTruncation;
      return SQL_SUCCESS_WITH_INFO;
    }
    return SQL_SUCCESS;
  };

  auto numeric_text = trim_whitespace(value);
  auto integer_text = numeric_text;
  if (integer_text.starts_with('+')) integer_text.remove_prefix(1);
  T exact{};
  const auto [end, error] = std::from_chars(
      integer_text.data(), integer_text.data() + integer_text.size(), exact);
  if (end == integer_text.data() + integer_text.size()) {
    if (error == std::errc{}) {
      return finish(exact, false);
    }
    if (error == std::errc::result_out_of_range) {
      if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
      return SQL_ERROR;
    }
  }

  // Parse decimal/scientific notation as digits so 64-bit boundaries never
  // pass through floating point. Keep strtold below for legacy text forms.
  std::size_t pos = 0;
  const bool negative = !numeric_text.empty() && numeric_text.front() == '-';
  if (!numeric_text.empty() &&
      (numeric_text.front() == '-' || numeric_text.front() == '+')) ++pos;
  std::string digits;
  digits.reserve(numeric_text.size());
  while (pos < numeric_text.size() &&
         numeric_text[pos] >= '0' && numeric_text[pos] <= '9') {
    digits.push_back(numeric_text[pos++]);
  }
  const auto whole_digits = digits.size();
  if (pos < numeric_text.size() && numeric_text[pos] == '.') {
    ++pos;
    while (pos < numeric_text.size() &&
           numeric_text[pos] >= '0' && numeric_text[pos] <= '9') {
      digits.push_back(numeric_text[pos++]);
    }
  }
  bool negative_exponent = false;
  std::size_t exponent = 0;
  bool valid_exponent = true;
  if (pos < numeric_text.size() &&
      (numeric_text[pos] == 'e' || numeric_text[pos] == 'E')) {
    ++pos;
    if (pos < numeric_text.size() &&
        (numeric_text[pos] == '-' || numeric_text[pos] == '+')) {
      negative_exponent = numeric_text[pos++] == '-';
    }
    const auto exponent_start = pos;
    while (pos < numeric_text.size() &&
           numeric_text[pos] >= '0' && numeric_text[pos] <= '9') {
      const auto digit = static_cast<std::size_t>(numeric_text[pos++] - '0');
      const auto max = std::numeric_limits<std::size_t>::max();
      exponent = exponent > (max - digit) / 10 ? max : exponent * 10 + digit;
    }
    valid_exponent = pos != exponent_start;
  }
  if (!digits.empty() && valid_exponent && pos == numeric_text.size()) {
    const auto first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) return finish(T{}, false);

    const auto max_size = std::numeric_limits<std::size_t>::max();
    const auto integral_digits = negative_exponent
        ? (exponent >= whole_digits ? 0 : whole_digits - exponent)
        : (exponent > max_size - whole_digits ? max_size
                                              : whole_digits + exponent);
    if (integral_digits <= first_nonzero) return finish(T{}, true);
    if (integral_digits - first_nonzero >
        static_cast<std::size_t>(std::numeric_limits<T>::digits10 + 1)) {
      if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
      return SQL_ERROR;
    }

    const auto limit = static_cast<std::uintmax_t>(
        std::numeric_limits<T>::max()) + (negative ? 1u : 0u);
    std::uintmax_t magnitude = 0;
    for (auto i = first_nonzero; i < integral_digits; ++i) {
      const auto digit = static_cast<std::uintmax_t>(
          i < digits.size() ? digits[i] - '0' : 0);
      if (magnitude > (limit - digit) / 10) {
        if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
        return SQL_ERROR;
      }
      magnitude = magnitude * 10 + digit;
    }
    const auto converted = negative
        ? (magnitude == limit ? std::numeric_limits<T>::min()
                              : static_cast<T>(-static_cast<std::intmax_t>(magnitude)))
        : static_cast<T>(magnitude);
    const auto fraction_start = std::min(integral_digits, digits.size());
    const bool fractional = std::any_of(
        digits.begin() + fraction_start, digits.end(),
        [](char digit) { return digit != '0'; });
    return finish(converted, fractional);
  }

  const auto parsed = parse_number(value, issue);
  if (!parsed) return SQL_ERROR;
  const auto truncated = std::trunc(*parsed);
  const auto upper_exclusive = std::ldexp(
      1.0L, std::numeric_limits<T>::digits);
  if (truncated < -upper_exclusive || truncated >= upper_exclusive) {
    if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
    return SQL_ERROR;
  }
  const T converted = static_cast<T>(truncated);
  return finish(converted, truncated != *parsed);
}

bool parse_digits(std::string_view value, std::size_t offset,
                  std::size_t count, unsigned& result) {
  if (offset + count > value.size()) return false;
  result = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char ch = value[offset + i];
    if (ch < '0' || ch > '9') return false;
    result = result * 10u + static_cast<unsigned>(ch - '0');
  }
  return true;
}

bool leap_year(unsigned year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

bool valid_date(unsigned year, unsigned month, unsigned day) {
  static constexpr unsigned days_per_month[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (year > static_cast<unsigned>(std::numeric_limits<SQLSMALLINT>::max()) ||
      month < 1 || month > 12 || day < 1) {
    return false;
  }
  const auto max_day = days_per_month[month - 1] +
      (month == 2 && leap_year(year) ? 1u : 0u);
  return day <= max_day;
}

bool parse_date(std::string_view value, unsigned& year, unsigned& month,
                unsigned& day) {
  return value.size() == 10 && value[4] == '-' && value[7] == '-' &&
      parse_digits(value, 0, 4, year) &&
      parse_digits(value, 5, 2, month) &&
      parse_digits(value, 8, 2, day) && valid_date(year, month, day);
}

bool valid_time_suffix(std::string_view suffix) {
  if (suffix.empty()) return true;
  std::size_t offset = 0;
  if (suffix[offset] == '.') {
    ++offset;
    const auto digits_start = offset;
    while (offset < suffix.size() && suffix[offset] >= '0' &&
           suffix[offset] <= '9') {
      ++offset;
    }
    if (offset == digits_start) return false;
  }
  if (offset == suffix.size()) return true;
  if (suffix[offset] != '+' && suffix[offset] != '-') return false;
  ++offset;
  const auto remaining = suffix.size() - offset;
  if (remaining == 2) {
    unsigned hours = 0;
    return parse_digits(suffix, offset, 2, hours) && hours <= 15;
  }
  if (remaining == 5 && suffix[offset + 2] == ':') {
    unsigned hours = 0;
    unsigned minutes = 0;
    return parse_digits(suffix, offset, 2, hours) &&
        parse_digits(suffix, offset + 3, 2, minutes) &&
        hours <= 15 && minutes <= 59;
  }
  return false;
}

bool parse_time(std::string_view value, unsigned& hour, unsigned& minute,
                unsigned& second, SQLUINTEGER& fraction,
                bool& discarded_fraction) {
  if (value.size() < 8 || value[2] != ':' || value[5] != ':' ||
      !parse_digits(value, 0, 2, hour) ||
      !parse_digits(value, 3, 2, minute) ||
      !parse_digits(value, 6, 2, second) ||
      hour > 23 || minute > 59 || second > 60 ||
      !valid_time_suffix(value.substr(8))) {
    return false;
  }

  fraction = 0;
  discarded_fraction = false;
  if (value.size() > 8 && value[8] == '.') {
    std::size_t offset = 9;
    unsigned digits = 0;
    while (offset < value.size() &&
           value[offset] >= '0' && value[offset] <= '9') {
      if (digits < 9) {
        fraction = fraction * 10u +
            static_cast<SQLUINTEGER>(value[offset] - '0');
        ++digits;
      } else if (value[offset] != '0') {
        discarded_fraction = true;
      }
      ++offset;
    }
    while (digits++ < 9) fraction *= 10u;
  }
  return true;
}

SQLRETURN convert_string(const std::string& value, void* buffer,
                         SQLLEN buffer_length, SQLLEN* indicator) {
  if (buffer_length < 0) return SQL_ERROR;
  store_indicator(indicator, static_cast<SQLLEN>(value.size()));
  if (buffer_length == 0) return SQL_SUCCESS_WITH_INFO;
  const auto capacity = static_cast<std::size_t>(buffer_length - 1);
  const auto copy_length = std::min(capacity, value.size());
  std::memcpy(buffer, value.data(), copy_length);
  static_cast<char*>(buffer)[copy_length] = '\0';
  return copy_length < value.size() ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

SQLRETURN convert_wide_string(const std::string& value, void* buffer,
                              SQLLEN buffer_length, SQLLEN* indicator,
                              ConversionIssue* issue) {
  if (buffer_length < 0) return SQL_ERROR;
  const auto wide = utf8_to_wide(value);
  if (!wide) {
    if (issue) *issue = ConversionIssue::InvalidCharacterValue;
    return SQL_ERROR;
  }

  const auto required_bytes = wide->size() * sizeof(SQLWCHAR);
  store_indicator(indicator, static_cast<SQLLEN>(required_bytes));
  const auto buffer_units =
      static_cast<std::size_t>(buffer_length) / sizeof(SQLWCHAR);
  if (buffer_units == 0) return SQL_SUCCESS_WITH_INFO;
  const auto capacity = buffer_units - 1;
  auto copy_length = std::min(capacity, wide->size());
  if constexpr (sizeof(SQLWCHAR) == 2) {
    if (copy_length < wide->size() && copy_length > 0 &&
        (*wide)[copy_length - 1] >= 0xd800 &&
        (*wide)[copy_length - 1] <= 0xdbff) {
      --copy_length;
    }
  }
  const auto copied_bytes = copy_length * sizeof(SQLWCHAR);
  if (copied_bytes > 0) {
    std::memcpy(buffer, wide->data(), copied_bytes);
  }
  const SQLWCHAR terminator = 0;
  std::memcpy(static_cast<char*>(buffer) + copied_bytes, &terminator,
              sizeof(terminator));
  return copy_length < wide->size()
      ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

SQLRETURN convert_floating(const std::string& value, SQLSMALLINT target_type,
                           void* buffer, SQLLEN* indicator,
                           ConversionIssue* issue) {
  const auto parsed = parse_number(value, issue);
  if (!parsed) return SQL_ERROR;
  if (target_type == SQL_C_FLOAT) {
    if (*parsed < -std::numeric_limits<SQLREAL>::max() ||
        *parsed > std::numeric_limits<SQLREAL>::max()) {
      if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
      return SQL_ERROR;
    }
    const SQLREAL converted = static_cast<SQLREAL>(*parsed);
    std::memcpy(buffer, &converted, sizeof(converted));
    store_indicator(indicator, static_cast<SQLLEN>(sizeof(SQLREAL)));
  } else {
    if (*parsed < -std::numeric_limits<SQLDOUBLE>::max() ||
        *parsed > std::numeric_limits<SQLDOUBLE>::max()) {
      if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
      return SQL_ERROR;
    }
    const SQLDOUBLE converted = static_cast<SQLDOUBLE>(*parsed);
    std::memcpy(buffer, &converted, sizeof(converted));
    store_indicator(indicator, static_cast<SQLLEN>(sizeof(SQLDOUBLE)));
  }
  return SQL_SUCCESS;
}

SQLRETURN convert_boolean(const std::string& value, void* buffer,
                          SQLLEN* indicator, ConversionIssue* issue) {
  SQLCHAR converted = 0;
  bool fractional = false;
  if (value == "t" || value == "true" || value == "1") {
    converted = 1;
  } else if (value != "f" && value != "false" && value != "0") {
    SQLSMALLINT numeric = 0;
    ConversionIssue numeric_issue = ConversionIssue::None;
    const auto result = convert_integral<SQLSMALLINT>(
        value, &numeric, nullptr, &numeric_issue);
    if (result == SQL_ERROR) {
      if (issue) *issue = numeric_issue;
      return SQL_ERROR;
    }
    const bool negative_fraction =
        trim_whitespace(value).starts_with('-') &&
        result == SQL_SUCCESS_WITH_INFO && numeric == 0;
    if (numeric < 0 || numeric > 1 || negative_fraction) {
      if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
      return SQL_ERROR;
    }
    converted = static_cast<SQLCHAR>(numeric);
    fractional = result == SQL_SUCCESS_WITH_INFO;
  }
  *static_cast<SQLCHAR*>(buffer) = converted;
  store_indicator(indicator, static_cast<SQLLEN>(sizeof(SQLCHAR)));
  if (fractional) {
    if (issue) *issue = ConversionIssue::FractionalTruncation;
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

SQLRETURN convert_date(const std::string& value, void* buffer,
                       SQLLEN* indicator, ConversionIssue* issue) {
  const auto text = trim_whitespace(value);
  const bool timestamp = text.size() >= 19 &&
      (text[10] == ' ' || text[10] == 'T');
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!parse_date(timestamp ? text.substr(0, 10) : text,
                  year, month, day)) {
    if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
    return SQL_ERROR;
  }
  bool lost_time = false;
  if (timestamp) {
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    SQLUINTEGER fraction = 0;
    bool discarded_fraction = false;
    if (!parse_time(text.substr(11), hour, minute, second, fraction,
                    discarded_fraction)) {
      if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
      return SQL_ERROR;
    }
    lost_time = hour != 0 || minute != 0 || second != 0 ||
        fraction != 0 || discarded_fraction;
  }
  const SQL_DATE_STRUCT date{
      static_cast<SQLSMALLINT>(year), static_cast<SQLUSMALLINT>(month),
      static_cast<SQLUSMALLINT>(day)};
  std::memcpy(buffer, &date, sizeof(date));
  store_indicator(indicator, static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)));
  if (lost_time) {
    if (issue) *issue = ConversionIssue::FractionalTruncation;
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

SQLRETURN convert_time(const std::string& value, void* buffer,
                       SQLLEN* indicator, ConversionIssue* issue) {
  auto text = trim_whitespace(value);
  if (text.size() >= 19 && (text[10] == ' ' || text[10] == 'T')) {
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    if (!parse_date(text.substr(0, 10), year, month, day)) {
      if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
      return SQL_ERROR;
    }
    text.remove_prefix(11);
  }
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  SQLUINTEGER fraction = 0;
  bool discarded_fraction = false;
  if (!parse_time(text, hour, minute, second, fraction,
                  discarded_fraction)) {
    if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
    return SQL_ERROR;
  }
  const SQL_TIME_STRUCT time{
      static_cast<SQLUSMALLINT>(hour), static_cast<SQLUSMALLINT>(minute),
      static_cast<SQLUSMALLINT>(second)};
  std::memcpy(buffer, &time, sizeof(time));
  store_indicator(indicator, static_cast<SQLLEN>(sizeof(SQL_TIME_STRUCT)));
  if (fraction != 0 || discarded_fraction) {
    if (issue) *issue = ConversionIssue::FractionalTruncation;
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

SQLRETURN convert_timestamp(const std::string& value, void* buffer,
                            SQLLEN* indicator, ConversionIssue* issue) {
  const auto text = trim_whitespace(value);
  const bool date_only = text.size() == 10;
  const bool time_only = text.size() >= 8 &&
      text[2] == ':' && text[5] == ':';
  if (!date_only && !time_only &&
      (text.size() < 19 || (text[10] != ' ' && text[10] != 'T'))) {
    if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
    return SQL_ERROR;
  }
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  SQLUINTEGER fraction = 0;
  bool discarded_fraction = false;
  const bool valid = time_only
      ? parse_time(text, hour, minute, second, fraction, discarded_fraction)
      : (parse_date(text.substr(0, 10), year, month, day) &&
         (date_only || parse_time(text.substr(11), hour, minute, second,
                                  fraction, discarded_fraction)));
  if (!valid) {
    if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
    return SQL_ERROR;
  }
  if (time_only) {
    const auto now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    const bool valid_local_date = localtime_s(&local, &now) == 0;
#else
    const bool valid_local_date = localtime_r(&now, &local) != nullptr;
#endif
    if (!valid_local_date || local.tm_year + 1900 >
            std::numeric_limits<SQLSMALLINT>::max()) {
      if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
      return SQL_ERROR;
    }
    year = static_cast<unsigned>(local.tm_year + 1900);
    month = static_cast<unsigned>(local.tm_mon + 1);
    day = static_cast<unsigned>(local.tm_mday);
  }
  const SQL_TIMESTAMP_STRUCT timestamp{
      static_cast<SQLSMALLINT>(year), static_cast<SQLUSMALLINT>(month),
      static_cast<SQLUSMALLINT>(day), static_cast<SQLUSMALLINT>(hour),
      static_cast<SQLUSMALLINT>(minute), static_cast<SQLUSMALLINT>(second),
      fraction};
  std::memcpy(buffer, &timestamp, sizeof(timestamp));
  store_indicator(indicator, static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT)));
  if (discarded_fraction) {
    if (issue) *issue = ConversionIssue::FractionalTruncation;
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
}

} // namespace

SQLRETURN TextDataConverter::convert_data(const std::string& value,
                                          SQLSMALLINT target_c_type,
                                          void* buffer,
                                          SQLLEN buffer_length,
                                          SQLLEN* indicator,
                                          ConversionIssue* issue) {
  if (issue) *issue = ConversionIssue::None;
  if (!buffer) return SQL_ERROR;
  switch (target_c_type) {
    case SQL_C_CHAR:
      return convert_string(value, buffer, buffer_length, indicator);
    case SQL_C_WCHAR:
      return convert_wide_string(
          value, buffer, buffer_length, indicator, issue);
    case SQL_C_SSHORT:
      return convert_integral<SQLSMALLINT>(value, buffer, indicator, issue);
    case SQL_C_SLONG:
      return convert_integral<SQLINTEGER>(value, buffer, indicator, issue);
    case SQL_C_SBIGINT:
      return convert_integral<SQLBIGINT>(value, buffer, indicator, issue);
    case SQL_C_FLOAT:
    case SQL_C_DOUBLE:
      return convert_floating(
          value, target_c_type, buffer, indicator, issue);
    case SQL_C_BIT:
      return convert_boolean(value, buffer, indicator, issue);
    case SQL_C_DATE:
      return convert_date(value, buffer, indicator, issue);
    case SQL_C_TIME:
      return convert_time(value, buffer, indicator, issue);
    case SQL_C_TIMESTAMP:
      return convert_timestamp(value, buffer, indicator, issue);
    case SQL_C_BINARY: {
      const auto decoded = decode_binary(value);
      if (!decoded || buffer_length < 0) {
        if (issue) *issue = ConversionIssue::InvalidCharacterValue;
        return SQL_ERROR;
      }
      const auto capacity = static_cast<std::size_t>(buffer_length);
      const auto copy_length = std::min(capacity, decoded->size());
      if (copy_length > 0) {
        std::memcpy(buffer, decoded->data(), copy_length);
      }
      store_indicator(indicator, static_cast<SQLLEN>(decoded->size()));
      return copy_length < decoded->size()
          ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    }
    default:
      return SQL_ERROR;
  }
}

std::optional<std::vector<std::byte>> TextDataConverter::decode_binary(
    std::string_view value) {
  std::vector<std::byte> decoded;
  if (value.starts_with("\\x")) {
    value.remove_prefix(2);
    if (value.size() % 2 != 0) return std::nullopt;
    decoded.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
      const auto high = hex_value(value[i]);
      const auto low = hex_value(value[i + 1]);
      if (high < 0 || low < 0) return std::nullopt;
      decoded.push_back(static_cast<std::byte>((high << 4) | low));
    }
    return decoded;
  }

  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    if (value[i] != '\\') {
      decoded.push_back(static_cast<std::byte>(
          static_cast<unsigned char>(value[i++])));
      continue;
    }
    if (i + 1 < value.size() && value[i + 1] == '\\') {
      decoded.push_back(std::byte{'\\'});
      i += 2;
      continue;
    }
    if (i + 3 >= value.size() || value[i + 1] < '0' ||
        value[i + 1] > '3' || value[i + 2] < '0' ||
        value[i + 2] > '7' || value[i + 3] < '0' ||
        value[i + 3] > '7') {
      return std::nullopt;
    }
    const auto octet = static_cast<unsigned char>(
        (value[i + 1] - '0') * 64 + (value[i + 2] - '0') * 8 +
        (value[i + 3] - '0'));
    decoded.push_back(static_cast<std::byte>(octet));
    i += 4;
  }
  return decoded;
}

std::string TextDataConverter::encode_binary(
    std::span<const std::byte> value) {
  static constexpr char hex[] = "0123456789abcdef";
  if (value.size() > (std::numeric_limits<std::size_t>::max() - 2) / 2) {
    throw std::length_error("Binary parameter value is too large");
  }
  std::string encoded;
  encoded.reserve(2 + value.size() * 2);
  encoded += "\\x";
  for (const auto item : value) {
    const auto octet = std::to_integer<unsigned char>(item);
    encoded.push_back(hex[octet >> 4]);
    encoded.push_back(hex[octet & 0x0f]);
  }
  return encoded;
}

} // namespace rs::odbc
