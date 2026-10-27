#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rs::util {

// Decode one Unicode scalar. On success offset follows that scalar; after
// failure the offset is unspecified and callers must stop decoding.
std::optional<std::uint32_t> next_utf8_code_point(
    std::string_view input, std::size_t& offset);

// Counts scalars, including embedded NULs; rejects malformed UTF-8.
std::optional<std::size_t> utf8_code_point_count(std::string_view input);

// Copies valid scalars unchanged, including embedded NULs; each byte that does
// not begin a valid scalar becomes U+FFFD, so the result is always valid.
std::string replace_invalid_utf8(std::string_view input);

} // namespace rs::util
