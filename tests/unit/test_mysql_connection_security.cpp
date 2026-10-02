#include <gtest/gtest.h>
#include "core/database/mysql/connection_security.h"
#include <type_traits>

namespace {
using namespace rs::core::database::mysql;
using rs::util::DbErrorCode;
static_assert(!std::is_copy_constructible_v<Sha2Token>);
static_assert(!std::is_copy_assignable_v<Sha2Token>);
std::array<std::byte, 20> challenge() {
  std::array<std::byte, 20> result{};
  for (std::size_t i = 0; i < result.size(); ++i) result[i] = static_cast<std::byte>(i);
  return result;
}
TEST(MySqlConnectionSecurityTest, SslRequestMatchesProtocol41WireBytesAndIgnoresUnknownFlags) {
  ServerGreeting greeting;
  greeting.capabilities = 0xffffffff;
  auto result = make_ssl_request(greeting); ASSERT_TRUE(result);
  constexpr std::array<unsigned char, 13> expected{32, 0, 0, 1, 0, 138, 8, 0, 0, 0, 1, 0, 45};
  for (std::size_t i = 0; i < expected.size(); ++i)
    EXPECT_EQ(static_cast<std::byte>(expected[i]), result->packet[i]);
  for (std::size_t i = 13; i < result->packet.size(); ++i) EXPECT_EQ(std::byte{0}, result->packet[i]);
  EXPECT_EQ(client_protocol_41 | client_ssl | client_secure_connection | client_plugin_auth, result->capabilities);
}
TEST(MySqlConnectionSecurityTest, SslRequestRejectsMissingCapabilitiesAndInvalidBudget) {
  const auto required = client_protocol_41 | client_ssl | client_secure_connection | client_plugin_auth;
  for (const auto flag : {client_protocol_41, client_ssl, client_secure_connection, client_plugin_auth}) {
    ServerGreeting greeting; greeting.capabilities = required & ~flag;
    auto rejected = make_ssl_request(greeting); ASSERT_FALSE(rejected);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature, rejected.error());
  }
  ServerGreeting greeting; greeting.capabilities = required;
  for (const auto limit : {std::uint32_t(0), std::uint32_t(connection_packet_limit + 1)}) {
    auto rejected = make_ssl_request(greeting, limit); ASSERT_FALSE(rejected);
    EXPECT_EQ(DbErrorCode::InvalidParameter, rejected.error());
  }
  auto minimum = make_ssl_request(greeting, 1); ASSERT_TRUE(minimum);
  EXPECT_EQ(std::byte{1}, minimum->packet[8]);
}
std::string hex(std::span<const unsigned char> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  for (const auto value : bytes) { result.push_back(digits[value >> 4]); result.push_back(digits[value & 15]); }
  return result;
}
TEST(MySqlConnectionSecurityTest, Sha2MatchesIndependentHashlibVectorsIncludingUtf8) {
  const auto salt = challenge();
  auto ascii = make_sha2_token("password", salt, true); ASSERT_TRUE(ascii);
  EXPECT_EQ("dd70fda3e8af6c5044473b5a0c6b43114b0b8b7f4462a4806b2ba095efef0909", hex(ascii->bytes()));
  auto utf8 = make_sha2_token("\xc3\xa9", salt, true); ASSERT_TRUE(utf8);
  EXPECT_EQ("dc753b04834bc887cfd33372179f83743c5037238c0b67197832c59c0ae27c49", hex(utf8->bytes()));
  auto empty = make_sha2_token("", salt, true, 0); ASSERT_TRUE(empty); EXPECT_TRUE(empty->bytes().empty());
  auto changed_salt = salt; changed_salt[0] = std::byte{255};
  auto changed = make_sha2_token("password", changed_salt, true); ASSERT_TRUE(changed);
  EXPECT_NE(hex(ascii->bytes()), hex(changed->bytes()));
}
TEST(MySqlConnectionSecurityTest, AuthenticationFailsClosedAndPreservesBorrowedInputs) {
  auto salt = challenge(); const auto original_salt = salt;
  std::string password = "password";
  for (const auto input : {std::string_view{}, std::string_view(password)}) {
    auto insecure = make_sha2_token(input, salt, false); ASSERT_FALSE(insecure);
    EXPECT_EQ(DbErrorCode::TLSError, insecure.error());
    EXPECT_EQ("MySQL authentication requires verified TLS", insecure.error_message());
  }
  auto exact = make_sha2_token(password, salt, true, password.size()); ASSERT_TRUE(exact);
  auto exceeded = make_sha2_token(password, salt, true, password.size() - 1); ASSERT_FALSE(exceeded);
  EXPECT_EQ(DbErrorCode::ResourceLimit, exceeded.error());
  auto invalid_limit = make_sha2_token("", salt, true, 1024 * 1024 + 1); EXPECT_FALSE(invalid_limit);
  auto nul = make_sha2_token(std::string_view("a\0b", 3), salt, true); ASSERT_FALSE(nul);
  EXPECT_EQ(DbErrorCode::InvalidParameter, nul.error());
  for (std::size_t size = 0; size < salt.size(); ++size)
    EXPECT_FALSE(make_sha2_token(password, std::span(salt).first(size), true));
  std::array<std::byte, 21> oversized{}; EXPECT_FALSE(make_sha2_token(password, oversized, true));
  EXPECT_EQ("password", password); EXPECT_EQ(original_salt, salt);
}
TEST(MySqlConnectionSecurityTest, MoveAndClearCleanseRetainedSourceStorage) {
  auto salt = challenge(); auto created = make_sha2_token("password", salt, true); ASSERT_TRUE(created);
  const auto source_bytes = created->bytes(); const auto expected = hex(source_bytes);
  Sha2Token moved{std::move(*created)};
  EXPECT_TRUE(created->bytes().empty());
  EXPECT_TRUE(std::all_of(source_bytes.begin(), source_bytes.end(), [](auto c) { return c == 0; }));
  EXPECT_EQ(expected, hex(moved.bytes()));
  auto previous = make_sha2_token("other", salt, true); ASSERT_TRUE(previous);
  const auto moved_bytes = moved.bytes(); *previous = std::move(moved);
  EXPECT_TRUE(std::all_of(moved_bytes.begin(), moved_bytes.end(), [](auto c) { return c == 0; }));
  EXPECT_EQ(expected, hex(previous->bytes()));
  const auto retained = previous->bytes(); previous->clear(); EXPECT_TRUE(previous->bytes().empty());
  EXPECT_TRUE(std::all_of(retained.begin(), retained.end(), [](auto c) { return c == 0; }));
}
}
