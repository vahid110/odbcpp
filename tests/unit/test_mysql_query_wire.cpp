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
