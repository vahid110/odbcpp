#include <gtest/gtest.h>

#include "core/database/postgres/pg_protocol_parser.h"
#include "odbcpp/database/query_parameter.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using rs::core::database::AuthenticationRequest;
using rs::core::database::Message;
using rs::core::database::QueryParameter;
using rs::core::database::QueryParameterType;
using rs::core::database::postgres::PgProtocolParser;

TEST(PgProtocolParserTest, ReadyForQueryRequiresOneValidStatusByte) {
  PgProtocolParser parser;
  for (const char status : {'I', 'T', 'E'}) {
    EXPECT_TRUE(parser.is_ready_for_query(
        Message{'Z', {static_cast<std::byte>(status)}}));
  }
  EXPECT_FALSE(parser.is_ready_for_query(Message{'C', {}}));
  EXPECT_THROW(parser.is_ready_for_query(Message{'Z', {}}),
               std::runtime_error);
  EXPECT_THROW(parser.is_ready_for_query(Message{'Z', {std::byte{'X'}}}),
               std::runtime_error);
  EXPECT_THROW(parser.is_ready_for_query(
                   Message{'Z', {std::byte{'I'}, std::byte{'T'}}}),
               std::runtime_error);
}

TEST(PgProtocolParserTest, RejectsEmbeddedNulInStartupFields) {
  PgProtocolParser parser;
  const std::string malformed("alice\0admin", sizeof("alice\0admin") - 1);

  EXPECT_THROW(parser.create_startup_message(malformed, "postgres", {}),
               std::invalid_argument);
  EXPECT_THROW(parser.create_startup_message("alice", malformed, {}),
               std::invalid_argument);
  EXPECT_THROW(parser.create_startup_message(
                   "alice", "postgres", {{malformed, "value"}}),
               std::invalid_argument);
  EXPECT_THROW(parser.create_startup_message(
                   "alice", "postgres", {{"application_name", malformed}}),
               std::invalid_argument);
}

TEST(PgProtocolParserTest, RejectsEmbeddedNulInAuthenticationCredentials) {
  PgProtocolParser parser;
  AuthenticationRequest request;
  request.type = AuthenticationRequest::Type::Cleartext;
  const std::string malformed("alice\0admin", sizeof("alice\0admin") - 1);

  EXPECT_THROW(parser.create_auth_response(request, malformed, "alice", true),
               std::invalid_argument);
  EXPECT_THROW(parser.create_auth_response(request, "password", malformed, true),
               std::invalid_argument);
}

TEST(PgProtocolParserTest, CleartextPasswordRequiresVerifiedPeerIdentity) {
  PgProtocolParser parser;
  AuthenticationRequest request;
  request.type = AuthenticationRequest::Type::Cleartext;

  EXPECT_THROW(
      parser.create_auth_response(request, "top-secret", "alice", false),
      std::runtime_error);

  const auto response =
      parser.create_auth_response(request, "top-secret", "alice", true);
  ASSERT_EQ(response.size(), 1u + 4u + std::string_view("top-secret").size() + 1u);
  EXPECT_EQ(response.front(), std::byte{'p'});
  EXPECT_EQ(response.back(), std::byte{0});
  const std::string serialized(
      reinterpret_cast<const char*>(response.data() + 5),
      response.size() - 6);
  EXPECT_EQ(serialized, "top-secret");
}

TEST(PgProtocolParserTest, AuthenticationOkAndCleartextHaveExactLengths) {
  PgProtocolParser parser;
  std::vector<std::byte> payload(4, std::byte{0});
  EXPECT_EQ(AuthenticationRequest::Type::None,
            parser.parse_auth_request(payload).type);
  payload.push_back(std::byte{0});
  EXPECT_THROW(parser.parse_auth_request(payload), std::runtime_error);

  payload.resize(4);
  payload[3] = std::byte{3};
  EXPECT_EQ(AuthenticationRequest::Type::Cleartext,
            parser.parse_auth_request(payload).type);
  payload.push_back(std::byte{0});
  EXPECT_THROW(parser.parse_auth_request(payload), std::runtime_error);
}

TEST(PgProtocolParserTest, Md5AuthenticationRequiresExactlyFourSaltBytes) {
  PgProtocolParser parser;
  std::vector<std::byte> payload{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{5},
      std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto parsed = parser.parse_auth_request(payload);
  ASSERT_EQ(parsed.type, AuthenticationRequest::Type::MD5);
  ASSERT_EQ(parsed.challenge_data.size(), 4u);

  const auto response =
      parser.create_auth_response(parsed, "password", "alice", false);
  ASSERT_EQ(response.size(), 41u);
  EXPECT_EQ(response[0], std::byte{'p'});
  EXPECT_EQ(response[4], std::byte{40});
  EXPECT_EQ(response[5], std::byte{'m'});
  EXPECT_EQ(response[6], std::byte{'d'});
  EXPECT_EQ(response[7], std::byte{'5'});
  EXPECT_EQ(response.back(), std::byte{0});

  payload.pop_back();
  EXPECT_THROW(parser.parse_auth_request(payload), std::runtime_error);
  payload.push_back(std::byte{4});
  payload.push_back(std::byte{5});
  EXPECT_THROW(parser.parse_auth_request(payload), std::runtime_error);

  AuthenticationRequest direct;
  direct.type = AuthenticationRequest::Type::MD5;
  for (const std::size_t salt_size : {0u, 3u, 5u}) {
    direct.challenge_data.resize(salt_size);
    EXPECT_THROW(parser.create_auth_response(
                     direct, "password", "alice", false),
                 std::invalid_argument);
  }
}

TEST(PgProtocolParserTest, ScramRequiresServerFinalAndRejectsDowngrade) {
  PgProtocolParser parser;
  std::vector<std::byte> offer{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{10}};
  for (const char ch : std::string_view("SCRAM-SHA-256")) {
    offer.push_back(static_cast<std::byte>(ch));
  }
  offer.push_back(std::byte{0});
  offer.push_back(std::byte{0});
  const auto request = parser.parse_auth_request(offer);
  ASSERT_FALSE(parser.create_auth_response(
                         request, "postgres", "postgres", false)
                   .empty());
  const std::vector<std::byte> auth_ok(4, std::byte{0});
  EXPECT_THROW(parser.parse_auth_request(auth_ok), std::runtime_error);
  const std::vector<std::byte> cleartext{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{3}};
  EXPECT_THROW(parser.parse_auth_request(cleartext), std::runtime_error);
  AuthenticationRequest forged_cleartext;
  forged_cleartext.type = AuthenticationRequest::Type::Cleartext;
  EXPECT_THROW(parser.create_auth_response(
                   forged_cleartext, "postgres", "postgres", true),
               std::runtime_error);

  parser.create_startup_message("postgres", "postgres", {});
  EXPECT_EQ(AuthenticationRequest::Type::None,
            parser.parse_auth_request(auth_ok).type);
}

TEST(PgProtocolParserTest, BackendKeyDataMatchesProtocol30Length) {
  PgProtocolParser parser;
  std::vector<std::byte> frame(13, std::byte{0});
  frame[0] = std::byte{'K'};
  frame[4] = std::byte{12};
  EXPECT_EQ(8u, parser.parse_message(frame).payload.size());

  frame.pop_back();
  frame[4] = std::byte{11};
  EXPECT_THROW(parser.parse_message(frame), std::runtime_error);

  frame.push_back(std::byte{0});
  frame.push_back(std::byte{0});
  frame[4] = std::byte{13};
  EXPECT_THROW(parser.parse_message(frame), std::runtime_error);
}

TEST(PgProtocolParserTest, FixedLengthQueryResponsesRejectPayloads) {
  PgProtocolParser parser;
  for (const char tag : {'1', '2', '3', 'n', 'I', 's'}) {
    SCOPED_TRACE(tag);
    std::vector<std::byte> frame{
        static_cast<std::byte>(tag), std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{4}};
    EXPECT_TRUE(parser.parse_message(frame).payload.empty());
    frame[4] = std::byte{5};
    frame.push_back(std::byte{0});
    EXPECT_THROW(parser.parse_message(frame), std::runtime_error);
  }
}

TEST(PgProtocolParserTest, NotificationResponseHasExactFields) {
  PgProtocolParser parser;
  const auto frame = [](std::vector<std::byte> payload) {
    std::vector<std::byte> bytes{
        std::byte{'A'}, std::byte{0}, std::byte{0}, std::byte{0},
        static_cast<std::byte>(payload.size() + 4)};
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
  };
  const std::vector<std::byte> valid{
      std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1},
      std::byte{'c'}, std::byte{0}, std::byte{'p'}, std::byte{0}};
  EXPECT_EQ(valid, parser.parse_message(frame(valid)).payload);

  auto truncated_pid = valid;
  truncated_pid.resize(3);
  EXPECT_THROW(parser.parse_message(frame(truncated_pid)), std::runtime_error);
  auto missing_channel = valid;
  missing_channel.resize(5);
  EXPECT_THROW(parser.parse_message(frame(missing_channel)), std::runtime_error);
  auto missing_payload = valid;
  missing_payload.pop_back();
  EXPECT_THROW(parser.parse_message(frame(missing_payload)), std::runtime_error);
  auto trailing = valid;
  trailing.push_back(std::byte{0});
  EXPECT_THROW(parser.parse_message(frame(trailing)), std::runtime_error);
}

