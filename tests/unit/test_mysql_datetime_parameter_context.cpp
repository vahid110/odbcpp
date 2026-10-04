#include <gtest/gtest.h>
#include "core/database/mysql/query_wire.h"
#include <array>
#include <limits>

namespace {
using namespace rs::core::database;
namespace query=rs::core::database::mysql::query_detail;
using rs::util::DbErrorCode;
using Bytes=std::vector<std::byte>;
constexpr auto candidate=query::ColumnContext::DatetimeParameterQ6Candidate;
void number(Bytes& b,std::uint64_t value,unsigned width) {
  for (unsigned i=0;i<width;++i) { b.push_back(static_cast<std::byte>((value>>(8*i))&255u)); }
}
void text(Bytes& b,std::string_view value) {
  number(b,value.size(),1);for (const auto c:value) { b.push_back(static_cast<std::byte>(c)); }
}
// SYNTHETIC reconstruction using observed numeric12/45/104/6 only.
// These names/flags/packet bytes were not retained by the native diagnostic.
Bytes definition(unsigned type=12,std::uint32_t width=104,unsigned q=6,unsigned charset=45,
    unsigned flags=0,std::string_view name="p") {
  Bytes b;text(b,"def");text(b,"");text(b,"");text(b,"");text(b,name);text(b,"");
  number(b,12,1);number(b,charset,2);number(b,width,4);number(b,type,1);
  number(b,flags,2);number(b,q,1);number(b,0,2);return b;
}
auto decode(std::span<const std::byte> bytes,query::ColumnContext context=candidate) {
  return query::column(bytes,ResultLimits{},nullptr,nullptr,nullptr,nullptr,context);
}
void unchanged_failure(std::span<const std::byte> bytes,DbErrorCode code,
    query::ColumnContext context=candidate,ResultLimits limits={}) {
  std::size_t names=777;std::uint8_t native=99;bool unsigned_value=true;
  query::NativeParameterDescriptorObservation raw{9,123,4,63};
  auto result=query::column(bytes,limits,&names,&native,&unsigned_value,&raw,context);
  ASSERT_FALSE(result);EXPECT_EQ(code,result.error());EXPECT_EQ(777u,names);EXPECT_EQ(99u,native);
  EXPECT_TRUE(unsigned_value);EXPECT_EQ(9u,raw.type);EXPECT_EQ(123u,raw.width);
  EXPECT_EQ(4u,raw.decimals);EXPECT_EQ(63u,raw.charset);
}
TEST(MySqlDatetimeParameterContextTest, ExplicitQualifiedContextPublishesOwningSemanticAndRawFields) {
  for (const auto flags:{0u,32u,0x8001u,65535u}) {
    auto bytes=definition(12,104,6,45,flags,"owned");
    std::size_t names{};std::uint8_t native{};bool unsigned_value{};
    query::NativeParameterDescriptorObservation raw;
    auto result=query::column(bytes,ResultLimits{},&names,&native,&unsigned_value,&raw,candidate);ASSERT_TRUE(result);
    ASSERT_TRUE(result->normalized_type);const auto info=*result->normalized_type;
    EXPECT_TRUE(info.known);EXPECT_EQ(ScalarType::Timestamp,info.type);
    EXPECT_EQ(26u,info.column_size);EXPECT_EQ(6,info.decimal_digits);
    EXPECT_EQ(104u,raw.width);EXPECT_EQ(45u,raw.charset);EXPECT_EQ(6u,raw.decimals);EXPECT_EQ(12u,raw.type);
    EXPECT_EQ(12u,native);EXPECT_EQ((flags&32u)!=0,unsigned_value);EXPECT_EQ(8u,names);
    std::fill(bytes.begin(),bytes.end(),std::byte{255});bytes.clear();bytes.shrink_to_fit();
    EXPECT_EQ("owned",result->name);EXPECT_EQ(104u,raw.width);EXPECT_EQ(26u,result->normalized_type->column_size);
  }
}
TEST(MySqlDatetimeParameterContextTest, DefaultAndExplicitResultPreserveExistingRefusalAndSemanticResults) {
  const auto expanded=definition();auto ordinary=query::column(expanded,ResultLimits{});
  ASSERT_FALSE(ordinary);EXPECT_EQ(DbErrorCode::ProtocolError,ordinary.error());
  unchanged_failure(expanded,DbErrorCode::ProtocolError,query::ColumnContext::Result);
  for (const auto q:{0u,3u,6u}) {
    const auto width=19u+(q?1u+q:0u);const auto bytes=definition(12,width,q);
    auto implicit=query::column(bytes,ResultLimits{});auto explicit_result=decode(bytes,query::ColumnContext::Result);
    ASSERT_TRUE(implicit);ASSERT_TRUE(explicit_result);
    EXPECT_EQ(width,implicit->normalized_type->column_size);EXPECT_EQ(q,static_cast<unsigned>(implicit->normalized_type->decimal_digits));
    EXPECT_EQ(implicit->normalized_type->column_size,explicit_result->normalized_type->column_size);
    unchanged_failure(bytes,q==6?DbErrorCode::ProtocolError:DbErrorCode::UnsupportedFeature);
  }
}
TEST(MySqlDatetimeParameterContextTest, UnknownProfilesAndQualifiedWidthContradictionsHaveExactCategories) {
  for (unsigned q=0;q<=255;++q) {
    for (const auto charset:{0u,45u,46u,63u,255u,65535u}) {
      const auto bytes=definition(12,104,q,charset);
      if (q==6 && charset==45) { EXPECT_TRUE(decode(bytes)); }
      else { unchanged_failure(bytes,DbErrorCode::UnsupportedFeature); }
    }
  }
  for (const auto width:{0u,26u,76u,92u,103u,105u,std::numeric_limits<std::uint32_t>::max()}) {
    unchanged_failure(definition(12,width,6,45),DbErrorCode::ProtocolError);
  }
  unchanged_failure(definition(12,26,6,63),DbErrorCode::UnsupportedFeature);
  unchanged_failure(definition(12,76,0,45),DbErrorCode::UnsupportedFeature);
  unchanged_failure(definition(12,92,3,45),DbErrorCode::UnsupportedFeature);
}
TEST(MySqlDatetimeParameterContextTest, AllPrefixesAndMalformedTailKeepEveryOutputAtomicBeforePolicy) {
  for (const auto qualified:{true,false}) {
    const auto bytes=definition(12,104,qualified?6:3,qualified?45:0);
    for (std::size_t n=0;n<bytes.size();++n) { unchanged_failure(std::span(bytes).first(n),DbErrorCode::ProtocolError); }
    for (unsigned fault=0;fault<4;++fault) {
      auto bad=bytes;
      if (fault==0) { bad[bad.size()-13]=std::byte{11}; }
      else if (fault==1) { bad.back()=std::byte{1}; }
      else if (fault==2) { bad.push_back(std::byte{0}); }
      else { bad[1]=std::byte{'x'}; }
      unchanged_failure(bad,DbErrorCode::ProtocolError);
    }
    unchanged_failure(definition(12,104,qualified?6:3,qualified?45:0,0,std::string_view("a\0b",3)),DbErrorCode::ProtocolError);
    unchanged_failure(definition(12,104,qualified?6:3,qualified?45:0,0,std::string(1,static_cast<char>(255))),DbErrorCode::ProtocolError);
  }
}
TEST(MySqlDatetimeParameterContextTest, NameBudgetsAndInvalidContextRetainValidationPrecedence) {
  for (const auto qualified:{true,false}) {
    const auto bytes=definition(12,104,qualified?6:3,qualified?45:0,0,"long");
    auto limits=ResultLimits{};limits.max_metadata_name_bytes=6;
    unchanged_failure(bytes,DbErrorCode::ResourceLimit,candidate,limits);
    limits=ResultLimits{};limits.max_column_name_bytes=3;
    unchanged_failure(bytes,DbErrorCode::ResourceLimit,candidate,limits);
  }
  const auto invalid=static_cast<query::ColumnContext>(255);
  unchanged_failure(definition(),DbErrorCode::UnsupportedFeature,invalid);
  auto malformed=definition();malformed.back()=std::byte{1};
  unchanged_failure(malformed,DbErrorCode::ProtocolError,invalid);
}
TEST(MySqlDatetimeParameterContextTest, OtherNativeFamiliesRemainIdenticalAcrossContexts) {
  struct Case { unsigned type;std::uint32_t width;unsigned q,charset,flags; };
  const std::array cases{Case{1,3,0,63,0},Case{2,5,0,63,32},Case{3,10,0,63,0},Case{9,8,0,63,0},
      Case{8,19,0,63,0},Case{8,20,0,63,32},Case{10,40,0,45,0},Case{246,7,2,45,0},Case{246,6,2,45,32},
      Case{253,40,0,45,0},Case{254,40,0,45,0},Case{252,40,0,63,0},Case{6,0,0,63,0},
      Case{7,26,6,45,0},Case{11,17,6,45,0},Case{0,7,2,45,0},Case{4,12,0,63,0},Case{5,22,0,63,0},
      Case{246,1,30,45,0},Case{253,40,0,8,0}};
  for (const auto& c:cases) {
    const auto bytes=definition(c.type,c.width,c.q,c.charset,c.flags);
    auto ordinary=decode(bytes,query::ColumnContext::Result);auto contextual=decode(bytes);
    ASSERT_EQ(static_cast<bool>(ordinary),static_cast<bool>(contextual));
    if (!ordinary) { EXPECT_EQ(ordinary.error(),contextual.error()); }
    else {
      EXPECT_EQ(ordinary->name,contextual->name);EXPECT_EQ(ordinary->normalized_type->type,contextual->normalized_type->type);
      EXPECT_EQ(ordinary->normalized_type->column_size,contextual->normalized_type->column_size);
      EXPECT_EQ(ordinary->normalized_type->decimal_digits,contextual->normalized_type->decimal_digits);
    }
  }
}
}
