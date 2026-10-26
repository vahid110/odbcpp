#pragma once

#include <limits>
#include <optional>
#include <string>
#include <string_view>

// UTF-8 <-> SQLWCHAR conversion for suite-side buffers. Direct builds use the
// driver's converters unchanged; Driver Manager builds cannot link them, so
// they use this strict equivalent (no terminator, nullopt on invalid input).
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include "tests/test_dm_odbc.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace odbcpp::test::text {

inline std::optional<std::vector<SQLWCHAR>> utf8_to_wide(std::string_view input) {
  std::vector<SQLWCHAR> output;
  output.reserve(input.size());
  for (std::size_t offset = 0; offset < input.size();) {
    const auto lead = static_cast<unsigned char>(input[offset]);
    const std::size_t extra = lead < 0x80 ? 0 : (lead >> 5) == 0x6 ? 1
        : (lead >> 4) == 0xe ? 2 : (lead >> 3) == 0x1e ? 3 : 4;
    if (extra == 4 || offset + extra >= input.size()) return std::nullopt;
    std::uint32_t code_point = extra == 0 ? lead : lead & (0x3f >> extra);
    for (std::size_t index = 1; index <= extra; ++index) {
      const auto next = static_cast<unsigned char>(input[offset + index]);
      if ((next & 0xc0) != 0x80) return std::nullopt;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    constexpr std::uint32_t minimum[]{0, 0x80, 0x800, 0x10000};
    if (code_point < minimum[extra] || code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
      return std::nullopt;
    }
    offset += extra + 1;
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (code_point > 0xffff) {
        const auto value = code_point - 0x10000;
        output.push_back(static_cast<SQLWCHAR>(0xd800 + (value >> 10)));
        output.push_back(static_cast<SQLWCHAR>(0xdc00 + (value & 0x3ff)));
        continue;
      }
    }
    output.push_back(static_cast<SQLWCHAR>(code_point));
  }
  return output;
}

inline std::optional<std::string> wide_to_utf8(std::span<const SQLWCHAR> input) {
  std::string output;
  for (std::size_t index = 0; index < input.size(); ++index) {
    std::uint32_t code_point = static_cast<std::uint32_t>(input[index]);
    bool paired = false;
    if constexpr (sizeof(SQLWCHAR) == 2) {
      if (code_point >= 0xd800 && code_point <= 0xdbff) {
        if (index + 1 == input.size()) return std::nullopt;
        const auto low = static_cast<std::uint32_t>(input[++index]);
        if (low < 0xdc00 || low > 0xdfff) return std::nullopt;
        code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
        paired = true;
      }
    }
    if (!paired && ((code_point >= 0xd800 && code_point <= 0xdfff) || code_point > 0x10ffff)) {
      return std::nullopt;
    }
    if (code_point < 0x80) {
      output.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
      output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
      output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else if (code_point < 0x10000) {
      output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
      output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    } else {
      output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
      output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
  }
  return output;
}

// Code points in valid UTF-8 text, or nullopt for invalid text.
inline std::optional<std::size_t> utf8_code_point_count(std::string_view input) {
  if (!utf8_to_wide(input)) return std::nullopt;
  std::size_t count = 0;
  for (const unsigned char byte : input) {
    if ((byte & 0xc0) != 0x80) ++count;
  }
  return count;
}

}  // namespace odbcpp::test::text
#else
#include "odbc/unicode.h"

namespace odbcpp::test::text {
using rs::odbc::utf8_code_point_count;
using rs::odbc::utf8_to_wide;
using rs::odbc::wide_to_utf8;
}  // namespace odbcpp::test::text
#endif

namespace odbcpp::test::text {

// ANSI calls through the Windows manager consume the active code page, while
// direct and POSIX fixtures use UTF-8. Refuse lossy conversion so Unicode
// assertions cannot silently test replacement characters or best-fit text.
inline std::optional<std::string> utf8_to_ansi_input(std::string_view input) {
#if defined(_WIN32) && defined(ODBCPP_TEST_VIA_DRIVER_MANAGER)
  const auto wide = utf8_to_wide(input);
  if (!wide || wide->size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  if (wide->empty()) {
    return std::string{};
  }
  const bool utf8_code_page = GetACP() == CP_UTF8;
  const DWORD flags = utf8_code_page ? 0 : WC_NO_BEST_FIT_CHARS;
  BOOL used_default = FALSE;
  auto* const report = utf8_code_page ? nullptr : &used_default;
  const int count = static_cast<int>(wide->size());
  const int size = WideCharToMultiByte(CP_ACP, flags, wide->data(), count,
                                     nullptr, 0, nullptr, report);
  if (size <= 0 || used_default) {
    return std::nullopt;
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (WideCharToMultiByte(CP_ACP, flags, wide->data(), count,
                          result.data(), size, nullptr, report) != size || used_default) {
    return std::nullopt;
  }
  return result;
#else
  return std::string(input);
#endif
}

}  // namespace odbcpp::test::text