TEST(PgProtocolParserTest, RejectsTrailingBytesAfterOneFrame) {
  PgProtocolParser parser;
  const std::vector<std::byte> frame{
      std::byte{'Z'}, std::byte{0}, std::byte{0}, std::byte{0},
      std::byte{5}, std::byte{'I'}, std::byte{0}};
  EXPECT_THROW(parser.parse_message(frame), std::runtime_error);
}

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

TEST(PgProtocolParserTest, ErrorResponseRequiresCompleteUniqueFields) {
  PgProtocolParser parser;
  Message valid{'E', {}};
  append_cstring(valid.payload, "SERROR");
  append_cstring(valid.payload, "C42601");
  append_cstring(valid.payload, "Msyntax error");
  append_cstring(valid.payload, "Xfuture field");
  valid.payload.push_back(std::byte{0});
  EXPECT_EQ("syntax error", parser.extract_error_message(valid));
  EXPECT_EQ("42601", parser.extract_error_sqlstate(valid));

  for (const char missing : {'S', 'C', 'M'}) {
    Message incomplete{'E', {}};
    if (missing != 'S') append_cstring(incomplete.payload, "SERROR");
    if (missing != 'C') append_cstring(incomplete.payload, "C42601");
    if (missing != 'M') append_cstring(incomplete.payload, "Msyntax error");
    incomplete.payload.push_back(std::byte{0});
    EXPECT_THROW(parser.extract_error_message(incomplete), std::runtime_error);
  }

  Message empty_message{'E', {}};
  append_cstring(empty_message.payload, "SERROR");
  append_cstring(empty_message.payload, "C42601");
  append_cstring(empty_message.payload, "M");
  empty_message.payload.push_back(std::byte{0});
  EXPECT_THROW(parser.extract_error_message(empty_message),
               std::runtime_error);

  auto duplicate = valid;
  duplicate.payload.pop_back();
  append_cstring(duplicate.payload, "SERROR");
  duplicate.payload.push_back(std::byte{0});
  EXPECT_THROW(parser.extract_error_message(duplicate), std::runtime_error);

  auto unterminated = valid;
  unterminated.payload.pop_back();
  unterminated.payload.pop_back();
  EXPECT_THROW(parser.extract_error_message(unterminated), std::runtime_error);

  auto trailing = valid;
  trailing.payload.push_back(std::byte{1});
  EXPECT_THROW(parser.extract_error_message(trailing), std::runtime_error);

  auto invalid_state = valid;
  invalid_state.payload.clear();
  append_cstring(invalid_state.payload, "SERROR");
  append_cstring(invalid_state.payload, "C22@12");
  append_cstring(invalid_state.payload, "Mbad state");
  invalid_state.payload.push_back(std::byte{0});
  EXPECT_THROW(parser.extract_error_sqlstate(invalid_state),
               std::runtime_error);
}

TEST(PgProtocolParserTest, ErrorAfterCommandIsASeparatePendingResult) {
  PgProtocolParser parser;
  Message command{'C', {}};
  append_cstring(command.payload, "SELECT 1");
  Message error{'E', {}};
  append_cstring(error.payload, "SERROR");
  append_cstring(error.payload, "C22012");
  append_cstring(error.payload, "Mdivision by zero");
  error.payload.push_back(std::byte{0});

  const auto result = parser.extract_query_result({command, error});
  EXPECT_EQ(rs::core::database::StatementKind::SelectCursor, result.statement_kind);
  ASSERT_EQ(1u, result.additional_results.size());
  ASSERT_TRUE(result.additional_results.front().error);
  EXPECT_EQ("Query error: division by zero",
            result.additional_results.front().error->message);
  EXPECT_EQ("22012", result.additional_results.front().error->native_state);

  const auto first_error = parser.extract_query_result({error});
  ASSERT_TRUE(first_error.error);
  EXPECT_EQ("Query error: division by zero", first_error.error->message);
  EXPECT_TRUE(first_error.additional_results.empty());
}

rs::core::database::Message one_column_description(
    std::string_view name, std::uint32_t type_oid = 23,
    std::uint16_t type_size = 4, std::uint16_t format_code = 0) {
  rs::core::database::Message message;
  message.tag = 'T';
  append_u16(message.payload, 1);
  append_cstring(message.payload, name);
  append_u32(message.payload, 0);
  append_u16(message.payload, 0);
  append_u32(message.payload, type_oid);
  append_u16(message.payload, type_size);
  append_u32(message.payload, 0xffffffff);
  append_u16(message.payload, format_code);
  return message;
}

rs::core::database::Message one_column_row(std::string_view value) {
  rs::core::database::Message message;
  message.tag = 'D';
  append_u16(message.payload, 1);
  append_u32(message.payload, static_cast<std::uint32_t>(value.size()));
  for (const char ch : value) {
    message.payload.push_back(static_cast<std::byte>(ch));
  }
  return message;
}

rs::core::database::Message command_complete(std::string_view tag) {
  rs::core::database::Message message;
  message.tag = 'C';
  append_cstring(message.payload, tag);
  return message;
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
      parser.create_auth_response(request, "pencil", "user", false));
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
      parser.create_auth_response(continuation, "pencil", "user", false));
  ASSERT_EQ(final_frames.size(), 1u);
  EXPECT_EQ(final_frames[0].tag, 'p');
  const std::string final(
      reinterpret_cast<const char*>(final_frames[0].payload.data()),
      final_frames[0].payload.size());
  EXPECT_TRUE(final.starts_with("c=biws,r=" + nonce + "server,p="));
  EXPECT_THROW(parser.parse_auth_request(
                   std::vector<std::byte>(4, std::byte{0})),
               std::runtime_error);
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

