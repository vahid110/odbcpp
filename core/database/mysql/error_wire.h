#pragma once

#include <cstddef>
#include <span>

namespace rs::core::database::mysql {
// The proof always negotiates CLIENT_PROTOCOL_41. Native message bytes are
// intentionally opaque; validating the header must not copy or expose them.
// https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_basic_err_packet.html
inline bool valid_protocol41_error_packet(std::span<const std::byte> bytes) {
  if (bytes.size()<9 || bytes[0]!=std::byte{255} || bytes[3]!=std::byte{'#'}) return false;
  for (const auto byte:bytes.subspan(4,5)) {
    const auto ch=std::to_integer<unsigned>(byte);
    if (!((ch>='0' && ch<='9') || (ch>='A' && ch<='Z'))) return false;
  }
  return true;
}
}
