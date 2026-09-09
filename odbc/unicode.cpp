#include "unicode.h"

#include <cstdint>
#include <limits>

namespace rs::odbc {
namespace {

bool append_utf8(std::string& output, std::uint32_t code_point) {
  if (code_point <= 0x7f) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0xffff) {
    if (code_point >= 0xd800 && code_point <= 0xdfff) return false;
    output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point <= 0x10ffff) {
    output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    return false;
  }
  return true;
}

std::optional<std::uint32_t> next_utf8_code_point(
    std::string_view input, std::size_t& offset) {
  if (offset >= input.size()) return std::nullopt;
  const auto first = static_cast<unsigned char>(input[offset++]);
  if (first <= 0x7f) return first;

  std::size_t continuation_count = 0;
  std::uint32_t code_point = 0;
  std::uint32_t minimum = 0;
  if ((first & 0xe0) == 0xc0) {
    continuation_count = 1;
    code_point = first & 0x1f;
    minimum = 0x80;
  } else if ((first & 0xf0) == 0xe0) {
    continuation_count = 2;
    code_point = first & 0x0f;
    minimum = 0x800;
  } else if ((first & 0xf8) == 0xf0) {
    continuation_count = 3;
    code_point = first & 0x07;
    minimum = 0x10000;
  } else {
    return std::nullopt;
  }
  if (continuation_count > input.size() - offset) return std::nullopt;
  for (std::size_t i = 0; i < continuation_count; ++i) {
    const auto byte = static_cast<unsigned char>(input[offset++]);
    if ((byte & 0xc0) != 0x80) return std::nullopt;
    code_point = (code_point << 6) | (byte & 0x3f);
  }
  if (code_point < minimum || code_point > 0x10ffff ||
      (code_point >= 0xd800 && code_point <= 0xdfff)) {
    return std::nullopt;
  }
  return code_point;
}

} // namespace

std::optional<std::string> wide_to_utf8(std::span<const SQLWCHAR> input) {
  std::string output;
  output.reserve(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    std::uint32_t code_point = static_cast<std::uint32_t>(input[i]);
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (code_point >= 0xd800 && code_point <= 0xdbff) {
        if (++i >= input.size()) return std::nullopt;
        const auto low = static_cast<std::uint32_t>(input[i]);
        if (low < 0xdc00 || low > 0xdfff) return std::nullopt;
        code_point = 0x10000 + ((code_point - 0xd800) << 10) +
            (low - 0xdc00);
      } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
        return std::nullopt;
      }
    }
    if (!append_utf8(output, code_point)) return std::nullopt;
  }
  return output;
}

std::optional<std::string> sqlwchar_to_utf8(
    const SQLWCHAR* input, SQLINTEGER length) {
  if (!input) return std::string{};
  std::size_t units = 0;
  if (length == SQL_NTS) {
    while (input[units] != 0) ++units;
  } else {
    if (length < 0) return std::nullopt;
    units = static_cast<std::size_t>(length);
  }
  return wide_to_utf8(std::span<const SQLWCHAR>(input, units));
}

std::optional<std::vector<SQLWCHAR>> utf8_to_wide(
    std::string_view input) {
  std::vector<SQLWCHAR> output;
  output.reserve(input.size());
  std::size_t offset = 0;
  while (offset < input.size()) {
    const auto code_point = next_utf8_code_point(input, offset);
    if (!code_point) return std::nullopt;
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (*code_point > 0xffff) {
        const auto value = *code_point - 0x10000;
        output.push_back(static_cast<SQLWCHAR>(0xd800 + (value >> 10)));
        output.push_back(static_cast<SQLWCHAR>(0xdc00 + (value & 0x3ff)));
        continue;
      }
    }
    if (*code_point > static_cast<std::uint32_t>(
            std::numeric_limits<SQLWCHAR>::max())) {
      return std::nullopt;
    }
    output.push_back(static_cast<SQLWCHAR>(*code_point));
  }
  return output;
}

} // namespace rs::odbc
