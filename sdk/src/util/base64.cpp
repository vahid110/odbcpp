#include "odbcpp/util/base64.h"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace rs::util {
namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

constexpr std::array<unsigned char, 256> make_decode_table() {
  std::array<unsigned char, 256> table{};
  table.fill(0xff);
  for (std::size_t i = 0; i < kAlphabet.size(); ++i) {
    table[static_cast<unsigned char>(kAlphabet[i])] =
        static_cast<unsigned char>(i);
  }
  return table;
}

constexpr auto kDecode = make_decode_table();

}  // namespace

std::string base64_encode(std::span<const unsigned char> input) {
  if (input.size() > (std::numeric_limits<std::size_t>::max() - 2) / 4 * 3) {
    throw std::length_error("base64 input is too large");
  }

  std::string output;
  output.reserve(4 * ((input.size() + 2) / 3));
  for (std::size_t offset = 0; offset < input.size(); offset += 3) {
    const auto remaining = input.size() - offset;
    const std::uint32_t block =
        (static_cast<std::uint32_t>(input[offset]) << 16) |
        (remaining > 1 ? static_cast<std::uint32_t>(input[offset + 1]) << 8 : 0) |
        (remaining > 2 ? static_cast<std::uint32_t>(input[offset + 2]) : 0);
    output.push_back(kAlphabet[(block >> 18) & 0x3f]);
    output.push_back(kAlphabet[(block >> 12) & 0x3f]);
    output.push_back(remaining > 1 ? kAlphabet[(block >> 6) & 0x3f] : '=');
    output.push_back(remaining > 2 ? kAlphabet[block & 0x3f] : '=');
  }
  return output;
}

std::vector<unsigned char> base64_decode(std::string_view input) {
  if (input.empty()) return {};
  if (input.size() % 4 != 0) {
    throw std::runtime_error("invalid base64 value");
  }

  const std::size_t padding =
      (input.back() == '=' ? 1U : 0U) +
      (input.size() > 1 && input[input.size() - 2] == '=' ? 1U : 0U);
  std::vector<unsigned char> output;
  output.reserve(3 * (input.size() / 4) - padding);

  for (std::size_t offset = 0; offset < input.size(); offset += 4) {
    const bool final_block = offset + 4 == input.size();
    const char third = input[offset + 2];
    const char fourth = input[offset + 3];
    if ((!final_block && (third == '=' || fourth == '=')) ||
        (third == '=' && fourth != '=')) {
      throw std::runtime_error("invalid base64 padding");
    }

    const auto first_value = kDecode[static_cast<unsigned char>(input[offset])];
    const auto second_value = kDecode[static_cast<unsigned char>(input[offset + 1])];
    const auto third_value = third == '=' ? 0 : kDecode[static_cast<unsigned char>(third)];
    const auto fourth_value = fourth == '=' ? 0 : kDecode[static_cast<unsigned char>(fourth)];
    if (first_value == 0xff || second_value == 0xff ||
        third_value == 0xff || fourth_value == 0xff) {
      throw std::runtime_error("invalid base64 value");
    }
    if ((third == '=' && (second_value & 0x0f) != 0) ||
        (fourth == '=' && third != '=' && (third_value & 0x03) != 0)) {
      throw std::runtime_error("non-canonical base64 value");
    }

    const std::uint32_t block =
        (static_cast<std::uint32_t>(first_value) << 18) |
        (static_cast<std::uint32_t>(second_value) << 12) |
        (static_cast<std::uint32_t>(third_value) << 6) |
        static_cast<std::uint32_t>(fourth_value);
    output.push_back(static_cast<unsigned char>((block >> 16) & 0xff));
    if (third != '=') {
      output.push_back(static_cast<unsigned char>((block >> 8) & 0xff));
    }
    if (fourth != '=') output.push_back(static_cast<unsigned char>(block & 0xff));
  }
  return output;
}

}  // namespace rs::util
