#include "unicode.h"
#include "core/util/utf8.h"

#include <cstdint>
#include <algorithm>
#include <cstring>
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

SQLWCHAR read_wide_unit(const unsigned char* bytes, std::size_t index) {
  SQLWCHAR unit{};
  std::memcpy(&unit, bytes + index * sizeof(unit), sizeof(unit));
  return unit;
}

std::optional<std::string> decode_wide_bytes(
    const void* input, std::size_t units,
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max(), bool* exceeded = nullptr) {
  const auto* bytes = static_cast<const unsigned char*>(input);
  std::string output;
  output.reserve(std::min(units, max_bytes));
  for (std::size_t i = 0; i < units; ++i) {
    std::uint32_t code_point = read_wide_unit(bytes, i);
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (code_point >= 0xd800 && code_point <= 0xdbff) {
        if (++i >= units) return std::nullopt;
        const auto low = static_cast<std::uint32_t>(read_wide_unit(bytes, i));
        if (low < 0xdc00 || low > 0xdfff) return std::nullopt;
        code_point = 0x10000 + ((code_point - 0xd800) << 10) +
            (low - 0xdc00);
      } else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
        return std::nullopt;
      }
    }
    if (code_point > 0x10ffff || (code_point >= 0xd800 && code_point <= 0xdfff)) return std::nullopt;
    const std::size_t bytes_needed = code_point <= 0x7f ? 1 : code_point <= 0x7ff ? 2 : code_point <= 0xffff ? 3 : 4;
    if (bytes_needed > max_bytes - output.size()) {
      if (exceeded) *exceeded = true;
      return std::nullopt;
    }
    if (!append_utf8(output, code_point)) return std::nullopt;
  }
  return output;
}

} // namespace

std::optional<std::string> wide_to_utf8(std::span<const SQLWCHAR> input) {
  return decode_wide_bytes(input.data(), input.size());
}

std::optional<std::string> sqlwchar_to_utf8(
    const void* input, SQLINTEGER length) {
  if (!input) return std::string{};
  std::size_t units = 0;
  if (length == SQL_NTS) {
    const auto* bytes = static_cast<const unsigned char*>(input);
    while (read_wide_unit(bytes, units) != 0) ++units;
  } else {
    if (length < 0) return std::nullopt;
    units = static_cast<std::size_t>(length);
  }
  return decode_wide_bytes(input, units);
}

std::optional<std::string> sqlwchar_to_utf8_bounded(
    const void* input, SQLINTEGER length, std::size_t max_bytes, bool& exceeded) {
  exceeded = false;
  if (!input) return std::string{};
  std::size_t units = 0;
  if (length == SQL_NTS) {
    const auto* bytes = static_cast<const unsigned char*>(input);
    while (read_wide_unit(bytes, units) != 0) {
      if (units == max_bytes) { exceeded = true; return std::nullopt; }
      ++units;
    }
  } else {
    if (length < 0) return std::nullopt;
    units = static_cast<std::size_t>(length);
    if (units > max_bytes) { exceeded = true; return std::nullopt; }
  }
  return decode_wide_bytes(input, units, max_bytes, &exceeded);
}

std::optional<std::vector<SQLWCHAR>> utf8_to_wide(
    std::string_view input) {
  std::vector<SQLWCHAR> output;
  output.reserve(input.size());
  std::size_t offset = 0;
  while (offset < input.size()) {
    const auto code_point = rs::util::next_utf8_code_point(input, offset);
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

std::optional<std::size_t> utf8_code_point_count(std::string_view input) {
  return rs::util::utf8_code_point_count(input);
}

} // namespace rs::odbc
