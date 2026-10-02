#include <gtest/gtest.h>
#include "core/database/mysql/handshake_wire.h"

namespace {
using namespace rs::core::database::mysql;
using rs::util::DbErrorCode;
using Bytes = std::vector<std::byte>;
void number(Bytes& bytes, std::uint32_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
}
void text(Bytes& bytes, std::string_view value) {
  for (const auto c : value) bytes.push_back(static_cast<std::byte>(c));
  bytes.push_back(std::byte{0});
}
Bytes greeting(std::uint32_t capabilities = client_protocol_41 | client_ssl | client_secure_connection | client_plugin_auth) {
  Bytes bytes{std::byte{10}};
  text(bytes, "8.4.11"); number(bytes, 0x12345678, 4);
  for (unsigned i = 0; i < 8; ++i) number(bytes, i, 1);
  number(bytes, 0, 1); number(bytes, capabilities & 65535, 2);
  number(bytes, 45, 1); number(bytes, 2, 2); number(bytes, capabilities >> 16, 2);
  number(bytes, 21, 1);
  for (unsigned i = 0; i < 10; ++i) number(bytes, 0, 1);
  for (unsigned i = 8; i < 20; ++i) number(bytes, i, 1);
  number(bytes, 0, 1); text(bytes, "caching_sha2_password");
  return bytes;
}
Bytes packet(const Bytes& payload, std::uint8_t sequence = 0) {
  Bytes bytes(payload.size() + 4);
  for (std::size_t i = 0; i < 3; ++i)
    bytes[i] = static_cast<std::byte>((payload.size() >> (8 * i)) & 255);
  bytes[3] = static_cast<std::byte>(sequence);
  std::copy(payload.begin(), payload.end(), bytes.begin() + 4);
  return bytes;
}
TEST(MySqlHandshakeWireTest, EverySplitAndByteFragmentRetainsExactOwnedGreeting) {
  const auto expected = greeting();
  const auto bytes = packet(expected);
  for (std::size_t split = 0; split < bytes.size(); ++split) {
    ConnectionPacketDecoder decoder{0};
    auto first = decoder.feed(std::span(bytes).first(split)); ASSERT_TRUE(first);
    EXPECT_EQ(split, first->consumed); EXPECT_FALSE(first->payload);
    auto second = decoder.feed(std::span(bytes).subspan(split)); ASSERT_TRUE(second);
    ASSERT_TRUE(second->payload); EXPECT_EQ(expected, *second->payload);
    EXPECT_EQ(bytes.size() - split, second->consumed);
    auto parsed = decode_server_greeting(*second->payload); ASSERT_TRUE(parsed);
    EXPECT_EQ("8.4.11", parsed->server_version); EXPECT_EQ(0x12345678u, parsed->connection_id);
    EXPECT_EQ(45, parsed->character_set); EXPECT_EQ(2, parsed->status);
    for (unsigned i = 0; i < 20; ++i) EXPECT_EQ(static_cast<std::byte>(i), parsed->challenge[i]);
    second->payload->clear(); EXPECT_EQ("8.4.11", parsed->server_version);
    EXPECT_FALSE(decoder.feed({}));
  }
  ConnectionPacketDecoder decoder{0};
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    auto progress = decoder.feed(std::span(bytes).subspan(i, 1)); ASSERT_TRUE(progress);
    EXPECT_EQ(i + 1 == bytes.size(), progress->payload.has_value());
  }
}
TEST(MySqlHandshakeWireTest, CoalescedPacketsStopAtBoundaryAndSequenceWrapIsExplicit) {
  auto first = packet(greeting(), 255); auto second = packet(greeting(), 0);
  auto combined = first; combined.insert(combined.end(), second.begin(), second.end());
  ConnectionPacketDecoder one{255}; auto a = one.feed(combined); ASSERT_TRUE(a); ASSERT_TRUE(a->payload);
  EXPECT_EQ(first.size(), a->consumed);
  ConnectionPacketDecoder two{0}; auto b = two.feed(std::span(combined).subspan(a->consumed));
  ASSERT_TRUE(b); ASSERT_TRUE(b->payload); EXPECT_EQ(second.size(), b->consumed);
}
TEST(MySqlHandshakeWireTest, HeaderLimitsAndSequenceFailuresAreTerminal) {
  for (const auto size : {std::size_t(5), connection_packet_limit + 1, std::size_t(0xffffff)}) {
    Bytes header; number(header, static_cast<std::uint32_t>(size), 3); number(header, 0, 1);
    ConnectionPacketDecoder decoder{0, 4}; auto result = decoder.feed(header);
    ASSERT_FALSE(result); EXPECT_EQ(DbErrorCode::ResourceLimit, result.error());
    EXPECT_FALSE(decoder.feed(packet({}))); // Cannot resume after a rejected header.
  }
  ConnectionPacketDecoder wrong{1}; auto rejected = wrong.feed(packet(greeting()));
  ASSERT_FALSE(rejected); EXPECT_EQ(DbErrorCode::ProtocolError, rejected.error());
  for (const auto limit : {connection_packet_limit + 1, std::size_t(0xffffff)}) {
    ConnectionPacketDecoder invalid{0, limit}; auto bad_policy = invalid.feed({});
    ASSERT_FALSE(bad_policy); EXPECT_EQ(DbErrorCode::InvalidParameter, bad_policy.error());
  }
  Bytes over_default; number(over_default, static_cast<std::uint32_t>(connection_packet_limit + 1), 3);
  number(over_default, 0, 1);
  ConnectionPacketDecoder bounded{0}; auto over = bounded.feed(over_default);
  ASSERT_FALSE(over); EXPECT_EQ(DbErrorCode::ResourceLimit, over.error());
  ConnectionPacketDecoder exact{0, 4}; auto success = exact.feed(packet(Bytes(4, std::byte{1})));
  ASSERT_TRUE(success); ASSERT_TRUE(success->payload); EXPECT_EQ(4u, success->payload->size());
  ConnectionPacketDecoder maximum{0};
  auto full = maximum.feed(packet(Bytes(connection_packet_limit, std::byte{1})));
  ASSERT_TRUE(full); ASSERT_TRUE(full->payload); EXPECT_EQ(connection_packet_limit, full->payload->size());
  ConnectionPacketDecoder empty{0, 0}; auto zero = empty.feed(packet({}));
  ASSERT_TRUE(zero); ASSERT_TRUE(zero->payload); EXPECT_TRUE(zero->payload->empty());
  EXPECT_FALSE(decode_server_greeting(*zero->payload));
}
TEST(MySqlHandshakeWireTest, EveryTruncatedGreetingIsRejected) {
  const auto bytes = greeting();
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    SCOPED_TRACE(length); EXPECT_FALSE(decode_server_greeting(std::span(bytes).first(length)));
  }
}
TEST(MySqlHandshakeWireTest, VersionTextIsAnObservationNotServerAdmission) {
  auto bytes = greeting(); bytes[1] = std::byte{'5'};
  auto parsed = decode_server_greeting(bytes); ASSERT_TRUE(parsed);
  EXPECT_EQ("5.4.11", parsed->server_version);
  // Session policy must enforce the supported server family before credentials.
}
TEST(MySqlHandshakeWireTest, MissingRequiredCapabilitiesNeverEnableFallback) {
  const auto required = client_protocol_41 | client_ssl | client_secure_connection | client_plugin_auth;
  for (const auto flag : {client_protocol_41, client_ssl, client_secure_connection, client_plugin_auth}) {
    auto result = decode_server_greeting(greeting(required & ~flag)); ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature, result.error());
  }
  auto unknown = decode_server_greeting(greeting(required | 0x80000000));
  ASSERT_TRUE(unknown); EXPECT_NE(0u, unknown->capabilities & 0x80000000);
}
TEST(MySqlHandshakeWireTest, MalformedFieldsAndUnsupportedPluginAreRejectedWithFixedErrors) {
  const auto original = greeting();
  // Version+NUL occupies bytes 1..7; thread id 8..11; scramble 12..19.
  for (auto [offset, value] : {std::pair<std::size_t, unsigned>{0, 9}, {1, 1}, {20, 1},
      {28, 20}, {29, 1}, {51, 1}}) {
    auto bytes = original; bytes[offset] = static_cast<std::byte>(value);
    auto result = decode_server_greeting(bytes); ASSERT_FALSE(result);
    EXPECT_EQ("MySQL server greeting rejected", result.error_message());
  }
  auto plugin = original; plugin[52] = std::byte{'x'};
  auto unsupported = decode_server_greeting(plugin); ASSERT_FALSE(unsupported);
  EXPECT_EQ(DbErrorCode::UnsupportedFeature, unsupported.error());
  auto trailing = original; trailing.push_back(std::byte{0}); EXPECT_FALSE(decode_server_greeting(trailing));
  EXPECT_FALSE(decode_server_greeting(Bytes(connection_packet_limit + 1, std::byte{0})));
}
}
