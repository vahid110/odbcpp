#include "core/database/mysql/prepared_wire.h"
#include <gtest/gtest.h>
#include <initializer_list>

namespace {
using namespace rs::core::database;
using namespace rs::core::database::mysql::prepared_detail;
using rs::util::DbErrorCode;
using Bytes=std::vector<std::byte>;
constexpr auto policy=DatetimeWriterPolicy::DateDatetimeQ6Candidate;
Bytes literal(std::initializer_list<unsigned> values) {
  Bytes out;for (const auto value:values) { out.push_back(static_cast<std::byte>(value)); }return out;
}
auto request(const QueryParameter& input,const InputLimits& limits=InputLimits{}) {
  return execute_datetime_q6_candidate_request(0x01020304,std::span(&input,1),limits,policy);
}
TEST(MySqlDatetimeQ6ParameterPacketTest, AllRawFormsAndNullHaveIndependentCompleteFrames) {
  struct Witness { const char* text;unsigned precision;unsigned micros;Bytes expected; };
  const std::array witnesses{
    Witness{"2024-01-02 00:00:00",0,0,literal({0x13,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,4,0xe8,7,1,2})},
    Witness{"2024-02-29 12:34:56",0,0,literal({0x16,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,7,0xe8,7,2,29,12,34,56})},
    Witness{"2024-02-29 12:34:56.123456",6,123456,literal({0x1a,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,11,0xe8,7,2,29,12,34,56,0x40,0xe2,1,0})}};
  for (const auto& item:witnesses) {
    auto output=request({item.text,QueryParameterType::Timestamp});ASSERT_TRUE(output);
    EXPECT_EQ(item.expected,output->packet);ASSERT_EQ(1u,output->datetime_observations.size());
    ASSERT_TRUE(output->datetime_observations[0]);const auto& observation=*output->datetime_observations[0];
    ASSERT_TRUE(observation.encoded.value);EXPECT_EQ(item.precision,observation.caller_precision);
    EXPECT_EQ(item.micros,observation.micros);
  }
  auto null=request({std::nullopt,QueryParameterType::Timestamp});ASSERT_TRUE(null);
  EXPECT_EQ(literal({14,0,0,0,23,4,3,2,1,0,1,0,0,0,1,1,12,0}),null->packet);
  ASSERT_EQ(1u,null->datetime_observations.size());ASSERT_TRUE(null->datetime_observations[0]);
  EXPECT_FALSE(null->datetime_observations[0]->encoded.value);
  EXPECT_FALSE(null->datetime_observations[0]->caller_precision);EXPECT_FALSE(null->datetime_observations[0]->micros);
}
TEST(MySqlDatetimeQ6ParameterPacketTest, EveryLexicalPrecisionIsOwnedEvenWhenZeroUsesRawFour) {
  const std::array<unsigned,7> micros{0,100000,120000,123000,123400,123450,123456};
  const std::array<const char*,7> fractions{"",".1",".12",".123",".1234",".12345",".123456"};
  for (unsigned precision=0;precision<=6;++precision) {
    auto output=request({std::string("2024-01-02 00:00:00")+fractions[precision],QueryParameterType::Timestamp});
    ASSERT_TRUE(output);ASSERT_TRUE(output->datetime_observations[0]);
    EXPECT_EQ(precision,output->datetime_observations[0]->caller_precision);
    EXPECT_EQ(micros[precision],output->datetime_observations[0]->micros);
    auto zero=request({std::string("2024-01-02 00:00:00")+(precision?"."+std::string(precision,'0'):""),QueryParameterType::Timestamp});
    ASSERT_TRUE(zero);EXPECT_EQ(literal({19,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,4,0xe8,7,1,2}),zero->packet);
    ASSERT_TRUE(zero->datetime_observations[0]);EXPECT_EQ(precision,zero->datetime_observations[0]->caller_precision);
    EXPECT_EQ(0u,zero->datetime_observations[0]->micros);
  }
}
TEST(MySqlDatetimeQ6ParameterPacketTest, DateAndDatetimeBothOrdersAndNullsPreserveSlotIdentity) {
  std::array<QueryParameter,2> slots{{{"2024-02-29",QueryParameterType::Date},{"2024-01-02 00:00:00",QueryParameterType::Timestamp}}};
  auto output=execute_datetime_q6_candidate_request(0x01020304,slots,InputLimits{},policy);ASSERT_TRUE(output);
  EXPECT_EQ(literal({26,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,10,0,12,0,4,0xe8,7,2,29,4,0xe8,7,1,2}),output->packet);
  EXPECT_FALSE(output->datetime_observations[0]);ASSERT_TRUE(output->datetime_observations[1]);
  std::swap(slots[0],slots[1]);auto reverse=execute_datetime_q6_candidate_request(0x01020304,slots,InputLimits{},policy);ASSERT_TRUE(reverse);
  EXPECT_EQ(literal({26,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,10,0,4,0xe8,7,1,2,4,0xe8,7,2,29}),reverse->packet);
  ASSERT_TRUE(reverse->datetime_observations[0]);EXPECT_FALSE(reverse->datetime_observations[1]);
  slots[0].value.reset();auto null=execute_datetime_q6_candidate_request(0x01020304,slots,InputLimits{},policy);ASSERT_TRUE(null);
  EXPECT_EQ(literal({21,0,0,0,23,4,3,2,1,0,1,0,0,0,1,1,12,0,10,0,4,0xe8,7,2,29}),null->packet);
  ASSERT_TRUE(null->datetime_observations[0]);EXPECT_FALSE(null->datetime_observations[0]->encoded.value);
  slots[0].value="2024-01-02 00:00:00";slots[1].value.reset();
  auto date_null=execute_datetime_q6_candidate_request(0x01020304,slots,InputLimits{},policy);ASSERT_TRUE(date_null);
  EXPECT_EQ(literal({21,0,0,0,23,4,3,2,1,0,1,0,0,0,2,1,12,0,10,0,4,0xe8,7,1,2}),date_null->packet);
  EXPECT_FALSE(date_null->datetime_observations[1]);
}
TEST(MySqlDatetimeQ6ParameterPacketTest, NineMixedSlotsHaveLiteralOffsetZeroBitmapAndValueOrder) {
  std::array<QueryParameter,9> slots{{{std::nullopt,QueryParameterType::Timestamp},{"2024-02-29",QueryParameterType::Date},
    {"-2",QueryParameterType::Int16},{"2024-01-02 00:00:00",QueryParameterType::Timestamp},
    {"x",QueryParameterType::Text},{"1",QueryParameterType::Boolean},{std::string("\0\xff",2),QueryParameterType::Binary,true},
    {"7",QueryParameterType::Int32},{std::nullopt,QueryParameterType::Timestamp}}};
  auto output=execute_datetime_q6_candidate_request(0x01020304,slots,InputLimits{},policy);ASSERT_TRUE(output);
  EXPECT_EQ(literal({0x35,0,0,0,23,4,3,2,1,0,1,0,0,0,1,1,1,
    12,0,10,0,2,0,12,0,253,0,1,0,252,0,3,0,12,0,
    4,0xe8,7,2,29,0xfe,0xff,4,0xe8,7,1,2,1,'x',1,2,0,0xff,7,0,0,0}),output->packet);
  ASSERT_EQ(57u,output->packet.size());ASSERT_EQ(9u,output->datetime_observations.size());
  for (std::size_t i=0;i<9;++i) {
    SCOPED_TRACE(i);
    if (i==0 || i==3 || i==8) {
      ASSERT_TRUE(output->datetime_observations[i]);EXPECT_EQ(i==3,output->datetime_observations[i]->encoded.value.has_value());
    } else { EXPECT_FALSE(output->datetime_observations[i]); }
  }
}
TEST(MySqlDatetimeQ6ParameterPacketTest, ExistingScalarNeighborsRetainLiteralNativeTypesAndExtrema) {
  std::array<QueryParameter,6> slots{{{"-32768",QueryParameterType::Int16},{"2147483647",QueryParameterType::Int32},
    {"-9223372036854775808",QueryParameterType::Int64},{"",QueryParameterType::Unspecified},{"é",QueryParameterType::Text},
    {std::nullopt,QueryParameterType::Binary}}};
  auto output=execute_datetime_q6_candidate_request(1,slots,InputLimits{},policy);ASSERT_TRUE(output);
  EXPECT_EQ(literal({42,0,0,0,23,1,0,0,0,0,1,0,0,0,0x20,1,2,0,3,0,8,0,253,0,253,0,252,0,
    0,0x80,0xff,0xff,0xff,0x7f,0,0,0,0,0,0,0,0x80,0,2,0xc3,0xa9}),output->packet);
  for (const auto& observation:output->datetime_observations) { EXPECT_FALSE(observation); }
}
TEST(MySqlDatetimeQ6ParameterPacketTest, EmptyAndClosedPoliciesDoNotWidenOldProfiles) {
  auto empty=execute_datetime_q6_candidate_request(1,{},InputLimits{},policy);ASSERT_TRUE(empty);
  EXPECT_EQ(literal({10,0,0,0,23,1,0,0,0,0,1,0,0,0}),empty->packet);EXPECT_TRUE(empty->datetime_observations.empty());
  auto invalid=execute_datetime_q6_candidate_request(1,{},InputLimits{},static_cast<DatetimeWriterPolicy>(99));ASSERT_FALSE(invalid);
  EXPECT_EQ(DbErrorCode::UnsupportedFeature,invalid.error());
  static_assert(static_cast<unsigned>(ParameterProfile::ExistingOnly)==0);
  static_assert(static_cast<unsigned>(ParameterProfile::DateCandidate)==1);
  for (const auto type:{QueryParameterType::Timestamp,QueryParameterType::Time,QueryParameterType::Numeric,
      QueryParameterType::Float32,QueryParameterType::Float64}) {
    for (const bool nullable:{false,true}) {
      QueryParameter input{nullable?std::optional<std::string>{}:std::optional<std::string>{"2024-01-02 00:00:00"},type};
      for (const auto old:{ParameterProfile::ExistingOnly,ParameterProfile::DateCandidate}) {
        auto refused=execute_request(1,std::span(&input,1),InputLimits{},old);ASSERT_FALSE(refused);
        EXPECT_EQ(DbErrorCode::UnsupportedFeature,refused.error());
      }
      if (type!=QueryParameterType::Timestamp) {
        auto refused=request(input);ASSERT_FALSE(refused);EXPECT_EQ(DbErrorCode::UnsupportedFeature,refused.error());
      }
    }
  }
  QueryParameter date{"2024-02-29",QueryParameterType::Date};
  auto default_date=execute_request(1,std::span(&date,1),InputLimits{});ASSERT_FALSE(default_date);
  EXPECT_EQ(DbErrorCode::UnsupportedFeature,default_date.error());
  auto admitted_date=execute_request(1,std::span(&date,1),InputLimits{},ParameterProfile::DateCandidate);ASSERT_TRUE(admitted_date);
  EXPECT_EQ(literal({19,0,0,0,23,1,0,0,0,0,1,0,0,0,0,1,10,0,4,0xe8,7,2,29}),*admitted_date);
}
TEST(MySqlDatetimeQ6ParameterPacketTest, ExactRawAggregateAndWireBudgetsAndFirstErrorArePreserved) {
  QueryParameter input{"2024-01-02 00:00:00",QueryParameterType::Timestamp};
  InputLimits exact;exact.max_parameters=1;exact.max_parameter_bytes=19;exact.max_parameter_total_bytes=19;exact.max_request_wire_bytes=23;
  auto fits=request(input,exact);ASSERT_TRUE(fits);EXPECT_EQ(23u,fits->packet.size());
  for (unsigned fault=0;fault<4;++fault) {
    auto limits=exact;
    if (fault==0) { limits.max_parameters=0; }
    if (fault==1) { limits.max_parameter_bytes=18; }
    if (fault==2) { limits.max_parameter_total_bytes=18; }
    if (fault==3) { limits.max_request_wire_bytes=22; }
    auto rejected=request(input,limits);ASSERT_FALSE(rejected);EXPECT_EQ(DbErrorCode::ResourceLimit,rejected.error());
  }
  for (const auto& value:std::array<std::pair<const char*,std::size_t>,2>{{{"2024-01-02 01:02:03",26},{"2024-01-02 01:02:03.000001",30}}}) {
    InputLimits limits;limits.max_request_wire_bytes=value.second;
    auto fit=request({value.first,QueryParameterType::Timestamp},limits);ASSERT_TRUE(fit);EXPECT_EQ(value.second,fit->packet.size());
    --limits.max_request_wire_bytes;auto rejected=request({value.first,QueryParameterType::Timestamp},limits);ASSERT_FALSE(rejected);
    EXPECT_EQ(DbErrorCode::ResourceLimit,rejected.error());
  }
  std::array<QueryParameter,2> two{{input,input}};InputLimits aggregate;aggregate.max_parameter_total_bytes=38;
  ASSERT_TRUE(execute_datetime_q6_candidate_request(1,two,aggregate,policy));
  aggregate.max_parameter_total_bytes=37;auto over=execute_datetime_q6_candidate_request(1,two,aggregate,policy);ASSERT_FALSE(over);
  EXPECT_EQ(DbErrorCode::ResourceLimit,over.error());
  two[0].value="bad";auto malformed=execute_datetime_q6_candidate_request(1,two,aggregate,policy);ASSERT_FALSE(malformed);
  EXPECT_EQ(DbErrorCode::InvalidParameter,malformed.error());
  two[0].value=std::string(100,'x');two[1].value="bad";aggregate.max_parameter_bytes=26;
  auto first_limit=execute_datetime_q6_candidate_request(1,two,aggregate,policy);ASSERT_FALSE(first_limit);
  EXPECT_EQ(DbErrorCode::ResourceLimit,first_limit.error());
  aggregate.max_parameters=0;auto count=execute_datetime_q6_candidate_request(1,two,aggregate,static_cast<DatetimeWriterPolicy>(99));ASSERT_FALSE(count);
  EXPECT_EQ(DbErrorCode::ResourceLimit,count.error());
}
TEST(MySqlDatetimeQ6ParameterPacketTest, NullZeroRawBudgetPrecisionSixRawCapAndNativeCountBoundsAreExact) {
  InputLimits null_limits;null_limits.max_parameter_bytes=0;null_limits.max_parameter_total_bytes=0;
  null_limits.max_request_wire_bytes=18;
  auto null=request({std::nullopt,QueryParameterType::Timestamp},null_limits);ASSERT_TRUE(null);EXPECT_EQ(18u,null->packet.size());
  null_limits.max_request_wire_bytes=17;auto too_small=request({std::nullopt,QueryParameterType::Timestamp},null_limits);ASSERT_FALSE(too_small);
  EXPECT_EQ(DbErrorCode::ResourceLimit,too_small.error());
  InputLimits lexical;lexical.max_parameter_bytes=26;lexical.max_parameter_total_bytes=26;
  auto exact=request({"2024-02-29 12:34:56.123456",QueryParameterType::Timestamp},lexical);ASSERT_TRUE(exact);
  lexical.max_parameter_bytes=25;auto short_raw=request({"2024-02-29 12:34:56.123456",QueryParameterType::Timestamp},lexical);ASSERT_FALSE(short_raw);
  EXPECT_EQ(DbErrorCode::ResourceLimit,short_raw.error());
  std::vector<QueryParameter> maximum(65535,QueryParameter{std::nullopt,QueryParameterType::Timestamp});
  // Native count is representable but the complete packet is too large.
  auto wire_limit=execute_datetime_q6_candidate_request(1,maximum,InputLimits{},policy);ASSERT_FALSE(wire_limit);
  EXPECT_EQ(DbErrorCode::ResourceLimit,wire_limit.error());
  maximum.emplace_back(std::nullopt,QueryParameterType::Timestamp);
  InputLimits larger_count;larger_count.max_parameters=65536;
  auto count_first=execute_datetime_q6_candidate_request(1,maximum,larger_count,static_cast<DatetimeWriterPolicy>(99));ASSERT_FALSE(count_first);
  EXPECT_EQ(DbErrorCode::ResourceLimit,count_first.error());
}
TEST(MySqlDatetimeQ6ParameterPacketTest, CallerHintBinaryAndScalarFailuresOccurBeforeFrameBudget) {
  QueryParameter bad{"2024-01-02 00:00:00",QueryParameterType::Timestamp,true};
  auto result=request(bad);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());
  bad.value.reset();result=request(bad);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());
  auto wrong=validate_datetime_parameter({std::nullopt,QueryParameterType::Text,true});ASSERT_FALSE(wrong);
  EXPECT_EQ(DbErrorCode::UnsupportedFeature,wrong.error());
  InputLimits limits;limits.max_request_wire_bytes=0;
  auto scalar=request({"32768",QueryParameterType::Int16},limits);ASSERT_FALSE(scalar);
  EXPECT_EQ(DbErrorCode::InvalidParameter,scalar.error());
  auto malformed=request({"2024-01-02 00:00:00.1234567",QueryParameterType::Timestamp},limits);ASSERT_FALSE(malformed);
  EXPECT_EQ(DbErrorCode::InvalidParameter,malformed.error());
  limits.max_parameter_bytes=0;auto raw_first=request({"bad",QueryParameterType::Timestamp},limits);ASSERT_FALSE(raw_first);
  EXPECT_EQ(DbErrorCode::ResourceLimit,raw_first.error());
}
TEST(MySqlDatetimeQ6ParameterPacketTest, TextLengthPrefixAndConnectionLimitRemainOrdinaryFraming) {
  for (const auto size:{250u,251u}) {
    auto output=request({std::string(size,'x'),QueryParameterType::Text});ASSERT_TRUE(output);
    Bytes expected=size==250?literal({9,1,0,0,23,4,3,2,1,0,1,0,0,0,0,1,253,0,250}):
        literal({12,1,0,0,23,4,3,2,1,0,1,0,0,0,0,1,253,0,252,251,0});
    expected.insert(expected.end(),size,std::byte{'x'});EXPECT_EQ(expected,output->packet);
  }
  InputLimits limits;limits.max_parameter_bytes=65520;limits.max_parameter_total_bytes=65520;limits.max_request_wire_bytes=65540;
  auto exact=request({std::string(65519,'x'),QueryParameterType::Text},limits);ASSERT_TRUE(exact);ASSERT_EQ(65540u,exact->packet.size());
  EXPECT_EQ(literal({0,0,1,0,23,4,3,2,1,0,1,0,0,0,0,1,253,0,252,0xef,0xff}),Bytes(exact->packet.begin(),exact->packet.begin()+21));
  auto over=request({std::string(65520,'x'),QueryParameterType::Text},limits);ASSERT_FALSE(over);EXPECT_EQ(DbErrorCode::ResourceLimit,over.error());
}
TEST(MySqlDatetimeQ6ParameterPacketTest, ReturnedPacketAndObservationsOwnInputsAndFreshStatementIds) {
  std::array<QueryParameter,2> inputs{{{"2024-02-29 12:34:56.123456",QueryParameterType::Timestamp},{"x",QueryParameterType::Text}}};
  auto first=execute_datetime_q6_candidate_request(0x01020304,inputs,InputLimits{},policy);ASSERT_TRUE(first);
  const auto expected=literal({30,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,253,0,11,0xe8,7,2,29,12,34,56,0x40,0xe2,1,0,1,'x'});
  EXPECT_EQ(expected,first->packet);
  auto fresh=execute_datetime_q6_candidate_request(9,inputs,InputLimits{},policy);ASSERT_TRUE(fresh);
  auto fresh_expected=expected;fresh_expected[5]=std::byte{9};fresh_expected[6]=fresh_expected[7]=fresh_expected[8]=std::byte{0};
  EXPECT_EQ(fresh_expected,fresh->packet);
  for (auto& input:inputs) { input.value="changed";input.type=QueryParameterType::Numeric; }
  EXPECT_EQ(expected,first->packet);ASSERT_TRUE(first->datetime_observations[0]);
  EXPECT_EQ(6u,first->datetime_observations[0]->caller_precision);EXPECT_EQ(123456u,first->datetime_observations[0]->micros);
  ASSERT_TRUE(first->datetime_observations[0]->encoded.value);
  EXPECT_EQ(literal({11,0xe8,7,2,29,12,34,56,0x40,0xe2,1,0}),*first->datetime_observations[0]->encoded.value);
  auto copy=*first;auto moved=std::move(copy);EXPECT_EQ(expected,moved.packet);
}
}
