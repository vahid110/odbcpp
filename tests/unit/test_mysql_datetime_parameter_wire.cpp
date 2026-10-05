#include "core/database/mysql/datetime_parameter_wire.h"
#include <gtest/gtest.h>
#include <initializer_list>

namespace {
using namespace rs::core::database::mysql::datetime_parameter_detail;
using rs::util::DbErrorCode;
using Bytes=std::vector<std::byte>;
Bytes literal(std::initializer_list<unsigned> values) {
  Bytes out;
  for (const auto value:values) { out.push_back(static_cast<std::byte>(value)); }
  return out;
}
TEST(MySqlDatetimeParameterWireTest, RawFormsHaveIndependentCalendarAndMicrosecondLiterals) {
  struct Witness { const char* text;unsigned precision;Bytes bytes; };
  const std::array witnesses{
      Witness{"1000-01-01 00:00:00",0,literal({4,0xe8,3,1,1})},
      Witness{"2024-01-02 00:00:00",0,literal({4,0xe8,7,1,2})},
      Witness{"2024-02-29 12:34:56",0,literal({7,0xe8,7,2,29,12,34,56})},
      Witness{"2024-02-29 12:34:56.123",3,literal({11,0xe8,7,2,29,12,34,56,0x78,0xe0,1,0})},
      Witness{"2024-02-29 12:34:56.123456",6,literal({11,0xe8,7,2,29,12,34,56,0x40,0xe2,1,0})},
      Witness{"9999-12-31 23:59:59.999999",6,literal({11,0x0f,0x27,12,31,23,59,59,0x3f,0x42,0x0f,0})}};
  for (const auto& witness:witnesses) {
    SCOPED_TRACE(witness.text);
    auto result=encode(witness.text,witness.precision);ASSERT_TRUE(result);
    ASSERT_TRUE(result->value);EXPECT_EQ(witness.bytes,*result->value);
    EXPECT_EQ(12,Parameter::native_type);EXPECT_EQ(0,Parameter::unsigned_flag);
  }
}
TEST(MySqlDatetimeParameterWireTest, EveryPrecisionPreservesExactFractionAndZeroLengthChoice) {
  const std::array fractions{"1","12","123","1234","12345","123456"};
  const std::array<Bytes,6> suffixes{literal({0xa0,0x86,1,0}),literal({0xc0,0xd4,1,0}),
      literal({0x78,0xe0,1,0}),literal({8,0xe2,1,0}),literal({0x3a,0xe2,1,0}),literal({0x40,0xe2,1,0})};
  for (unsigned precision=1;precision<=6;++precision) {
    const auto input=std::string("2024-01-02 00:00:00.")+fractions[precision-1];
    auto result=encode(input,precision);ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    auto expected=literal({11,0xe8,7,1,2,0,0,0});
    expected.insert(expected.end(),suffixes[precision-1].begin(),suffixes[precision-1].end());
    EXPECT_EQ(expected,*result->value);
    const auto zero=std::string("2024-01-02 00:00:00.")+std::string(precision,'0');
    auto midnight=encode(zero,precision);ASSERT_TRUE(midnight);ASSERT_TRUE(midnight->value);
    EXPECT_EQ(literal({4,0xe8,7,1,2}),*midnight->value);
    const auto clock=std::string("2024-01-02 01:02:03.")+std::string(precision,'0');
    auto timed=encode(clock,precision);ASSERT_TRUE(timed);ASSERT_TRUE(timed->value);
    EXPECT_EQ(literal({7,0xe8,7,1,2,1,2,3}),*timed->value);
  }
}
TEST(MySqlDatetimeParameterWireTest, GregorianBoundsAndLeapRulesRejectInvalidBeforeBytes) {
  for (const auto text:{"0999-12-31 00:00:00","10000-01-01 00:00:00","0000-00-00 00:00:00",
      "1900-02-29 00:00:00","2100-02-29 00:00:00","2024-13-01 00:00:00",
      "2024-01-00 00:00:00","2024-04-31 00:00:00","2024-01-01 24:00:00",
      "2024-01-01 00:60:00","2024-01-01 00:00:60"}) {
    SCOPED_TRACE(text);auto invalid=encode(text,0);ASSERT_FALSE(invalid);
    EXPECT_EQ(DbErrorCode::InvalidParameter,invalid.error());
  }
  auto leap=encode("2000-02-29 00:00:00",0);ASSERT_TRUE(leap);ASSERT_TRUE(leap->value);
  EXPECT_EQ(literal({4,0xd0,7,2,29}),*leap->value);
}
TEST(MySqlDatetimeParameterWireTest, LexicalFramingAndPrecisionNeverTruncateOrCoerce) {
  for (const auto text:{"","2024-01-01","2024-01-01T00:00:00","2024-01-01 00:00:00Z",
      "+2024-01-01 00:00:00","2024-01-01 00:00:00 ","2024-01-01 00:00:00.1",
      "2024-01-01 00:00:00+00:00","2024-01-01 00:00:0a"}) {
    auto invalid=encode(text,0);ASSERT_FALSE(invalid);EXPECT_EQ(DbErrorCode::InvalidParameter,invalid.error());
  }
  for (unsigned precision=1;precision<=6;++precision) {
    auto missing=encode("2024-01-01 00:00:00",precision);ASSERT_FALSE(missing);
    EXPECT_EQ(DbErrorCode::InvalidParameter,missing.error());
  }
  std::string nul{"2024-01-01 00:00:00"};nul[11]='\0';
  auto invalid=encode(nul,0);ASSERT_FALSE(invalid);EXPECT_EQ(DbErrorCode::InvalidParameter,invalid.error());
  auto nonascii=encode("2024-01-01 00:00:00.é",2);ASSERT_FALSE(nonascii);
  EXPECT_EQ(DbErrorCode::InvalidParameter,nonascii.error());
}
TEST(MySqlDatetimeParameterWireTest, EveryTruncatedCanonicalPrefixRefusesWithoutOutput) {
  const std::string full="2024-02-29 12:34:56.123456";
  for (std::size_t size=0;size<full.size();++size) {
    SCOPED_TRACE(size);auto prefix=encode(std::string_view(full).substr(0,size),6);ASSERT_FALSE(prefix);
    EXPECT_EQ(DbErrorCode::InvalidParameter,prefix.error());
  }
  for (const char separator:{' ','.',':','-'}) {
    auto wrong=full;wrong[19]=separator;
    if (separator=='.') { continue; }
    auto malformed=encode(wrong,6);ASSERT_FALSE(malformed);EXPECT_EQ(DbErrorCode::InvalidParameter,malformed.error());
  }
}
TEST(MySqlDatetimeParameterWireTest, NullHasNoBytesAndUnsupportedPrecisionWinsBeforeNull) {
  auto null=encode(std::nullopt,0,0);ASSERT_TRUE(null);EXPECT_FALSE(null->value);
  auto invalid=encode(std::nullopt,7,0);ASSERT_FALSE(invalid);EXPECT_EQ(DbErrorCode::UnsupportedFeature,invalid.error());
  auto invalid_value=encode("bad",7,0);ASSERT_FALSE(invalid_value);
  EXPECT_EQ(DbErrorCode::UnsupportedFeature,invalid_value.error());
}
TEST(MySqlDatetimeParameterWireTest, EncodedBudgetsAreExactAndFollowCallerValidation) {
  struct Case { const char* text;unsigned precision;std::size_t size; };
  for (const auto& item:std::array{Case{"2024-01-01 00:00:00",0,5},
      Case{"2024-01-01 01:02:03",0,8},Case{"2024-01-01 01:02:03.000001",6,12}}) {
    auto exact=encode(item.text,item.precision,item.size);ASSERT_TRUE(exact);ASSERT_TRUE(exact->value);
    EXPECT_EQ(item.size,exact->value->size());
    auto short_budget=encode(item.text,item.precision,item.size-1);ASSERT_FALSE(short_budget);
    EXPECT_EQ(DbErrorCode::ResourceLimit,short_budget.error());
  }
  auto malformed=encode("2024-01-01 24:00:00",0,0);ASSERT_FALSE(malformed);
  EXPECT_EQ(DbErrorCode::InvalidParameter,malformed.error());
}
TEST(MySqlDatetimeParameterWireTest, OutputOwnsCalendarBytesAfterCallerMutationAndScopeExit) {
  auto result=[] {
    std::string input="2024-02-29 12:34:56.123456";
    auto encoded=encode(input,6);input.assign(input.size(),'x');return encoded;
  }();
  ASSERT_TRUE(result);ASSERT_TRUE(result->value);
  const auto expected=literal({11,0xe8,7,2,29,12,34,56,0x40,0xe2,1,0});
  EXPECT_EQ(expected,*result->value);
  auto copy=*result;auto moved=std::move(copy);ASSERT_TRUE(moved.value);EXPECT_EQ(expected,*moved.value);
}
}