// Mechanics only: pinned AWS ODBC 56d35297f9bee0cc31c0148581c87ca455639a39
// rsMetadataAPIHelper.cpp:186 and rsMetadataServerProxyHelper.cpp:806-810 use
// this template and database/schema/table order. rsutil.c:16303-16325 permits
// OID0 with StringType=unspecified (default varchar instead uses OID1043).
// Our extra Describe Statement is intentional existing behavior, not evidence
// of Redshift server acceptance, capability negotiation or native SHOW widths.
TEST(PgProtocolParserTest, ShowPrimaryKeysKeepsUnspecifiedIdentifiersInBindValues) {
  PgProtocolParser parser;
  const std::vector<QueryParameter> params{
      {"database.'\"_%?", QueryParameterType::Unspecified},
      {"schema.\"'_%?", QueryParameterType::Unspecified},
      {"table.'\"_%?; --", QueryParameterType::Unspecified},
  };
  const auto frames = split_frames(parser.create_prepared_query(
      "SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;", params));
  ASSERT_EQ(6u, frames.size());
  const std::string tags = "PDBDES";
  for (std::size_t i = 0; i < tags.size(); ++i) EXPECT_EQ(tags[i], frames[i].tag);

  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  const auto sql = read_cstring(frames[0].payload, offset);
  EXPECT_EQ("SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE $1.$2.$3;", sql);
  for (const auto& param : params) EXPECT_EQ(std::string::npos, sql.find(*param.value));
  ASSERT_EQ(offset + 14u, frames[0].payload.size());
  ASSERT_EQ(3u, read_u16(frames[0].payload, offset));
  offset += 2;
  for (std::size_t i = 0; i < params.size(); ++i) {
    EXPECT_EQ(0u, read_u32(frames[0].payload, offset));
    offset += 4;
  }
  EXPECT_EQ(offset, frames[0].payload.size());

  // The existing extra Describe targets the unnamed statement before Bind.
  ASSERT_EQ(2u, frames[1].payload.size());
  EXPECT_EQ(std::byte{'S'}, frames[1].payload[0]);
  EXPECT_EQ(std::byte{0}, frames[1].payload[1]);
  offset = 0;
  EXPECT_TRUE(read_cstring(frames[2].payload, offset).empty()); // portal
  EXPECT_TRUE(read_cstring(frames[2].payload, offset).empty()); // statement
  ASSERT_LE(offset + 4u, frames[2].payload.size());
  EXPECT_EQ(0u, read_u16(frames[2].payload, offset)); // text parameter format
  offset += 2;
  ASSERT_EQ(3u, read_u16(frames[2].payload, offset));
  offset += 2;
  for (const auto& param : params) {
    ASSERT_LE(offset + 4u, frames[2].payload.size());
    const auto length = read_u32(frames[2].payload, offset);
    offset += 4;
    ASSERT_EQ(param.value->size(), length);
    ASSERT_LE(offset + length, frames[2].payload.size());
    EXPECT_EQ(*param.value, std::string(
        reinterpret_cast<const char*>(frames[2].payload.data() + offset), length));
    offset += length;
  }
  ASSERT_EQ(offset + 2u, frames[2].payload.size());
  EXPECT_EQ(0u, read_u16(frames[2].payload, offset)); // text result format
  ASSERT_EQ(2u, frames[3].payload.size());
  EXPECT_EQ(std::byte{'P'}, frames[3].payload[0]);
  EXPECT_EQ(std::byte{0}, frames[3].payload[1]);
  ASSERT_EQ(5u, frames[4].payload.size());
  EXPECT_EQ(std::byte{0}, frames[4].payload[0]);
  EXPECT_EQ(0u, read_u32(frames[4].payload, 1)); // unlimited protocol Execute
  EXPECT_TRUE(frames[5].payload.empty());
}

TEST(PgProtocolParserTest, ShowPrimaryKeysTextHintRemainsOid25NotVarcharOrUnspecified) {
  PgProtocolParser parser;
  const std::vector<QueryParameter> params{
      {"database", QueryParameterType::Text},
      {"schema", QueryParameterType::Text},
      {"table", QueryParameterType::Text},
  };
  const auto frames = split_frames(parser.create_prepared_query(
      "SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE ?.?.?;", params));
  ASSERT_EQ(6u, frames.size());
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ("SHOW CONSTRAINTS PRIMARY KEYS FROM TABLE $1.$2.$3;",
      read_cstring(frames[0].payload, offset));
  ASSERT_EQ(offset + 14u, frames[0].payload.size());
  ASSERT_EQ(3u, read_u16(frames[0].payload, offset));
  offset += 2;
  for (std::size_t i = 0; i < params.size(); ++i) {
    const auto oid = read_u32(frames[0].payload, offset);
    EXPECT_EQ(25u, oid);
    EXPECT_NE(0u, oid);
    EXPECT_NE(1043u, oid);
    offset += 4;
  }
}

TEST(PgProtocolParserTest, CreatesStatementDescriptionExchange) {
  PgProtocolParser parser;
  const std::vector<QueryParameterType> parameter_types{
      QueryParameterType::Int32,
      QueryParameterType::Text,
  };

  const auto frames = split_frames(parser.create_statement_description(
      "SELECT ? + 1, '?'::text, ?::text", parameter_types));

  ASSERT_EQ(frames.size(), 3u);
  EXPECT_EQ(frames[0].tag, 'P');
  EXPECT_EQ(frames[1].tag, 'D');
  EXPECT_EQ(frames[2].tag, 'S');

  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ(read_cstring(frames[0].payload, offset),
            "SELECT $1 + 1, '?'::text, $2::text");
  ASSERT_LE(offset + 10, frames[0].payload.size());
  EXPECT_EQ(read_u16(frames[0].payload, offset), 2);
  offset += 2;
  EXPECT_EQ(read_u32(frames[0].payload, offset), 23u);
  offset += 4;
  EXPECT_EQ(read_u32(frames[0].payload, offset), 25u);
  offset += 4;
  EXPECT_EQ(offset, frames[0].payload.size());

  ASSERT_EQ(frames[1].payload.size(), 2u);
  EXPECT_EQ(frames[1].payload[0], std::byte{'S'});
  EXPECT_EQ(frames[1].payload[1], std::byte{0});
  EXPECT_TRUE(frames[2].payload.empty());
}

TEST(PgProtocolParserTest, DateParameterUsesPostgresqlDateOid) {
  PgProtocolParser parser;
  const std::vector<QueryParameterType> types{QueryParameterType::Date};
  const auto frames = split_frames(parser.create_statement_description(
      "SELECT ?", types));
  ASSERT_EQ(frames.size(), 3u);
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ("SELECT $1", read_cstring(frames[0].payload, offset));
  ASSERT_LE(offset + 6, frames[0].payload.size());
  EXPECT_EQ(1, read_u16(frames[0].payload, offset));
  offset += 2;
  EXPECT_EQ(1082u, read_u32(frames[0].payload, offset));
}

TEST(PgProtocolParserTest, TimeParameterUsesPostgresqlTimeOid) {
  PgProtocolParser parser;
  const std::vector<QueryParameterType> types{QueryParameterType::Time};
  const auto frames = split_frames(parser.create_statement_description(
      "SELECT ?", types));
  ASSERT_EQ(frames.size(), 3u);
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ("SELECT $1", read_cstring(frames[0].payload, offset));
  ASSERT_LE(offset + 6, frames[0].payload.size());
  EXPECT_EQ(1, read_u16(frames[0].payload, offset));
  offset += 2;
  EXPECT_EQ(1083u, read_u32(frames[0].payload, offset));
}

TEST(PgProtocolParserTest, TimestampParameterUsesPostgresqlTimestampOid) {
  PgProtocolParser parser;
  const std::vector<QueryParameterType> types{QueryParameterType::Timestamp};
  const auto frames = split_frames(parser.create_statement_description(
      "SELECT ?", types));
  ASSERT_EQ(frames.size(), 3u);
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ("SELECT $1", read_cstring(frames[0].payload, offset));
  ASSERT_LE(offset + 6, frames[0].payload.size());
  EXPECT_EQ(1, read_u16(frames[0].payload, offset));
  offset += 2;
  EXPECT_EQ(1114u, read_u32(frames[0].payload, offset));
}

