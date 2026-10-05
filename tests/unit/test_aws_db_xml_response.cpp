#include "odbcpp/auth/aws_db_xml_response.h"
#include <gtest/gtest.h>
#include <chrono>
#include <string>
using namespace rs::core::auth;
namespace {
using namespace std::chrono_literals;
struct Clock final:MonotonicClock {
  rs::util::Deadline value{1s},late{10s};int calls{0},at{0};
  rs::util::Deadline now()noexcept override{if(++calls==at)value=late;return value;}
};
struct Stop final:Cancellation {mutable int calls{0};int at{0};bool stop_requested()const noexcept override{return ++calls==at;}};
std::string envelope(std::string_view leaves,bool iam=false,std::string_view metadata=""){
  const std::string op=iam?"GetClusterCredentialsWithIAM":"GetClusterCredentials";
  return "<"+op+"Response xmlns=\"http://redshift.amazonaws.com/doc/2012-12-01/\"><"+op+"Result>"+std::string(leaves)+"</"+op+"Result>"+std::string(metadata)+"</"+op+"Response>";
}
ResponseBytes raw(std::string_view text){
  Clock c;auto r=StreamOwner::create(StreamOwner::max_bytes,rs::util::Deadline(10s),c,nullptr);
  auto b=std::move(std::get<StreamOwner>(r));b.io()->write(text.data(),static_cast<std::streamsize>(text.size()));
  return std::move(std::get<ResponseBytes>(seal_response(std::move(b))));
}
XmlOutcome<ResponseSnapshot> parse(std::string_view text,Clock& c,bool iam=false,const Cancellation* stop=nullptr){
  auto b=raw(text);return parse_aws_db_xml_response(iam?DbCredentialOperation::ClusterGetCredentialsWithIam:DbCredentialOperation::ClusterGetCredentials,
      iam?ResponseShape::ClusterWithIamResult:ResponseShape::ClusterResult,std::move(b),rs::util::Deadline(10s),rs::util::Deadline(1s),c,stop);
}
constexpr std::string_view leaves="<DbUser>IAM:user</DbUser><DbPassword>secret</DbPassword><Expiration>2026-10-04T12:00:00.000001Z</Expiration>";
TEST(AwsDbXmlResponse, BothMethodOwningIsoMicrosecondsAndMetadata){
  for(bool iam:{false,true}){
    Clock c;auto r=parse(envelope(leaves,iam,"<ResponseMetadata><RequestId>synthetic</RequestId></ResponseMetadata>"),c,iam);
    ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));auto& s=std::get<ResponseSnapshot>(r);
    ASSERT_EQ(s.occurrences().size(),3u);EXPECT_EQ(s.occurrences()[0].key(),"DbUser");EXPECT_EQ(s.shape(),iam?ResponseShape::ClusterWithIamResult:ResponseShape::ClusterResult);
    auto f=extract_db_fields(iam?DbCredentialOperation::ClusterGetCredentialsWithIam:DbCredentialOperation::ClusterGetCredentials,std::move(s));ASSERT_TRUE(f);
    EXPECT_EQ(f.value().user,"IAM:user");EXPECT_EQ(f.value().expiry.microseconds_since_epoch%1000000,1);
    f.value().password.with_bytes([](auto b){EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data()),b.size()),"secret");});
  }
}
TEST(AwsDbXmlResponse, DeclarationBomAndEmptyLeaves){
  Clock c;auto r=parse(std::string("\xEF\xBB\xBF<?xml version='1.0' encoding='UTF-8'?>\n")+envelope("<DbPassword/><DbUser></DbUser>"),c);
  ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));auto& s=std::get<ResponseSnapshot>(r);ASSERT_EQ(s.occurrences().size(),2u);
  for(const auto& field:s.occurrences()){ASSERT_TRUE(std::holds_alternative<TextBytes>(field.atom()));EXPECT_TRUE(std::get<TextBytes>(field.atom()).bytes.empty());}
}
TEST(AwsDbXmlResponse, DuplicateUnknownAndMethodSpecificRefreshDisposition){
  for(const auto& [body,error]:{std::pair{std::string(leaves)+"<DbUser>other</DbUser>",FieldFailure::DuplicateField},
                       std::pair{std::string(leaves)+"<Unknown>x</Unknown>",FieldFailure::UnknownField}}){
    Clock c;auto r=parse(envelope(body),c);ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));
    auto f=extract_db_fields(DbCredentialOperation::ClusterGetCredentials,std::move(std::get<ResponseSnapshot>(r)));ASSERT_FALSE(f);EXPECT_EQ(f.error().failure,error);
  }
  for(bool iam:{false,true}){
    Clock c;auto r=parse(envelope(std::string(leaves)+"<NextRefreshTime>2026-10-04T13:00:00Z</NextRefreshTime>",iam),c,iam);
    ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));auto f=extract_db_fields(iam?DbCredentialOperation::ClusterGetCredentialsWithIam:DbCredentialOperation::ClusterGetCredentials,std::move(std::get<ResponseSnapshot>(r)));
    EXPECT_EQ(static_cast<bool>(f),iam);if(f){EXPECT_EQ(f.value().expiry.microseconds_since_epoch%1000000,1);}
  }
}
TEST(AwsDbXmlResponse, DecodeOnceReferencesUtf8AndLiteralNewlineNormalization){
  Clock c;auto r=parse(envelope("<DbPassword>&amp;lt;&lt;&gt;&quot;&apos;&#65;&#x1f600;\r\n\r&#13;</DbPassword>"),c);
  ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));std::get<TextBytes>(std::get<ResponseSnapshot>(r).occurrences()[0].atom()).bytes.with_bytes([](auto b){
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(b.data()),b.size()),std::string("&lt;<>\"'A\xF0\x9F\x98\x80\n\n\r"));
  });
}
TEST(AwsDbXmlResponse, NamespaceRootResultAndMetadataCardinalityRefuse){
  Clock c;const auto good=envelope(leaves);
  for(auto text:{std::string("<GetClusterCredentialsResult/>"),envelope(leaves,true),std::string("<ErrorResponse><Error>"+good+"</Error></ErrorResponse>"),good+"x",good+good}){
    EXPECT_TRUE(std::holds_alternative<XmlError>(parse(text,c)));
  }
  for(auto suffix:{"<GetClusterCredentialsResult/>","<ResponseMetadata/><ResponseMetadata/>","<ResponseMetadata><RequestId/><RequestId/></ResponseMetadata>"}){
    auto text=envelope(leaves,false,suffix);EXPECT_EQ(std::get<XmlError>(parse(text,c)).failure,XmlFailure::DuplicateEnvelope);
  }
  auto wrong=good;wrong.replace(wrong.find("http://"),7,"https://");EXPECT_EQ(std::get<XmlError>(parse(wrong,c)).failure,XmlFailure::InvalidNamespace);
  auto missing=good;const auto start=missing.find(" xmlns=");missing.erase(start,missing.find('>',start)-start);EXPECT_EQ(std::get<XmlError>(parse(missing,c)).failure,XmlFailure::InvalidNamespace);
}
TEST(AwsDbXmlResponse, AsciiNameGrammarPrefixAttributesAndNestedContentRefuse){
  Clock c;
  for(auto name:{"x:y","1bad","bad@name","bad name","é"}){
    EXPECT_TRUE(std::holds_alternative<XmlError>(parse(envelope("<"+std::string(name)+"/>"),c)));
  }
  for(auto leaf:{"<DbUser xmlns='wrong'>u</DbUser>","<DbUser xsi:nil='true'/>","<DbPassword><x/></DbPassword>","<DbPassword>x<![CDATA[y]]></DbPassword>"}){
    EXPECT_TRUE(std::holds_alternative<XmlError>(parse(envelope(leaf),c)));
  }
  for(auto name:{"_abc","a-b.c_1"}){
    EXPECT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(envelope("<"+std::string(name)+"/>"),c)));
  }
}
TEST(AwsDbXmlResponse, DtdEntitiesPiCommentEncodingAndMalformedInputRefuse){
  Clock c;
  for(auto prefix:{"<!DOCTYPE x [<!ENTITY p SYSTEM 'file:///no-read'>]>","<?other x?>","<!--x-->","<?xml version='1.1'?>","<?xml version='&#49;.0'?>","<?xml version='1.0' encoding='UTF-16'?>","<?xml version='1.0' standalone='yes'?>"}){
    EXPECT_TRUE(std::holds_alternative<XmlError>(parse(std::string(prefix)+envelope(leaves),c)));
  }
  for(auto text:{"&unknown;","&#0;","&#xD800;","&#1114112;","&amp","&#x;","&#-1;", "]]>"}){
    EXPECT_TRUE(std::holds_alternative<XmlError>(parse(envelope("<DbPassword>"+std::string(text)+"</DbPassword>"),c)));
  }
  for(auto text:{std::string("\xC0\xAF"),std::string("\xED\xA0\x80"),std::string("\xE2\x82"),std::string(1,'\0'),std::string(1,'\1')}){
    EXPECT_TRUE(std::holds_alternative<XmlError>(parse(envelope("<DbPassword>"+text+"</DbPassword>"),c)));
  }
  auto trunc=envelope(leaves);trunc.pop_back();EXPECT_TRUE(std::holds_alternative<XmlError>(parse(trunc,c)));
  EXPECT_TRUE(std::holds_alternative<XmlError>(parse(envelope("<DbUser>x</DbPassword>"),c)));
}
TEST(AwsDbXmlResponse, LeafKeyFieldMetadataAndAggregateBounds){
  Clock c;
  for(auto n:{65536u,65537u}){
    EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(parse(envelope("<DbPassword>"+std::string(n,'a')+"</DbPassword>"),c)),n==65536);
  }
  for(auto n:{64u,65u}){
    auto key=std::string(n,'k');EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(parse(envelope("<"+key+"/>"),c)),n==64);
  }
  for(auto n:{8u,9u}){
    std::string fields;for(unsigned i=0;i<n;++i)fields+="<k"+std::to_string(i)+"/>";
    EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(parse(envelope(fields),c)),n==8);
  }
  for(auto n:{128u,129u}){
    const auto meta="<ResponseMetadata><RequestId>"+std::string(n,'r')+"</RequestId></ResponseMetadata>";
    EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(parse(envelope(leaves,false,meta),c)),n==128);
  }
  EXPECT_EQ(std::get<XmlError>(parse(envelope("<a>"+std::string(40000,'a')+"</a><b>"+std::string(40000,'b')+"</b>"),c)).failure,XmlFailure::ResourceLimit);
}
TEST(AwsDbXmlResponse, ConsumingIsolationMovedFromDeadlineAndClockCheckpoints){
  Clock c;auto body=raw(envelope(leaves));auto r=parse_aws_db_xml_response(DbCredentialOperation::ServerlessGetCredentials,ResponseShape::ClusterResult,std::move(body),rs::util::Deadline(10s),rs::util::Deadline(1s),c);
  EXPECT_EQ(std::get<XmlError>(r).failure,XmlFailure::UnsupportedOperation);EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);
  auto b=raw(envelope(leaves));auto moved=std::move(b);
  auto invalid=parse_aws_db_xml_response(DbCredentialOperation::ClusterGetCredentials,ResponseShape::ClusterResult,std::move(b),rs::util::Deadline(10s),rs::util::Deadline(1s),c);
  EXPECT_EQ(std::get<XmlError>(invalid).failure,XmlFailure::InvalidBody);
  auto mismatch=parse_aws_db_xml_response(DbCredentialOperation::ClusterGetCredentials,ResponseShape::ClusterWithIamResult,std::move(moved),rs::util::Deadline(10s),rs::util::Deadline(1s),c);
  EXPECT_EQ(std::get<XmlError>(mismatch).failure,XmlFailure::InvalidShape);
  Clock expired;expired.value=rs::util::Deadline(10s);EXPECT_EQ(std::get<XmlError>(parse(envelope(leaves),expired)).failure,XmlFailure::DeadlineElapsed);
  Clock rollback;rollback.value=rs::util::Deadline(0s);EXPECT_EQ(std::get<XmlError>(parse(envelope(leaves),rollback)).failure,XmlFailure::ClockRollback);
}
TEST(AwsDbXmlResponse, CancelDuringLeafAndFinalPublicationSafeErrors){
  Clock baseline;ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(envelope(leaves),baseline)));
  Clock late;late.at=baseline.calls;EXPECT_EQ(std::get<XmlError>(parse(envelope(leaves),late)).failure,XmlFailure::DeadlineElapsed);
  for(int at:{1,8,baseline.calls}){
    Clock c;Stop stop;stop.at=at;auto r=parse(envelope(leaves),c,false,&stop);ASSERT_TRUE(std::holds_alternative<XmlError>(r));
    EXPECT_EQ(std::get<XmlError>(r).failure,XmlFailure::Cancelled);EXPECT_EQ(std::get<XmlError>(r).safe_message().find("secret"),std::string_view::npos);
  }
}
TEST(AwsDbXmlResponse, TimestampPaddingRefusesWithoutSilentTrimOrRefreshAuthority){
  Clock c;auto r=parse(envelope("<DbUser>u</DbUser><DbPassword>p</DbPassword><Expiration> 2026-10-04T12:00:00Z </Expiration>"),c);
  ASSERT_TRUE(std::holds_alternative<ResponseSnapshot>(r));auto f=extract_db_fields(DbCredentialOperation::ClusterGetCredentials,std::move(std::get<ResponseSnapshot>(r)));
  ASSERT_FALSE(f);EXPECT_EQ(f.error().failure,FieldFailure::InvalidTimestamp);
}
TEST(AwsDbXmlResponse, ExactAggregateRawBudgetAndDuplicateNamespaceRefuse){
  Clock c;constexpr std::string_view uri="http://redshift.amazonaws.com/doc/2012-12-01/";
  const auto remainder=ResponseSnapshot::max_aggregate_bytes-uri.size()-65536;
  for(auto extra:{0u,1u}){
    auto r=parse(envelope("<a>"+std::string(65536,'a')+"</a><b>"+std::string(remainder+extra,'b')+"</b>"),c);
    EXPECT_EQ(std::holds_alternative<ResponseSnapshot>(r),extra==0);
    if(extra){EXPECT_EQ(std::get<XmlError>(r).failure,XmlFailure::ResourceLimit);}
  }
  auto full=envelope("");full.append(StreamOwner::max_bytes-full.size(),' ');
  EXPECT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(full,c)));
  auto duplicate=envelope(leaves);duplicate.insert(duplicate.find('>')," xmlns='http://redshift.amazonaws.com/doc/2012-12-01/'");
  EXPECT_EQ(std::get<XmlError>(parse(duplicate,c)).failure,XmlFailure::InvalidNamespace);
  auto invalid=raw(envelope(leaves));auto result=parse_aws_db_xml_response(DbCredentialOperation::ClusterGetCredentials,ResponseShape::ClusterResult,
      std::move(invalid),rs::util::Deadline::max(),rs::util::Deadline(1s),c);
  EXPECT_EQ(std::get<XmlError>(result).failure,XmlFailure::InvalidDeadline);
}
TEST(AwsDbXmlResponse, WhitespaceEnvelopeOrderAndReferenceOverflow){
  Clock c;auto text=envelope(leaves,false,"<ResponseMetadata/>");
  const std::string from="<GetClusterCredentialsResult>";
  text.erase(text.find("<ResponseMetadata/>"),std::string("<ResponseMetadata/>").size());
  text.insert(text.find(from),"\r\n<ResponseMetadata/>\n");
  EXPECT_TRUE(std::holds_alternative<ResponseSnapshot>(parse(text,c)));
  for(auto entity:{"&#999999999;","&#xFFFFFFFF;","&#99999999999999999999;"}){
    EXPECT_TRUE(std::holds_alternative<XmlError>(parse(envelope("<DbUser>"+std::string(entity)+"</DbUser>"),c)));
  }
  const auto large=envelope("<DbPassword>"+std::string(4000,'s')+"</DbPassword>");
  Stop stop;stop.at=12;auto r=parse(large,c,false,&stop);
  ASSERT_TRUE(std::holds_alternative<XmlError>(r));EXPECT_EQ(std::get<XmlError>(r).failure,XmlFailure::Cancelled);
}

}
