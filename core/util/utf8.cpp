#include "utf8.h"

namespace rs::util {

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

std::optional<std::size_t> utf8_code_point_count(std::string_view input) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while (offset < input.size()) {
    if (!next_utf8_code_point(input, offset)) return std::nullopt;
    ++count;
  }
  return count;
}

} // namespace rs::util
