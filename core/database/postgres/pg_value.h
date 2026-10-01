#pragma once

#include "core/database/native_type_info.h"
#include "core/util/hex.h"

namespace rs::core::database::postgres {

// Private PostgreSQL text-format codec; independent of session composition.
inline std::optional<std::string> normalize_pg_result_value(
    ScalarType type, std::string_view value) {
  if (type == ScalarType::Boolean) {
    if (value == "t" || value == "true" || value == "1") return "1";
    if (value == "f" || value == "false" || value == "0") return "0";
    return std::nullopt;
  }
  if (type != ScalarType::Binary) return std::string(value);
  std::string decoded;
  if (value.starts_with("\\x")) return rs::util::decode_hex(value.substr(2));

  decoded.reserve(value.size());
  for (std::size_t i = 0; i < value.size();) {
    if (value[i] != '\\') {
      decoded.push_back(value[i++]);
      continue;
    }
    if (i + 1 < value.size() && value[i + 1] == '\\') {
      decoded.push_back('\\');
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
    decoded.push_back(static_cast<char>(octet));
    i += 4;
  }
  return decoded;
}

} // namespace rs::core::database::postgres
