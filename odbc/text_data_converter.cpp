#include "text_data_converter.h"
#include "unicode.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace rs::odbc {
namespace {

std::optional<long double> parse_number(const std::string& value,
                                        ConversionIssue* issue) {
  if (value.empty()) {
    if (issue) *issue = ConversionIssue::InvalidCharacterValue;
    return std::nullopt;
  }
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtold(value.c_str(), &end);
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
  const auto parsed = parse_number(value, issue);
  if (!parsed) return SQL_ERROR;
  const auto truncated = std::trunc(*parsed);
  if (truncated < static_cast<long double>(std::numeric_limits<T>::lowest()) ||
      truncated > static_cast<long double>(std::numeric_limits<T>::max())) {
    if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
    return SQL_ERROR;
  }
  *static_cast<T*>(buffer) = static_cast<T>(truncated);
  if (indicator) *indicator = sizeof(T);
  if (truncated != *parsed) {
    if (issue) *issue = ConversionIssue::FractionalTruncation;
    return SQL_SUCCESS_WITH_INFO;
  }
  return SQL_SUCCESS;
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
                unsigned& second, SQLUINTEGER& fraction) {
  if (value.size() < 8 || value[2] != ':' || value[5] != ':' ||
      !parse_digits(value, 0, 2, hour) ||
      !parse_digits(value, 3, 2, minute) ||
      !parse_digits(value, 6, 2, second) ||
      hour > 23 || minute > 59 || second > 60 ||
      !valid_time_suffix(value.substr(8))) {
    return false;
  }

  fraction = 0;
  if (value.size() > 8 && value[8] == '.') {
    std::size_t offset = 9;
    unsigned digits = 0;
    while (offset < value.size() && digits < 9 &&
           value[offset] >= '0' && value[offset] <= '9') {
      fraction = fraction * 10u + static_cast<SQLUINTEGER>(value[offset] - '0');
      ++offset;
      ++digits;
    }
    while (digits++ < 9) fraction *= 10u;
  }
  return true;
}

SQLRETURN convert_string(const std::string& value, void* buffer,
                         SQLLEN buffer_length, SQLLEN* indicator) {
  if (buffer_length <= 0) return SQL_ERROR;
  const auto capacity = static_cast<std::size_t>(buffer_length - 1);
  const auto copy_length = std::min(capacity, value.size());
  std::memcpy(buffer, value.data(), copy_length);
  static_cast<char*>(buffer)[copy_length] = '\0';
  if (indicator) *indicator = static_cast<SQLLEN>(value.size());
  return copy_length < value.size() ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

SQLRETURN convert_wide_string(const std::string& value, void* buffer,
                              SQLLEN buffer_length, SQLLEN* indicator,
                              ConversionIssue* issue) {
  if (buffer_length < static_cast<SQLLEN>(sizeof(SQLWCHAR))) return SQL_ERROR;
  const auto wide = utf8_to_wide(value);
  if (!wide) {
    if (issue) *issue = ConversionIssue::InvalidCharacterValue;
    return SQL_ERROR;
  }

  const auto required_bytes = wide->size() * sizeof(SQLWCHAR);
  if (indicator) *indicator = static_cast<SQLLEN>(required_bytes);
  const auto buffer_units =
      static_cast<std::size_t>(buffer_length) / sizeof(SQLWCHAR);
  const auto capacity = buffer_units - 1;
  auto copy_length = std::min(capacity, wide->size());
  if constexpr (sizeof(SQLWCHAR) == 2) {
    if (copy_length < wide->size() && copy_length > 0 &&
        (*wide)[copy_length - 1] >= 0xd800 &&
        (*wide)[copy_length - 1] <= 0xdbff) {
      --copy_length;
    }
  }
  std::copy_n(wide->begin(), copy_length, static_cast<SQLWCHAR*>(buffer));
  static_cast<SQLWCHAR*>(buffer)[copy_length] = 0;
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
    *static_cast<SQLREAL*>(buffer) = static_cast<SQLREAL>(*parsed);
    if (indicator) *indicator = sizeof(SQLREAL);
  } else {
    if (*parsed < -std::numeric_limits<SQLDOUBLE>::max() ||
        *parsed > std::numeric_limits<SQLDOUBLE>::max()) {
      if (issue) *issue = ConversionIssue::NumericValueOutOfRange;
      return SQL_ERROR;
    }
    *static_cast<SQLDOUBLE*>(buffer) = static_cast<SQLDOUBLE>(*parsed);
    if (indicator) *indicator = sizeof(SQLDOUBLE);
  }
  return SQL_SUCCESS;
}

SQLRETURN convert_boolean(const std::string& value, void* buffer,
                          SQLLEN* indicator, ConversionIssue* issue) {
  SQLCHAR converted = 0;
  if (value == "t" || value == "true" || value == "1") {
    converted = 1;
  } else if (value != "f" && value != "false" && value != "0") {
    if (issue) *issue = ConversionIssue::InvalidCharacterValue;
    return SQL_ERROR;
  }
  *static_cast<SQLCHAR*>(buffer) = converted;
  if (indicator) *indicator = sizeof(SQLCHAR);
  return SQL_SUCCESS;
}

SQLRETURN convert_date(const std::string& value, void* buffer,
                       SQLLEN* indicator, ConversionIssue* issue) {
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  if (!parse_date(value, year, month, day)) {
    if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
    return SQL_ERROR;
  }
  auto* date = static_cast<SQL_DATE_STRUCT*>(buffer);
  date->year = static_cast<SQLSMALLINT>(year);
  date->month = static_cast<SQLUSMALLINT>(month);
  date->day = static_cast<SQLUSMALLINT>(day);
  if (indicator) *indicator = sizeof(SQL_DATE_STRUCT);
  return SQL_SUCCESS;
}

SQLRETURN convert_time(const std::string& value, void* buffer,
                       SQLLEN* indicator, ConversionIssue* issue) {
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  SQLUINTEGER fraction = 0;
  if (!parse_time(value, hour, minute, second, fraction)) {
    if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
    return SQL_ERROR;
  }
  auto* time = static_cast<SQL_TIME_STRUCT*>(buffer);
  time->hour = static_cast<SQLUSMALLINT>(hour);
  time->minute = static_cast<SQLUSMALLINT>(minute);
  time->second = static_cast<SQLUSMALLINT>(second);
  if (indicator) *indicator = sizeof(SQL_TIME_STRUCT);
  return SQL_SUCCESS;
}

SQLRETURN convert_timestamp(const std::string& value, void* buffer,
                            SQLLEN* indicator, ConversionIssue* issue) {
  if (value.size() < 19 || (value[10] != ' ' && value[10] != 'T')) {
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
  if (!parse_date(std::string_view(value).substr(0, 10), year, month, day) ||
      !parse_time(std::string_view(value).substr(11), hour, minute, second,
                  fraction)) {
    if (issue) *issue = ConversionIssue::InvalidDatetimeFormat;
    return SQL_ERROR;
  }
  auto* timestamp = static_cast<SQL_TIMESTAMP_STRUCT*>(buffer);
  timestamp->year = static_cast<SQLSMALLINT>(year);
  timestamp->month = static_cast<SQLUSMALLINT>(month);
  timestamp->day = static_cast<SQLUSMALLINT>(day);
  timestamp->hour = static_cast<SQLUSMALLINT>(hour);
  timestamp->minute = static_cast<SQLUSMALLINT>(minute);
  timestamp->second = static_cast<SQLUSMALLINT>(second);
  timestamp->fraction = fraction;
  if (indicator) *indicator = sizeof(SQL_TIMESTAMP_STRUCT);
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
      if (indicator) *indicator = static_cast<SQLLEN>(decoded->size());
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
