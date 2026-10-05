#include <gtest/gtest.h>

#include "odbcpp/security/crypto.h"

#include <array>
#include <string_view>

namespace {

std::string hex(std::span<const unsigned char> input) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string output;
  output.reserve(input.size() * 2);
  for (const auto value : input) {
    output.push_back(digits[value >> 4]);
    output.push_back(digits[value & 0x0f]);
  }
  return output;
}

std::span<const unsigned char> bytes(std::string_view value) {
  return {reinterpret_cast<const unsigned char*>(value.data()), value.size()};
}

}  // namespace

TEST(CryptoPrimitivesTest, MatchesStandardDigestAndMacVectors) {
  EXPECT_EQ(hex(rs::core::security::md5(bytes(""))),
            "d41d8cd98f00b204e9800998ecf8427e");
  EXPECT_EQ(hex(rs::core::security::sha256(bytes("abc"))),
            "ba7816bf8f01cfea414140de5dae2223"
            "b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(hex(rs::core::security::hmac_sha256(
                bytes("key"),
                bytes("The quick brown fox jumps over the lazy dog"))),
            "f7bc83f430538424b13298e6aa6fb143"
            "ef4d59a14946175997479dbc2d1a3cd8");
}

TEST(CryptoPrimitivesTest, ReportsCompileAndRuntimeProviderIdentity) {
  const auto identity = rs::core::security::crypto_provider_identity();
#ifdef ODBCPP_TEST_AWS_LC
  EXPECT_EQ(identity.provider, "AWS_LC");
  EXPECT_EQ(identity.runtime_version.find("AWS-LC"), 0u);
  EXPECT_FALSE(identity.fips_enabled);
#else
  EXPECT_EQ(identity.provider, "OPENSSL");
#endif
  EXPECT_FALSE(identity.compile_version.empty());
  EXPECT_FALSE(identity.runtime_version.empty());
}

TEST(CryptoPrimitivesTest, MatchesPbkdf2VectorAndRejectsZeroIterations) {
  EXPECT_EQ(hex(rs::core::security::pbkdf2_hmac_sha256(
                "password", bytes("salt"), 1)),
            "120fb6cffcf8b32c43e7225256c4f837"
            "a86548c92ccc35480805987cb70be17b");
  EXPECT_THROW(
      (void)rs::core::security::pbkdf2_hmac_sha256(
          "password", bytes("salt"), 0),
      std::invalid_argument);
}

TEST(CryptoPrimitivesTest, ComparesAndCleansesBuffers) {
  std::array<unsigned char, 4> value{1, 2, 3, 4};
  const std::array<unsigned char, 4> equal{1, 2, 3, 4};
  const std::array<unsigned char, 4> different{1, 2, 3, 5};
  const std::array<unsigned char, 3> shorter{1, 2, 3};

  EXPECT_TRUE(rs::core::security::constant_time_equal(value, equal));
  EXPECT_FALSE(rs::core::security::constant_time_equal(value, different));
  EXPECT_FALSE(rs::core::security::constant_time_equal(value, shorter));

  rs::core::security::secure_cleanse(value);
  EXPECT_EQ(value, (std::array<unsigned char, 4>{}));
}

TEST(CryptoPrimitivesTest, FillsRandomOutputAndAcceptsEmptyOutput) {
  std::array<unsigned char, 32> output{};
  EXPECT_NO_THROW(rs::core::security::secure_random(output));
  EXPECT_NO_THROW(rs::core::security::secure_random({}));
}
