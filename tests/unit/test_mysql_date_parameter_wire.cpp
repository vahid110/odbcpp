#include <gtest/gtest.h>
#include "core/database/mysql/date_parameter_wire.h"
#include "core/database/mysql/prepared_wire.h"
#include <string>

namespace {
namespace date=rs::core::database::mysql::date_parameter_detail;
using rs::util::DbErrorCode;
using Bytes=std::array<std::byte,5>;

TEST(MySqlDateParameterWireTest, BoundsAndLeapDatesEncodeExactIndependentCalendarBytes) {
  struct Case { std::string_view text;Bytes expected; };
  const Case cases[]{
      {"1000-01-01",{std::byte{4},std::byte{0xe8},std::byte{0x03},std::byte{1},std::byte{1}}},
      {"9999-12-31",{std::byte{4},std::byte{0x0f},std::byte{0x27},std::byte{12},std::byte{31}}},
      {"2000-02-29",{std::byte{4},std::byte{0xd0},std::byte{0x07},std::byte{2},std::byte{29}}},
      {"1900-02-28",{std::byte{4},std::byte{0x6c},std::byte{0x07},std::byte{2},std::byte{28}}},
      {"2024-02-29",{std::byte{4},std::byte{0xe8},std::byte{0x07},std::byte{2},std::byte{29}}},
      {"2100-03-01",{std::byte{4},std::byte{0x34},std::byte{0x08},std::byte{3},std::byte{1}}},
  };
  for (const auto& c:cases) {
    SCOPED_TRACE(c.text);auto result=date::encode(c.text);ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_EQ(c.expected,*result->value);EXPECT_EQ(10u,result->native_type);EXPECT_EQ(0u,result->unsigned_flag);
  }
}
TEST(MySqlDateParameterWireTest, InvalidCalendarAndLiteralRelaxationsProduceOnlyInvalidParameter) {
  for (const auto text:{"","0000-00-00","2024-00-01","2024-01-00","0999-12-31","10000-01-01",
      "1900-02-29","2100-02-29","2023-02-29","2024-04-31","2024-13-01","2024-01-32",
      "20240101","24-01-01","2024/01/01","2024-1-01","+2024-01-01"," 2024-01-01","2024-01-01 ",
      "2024-01-01 00:00:00","2024-01-01.000000","2024-01-01+00:00","abcd-ef-gh"}) {
    SCOPED_TRACE(text);auto result=date::encode(std::string_view(text));ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());EXPECT_TRUE(result.error_message().empty());
  }
  for (const auto text:{std::string_view("2024-01-\0" "1",10),std::string_view("2024-01-\xff" "1",10)}) {
    auto result=date::encode(text);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());
    EXPECT_TRUE(result.error_message().empty());
  }
}
TEST(MySqlDateParameterWireTest, EveryIncompletePrefixAndExtraByteIsInvalidWithoutBorrowedOutput) {
  const std::string input="2000-02-29";
  for (std::size_t n=0;n<input.size();++n) {
    SCOPED_TRACE(n);auto result=date::encode(std::string_view(input).substr(0,n));ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());
  }
  for (char extra:{'0',' ',char{0}}) {
    auto result=date::encode(input+extra);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());
  }
  EXPECT_EQ("2000-02-29",input);
}
TEST(MySqlDateParameterWireTest, NullRetainsDateTypeAndOmitsValuesRatherThanEncodingZeroDate) {
  auto null=date::encode(std::nullopt);ASSERT_TRUE(null);EXPECT_FALSE(null->value);
  EXPECT_EQ(10u,null->native_type);EXPECT_EQ(0u,null->unsigned_flag);
  auto empty=date::encode(std::string_view{});ASSERT_FALSE(empty);EXPECT_EQ(DbErrorCode::InvalidParameter,empty.error());
  auto zero=date::encode("0000-00-00");ASSERT_FALSE(zero);EXPECT_EQ(DbErrorCode::InvalidParameter,zero.error());
}
TEST(MySqlDateParameterWireTest, BytesOwnTheirStorageAfterInputMutationAndDestruction) {
  auto result=[] {
    std::string input="2000-02-29";auto encoded=date::encode(input);
    input.assign(input.size(),'x');return encoded;
  }();
  ASSERT_TRUE(result);ASSERT_TRUE(result->value);
  const Bytes expected{std::byte{4},std::byte{0xd0},std::byte{7},std::byte{2},std::byte{29}};
  EXPECT_EQ(expected,*result->value);
  auto copy=*result;(*result->value)[1]=std::byte{0};ASSERT_TRUE(copy.value);EXPECT_EQ(expected,*copy.value);
}
TEST(MySqlDateParameterWireTest, PrivateEncoderDoesNotEnablePreparedDateParametersIncludingNull) {
  using namespace rs::core::database;
  for (const auto& value:{std::optional<std::string>{"2000-02-29"},std::optional<std::string>{}}) {
    const std::array<QueryParameter,1> parameters{{{value,QueryParameterType::Date}}};
    auto result=mysql::prepared_detail::execute_request(1,parameters,InputLimits{});ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
  }
}
}
