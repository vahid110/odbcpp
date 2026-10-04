#include <gtest/gtest.h>
#include "core/database/mysql/date_parameter_descriptor.h"

namespace {
using namespace rs::core::database;
namespace query=rs::core::database::mysql::query_detail;
namespace date=rs::core::database::mysql::date_parameter_detail;
using rs::util::DbErrorCode;
struct Receipt { query::NativeParameterDescriptorObservation raw;NativeTypeInfo normalized; };
auto read(std::uint8_t type=10,std::uint32_t width=10,std::uint8_t decimals=0) {
  // Complete ColumnDefinition41, independent of the guarded shape predicate.
  std::vector<std::byte> bytes{std::byte{3},std::byte{'d'},std::byte{'e'},std::byte{'f'},
      std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{'p'},std::byte{0},
      std::byte{12},std::byte{63},std::byte{0}};
  for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::byte>((width>>(8*i))&255));
  bytes.insert(bytes.end(),{static_cast<std::byte>(type),std::byte{0},std::byte{0},
      static_cast<std::byte>(decimals),std::byte{0},std::byte{0}});
  query::NativeParameterDescriptorObservation raw;
  auto metadata=query::column(bytes,ResultLimits{},nullptr,nullptr,nullptr,&raw);
  if (!metadata) return rs::util::Result<Receipt>{metadata.error()};
  return rs::util::Result<Receipt>{Receipt{raw,*metadata->normalized_type}};
}
TEST(MySqlDateParameterDescriptorTest, ActualExactReceiptAcceptsDateRegardlessOfValueOrNull) {
  auto receipt=read();ASSERT_TRUE(receipt);
  // Only the explicit hint is an input: this guard cannot inspect or coerce
  // values, including invalid values that the separate encoder must reject.
  for (const auto& value:{std::optional<std::string>{"1000-01-01"},std::optional<std::string>{"9999-12-31"},
      std::optional<std::string>{},std::optional<std::string>{"invalid"}}) {
    const QueryParameter caller{value,QueryParameterType::Date};
    EXPECT_TRUE(date::descriptor(caller.type,receipt->raw,receipt->normalized));
    EXPECT_EQ(value,caller.value);EXPECT_EQ(QueryParameterType::Date,caller.type);
  }
  EXPECT_EQ(10u,receipt->raw.type);EXPECT_EQ(10u,receipt->raw.width);EXPECT_EQ(0u,receipt->raw.decimals);
}
TEST(MySqlDateParameterDescriptorTest, EveryContradictoryRawDateShapeIsProtocolError) {
  for (const std::uint32_t width:{0u,9u,10u,11u,0xffffffffu})
    for (const std::uint8_t decimals:{std::uint8_t{0},std::uint8_t{1},std::uint8_t{31},std::uint8_t{255}}) {
      SCOPED_TRACE(width);
      SCOPED_TRACE(unsigned(decimals));auto receipt=read(10,width,decimals);ASSERT_TRUE(receipt);
      const auto result=date::descriptor(QueryParameterType::Date,receipt->raw,receipt->normalized);
      if (width==10 && decimals==0) EXPECT_TRUE(result);
      else {ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());}
      EXPECT_EQ(width,receipt->raw.width);EXPECT_EQ(decimals,receipt->raw.decimals);
      EXPECT_EQ(ScalarType::Date,receipt->normalized.type);EXPECT_EQ(10u,receipt->normalized.column_size);
    }
}
TEST(MySqlDateParameterDescriptorTest, AllNormalizedDateShapeAndReverseAffinityFaultsFailClosed) {
  auto receipt=read();ASSERT_TRUE(receipt);
  for (bool known:{false,true}) for (std::uint64_t width:{0u,9u,10u,11u})
    for (std::int16_t decimals:{std::int16_t{-1},std::int16_t{0},std::int16_t{1},std::int16_t{6}}) {
      const NativeTypeInfo normalized{ScalarType::Date,width,decimals,known};
      auto result=date::descriptor(QueryParameterType::Date,receipt->raw,normalized);
      if (known && width==10 && decimals==0) EXPECT_TRUE(result);
      else {ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());}
    }
  for (const auto type:{ScalarType::VarChar,ScalarType::BigInt,ScalarType::Timestamp,ScalarType::Decimal}) {
    auto altered=receipt->normalized;altered.type=type;
    auto result=date::descriptor(QueryParameterType::Date,receipt->raw,altered);
    ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  }
  // Reverse affinity: a Date normalized family cannot accompany native8.
  auto raw=receipt->raw;raw.type=8;
  auto result=date::descriptor(QueryParameterType::Date,raw,receipt->normalized);
  ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
}
TEST(MySqlDateParameterDescriptorTest, SupportedOtherReceiptsAndEveryOtherHintRemainUnsupported) {
  auto receipt=read();ASSERT_TRUE(receipt);
  const QueryParameter default_caller;
  auto default_result=date::descriptor(default_caller.type,receipt->raw,receipt->normalized);
  ASSERT_FALSE(default_result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,default_result.error());
  for (auto hint:{QueryParameterType::Unspecified,QueryParameterType::Text,QueryParameterType::Int16,
      QueryParameterType::Int32,QueryParameterType::Int64,QueryParameterType::Float32,QueryParameterType::Float64,
      QueryParameterType::Numeric,QueryParameterType::Boolean,QueryParameterType::Binary,
      QueryParameterType::Time,QueryParameterType::Timestamp}) {
    auto result=date::descriptor(hint,receipt->raw,receipt->normalized);
    ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
  }
  for (auto type:{std::uint8_t{1},std::uint8_t{2},std::uint8_t{3},std::uint8_t{8},std::uint8_t{9},
      std::uint8_t{6},std::uint8_t{253}}) {
    auto other=read(type);ASSERT_TRUE(other);
    auto result=date::descriptor(QueryParameterType::Date,other->raw,other->normalized);
    ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
  }
}
TEST(MySqlDateParameterDescriptorTest, ObservationCopiesOwnFieldsAndGuardNeverMutatesInputs) {
  auto source=read();ASSERT_TRUE(source);const auto owned=*source;
  source->raw.width=9;source->normalized.type=ScalarType::BigInt;
  EXPECT_TRUE(date::descriptor(QueryParameterType::Date,owned.raw,owned.normalized));
  EXPECT_EQ(10u,owned.raw.type);EXPECT_EQ(10u,owned.raw.width);EXPECT_EQ(0u,owned.raw.decimals);
  EXPECT_TRUE(owned.normalized.known);EXPECT_EQ(ScalarType::Date,owned.normalized.type);
  EXPECT_EQ(10u,owned.normalized.column_size);EXPECT_EQ(0,owned.normalized.decimal_digits);
  auto rejected=date::descriptor(QueryParameterType::Binary,owned.raw,owned.normalized);
  ASSERT_FALSE(rejected);EXPECT_EQ(DbErrorCode::UnsupportedFeature,rejected.error());
  EXPECT_TRUE(date::descriptor(QueryParameterType::Date,owned.raw,owned.normalized));
}
}
