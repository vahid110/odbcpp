#include <gtest/gtest.h>

#include "odbcpp/util/base64.h"

#include <string_view>

namespace {

std::span<const unsigned char> bytes(std::string_view value) {
  return {reinterpret_cast<const unsigned char*>(value.data()), value.size()};
}

std::string text(std::span<const unsigned char> value) {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

}  // namespace

TEST(Base64Test, EncodesAndDecodesRfc4648Vectors) {
  EXPECT_EQ(rs::util::base64_encode(bytes("")), "");
  EXPECT_EQ(rs::util::base64_encode(bytes("f")), "Zg==");
  EXPECT_EQ(rs::util::base64_encode(bytes("fo")), "Zm8=");
  EXPECT_EQ(rs::util::base64_encode(bytes("foo")), "Zm9v");
  EXPECT_EQ(text(rs::util::base64_decode("Zm9vYmFy")), "foobar");
}

TEST(Base64Test, RejectsMalformedAndNonCanonicalInput) {
  EXPECT_THROW((void)rs::util::base64_decode("abc"), std::runtime_error);
  EXPECT_THROW((void)rs::util::base64_decode("=m9v"), std::runtime_error);
  EXPECT_THROW((void)rs::util::base64_decode("Zm=v"), std::runtime_error);
  EXPECT_THROW((void)rs::util::base64_decode("Zg=a"), std::runtime_error);
  EXPECT_THROW((void)rs::util::base64_decode("Zh=="), std::runtime_error);
  EXPECT_THROW((void)rs::util::base64_decode("Zm9="), std::runtime_error);
}