TEST(PgProtocolParserTest, RejectsMismatchedDescriptionMarkerCount) {
  PgProtocolParser parser;
  const std::vector<QueryParameterType> parameter_types{
      QueryParameterType::Int32,
  };
  EXPECT_THROW(
      parser.create_statement_description("SELECT ?, ?", parameter_types),
      std::invalid_argument);
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

TEST(PgProtocolParserTest, RejectsEmbeddedNulInSqlText) {
  PgProtocolParser parser;
  const std::string sql("SELECT 1\0;SELECT 2",
                        sizeof("SELECT 1\0;SELECT 2") - 1);

  EXPECT_THROW(parser.create_simple_query(sql), std::invalid_argument);
  EXPECT_THROW(parser.create_prepared_query(
                   sql, std::span<const QueryParameter>{}),
               std::invalid_argument);
  EXPECT_THROW(parser.create_statement_description(
                   sql, std::span<const QueryParameterType>{}),
               std::invalid_argument);
}

TEST(PgProtocolParserTest, CountsOnlyUnquotedOdbcParameterMarkers) {
  EXPECT_EQ(3u, PgProtocolParser::parameter_marker_count(
      "SELECT ?, '?'::text, ? /* ? */, $$?$$, ? -- ?\n"));
  EXPECT_EQ(0u, PgProtocolParser::parameter_marker_count(
      "SELECT $1, '$2', $$?$$"));
}

TEST(PgProtocolParserTest, DistinguishesIntegerAndFloatParameterTypeOids) {
  PgProtocolParser parser;
  const std::vector<QueryParameterType> types{
      QueryParameterType::Int16, QueryParameterType::Int32,
      QueryParameterType::Float32, QueryParameterType::Float64};
  const auto frames = split_frames(parser.create_statement_description(
      "SELECT ?, ?, ?, ?", types));
  ASSERT_EQ(3u, frames.size());
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ("SELECT $1, $2, $3, $4",
            read_cstring(frames[0].payload, offset));
  ASSERT_LE(offset + 18, frames[0].payload.size());
  EXPECT_EQ(4, read_u16(frames[0].payload, offset));
  offset += 2;
  constexpr std::uint32_t expected_oids[]{21, 23, 700, 701};
  for (const auto oid : expected_oids) {
    EXPECT_EQ(oid, read_u32(frames[0].payload, offset));
    offset += 4;
  }
  EXPECT_EQ(offset, frames[0].payload.size());
}

TEST(PgProtocolParserTest, BackslashInQuotedIdentifierDoesNotHideMarker) {
  PgProtocolParser parser;
  constexpr std::string_view sql =
      R"(SELECT 1 AS "slash\", ?::integer AS value)";
  EXPECT_EQ(1u, PgProtocolParser::parameter_marker_count(sql));

  const std::vector<QueryParameterType> types{QueryParameterType::Int32};
  const auto frames = split_frames(
      parser.create_statement_description(sql, types));
  ASSERT_FALSE(frames.empty());
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ(R"(SELECT 1 AS "slash\", $1::integer AS value)",
            read_cstring(frames[0].payload, offset));
}

TEST(PgProtocolParserTest, BackslashInOrdinaryStringDoesNotHideMarker) {
  PgProtocolParser parser;
  constexpr std::string_view sql =
      R"(SELECT 'slash\' AS literal, ?::integer AS value)";
  EXPECT_EQ(1u, PgProtocolParser::parameter_marker_count(sql));
  EXPECT_EQ(1u, PgProtocolParser::parameter_marker_count(
      R"(SELECT E'slash\' ? hidden' AS literal, ?::integer AS value)"));

  const std::vector<QueryParameterType> types{QueryParameterType::Int32};
  const auto frames = split_frames(
      parser.create_statement_description(sql, types));
  ASSERT_FALSE(frames.empty());
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ(R"(SELECT 'slash\' AS literal, $1::integer AS value)",
            read_cstring(frames[0].payload, offset));
}

TEST(PgProtocolParserTest, DollarSignsInIdentifierDoNotStartQuotedString) {
  PgProtocolParser parser;
  constexpr std::string_view sql =
      "SELECT 1 AS foo$tag$bar, ?::integer AS value";
  EXPECT_EQ(1u, PgProtocolParser::parameter_marker_count(sql));
  EXPECT_EQ(1u, PgProtocolParser::parameter_marker_count(
      "SELECT $tag$?$tag$, ?::integer"));

  const std::vector<QueryParameterType> types{QueryParameterType::Int32};
  const auto frames = split_frames(
      parser.create_statement_description(sql, types));
  ASSERT_FALSE(frames.empty());
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ("SELECT 1 AS foo$tag$bar, $1::integer AS value",
            read_cstring(frames[0].payload, offset));
}

TEST(PgProtocolParserTest, CarriageReturnEndsLineCommentBeforeMarker) {
  PgProtocolParser parser;
  constexpr std::string_view sql =
      "SELECT 1 -- ignored ?\r, ?::integer AS value";
  EXPECT_EQ(1u, PgProtocolParser::parameter_marker_count(sql));

  const std::vector<QueryParameterType> types{QueryParameterType::Int32};
  const auto frames = split_frames(
      parser.create_statement_description(sql, types));
  ASSERT_FALSE(frames.empty());
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(frames[0].payload, offset).empty());
  EXPECT_EQ("SELECT 1 -- ignored ?\r, $1::integer AS value",
            read_cstring(frames[0].payload, offset));
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
  EXPECT_EQ(result.columns[1].type_id, 23u);
  EXPECT_EQ(result.columns[1].type_size, 4);
  EXPECT_EQ(rs::core::database::StatementKind::UpdateWhere, result.statement_kind);
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

TEST(PgProtocolParserTest, PreservesOrderedQueryResults) {
  PgProtocolParser parser;
  std::vector<rs::core::database::Message> messages;
  messages.push_back(one_column_description("first"));
  messages.push_back(one_column_row("1"));
  messages.push_back(command_complete("SELECT 1"));
  messages.push_back(command_complete("UPDATE 2"));
  messages.push_back(one_column_description("last", 25, 0xffff));
  messages.push_back(one_column_row("done"));
  messages.push_back(command_complete("SELECT 1"));

  const auto result = parser.extract_query_result(messages);
  ASSERT_EQ(result.columns.size(), 1u);
  EXPECT_EQ(result.columns[0].name, "first");
  ASSERT_EQ(result.rows.size(), 1u);
  ASSERT_TRUE(result.rows[0][0].has_value());
  EXPECT_EQ(*result.rows[0][0], "1");
  ASSERT_EQ(result.additional_results.size(), 2u);

  const auto& update = result.additional_results[0];
  EXPECT_TRUE(update.columns.empty());
  EXPECT_TRUE(update.rows.empty());
  EXPECT_EQ(rs::core::database::StatementKind::UpdateWhere, update.statement_kind);
  EXPECT_EQ(update.affected_rows, 2u);

  const auto& last = result.additional_results[1];
  ASSERT_EQ(last.columns.size(), 1u);
  EXPECT_EQ(last.columns[0].name, "last");
  ASSERT_EQ(last.rows.size(), 1u);
  ASSERT_TRUE(last.rows[0][0].has_value());
  EXPECT_EQ(*last.rows[0][0], "done");
}

TEST(PgProtocolParserTest, DataRowMustMatchDescribedColumnCount) {
  PgProtocolParser parser;
  const auto description = one_column_description("value");
  const auto complete = command_complete("SELECT 1");
  EXPECT_EQ(parser.extract_query_result(
                {description, one_column_row("ok"), complete}).rows.size(),
            1u);

  rs::core::database::Message missing;
  missing.tag = 'D';
  append_u16(missing.payload, 0);
  EXPECT_THROW(parser.extract_query_result({description, missing, complete}),
               std::runtime_error);

  rs::core::database::Message extra;
  extra.tag = 'D';
  append_u16(extra.payload, 2);
  for (const char value : {'a', 'b'}) {
    append_u32(extra.payload, 1);
    extra.payload.push_back(static_cast<std::byte>(value));
  }
  EXPECT_THROW(parser.extract_query_result({description, extra, complete}),
               std::runtime_error);
}

TEST(PgProtocolParserTest, DataRowRequiresCurrentRowDescription) {
  PgProtocolParser parser;
  const auto description = one_column_description("value");
  const auto row = one_column_row("ok");
  const auto complete = command_complete("SELECT 1");

  EXPECT_THROW(parser.extract_query_result({row, complete}),
               std::runtime_error);
  EXPECT_THROW(parser.extract_query_result(
                   {description, row, complete, row, complete}),
               std::runtime_error);
}

TEST(PgProtocolParserTest, RejectsDuplicateRowDescriptionWithinResult) {
  PgProtocolParser parser;
  const auto first = one_column_description("first");
  const auto second = one_column_description("second");
  const auto row = one_column_row("ok");
  const auto complete = command_complete("SELECT 1");

  EXPECT_THROW(parser.extract_query_result({first, second, row, complete}),
               std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({first, row, second, complete}),
               std::runtime_error);
  EXPECT_THROW(parser.extract_query_result(
                   {first, row, {'2', {}}, second, complete}),
               std::runtime_error);
  EXPECT_EQ(parser.extract_query_result(
                {first, row, complete, second, row, complete})
                .additional_results.size(),
            1u);
}

TEST(PgProtocolParserTest, PortalDescriptionReplacesStatementDescription) {
  PgProtocolParser parser;
  const auto result = parser.extract_query_result(
      {{'1', {}}, {'t', {std::byte{0}, std::byte{0}}},
       one_column_description("statement"), {'2', {}},
       one_column_description("portal"), one_column_row("ok"),
       command_complete("SELECT 1")});
  ASSERT_EQ(result.columns.size(), 1u);
  EXPECT_EQ(result.columns[0].name, "portal");
  ASSERT_EQ(result.rows.size(), 1u);
  ASSERT_TRUE(result.rows[0][0].has_value());
  EXPECT_EQ(*result.rows[0][0], "ok");
}

TEST(PgProtocolParserTest, RowDescriptionRejectsUnknownFormatCodes) {
  PgProtocolParser parser;
  const auto text = parser.extract_query_result(
      {one_column_description("value", 23, 4, 0),
       command_complete("SELECT 0")});
  ASSERT_EQ(text.columns.size(), 1u);
  EXPECT_EQ(text.columns[0].format_code, 0);

  const auto binary = parser.extract_query_result(
      {one_column_description("value", 23, 4, 1),
       command_complete("SELECT 0")});
  ASSERT_EQ(binary.columns.size(), 1u);
  EXPECT_EQ(binary.columns[0].format_code, 1);

  EXPECT_THROW(parser.extract_query_result(
                   {one_column_description("value", 23, 4, 2),
                    command_complete("SELECT 0")}),
               std::runtime_error);
}

} // namespace

