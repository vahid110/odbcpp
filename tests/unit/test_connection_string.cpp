#include <gtest/gtest.h>

#include "odbc/connection_string.h"

#include <stdexcept>

namespace {

using rs::odbc::ConnectionString;

TEST(ConnectionStringTest, BracedSemicolonsCannotIntroduceNewOptions) {
  const auto parsed = ConnectionString::parse(
      "PWD={secret;SERVER=attacker.example;TransportMode=Sync};"
      "SERVER=database.example;TransportMode=Async");

  EXPECT_EQ(parsed.at("PWD"),
            "secret;SERVER=attacker.example;TransportMode=Sync");
  EXPECT_EQ(parsed.at("SERVER"), "database.example");
  EXPECT_EQ(parsed.at("TRANSPORTMODE"), "Async");
  EXPECT_EQ(parsed.size(), 3u);
}

TEST(ConnectionStringTest, DecodesEscapedClosingBracesAndPreservesBracedSpace) {
  const auto parsed = ConnectionString::parse(
      "DRIVER={ODBC}}PP; Driver};PWD={ a}}b; c };UID=  alice  ;EMPTY={}");

  EXPECT_EQ(parsed.at("DRIVER"), "ODBC}PP; Driver");
  EXPECT_EQ(parsed.at("PWD"), " a}b; c ");
  EXPECT_EQ(parsed.at("UID"), "alice");
  EXPECT_EQ(parsed.at("EMPTY"), "");
}

TEST(ConnectionStringTest, RejectsMalformedBracedValues) {
  EXPECT_THROW(ConnectionString::parse("PWD={secret;SERVER=attacker.example"),
               std::invalid_argument);
  EXPECT_THROW(ConnectionString::parse("PWD={secret}suffix;SERVER=database.example"),
               std::invalid_argument);
  EXPECT_THROW(ConnectionString::parse("PWD={secret}}"),
               std::invalid_argument);
}

TEST(ConnectionStringTest, RejectsEmbeddedNulBeforeParsingAttributes) {
  const std::string input("UID=alice\0;SERVER=attacker.example",
                          sizeof("UID=alice\0;SERVER=attacker.example") - 1);
  EXPECT_THROW(ConnectionString::parse(input), std::invalid_argument);
}

TEST(ConnectionStringTest, KeepsFirstRepeatedOption) {
  const auto parsed = ConnectionString::parse(
      "; NO_EQUALS ; UID=first;UID=second;PWD= plain ;");

  EXPECT_EQ(parsed.at("UID"), "first");
  EXPECT_EQ(parsed.at("PWD"), "plain");
  EXPECT_EQ(parsed.size(), 2u);
}

} // namespace
