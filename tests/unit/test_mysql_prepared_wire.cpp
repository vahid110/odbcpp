#include <gtest/gtest.h>

#include "core/database/mysql/prepared_wire.h"

namespace {
using namespace rs::core::database;
using namespace rs::core::database::mysql;
using namespace rs::core::database::mysql::prepared_detail;
using rs::util::DbErrorCode;
using Bytes = std::vector<std::byte>;

void little(Bytes& bytes, std::uint64_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i)
    bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
}

void length_bytes(Bytes& bytes, std::string_view value) {
  if (value.size() < 251) bytes.push_back(static_cast<std::byte>(value.size()));
  else { bytes.push_back(std::byte{252}); little(bytes, value.size(), 2); }
  for (const auto ch : value)
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
}

ResultColumnMetadata metadata(ScalarType type) {
  return {"value", NativeTypeInfo{type, 20, 0, true}};
}

TEST(MySqlPreparedWireTest, ParsesExactPrepareOkAndRejectsWarningsOrMalformedShapes) {
  Bytes packet{std::byte{0}};
  little(packet, 0x78563412, 4); little(packet, 3, 2); little(packet, 2, 2);
  packet.push_back(std::byte{0}); little(packet, 0, 2);
  auto parsed = parse_prepare(packet); ASSERT_TRUE(parsed);
  EXPECT_EQ(0x78563412u, parsed->statement_id);
  EXPECT_EQ(3u, parsed->columns); EXPECT_EQ(2u, parsed->parameters);
  for (std::size_t size = 0; size < packet.size(); ++size)
    EXPECT_FALSE(parse_prepare(std::span(packet).first(size))) << size;
  auto extra = packet; extra.push_back(std::byte{0}); EXPECT_FALSE(parse_prepare(extra));
  auto tag = packet; tag[0] = std::byte{1}; EXPECT_FALSE(parse_prepare(tag));
  auto filler = packet; filler[9] = std::byte{1}; EXPECT_FALSE(parse_prepare(filler));
  auto warning = packet; warning[10] = std::byte{1};
  auto rejected = parse_prepare(warning); ASSERT_FALSE(rejected);
  EXPECT_EQ(DbErrorCode::UnsupportedFeature, rejected.error());
}

TEST(MySqlPreparedWireTest, ExecuteRequestFramesTypedValuesWithoutSqlInterpolation) {
  const std::array<QueryParameter, 8> parameters{{
      {"-32768", QueryParameterType::Int16},
      {"2147483647", QueryParameterType::Int32},
      {"-9223372036854775808", QueryParameterType::Int64},
      {"1", QueryParameterType::Boolean},
      {std::string{"a\0b", 3}, QueryParameterType::Text},
      {std::string{"\0\xff", 2}, QueryParameterType::Binary, true},
      {std::nullopt, QueryParameterType::Int32},
      {std::nullopt, QueryParameterType::Unspecified},
  }};
  auto request = execute_request(0x78563412, parameters, InputLimits{}); ASSERT_TRUE(request);
  ASSERT_GE(request->size(), 14u + 1 + 1 + parameters.size() * 2);
  EXPECT_EQ(std::byte{0}, (*request)[3]); EXPECT_EQ(std::byte{23}, (*request)[4]);
  EXPECT_EQ(std::byte{0x12}, (*request)[5]); EXPECT_EQ(std::byte{0x78}, (*request)[8]);
  EXPECT_EQ(std::byte{0}, (*request)[9]); EXPECT_EQ(std::byte{1}, (*request)[10]);
  EXPECT_EQ(std::byte{0xc0}, (*request)[14]); // Parameters 6 and 7 are NULL.
  EXPECT_EQ(std::byte{1}, (*request)[15]);   // New parameter types follow.
  const std::array<unsigned, 8> types{2, 3, 8, 1, 253, 252, 3, 253};
  for (std::size_t i = 0; i < types.size(); ++i) {
    EXPECT_EQ(static_cast<std::byte>(types[i]), (*request)[16 + i * 2]);
    EXPECT_EQ(std::byte{0}, (*request)[17 + i * 2]);
  }
  const auto payload = std::to_integer<std::size_t>((*request)[0]) |
      (std::to_integer<std::size_t>((*request)[1]) << 8) |
      (std::to_integer<std::size_t>((*request)[2]) << 16);
  EXPECT_EQ(request->size() - 4, payload);
  EXPECT_EQ(std::byte{0xff}, request->back());
}