TEST(PgParameterContractTest, RawBinaryIsEncodedOnlyByPostgresAndNullStaysDistinct) {
  PgProtocolParser parser;
  const std::string raw("\0\xff\\x41", 6);
  const std::vector<QueryParameter> params{
      {raw, QueryParameterType::Binary},
      {"", QueryParameterType::Binary},
      {std::nullopt, QueryParameterType::Binary},
      {"\\x41", QueryParameterType::Text},
      {raw, QueryParameterType::Text, true}};
  const auto frames = split_frames(parser.create_prepared_query("SELECT ?, ?, ?, ?, ?", params));
  ASSERT_EQ(6u, frames.size());
  const auto& bind = frames[2].payload;
  std::size_t offset = 0;
  EXPECT_TRUE(read_cstring(bind, offset).empty());
  EXPECT_TRUE(read_cstring(bind, offset).empty());
  EXPECT_EQ(0, read_u16(bind, offset)); offset += 2;
  EXPECT_EQ(5, read_u16(bind, offset)); offset += 2;
  const std::vector<std::optional<std::string>> expected{
      "\\x00ff5c783431", "\\x", std::nullopt, "\\x41", "\\x00ff5c783431"};
  for (const auto& value : expected) {
    ASSERT_LE(offset + 4, bind.size());
    const auto length = read_u32(bind, offset); offset += 4;
    if (!value) { EXPECT_EQ(0xffffffffu, length); continue; }
    ASSERT_EQ(value->size(), length);
    ASSERT_LE(offset + length, bind.size());
    EXPECT_EQ(*value, std::string(reinterpret_cast<const char*>(bind.data() + offset), length));
    offset += length;
  }
  ASSERT_EQ(offset + 2, bind.size());
  EXPECT_EQ(0, read_u16(bind, offset));
  EXPECT_EQ(raw, *params[0].value);
}

TEST(PgCommandContractTest, NormalizesCompletionAndRejectsPrefixLookalikes) {
  using rs::core::database::StatementKind;
  PgProtocolParser parser;
  struct Case { const char* tag; StatementKind kind; };
  for (const auto& item : {
      Case{"SELECT 12", StatementKind::SelectCursor},
      Case{"INSERT 0 2", StatementKind::Insert},
      Case{"UPDATE 3", StatementKind::UpdateWhere},
      Case{"CREATE TABLE", StatementKind::CreateTable},
      Case{"CREATE INDEX", StatementKind::CreateIndex},
      Case{"DROP INDEX", StatementKind::DropIndex},
      Case{"SELECTED 1", StatementKind::Unknown},
      Case{"CREATE TABLESPACE", StatementKind::Unknown},
      Case{"SET", StatementKind::Unknown}}) {
    SCOPED_TRACE(item.tag);
    Message complete{'C', {}};
    append_cstring(complete.payload, item.tag);
    const auto result = parser.extract_query_result({complete});
    EXPECT_EQ(std::optional<StatementKind>(item.kind), result.statement_kind);
  }
  EXPECT_FALSE(parser.extract_query_result({}).statement_kind);
}

TEST(PgRequestBudgetTest, ExactWireSizesCoverHeadersMarkersNullAndBinaryExpansion) {
  using namespace rs::core::database;
  postgres::PgProtocolParser parser;
  const auto direct = parser.create_simple_query("SELECT 1");
  EXPECT_EQ(direct, parser.create_simple_query("SELECT 1", direct.size()));
  EXPECT_THROW(parser.create_simple_query("SELECT 1", direct.size() - 1), RequestWireLimitExceeded);
  const std::vector<QueryParameter> params{
      {std::nullopt, QueryParameterType::Text}, {std::string{}, QueryParameterType::Text},
      {std::string{"\0x", 2}, QueryParameterType::Binary}};
  const auto prepared = parser.create_prepared_query("SELECT ?, ?, ?", params);
  EXPECT_EQ(prepared, parser.create_prepared_query("SELECT ?, ?, ?", params, prepared.size()));
  EXPECT_THROW(parser.create_prepared_query("SELECT ?, ?, ?", params, prepared.size() - 1), RequestWireLimitExceeded);
  const std::vector<QueryParameterType> types{QueryParameterType::Text, QueryParameterType::Text, QueryParameterType::Binary};
  const auto description = parser.create_statement_description("SELECT ?, ?, ?", types);
  EXPECT_EQ(description, parser.create_statement_description("SELECT ?, ?, ?", types, description.size()));
  EXPECT_THROW(parser.create_statement_description("SELECT ?, ?, ?", types, description.size() - 1), RequestWireLimitExceeded);
  EXPECT_THROW(parser.create_simple_query("", 0), RequestWireLimitExceeded);
  EXPECT_THROW(parser.create_prepared_query("", {}, 0), RequestWireLimitExceeded);
  EXPECT_THROW(parser.create_statement_description("", {}, 0), RequestWireLimitExceeded);
}

TEST(PgRequestBudgetTest, MultiDigitMarkersAndBinaryInputHintsFitExactWireBudget) {
  using namespace rs::core::database;
  postgres::PgProtocolParser parser;
  std::vector<QueryParameter> params;
  std::string sql = "SELECT ";
  for (int i = 0; i < 10; ++i) {
    if (i != 0) sql += ',';
    sql += '?';
    params.push_back({std::string{"\0x", 2}, QueryParameterType::Text, true});
  }
  const auto wire = parser.create_prepared_query(sql, params);
  EXPECT_EQ(wire, parser.create_prepared_query(sql, params, wire.size()));
  EXPECT_THROW(parser.create_prepared_query(sql, params, wire.size() - 1), RequestWireLimitExceeded);
}

