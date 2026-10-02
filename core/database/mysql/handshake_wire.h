#pragma once
// Private MySQL connection-phase codec; unrelated to the PostgreSQL parser.
#include "core/util/result.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rs::core::database::mysql {
constexpr std::size_t connection_packet_limit = 65536;
constexpr std::uint32_t client_protocol_41 = 0x200;
constexpr std::uint32_t client_ssl = 0x800;
constexpr std::uint32_t client_secure_connection = 0x8000;
constexpr std::uint32_t client_plugin_auth = 0x80000;

struct PacketProgress {
  std::size_t consumed{};
  std::optional<std::vector<std::byte>> payload;
};
// One bounded connection-phase packet. Feed stops exactly at a packet boundary;
// callers retain coalesced bytes and supply the next expected sequence explicitly.
// Failures and completed packets are terminal. Continuation framing is unclaimed.
class ConnectionPacketDecoder {
 public:
  explicit ConnectionPacketDecoder(std::uint8_t expected_sequence,
      std::size_t limit = connection_packet_limit) : sequence_(expected_sequence), limit_(limit) {}
  rs::util::Result<PacketProgress> feed(std::span<const std::byte> input) {
    if (terminal_) return failure(rs::util::DbErrorCode::ProtocolError);
    if (limit_ > connection_packet_limit) return failure(rs::util::DbErrorCode::InvalidParameter);
    std::size_t consumed = 0;
    while (header_size_ < header_.size() && consumed < input.size())
      header_[header_size_++] = input[consumed++];
    if (header_size_ != header_.size()) return PacketProgress{consumed, std::nullopt};
    if (!length_) {
      const auto value = [](std::byte b) { return std::to_integer<std::size_t>(b); };
      const auto size = value(header_[0]) | (value(header_[1]) << 8) | (value(header_[2]) << 16);
      if (header_[3] != static_cast<std::byte>(sequence_)) return failure(rs::util::DbErrorCode::ProtocolError);
      if (size > limit_) return failure(rs::util::DbErrorCode::ResourceLimit);
      length_ = size;
    }
    const auto amount = std::min(*length_ - payload_.size(), input.size() - consumed);
    payload_.insert(payload_.end(), input.begin() + consumed, input.begin() + consumed + amount);
    consumed += amount;
    if (payload_.size() != *length_) return PacketProgress{consumed, std::nullopt};
    terminal_ = true;
    return PacketProgress{consumed, std::move(payload_)};
  }
 private:
  rs::util::Result<PacketProgress> failure(rs::util::DbErrorCode code) {
    terminal_ = true;
    payload_.clear();
    return {code, "MySQL connection packet rejected"};
  }
  std::array<std::byte, 4> header_{};
  std::size_t header_size_{};
  std::optional<std::size_t> length_;
  std::vector<std::byte> payload_;
  std::uint8_t sequence_;
  std::size_t limit_;
  bool terminal_{};
};

struct ServerGreeting {
  std::string server_version;
  std::uint32_t connection_id{};
  std::uint32_t capabilities{};
  std::uint8_t character_set{};
  std::uint16_t status{};
  std::array<std::byte, 20> challenge{};
};

// Deliberately narrow protocol-v10 caching_sha2_password/SSL-capable shape.
// It decodes public greeting data only; it neither authenticates nor grants reuse.
// Server-version eligibility and verified TLS belong to later session policy,
// which must admit the server before sending any credential response.
inline rs::util::Result<ServerGreeting> decode_server_greeting(std::span<const std::byte> data) {
  const auto rejected = [](rs::util::DbErrorCode code) -> rs::util::Result<ServerGreeting> {
    return {code, "MySQL server greeting rejected"};
  };
  if (data.size() > connection_packet_limit) return rejected(rs::util::DbErrorCode::ResourceLimit);
  std::size_t offset = 0;
  const auto read = [&](std::size_t count, std::uint32_t& out) {
    if (count > data.size() - offset) return false;
    out = 0;
    for (std::size_t i = 0; i < count; ++i)
      out |= std::to_integer<std::uint32_t>(data[offset++]) << (8 * i);
    return true;
  };
  std::uint32_t value{};
  if (!read(1, value) || value != 10) return rejected(rs::util::DbErrorCode::ProtocolError);
  const auto version_start = offset;
  while (offset < data.size() && data[offset] != std::byte{0}) {
    const auto c = std::to_integer<unsigned char>(data[offset++]);
    if (c < 32 || c > 126 || offset - version_start > 128)
      return rejected(rs::util::DbErrorCode::ProtocolError);
  }
  if (offset == data.size() || offset == version_start) return rejected(rs::util::DbErrorCode::ProtocolError);
  ServerGreeting greeting;
  greeting.server_version.assign(reinterpret_cast<const char*>(data.data() + version_start), offset - version_start);
  ++offset;
  if (!read(4, greeting.connection_id) || data.size() - offset < 8)
    return rejected(rs::util::DbErrorCode::ProtocolError);
  std::copy_n(data.begin() + offset, 8, greeting.challenge.begin()); offset += 8;
  if (!read(1, value) || value != 0 || !read(2, greeting.capabilities) || !read(1, value))
    return rejected(rs::util::DbErrorCode::ProtocolError);
  greeting.character_set = static_cast<std::uint8_t>(value);
  if (!read(2, value)) return rejected(rs::util::DbErrorCode::ProtocolError);
  greeting.status = static_cast<std::uint16_t>(value);
  if (!read(2, value)) return rejected(rs::util::DbErrorCode::ProtocolError);
  greeting.capabilities |= value << 16;
  const auto required = client_protocol_41 | client_ssl | client_secure_connection | client_plugin_auth;
  if ((greeting.capabilities & required) != required) return rejected(rs::util::DbErrorCode::UnsupportedFeature);
  if (!read(1, value) || value != 21 || data.size() - offset < 23)
    return rejected(rs::util::DbErrorCode::ProtocolError);
  for (std::size_t i = 0; i < 10; ++i)
    if (data[offset++] != std::byte{0}) return rejected(rs::util::DbErrorCode::ProtocolError);
  std::copy_n(data.begin() + offset, 12, greeting.challenge.begin() + 8); offset += 12;
  if (data[offset++] != std::byte{0}) return rejected(rs::util::DbErrorCode::ProtocolError);
  constexpr std::string_view plugin = "caching_sha2_password";
  if (data.size() - offset != plugin.size() + 1 || data.back() != std::byte{0})
    return rejected(rs::util::DbErrorCode::ProtocolError);
  if (!std::equal(plugin.begin(), plugin.end(), data.begin() + offset,
      [](char a, std::byte b) { return static_cast<unsigned char>(a) == std::to_integer<unsigned char>(b); }))
    return rejected(rs::util::DbErrorCode::UnsupportedFeature);
  return greeting;
}
}