TEST(MySqlPreparedWireTest, ExecuteRequestValidatesTypesValuesAndAllBudgetsBeforeAllocation) {
  const std::array<QueryParameterType, 6> unsupported{QueryParameterType::Float32,
      QueryParameterType::Float64, QueryParameterType::Numeric, QueryParameterType::Date,
      QueryParameterType::Time, QueryParameterType::Timestamp};
  for (const auto type : unsupported) {
    for (const auto& value : {std::optional<std::string>{"1"}, std::optional<std::string>{}}) {
      const std::array<QueryParameter, 1> parameters{{{value, type}}};
      auto result = execute_request(1, parameters, InputLimits{}); ASSERT_FALSE(result);
      EXPECT_EQ(DbErrorCode::UnsupportedFeature, result.error());
    }
  }
  const std::array<QueryParameter, 6> invalid{{
      {"32768", QueryParameterType::Int16}, {"-2147483649", QueryParameterType::Int32},
      {"9223372036854775808", QueryParameterType::Int64}, {"true", QueryParameterType::Boolean},
      {std::string(1, static_cast<char>(255)), QueryParameterType::Text},
      {"raw", QueryParameterType::Text, true},
  }};
  for (const auto& parameter : invalid) {
    auto result = execute_request(1, std::span(&parameter, 1), InputLimits{}); ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::InvalidParameter, result.error());
  }

  const std::array<QueryParameter, 2> values{{{"abc", QueryParameterType::Text},
      {"de", QueryParameterType::Binary, true}}};
  for (unsigned mode = 0; mode < 5; ++mode) {
    auto limits = InputLimits{};
    if (mode == 0) limits.max_parameters = 1;
    if (mode == 1) limits.max_parameter_bytes = 2;
    if (mode == 2) limits.max_parameter_total_bytes = 4;
    if (mode == 3) limits.max_request_wire_bytes = 10;
    std::vector<QueryParameter> oversized(values.begin(), values.end());
    if (mode == 4) oversized[0].value = std::string(connection_packet_limit, 'x');
    auto result = execute_request(1, oversized, limits); ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::ResourceLimit, result.error());
  }
  auto empty = execute_request(0xffffffff, {}, InputLimits{}); ASSERT_TRUE(empty);
  EXPECT_EQ(14u, empty->size()); EXPECT_EQ(std::byte{23}, (*empty)[4]);
}

TEST(MySqlPreparedWireTest, BinaryRowsOwnSignedUnsignedNullTextAndBinaryValues) {
  const std::array<NativeColumn, 8> native{{{1, false}, {2, true}, {3, false}, {9, true},
      {8, false}, {8, true}, {253, false}, {252, false}}};
  const std::array<ResultColumnMetadata, 8> columns{{metadata(ScalarType::SmallInt),
      metadata(ScalarType::Integer), metadata(ScalarType::Integer), metadata(ScalarType::Integer),
      metadata(ScalarType::BigInt), metadata(ScalarType::Numeric), metadata(ScalarType::VarChar),
      metadata(ScalarType::Binary)}};
  Bytes row{std::byte{0}, std::byte{0}, std::byte{0}};
  little(row, 0x80, 1); little(row, 0xffff, 2); little(row, 0x80000000, 4);
  little(row, 0xffffff, 4); little(row, 0x8000000000000000ULL, 8);
  little(row, 0xffffffffffffffffULL, 8); length_bytes(row, std::string_view{"a\0b", 3});
  length_bytes(row, std::string_view{"\0\xff", 2});
  std::vector<CellEncodingError> errors;
  auto decoded = binary_row(row, native, columns, 4, errors); ASSERT_TRUE(decoded);
  const std::array<std::string, 8> expected{"-128", "65535", "-2147483648", "16777215",
      "-9223372036854775808", "18446744073709551615", std::string{"a\0b", 3},
      std::string{"\0\xff", 2}};
  for (std::size_t i = 0; i < expected.size(); ++i) ASSERT_EQ(expected[i], *(*decoded)[i]);
  EXPECT_TRUE(errors.empty());

  const std::array<NativeColumn, 7> nullable{{{6, false}, {1, false}, {1, false}, {1, false},
      {1, false}, {1, false}, {1, false}}};
  std::array<ResultColumnMetadata, 7> nullable_columns{};
  for (auto& column : nullable_columns) column = metadata(ScalarType::SmallInt);
  Bytes null_row{std::byte{0}, std::byte{0x04}, std::byte{0x01}};
  for (unsigned i = 0; i < 5; ++i) null_row.push_back(static_cast<std::byte>(i));
  decoded = binary_row(null_row, nullable, nullable_columns, 0, errors); ASSERT_TRUE(decoded);
  EXPECT_FALSE((*decoded)[0]); EXPECT_FALSE((*decoded)[6]);
  auto unused_bit = null_row; unused_bit[2] |= std::byte{0x80};
  EXPECT_FALSE(binary_row(unused_bit, nullable, nullable_columns, 0, errors));
}

