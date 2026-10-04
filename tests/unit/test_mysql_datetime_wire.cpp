#include <gtest/gtest.h>
#include "core/database/mysql/datetime_wire.h"
#include <limits>
#include <vector>

namespace {
namespace dt=rs::core::database::mysql::datetime_detail;
using Bytes=std::vector<std::byte>;
using rs::util::DbErrorCode;
Bytes field(unsigned year,unsigned month,unsigned day,
    std::optional<std::array<unsigned,3>> clock=std::nullopt,
    std::optional<std::uint32_t> micros=std::nullopt) {
  Bytes bytes{static_cast<std::byte>(micros?11:(clock?7:4)),static_cast<std::byte>(year&255),
      static_cast<std::byte>((year>>8)&255),static_cast<std::byte>(month),static_cast<std::byte>(day)};
  if (clock) for (auto value:*clock) bytes.push_back(static_cast<std::byte>(value));
  if (micros) for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::byte>((*micros>>(8*i))&255));
  return bytes;
}
auto decode(const Bytes& bytes,dt::Profile profile) {
  return dt::binary_cell(std::span<const std::byte>(bytes),profile);
}
TEST(MySqlDatetimeWireTest, MetadataRetainsKindAndBoundsPrecisionWithoutScalarMapping) {
  for (auto kind:{dt::Kind::Datetime,dt::Kind::Timestamp}) for (unsigned p=0;p<=6;++p) {
    const auto width=19+(p?1+p:0);auto result=dt::metadata(kind,width,p);
    ASSERT_TRUE(result);EXPECT_EQ(kind,result->kind);EXPECT_EQ(p,result->precision);
    for (auto bad:{std::uint64_t{0},std::uint64_t{width-1},std::uint64_t{width+1},std::numeric_limits<std::uint64_t>::max()}) {
      auto rejected=dt::metadata(kind,bad,p);ASSERT_FALSE(rejected);EXPECT_EQ(DbErrorCode::ProtocolError,rejected.error());
    }
  }
  for (auto p:{std::uint64_t{7},std::numeric_limits<std::uint64_t>::max()}) {
    auto result=dt::metadata(dt::Kind::Datetime,19,p);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
  }
  auto kind=dt::metadata(static_cast<dt::Kind>(77),19,0);ASSERT_FALSE(kind);
  EXPECT_EQ(DbErrorCode::UnsupportedFeature,kind.error());
}
TEST(MySqlDatetimeWireTest, RawCrossformsPreserveLeapCalendarAndExactDeclaredFractions) {
  constexpr std::array<unsigned,3> clock{12,34,56};
  for (auto kind:{dt::Kind::Datetime,dt::Kind::Timestamp}) for (unsigned p=0;p<=6;++p) {
    const auto fraction=123456u/dt::powers[6-p]*dt::powers[6-p];
    std::string expected="2024-02-29 12:34:56";
    if (p) expected+="."+std::string("123456").substr(0,p);
    auto wire=decode(field(2024,2,29,clock,fraction),{kind,p});ASSERT_TRUE(wire);ASSERT_TRUE(wire->value);
    EXPECT_EQ(expected,*wire->value);EXPECT_FALSE(wire->encoding_error);
    auto text=dt::text_cell(expected,{kind,p});ASSERT_TRUE(text);EXPECT_EQ(wire->value,text->value);EXPECT_FALSE(text->encoding_error);
    auto zero=decode(field(2024,2,29,clock),{kind,p});ASSERT_TRUE(zero);
    EXPECT_EQ(std::optional<std::string>{"2024-02-29 12:34:56"+(p?"."+std::string(p,'0'):"")},zero->value);
    EXPECT_FALSE(zero->encoding_error);
    for (const auto& bytes:{field(2024,2,29),field(2024,2,29,std::array<unsigned,3>{0,0,0}),
        field(2024,2,29,std::array<unsigned,3>{0,0,0},0)}) {
      auto midnight=decode(bytes,{kind,p});ASSERT_TRUE(midnight);EXPECT_FALSE(midnight->encoding_error);
      EXPECT_EQ(std::optional<std::string>{"2024-02-29 00:00:00"+(p?"."+std::string(p,'0'):"")},midnight->value);
    }
  }
}
TEST(MySqlDatetimeWireTest, CalendarBoundsAreComponentProofNotUtcOrNativeStorageClaims) {
  for (auto kind:{dt::Kind::Datetime,dt::Kind::Timestamp}) {
    // TIMESTAMP uses session-local fields. Neither epoch bounds nor the native
    // storage endpoint fraction is inferred from this pure codec success.
    for (const auto& c:{std::pair{field(1000,1,1),"1000-01-01 00:00:00.000000"},
        std::pair{field(9999,12,31,std::array<unsigned,3>{23,59,59},499999),"9999-12-31 23:59:59.499999"},
        std::pair{field(9999,12,31,std::array<unsigned,3>{23,59,59},999999),"9999-12-31 23:59:59.999999"},
        std::pair{field(1969,12,31,std::array<unsigned,3>{23,0,1}),"1969-12-31 23:00:01.000000"},
        std::pair{field(2038,1,19,std::array<unsigned,3>{17,14,7}),"2038-01-19 17:14:07.000000"},
        std::pair{field(2000,2,29),"2000-02-29 00:00:00.000000"}}) {
      auto wire=decode(c.first,{kind,6});ASSERT_TRUE(wire);EXPECT_FALSE(wire->encoding_error);EXPECT_EQ(c.second,*wire->value);
      auto text=dt::text_cell(std::string_view(c.second),{kind,6});ASSERT_TRUE(text);EXPECT_EQ(wire->value,text->value);
    }
  }
}
TEST(MySqlDatetimeWireTest, CompleteInvalidCalendarClockAndPrecisionAreNonNullCellErrors) {
  for (const auto& bytes:{Bytes{std::byte{0}},field(0,0,0),field(2024,0,1),field(2024,1,0),
      field(999,12,31),field(10000,1,1),field(65535,1,1),field(2024,13,1),field(2024,1,32),
      field(1900,2,29),field(2100,2,29),field(2023,2,29),field(2024,4,31),
      field(2024,1,1,std::array<unsigned,3>{24,0,0}),field(2024,1,1,std::array<unsigned,3>{0,60,0}),
      field(2024,1,1,std::array<unsigned,3>{0,0,60}),field(2024,1,1,std::array<unsigned,3>{0,0,0},1000000),
      field(2024,1,1,std::array<unsigned,3>{0,0,0},std::numeric_limits<std::uint32_t>::max())}) {
    auto result=decode(bytes,{dt::Kind::Datetime,6});ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);
  }
  for (unsigned p=0;p<6;++p) {
    auto result=decode(field(2024,1,1,std::array<unsigned,3>{1,2,3},123456),{dt::Kind::Timestamp,p});
    ASSERT_TRUE(result);ASSERT_TRUE(result->value);EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);
  }
}
TEST(MySqlDatetimeWireTest, TextRejectsRelaxationZeroDatesInvalidCalendarsAndPrecisionMismatch) {
  for (const auto text:{"","0000-00-00 00:00:00","2024-00-01 00:00:00","2024-01-00 00:00:00",
      "0999-12-31 23:59:59","1900-02-29 00:00:00","2100-02-29 00:00:00","2024-04-31 00:00:00",
      "2024-01-01 24:00:00","2024-01-01 00:60:00","2024-01-01 00:00:60","2024-1-01 00:00:00",
      "2024-01-01T00:00:00","2024-01-01 00:00:00Z","2024-01-01 00:00:00+00:00",
      " 2024-01-01 00:00:00","2024-01-01 00:00:00 ","2024-01-01 00:00:00.0"}) {
    SCOPED_TRACE(text);auto result=dt::text_cell(std::string_view(text),{dt::Kind::Datetime,0});ASSERT_TRUE(result);
    ASSERT_TRUE(result->value);EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);
  }
  for (const auto text:{std::string_view("2024-01-01 00:00:00"),std::string_view("2024-01-01 00:00:00.12345"),
      std::string_view("2024-01-01 00:00:00.1234567"),std::string_view("2024-01-01 00:00:00.12345x"),
      std::string_view("2024-01-01 00:00:00.12345\0",26),std::string_view("2024-01-01 00:00:00.12345\xff",26)}) {
    auto result=dt::text_cell(text,{dt::Kind::Timestamp,6});ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);
  }
}
TEST(MySqlDatetimeWireTest, AllPrefixesExtraBytesAndUnknownLengthsAreFramingFailures) {
  for (const auto& bytes:{Bytes{std::byte{0}},field(2024,2,29),field(2024,2,29,std::array<unsigned,3>{1,2,3}),
      field(2024,2,29,std::array<unsigned,3>{1,2,3},123456)}) {
    for (std::size_t n=0;n<bytes.size();++n) {
      auto result=dt::binary_cell(std::span<const std::byte>(bytes).first(n),{dt::Kind::Datetime,6});ASSERT_FALSE(result);
      EXPECT_EQ(DbErrorCode::ProtocolError,result.error());EXPECT_TRUE(result.error_message().empty());
    }
    auto extra=bytes;extra.push_back(std::byte{0});auto result=decode(extra,{dt::Kind::Timestamp,6});ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  }
  for (unsigned length:{1u,3u,5u,8u,10u,12u,251u,255u}) {
    Bytes bytes(length+1,std::byte{0});bytes[0]=static_cast<std::byte>(length);
    auto result=decode(bytes,{dt::Kind::Datetime,0});ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  }
}
TEST(MySqlDatetimeWireTest, NullIsExplicitProfilesCheckedBeforeNullAndValuesOwnTheirBytes) {
  for (auto kind:{dt::Kind::Datetime,dt::Kind::Timestamp}) {
    auto text=dt::text_cell(std::nullopt,{kind,6});ASSERT_TRUE(text);EXPECT_FALSE(text->value);EXPECT_FALSE(text->encoding_error);
    auto binary=dt::binary_cell(std::nullopt,{kind,6});ASSERT_TRUE(binary);EXPECT_FALSE(binary->value);EXPECT_FALSE(binary->encoding_error);
  }
  for (auto profile:{dt::Profile{dt::Kind::Datetime,7},dt::Profile{static_cast<dt::Kind>(77),0}}) {
    for (const auto& result:{dt::text_cell(std::nullopt,profile),dt::binary_cell(std::nullopt,profile),
        dt::text_cell("2024-01-01 00:00:00",profile),decode(Bytes{std::byte{0}},profile)}) {
      ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());EXPECT_TRUE(result.error_message().empty());
    }
  }
  std::string input="2024-02-29 12:34:56.123456";auto text=dt::text_cell(input,{dt::Kind::Datetime,6});input.assign(input.size(),'x');
  ASSERT_TRUE(text);EXPECT_EQ(std::optional<std::string>{"2024-02-29 12:34:56.123456"},text->value);
  auto bytes=field(2024,2,29,std::array<unsigned,3>{12,34,56},123456);auto wire=decode(bytes,{dt::Kind::Timestamp,6});
  bytes.clear();bytes.shrink_to_fit();ASSERT_TRUE(wire);EXPECT_EQ(text->value,wire->value);
}
}
