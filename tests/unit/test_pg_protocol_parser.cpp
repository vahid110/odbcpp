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

void append_u16(std::vector<std::byte>& data, std::uint16_t value) {
  data.push_back(static_cast<std::byte>((value >> 8) & 0xff));
  data.push_back(static_cast<std::byte>(value & 0xff));
}

void append_u32(std::vector<std::byte>& data, std::uint32_t value) {
  data.push_back(static_cast<std::byte>((value >> 24) & 0xff));
  data.push_back(static_cast<std::byte>((value >> 16) & 0xff));
  data.push_back(static_cast<std::byte>((value >> 8) & 0xff));
  data.push_back(static_cast<std::byte>(value & 0xff));
}

void append_cstring(std::vector<std::byte>& data, std::string_view value) {
  for (const char ch : value) data.push_back(static_cast<std::byte>(ch));
  data.push_back(std::byte{0});
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

TEST(PgProtocolParserTest, CreatesPostgreSqlScramMessages) {
  PgProtocolParser parser;
  std::vector<std::byte> sasl_payload;
  append_u32(sasl_payload, 10);
  append_cstring(sasl_payload, "SCRAM-SHA-256");
  sasl_payload.push_back(std::byte{0});

  const auto request = parser.parse_auth_request(sasl_payload);
  EXPECT_EQ(request.type,
            rs::core::database::AuthenticationRequest::Type::SASL);
  const auto initial_frames = split_frames(
      parser.create_auth_response(request, "pencil", "user"));
  ASSERT_EQ(initial_frames.size(), 1u);
  EXPECT_EQ(initial_frames[0].tag, 'p');

  std::size_t offset = 0;
  EXPECT_EQ(read_cstring(initial_frames[0].payload, offset),
            "SCRAM-SHA-256");
  const auto initial_length = read_u32(initial_frames[0].payload, offset);
  offset += 4;
  ASSERT_EQ(offset + initial_length, initial_frames[0].payload.size());
  const std::string initial(
      reinterpret_cast<const char*>(initial_frames[0].payload.data() + offset),
      initial_length);
  ASSERT_TRUE(initial.starts_with("n,,n=user,r="));
  const auto nonce = initial.substr(std::string("n,,n=user,r=").size());

  std::vector<std::byte> continue_payload;
  append_u32(continue_payload, 11);
  const auto server_first = "r=" + nonce +
      "server,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
  continue_payload.insert(
      continue_payload.end(),
      reinterpret_cast<const std::byte*>(server_first.data()),
      reinterpret_cast<const std::byte*>(server_first.data() +
                                         server_first.size()));
  const auto continuation = parser.parse_auth_request(continue_payload);
  EXPECT_EQ(continuation.type,
            rs::core::database::AuthenticationRequest::Type::SASLContinue);
  const auto final_frames = split_frames(
      parser.create_auth_response(continuation, "pencil", "user"));
  ASSERT_EQ(final_frames.size(), 1u);
  EXPECT_EQ(final_frames[0].tag, 'p');
  const std::string final(
      reinterpret_cast<const char*>(final_frames[0].payload.data()),
      final_frames[0].payload.size());
  EXPECT_TRUE(final.starts_with("c=biws,r=" + nonce + "server,p="));
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

  ASSERT_EQ(frames.size(), 6u);
  EXPECT_EQ(frames[0].tag, 'P');
  EXPECT_EQ(frames[1].tag, 'D');
  EXPECT_EQ(frames[2].tag, 'B');
  EXPECT_EQ(frames[3].tag, 'D');
  EXPECT_EQ(frames[4].tag, 'E');
  EXPECT_EQ(frames[5].tag, 'S');

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

  ASSERT_EQ(frames[1].payload.size(), 2u);
  EXPECT_EQ(static_cast<char>(frames[1].payload[0]), 'S');
  EXPECT_EQ(frames[1].payload[1], std::byte{0});

  offset = 0;
  EXPECT_TRUE(read_cstring(frames[2].payload, offset).empty());
  EXPECT_TRUE(read_cstring(frames[2].payload, offset).empty());
  EXPECT_EQ(read_u16(frames[2].payload, offset), 0);
  offset += 2;
  ASSERT_EQ(read_u16(frames[2].payload, offset), 3);
  offset += 2;

  ASSERT_EQ(read_u32(frames[2].payload, offset), 1u);
  offset += 4;
  EXPECT_EQ(static_cast<char>(frames[2].payload[offset++]), '7');

  const auto text_length = read_u32(frames[2].payload, offset);
  offset += 4;
  ASSERT_EQ(text_length, params[1].value->size());
  const std::string bound_text(
      reinterpret_cast<const char*>(frames[2].payload.data() + offset),
      text_length);
  EXPECT_EQ(bound_text, *params[1].value);
  offset += text_length;

  EXPECT_EQ(read_u32(frames[2].payload, offset), 0xffffffffu);
  offset += 4;
  EXPECT_EQ(read_u16(frames[2].payload, offset), 0);

  ASSERT_EQ(frames[3].payload.size(), 2u);
  EXPECT_EQ(static_cast<char>(frames[3].payload[0]), 'P');
  EXPECT_EQ(frames[3].payload[1], std::byte{0});
  ASSERT_EQ(frames[4].payload.size(), 5u);
  EXPECT_EQ(frames[4].payload[0], std::byte{0});
  EXPECT_EQ(read_u32(frames[4].payload, 1), 0u);
  EXPECT_TRUE(frames[5].payload.empty());
}

TEST(PgProtocolParserTest, PreservesNativeDollarParameters) {
  PgProtocolParser parser;
  const std::vector<QueryParameter> params{
      {"10", QueryParameterType::Int32},
      {"20", QueryParameterType::Int32},
  };
  const auto frames = split_frames(
      parser.create_prepared_query("SELECT $1 + $2", params));
  ASSERT_EQ(frames.size(), 6u);
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

TEST(PgProtocolParserTest, CountsOnlyUnquotedOdbcParameterMarkers) {
  EXPECT_EQ(3u, PgProtocolParser::parameter_marker_count(
      "SELECT ?, '?'::text, ? /* ? */, $$?$$, ? -- ?\n"));
  EXPECT_EQ(0u, PgProtocolParser::parameter_marker_count(
      "SELECT $1, '$2', $$?$$"));
}

TEST(PgProtocolParserTest, ExtractsResultAndParameterMetadata) {
  PgProtocolParser parser;
  std::vector<rs::core::database::Message> messages;

  rs::core::database::Message parameters;
  parameters.tag = 't';
  append_u16(parameters.payload, 2);
  append_u32(parameters.payload, 25);
  append_u32(parameters.payload, 23);
  messages.push_back(std::move(parameters));

  rs::core::database::Message description;
  description.tag = 'T';
  append_u16(description.payload, 2);
  append_cstring(description.payload, "greeting");
  append_u32(description.payload, 0);
  append_u16(description.payload, 0);
  append_u32(description.payload, 25);
  append_u16(description.payload, 0xffff);
  append_u32(description.payload, 0xffffffff);
  append_u16(description.payload, 0);
  append_cstring(description.payload, "answer");
  append_u32(description.payload, 1234);
  append_u16(description.payload, 2);
  append_u32(description.payload, 23);
  append_u16(description.payload, 4);
  append_u32(description.payload, 0xffffffff);
  append_u16(description.payload, 0);
  messages.push_back(std::move(description));

  rs::core::database::Message complete;
  complete.tag = 'C';
  append_cstring(complete.payload, "UPDATE 7");
  messages.push_back(std::move(complete));

  const auto result = parser.extract_query_result(messages);
  ASSERT_EQ(result.parameter_type_ids,
            (std::vector<std::uint32_t>{25, 23}));
  ASSERT_EQ(result.columns.size(), 2u);
  EXPECT_EQ(result.columns[0].name, "greeting");
  EXPECT_EQ(result.columns[0].type_id, 25u);
  EXPECT_EQ(result.columns[0].type_size, -1);
  EXPECT_EQ(result.columns[1].name, "answer");
  EXPECT_EQ(result.columns[1].table_id, 1234u);
  EXPECT_EQ(result.columns[1].table_column, 2);
  EXPECT_EQ(result.columns[1].type_id, 23u);
  EXPECT_EQ(result.columns[1].type_size, 4);
  EXPECT_EQ(result.command_tag, "UPDATE 7");
  EXPECT_EQ(result.affected_rows, 7u);
}

TEST(PgProtocolParserTest, RejectsTruncatedMetadata) {
  PgProtocolParser parser;
  rs::core::database::Message description;
  description.tag = 'T';
  append_u16(description.payload, 1);
  append_cstring(description.payload, "incomplete");
  EXPECT_THROW(parser.extract_query_result({description}), std::runtime_error);
}

} // namespace
