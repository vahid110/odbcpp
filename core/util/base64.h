#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rs::util {

std::string base64_encode(std::span<const unsigned char> input);
std::vector<unsigned char> base64_decode(std::string_view input);

}  // namespace rs::util
