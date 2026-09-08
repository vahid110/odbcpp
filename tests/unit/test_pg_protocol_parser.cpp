#include <gtest/gtest.h>

#include "core/database/postgres/pg_protocol_parser.h"
#include "core/database/query_parameter.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using rs::core::database::QueryParameter;
using rs::core::database::QueryParameterType;
using rs::core::database::postgres::PgProtocolParser;

struct Frame {
  char tag{};
  std::vector<std::byte> payload;
};

std::uint16_t read_u16(std::span<const std::byte> data, std::size_t offset) {
  return (static_cast<std::uint16_t>(data[offset]) << 8) |
         static_cast<std::uint16_t>(data[offset + 1]);
}

std::uint32_t read_u32(std::span<const std::byte> data, std::size_t offset) {
  return (static_cast<std::uint32_t>(data[offset]) << 24) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
         static_cast<std::uint32_t>(data[offset + 3]);
}

std::vector<Frame> split_frames(std::span<const std::byte> wire) {
  std::vector<Frame> frames;
  std::size_t offset = 0;
  while (offset < wire.size()) {
    EXPECT_GE(wire.size() - offset, 5u);
    if (wire.size() - offset < 5) break;
    const auto length = read_u32(wire, offset + 1);
    EXPECT_GE(length, 4u);
    EXPECT_LE(offset + 1 + length, wire.size());
    if (length < 4 || offset + 1 + length > wire.size()) break;
    frames.push_back(Frame{
        static_cast<char>(wire[offset]),
        std::vector<std::byte>(wire.begin() + offset + 5,
                               wire.begin() + offset + 1 + length)});
    offset += 1 + length;
  }
  EXPECT_EQ(offset, wire.size());
  return frames;
}

std::string read_cstring(std::span<const std::byte> data, std::size_t& offset) {
  const auto start = offset;
  while (offset < data.size() && data[offset] != std::byte{0}) ++offset;
  EXPECT_LT(offset, data.size());
  std::string value(reinterpret_cast<const char*>(data.data() + start),
                    offset - start);
  if (offset < data.size()) ++offset;
  return value;
}

TEST(PgProtocolParserTest, CreatesCompleteExtendedQueryExchange) {
  PgProtocolParser parser;
  const std::vector<QueryParameter> params{
      {"7", QueryParameterType::Int32},
      {"x'y; DROP TABLE example; --", QueryParameterType::Text},
      {std::nullopt, QueryParameterType::Text},
  };

  const auto wire = parser.create_prepared_query(
      "SELECT ? + 1, '?'::text, ?::text, ?::text, $$?$$ -- ?\n",
      params);
  const auto frames = split_frames(wire);

  ASSERT_EQ(frames.size(), 5u);
  EXPECT_EQ(frames[0].tag, 'P');
  EXPECT_EQ(frames[1].tag, 'B');
  EXPECT_EQ(frames[2].tag, 'D');
  EXPECT_EQ(frames[3].tag, 'E');
  EXPECT_EQ(frames[4].tag, 'S');

  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  const auto rewritten_sql = read_cstring(frames[0].payload, offset);
  EXPECT_EQ(rewritten_sql,
            "SELECT $1 + 1, '?'::text, $2::text, $3::text, $$?$$ -- ?\n");
  EXPECT_EQ(rewritten_sql.find("DROP TABLE"), std::string::npos);
  ASSERT_LE(offset + 2, frames[0].payload.size());
  EXPECT_EQ(read_u16(frames[0].payload, offset), 3);
  offset += 2;
  EXPECT_EQ(read_u32(frames[0].payload, offset), 23u);
  EXPECT_EQ(read_u32(frames[0].payload, offset + 4), 25u);
  EXPECT_EQ(read_u32(frames[0].payload, offset + 8), 25u);

  offset = 0;
  EXPECT_TRUE(read_cstring(frames[1].payload, offset).empty());
  EXPECT_TRUE(read_cstring(frames[1].payload, offset).empty());
  EXPECT_EQ(read_u16(frames[1].payload, offset), 0);
  offset += 2;
  ASSERT_EQ(read_u16(frames[1].payload, offset), 3);
  offset += 2;

  ASSERT_EQ(read_u32(frames[1].payload, offset), 1u);
  offset += 4;
  EXPECT_EQ(static_cast<char>(frames[1].payload[offset++]), '7');

  const auto text_length = read_u32(frames[1].payload, offset);
  offset += 4;
  ASSERT_EQ(text_length, params[1].value->size());
  const std::string bound_text(
      reinterpret_cast<const char*>(frames[1].payload.data() + offset),
      text_length);
  EXPECT_EQ(bound_text, *params[1].value);
  offset += text_length;

  EXPECT_EQ(read_u32(frames[1].payload, offset), 0xffffffffu);
  offset += 4;
  EXPECT_EQ(read_u16(frames[1].payload, offset), 0);

  ASSERT_EQ(frames[2].payload.size(), 2u);
  EXPECT_EQ(static_cast<char>(frames[2].payload[0]), 'P');
  EXPECT_EQ(frames[2].payload[1], std::byte{0});
  ASSERT_EQ(frames[3].payload.size(), 5u);
  EXPECT_EQ(frames[3].payload[0], std::byte{0});
  EXPECT_EQ(read_u32(frames[3].payload, 1), 0u);
  EXPECT_TRUE(frames[4].payload.empty());
}

TEST(PgProtocolParserTest, PreservesNativeDollarParameters) {
  PgProtocolParser parser;
  const std::vector<QueryParameter> params{
      {"10", QueryParameterType::Int32},
      {"20", QueryParameterType::Int32},
  };
  const auto frames = split_frames(
      parser.create_prepared_query("SELECT $1 + $2", params));
  ASSERT_EQ(frames.size(), 5u);
  std::size_t offset = 0;
  read_cstring(frames[0].payload, offset);
  EXPECT_EQ(read_cstring(frames[0].payload, offset), "SELECT $1 + $2");
}

TEST(PgProtocolParserTest, RejectsMismatchedOdbcMarkerCount) {
  PgProtocolParser parser;
  const std::vector<QueryParameter> params{
      {"one", QueryParameterType::Text},
  };
  EXPECT_THROW(parser.create_prepared_query("SELECT ?, ?", params),
               std::invalid_argument);
}

} // namespace
