#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rs::util {

// Plain hexadecimal only: no SQL literal prefix or database escape rules.
inline std::optional<std::string> decode_hex(std::string_view value) {
  if (value.size() % 2 != 0) return std::nullopt;
  const auto digit = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::string result;
  result.reserve(value.size() / 2);
  for (std::size_t i = 0; i < value.size(); i += 2) {
    const auto high = digit(value[i]);
    const auto low = digit(value[i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    result.push_back(static_cast<char>((high << 4) | low));
  }
  return result;
}

inline std::string encode_hex(std::string_view value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  if (value.size() > result.max_size() / 2) {
    throw std::length_error("Hexadecimal value is too large");
  }
  result.reserve(value.size() * 2);
  for (const unsigned char byte : value) {
    result.push_back(digits[byte >> 4]);
    result.push_back(digits[byte & 15]);
  }
  return result;
}

} // namespace rs::util
