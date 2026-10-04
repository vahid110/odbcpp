#include <gtest/gtest.h>
#include "core/database/mysql/prepared_wire.h"

namespace {
using namespace rs::core::database;
using namespace rs::core::database::mysql;
using rs::util::DbErrorCode;
using Bytes=std::vector<std::byte>;
void little(Bytes& bytes,unsigned n,unsigned width) {
  for (unsigned i=0;i<width;++i) bytes.push_back(static_cast<std::byte>((n>>(8*i))&255));
}
void text(Bytes& bytes,std::string_view value) {
  bytes.push_back(static_cast<std::byte>(value.size()));
  for (auto ch:value) bytes.push_back(static_cast<std::byte>(ch));
}
Bytes definition(unsigned width,unsigned precision,unsigned type=12) {
  Bytes bytes;for (auto value:{"def","","","","datetime",""}) text(bytes,value);
  little(bytes,12,1);little(bytes,63,2);little(bytes,width,4);little(bytes,type,1);
  little(bytes,0,2);little(bytes,precision,1);little(bytes,0,2);return bytes;
}
TEST(MySqlDatetimeResultsTest, ActualDefinitionsValidateAllPrecisionsAndPreserveOtherTemporalRefusals) {
  for (unsigned p=0;p<=6;++p) {
    const auto width=19+(p?1+p:0);const auto bytes=definition(width,p);
    std::uint8_t type{};bool unsigned_value{};
    auto column=query_detail::column(bytes,ResultLimits{},nullptr,&type,&unsigned_value);ASSERT_TRUE(column);
    ASSERT_TRUE(column->normalized_type);EXPECT_TRUE(column->normalized_type->known);
    EXPECT_EQ(ScalarType::Timestamp,column->normalized_type->type);EXPECT_EQ(width,column->normalized_type->column_size);
    EXPECT_EQ(p,column->normalized_type->decimal_digits);EXPECT_EQ(12u,type);EXPECT_FALSE(unsigned_value);
    for (std::size_t n=0;n<bytes.size();++n) {
      auto prefix=query_detail::column(std::span(bytes).first(n),ResultLimits{});ASSERT_FALSE(prefix);
      EXPECT_EQ(DbErrorCode::ProtocolError,prefix.error());
    }
    auto extra=bytes;extra.push_back(std::byte{0});EXPECT_FALSE(query_detail::column(extra,ResultLimits{}));
    for (auto wrong:{0u,width-1,width+1,40u}) {
      auto rejected=query_detail::column(definition(wrong,p),ResultLimits{});ASSERT_FALSE(rejected);
      EXPECT_EQ(DbErrorCode::ProtocolError,rejected.error());
    }
  }
  for (unsigned p:{7u,31u,255u}) {
    auto rejected=query_detail::column(definition(19,p),ResultLimits{});ASSERT_FALSE(rejected);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature,rejected.error());
  }
  for (unsigned type:{0u,4u,5u,7u,11u,14u,17u,18u,19u}) {
    auto rejected=query_detail::column(definition(19,0,type),ResultLimits{});ASSERT_FALSE(rejected);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature,rejected.error());
  }
}
TEST(MySqlDatetimeResultsTest, DirectAndBinaryAffinityIsSymmetricBeforeNullOrValueDecoding) {
  auto column=query_detail::column(definition(26,6),ResultLimits{});ASSERT_TRUE(column);
  const std::vector<ResultColumnMetadata> good{*column};
  const std::array<query_detail::NativeColumn,1> datetime{{{12,false}}},integer{{{8,false}}};
  const Bytes binary_null{std::byte{0},std::byte{4}},binary_integer(10,std::byte{0});
  const Bytes binary_date{std::byte{0},std::byte{0},std::byte{4},std::byte{0xe8},std::byte{7},std::byte{2},std::byte{29}};
  const Bytes direct_null{std::byte{251}};Bytes direct_date;text(direct_date,"2024-02-29 00:00:00.000000");
  std::vector<CellEncodingError> errors;
  for (bool null:{false,true}) {
    auto binary=prepared_detail::binary_row(null?binary_null:binary_date,datetime,good,0,errors);ASSERT_TRUE(binary);
    auto direct=query_detail::row(null?direct_null:direct_date,good,0,errors,datetime);ASSERT_TRUE(direct);
    EXPECT_EQ(*binary,*direct);EXPECT_TRUE(errors.empty());
  }
  for (const auto info:{NativeTypeInfo{ScalarType::BigInt,19,0,true},NativeTypeInfo{ScalarType::Timestamp,26,6,false},
      NativeTypeInfo{ScalarType::Timestamp,25,6,true},NativeTypeInfo{ScalarType::Timestamp,19,-1,true},
      NativeTypeInfo{ScalarType::Timestamp,27,7,true}}) {
    const std::vector<ResultColumnMetadata> bad{{"datetime",info}};
    for (bool null:{false,true}) {
      errors.clear();auto binary=prepared_detail::binary_row(null?binary_null:binary_date,datetime,bad,0,errors);ASSERT_FALSE(binary);
      EXPECT_EQ(DbErrorCode::ProtocolError,binary.error());EXPECT_TRUE(errors.empty());
      auto direct=query_detail::row(null?direct_null:direct_date,bad,0,errors,datetime);ASSERT_FALSE(direct);
      EXPECT_EQ(DbErrorCode::ProtocolError,direct.error());EXPECT_TRUE(errors.empty());
    }
  }
  Bytes direct_integer;text(direct_integer,"42");
  for (bool null:{false,true}) {
    errors.clear();auto binary=prepared_detail::binary_row(null?binary_null:binary_integer,integer,good,0,errors);ASSERT_FALSE(binary);
    EXPECT_EQ(DbErrorCode::ProtocolError,binary.error());EXPECT_TRUE(errors.empty());
    auto direct=query_detail::row(null?direct_null:direct_integer,good,0,errors,integer);ASSERT_FALSE(direct);
    EXPECT_EQ(DbErrorCode::ProtocolError,direct.error());EXPECT_TRUE(errors.empty());
    direct=query_detail::row(null?direct_null:direct_date,good,0,errors);ASSERT_FALSE(direct);
    EXPECT_EQ(DbErrorCode::ProtocolError,direct.error());EXPECT_TRUE(errors.empty());
  }
}
}