TEST(PgRequestBudgetTest, StartupAndAuthenticationPacketsFitExactWireLimits) {
  using namespace rs::core::database;
  postgres::PgProtocolParser parser;
  const std::map<std::string, std::string> options{{"application_name", "odbcpp"}};
  const auto startup = parser.create_startup_message("user", "db", options);
  EXPECT_EQ(startup, parser.create_startup_message("user", "db", options, startup.size()));
  EXPECT_THROW(parser.create_startup_message("user", "db", options, startup.size() - 1), RequestWireLimitExceeded);
  EXPECT_THROW(parser.create_startup_message("", "", {}, 0), RequestWireLimitExceeded);
  AuthenticationRequest request;
  request.type = AuthenticationRequest::Type::Cleartext;
  const auto cleartext = parser.create_auth_response(request, "secret", "user", true);
  EXPECT_EQ(cleartext, parser.create_auth_response(request, "secret", "user", true, cleartext.size()));
  EXPECT_THROW(parser.create_auth_response(request, "secret", "user", true, cleartext.size() - 1), RequestWireLimitExceeded);
  request.type = AuthenticationRequest::Type::MD5;
  request.challenge_data = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  const auto md5 = parser.create_auth_response(request, "secret", "user", false);
  ASSERT_EQ(md5.size(), 41u);
  EXPECT_EQ(md5, parser.create_auth_response(request, "secret", "user", false, 41));
  EXPECT_THROW(parser.create_auth_response(request, "secret", "user", false, 40), RequestWireLimitExceeded);
  request.type = AuthenticationRequest::Type::None;
  EXPECT_TRUE(parser.create_auth_response(request, "secret", "user", false, 0).empty());
}

TEST(PgRequestBudgetTest, ScramInitialResponseLimitIncludesEscapedUsername) {
  using namespace rs::core::database;
  AuthenticationRequest request;
  request.type = AuthenticationRequest::Type::SASL;
  constexpr char mechanisms[] = "SCRAM-SHA-256\0";
  for (const char ch : std::string_view(mechanisms, sizeof(mechanisms))) request.challenge_data.push_back(static_cast<std::byte>(ch));
  postgres::PgProtocolParser baseline;
  const auto wire = baseline.create_auth_response(request, "secret", "user,=", true);
  postgres::PgProtocolParser exact;
  EXPECT_EQ(exact.create_auth_response(request, "secret", "user,=", true, wire.size()).size(), wire.size());
  postgres::PgProtocolParser over;
  EXPECT_THROW(over.create_auth_response(request, "secret", "user,=", true, wire.size() - 1), RequestWireLimitExceeded);
}

TEST(PgRequestBudgetTest, ScramContinuationResponseFitsExactWireLimit) {
  using namespace rs::core::database;
  const auto continuation = [](std::size_t limit) {
    postgres::PgProtocolParser parser;
    AuthenticationRequest offer;
    offer.type = AuthenticationRequest::Type::SASL;
    constexpr char mechanisms[] = "SCRAM-SHA-256\0";
    for (const char ch : std::string_view(mechanisms, sizeof(mechanisms))) offer.challenge_data.push_back(static_cast<std::byte>(ch));
    const auto initial_wire = parser.create_auth_response(offer, "pencil", "user", true);
    const std::string initial(reinterpret_cast<const char*>(initial_wire.data() + 23), initial_wire.size() - 23);
    const auto nonce = initial.substr(initial.find(",r=") + 3);
    const auto server = "r=" + nonce + "server,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
    AuthenticationRequest challenge;
    challenge.type = AuthenticationRequest::Type::SASLContinue;
    for (const char ch : server) challenge.challenge_data.push_back(static_cast<std::byte>(ch));
    return parser.create_auth_response(challenge, "pencil", "user", true, limit);
  };
  const auto baseline = continuation(4096);
  EXPECT_EQ(continuation(baseline.size()).size(), baseline.size());
  EXPECT_THROW(continuation(baseline.size() - 1), RequestWireLimitExceeded);
}

TEST(PgProtocolParserTest, CompletionSnapshotsKeepKindsAndCountsWithoutNativeTags) {
  PgProtocolParser parser;
  const auto result = parser.extract_query_result({command_complete("UPDATE 7"),
      command_complete("DELETE 0"), command_complete("SET")});
  ASSERT_EQ(2u, result.additional_results.size());
  EXPECT_EQ(rs::core::database::StatementKind::UpdateWhere, result.statement_kind);
  EXPECT_EQ(7u, result.affected_rows);
  EXPECT_EQ(rs::core::database::StatementKind::DeleteWhere, result.additional_results[0].statement_kind);
  EXPECT_EQ(0u, result.additional_results[0].affected_rows);
  EXPECT_EQ(rs::core::database::StatementKind::Unknown, result.additional_results[1].statement_kind);
  EXPECT_EQ(0u, result.additional_results[1].affected_rows);
  EXPECT_EQ(rs::core::database::StatementKind::SelectCursor,
      parser.extract_query_result({command_complete("SELECT 1")}).statement_kind);
  EXPECT_EQ(7u, result.affected_rows); // later decoding cannot alter an owning completion
}

TEST(PgProtocolParserTest, DiscardedProvenanceAndCompletionBytesStillRequireValidFraming) {
  PgProtocolParser parser;
  const auto description = one_column_description("name");
  // Two-byte count plus five-byte name precede table OID and attribute number.
  for (std::size_t size = 7; size < 13; ++size) {
    auto truncated = description; truncated.payload.resize(size);
    EXPECT_THROW(parser.extract_query_result({truncated}), std::runtime_error);
  }
  auto unterminated = command_complete("UPDATE 7"); unterminated.payload.pop_back();
  EXPECT_THROW(parser.extract_query_result({unterminated}), std::runtime_error);
  auto trailing = command_complete("UPDATE 7"); trailing.payload.push_back(std::byte{'x'});
  EXPECT_THROW(parser.extract_query_result({trailing}), std::runtime_error);
  const auto recovered = parser.extract_query_result({description, command_complete("SELECT 0")});
  ASSERT_EQ(1u, recovered.columns.size()); EXPECT_EQ("name", recovered.columns[0].name);
  EXPECT_EQ(0u, recovered.affected_rows);
}

TEST(PgProtocolParserTest, AuthenticationCleanupDropsInterruptedScramAndAllowsFreshExchange) {
  PgProtocolParser parser;
  AuthenticationRequest offer;
  offer.type = AuthenticationRequest::Type::SASL;
  constexpr char mechanisms[] = "SCRAM-SHA-256\0";
  for (const char ch : std::string_view(mechanisms, sizeof(mechanisms))) {
    offer.challenge_data.push_back(static_cast<std::byte>(ch));
  }
  ASSERT_FALSE(parser.create_auth_response(offer, "secret", "user", true).empty());
  const std::vector<std::byte> auth_ok(4, std::byte{0});
  EXPECT_THROW(parser.parse_auth_request(auth_ok), std::runtime_error);
  parser.clear_authentication_state();
  parser.clear_authentication_state();
  for (auto type : {AuthenticationRequest::Type::SASLContinue,
                    AuthenticationRequest::Type::SASLFinal}) {
    AuthenticationRequest stale;
    stale.type = type;
    EXPECT_THROW(parser.create_auth_response(stale, "secret", "user", true), std::runtime_error);
  }
  EXPECT_EQ(parser.parse_auth_request(auth_ok).type, AuthenticationRequest::Type::None);
  ASSERT_FALSE(parser.create_auth_response(offer, "new-secret", "user", true).empty());
  EXPECT_THROW(parser.parse_auth_request(auth_ok), std::runtime_error);
  parser.clear_authentication_state();
}

TEST(PgParameterContractTest, PreparedHexTextHasLiteralOwningOid25AndEmptyNullFrames) {
  PgProtocolParser parser;
  const unsigned char parse_bytes[]{
      0,'S','E','L','E','C','T',' ','F','R','O','M','_','H','E','X','(',
      '$','1',')',' ','A','S',' ','b',0,0,1,0,0,0,25};
  const unsigned char nonempty_bytes[]{0,0,0,0,0,1,0,0,0,6,'0','0','4','1','f','f',0,0};
  const unsigned char empty_bytes[]{0,0,0,0,0,1,0,0,0,0,0,0};
  const unsigned char null_bytes[]{0,0,0,0,0,1,255,255,255,255,0,0};
  const auto literal=[](std::span<const unsigned char> input) {
    std::vector<std::byte> output;
    for(auto byte:input) { output.push_back(static_cast<std::byte>(byte)); }
    return output;
  };
  const std::vector<std::vector<std::byte>> expected_bind{
      literal(nonempty_bytes),literal(empty_bytes),literal(null_bytes)};
  for(unsigned trial=0;trial<3;++trial) {
    SCOPED_TRACE(trial);
    std::string caller="0041ff";
    std::vector<QueryParameter> parameters{
        {trial==2?std::nullopt:std::optional<std::string>{trial==1?"":caller},QueryParameterType::Text}};
    caller.assign("poison"); // Parameter storage owns input before serialization.
    const auto frames=split_frames(parser.create_prepared_query("SELECT FROM_HEX(?) AS b",parameters));
    ASSERT_EQ(6U,frames.size());
    EXPECT_EQ('P',frames[0].tag);EXPECT_EQ(literal(parse_bytes),frames[0].payload);
    EXPECT_EQ('B',frames[2].tag);EXPECT_EQ(expected_bind[trial],frames[2].payload);
    // Mutating the parameter after encoding cannot alter the owning wire bytes.
    parameters[0].value="poison";
    EXPECT_EQ(expected_bind[trial],frames[2].payload);
  }
}


