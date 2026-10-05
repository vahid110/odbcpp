#include <gtest/gtest.h>
#include "core/database/mysql/parameter_receipt_shape.h"
#include "core/database/mysql/date_parameter_descriptor.h"
namespace {
using namespace rs::core::database;
namespace q=rs::core::database::mysql::query_detail;
namespace shape=rs::core::database::mysql::parameter_receipt_detail;
namespace date=rs::core::database::mysql::date_parameter_detail;
using rs::util::DbErrorCode;
using Raw=q::NativeParameterDescriptorObservation;
using Bytes=std::vector<std::byte>;
void n(Bytes& b,std::uint64_t x,unsigned width) { for(unsigned i=0;i<width;++i) { b.push_back(static_cast<std::byte>((x>>(8*i))&255)); } }
Bytes column(unsigned type,unsigned charset,unsigned width,unsigned scale,unsigned flags) {
  Bytes b{std::byte{3},std::byte{'d'},std::byte{'e'},std::byte{'f'},std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{'p'},std::byte{0}};
  n(b,12,1);n(b,charset,2);n(b,width,4);n(b,type,1);n(b,flags,2);n(b,scale,1);n(b,0,2);return b;
}
template<class T> void error(const rs::util::Result<T>& v,DbErrorCode e) { ASSERT_FALSE(v);EXPECT_EQ(e,v.error()); }
const Raw dt{12,104,6,45,0};const NativeTypeInfo datetime{ScalarType::Timestamp,26,6,true};
const Raw dec{246,67,30,63,128};const NativeTypeInfo decimal{ScalarType::Decimal,65,30,true};
TEST(MySqlParameterReceiptShapeTest, ParserOwnsAnchorsAndResultContextStaysStrict) {
  Raw raw;auto bytes=column(12,45,104,6,0);auto c=q::column(bytes,ResultLimits{},nullptr,nullptr,nullptr,&raw,q::ColumnContext::DatetimeParameterQ6Candidate);ASSERT_TRUE(c);
  auto owned=raw;const auto info=*c->normalized_type;bytes.clear();raw={};auto s=shape::datetime_q6_shape(owned,info);ASSERT_TRUE(s);EXPECT_EQ(6u,*s);
  error(q::column(column(12,45,104,6,0),ResultLimits{}),DbErrorCode::ProtocolError);
  auto d=q::column(column(246,63,67,30,128),ResultLimits{},nullptr,nullptr,nullptr,&raw);ASSERT_TRUE(d);ASSERT_TRUE(raw.flags);EXPECT_EQ(128,*raw.flags);
  auto p=shape::decimal_65q30_shape(raw,*raw.flags,*d->normalized_type);ASSERT_TRUE(p);EXPECT_EQ(65u,p->precision);EXPECT_EQ(30u,p->scale);
  auto physical=q::column(column(246,63,7,2,128),ResultLimits{});ASSERT_TRUE(physical);EXPECT_EQ(5u,physical->normalized_type->column_size);EXPECT_EQ(2,physical->normalized_type->decimal_digits);
}
TEST(MySqlParameterReceiptShapeTest, DatetimeFamilyPolicyAndDimensionFirstErrorsAreLiteral) {
  auto raw=dt;auto norm=datetime;raw.type=7;norm.known=false;raw.charset=0;error(shape::datetime_q6_shape(raw,norm),DbErrorCode::UnsupportedFeature);
  raw=dt;norm={ScalarType::BigInt,19,0,false};raw.type=8;error(shape::datetime_q6_shape(raw,norm),DbErrorCode::UnsupportedFeature);
  for(auto family:{ScalarType::Date,ScalarType::Decimal,ScalarType::BigInt}) { norm=datetime;norm.type=family;error(shape::datetime_q6_shape(dt,norm),DbErrorCode::ProtocolError); }
  raw=dt;raw.type=8;error(shape::datetime_q6_shape(raw,datetime),DbErrorCode::ProtocolError);
  norm=datetime;norm.known=false;raw=dt;raw.charset=0;error(shape::datetime_q6_shape(raw,norm),DbErrorCode::ProtocolError);
  for(unsigned cs:{0u,46u,63u,255u,65535u}) { raw=dt;raw.charset=static_cast<std::uint16_t>(cs);raw.width=0;error(shape::datetime_q6_shape(raw,datetime),DbErrorCode::UnsupportedFeature); }
  for(unsigned p:{0u,3u,7u,255u}) { raw=dt;raw.decimals=static_cast<std::uint8_t>(p);raw.width=0;error(shape::datetime_q6_shape(raw,datetime),DbErrorCode::UnsupportedFeature); }
  for(unsigned width:{0u,26u,103u,105u,0xffffffffu}) { raw=dt;raw.width=width;error(shape::datetime_q6_shape(raw,datetime),DbErrorCode::ProtocolError); }
  for(std::uint64_t width:{std::uint64_t{0},std::uint64_t{25},std::uint64_t{27},std::uint64_t{104},UINT64_MAX}) { norm=datetime;norm.column_size=width;error(shape::datetime_q6_shape(dt,norm),DbErrorCode::ProtocolError); }
  for(std::int16_t p:{std::int16_t{-1},std::int16_t{0},std::int16_t{3},std::int16_t{7},std::int16_t{INT16_MAX}}) { norm=datetime;norm.decimal_digits=p;error(shape::datetime_q6_shape(dt,norm),DbErrorCode::ProtocolError); }
  for(auto flags:{std::optional<std::uint16_t>{},std::optional<std::uint16_t>{0},std::optional<std::uint16_t>{32},std::optional<std::uint16_t>{128},std::optional<std::uint16_t>{65535}}) { raw=dt;raw.flags=flags;ASSERT_TRUE(shape::datetime_q6_shape(raw,datetime)); }
}
TEST(MySqlParameterReceiptShapeTest, DecimalCoherenceLegacyKnownAndFamilyPrecedence) {
  auto raw=dec;auto norm=decimal;raw.flags.reset();raw.type=0;norm.known=false;error(shape::decimal_65q30_shape(raw,128,norm),DbErrorCode::ProtocolError);
  for(unsigned flags:{0u,32u,128u,65535u}) { raw=dec;raw.flags.reset();error(shape::decimal_65q30_shape(raw,static_cast<std::uint16_t>(flags),decimal),DbErrorCode::ProtocolError); }
  raw=dec;error(shape::decimal_65q30_shape(raw,0,decimal),DbErrorCode::ProtocolError);raw.flags=0x8000;error(shape::decimal_65q30_shape(raw,0x80,decimal),DbErrorCode::ProtocolError);
  raw=dec;raw.type=0;norm.known=false;error(shape::decimal_65q30_shape(raw,128,norm),DbErrorCode::UnsupportedFeature);
  raw=dec;raw.type=8;norm={ScalarType::BigInt,19,0,false};error(shape::decimal_65q30_shape(raw,128,norm),DbErrorCode::ProtocolError);norm.known=true;error(shape::decimal_65q30_shape(raw,128,norm),DbErrorCode::UnsupportedFeature);
  norm=decimal;norm.type=ScalarType::Numeric;raw=dec;raw.flags=0;error(shape::decimal_65q30_shape(raw,0,norm),DbErrorCode::ProtocolError);
  raw=dec;raw.type=8;error(shape::decimal_65q30_shape(raw,128,decimal),DbErrorCode::ProtocolError);
}
TEST(MySqlParameterReceiptShapeTest, DecimalFullWordAndTupleBoundariesDoNotInferStorage) {
  for(unsigned flags:{0u,1u,32u,128u,129u,160u,0x2000u,0x8000u,65535u}) { auto raw=dec;raw.flags=static_cast<std::uint16_t>(flags);auto s=shape::decimal_65q30_shape(raw,static_cast<std::uint16_t>(flags),decimal);if(flags==128) { ASSERT_TRUE(s);EXPECT_EQ(65u,s->precision); } else { error(s,DbErrorCode::UnsupportedFeature); } }
  for(unsigned cs:{0u,45u,46u,255u,65535u}) { auto raw=dec;raw.charset=static_cast<std::uint16_t>(cs);raw.width=0;error(shape::decimal_65q30_shape(raw,128,decimal),DbErrorCode::UnsupportedFeature); }
  for(unsigned p:{0u,2u,29u,31u,255u}) { auto raw=dec;raw.decimals=static_cast<std::uint8_t>(p);raw.width=0;error(shape::decimal_65q30_shape(raw,128,decimal),DbErrorCode::UnsupportedFeature); }
  for(unsigned width:{0u,5u,66u,68u,0xffffffffu}) { auto raw=dec;raw.width=width;error(shape::decimal_65q30_shape(raw,128,decimal),DbErrorCode::ProtocolError); }
  for(std::uint64_t width:{std::uint64_t{0},std::uint64_t{5},std::uint64_t{64},std::uint64_t{66},UINT64_MAX}) { auto norm=decimal;norm.column_size=width;error(shape::decimal_65q30_shape(dec,128,norm),DbErrorCode::ProtocolError); }
  for(std::int16_t p:{std::int16_t{-1},std::int16_t{2},std::int16_t{29},std::int16_t{31},std::int16_t{INT16_MAX}}) { auto norm=decimal;norm.decimal_digits=p;error(shape::decimal_65q30_shape(dec,128,norm),DbErrorCode::ProtocolError); }
  Raw raw;bool uns=true;auto c=q::column(column(246,63,67,30,128),ResultLimits{},nullptr,nullptr,&uns,&raw);ASSERT_TRUE(c);EXPECT_FALSE(uns);ASSERT_TRUE(raw.flags);EXPECT_EQ(128,*raw.flags);
  error(q::column(column(246,63,67,30,160),ResultLimits{}),DbErrorCode::UnsupportedFeature);
}
TEST(MySqlParameterReceiptShapeTest, DateSplitKeepsShapeBeforeCharsetWidthAndHint) {
  for(unsigned cs:{63u,45u}) { Raw raw{10,cs==63?10u:40u,0,static_cast<std::uint16_t>(cs),0};NativeTypeInfo norm{ScalarType::Date,10,0,true};ASSERT_TRUE(date::receipt_shape(raw,norm));ASSERT_TRUE(date::descriptor(QueryParameterType::Date,raw,norm));error(date::descriptor(QueryParameterType::Text,raw,norm),DbErrorCode::UnsupportedFeature);
    for(unsigned width:{0u,9u,11u,39u,41u,0xffffffffu}) { auto bad=raw;bad.width=width;error(date::receipt_shape(bad,norm),DbErrorCode::ProtocolError);error(date::descriptor(QueryParameterType::Text,bad,norm),DbErrorCode::ProtocolError); }
    norm.known=false;raw.charset=0;error(date::receipt_shape(raw,norm),DbErrorCode::ProtocolError);
  }
  Raw raw{10,10,0,0,{}};NativeTypeInfo norm{ScalarType::Date,10,0,true};error(date::receipt_shape(raw,norm),DbErrorCode::UnsupportedFeature);norm.column_size=40;error(date::receipt_shape(raw,norm),DbErrorCode::ProtocolError);
  raw={8,19,0,63,0};norm={ScalarType::BigInt,19,0,false};error(date::receipt_shape(raw,norm),DbErrorCode::UnsupportedFeature);norm={ScalarType::Date,10,0,true};error(date::receipt_shape(raw,norm),DbErrorCode::ProtocolError);
}
TEST(MySqlParameterReceiptShapeTest, MalformedTailPublicationIsAtomicAndCopiesOwnFields) {
  const Raw sentinel{8,19,0,63,65535};Raw raw=sentinel;auto bytes=column(246,63,67,30,128);bytes.pop_back();EXPECT_FALSE(q::column(bytes,ResultLimits{},nullptr,nullptr,nullptr,&raw));EXPECT_EQ(sentinel.type,raw.type);EXPECT_EQ(sentinel.flags,raw.flags);EXPECT_EQ(sentinel.width,raw.width);
  bytes=column(246,63,67,30,128);auto c=q::column(bytes,ResultLimits{},nullptr,nullptr,nullptr,&raw);ASSERT_TRUE(c);const auto owned=raw;const auto norm=*c->normalized_type;bytes.clear();raw.flags.reset();auto result=shape::decimal_65q30_shape(owned,128,norm);ASSERT_TRUE(result);EXPECT_EQ(65u,result->precision);EXPECT_EQ(30u,result->scale);EXPECT_EQ(std::optional<std::uint16_t>{128},owned.flags);
}
}
