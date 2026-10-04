#include <gtest/gtest.h>
#include "core/database/mysql/query_wire.h"

namespace {
using namespace rs::core::database;
namespace query=rs::core::database::mysql::query_detail;
using rs::util::DbErrorCode;
using Bytes=std::vector<std::byte>;
constexpr query::NativeParameterDescriptorObservation sentinel{8,0x78563412,73};
void number(Bytes& bytes,std::uint64_t n,std::size_t width) {
  for (std::size_t i=0;i<width;++i) bytes.push_back(static_cast<std::byte>((n>>(8*i))&255));
}
Bytes definition(std::uint32_t width=10,std::uint8_t decimals=0,std::uint8_t type=10,std::string_view name="d") {
  Bytes bytes;
  for (const auto field:{std::string_view("def"),std::string_view{},std::string_view{},std::string_view{},name,std::string_view{}}) {
    bytes.push_back(static_cast<std::byte>(field.size()));
    for (const auto ch:field) bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  number(bytes,12,1);number(bytes,63,2);number(bytes,width,4);number(bytes,type,1);
  number(bytes,0,2);number(bytes,decimals,1);number(bytes,0,2);return bytes;
}
auto decode(const Bytes& bytes,query::NativeParameterDescriptorObservation& observation,const ResultLimits& limits=ResultLimits{}) {
  return query::column(bytes,limits,nullptr,nullptr,nullptr,&observation);
}
void expect_unchanged(const query::NativeParameterDescriptorObservation& observation) {
  EXPECT_EQ(sentinel.type,observation.type);EXPECT_EQ(sentinel.width,observation.width);EXPECT_EQ(sentinel.decimals,observation.decimals);
}
TEST(MySqlParameterDescriptorObservationTest, ExactDateObservationAndOwningNormalizedResultAgree) {
  // Independent complete ColumnDefinition41 literal: six names, fixed12 tail.
  Bytes bytes{std::byte{3},std::byte{'d'},std::byte{'e'},std::byte{'f'},std::byte{0},std::byte{0},std::byte{0},
      std::byte{1},std::byte{'d'},std::byte{0},std::byte{12},std::byte{63},std::byte{0},std::byte{10},
      std::byte{0},std::byte{0},std::byte{0},std::byte{10},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0}};
  auto observation=sentinel;auto result=decode(bytes,observation);ASSERT_TRUE(result);
  EXPECT_EQ(10u,observation.type);EXPECT_EQ(10u,observation.width);EXPECT_EQ(0u,observation.decimals);
  ASSERT_TRUE(result->normalized_type);EXPECT_TRUE(result->normalized_type->known);
  EXPECT_EQ(ScalarType::Date,result->normalized_type->type);EXPECT_EQ(10u,result->normalized_type->column_size);
  EXPECT_EQ(0,result->normalized_type->decimal_digits);EXPECT_EQ("d",result->name);
  bytes.assign(bytes.size(),std::byte{0});
  EXPECT_EQ("d",result->name);EXPECT_EQ(10u,observation.type);EXPECT_EQ(10u,observation.width);EXPECT_EQ(0u,observation.decimals);
  auto copy=observation;observation.width=1;EXPECT_EQ(10u,copy.width);
}
TEST(MySqlParameterDescriptorObservationTest, ContradictoryRawDateShapeIsRetainedWithoutChangingResultNormalization) {
  for (std::uint32_t width:{0u,9u,11u,0xffffffffu}) for (std::uint8_t decimals:{std::uint8_t{0},std::uint8_t{1},std::uint8_t{31},std::uint8_t{255}}) {
    SCOPED_TRACE(width);
    SCOPED_TRACE(unsigned(decimals));
    const auto bytes=definition(width,decimals);auto observation=sentinel;auto observed=decode(bytes,observation);ASSERT_TRUE(observed);
    auto ordinary=query::column(bytes,ResultLimits{});ASSERT_TRUE(ordinary);
    EXPECT_EQ(10u,observation.type);EXPECT_EQ(width,observation.width);EXPECT_EQ(decimals,observation.decimals);
    ASSERT_TRUE(observed->normalized_type);ASSERT_TRUE(ordinary->normalized_type);
    EXPECT_TRUE(observed->normalized_type->known);EXPECT_EQ(ScalarType::Date,observed->normalized_type->type);
    EXPECT_EQ(10u,observed->normalized_type->column_size);EXPECT_EQ(0,observed->normalized_type->decimal_digits);
    EXPECT_EQ(ordinary->normalized_type->type,observed->normalized_type->type);
    EXPECT_EQ(ordinary->normalized_type->column_size,observed->normalized_type->column_size);
    EXPECT_EQ(ordinary->normalized_type->decimal_digits,observed->normalized_type->decimal_digits);
  }
}
TEST(MySqlParameterDescriptorObservationTest, EveryTruncationAndMalformedDefinitionPreservesObservation) {
  const auto valid=definition();
  for (std::size_t n=0;n<valid.size();++n) {
    SCOPED_TRACE(n);auto observation=sentinel;
    auto result=query::column(std::span(valid).first(n),ResultLimits{},nullptr,nullptr,nullptr,&observation);
    ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());expect_unchanged(observation);
  }
  for (unsigned fault=0;fault<5;++fault) {
    auto bytes=valid;
    if (fault==0) bytes.push_back(std::byte{0});
    if (fault==1) bytes.back()=std::byte{1};
    if (fault==2) bytes[10]=std::byte{11};
    if (fault==3) bytes[1]=std::byte{'x'};
    if (fault==4) bytes[8]=std::byte{0xff};
    auto observation=sentinel;auto result=decode(bytes,observation);ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::ProtocolError,result.error());expect_unchanged(observation);
  }
}
TEST(MySqlParameterDescriptorObservationTest, LimitsAndUnsupportedNormalizationNeverPublishRawFields) {
  for (unsigned fault=0;fault<4;++fault) {
    SCOPED_TRACE(fault);auto bytes=definition();auto limits=ResultLimits{};
    if (fault==0) limits.max_metadata_name_bytes=3;
    if (fault==1) limits.max_column_name_bytes=0;
    if (fault==2) bytes=definition(19,0,7);
    if (fault==3) bytes=definition(3,30,246);
    auto observation=sentinel;auto result=decode(bytes,observation,limits);ASSERT_FALSE(result);
    EXPECT_EQ(fault<2?DbErrorCode::ResourceLimit:(fault==2?DbErrorCode::UnsupportedFeature:DbErrorCode::ProtocolError),result.error());
    expect_unchanged(observation);
  }
}
TEST(MySqlParameterDescriptorObservationTest, NullRowMarkerIsNotAColumnDefinitionOrParameterReceipt) {
  auto observation=sentinel;const Bytes null_row{std::byte{251}};auto result=decode(null_row,observation);ASSERT_FALSE(result);
  EXPECT_EQ(DbErrorCode::ProtocolError,result.error());expect_unchanged(observation);
  // Nullable result values do not erase the descriptor or change its raw type.
  auto metadata=decode(definition(),observation);ASSERT_TRUE(metadata);
  const std::vector<ResultColumnMetadata> columns{*metadata};const std::array<query::NativeColumn,1> native{{{10,false}}};
  std::vector<CellEncodingError> errors;auto row=query::row(null_row,columns,0,errors,native);ASSERT_TRUE(row);
  EXPECT_FALSE((*row)[0]);EXPECT_TRUE(errors.empty());EXPECT_EQ(10u,observation.type);EXPECT_EQ(10u,observation.width);EXPECT_EQ(0u,observation.decimals);
  // MYSQL_TYPE_NULL is an actual expression descriptor, distinct from a NULL
  // row marker. Observing it cannot manufacture a native DATE receipt.
  auto null_expression=decode(definition(0,0,6),observation);ASSERT_TRUE(null_expression);
  EXPECT_EQ(6u,observation.type);EXPECT_EQ(0u,observation.width);EXPECT_EQ(0u,observation.decimals);
  ASSERT_TRUE(null_expression->normalized_type);EXPECT_EQ(ScalarType::VarChar,null_expression->normalized_type->type);
}
}
