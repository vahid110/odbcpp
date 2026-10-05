#include "odbcpp/auth/aws_db_json_response.h"
#include <gtest/gtest.h>
#include <chrono>
#include <string>
using namespace rs::core::auth;
namespace {
using namespace std::chrono_literals;
struct Clock final : MonotonicClock {
  rs::util::Deadline value{1s}, late{10s}; int calls{0}, late_at{0};
  rs::util::Deadline now() noexcept override { if(++calls==late_at)value=late;return value; }
};
struct Stop final : Cancellation { mutable int calls{0};int at{0};bool stop_requested() const noexcept override{return ++calls==at;} };
ResponseBytes raw(std::string_view text) {
  Clock clock;auto made=StreamOwner::create(StreamOwner::max_bytes,rs::util::Deadline(10s),clock,nullptr);
  auto owner=std::move(std::get<StreamOwner>(made));owner.io()->write(text.data(),static_cast<std::streamsize>(text.size()));
  return std::move(std::get<ResponseBytes>(seal_response(std::move(owner))));
}
JsonOutcome<ResponseSnapshot> parse(std::string_view text,Clock& c,const Cancellation* stop=nullptr) {
  auto b=raw(text);return parse_aws_db_json_response(DbCredentialOperation::ServerlessGetCredentials,
      ResponseShape::ServerlessObject,std::move(b),rs::util::Deadline(10s),rs::util::Deadline(1s),c,stop);
}
constexpr std::string_view valid=R"({"dbUser":"IAM:user","dbPassword":"secret","expiration":1.000001})";
TEST(AwsDbJsonResponse, ExactMicrosecondsOwningCompositionAndOrder) {
  Clock c;auto result=parse(valid,c);ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(result));
  auto& snapshot=std::get<ResponseSnapshot>(result);
  ASSERT_EQ(snapshot.occurrences().size(),3u);EXPECT_EQ(snapshot.occurrences()[0].key(),"dbUser");
  EXPECT_EQ(std::get<NumberLexeme>(snapshot.occurrences()[2].atom()).bytes,"1.000001");
  auto extracted=extract_db_fields(DbCredentialOperation::ServerlessGetCredentials,std::move(snapshot));ASSERT_TRUE(extracted);
  EXPECT_EQ(extracted.value().user,"IAM:user");EXPECT_EQ(extracted.value().expiry.microseconds_since_epoch,1000001);
  extracted.value().password.with_bytes([](auto b){EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data()),b.size()),"secret");});
}
TEST(AwsDbJsonResponse, DuplicateEscapedAliasesAndUnknownRemainVisible) {
  for(auto [text,expected]:{std::pair{std::string_view(R"({"expiration":1,"expir\u0061tion":2})"),FieldFailure::DuplicateField},
                           std::pair{std::string_view(R"({"unknown":"secret"})"),FieldFailure::UnknownField}}) {
    Clock c;auto r=parse(text,c);ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));
    auto f=extract_db_fields(DbCredentialOperation::ServerlessGetCredentials,std::move(std::get<ResponseSnapshot>(r)));
    ASSERT_FALSE(f);EXPECT_EQ(f.error().failure,expected);
  }
}
TEST(AwsDbJsonResponse, WrongTypesNotCoercedAndNestedSyntaxFullyValidated) {
  for(auto value:{"null","true","false","1","{}","[]"}) {
    Clock c;auto r=parse(std::string(R"({"dbUser":"u","expiration":1,"dbPassword":)")+value+"}",c);ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));
    EXPECT_FALSE(std::holds_alternative<TextBytes>(std::get<ResponseSnapshot>(r).occurrences()[2].atom()));
    auto e=extract_db_fields(DbCredentialOperation::ServerlessGetCredentials,std::move(std::get<ResponseSnapshot>(r)));
    ASSERT_FALSE(e);EXPECT_EQ(e.error().failure,FieldFailure::WrongType);
  }
  for(auto text:{R"({"unknown":[1,]})",R"({"dbPassword":{"x":tru}})",R"({"x":[{"a":"\uD800"}]})"}) {
    Clock c;auto r=parse(text,c);EXPECT_TRUE(std::holds_alternative<JsonError>(r));
  }
}
TEST(AwsDbJsonResponse, ExactNumericSpellingWithoutFloatConversion) {
  for(auto number:{"-0","0","1e+2","1E-2","1.000001","1e999999"}) {
    Clock c;auto r=parse(std::string(R"({"expiration":)")+number+"}",c);ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));
    EXPECT_EQ(std::get<NumberLexeme>(std::get<ResponseSnapshot>(r).occurrences()[0].atom()).bytes,number);
  }
  for(auto number:{"01","+1","1.",".1","1e","1e+","NaN","Infinity","0x1"}) {
    Clock c;EXPECT_TRUE(std::holds_alternative<JsonError>(parse(std::string(R"({"expiration":)")+number+"}",c)));
  }
}
TEST(AwsDbJsonResponse, CompleteInputObjectGrammar) {
  for(auto text:{"[]","null","{}{}","{}x",R"({"x":1,})",R"({/*x*/})","\xEF\xBB\xBF{}",R"({"x":"a
b"})"}) {
    Clock c;EXPECT_TRUE(std::holds_alternative<JsonError>(parse(text,c)));
  }
  Clock c;EXPECT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(" \r\n\t{} \t",c)));
}
TEST(AwsDbJsonResponse, Utf8EscapesAndSurrogateScalarEquivalence) {
  for(auto spelling:{R"(\uD83D\uDE00)","\xF0\x9F\x98\x80"}) {
    Clock c;auto r=parse(std::string(R"({"dbUser":")")+spelling+R"("})",c);ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));
    std::get<TextBytes>(std::get<ResponseSnapshot>(r).occurrences()[0].atom()).bytes.with_bytes([](auto b){EXPECT_EQ(b.size(),4u);});
  }
  for(auto spelling:{R"(\uD800)",R"(\uDC00)",R"(\uD800\u0041)",R"(\uZZZZ)",R"(\q)","\xC0\xAF","\xED\xA0\x80","\xF4\x90\x80\x80","\xE2\x82"}) {
    Clock c;EXPECT_TRUE(std::holds_alternative<JsonError>(parse(std::string(R"({"dbUser":")")+spelling+R"("})",c)));
  }
}
TEST(AwsDbJsonResponse, EscapedNullAndControlsRetainedForSemanticRefusal) {
  Clock c;auto r=parse(R"({"dbUser":"u","dbPassword":"a\u0000b","expiration":1})",c);
  ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));
  auto e=extract_db_fields(DbCredentialOperation::ServerlessGetCredentials,std::move(std::get<ResponseSnapshot>(r)));ASSERT_FALSE(e);
  EXPECT_EQ(e.error().failure,FieldFailure::InvalidPassword);
}
TEST(AwsDbJsonResponse, ExactAndOverStringKeyNumberFieldAndDepthBounds) {
  Clock c;
  auto text=std::string(R"({"x":")")+std::string(65536,'a')+R"("})";
  EXPECT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(text,c)));
  text=std::string(R"({"x":")")+std::string(65537,'a')+R"("})";
  EXPECT_EQ(std::get<JsonError>(parse(text,c)).failure,JsonFailure::ResourceLimit);
  for(auto n:{64u,65u}) {
    auto key=std::string("{\"")+std::string(n,'k')+"\":0}";
    auto r=parse(key,c);EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(r),n==64);
    auto number=std::string(R"({"x":)")+std::string(n,'1')+"}";
    auto nr=parse(number,c);EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(nr),n==64);
  }
  for(auto n:{8u,9u}) {
    std::string fields="{";for(unsigned i=0;i<n;++i){if(i)fields+=',';fields+="\"k"+std::to_string(i)+"\":0";}fields+='}';
    EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(parse(fields,c)),n==8);
  }
  for(auto depth:{7u,8u}) {
    std::string nest=R"({"x":)"+std::string(depth,'[')+"0"+std::string(depth,']')+"}";
    EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(parse(nest,c)),depth==7);
  }
}
TEST(AwsDbJsonResponse, NestedNodeAndAggregateBudgetsBoundDiscardedValues) {
  Clock c;std::string nodes=R"({"x":[)";for(int i=0;i<127;++i){if(i)nodes+=',';nodes+='0';}nodes+="]}";
  EXPECT_EQ(std::get<JsonError>(parse(nodes,c)).failure,JsonFailure::ResourceLimit);
  std::string text=std::string(R"({"x":[")")+std::string(40000,'a')+R"(",")"+std::string(40000,'b')+R"("]})";
  EXPECT_EQ(std::get<JsonError>(parse(text,c)).failure,JsonFailure::ResourceLimit);
}
TEST(AwsDbJsonResponse, UnsupportedShapesConsumeAndMovedFromBodyRefuses) {
  Clock c;for(auto op:{DbCredentialOperation::ClusterGetCredentials,DbCredentialOperation::ClusterGetCredentialsWithIam,static_cast<DbCredentialOperation>(99)}) {
    auto b=raw(valid);auto r=parse_aws_db_json_response(op,ResponseShape::ServerlessObject,std::move(b),rs::util::Deadline(10s),rs::util::Deadline(1s),c);
    EXPECT_EQ(std::get<JsonError>(r).failure,JsonFailure::UnsupportedOperation);EXPECT_THROW(b.with_bytes([](auto){}),std::logic_error);
  }
  auto b=raw(valid);auto moved=std::move(b);
  auto r=parse_aws_db_json_response(DbCredentialOperation::ServerlessGetCredentials,ResponseShape::ServerlessObject,std::move(b),rs::util::Deadline(10s),rs::util::Deadline(1s),c);
  EXPECT_EQ(std::get<JsonError>(r).failure,JsonFailure::InvalidBody);
  auto s=parse_aws_db_json_response(DbCredentialOperation::ServerlessGetCredentials,ResponseShape::ClusterResult,std::move(moved),rs::util::Deadline(10s),rs::util::Deadline(1s),c);
  EXPECT_EQ(std::get<JsonError>(s).failure,JsonFailure::InvalidShape);
}
TEST(AwsDbJsonResponse, CancellationDeadlineRollbackAndSafeErrors) {
  for(int at:{1,3,8}) {
    Clock c;Stop stop;stop.at=at;auto r=parse(std::string(R"({"dbPassword":")")+std::string(1000,'s')+R"("})",c,&stop);
    ASSERT_TRUE(std::holds_alternative<JsonError>(r));EXPECT_EQ(std::get<JsonError>(r).failure,JsonFailure::Cancelled);
    EXPECT_EQ(std::get<JsonError>(r).safe_message().find("ssss"),std::string_view::npos);
  }
  Clock late;late.value=rs::util::Deadline(10s);EXPECT_EQ(std::get<JsonError>(parse(valid,late)).failure,JsonFailure::DeadlineElapsed);
  Clock rollback;rollback.value=rs::util::Deadline(0s);EXPECT_EQ(std::get<JsonError>(parse(valid,rollback)).failure,JsonFailure::ClockRollback);
  Clock ending;ending.late_at=6;EXPECT_EQ(std::get<JsonError>(parse(valid,ending)).failure,JsonFailure::DeadlineElapsed);
}
TEST(AwsDbJsonResponse, ExactAggregateNodeAndRawBodyBudgets) {
  Clock c;
  for(auto extra:{0u,1u}) {
    const auto text=std::string(R"({"x":")")+std::string(65536,'a')+R"(","y":")"+std::string(6142+extra,'b')+R"("})";
    auto r=parse(text,c);EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(r),extra==0);
    if(extra) { EXPECT_EQ(std::get<JsonError>(r).failure,JsonFailure::ResourceLimit); }
  }
  std::string nodes=R"({"x":[)";for(int i=0;i<126;++i){if(i)nodes+=',';nodes+='0';}nodes+="]}";
  EXPECT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(nodes,c)));
  std::string full(StreamOwner::max_bytes-2,' ');full+="{}";
  EXPECT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(full,c)));
  EXPECT_EQ(std::get<JsonError>(parse("",c)).failure,JsonFailure::InvalidSyntax);
}
TEST(AwsDbJsonResponse, FinalPublicationCheckpointAndRefreshExtraction) {
  Clock baseline;ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(valid,baseline)));
  Clock late;late.late_at=baseline.calls;
  EXPECT_EQ(std::get<JsonError>(parse(valid,late)).failure,JsonFailure::DeadlineElapsed);
  Clock c;Stop final_stop;final_stop.at=baseline.calls;
  EXPECT_EQ(std::get<JsonError>(parse(valid,c,&final_stop)).failure,JsonFailure::Cancelled);
  for(auto refresh:{"2.000001","null","true"}) {
    Clock clock;auto r=parse(std::string(R"({"dbUser":"u","dbPassword":"p","expiration":1,"nextRefreshTime":)")+refresh+"}",clock);
    ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));
    auto e=extract_db_fields(DbCredentialOperation::ServerlessGetCredentials,std::move(std::get<ResponseSnapshot>(r)));
    EXPECT_EQ(static_cast<bool>(e),std::string_view(refresh)=="2.000001");
    if(e) { EXPECT_EQ(e.value().expiry.microseconds_since_epoch,1000000); }
  }
}
TEST(AwsDbJsonResponse, EscapedStringsAndInvalidDeadlineHaveClosedOutcomes) {
  Clock c;auto r=parse(R"({"dbPassword":"a\"\\\/\b\f\n\r\t"})",c);
  ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));
  std::get<TextBytes>(std::get<ResponseSnapshot>(r).occurrences()[0].atom()).bytes.with_bytes([](auto b){
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data()),b.size()),std::string("a\"\\/\b\f\n\r\t"));
  });
  auto body=raw(valid);auto bad=parse_aws_db_json_response(DbCredentialOperation::ServerlessGetCredentials,ResponseShape::ServerlessObject,
      std::move(body),rs::util::Deadline::max(),rs::util::Deadline(1s),c);
  EXPECT_EQ(std::get<JsonError>(bad).failure,JsonFailure::InvalidDeadline);
  EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);
}

}
