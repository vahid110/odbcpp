#include "core/auth/checked_aws_db_xml_response.h"
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include <type_traits>
#include <vector>
namespace a=rs::core::auth;
using namespace std::chrono_literals;
namespace {
constexpr auto op=a::DbCredentialOperation::ClusterGetCredentials;
constexpr auto shape=a::ResponseShape::ClusterResult;
std::string envelope(std::string_view leaves,bool iam=false) {
  const std::string method=iam?"GetClusterCredentialsWithIAM":"GetClusterCredentials";
  return "<"+method+"Response xmlns=\"http://redshift.amazonaws.com/doc/2012-12-01/\"><"+method+"Result>"+
      std::string(leaves)+"</"+method+"Result></"+method+"Response>";
}
const std::string valid=envelope("<DbUser>u</DbUser><DbPassword>p</DbPassword><Expiration>1970-01-01T00:00:01.000001Z</Expiration>");
// Direct owning gates test local boundary events, not actual c/n, clocks,
// cancellation sources, quality, thread enforcement or authenticated provenance.
enum class Event { Check, Reject, Destroy };
enum class Reply { Normal, Success, Nonterminal, Unconfirmed, Mismatched, Throws, ConfirmationThrows };
struct Lifetime { unsigned owners{},gates{}; };
struct Owner {
  Owner() { events.reserve(4096); }
  ~Owner() { ++life->owners; }
  std::shared_ptr<Lifetime> life=std::make_shared<Lifetime>();
  std::vector<Event> events;
  std::optional<a::BoundaryFailure> first,scheduled;
  unsigned checks{},rejections{},live{},fault_at{};
  Reply reply{Reply::Normal};
};
a::Request request() {
  auto binding=a::Binding::create({a::Service::Redshift,"xml.fixture.invalid",5439,"db","user","resource","xml.fixture.invalid","local-policy"},
      {a::SourceKind::TrustedTemporaryDbIssuer,"synthetic","one"},a::Method::TemporaryDatabasePassword);
  return std::move(a::Request::create(std::move(binding).value(),rs::util::Deadline{10ms},100us)).value();
}
class Gate final:public a::detail::ProcessingBoundaryGate {
 public:
  explicit Gate(std::shared_ptr<Owner> owner):owner_(std::move(owner)),request_(::request()) { ++owner_->live; }
  ~Gate() noexcept override {
    if(!owner_->live) { std::terminate(); }
    --owner_->live;++owner_->life->gates;owner_->events.push_back(Event::Destroy);
  }
  const a::Request* request() const noexcept override { return &request_; }
  a::BoundaryStatus observe_checkpoint() override {
    ++owner_->checks;owner_->events.push_back(Event::Check);
    if(owner_->rejections && owner_->reply==Reply::ConfirmationThrows) { throw std::runtime_error("fixed synthetic check text"); }
    if(owner_->rejections && owner_->reply==Reply::Mismatched) { return a::BoundaryFailure::ReadFailed; }
    if(owner_->fault_at && owner_->checks==owner_->fault_at && !owner_->first) { owner_->first=owner_->scheduled; }
    if(owner_->first) { return *owner_->first; }
    return std::monostate{};
  }
  a::BoundaryStatus reject_body_fault() override {
    ++owner_->rejections;owner_->events.push_back(Event::Reject);
    switch(owner_->reply) {
      case Reply::Success:return std::monostate{};
      case Reply::Nonterminal:return a::BoundaryFailure::InvalidState;
      case Reply::Unconfirmed:return a::BoundaryFailure::BodyRejected;
      case Reply::Throws:throw std::runtime_error("fixed synthetic refusal text");
      case Reply::Normal:case Reply::Mismatched:case Reply::ConfirmationThrows:break;
    }
    if(!owner_->first) { owner_->first=a::BoundaryFailure::BodyRejected; }
    return *owner_->first;
  }
 private:
  const std::shared_ptr<Owner> owner_;const a::Request request_;
};
class Clock final:public a::MonotonicClock {
 public:
  rs::util::Deadline now() noexcept override { ++reads;return rs::util::Deadline{0us}; }
  unsigned reads{};
};
a::ResponseBytes raw(std::string_view text) {
  Clock producer;auto made=a::StreamOwner::create(a::StreamOwner::max_bytes,rs::util::Deadline{10ms},producer,nullptr);
  auto stream=std::get<a::StreamOwner>(std::move(made));stream.io()->write(text.data(),static_cast<std::streamsize>(text.size()));
  return std::get<a::ResponseBytes>(a::seal_response(std::move(stream)));
}
struct Fixture {
  std::shared_ptr<Owner> owner=std::make_shared<Owner>();
  a::ProcessingBoundaryLease phase() {
    return std::get<a::ProcessingBoundaryLease>(a::detail::ResponseLeaseFactory::bind_processing(std::make_unique<Gate>(owner)));
  }
  a::CheckedXmlResult<a::ResponseSnapshot> parse(std::string_view text) {
    auto body=raw(text);return a::parse_aws_db_xml_response_checked(op,shape,std::move(body),phase());
  }
};
void error(const a::CheckedXmlResult<a::ResponseSnapshot>& result,std::optional<a::XmlFailure> local,
    std::optional<a::BoundaryFailure> boundary) {
  ASSERT_TRUE(std::holds_alternative<a::CheckedXmlError>(result));const auto& e=std::get<a::CheckedXmlError>(result);
  if(local) { ASSERT_TRUE(e.local());EXPECT_EQ(e.local()->failure,*local); }
  else { EXPECT_FALSE(e.local()); }
  EXPECT_EQ(e.boundary(),boundary);EXPECT_TRUE(e.safe_message().find("synthetic")==std::string_view::npos);
  EXPECT_TRUE(e.safe_message().find("DbPassword")==std::string_view::npos);
}
void terminal(const Fixture& f,a::BoundaryFailure expected) {
  EXPECT_EQ(f.owner->first,expected);EXPECT_EQ(f.owner->live,0u);EXPECT_EQ(f.owner->life->gates,1u);
  ASSERT_GE(f.owner->events.size(),3u);const auto n=f.owner->events.size();
  EXPECT_EQ(f.owner->events[n-3],Event::Reject);EXPECT_EQ(f.owner->events[n-2],Event::Check);EXPECT_EQ(f.owner->events[n-1],Event::Destroy);
}
static_assert(std::is_nothrow_move_constructible_v<a::ResponseSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<a::CheckedXmlResult<a::ResponseSnapshot>>);
static_assert(!std::is_nothrow_move_constructible_v<a::ResponseBytes>);
}
TEST(CheckedXmlPortable, BothMethodsOwningSnapshotAndSeparateExtractionLease) {
  for(bool iam:{false,true}) {
    Fixture f;
    {
      const auto method=iam?a::DbCredentialOperation::ClusterGetCredentialsWithIam:op;
      const auto format=iam?a::ResponseShape::ClusterWithIamResult:shape;
      auto body=raw(envelope("<DbUser>u</DbUser><DbPassword>p</DbPassword><Expiration>1970-01-01T00:00:01.000001Z</Expiration>",iam));
      auto keep=f.phase();auto phase=f.phase();
      ASSERT_TRUE(phase.request());EXPECT_EQ(phase.request()->deadline(),rs::util::Deadline{10ms});EXPECT_EQ(phase.request()->headroom(),100us);
      auto parsed=a::parse_aws_db_xml_response_checked(method,format,std::move(body),std::move(phase));
      ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(parsed));EXPECT_EQ(phase.request(),nullptr);
      EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);EXPECT_EQ(f.owner->live,1u);
      auto& snapshot=std::get<a::ResponseSnapshot>(parsed);ASSERT_EQ(snapshot.occurrences().size(),3u);
      EXPECT_EQ(snapshot.shape(),format);EXPECT_TRUE(snapshot.occurrences()[0].key()=="DbUser");
      ASSERT_TRUE(std::holds_alternative<a::TextBytes>(snapshot.occurrences()[2].atom()));
      auto fields=a::extract_db_fields(method,std::move(snapshot));ASSERT_TRUE(fields);
      EXPECT_EQ(fields.value().expiry.microseconds_since_epoch,1000001);EXPECT_TRUE(fields.value().user=="u");
      fields.value().password.with_bytes([](auto bytes) { EXPECT_TRUE(bytes.size()==1 && bytes[0]==std::byte{'p'}); });
      EXPECT_TRUE(std::holds_alternative<std::monostate>(keep.check()));EXPECT_EQ(f.owner->live,1u);EXPECT_FALSE(f.owner->first);
    }
    EXPECT_EQ(f.owner->live,0u);EXPECT_EQ(f.owner->life->gates,2u);EXPECT_FALSE(f.owner->first);
    EXPECT_EQ(f.owner->events.back(),Event::Destroy);
  }
}
TEST(CheckedXmlPortable, InitialPeriodicFinalAndPostCleanupChecks) {
  const auto text=envelope("<DbPassword>"+std::string(1000,'s')+"</DbPassword>");
  Clock legacy;auto legacy_body=raw(text);
  ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(a::parse_aws_db_xml_response(op,shape,std::move(legacy_body),rs::util::Deadline{10ms},rs::util::Deadline{0us},legacy)));
  Fixture baseline;auto body=raw(text);auto phase=baseline.phase();const auto start=baseline.owner->checks;
  ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(a::parse_aws_db_xml_response_checked(op,shape,std::move(body),std::move(phase))));
  const auto count=baseline.owner->checks-start;EXPECT_EQ(count,legacy.reads+2);ASSERT_GT(count,8u);
  for(const auto offset:{1u,8u,count-1,count}) {
    Fixture f;auto bytes=raw(text);auto lease=f.phase();f.owner->scheduled=a::BoundaryFailure::DeadlineElapsed;f.owner->fault_at=f.owner->checks+offset;
    error(a::parse_aws_db_xml_response_checked(op,shape,std::move(bytes),std::move(lease)),{},a::BoundaryFailure::DeadlineElapsed);
    terminal(f,a::BoundaryFailure::DeadlineElapsed);
  }
}
TEST(CheckedXmlPortable, FirstFaultSurvivesRecoveryCancellationAndLocalErrors) {
  constexpr std::array reasons{a::BoundaryFailure::ReadFailed,a::BoundaryFailure::ClockRollback,a::BoundaryFailure::DeadlineElapsed,
      a::BoundaryFailure::Cancelled,a::BoundaryFailure::SourceClosed,a::BoundaryFailure::SourceChanged,a::BoundaryFailure::StaleObservation};
  for(const auto first:reasons) {
    for(int path=0;path<3;++path) {
      Fixture f;auto body=raw("<");auto phase=f.phase();auto keep=f.phase();f.owner->first=first;
      auto result=path==2?a::detail::CheckedXmlTestAccess::refuse_processing(op,shape,std::move(body),std::move(phase),a::detail::CheckedXmlConstructionFailure::BeforeParse):
          a::parse_aws_db_xml_response_checked(path==0?a::DbCredentialOperation::ServerlessGetCredentials:op,shape,std::move(body),std::move(phase));
      error(result,{},first);f.owner->scheduled=a::BoundaryFailure::Cancelled;f.owner->fault_at=f.owner->checks+1;
      EXPECT_EQ(std::get<a::BoundaryFailure>(keep.check()),first);EXPECT_EQ(std::get<a::BoundaryFailure>(keep.reject_body()),first);
      EXPECT_EQ(f.owner->first,first);EXPECT_EQ(f.owner->live,1u);
    }
  }
}
TEST(CheckedXmlPortable, CompleteGrammarNamespaceEntitiesUtf8AndResourceRefusals) {
  auto wrong_namespace=valid;wrong_namespace.replace(wrong_namespace.find("http://"),4,"xxxx");
  const std::array pairs{
    std::pair{valid+"x",a::XmlFailure::InvalidSyntax},std::pair{valid+std::string(1,'\0'),a::XmlFailure::InvalidSyntax},
    std::pair{wrong_namespace,a::XmlFailure::InvalidNamespace},
    std::pair{envelope("<DbPassword>&external;</DbPassword>"),a::XmlFailure::InvalidEntity},
    std::pair{envelope("<DbPassword>"+std::string("\xc0",1)+"</DbPassword>"),a::XmlFailure::InvalidUtf8},
    std::pair{std::string("<!DOCTYPE x>")+valid,a::XmlFailure::UnsupportedMarkup},
    std::pair{envelope("<p:DbUser>x</p:DbUser>"),a::XmlFailure::InvalidName}};
  for(const auto& [text,expected]:pairs) { Fixture f;error(f.parse(text),expected,a::BoundaryFailure::BodyRejected);terminal(f,a::BoundaryFailure::BodyRejected); }
  for(unsigned extra:{0u,1u}) {
    Fixture f;auto result=f.parse(envelope("<DbPassword>"+std::string(65536+extra,'a')+"</DbPassword>"));
    EXPECT_EQ(std::holds_alternative<a::ResponseSnapshot>(result),extra==0);
    if(extra) { error(result,a::XmlFailure::ResourceLimit,a::BoundaryFailure::BodyRejected);terminal(f,a::BoundaryFailure::BodyRejected); }
  }
}
TEST(CheckedXmlPortable, DuplicatesEmptyAndMissingRemainStructuralUntilExtraction) {
  const std::array pairs{
      std::pair{std::string("<DbUser>u</DbUser><DbUser>v</DbUser>"),a::FieldFailure::DuplicateField},
      std::pair{std::string("<DbUser>u</DbUser><DbPassword/><Expiration>1970-01-01T00:00:01Z</Expiration>"),a::FieldFailure::InvalidPassword},
      std::pair{std::string("<DbUser>u</DbUser><DbPassword>p</DbPassword>"),a::FieldFailure::MissingField}};
  for(const auto& [leaves,expected]:pairs) {
    Fixture f;auto body=raw(envelope(leaves));auto keep=f.phase();
    auto parsed=a::parse_aws_db_xml_response_checked(op,shape,std::move(body),f.phase());
    ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(parsed));EXPECT_FALSE(f.owner->first);EXPECT_EQ(f.owner->live,1u);
    auto& snapshot=std::get<a::ResponseSnapshot>(parsed);
    if(expected==a::FieldFailure::DuplicateField) {
      ASSERT_EQ(snapshot.occurrences().size(),2u);EXPECT_TRUE(snapshot.occurrences()[0].key()=="DbUser");EXPECT_TRUE(snapshot.occurrences()[1].key()=="DbUser");
    }
    if(expected==a::FieldFailure::InvalidPassword) {
      ASSERT_TRUE(std::holds_alternative<a::TextBytes>(snapshot.occurrences()[1].atom()));EXPECT_TRUE(std::get<a::TextBytes>(snapshot.occurrences()[1].atom()).bytes.empty());
    }
    auto extracted=a::extract_db_fields(op,std::move(snapshot));ASSERT_FALSE(extracted);EXPECT_EQ(extracted.error().failure,expected);
    EXPECT_EQ(std::get<a::BoundaryFailure>(keep.reject_body()),a::BoundaryFailure::BodyRejected);EXPECT_EQ(std::get<a::BoundaryFailure>(keep.check()),a::BoundaryFailure::BodyRejected);
  }
}
TEST(CheckedXmlPortable, UnsupportedCrossMethodMovedNullAndActiveBorrowConsumption) {
  for(const auto operation:{a::DbCredentialOperation::ServerlessGetCredentials,static_cast<a::DbCredentialOperation>(99)}) {
    Fixture f;auto body=raw(valid);error(a::parse_aws_db_xml_response_checked(operation,shape,std::move(body),f.phase()),a::XmlFailure::UnsupportedOperation,a::BoundaryFailure::BodyRejected);
    EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);terminal(f,a::BoundaryFailure::BodyRejected);
  }
  { Fixture f;auto body=raw(valid);error(a::parse_aws_db_xml_response_checked(op,a::ResponseShape::ClusterWithIamResult,std::move(body),f.phase()),a::XmlFailure::InvalidShape,a::BoundaryFailure::BodyRejected); }
  { Fixture f;error(f.parse(envelope("",true)),a::XmlFailure::InvalidEnvelope,a::BoundaryFailure::BodyRejected); }
  { Fixture f;auto body=raw(valid);auto retained=std::move(body);error(a::parse_aws_db_xml_response_checked(op,shape,std::move(body),f.phase()),a::XmlFailure::InvalidBody,a::BoundaryFailure::BodyRejected); }
  { Fixture f;auto body=raw(valid);auto phase=f.phase();auto retained=std::move(phase);const auto checks=f.owner->checks;
    error(a::parse_aws_db_xml_response_checked(op,shape,std::move(body),std::move(phase)),{},a::BoundaryFailure::InvalidBoundary);
    EXPECT_EQ(f.owner->checks,checks);EXPECT_FALSE(f.owner->first);EXPECT_EQ(f.owner->live,1u);EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);
  }
  EXPECT_EQ(std::get<a::BoundaryFailure>(a::detail::ResponseLeaseFactory::bind_processing(nullptr)),a::BoundaryFailure::InvalidBoundary);
  EXPECT_DEATH({ Fixture f;auto body=raw(valid);auto phase=f.phase();body.with_bytes([&](auto) {
    (void)a::parse_aws_db_xml_response_checked(op,shape,std::move(body),std::move(phase));
  }); },"");
}
TEST(CheckedXmlPortable, AllocationGuardsAndNoPartialPublication) {
  for(const auto step:{a::detail::CheckedXmlConstructionFailure::BeforeParse,a::detail::CheckedXmlConstructionFailure::BeforePublication,static_cast<a::detail::CheckedXmlConstructionFailure>(99)}) {
    Fixture f;auto body=raw(valid);auto result=a::detail::CheckedXmlTestAccess::refuse_processing(op,shape,std::move(body),f.phase(),step);
    error(result,step==static_cast<a::detail::CheckedXmlConstructionFailure>(99)?a::XmlFailure::InvalidBody:a::XmlFailure::AllocationFailed,a::BoundaryFailure::BodyRejected);
    EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);terminal(f,a::BoundaryFailure::BodyRejected);
  }
}
TEST(CheckedXmlPortable, OwningGateTerminalConfirmationAndSafeErrors) {
  Fixture facade;auto life=facade.owner->life;std::weak_ptr<Owner> weak=facade.owner;auto phase=facade.phase();facade.owner.reset();auto body=raw(valid);
  auto result=a::parse_aws_db_xml_response_checked(op,shape,std::move(body),std::move(phase));ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(result));
  EXPECT_TRUE(weak.expired());EXPECT_EQ(life->owners,1u);EXPECT_EQ(life->gates,1u);
  for(const auto reply:{Reply::Success,Reply::Nonterminal,Reply::Unconfirmed,Reply::Mismatched,Reply::Throws,Reply::ConfirmationThrows}) {
    for(int path=0;path<3;++path) {
      EXPECT_DEATH({ Fixture f;auto bytes=raw(path==0?"<":valid);auto lease=f.phase();f.owner->reply=reply;
        if(path==0) { (void)a::parse_aws_db_xml_response_checked(op,shape,std::move(bytes),std::move(lease)); }
        else { (void)a::detail::CheckedXmlTestAccess::refuse_processing(op,shape,std::move(bytes),std::move(lease),
            path==1?a::detail::CheckedXmlConstructionFailure::BeforeParse:a::detail::CheckedXmlConstructionFailure::BeforePublication); }
      },"");
    }
  }
}
