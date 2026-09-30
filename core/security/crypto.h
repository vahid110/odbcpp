#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace rs::core::security {

using Md5Digest = std::array<unsigned char, 16>;
using Sha256Digest = std::array<unsigned char, 32>;

Md5Digest md5(std::span<const unsigned char> input);
Sha256Digest sha256(std::span<const unsigned char> input);
Sha256Digest hmac_sha256(std::span<const unsigned char> key,
                         std::span<const unsigned char> input);
Sha256Digest pbkdf2_hmac_sha256(
    std::string_view password, std::span<const unsigned char> salt,
    std::uint32_t iterations);

void secure_random(std::span<unsigned char> output);
bool constant_time_equal(std::span<const unsigned char> left,
                         std::span<const unsigned char> right) noexcept;
void secure_cleanse(std::span<unsigned char> value) noexcept;

}  // namespace rs::core::security
