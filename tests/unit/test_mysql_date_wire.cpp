#include <gtest/gtest.h>
#include "core/database/mysql/date_wire.h"
#include "core/database/mysql/prepared_wire.h"
#include <vector>

namespace {
namespace date=rs::core::database::mysql::date_detail;
using Bytes=std::vector<std::byte>;
using rs::util::DbErrorCode;
Bytes binary(unsigned year,unsigned month,unsigned day) {
  return {std::byte{4},static_cast<std::byte>(year&255),static_cast<std::byte>(year>>8),
      static_cast<std::byte>(month),static_cast<std::byte>(day)};
}
auto decode(const Bytes& bytes) { return date::binary_cell(std::span<const std::byte>(bytes)); }
TEST(MySqlDateWireTest, GuaranteedBoundsAndGregorianLeapRulesAgreeAcrossForms) {
  struct Case { unsigned year,month,day;std::string_view text; };
  for (const auto c:{Case{1000,1,1,"1000-01-01"},Case{9999,12,31,"9999-12-31"},
      Case{2000,2,29,"2000-02-29"},Case{1900,2,28,"1900-02-28"},Case{2024,2,29,"2024-02-29"}}) {
    SCOPED_TRACE(c.text);
    EXPECT_TRUE(date::valid_cell(c.text));
    const auto text=date::text_cell(c.text);ASSERT_TRUE(text.value);
    EXPECT_EQ(c.text,*text.value);EXPECT_FALSE(text.encoding_error);
    auto result=decode(binary(c.year,c.month,c.day));ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_EQ(c.text,*result->value);EXPECT_FALSE(result->encoding_error);
  }
}
TEST(MySqlDateWireTest, CompleteInvalidDatesAreDeferredNonNullCells) {
  struct Case { unsigned year,month,day;std::string_view text; };
  for (const auto c:{Case{0,0,0,"0000-00-00"},Case{2024,0,1,"2024-00-01"},
      Case{2024,1,0,"2024-01-00"},Case{1900,2,29,"1900-02-29"},
      Case{2023,2,29,"2023-02-29"},Case{2024,4,31,"2024-04-31"},
      Case{999,12,31,"0999-12-31"},Case{10000,1,1,"10000-01-01"},
      Case{2024,13,1,"2024-13-01"},Case{2024,1,32,"2024-01-32"}}) {
    SCOPED_TRACE(c.text);
    EXPECT_FALSE(date::valid_cell(c.text));
    const auto text=date::text_cell(c.text);ASSERT_TRUE(text.value);
    EXPECT_TRUE(text.value->empty());EXPECT_TRUE(text.encoding_error);
    auto result=decode(binary(c.year,c.month,c.day));ASSERT_TRUE(result);ASSERT_TRUE(result->value);
    EXPECT_TRUE(result->value->empty());EXPECT_TRUE(result->encoding_error);
  }
  auto zero=decode(Bytes{std::byte{0}});ASSERT_TRUE(zero);ASSERT_TRUE(zero->value);
  EXPECT_TRUE(zero->value->empty());EXPECT_TRUE(zero->encoding_error);
}
TEST(MySqlDateWireTest, StrictTextShapeDoesNotAcceptInputLiteralRelaxations) {
  for (const auto text:{"","20240101","24-01-01","2024/01/01","2024-1-01",
      "+2024-01-01"," 2024-01-01","2024-01-01 ","2024-01-01 00:00:00","abcd-ef-gh"}) {
    SCOPED_TRACE(text);const auto result=date::text_cell(std::string_view(text));
    ASSERT_TRUE(result.value);EXPECT_TRUE(result.value->empty());EXPECT_TRUE(result.encoding_error);
  }
  for (const auto text:{std::string_view("2024-01-\0" "1",10),std::string_view("2024-01-\xff" "1",10)}) {
    const auto result=date::text_cell(text);ASSERT_TRUE(result.value);
    EXPECT_TRUE(result.value->empty());EXPECT_TRUE(result.encoding_error);
  }
}
TEST(MySqlDateWireTest, TruncatedExtraAndUnsupportedFramingNeverBecomeCellErrors) {
  for (unsigned length:{0u,4u,7u,11u}) {
    Bytes complete(length+1,std::byte{0});complete[0]=static_cast<std::byte>(length);
    for (std::size_t n=0;n<complete.size();++n) {
      auto result=date::binary_cell(std::span<const std::byte>(complete).first(n));
      ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());EXPECT_TRUE(result.error_message().empty());
    }
    if (length==7 || length==11) {
      auto result=decode(complete);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
    }
    complete.push_back(std::byte{0});auto extra=decode(complete);ASSERT_FALSE(extra);
    EXPECT_EQ(DbErrorCode::ProtocolError,extra.error());
  }
  for (unsigned length:{1u,2u,3u,5u,6u,8u,10u,12u,251u,255u}) {
    Bytes bytes(length+1,std::byte{0});bytes[0]=static_cast<std::byte>(length);
    auto result=decode(bytes);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  }
}
TEST(MySqlDateWireTest, NullIsExplicitAndStringsOwnSourceBytes) {
  const auto text_null=date::text_cell(std::nullopt);EXPECT_FALSE(text_null.value);EXPECT_FALSE(text_null.encoding_error);
  auto binary_null=date::binary_cell(std::nullopt);ASSERT_TRUE(binary_null);
  EXPECT_FALSE(binary_null->value);EXPECT_FALSE(binary_null->encoding_error);
  std::string input="2000-02-29";auto text=date::text_cell(input);input.assign(input.size(),'x');
  ASSERT_TRUE(text.value);EXPECT_EQ("2000-02-29",*text.value);
  auto bytes=binary(9999,12,31);auto result=decode(bytes);bytes.assign(bytes.size(),std::byte{0});
  ASSERT_TRUE(result);ASSERT_TRUE(result->value);EXPECT_EQ("9999-12-31",*result->value);
}
TEST(MySqlDateWireTest, DecodedDateMetadataChecksWholeDefinitionAndBinaryAffinityBeforeNull) {
  using namespace rs::core::database;
  using namespace rs::core::database::mysql;
  Bytes definition;
  for (const auto field:{"def","","","","date",""}) {
    definition.push_back(static_cast<std::byte>(std::string_view(field).size()));
    for (const auto ch:std::string_view(field)) definition.push_back(static_cast<std::byte>(ch));
  }
  const Bytes fixed{std::byte{12},std::byte{63},std::byte{0},std::byte{10},std::byte{0},std::byte{0},std::byte{0},
      std::byte{10},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0}};
  definition.insert(definition.end(),fixed.begin(),fixed.end());
  std::uint8_t type{};bool unsigned_value{};
  auto decoded=query_detail::column(definition,ResultLimits{},nullptr,&type,&unsigned_value);ASSERT_TRUE(decoded);
  ASSERT_TRUE(decoded->normalized_type);EXPECT_EQ(10u,type);EXPECT_FALSE(unsigned_value);
  EXPECT_EQ(ScalarType::Date,decoded->normalized_type->type);EXPECT_TRUE(decoded->normalized_type->known);
  EXPECT_EQ(10u,decoded->normalized_type->column_size);EXPECT_EQ(0,decoded->normalized_type->decimal_digits);
  for (std::size_t n=0;n<definition.size();++n) {
    auto truncated=query_detail::column(std::span(definition).first(n),ResultLimits{});ASSERT_FALSE(truncated);
    EXPECT_EQ(DbErrorCode::ProtocolError,truncated.error());
  }
  definition.push_back(std::byte{0});EXPECT_FALSE(query_detail::column(definition,ResultLimits{}));
  const std::array<prepared_detail::NativeColumn,1> native{{{10,false}}};
  const Bytes nullrow{std::byte{0},std::byte{4}};
  std::array<ResultColumnMetadata,1> columns{*decoded};std::vector<CellEncodingError> errors;
  auto valid=prepared_detail::binary_row(nullrow,native,columns,0,errors);ASSERT_TRUE(valid);EXPECT_FALSE((*valid)[0]);EXPECT_TRUE(errors.empty());
  columns[0].normalized_type=NativeTypeInfo{ScalarType::BigInt,19,0,true};
  auto mismatch=prepared_detail::binary_row(nullrow,native,columns,0,errors);ASSERT_FALSE(mismatch);
  EXPECT_EQ(DbErrorCode::ProtocolError,mismatch.error());
  // Reverse mismatch must fail before NULL or integer conversion, too.
  columns[0]=*decoded;
  const std::array<prepared_detail::NativeColumn,1> integer_native{{{8,false}}};
  const Bytes integer_row(10,std::byte{0});
  for (const auto& row:{nullrow,integer_row}) {
    errors.clear();
    auto reverse=prepared_detail::binary_row(row,integer_native,columns,0,errors);ASSERT_FALSE(reverse);
    EXPECT_EQ(DbErrorCode::ProtocolError,reverse.error());EXPECT_TRUE(errors.empty());
  }
}
TEST(MySqlDateWireTest, DirectDateAffinityIsRequiredBeforeNullOrCellValidation) {
  using namespace rs::core::database;
  using namespace rs::core::database::mysql;
  const Bytes nullrow{std::byte{251}};
  Bytes daterow{std::byte{10}};
  for (const auto ch:std::string_view("2000-02-29")) daterow.push_back(static_cast<std::byte>(ch));
  const std::array<query_detail::NativeColumn,1> native{{{10,false}}};
  const NativeTypeInfo exact{ScalarType::Date,10,0,true};
  std::vector<ResultColumnMetadata> columns{{"date",exact}};
  std::vector<CellEncodingError> errors;
  auto null=query_detail::row(nullrow,columns,0,errors,native);ASSERT_TRUE(null);EXPECT_FALSE((*null)[0]);
  auto value=query_detail::row(daterow,columns,0,errors,native);ASSERT_TRUE(value);
  EXPECT_EQ(std::optional<std::string>{"2000-02-29"},(*value)[0]);EXPECT_TRUE(errors.empty());
  for (unsigned fault=0;fault<7;++fault) {
    SCOPED_TRACE(fault);columns[0].normalized_type=exact;
    auto wire=native;
    if (fault==0) columns[0].normalized_type->known=false;
    if (fault==1) columns[0].normalized_type->column_size=9;
    if (fault==2) columns[0].normalized_type->decimal_digits=1;
    if (fault==3) columns[0].normalized_type=NativeTypeInfo{ScalarType::BigInt,19,0,true};
    if (fault==4) wire[0].type=8;
    if (fault==5) columns[0].normalized_type.reset();
    for (const auto& row:{nullrow,daterow}) {
      errors.clear();
      auto rejected=query_detail::row(row,columns,0,errors,
          fault==6?std::span<const query_detail::NativeColumn>{}:std::span<const query_detail::NativeColumn>(wire));
      ASSERT_FALSE(rejected);EXPECT_EQ(DbErrorCode::ProtocolError,rejected.error());EXPECT_TRUE(errors.empty());
    }
  }
}
}
