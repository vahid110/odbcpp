#include <gtest/gtest.h>
#include "core/database/mysql/query_wire.h"
#include "core/database/mysql/error_wire.h"
namespace {
using namespace rs::core::database;
using namespace rs::core::database::mysql;
using rs::util::DbErrorCode;
using Bytes=std::vector<std::byte>;
void number(Bytes& bytes,std::uint64_t n,std::size_t width) {
  for (std::size_t i=0;i<width;++i) bytes.push_back(static_cast<std::byte>((n>>(8*i))&255));
}
void text(Bytes& bytes,std::string_view s) {
  bytes.push_back(static_cast<std::byte>(s.size()));
  for (const auto ch:s) bytes.push_back(static_cast<std::byte>(ch));
}
Bytes column(unsigned type=8,unsigned charset=63,unsigned flags=0,std::string_view name="col") {
  Bytes b;for (const auto field:{std::string_view("def"),std::string_view{},std::string_view{},std::string_view{},name,std::string_view{}}) text(b,field);
  number(b,12,1);number(b,charset,2);number(b,40,4);number(b,type,1);number(b,flags,2);number(b,0,1);number(b,0,2);return b;
}
TEST(MySqlQueryWireTest, MetadataTruncationTrailingBytesAndUnknownTypesFailClosed) {
  const auto bytes=column();
  for (std::size_t n=0;n<bytes.size();++n) EXPECT_FALSE(query_detail::column(std::span(bytes).first(n),ResultLimits{}))<<n;
  auto extra=bytes;extra.push_back(std::byte{0});EXPECT_FALSE(query_detail::column(extra,ResultLimits{}));
  auto unknown=query_detail::column(column(7),ResultLimits{});ASSERT_FALSE(unknown);EXPECT_EQ(DbErrorCode::UnsupportedFeature,unknown.error());
  auto charset=query_detail::column(column(253,8),ResultLimits{});ASSERT_FALSE(charset);EXPECT_EQ(DbErrorCode::UnsupportedFeature,charset.error());
  EXPECT_FALSE(query_detail::column(column(8,63,0,std::string_view("a\0b",3)),ResultLimits{}));
  EXPECT_FALSE(query_detail::column(column(8,63,0,std::string(1,static_cast<char>(255))),ResultLimits{}));
}
TEST(MySqlQueryWireTest, MetadataMapsUnsignedWithoutLosingRangeAndBinaryWithoutTextConversion) {
  auto signed_col=query_detail::column(column(),ResultLimits{});ASSERT_TRUE(signed_col);
  EXPECT_EQ(ScalarType::BigInt,signed_col->normalized_type->type);EXPECT_EQ(19u,signed_col->normalized_type->column_size);
  auto unsigned_col=query_detail::column(column(8,63,32),ResultLimits{});ASSERT_TRUE(unsigned_col);
  EXPECT_EQ(ScalarType::Numeric,unsigned_col->normalized_type->type);EXPECT_EQ(20u,unsigned_col->normalized_type->column_size);
  auto binary=query_detail::column(column(253),ResultLimits{});ASSERT_TRUE(binary);EXPECT_EQ(ScalarType::Binary,binary->normalized_type->type);
  auto utf8=query_detail::column(column(253,45),ResultLimits{});ASSERT_TRUE(utf8);EXPECT_EQ(10u,utf8->normalized_type->column_size);
  auto limits=ResultLimits{};limits.max_column_name_bytes=2;EXPECT_FALSE(query_detail::column(column(),limits));
  limits=ResultLimits{};limits.max_metadata_name_bytes=5;EXPECT_FALSE(query_detail::column(column(),limits));
}
TEST(MySqlQueryWireTest, RowsPreserveNullEmptyBinaryAndDeferInvalidUtf8AndNumericRange) {
  const std::vector<ResultColumnMetadata> cols{{"n",NativeTypeInfo{ScalarType::BigInt,19,0,true}},
      {"b",NativeTypeInfo{ScalarType::Binary,2,0,true}}, {"t",NativeTypeInfo{ScalarType::VarChar,2,0,true}}};
  Bytes bytes;text(bytes,"9223372036854775808");text(bytes,std::string_view("\0\xff",2));text(bytes,std::string(1,static_cast<char>(255)));
  std::vector<CellEncodingError> errors;auto row=query_detail::row(bytes,cols,7,errors);ASSERT_TRUE(row);
  EXPECT_TRUE((*row)[0]->empty());EXPECT_EQ(std::string("\0\xff",2),*(*row)[1]);EXPECT_TRUE((*row)[2]->empty());
  ASSERT_EQ(2u,errors.size());EXPECT_EQ((CellEncodingError{7,0}),errors[0]);EXPECT_EQ((CellEncodingError{7,2}),errors[1]);
  for (std::size_t n=0;n<bytes.size();++n) { errors.clear();EXPECT_FALSE(query_detail::row(std::span(bytes).first(n),cols,0,errors)); }
  bytes={std::byte{251},std::byte{0},std::byte{0}};errors.clear();row=query_detail::row(bytes,cols,0,errors);ASSERT_TRUE(row);
  EXPECT_FALSE((*row)[0]);EXPECT_TRUE((*row)[1]->empty());EXPECT_TRUE((*row)[2]->empty());
  EXPECT_TRUE(query_detail::valid_cell("18446744073709551615",ScalarType::Numeric));
  EXPECT_FALSE(query_detail::valid_cell("18446744073709551616",ScalarType::Numeric));
  EXPECT_FALSE(query_detail::valid_cell("-1",ScalarType::Numeric));
  EXPECT_FALSE(query_detail::valid_cell("32768",ScalarType::SmallInt));
  EXPECT_FALSE(query_detail::valid_cell("12garbage",ScalarType::Integer));
}
TEST(MySqlQueryWireTest, Protocol41ErrorHeaderRequiresMarkerAndCompleteUppercaseAlphanumericState) {
  Bytes valid{std::byte{255},std::byte{40},std::byte{4},std::byte{'#'},
      std::byte{'4'},std::byte{'2'},std::byte{'S'},std::byte{'0'},std::byte{'2'}};
  EXPECT_TRUE(valid_protocol41_error_packet(valid));
  for (std::size_t n=0;n<valid.size();++n)
    EXPECT_FALSE(valid_protocol41_error_packet(std::span(valid).first(n)))<<n;
  auto invalid=valid;invalid[0]=std::byte{0};EXPECT_FALSE(valid_protocol41_error_packet(invalid));
  invalid=valid;invalid[3]=std::byte{'!'};EXPECT_FALSE(valid_protocol41_error_packet(invalid));
  valid.push_back(std::byte{0});valid.push_back(std::byte{255});
  EXPECT_TRUE(valid_protocol41_error_packet(valid)); // No message encoding assumption.
}

TEST(MySqlQueryWireTest, CompletionUsesFullLengthEncodedCountAndRejectsMalformedEof) {
  Bytes ok{std::byte{0},std::byte{254}};number(ok,4294967296ULL,8);number(ok,0,1);number(ok,2,2);number(ok,0,2);
  auto result=query_detail::completion(ok,false);ASSERT_TRUE(result);EXPECT_EQ(4294967296ULL,result->affected);
  for (std::size_t n=0;n<ok.size();++n) EXPECT_FALSE(query_detail::completion(std::span(ok).first(n),false));
  const Bytes eof{std::byte{254},std::byte{0},std::byte{0},std::byte{1},std::byte{0}};
  result=query_detail::completion(eof,true);ASSERT_TRUE(result);EXPECT_EQ(1u,result->status);
  for (std::size_t n=0;n<eof.size();++n) EXPECT_FALSE(query_detail::completion(std::span(eof).first(n),true));
}
}

