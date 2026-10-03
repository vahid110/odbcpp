#include <gtest/gtest.h>
#include "core/database/mysql/prepared_wire.h"

namespace {
using namespace rs::core::database;
using namespace rs::core::database::mysql;
using Bytes=std::vector<std::byte>;
using rs::util::DbErrorCode;
void little(Bytes& bytes,std::uint64_t n,std::size_t width) {
  for (std::size_t i=0;i<width;++i) bytes.push_back(static_cast<std::byte>((n>>(8*i))&255));
}
void text(Bytes& bytes,std::string_view value) {
  bytes.push_back(static_cast<std::byte>(value.size()));
  for (const auto ch:value) bytes.push_back(static_cast<std::byte>(ch));
}
Bytes definition(unsigned width,unsigned scale,bool unsigned_value=false,unsigned type=246) {
  Bytes bytes;for (const auto field:{"def","","","","decimal",""}) text(bytes,field);
  little(bytes,12,1);little(bytes,63,2);little(bytes,width,4);little(bytes,type,1);
  little(bytes,unsigned_value?32:0,2);little(bytes,scale,1);little(bytes,0,2);return bytes;
}
TEST(MySqlDecimalWireTest, ActualMetadataDerivesPrecisionAndChecksProfileWithoutFallback) {
  struct Case { unsigned width,scale,precision;bool unsigned_value; };
  for (const auto c:{Case{2,0,1,false},Case{7,2,5,false},Case{6,2,5,true},
      Case{7,5,5,false},Case{66,0,65,false},Case{67,30,65,false}}) {
    const auto bytes=definition(c.width,c.scale,c.unsigned_value);
    std::uint8_t type{};bool native_unsigned{};
    auto result=query_detail::column(bytes,ResultLimits{},nullptr,&type,&native_unsigned);ASSERT_TRUE(result);
    EXPECT_EQ(ScalarType::Decimal,result->normalized_type->type);
    EXPECT_EQ(c.precision,result->normalized_type->column_size);EXPECT_EQ(c.scale,result->normalized_type->decimal_digits);
    EXPECT_TRUE(result->normalized_type->known);EXPECT_EQ(246u,type);EXPECT_EQ(c.unsigned_value,native_unsigned);
    for (std::size_t n=0;n<bytes.size();++n) EXPECT_FALSE(query_detail::column(std::span(bytes).first(n),ResultLimits{}));
    auto trailing=bytes;trailing.push_back(std::byte{0});EXPECT_FALSE(query_detail::column(trailing,ResultLimits{}));
  }
  for (const auto c:{Case{0,0,0,false},Case{1,0,0,false},Case{5,5,0,false}}) {
    auto result=query_detail::column(definition(c.width,c.scale),ResultLimits{});ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  }
  for (const auto c:{Case{68,30,0,false},Case{67,31,0,false}}) {
    auto result=query_detail::column(definition(c.width,c.scale),ResultLimits{});ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
  }
  for (unsigned type:{0u,4u,5u,7u,11u,12u}) {
    auto result=query_detail::column(definition(7,2,false,type),ResultLimits{});ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
  }
}
TEST(MySqlDecimalWireTest, ExactDigitsDoNotRoundNarrowOrConfuseEmptyWithNull) {
  const NativeTypeInfo decimal{ScalarType::Decimal,5,2,true};
  for (const auto value:{"999.99","-999.99","0.00","-0.00","0001.20"})
    EXPECT_TRUE(decimal_detail::valid_cell(value,decimal,false))<<value;
  for (const auto value:{"","-",".12","1.","+1.20"," 1.20","1.20 ","1e2","1.2","1.200","1.2.0","1000.00","-1000.00"})
    EXPECT_FALSE(decimal_detail::valid_cell(value,decimal,false))<<value;
  EXPECT_FALSE(decimal_detail::valid_cell(std::string_view("1\0.20",5),decimal,false));
  EXPECT_FALSE(decimal_detail::valid_cell(std::string_view("1.\xff" "0",4),decimal,false));
  EXPECT_FALSE(decimal_detail::valid_cell("-0.00",decimal,true));
  EXPECT_TRUE(decimal_detail::valid_cell("0.99999",NativeTypeInfo{ScalarType::Decimal,5,5,true},false));
  EXPECT_FALSE(decimal_detail::valid_cell("1.00000",NativeTypeInfo{ScalarType::Decimal,5,5,true},false));
  EXPECT_TRUE(decimal_detail::valid_cell(std::string(65,'9'),NativeTypeInfo{ScalarType::Decimal,65,0,true},false));
  EXPECT_FALSE(decimal_detail::valid_cell(std::string(66,'9'),NativeTypeInfo{ScalarType::Decimal,65,0,true},false));
  EXPECT_TRUE(decimal_detail::valid_cell("-"+std::string(35,'9')+"."+std::string(30,'9'),NativeTypeInfo{ScalarType::Decimal,65,30,true},false));
  EXPECT_TRUE(query_detail::valid_cell("18446744073709551615",ScalarType::Numeric));
  EXPECT_FALSE(query_detail::valid_cell("1.20",ScalarType::Numeric));
}
TEST(MySqlDecimalWireTest, TextAndBinaryRowsShareUnsignedScaleAndDeferredCellPolicy) {
  std::vector<ResultColumnMetadata> columns;std::vector<query_detail::NativeColumn> native;
  for (const auto& bytes:{definition(7,2),definition(6,2,true),definition(7,5)}) {
    query_detail::NativeColumn descriptor;
    auto column=query_detail::column(bytes,ResultLimits{},nullptr,&descriptor.type,&descriptor.unsigned_value);ASSERT_TRUE(column);
    columns.push_back(*column);native.push_back(descriptor);
  }
  for (bool invalid:{false,true}) {
    Bytes direct,binary{std::byte{0},std::byte{0}};
    const std::array values{invalid?"1000.00":"-999.99",invalid?"-0.01":"999.99",invalid?"0.999999":"0.99999"};
    for (const auto value:values) { text(direct,value);text(binary,value); }
    std::vector<CellEncodingError> direct_errors,binary_errors;
    auto a=query_detail::row(direct,columns,7,direct_errors,native);
    auto b=prepared_detail::binary_row(binary,native,columns,7,binary_errors);ASSERT_TRUE(a);ASSERT_TRUE(b);
    EXPECT_EQ(*a,*b);EXPECT_EQ(direct_errors,binary_errors);
    EXPECT_EQ(invalid?(std::vector<CellEncodingError>{{7,0},{7,1},{7,2}}):std::vector<CellEncodingError>{},direct_errors);
    for (std::size_t i=0;i<3;++i) EXPECT_EQ(invalid?"":values[i],*(*a)[i]);
  }
  std::vector<CellEncodingError> errors;
  auto nulls=query_detail::row(Bytes{std::byte{251},std::byte{251},std::byte{251}},columns,0,errors,native);ASSERT_TRUE(nulls);
  auto binary_nulls=prepared_detail::binary_row(Bytes{std::byte{0},std::byte{28}},native,columns,0,errors);ASSERT_TRUE(binary_nulls);
  EXPECT_EQ(*nulls,*binary_nulls);EXPECT_TRUE(errors.empty());
  auto empty=query_detail::row(Bytes{std::byte{0},std::byte{251},std::byte{251}},columns,1,errors,native);ASSERT_TRUE(empty);
  ASSERT_TRUE((*empty)[0]);EXPECT_TRUE((*empty)[0]->empty());EXPECT_EQ((std::vector<CellEncodingError>{{1,0}}),errors);
}
TEST(MySqlDecimalWireTest, BadLengthFramingAndMetadataAffinityRejectWholeRows) {
  auto column=query_detail::column(definition(7,2),ResultLimits{});ASSERT_TRUE(column);
  const std::vector<ResultColumnMetadata> columns{*column};
  const std::array<query_detail::NativeColumn,1> native{{{246,false}}};
  for (const auto& value:{Bytes{std::byte{255}},Bytes{std::byte{252}},Bytes{std::byte{253},std::byte{1}},
      Bytes{std::byte{254},std::byte{1}},Bytes{std::byte{5},std::byte{'1'}},Bytes{std::byte{4},std::byte{'1'},std::byte{'.'},std::byte{'2'},std::byte{'0'},std::byte{0}}}) {
    std::vector<CellEncodingError> errors;
    auto direct=query_detail::row(value,columns,0,errors,native);ASSERT_FALSE(direct);EXPECT_EQ(DbErrorCode::ProtocolError,direct.error());
    Bytes binary{std::byte{0},std::byte{0}};binary.insert(binary.end(),value.begin(),value.end());
    auto result=prepared_detail::binary_row(binary,native,columns,0,errors);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  }
  std::vector<CellEncodingError> errors;
  EXPECT_FALSE(query_detail::row(Bytes{std::byte{251}},columns,0,errors));
  EXPECT_FALSE(prepared_detail::binary_row(Bytes{std::byte{0},std::byte{1}},native,columns,0,errors));
}
}