TEST(PgParameterContractTest, PreparedNumericHasLiteralOwningOid1700AndNullFrames) {
  PgProtocolParser parser;
  const unsigned char parse_bytes[]{
      0,'S','E','L','E','C','T',' ','C','A','S','T','(', '$','1',' ','A','S',' ',
      'D','E','C','I','M','A','L','(','5',',','2',')',')',' ','A','S',' ',
      'a','m','o','u','n','t',0,0,1,0,0,6,164}; // 1700 big endian, literal.
  const unsigned char positive_bytes[]{0,0,0,0,0,1,0,0,0,6,'1','2','3','.','4','5',0,0};
  const unsigned char negative_bytes[]{0,0,0,0,0,1,0,0,0,7,'-','1','2','3','.','4','5',0,0};
  const unsigned char null_bytes[]{0,0,0,0,0,1,255,255,255,255,0,0};
  const auto literal=[](std::span<const unsigned char> input) {
    std::vector<std::byte> output;
    for(auto byte:input) { output.push_back(static_cast<std::byte>(byte)); }
    return output;
  };
  const std::vector<std::vector<std::byte>> expected_bind{
      literal(positive_bytes),literal(negative_bytes),literal(null_bytes)};
  for(unsigned trial=0;trial<3;++trial) {
    SCOPED_TRACE(trial);
    std::string caller=trial==1?"-123.45":"123.45";
    std::vector<QueryParameter> parameters{
        {trial==2?std::nullopt:std::optional<std::string>{caller},QueryParameterType::Numeric}};
    caller.assign("poison"); // QueryParameter owns the original text.
    const auto wire=parser.create_prepared_query(
        "SELECT CAST(? AS DECIMAL(5,2)) AS amount",parameters);
    const auto frames=split_frames(wire);
    ASSERT_EQ(6U,frames.size());
    const char tags[]{'P','D','B','D','E','S'};
    for(std::size_t index=0;index<6;++index) { EXPECT_EQ(tags[index],frames[index].tag); }
    EXPECT_EQ(literal(parse_bytes),frames[0].payload);
    EXPECT_EQ(expected_bind[trial],frames[2].payload);
    parameters[0].value="poison";
    const auto retained=split_frames(wire);
    ASSERT_EQ(6U,retained.size());
    EXPECT_EQ(expected_bind[trial],retained[2].payload);
    EXPECT_EQ(literal(parse_bytes),retained[0].payload);
  }
}


TEST(PgParameterContractTest, PreparedTemporalTripletHasLiteralOwningOidsAndNullFrames) {
  PgProtocolParser parser;
  const unsigned char parse_bytes[]{
      0,'S','E','L','E','C','T',' ','C','A','S','T','(','$','1',' ','A','S',' ','D','A','T','E',')',
      ' ','A','S',' ','c','a','l','e','n','d','a','r','_','d','a','y',',',' ',
      'C','A','S','T','(','$','2',' ','A','S',' ','T','I','M','E',')',' ','A','S',' ',
      'c','l','o','c','k','_','t','i','m','e',',',' ',
      'C','A','S','T','(','$','3',' ','A','S',' ','T','I','M','E','S','T','A','M','P',')',
      ' ','A','S',' ','s','t','a','m','p',0,0,3,0,0,4,58,0,0,4,59,0,0,4,90};
  const unsigned char value_bytes[]{0,0,0,0,0,3,
      0,0,0,10,'2','0','2','4','-','0','2','-','2','9',
      0,0,0,8,'1','2',':','3','4',':','5','6',
      0,0,0,26,'2','0','2','4','-','0','2','-','2','9',' ','1','2',':','3','4',':','5','6','.','1','2','3','4','5','6',0,0};
  const unsigned char null_bytes[]{0,0,0,0,0,3,
      255,255,255,255,255,255,255,255,255,255,255,255,0,0};
  const auto literal=[](std::span<const unsigned char> input) {
    std::vector<std::byte> output;
    for(auto byte:input) { output.push_back(static_cast<std::byte>(byte)); }
    return output;
  };
  const char* query="SELECT CAST(? AS DATE) AS calendar_day, CAST(? AS TIME) AS clock_time, CAST(? AS TIMESTAMP) AS stamp";
  for(const bool null:{false,true}) {
    SCOPED_TRACE(null);
    std::string date="2024-02-29",time="12:34:56",stamp="2024-02-29 12:34:56.123456";
    std::vector<QueryParameter> parameters{
        {null?std::nullopt:std::optional<std::string>{date},QueryParameterType::Date},
        {null?std::nullopt:std::optional<std::string>{time},QueryParameterType::Time},
        {null?std::nullopt:std::optional<std::string>{stamp},QueryParameterType::Timestamp}};
    date="poison";time="poison";stamp="poison";
    const auto wire=parser.create_prepared_query(query,parameters);
    const auto frames=split_frames(wire);ASSERT_EQ(6U,frames.size());
    const char tags[]{'P','D','B','D','E','S'};
    for(std::size_t index=0;index<6;++index) { EXPECT_EQ(tags[index],frames[index].tag); }
    EXPECT_EQ(literal(parse_bytes),frames[0].payload);
    const auto expected_bind=null?literal(null_bytes):literal(value_bytes);
    EXPECT_EQ(expected_bind,frames[2].payload);
    for(auto& parameter:parameters) { parameter.value="poison"; }
    const auto retained=split_frames(wire);ASSERT_EQ(6U,retained.size());
    EXPECT_EQ(literal(parse_bytes),retained[0].payload);EXPECT_EQ(expected_bind,retained[2].payload);
  }
}


namespace {
TEST(PgExecutionShapeTest, PortalZeroFieldsNoDataAndStatementDescriptionNeverInterchange) {
  using rs::core::database::ExecutionResultShape;
  PgProtocolParser parser;
  const Message zero{'T', {std::byte{0}, std::byte{0}}};
  const auto result = parser.extract_query_result({zero, {'2',{}}, zero, command_complete("SELECT 0")});
  EXPECT_EQ(ExecutionResultShape::ResultSet, result.execution_result_shape); EXPECT_TRUE(result.columns.empty());
  const auto command = parser.extract_query_result({zero, {'2',{}}, {'n',{}}, command_complete("UPDATE 0")});
  EXPECT_EQ(ExecutionResultShape::NoResultSet, command.execution_result_shape); EXPECT_TRUE(command.columns.empty());
  const auto missing = parser.extract_query_result({zero, {'2',{}}, command_complete("SELECT 0")});
  EXPECT_FALSE(missing.execution_result_shape);
  EXPECT_FALSE(parser.extract_query_result({zero}).execution_result_shape);
  EXPECT_FALSE(parser.extract_query_result({{'n',{}}}).execution_result_shape);
}
TEST(PgExecutionShapeTest, MalformedNoDataErrorAndCompoundCannotBorrowPriorAuthority) {
  using rs::core::database::ExecutionResultShape;
  PgProtocolParser parser;
  EXPECT_THROW(parser.extract_query_result({{'2',{}},{'n',{std::byte{0}}},command_complete("UPDATE 0")}),std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'2',{}},{'n',{}},{'n',{}},command_complete("UPDATE 0")}),std::runtime_error);
  const Message zero{'T', {std::byte{0}, std::byte{0}}};
  const auto compound=parser.extract_query_result({zero,command_complete("SELECT 0"),command_complete("UPDATE 1")});
  EXPECT_EQ(ExecutionResultShape::ResultSet,compound.execution_result_shape);
  ASSERT_EQ(1u,compound.additional_results.size());
  EXPECT_EQ(ExecutionResultShape::NoResultSet,compound.additional_results[0].execution_result_shape);
}
}


