// tests/unit/test_datarow.cpp
#include <gtest/gtest.h>

#include "core/database/postgres/pg_protocol_parser.h"

#include <cstddef>
#include <vector>

static rs::core::database::ResultRow parse_datarow(
    const std::vector<unsigned char>& payload) {
  rs::core::database::Message message;
  message.tag = 'D';
  message.payload.reserve(payload.size());
  for (const auto value : payload) {
    message.payload.push_back(static_cast<std::byte>(value));
  }
  rs::core::database::postgres::PgProtocolParser parser;
  auto rows = parser.extract_query_results({message});
  if (rows.size() != 1) throw std::runtime_error("expected one DataRow");
  return std::move(rows.front());
}

TEST(DataRow, SingleColumnText) {
  // ncols=1, len=3, "foo"
  std::vector<unsigned char> pl = {
    0x00, 0x01,              // 1 column
    0x00, 0x00, 0x00, 0x03,  // length=3
    'f','o','o'
  };
  auto row = parse_datarow(pl);
  ASSERT_EQ(row.size(), 1u);
  ASSERT_TRUE(row[0].has_value());
  EXPECT_EQ(*row[0], "foo");
}

TEST(DataRow, MultipleColumnsWithNull) {
  // ncols=3:  "hello", NULL, "xyz"
  std::vector<unsigned char> pl = {
    0x00, 0x03,              // 3 columns
    0x00,0x00,0x00,0x05, 'h','e','l','l','o',
    0xFF,0xFF,0xFF,0xFF,     // -1 = NULL
    0x00,0x00,0x00,0x03, 'x','y','z'
  };
  auto row = parse_datarow(pl);
  ASSERT_EQ(row.size(), 3u);
  ASSERT_TRUE(row[0].has_value());
  EXPECT_EQ(*row[0], "hello");
  EXPECT_FALSE(row[1].has_value());
  ASSERT_TRUE(row[2].has_value());
  EXPECT_EQ(*row[2], "xyz");
}

TEST(DataRow, DistinguishesEmptyStringFromNull) {
  std::vector<unsigned char> pl = {
    0x00, 0x02,
    0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF
  };
  auto row = parse_datarow(pl);
  ASSERT_EQ(row.size(), 2u);
  ASSERT_TRUE(row[0].has_value());
  EXPECT_TRUE(row[0]->empty());
  EXPECT_FALSE(row[1].has_value());
}

TEST(DataRow, TruncatedThrows) {
  // Bad payload: claims 1 column but no length available
  std::vector<unsigned char> pl = { 0x00, 0x01 };
  EXPECT_THROW(parse_datarow(pl), std::runtime_error);
}
