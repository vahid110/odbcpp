#include <gtest/gtest.h>
#include <array>
#include "core/database/mysql/query_wire.h"

namespace {
using namespace rs::core::database;
namespace query=rs::core::database::mysql::query_detail;
using rs::util::DbErrorCode;
using Bytes=std::vector<std::byte>;
using Raw=query::NativeParameterDescriptorObservation;
void number(Bytes& bytes,std::uint32_t value,unsigned width) {
  for (unsigned i=0;i<width;++i) { bytes.push_back(static_cast<std::byte>((value>>(8*i))&255)); }
}
Bytes definition(unsigned type,unsigned charset,unsigned width,unsigned decimals,unsigned flags,
    std::string_view name="d") {
  Bytes bytes;
  for (const auto value:{std::string_view{"def"},std::string_view{},std::string_view{},std::string_view{},name,std::string_view{}}) {
    bytes.push_back(static_cast<std::byte>(value.size()));
    for (const char c:value) { bytes.push_back(static_cast<std::byte>(c)); }
  }
  number(bytes,12,1);number(bytes,charset,2);number(bytes,width,4);number(bytes,type,1);
  number(bytes,flags,2);number(bytes,decimals,1);number(bytes,0,2);return bytes;
}
void atomic_failure(const Bytes& bytes,DbErrorCode expected,
    query::ColumnContext context=query::ColumnContext::Result,ResultLimits limits={}) {
  for (bool present:{false,true}) {
    Raw raw{8,0x78563412u,73,0x4567};if (present) { raw.flags=0xbeef; }
    std::uint8_t type=9;bool unsigned_value=true;std::size_t names=99;
    auto result=query::column(bytes,limits,&names,&type,&unsigned_value,&raw,context);
    ASSERT_FALSE(result);EXPECT_EQ(expected,result.error());
    EXPECT_EQ(8u,raw.type);EXPECT_EQ(0x78563412u,raw.width);EXPECT_EQ(73u,raw.decimals);EXPECT_EQ(0x4567u,raw.charset);
    EXPECT_EQ(present?std::optional<std::uint16_t>{0xbeef}:std::nullopt,raw.flags);
    EXPECT_EQ(9u,type);EXPECT_TRUE(unsigned_value);EXPECT_EQ(99u,names);
  }
}
TEST(MySqlParameterFlagsObservationTest, LiteralTwoByteWordsPreserveUnsignedRelationshipAndAbsentVersusZero) {
  Raw legacy{246,67,30,63};EXPECT_FALSE(legacy.flags);Raw empty;EXPECT_FALSE(empty.flags);
  const std::array<std::array<unsigned,3>,5> samples{{{0,0,0},{128,128,0},{0x20,0x20,0},{0x8000,0,128},{0xffff,255,255}}};
  for (const auto& sample:samples) {
    const auto bytes=definition(1,63,3,0,sample[0]);
    EXPECT_EQ(static_cast<std::byte>(sample[1]),bytes[bytes.size()-5]);EXPECT_EQ(static_cast<std::byte>(sample[2]),bytes[bytes.size()-4]);
    Raw raw;bool unsigned_value{};std::uint8_t type{};std::size_t names{};
    auto result=query::column(bytes,ResultLimits{},&names,&type,&unsigned_value,&raw);ASSERT_TRUE(result);
    ASSERT_TRUE(raw.flags);EXPECT_EQ(sample[0],*raw.flags);EXPECT_EQ((sample[0]&32)!=0,unsigned_value);
    EXPECT_EQ(1u,type);EXPECT_EQ(1u,raw.type);EXPECT_EQ(63u,raw.charset);EXPECT_EQ(3u,raw.width);EXPECT_EQ(0u,raw.decimals);EXPECT_EQ(4u,names);
    ASSERT_TRUE(result->normalized_type);EXPECT_EQ(ScalarType::SmallInt,result->normalized_type->type);EXPECT_EQ(3u,result->normalized_type->column_size);
  }
  auto zero=query::column(definition(1,63,3,0,0),ResultLimits{},nullptr,nullptr,nullptr,&legacy);ASSERT_TRUE(zero);ASSERT_TRUE(legacy.flags);EXPECT_EQ(0u,*legacy.flags);
}
TEST(MySqlParameterFlagsObservationTest, AllSixteenBitsRoundTripWithoutExtraProfilePolicies) {
  for (unsigned flags=0;flags<=65535;++flags) {
    Raw raw;bool unsigned_value{};const auto bytes=definition(1,63,3,0,flags);
    auto observed=query::column(bytes,ResultLimits{},nullptr,nullptr,&unsigned_value,&raw);ASSERT_TRUE(observed);
    ASSERT_TRUE(raw.flags);EXPECT_EQ(flags,*raw.flags);EXPECT_EQ((flags&32)!=0,unsigned_value);
    EXPECT_EQ(ScalarType::SmallInt,observed->normalized_type->type);
  }
}
TEST(MySqlParameterFlagsObservationTest, ActualNumericFieldsAndNamesOwnValuesWithoutCapturedPacketClaim) {
  // Synthetic reconstruction from numeric246/63/67/q30/128 observation only.
  auto bytes=definition(246,63,67,30,128,"decimal");Raw raw;bool unsigned_value=true;
  auto result=query::column(bytes,ResultLimits{},nullptr,nullptr,&unsigned_value,&raw);ASSERT_TRUE(result);
  ASSERT_TRUE(raw.flags);EXPECT_EQ(128u,*raw.flags);EXPECT_FALSE(unsigned_value);
  EXPECT_EQ(65u,result->normalized_type->column_size);EXPECT_EQ(30,result->normalized_type->decimal_digits);EXPECT_EQ(ScalarType::Decimal,result->normalized_type->type);
  const auto copy=raw;auto owning=*result;bytes.assign(bytes.size(),std::byte{0});result->name="mutated";raw.flags=0;
  EXPECT_EQ("decimal",owning.name);EXPECT_EQ(65u,owning.normalized_type->column_size);EXPECT_EQ(std::optional<std::uint16_t>{128},copy.flags);EXPECT_EQ(67u,copy.width);
  auto unsigned_bytes=definition(246,63,66,30,160);Raw unsigned_raw;bool actual_unsigned=false;
  auto unsigned_result=query::column(unsigned_bytes,ResultLimits{},nullptr,nullptr,&actual_unsigned,&unsigned_raw);ASSERT_TRUE(unsigned_result);
  EXPECT_TRUE(actual_unsigned);EXPECT_EQ(std::optional<std::uint16_t>{160},unsigned_raw.flags);EXPECT_EQ(65u,unsigned_result->normalized_type->column_size);
  // Publication alone admits no Numeric parameter nor unsigned guard profile.
}
TEST(MySqlParameterFlagsObservationTest, EveryTruncatedPayloadPrefixLeavesAllOutputsUnchanged) {
  const auto valid=definition(246,63,67,30,128);
  for (std::size_t length=0;length<valid.size();++length) {
    atomic_failure(Bytes(valid.begin(),valid.begin()+length),DbErrorCode::ProtocolError);
  }
}
TEST(MySqlParameterFlagsObservationTest, LateValidationAndPolicyFailuresDoNotPublishReadFlags) {
  auto filler=definition(246,63,67,30,128);filler.back()=std::byte{1};atomic_failure(filler,DbErrorCode::ProtocolError);
  auto trailing=definition(246,63,67,30,128);trailing.push_back(std::byte{0});atomic_failure(trailing,DbErrorCode::ProtocolError);
  atomic_failure(definition(246,63,67,30,128,std::string_view("a\0b",3)),DbErrorCode::ProtocolError);
  atomic_failure(definition(246,63,67,30,128,std::string_view("\xff",1)),DbErrorCode::ProtocolError);
  atomic_failure(definition(246,63,67,30,128),DbErrorCode::UnsupportedFeature,static_cast<query::ColumnContext>(99));
  atomic_failure(definition(7,63,26,6,128),DbErrorCode::UnsupportedFeature);
  atomic_failure(definition(0,63,7,2,128),DbErrorCode::UnsupportedFeature);
  atomic_failure(definition(253,999,40,0,128),DbErrorCode::UnsupportedFeature);
  atomic_failure(definition(12,45,103,6,128),DbErrorCode::ProtocolError,query::ColumnContext::DatetimeParameterQ6Candidate);
  atomic_failure(definition(246,63,67,30,160),DbErrorCode::UnsupportedFeature); // unsigned precision66
  atomic_failure(definition(246,63,1,0,128),DbErrorCode::ProtocolError);
  ResultLimits names;names.max_column_name_bytes=0;atomic_failure(definition(246,63,67,30,128),DbErrorCode::ResourceLimit,query::ColumnContext::Result,names);
  ResultLimits budget;budget.max_metadata_name_bytes=2;
  auto invalid=definition(246,63,67,30,128);invalid.back()=std::byte{1};atomic_failure(invalid,DbErrorCode::ResourceLimit,query::ColumnContext::Result,budget);
}
TEST(MySqlParameterFlagsObservationTest, DefaultResultAndQualifiedParameterMappingRemainUnchanged) {
  const auto expanded=definition(12,45,104,6,0x8000);Raw raw;
  auto result=query::column(expanded,ResultLimits{},nullptr,nullptr,nullptr,&raw,query::ColumnContext::DatetimeParameterQ6Candidate);ASSERT_TRUE(result);
  EXPECT_EQ(104u,raw.width);EXPECT_EQ(std::optional<std::uint16_t>{0x8000},raw.flags);EXPECT_EQ(26u,result->normalized_type->column_size);EXPECT_EQ(6,result->normalized_type->decimal_digits);
  atomic_failure(expanded,DbErrorCode::ProtocolError);
  const auto semantic=definition(12,45,26,6,0x20);auto ordinary=query::column(semantic,ResultLimits{});ASSERT_TRUE(ordinary);
  auto observed=query::column(semantic,ResultLimits{},nullptr,nullptr,nullptr,&raw);ASSERT_TRUE(observed);
  EXPECT_EQ(ordinary->name,observed->name);EXPECT_EQ(ordinary->normalized_type->type,observed->normalized_type->type);EXPECT_EQ(ordinary->normalized_type->column_size,observed->normalized_type->column_size);EXPECT_EQ(ordinary->normalized_type->decimal_digits,observed->normalized_type->decimal_digits);EXPECT_EQ(std::optional<std::uint16_t>{0x20},raw.flags);
  for (const auto& bytes:{definition(10,45,40,0,128),definition(8,63,20,0,0x20),definition(246,63,67,30,128)}) {
    Raw default_raw,candidate_raw;bool default_unsigned{},candidate_unsigned{};
    auto default_result=query::column(bytes,ResultLimits{},nullptr,nullptr,&default_unsigned,&default_raw);
    auto candidate_result=query::column(bytes,ResultLimits{},nullptr,nullptr,&candidate_unsigned,&candidate_raw,query::ColumnContext::DatetimeParameterQ6Candidate);
    ASSERT_TRUE(default_result);ASSERT_TRUE(candidate_result);EXPECT_EQ(default_raw.flags,candidate_raw.flags);EXPECT_EQ(default_unsigned,candidate_unsigned);
    EXPECT_EQ(default_result->normalized_type->type,candidate_result->normalized_type->type);EXPECT_EQ(default_result->normalized_type->column_size,candidate_result->normalized_type->column_size);EXPECT_EQ(default_result->normalized_type->decimal_digits,candidate_result->normalized_type->decimal_digits);
  }
}
}