namespace {
TEST(MySqlTextIntegerDomainTest, MetadataDerivedDomainsPreserveValidBytesAndDeferImpossibleValues) {
  struct Bound {
    unsigned type, flags;
    const char *minimum, *maximum, *below, *above;
  };
  const std::array<Bound, 6> bounds{{
      {1, 0, "-128", "127", "-129", "128"},
      {1, 32, "0", "255", "-1", "256"},
      {2, 0, "-32768", "32767", "-32769", "32768"},
      {2, 32, "0", "65535", "-1", "65536"},
      {3, 0, "-2147483648", "2147483647", "-2147483649", "2147483648"},
      {3, 32, "0", "4294967295", "-1", "4294967296"},
  }};
  for (const auto& bound : bounds) {
    std::uint8_t type = 0;
    bool unsigned_value = false;
    auto meta = query_detail::column(column(bound.type, 63, bound.flags), ResultLimits{},
        nullptr, &type, &unsigned_value);
    ASSERT_TRUE(meta);
    EXPECT_EQ(bound.type, type);
    EXPECT_EQ(bound.flags == 32, unsigned_value);
    const std::vector<ResultColumnMetadata> columns{*meta};
    const std::array<query_detail::NativeColumn, 1> native{{{type, unsigned_value}}};
    for (const auto value : {bound.minimum, bound.maximum, "0", "-0", "0007", "-0000"}) {
      Bytes bytes;
      text(bytes, value);
      std::vector<CellEncodingError> errors;
      auto decoded = query_detail::row(bytes, columns, 5, errors, native);
      ASSERT_TRUE(decoded);
      ASSERT_EQ(1u, decoded->size());
      ASSERT_TRUE((*decoded)[0]);
      EXPECT_EQ(value, *(*decoded)[0]); // Preserve accepted spelling, not reformatted output.
      EXPECT_TRUE(errors.empty());
    }
    for (const auto value : {bound.below, bound.above, "", "12garbage", "+1"}) {
      Bytes bytes;
      text(bytes, value);
      std::vector<CellEncodingError> errors;
      auto decoded = query_detail::row(bytes, columns, 5, errors, native);
      ASSERT_TRUE(decoded);
      ASSERT_TRUE((*decoded)[0]);
      EXPECT_TRUE((*decoded)[0]->empty());
      EXPECT_EQ((std::vector<CellEncodingError>{{5, 0}}), errors);
    }
    std::vector<CellEncodingError> errors;
    auto null = query_detail::row(Bytes{std::byte{251}}, columns, 5, errors, native);
    ASSERT_TRUE(null);
    EXPECT_FALSE((*null)[0]);
    EXPECT_TRUE(errors.empty());
  }
}

TEST(MySqlTextIntegerDomainTest, MixedRowsKeepOrderedErrorsNullBinaryAndNormalizedFallback) {
  std::vector<ResultColumnMetadata> columns;
  std::vector<query_detail::NativeColumn> native;
  for (const auto& [type, flags] : {std::pair{3u, 32u}, std::pair{1u, 0u},
      std::pair{252u, 0u}, std::pair{253u, 0u}}) {
    query_detail::NativeColumn observed;
    auto meta = query_detail::column(column(type, type == 253 ? 45 : 63, flags),
        ResultLimits{}, nullptr, &observed.type, &observed.unsigned_value);
    ASSERT_TRUE(meta);
    columns.push_back(*meta);
    native.push_back(observed);
  }
  Bytes bytes;
  text(bytes, "4294967296");
  text(bytes, "-129");
  text(bytes, std::string_view{"\0\xff", 2});
  text(bytes, std::string_view{"\xff", 1});
  std::vector<CellEncodingError> errors;
  auto decoded = query_detail::row(bytes, columns, 11, errors, native);
  ASSERT_TRUE(decoded);
  EXPECT_EQ((std::vector<CellEncodingError>{{11, 0}, {11, 1}, {11, 3}}), errors);
  EXPECT_TRUE((*decoded)[0]->empty());
  EXPECT_TRUE((*decoded)[1]->empty());
  EXPECT_EQ(std::string("\0\xff", 2), *(*decoded)[2]);
  EXPECT_TRUE((*decoded)[3]->empty());
  bytes.assign(bytes.size(), std::byte{0});
  EXPECT_EQ(std::string("\0\xff", 2), *(*decoded)[2]); // Owning, after source overwrite.

  bytes = {std::byte{251}, std::byte{251}, std::byte{0}, std::byte{0}};
  errors.clear();
  auto nullable = query_detail::row(bytes, columns, 12, errors, native);
  ASSERT_TRUE(nullable);
  EXPECT_FALSE((*nullable)[0]);
  EXPECT_FALSE((*nullable)[1]);
  EXPECT_TRUE((*nullable)[2]->empty());
  EXPECT_TRUE((*nullable)[3]->empty());
  EXPECT_TRUE(errors.empty());

  // The existing no-native-observation codec fallback remains normalized-only.
  const std::vector<ResultColumnMetadata> fallback{{"n", NativeTypeInfo{ScalarType::BigInt, 19, 0, true}}};
  for (const auto value : {"-1", "4294967296"}) {
    bytes.clear();
    text(bytes, value);
    errors.clear();
    auto legacy = query_detail::row(bytes, fallback, 0, errors);
    ASSERT_TRUE(legacy);
    EXPECT_EQ(value, *(*legacy)[0]);
    EXPECT_TRUE(errors.empty());
  }
  auto bigint = query_detail::column(column(8, 63, 32), ResultLimits{});
  ASSERT_TRUE(bigint);
  const std::vector<ResultColumnMetadata> bigint_columns{*bigint};
  const std::array<query_detail::NativeColumn, 1> bigint_native{{{8, true}}};
  bytes.clear();
  text(bytes, "18446744073709551615");
  errors.clear();
  auto maximum = query_detail::row(bytes, bigint_columns, 0, errors, bigint_native);
  ASSERT_TRUE(maximum);
  EXPECT_EQ("18446744073709551615", *(*maximum)[0]);
  EXPECT_TRUE(errors.empty());
  bytes.push_back(std::byte{0});
  EXPECT_FALSE(query_detail::row(bytes, bigint_columns, 0, errors, bigint_native));
  EXPECT_FALSE(query_detail::row(Bytes{std::byte{252}}, bigint_columns, 0, errors, bigint_native));
}
}