namespace {
TEST(PgExecutionShapeTest, ParseAndStatementDescriptionCannotReplaceBindAndPortalAuthority) {
  PgProtocolParser parser;
  const Message zero{'T', {std::byte{0}, std::byte{0}}};
  const Message parameters{'t', {std::byte{0}, std::byte{0}}};
  const auto missing_bind = parser.extract_query_result({{'1',{}},parameters,zero,command_complete("SELECT 0")});
  EXPECT_FALSE(missing_bind.execution_result_shape);
  EXPECT_FALSE(missing_bind.prepared_execution_authority);
  const auto missing_portal = parser.extract_query_result({{'1',{}},parameters,zero,{'2',{}},command_complete("SELECT 0")});
  EXPECT_FALSE(missing_portal.execution_result_shape);
  EXPECT_FALSE(missing_portal.prepared_execution_authority);
  const auto complete = parser.extract_query_result({{'1',{}},parameters,zero,{'2',{}},zero,command_complete("SELECT 0")});
  EXPECT_EQ(rs::core::database::ExecutionResultShape::ResultSet, complete.execution_result_shape);
  EXPECT_TRUE(complete.prepared_execution_authority);
  const auto cached = parser.extract_query_result({{'2',{}},{'n',{}},command_complete("UPDATE 0")});
  EXPECT_EQ(rs::core::database::ExecutionResultShape::NoResultSet, cached.execution_result_shape);
  EXPECT_TRUE(cached.prepared_execution_authority);
  const auto simple = parser.extract_query_result({zero,command_complete("SELECT 0")});
  EXPECT_EQ(rs::core::database::ExecutionResultShape::ResultSet, simple.execution_result_shape);
  EXPECT_FALSE(simple.prepared_execution_authority);
  const auto command = parser.extract_query_result({command_complete("UPDATE 0")});
  EXPECT_EQ(rs::core::database::ExecutionResultShape::NoResultSet, command.execution_result_shape);
  EXPECT_FALSE(command.prepared_execution_authority);
}
}

namespace {
TEST(PgExecutionShapeTest, NewStatementFramesCannotReuseEarlierBoundPortalAuthority) {
  PgProtocolParser parser;
  const Message zero{'T', {std::byte{0}, std::byte{0}}};
  const Message parameters{'t', {std::byte{0}, std::byte{0}}};
  EXPECT_THROW(parser.extract_query_result({{'1',{}},parameters,zero,{'2',{}},{'n',{}},
      {'1',{}},parameters,zero,command_complete("SELECT 0")}), std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'1',{}},parameters,zero,{'2',{}},{'n',{}},
      parameters,zero,command_complete("SELECT 0")}), std::runtime_error);
  const auto valid = parser.extract_query_result({{'1',{}},parameters,zero,{'2',{}},zero,command_complete("SELECT 0")});
  EXPECT_TRUE(valid.prepared_execution_authority);
  EXPECT_EQ(rs::core::database::ExecutionResultShape::ResultSet, valid.execution_result_shape);
  const auto description = parser.extract_query_result({{'1',{}},parameters,zero});
  EXPECT_FALSE(description.prepared_execution_authority); EXPECT_FALSE(description.execution_result_shape);
  const auto simple = parser.extract_query_result({zero,command_complete("SELECT 0")});
  EXPECT_FALSE(simple.prepared_execution_authority);
  EXPECT_EQ(rs::core::database::ExecutionResultShape::ResultSet, simple.execution_result_shape);
}
}

namespace {
TEST(PgEmptyPreparedAuthorityTest, FullAndCachedEmptyPortalCompleteWithOwningNoResult) {
  PgProtocolParser parser;
  const Message parameters{'t', {std::byte{0}, std::byte{0}}};
  for (bool cached : {false, true}) {
    SCOPED_TRACE(cached);
    std::vector<Message> messages;
    if (!cached) { messages.push_back({'1',{}}); messages.push_back(parameters); }
    messages.push_back({'2',{}}); messages.push_back({'n',{}});
    messages.push_back({'I',{}}); messages.push_back({'Z',{std::byte{'I'}}});
    const auto result=parser.extract_query_result(messages);
    EXPECT_TRUE(result.prepared_execution_authority);
    EXPECT_EQ(rs::core::database::ExecutionResultShape::NoResultSet,result.execution_result_shape);
    EXPECT_TRUE(result.columns.empty()); EXPECT_TRUE(result.rows.empty());
    EXPECT_TRUE(result.parameter_type_ids.empty()); EXPECT_TRUE(result.additional_results.empty());
    EXPECT_EQ(0u,result.affected_rows); EXPECT_FALSE(result.error); EXPECT_FALSE(result.statement_kind);
  }
}
TEST(PgEmptyPreparedAuthorityTest, MissingStatementOnlyAndZeroColumnPortalCannotAuthorizeEmpty) {
  PgProtocolParser parser;
  const Message parameters{'t', {std::byte{0}, std::byte{0}}};
  const Message zero{'T', {std::byte{0},std::byte{0}}};
  EXPECT_THROW(parser.extract_query_result({{'1',{}},parameters,{'n',{}},{'I',{}}}),std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'1',{}},parameters,{'2',{}},{'I',{}}}),std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'1',{}},parameters,{'n',{}},{'2',{}},{'I',{}}}),std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'2',{}},zero,{'I',{}}}),std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'2',{}},{'n',{}},{'I',{std::byte{0}}}}),std::runtime_error);
}
TEST(PgEmptyPreparedAuthorityTest, CompletionAndNewDescriptionCannotReusePriorEmptyAuthority) {
  PgProtocolParser parser;
  const Message parameters{'t', {std::byte{0},std::byte{0}}};
  EXPECT_THROW(parser.extract_query_result({{'2',{}},{'n',{}},{'I',{}},{'I',{}}}),std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'2',{}},{'n',{}},command_complete("UPDATE 0"),{'I',{}}}),std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'2',{}},{'n',{}},{'1',{}},parameters,{'I',{}}}),std::runtime_error);
  EXPECT_THROW(parser.extract_query_result({{'2',{}},{'n',{}},parameters,{'I',{}}}),std::runtime_error);
  const auto repeat=parser.extract_query_result({{'2',{}},{'n',{}},{'I',{}},{'2',{}},{'n',{}},{'I',{}}});
  ASSERT_EQ(1u,repeat.additional_results.size());
  EXPECT_TRUE(repeat.prepared_execution_authority);
  EXPECT_TRUE(repeat.additional_results[0].prepared_execution_authority);
  EXPECT_EQ(rs::core::database::ExecutionResultShape::NoResultSet,repeat.additional_results[0].execution_result_shape);
}
TEST(PgEmptyPreparedAuthorityTest, SimpleEmptyAndOrdinaryCommandSemanticsRemainSeparate) {
  PgProtocolParser parser;
  const auto simple=parser.extract_query_result({{'I',{}},{'Z',{std::byte{'I'}}}});
  EXPECT_FALSE(simple.prepared_execution_authority); EXPECT_FALSE(simple.execution_result_shape);
  EXPECT_TRUE(simple.rows.empty()); EXPECT_TRUE(simple.columns.empty()); EXPECT_EQ(0u,simple.affected_rows);
  const auto command=parser.extract_query_result({{'2',{}},{'n',{}},command_complete("UPDATE 1")});
  EXPECT_TRUE(command.prepared_execution_authority); EXPECT_EQ(1u,command.affected_rows);
  EXPECT_EQ(rs::core::database::ExecutionResultShape::NoResultSet,command.execution_result_shape);
}
}
