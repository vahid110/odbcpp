// What a manager that widens ANSI text byte by byte (AnsiTextWidenedPerByte)
// stores and gives back, per C library. iODBC 3.52.16 widens with mbrtowc in
// the "C" locale: Darwin maps each byte to the code point of the same value,
// glibc rejects bytes above 0x7f and iODBC substitutes U+FFFD.
#include "tests/test_dm_expectations.h"

#include <gtest/gtest.h>

#include <string>

namespace {
using odbcpp::test::dm::ansi_widened;
using odbcpp::test::dm::ansi_widened_code_point;
using odbcpp::test::dm::ansi_widened_round_trip;

TEST(DriverManagerAnsiWidening, AsciiBytesKeepTheirCodePoint) {
  for (unsigned byte = 0; byte < 0x80; ++byte) {
    EXPECT_EQ(char32_t{byte}, ansi_widened_code_point(static_cast<unsigned char>(byte)));
  }
  EXPECT_EQ("SELECT 'AZ'", ansi_widened("SELECT 'AZ'"));
  EXPECT_EQ("SELECT 'AZ'", ansi_widened_round_trip("SELECT 'AZ'"));
}

TEST(DriverManagerAnsiWidening, HighBytesFollowTheCLibraryCLocale) {
  const std::string e_acute = "\xc3\xa9";
#if defined(__GLIBC__)
  EXPECT_EQ(char32_t{0xfffd}, ansi_widened_code_point(0xc3));
  EXPECT_EQ(char32_t{0xfffd}, ansi_widened_code_point(0xff));
  EXPECT_EQ("A\xef\xbf\xbd\xef\xbf\xbdZ", ansi_widened("A" + e_acute + "Z"));
  EXPECT_EQ("A??Z", ansi_widened_round_trip("A" + e_acute + "Z"));
#else
  EXPECT_EQ(char32_t{0xc3}, ansi_widened_code_point(0xc3));
  EXPECT_EQ(char32_t{0xff}, ansi_widened_code_point(0xff));
  // The Latin-1 reading of the two UTF-8 bytes of U+00E9.
  EXPECT_EQ("A\xc3\x83\xc2\xa9Z", ansi_widened("A" + e_acute + "Z"));
  EXPECT_EQ("A" + e_acute + "Z", ansi_widened_round_trip("A" + e_acute + "Z"));
#endif
}
}  // namespace
