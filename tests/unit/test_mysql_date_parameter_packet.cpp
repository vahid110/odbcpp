#include <gtest/gtest.h>
#include "core/database/mysql/prepared_wire.h"
#include <initializer_list>

namespace {
using namespace rs::core::database;
using namespace rs::core::database::mysql::prepared_detail;
using rs::util::DbErrorCode;
using Bytes=std::vector<std::byte>;
constexpr auto candidate=ParameterProfile::DateCandidate;
Bytes literal(std::initializer_list<unsigned> values) {
  Bytes bytes;for (const auto value:values) bytes.push_back(static_cast<std::byte>(value));return bytes;
}
auto request(const QueryParameter& value,const InputLimits& limits=InputLimits{}) {
  return execute_request(0x78563412,std::span(&value,1),limits,candidate);
}
TEST(MySqlDateParameterPacketTest, CompleteDateAndTypedNullFramesHaveIndependentLiteralExpectations) {
  const QueryParameter date{"2000-02-29",QueryParameterType::Date};
  auto encoded=request(date);ASSERT_TRUE(encoded);
  EXPECT_EQ(literal({0x13,0,0,0,0x17,0x12,0x34,0x56,0x78,0,1,0,0,0,0,1,10,0,4,0xd0,7,2,29}),*encoded);
  const QueryParameter null{std::nullopt,QueryParameterType::Date};
  auto nullable=request(null);ASSERT_TRUE(nullable);
  EXPECT_EQ(literal({0x0e,0,0,0,0x17,0x12,0x34,0x56,0x78,0,1,0,0,0,1,1,10,0}),*nullable);
  EXPECT_EQ("2000-02-29",date.value);EXPECT_FALSE(null.value);
}
TEST(MySqlDateParameterPacketTest, NineMixedParametersCrossBitmapBoundaryWithoutShiftingValues) {
  std::array<QueryParameter,9> parameters{{
      {"2000-02-29",QueryParameterType::Date},{"-32768",QueryParameterType::Int16},
      {"2147483647",QueryParameterType::Int32},{"-9223372036854775808",QueryParameterType::Int64},
      {"1",QueryParameterType::Boolean},{std::string{"a\0b",3},QueryParameterType::Text},
      {std::string{"\0\xff",2},QueryParameterType::Binary,true},
      {std::nullopt,QueryParameterType::Date},{std::nullopt,QueryParameterType::Date}}};
  const auto expected=literal({0x3a,0,0,0,0x17,0x12,0x34,0x56,0x78,0,1,0,0,0,
      0x80,1,1,10,0,2,0,3,0,8,0,1,0,253,0,252,0,10,0,10,0,
      4,0xd0,7,2,29,0,0x80,0xff,0xff,0xff,0x7f,0,0,0,0,0,0,0,0x80,1,3,'a',0,'b',2,0,0xff});
  auto null_packet=execute_request(0x78563412,parameters,InputLimits{},candidate);ASSERT_TRUE(null_packet);
  ASSERT_EQ(62u,null_packet->size());EXPECT_EQ(expected,*null_packet);
  parameters[8].value="2024-02-29";
  const auto nonnull_expected=literal({0x3f,0,0,0,0x17,0x12,0x34,0x56,0x78,0,1,0,0,0,
      0x80,0,1,10,0,2,0,3,0,8,0,1,0,253,0,252,0,10,0,10,0,
      4,0xd0,7,2,29,0,0x80,0xff,0xff,0xff,0x7f,0,0,0,0,0,0,0,0x80,1,3,'a',0,'b',2,0,0xff,4,0xe8,7,2,29});
  auto nonnull_packet=execute_request(0x78563412,parameters,InputLimits{},candidate);ASSERT_TRUE(nonnull_packet);
  ASSERT_EQ(67u,nonnull_packet->size());EXPECT_EQ(nonnull_expected,*nonnull_packet);EXPECT_EQ(expected,*null_packet);
}
TEST(MySqlDateParameterPacketTest, RawAndWireBudgetsAccountForDateAndNullIndependently) {
  const QueryParameter date{"2000-02-29",QueryParameterType::Date},null{std::nullopt,QueryParameterType::Date};
  InputLimits exact;exact.max_parameters=1;exact.max_parameter_bytes=10;exact.max_parameter_total_bytes=10;exact.max_request_wire_bytes=23;
  auto fits=request(date,exact);ASSERT_TRUE(fits);EXPECT_EQ(23u,fits->size());
  for (unsigned fault=0;fault<4;++fault) {
    SCOPED_TRACE(fault);auto limits=exact;
    if (fault==0) limits.max_parameters=0;
    if (fault==1) limits.max_parameter_bytes=9;
    if (fault==2) limits.max_parameter_total_bytes=9;
    if (fault==3) limits.max_request_wire_bytes=22;
    auto rejected=request(date,limits);ASSERT_FALSE(rejected);EXPECT_EQ(DbErrorCode::ResourceLimit,rejected.error());
  }
  exact.max_parameter_bytes=0;exact.max_parameter_total_bytes=0;exact.max_request_wire_bytes=18;
  auto null_fits=request(null,exact);ASSERT_TRUE(null_fits);EXPECT_EQ(18u,null_fits->size());
  exact.max_request_wire_bytes=17;auto null_short=request(null,exact);ASSERT_FALSE(null_short);
  EXPECT_EQ(DbErrorCode::ResourceLimit,null_short.error());
  std::array<QueryParameter,2> two{{date,date}};exact.max_parameters=2;exact.max_parameter_bytes=10;exact.max_parameter_total_bytes=20;exact.max_request_wire_bytes=30;
  auto aggregate=execute_request(1,two,exact,candidate);ASSERT_TRUE(aggregate);EXPECT_EQ(30u,aggregate->size());
  exact.max_parameter_total_bytes=19;auto too_much=execute_request(1,two,exact,candidate);ASSERT_FALSE(too_much);
  EXPECT_EQ(DbErrorCode::ResourceLimit,too_much.error());
}
TEST(MySqlDateParameterPacketTest, PacketsOwnBytesAfterCallerMutationDestructionAndLaterEncoding) {
  auto first=[] {
    QueryParameter value{"2000-02-29",QueryParameterType::Date};auto encoded=request(value);
    value.value="changed";return encoded;
  }();
  ASSERT_TRUE(first);
  const QueryParameter other{"9999-12-31",QueryParameterType::Date};
  auto second=execute_request(0xffffffff,std::span(&other,1),InputLimits{},candidate);ASSERT_TRUE(second);
  const auto first_expected=literal({0x13,0,0,0,0x17,0x12,0x34,0x56,0x78,0,1,0,0,0,0,1,10,0,4,0xd0,7,2,29});
  const auto second_expected=literal({0x13,0,0,0,0x17,0xff,0xff,0xff,0xff,0,1,0,0,0,0,1,10,0,4,0x0f,0x27,12,31});
  EXPECT_EQ(first_expected,*first);EXPECT_EQ(second_expected,*second);
  first->assign(first->size(),std::byte{0});EXPECT_EQ(second_expected,*second);
}
TEST(MySqlDateParameterPacketTest, CandidateRejectsInvalidDatesAndBothProfilesPreserveOtherRefusals) {
  for (const auto value:{"","0000-00-00","2023-02-29","2024-2-29"}) {
    auto rejected=request(QueryParameter{value,QueryParameterType::Date});ASSERT_FALSE(rejected);
    EXPECT_EQ(DbErrorCode::InvalidParameter,rejected.error());EXPECT_TRUE(rejected.error_message().empty());
  }
  const std::array<QueryParameter,2> invalid{{
      {std::string{"2024-01-\0" "1",10},QueryParameterType::Date},
      {"2000-02-29",QueryParameterType::Date,true}}};
  for (const auto& parameter:invalid) {
    auto rejected=request(parameter);ASSERT_FALSE(rejected);
    EXPECT_EQ(DbErrorCode::InvalidParameter,rejected.error());
  }
  for (const auto& value:{std::optional<std::string>{"2000-02-29"},std::optional<std::string>{}}) {
    const std::array<QueryParameter,1> parameters{{{value,QueryParameterType::Date}}};
    for (bool explicit_profile:{false,true}) {
      auto refused=explicit_profile?execute_request(1,parameters,InputLimits{},ParameterProfile::ExistingOnly):execute_request(1,parameters,InputLimits{});
      ASSERT_FALSE(refused);EXPECT_EQ(DbErrorCode::UnsupportedFeature,refused.error());
    }
  }
  for (auto profile:{ParameterProfile::ExistingOnly,candidate}) for (auto type:{QueryParameterType::Float32,QueryParameterType::Float64,
      QueryParameterType::Numeric,QueryParameterType::Time,QueryParameterType::Timestamp}) {
    for (const auto& value:{std::optional<std::string>{"1"},std::optional<std::string>{}}) {
      const std::array<QueryParameter,1> parameters{{{value,type}}};auto refused=execute_request(1,parameters,InputLimits{},profile);
      ASSERT_FALSE(refused);EXPECT_EQ(DbErrorCode::UnsupportedFeature,refused.error());
    }
  }
}
}
