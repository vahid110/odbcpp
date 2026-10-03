#include <gtest/gtest.h>
#include "core/database/mysql/time_wire.h"
#include <limits>
#include <vector>

namespace {
namespace time=rs::core::database::mysql::time_detail;
using Bytes=std::vector<std::byte>;
using rs::util::DbErrorCode;
void number(Bytes& bytes,std::uint32_t value) {
  for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::byte>((value>>(8*i))&255));
}
Bytes field(unsigned sign,std::uint32_t days,unsigned hour,unsigned minute,unsigned second,
    std::optional<std::uint32_t> micros=std::nullopt) {
  Bytes result{static_cast<std::byte>(micros?12:8),static_cast<std::byte>(sign)};
  number(result,days);result.push_back(static_cast<std::byte>(hour));result.push_back(static_cast<std::byte>(minute));
  result.push_back(static_cast<std::byte>(second));
  if (micros) { number(result,*micros); }
  return result;
}
auto decode(const Bytes& bytes,unsigned precision) {
  return time::binary_cell(std::span<const std::byte>(bytes),time::Profile{precision});
}
TEST(MySqlTimeWireTest, DeclaredPrecisionAndDisplayWidthAreBoundedWithoutPublishingScalarType) {
  for (unsigned p=0;p<=6;++p) {
    const auto width=10+(p?1+p:0);
    auto result=time::metadata(width,p);ASSERT_TRUE(result);EXPECT_EQ(p,result->precision);
    for (auto bad:{std::uint64_t{0},std::uint64_t{width-1},std::uint64_t{width+1},std::numeric_limits<std::uint64_t>::max()}) {
      auto rejected=time::metadata(bad,p);ASSERT_FALSE(rejected);EXPECT_EQ(DbErrorCode::ProtocolError,rejected.error());
    }
  }
  for (auto precision:{std::uint64_t{7},std::uint64_t{31},std::numeric_limits<std::uint64_t>::max()}) {
    auto result=time::metadata(10,precision);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
  }
}
TEST(MySqlTimeWireTest, ActualRawDurationsMatchExactTextSignFractionsBoundsAndTimeOfDay) {
  struct Case { Bytes bytes;unsigned precision;std::string_view text;bool time_of_day; };
  for (const auto& c:{Case{Bytes{std::byte{0}},0,"00:00:00",true},
      Case{Bytes{std::byte{0}},6,"00:00:00.000000",true},
      Case{field(0,0,1,2,3),0,"01:02:03",true},
      Case{field(0,0,1,2,3),6,"01:02:03.000000",true},
      Case{field(0,0,1,2,3,100000),1,"01:02:03.1",true},
      Case{field(0,0,1,2,3,120000),2,"01:02:03.12",true},
      Case{field(0,0,1,2,3,123000),3,"01:02:03.123",true},
      Case{field(0,0,1,2,3,123400),4,"01:02:03.1234",true},
      Case{field(0,0,1,2,3,123450),5,"01:02:03.12345",true},
      Case{field(0,0,1,2,3,123456),6,"01:02:03.123456",true},
      Case{field(0,0,23,59,59,999999),6,"23:59:59.999999",true},
      Case{field(0,1,0,0,0),0,"24:00:00",false},
      Case{field(1,0,0,0,0,1),6,"-00:00:00.000001",false},
      Case{field(0,34,22,59,59),0,"838:59:59",false},
      Case{field(1,34,22,59,59,0),6,"-838:59:59.000000",false}}) {
    SCOPED_TRACE(c.text);auto wire=decode(c.bytes,c.precision);ASSERT_TRUE(wire);ASSERT_TRUE(wire->value);
    EXPECT_EQ(c.text,*wire->value);EXPECT_FALSE(wire->encoding_error);EXPECT_EQ(c.time_of_day,wire->time_of_day);
    auto text=time::text_cell(c.text,time::Profile{c.precision});ASSERT_TRUE(text);ASSERT_TRUE(text->value);
    EXPECT_EQ(wire->value,text->value);EXPECT_FALSE(text->encoding_error);EXPECT_EQ(wire->time_of_day,text->time_of_day);
  }
}
TEST(MySqlTimeWireTest, CompleteMalformedComponentsAndPrecisionBecomeNonNullCellErrors) {
  for (const auto& bytes:{field(2,0,1,2,3),field(0,0,24,0,0),field(0,0,1,60,0),field(0,0,1,0,60),
      field(0,0,0,0,0,1000000),field(0,std::numeric_limits<std::uint32_t>::max(),0,0,0),
      field(0,34,22,59,59,1),field(0,34,23,0,0),field(1,0,0,0,0),field(1,0,0,0,0,0)}) {
    auto result=decode(bytes,6);ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);EXPECT_FALSE(result->time_of_day);
  }
  for (unsigned p:{0u,1u,2u,3u,4u,5u}) {
    auto result=decode(field(0,0,1,2,3,100001),p);ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);
  }
  auto valid_zero=decode(field(0,0,0,0,0,0),0);ASSERT_TRUE(valid_zero);
  EXPECT_EQ(std::optional<std::string>{"00:00:00"},valid_zero->value);EXPECT_FALSE(valid_zero->encoding_error);
}
TEST(MySqlTimeWireTest, StrictTextRejectsRelaxationsOverflowNegativeZeroAndWrongFractionWidth) {
  for (const auto text:{"","1:02:03","001:02:03","+01:02:03"," 01:02:03","01:02:03 ","1 01:02:03",
      "01:60:03","01:02:60","839:00:00","99999999999999999:00:00","-00:00:00","01:02:03Z",
      "01:02:03.0","01:02:03+01:00","ab:cd:ef"}) {
    SCOPED_TRACE(text);auto result=time::text_cell(std::string_view(text),time::Profile{0});ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);
  }
  for (const auto text:{"00:00:00","00:00:00.00000","00:00:00.0000000","-00:00:00.000000","838:59:59.000001"}) {
    auto result=time::text_cell(std::string_view(text),time::Profile{6});ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);
  }
  for (const auto text:{std::string_view("01:02:\0" "3",8),std::string_view("01:02:\xff" "3",8)}) {
    auto result=time::text_cell(text,time::Profile{0});ASSERT_TRUE(result);EXPECT_TRUE(result->encoding_error);
  }
}
TEST(MySqlTimeWireTest, EveryPrefixExtraByteAndWrongRawLengthIsFramingFailure) {
  for (const auto& bytes:{Bytes{std::byte{0}},field(0,0,1,2,3),field(0,0,1,2,3,123456)}) {
    for (std::size_t n=0;n<bytes.size();++n) {
      auto result=time::binary_cell(std::span<const std::byte>(bytes).first(n),time::Profile{6});ASSERT_FALSE(result);
      EXPECT_EQ(DbErrorCode::ProtocolError,result.error());EXPECT_TRUE(result.error_message().empty());
    }
    auto extra=bytes;extra.push_back(std::byte{0});auto result=decode(extra,6);ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  }
  for (unsigned length:{1u,4u,7u,11u,13u,251u,255u}) {
    Bytes bytes(length+1,std::byte{0});bytes[0]=static_cast<std::byte>(length);
    auto result=decode(bytes,0);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  }
}
TEST(MySqlTimeWireTest, NullRequiresExplicitAbsenceAndBothFormsOwnInputBytes) {
  auto text_null=time::text_cell(std::nullopt,time::Profile{6});ASSERT_TRUE(text_null);
  EXPECT_FALSE(text_null->value);EXPECT_FALSE(text_null->encoding_error);EXPECT_FALSE(text_null->time_of_day);
  auto wire_null=time::binary_cell(std::nullopt,time::Profile{6});ASSERT_TRUE(wire_null);
  EXPECT_FALSE(wire_null->value);EXPECT_FALSE(wire_null->encoding_error);
  for (const auto& result:{time::text_cell(std::nullopt,time::Profile{7}),time::binary_cell(std::nullopt,time::Profile{7})}) {
    ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
  }
  std::string input="-838:59:59.000000";auto text=time::text_cell(input,time::Profile{6});input.assign(input.size(),'x');
  ASSERT_TRUE(text);EXPECT_EQ(std::optional<std::string>{"-838:59:59.000000"},text->value);
  auto bytes=field(0,0,1,2,3,123456);auto wire=decode(bytes,6);bytes.clear();bytes.shrink_to_fit();
  ASSERT_TRUE(wire);EXPECT_EQ(std::optional<std::string>{"01:02:03.123456"},wire->value);
}
}
