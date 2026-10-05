#include "odbcpp/auth/checked_aws_db_json_response.h"
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include <type_traits>
#include <vector>
namespace a=rs::core::auth;
using namespace std::chrono_literals;
namespace {
constexpr auto op=a::DbCredentialOperation::ServerlessGetCredentials;
constexpr auto shape=a::ResponseShape::ServerlessObject;
constexpr std::string_view valid=R"({"dbUser":"u","dbPassword":"p","expiration":1.000001})";
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
  auto binding=a::Binding::create({a::Service::Redshift,"json.fixture.invalid",5439,"db","user","resource","json.fixture.invalid","local-policy"},
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
  a::CheckedJsonResult<a::ResponseSnapshot> parse(std::string_view text) {
    auto body=raw(text);return a::parse_aws_db_json_response_checked(op,shape,std::move(body),phase());
  }
};
void error(const a::CheckedJsonResult<a::ResponseSnapshot>& result,std::optional<a::JsonFailure> local,
    std::optional<a::BoundaryFailure> boundary) {
  ASSERT_TRUE(std::holds_alternative<a::CheckedJsonError>(result));const auto& e=std::get<a::CheckedJsonError>(result);
  if(local) { ASSERT_TRUE(e.local());EXPECT_EQ(e.local()->failure,*local); }
  else { EXPECT_FALSE(e.local()); }
  EXPECT_EQ(e.boundary(),boundary);EXPECT_TRUE(e.safe_message().find("synthetic")==std::string_view::npos);
  EXPECT_TRUE(e.safe_message().find("dbPassword")==std::string_view::npos);
}
void terminal(const Fixture& f,a::BoundaryFailure expected) {
  EXPECT_EQ(f.owner->first,expected);EXPECT_EQ(f.owner->live,0u);EXPECT_EQ(f.owner->life->gates,1u);
  ASSERT_GE(f.owner->events.size(),3u);const auto n=f.owner->events.size();
  EXPECT_EQ(f.owner->events[n-3],Event::Reject);EXPECT_EQ(f.owner->events[n-2],Event::Check);EXPECT_EQ(f.owner->events[n-1],Event::Destroy);
}
static_assert(std::is_nothrow_move_constructible_v<a::ResponseSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<a::CheckedJsonResult<a::ResponseSnapshot>>);
static_assert(!std::is_nothrow_move_constructible_v<a::ResponseBytes>);
}
TEST(CheckedJsonPortable, OwningSnapshotAndSeparateExtractionLease) {
  Fixture f;
  {
  auto body=raw(valid);auto keep=f.phase();auto phase=f.phase();
  ASSERT_TRUE(phase.request());EXPECT_EQ(phase.request()->deadline(),rs::util::Deadline{10ms});EXPECT_EQ(phase.request()->headroom(),100us);
  auto parsed=a::parse_aws_db_json_response_checked(op,shape,std::move(body),std::move(phase));
  ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(parsed));EXPECT_EQ(phase.request(),nullptr);
  EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);EXPECT_EQ(f.owner->live,1u);EXPECT_EQ(f.owner->life->gates,1u);
  auto& snapshot=std::get<a::ResponseSnapshot>(parsed);ASSERT_EQ(snapshot.occurrences().size(),3u);
  EXPECT_TRUE(snapshot.occurrences()[0].key()=="dbUser");EXPECT_TRUE(snapshot.occurrences()[1].key()=="dbPassword");
  EXPECT_TRUE(std::get<a::NumberLexeme>(snapshot.occurrences()[2].atom()).bytes=="1.000001");
  auto extracted=a::extract_db_fields(op,std::move(snapshot));ASSERT_TRUE(extracted);
  EXPECT_EQ(extracted.value().expiry.microseconds_since_epoch,1000001);EXPECT_TRUE(extracted.value().user=="u");
  extracted.value().password.with_bytes([](auto bytes) { EXPECT_TRUE(bytes.size()==1 && bytes[0]==std::byte{'p'}); });
  EXPECT_TRUE(std::holds_alternative<std::monostate>(keep.check()));EXPECT_EQ(f.owner->live,1u);EXPECT_FALSE(f.owner->first);
  } // Retained extraction borrow releases after owning fields clean up.
  EXPECT_EQ(f.owner->live,0u);EXPECT_EQ(f.owner->life->gates,2u);EXPECT_FALSE(f.owner->first);
  ASSERT_FALSE(f.owner->events.empty());EXPECT_EQ(f.owner->events.back(),Event::Destroy);
}
TEST(CheckedJsonPortable, InitialPeriodicFinalAndPostCleanupChecks) {
  const std::string text=std::string(R"({"dbPassword":")")+std::string(1000,'s')+R"("})";
  Clock legacy;auto legacy_body=raw(text);auto legacy_result=a::parse_aws_db_json_response(op,shape,std::move(legacy_body),rs::util::Deadline{10ms},rs::util::Deadline{0us},legacy);
  ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(legacy_result));
  Fixture baseline;auto body=raw(text);auto phase=baseline.phase();const auto start=baseline.owner->checks;
  ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(a::parse_aws_db_json_response_checked(op,shape,std::move(body),std::move(phase))));
  const auto count=baseline.owner->checks-start;EXPECT_EQ(count,legacy.reads+2);ASSERT_GT(count,8u);
  for(const auto offset:{1u,8u,count-1,count}) {
    Fixture f;auto bytes=raw(text);auto lease=f.phase();f.owner->scheduled=a::BoundaryFailure::DeadlineElapsed;f.owner->fault_at=f.owner->checks+offset;
    error(a::parse_aws_db_json_response_checked(op,shape,std::move(bytes),std::move(lease)),{},a::BoundaryFailure::DeadlineElapsed);
    terminal(f,a::BoundaryFailure::DeadlineElapsed);
  }
}
TEST(CheckedJsonPortable, FirstFaultSurvivesRecoveryAndLaterCancellation) {
  constexpr std::array reasons{a::BoundaryFailure::ReadFailed,a::BoundaryFailure::ClockRollback,a::BoundaryFailure::DeadlineElapsed,
      a::BoundaryFailure::Cancelled,a::BoundaryFailure::SourceClosed,a::BoundaryFailure::SourceChanged,a::BoundaryFailure::StaleObservation};
  for(const auto first:reasons) {
    for(int path=0;path<3;++path) {
      Fixture f;auto body=raw("{");auto phase=f.phase();auto keep=f.phase();f.owner->first=first;
      auto result=path==2?a::detail::CheckedJsonTestAccess::refuse_processing(op,shape,std::move(body),std::move(phase),a::detail::CheckedJsonConstructionFailure::BeforeParse):
          a::parse_aws_db_json_response_checked(path==0?a::DbCredentialOperation::ClusterGetCredentials:op,shape,std::move(body),std::move(phase));
      error(result,{},first);f.owner->scheduled=a::BoundaryFailure::Cancelled;f.owner->fault_at=f.owner->checks+1;
      EXPECT_EQ(std::get<a::BoundaryFailure>(keep.check()),first);EXPECT_EQ(std::get<a::BoundaryFailure>(keep.reject_body()),first);
      EXPECT_EQ(f.owner->first,first);EXPECT_EQ(f.owner->live,1u);
    }
  }
}
TEST(CheckedJsonPortable, CompleteGrammarUtf8EscapesAndResourceRefusals) {
  const std::array pairs{
      std::pair{std::string("{}x"),a::JsonFailure::InvalidSyntax},std::pair{std::string("{}\0",3),a::JsonFailure::InvalidSyntax},
      std::pair{std::string(R"({"x":"\uD800"})"),a::JsonFailure::InvalidEscape},std::pair{std::string(R"({"x":"\q"})"),a::JsonFailure::InvalidEscape},
      std::pair{std::string("{\"x\":\"\xc0\x80\"}"),a::JsonFailure::InvalidUtf8},
      std::pair{std::string("{\"x\":\"\n\"}"),a::JsonFailure::InvalidSyntax},std::pair{std::string("{\"x\":[1,]}"),a::JsonFailure::InvalidSyntax}};
  for(const auto& [text,expected]:pairs) { Fixture f;error(f.parse(text),expected,a::BoundaryFailure::BodyRejected);terminal(f,a::BoundaryFailure::BodyRejected); }
  auto bounds=[](const std::string& text,bool success) {
    Fixture f;auto result=f.parse(text);EXPECT_EQ(std::holds_alternative<a::ResponseSnapshot>(result),success);
    if(!success) { error(result,a::JsonFailure::ResourceLimit,a::BoundaryFailure::BodyRejected);terminal(f,a::BoundaryFailure::BodyRejected); }
  };
  for(unsigned extra:{0u,1u}) {
    bounds(std::string(R"({"x":")")+std::string(65536+extra,'a')+R"("})",extra==0);
    bounds("{\""+std::string(64+extra,'k')+"\":0}",extra==0);
    bounds(std::string(R"({"x":)")+std::string(64+extra,'1')+"}",extra==0);
    std::string fields="{";for(unsigned n=0;n<8+extra;++n) { if(n) { fields+=','; }fields+="\"k"+std::to_string(n)+"\":0"; }fields+='}';bounds(fields,extra==0);
    bounds(std::string(R"({"x":)")+std::string(7+extra,'[')+"0"+std::string(7+extra,']')+"}",extra==0);
    std::string nodes=R"({"x":[)";for(unsigned n=0;n<126+extra;++n) { if(n) { nodes+=','; }nodes+='0'; }nodes+="]}";bounds(nodes,extra==0);
    bounds(std::string(R"({"x":")")+std::string(65536,'a')+R"(","y":")"+std::string(6142+extra,'b')+R"("})",extra==0);
  }
  bounds(std::string(a::StreamOwner::max_bytes-2,' ')+"{}",true); // Raw owner makes above-cap input unconstructible.
}
TEST(CheckedJsonPortable, OrderedDuplicatesAndWrongTypesRemainStructural) {
  auto exercise=[](std::string_view text,a::FieldFailure expected) {
    Fixture f;auto body=raw(text);auto keep=f.phase();auto parsed=a::parse_aws_db_json_response_checked(op,shape,std::move(body),f.phase());
    ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(parsed));EXPECT_FALSE(f.owner->first);EXPECT_EQ(f.owner->live,1u);
    auto& snapshot=std::get<a::ResponseSnapshot>(parsed);ASSERT_FALSE(snapshot.occurrences().empty());
    if(expected==a::FieldFailure::DuplicateField) {
      ASSERT_EQ(snapshot.occurrences().size(),2u);EXPECT_TRUE(snapshot.occurrences()[0].key()=="expiration");EXPECT_TRUE(snapshot.occurrences()[1].key()=="expiration");
      EXPECT_TRUE(std::get<a::NumberLexeme>(snapshot.occurrences()[0].atom()).bytes=="1");EXPECT_TRUE(std::get<a::NumberLexeme>(snapshot.occurrences()[1].atom()).bytes=="2");
    }
    auto extracted=a::extract_db_fields(op,std::move(snapshot));ASSERT_FALSE(extracted);EXPECT_EQ(extracted.error().failure,expected);
    EXPECT_EQ(std::get<a::BoundaryFailure>(keep.reject_body()),a::BoundaryFailure::BodyRejected);EXPECT_EQ(std::get<a::BoundaryFailure>(keep.check()),a::BoundaryFailure::BodyRejected);
  };
  exercise(R"({"expiration":1,"expir\u0061tion":2})",a::FieldFailure::DuplicateField);
  exercise(R"({"unknown":"p"})",a::FieldFailure::UnknownField);
  for(const auto atom:{"null","true","false","1","{}","[]"}) { exercise(std::string(R"({"dbUser":"u","expiration":1,"dbPassword":)")+atom+"}",a::FieldFailure::WrongType); }
}
TEST(CheckedJsonPortable, UnsupportedMovedNullAndActiveBorrowConsumption) {
  for(const auto operation:{a::DbCredentialOperation::ClusterGetCredentials,a::DbCredentialOperation::ClusterGetCredentialsWithIam,static_cast<a::DbCredentialOperation>(99)}) {
    Fixture f;auto body=raw(valid);error(a::parse_aws_db_json_response_checked(operation,shape,std::move(body),f.phase()),a::JsonFailure::UnsupportedOperation,a::BoundaryFailure::BodyRejected);
    EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);terminal(f,a::BoundaryFailure::BodyRejected);
  }
  { Fixture f;auto body=raw(valid);error(a::parse_aws_db_json_response_checked(op,a::ResponseShape::ClusterResult,std::move(body),f.phase()),a::JsonFailure::InvalidShape,a::BoundaryFailure::BodyRejected); }
  { Fixture f;auto body=raw(valid);auto retained=std::move(body);error(a::parse_aws_db_json_response_checked(op,shape,std::move(body),f.phase()),a::JsonFailure::InvalidBody,a::BoundaryFailure::BodyRejected); }
  { Fixture f;auto body=raw(valid);auto phase=f.phase();auto retained=std::move(phase);const auto checks=f.owner->checks;
    error(a::parse_aws_db_json_response_checked(op,shape,std::move(body),std::move(phase)),{},a::BoundaryFailure::InvalidBoundary);
    EXPECT_EQ(f.owner->checks,checks);EXPECT_FALSE(f.owner->first);EXPECT_EQ(f.owner->live,1u);EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);
  }
  EXPECT_EQ(std::get<a::BoundaryFailure>(a::detail::ResponseLeaseFactory::bind_processing(nullptr)),a::BoundaryFailure::InvalidBoundary);
  EXPECT_DEATH({ Fixture f;auto body=raw(valid);auto phase=f.phase();body.with_bytes([&](auto) {
    (void)a::parse_aws_db_json_response_checked(op,shape,std::move(body),std::move(phase));
  }); },"");
}
TEST(CheckedJsonPortable, AllocationGuardsAndNoPartialPublication) {
  for(const auto step:{a::detail::CheckedJsonConstructionFailure::BeforeParse,a::detail::CheckedJsonConstructionFailure::BeforePublication,static_cast<a::detail::CheckedJsonConstructionFailure>(99)}) {
    Fixture f;auto body=raw(valid);auto result=a::detail::CheckedJsonTestAccess::refuse_processing(op,shape,std::move(body),f.phase(),step);
    error(result,step==static_cast<a::detail::CheckedJsonConstructionFailure>(99)?a::JsonFailure::InvalidBody:a::JsonFailure::AllocationFailed,a::BoundaryFailure::BodyRejected);
    EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);terminal(f,a::BoundaryFailure::BodyRejected);
  }
}
TEST(CheckedJsonPortable, OwningGateTerminalConfirmationAndSafeErrors) {
  Fixture facade;auto life=facade.owner->life;std::weak_ptr<Owner> weak=facade.owner;auto phase=facade.phase();facade.owner.reset();auto body=raw(valid);
  auto result=a::parse_aws_db_json_response_checked(op,shape,std::move(body),std::move(phase));ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(result));
  EXPECT_TRUE(weak.expired());EXPECT_EQ(life->owners,1u);EXPECT_EQ(life->gates,1u);
  for(const auto reply:{Reply::Success,Reply::Nonterminal,Reply::Unconfirmed,Reply::Mismatched,Reply::Throws,Reply::ConfirmationThrows}) {
    for(int path=0;path<3;++path) {
      EXPECT_DEATH({ Fixture f;auto bytes=raw(path==0?"{":valid);auto lease=f.phase();f.owner->reply=reply;
        if(path==0) { (void)a::parse_aws_db_json_response_checked(op,shape,std::move(bytes),std::move(lease)); }
        else { (void)a::detail::CheckedJsonTestAccess::refuse_processing(op,shape,std::move(bytes),std::move(lease),
            path==1?a::detail::CheckedJsonConstructionFailure::BeforeParse:a::detail::CheckedJsonConstructionFailure::BeforePublication); }
      },"");
    }
  }
}