TEST(MySqlPreparedWireTest, BinaryRowsDeferBadTextAndRejectEveryMalformedBoundary) {
  const std::array<NativeColumn, 2> native{{{253, false}, {2, false}}};
  const std::array<ResultColumnMetadata, 2> columns{{metadata(ScalarType::VarChar),
      metadata(ScalarType::SmallInt)}};
  Bytes row{std::byte{0}, std::byte{0}};
  length_bytes(row, std::string(1, static_cast<char>(255))); little(row, 7, 2);
  std::vector<CellEncodingError> errors;
  auto decoded = binary_row(row, native, columns, 9, errors); ASSERT_TRUE(decoded);
  ASSERT_EQ(1u, errors.size()); EXPECT_EQ((CellEncodingError{9, 0}), errors[0]);
  EXPECT_TRUE((*decoded)[0]->empty()); EXPECT_EQ("7", *(*decoded)[1]);
  for (std::size_t size = 0; size < row.size(); ++size)
    EXPECT_FALSE(binary_row(std::span(row).first(size), native, columns, 0, errors)) << size;
  auto trailing = row; trailing.push_back(std::byte{0}); EXPECT_FALSE(binary_row(trailing, native, columns, 0, errors));
  auto header = row; header[0] = std::byte{1}; EXPECT_FALSE(binary_row(header, native, columns, 0, errors));
  auto reserved = row; reserved[1] = std::byte{1}; EXPECT_FALSE(binary_row(reserved, native, columns, 0, errors));
  const std::array<NativeColumn, 2> unsupported{{{4, false}, {2, false}}};
  auto rejected = binary_row(row, unsupported, columns, 0, errors); ASSERT_FALSE(rejected);
  EXPECT_EQ(DbErrorCode::UnsupportedFeature, rejected.error());
  auto unsupported_null = row; unsupported_null[1] = std::byte{0x04};
  EXPECT_FALSE(binary_row(unsupported_null, unsupported, columns, 0, errors));
  EXPECT_FALSE(binary_row(row, std::span(native).first(1), columns, 0, errors));
  auto missing_type = columns; missing_type[0].normalized_type.reset();
  EXPECT_FALSE(binary_row(row, native, missing_type, 0, errors));
  auto missing_null = row; missing_null[1] = std::byte{0x04};
  EXPECT_FALSE(binary_row(missing_null, native, missing_type, 0, errors));
}

}  // namespace

TEST(MySqlPreparedWireAdditionalTest, TextBlobUtf8AndIntegerRangeFollowNormalizedCellContract) {
  const std::array<NativeColumn,2> native{{{252,false},{9,true}}};
  const std::array<ResultColumnMetadata,2> columns{{metadata(ScalarType::VarChar),metadata(ScalarType::Integer)}};
  Bytes row{std::byte{0},std::byte{0}};length_bytes(row,std::string(1,static_cast<char>(255)));little(row,0xffffffff,4);
  std::vector<CellEncodingError> errors;auto result=binary_row(row,native,columns,3,errors);ASSERT_TRUE(result);
  EXPECT_TRUE((*result)[0]->empty());EXPECT_TRUE((*result)[1]->empty());
  EXPECT_EQ((std::vector<CellEncodingError>{{3,0},{3,1}}),errors);
}
