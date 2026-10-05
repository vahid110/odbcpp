#include "odbcpp/auth/response_operation.h"
#include "odbcpp/auth/checked_response_stream.h"
#include "odbcpp/auth/checked_aws_db_json_response.h"
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include <type_traits>
#include <vector>
namespace a=rs::core::auth;
using namespace std::chrono_literals;
namespace {
a::Request request(int changed=0) {
  a::TargetInput target{a::Service::Redshift,"operation.fixture.invalid",5439,"db","u","resource","operation.fixture.invalid","local-policy"};
  a::SourceInput source{a::SourceKind::TrustedTemporaryDbIssuer,"synthetic","one"};auto method=a::Method::TemporaryDatabasePassword;
  if(changed==1) { target.endpoint="other.invalid"; }
  if(changed==2) { target.port=5440; }
  if(changed==3) { target.database="other"; }
  if(changed==4) { target.principal="other"; }
  if(changed==5) { target.resource_id="other"; }
  if(changed==6) { target.tls_identity="other.invalid"; }
  if(changed==7) { target.trust_policy="other"; }
  if(changed==8) { source.identity="other"; }
  if(changed==9) { source.generation="two"; }
  if(changed==10) { method=a::Method::OrdinaryPassword;source.kind=a::SourceKind::ExternalPassword; }
  auto binding=a::Binding::create(std::move(target),std::move(source),method);
  if(!binding) { throw std::logic_error("fixed test request invalid"); }
  return std::move(a::Request::create(std::move(binding).value(),rs::util::Deadline{changed==11?11ms:10ms},changed==12?101us:100us)).value();
}
struct Life { unsigned observers{}; };
enum class ReadMode { Actual, Failed, Malformed, Throws };
enum class Action { None, Cancel, ChangeSource, Reenter, DestroyPermit };
class Observer final:public a::ResponseObservationSource {
 public:
  ~Observer() noexcept override { ++life->observers; }
  a::ResponseClockRead read_monotonic() override {
    ++reads;if(read_failure_at==reads) { return a::ResponseReadFailure::ReadFailed; }
    if(action==Action::Cancel) { cancel=true; }
    if(action==Action::ChangeSource) { (void)source->retire(a::SourceRetirement::Changed); }
    if(action==Action::Reenter) { (void)operation->complete_transport(); }
    if(action==Action::DestroyPermit) { permit->reset(); }
    switch(mode) {
      case ReadMode::Actual:return now;
      case ReadMode::Failed:return a::ResponseReadFailure::ReadFailed;
      case ReadMode::Malformed:return static_cast<a::ResponseReadFailure>(99);
      case ReadMode::Throws:throw std::runtime_error("fixed private read text");
    }
    std::terminate();
  }
  bool cancellation_requested() override {
    ++cancels;if(retire_on_cancel==cancels) { (void)source->retire(a::SourceRetirement::Changed); }
    if(cancel_throws) { throw std::runtime_error("fixed private cancel text"); }return cancel;
  }
  rs::util::Deadline now{100us};unsigned reads{},cancels{},retire_on_cancel{},read_failure_at{};bool cancel{},cancel_throws{};
  ReadMode mode{ReadMode::Actual};Action action{Action::None};
  std::shared_ptr<Life> life=std::make_shared<Life>();
  a::ResponseSourceGeneration* source{};a::ResponseOperation* operation{};
  std::optional<a::TransportBoundaryLease>* permit{};
};
struct Fixture {
  std::shared_ptr<Observer> observer=std::make_shared<Observer>();
  std::shared_ptr<a::ResponseSourceGeneration> source;
  std::unique_ptr<a::ResponseOperation> operation;
  Fixture() {
    source=std::get<std::shared_ptr<a::ResponseSourceGeneration>>(a::ResponseSourceGeneration::create(request().binding()));
    observer->source=source.get();operation=std::get<std::unique_ptr<a::ResponseOperation>>(a::ResponseOperation::begin(request(),observer,source));
    observer->operation=operation.get();
  }
  void transport() { ASSERT_TRUE(std::holds_alternative<std::monostate>(operation->begin_transport())); }
  a::TransportBoundaryLease lease() { return std::get<a::TransportBoundaryLease>(operation->transport_lease(request())); }
  a::ProcessingBoundaryLease phase() { return std::get<a::ProcessingBoundaryLease>(operation->processing_lease(request())); }
  void completed() { ASSERT_TRUE(std::holds_alternative<std::monostate>(operation->complete_transport())); }
};
template<class T> void failure(const a::BoundaryResult<T>& r,a::BoundaryFailure expected) {
  ASSERT_TRUE(std::holds_alternative<a::BoundaryFailure>(r));EXPECT_EQ(std::get<a::BoundaryFailure>(r),expected);
}
constexpr std::string_view valid=R"({"dbUser":"u","dbPassword":"p","expiration":1.000001})";
a::ResponseBytes capture(Fixture& f,std::string_view body) {
  auto stream=std::get<a::CheckedStreamOwner>(a::CheckedStreamOwner::create(1024,f.lease()));stream.io()->write(body.data(),static_cast<std::streamsize>(body.size()));
  return std::get<a::ResponseBytes>(a::seal_checked_response(std::move(stream)));
}
static_assert(!std::is_copy_constructible_v<a::ResponseOperation>);
static_assert(!std::is_move_constructible_v<a::ResponseOperation>);
static_assert(std::is_nothrow_move_constructible_v<a::ResponseCompletion>);
}
TEST(ResponseOperation, OwningExactRequestAndZeroObservationMismatches) {
  Fixture f;f.transport();const auto reads=f.observer->reads,cancels=f.observer->cancels;
  for(int changed=1;changed<=12;++changed) { failure(f.operation->transport_lease(request(changed)),a::BoundaryFailure::WrongOperation); }
  auto invalid=request();auto kept=std::move(invalid);failure(f.operation->transport_lease(invalid),a::BoundaryFailure::InvalidInput);
  EXPECT_EQ(f.observer->reads,reads);EXPECT_EQ(f.observer->cancels,cancels);EXPECT_FALSE(f.operation->fault());
  auto lease=f.lease();ASSERT_TRUE(lease.request());EXPECT_TRUE(lease.request()->binding().source().generation=="one");
  EXPECT_EQ(lease.request()->deadline(),rs::util::Deadline{10ms});EXPECT_EQ(lease.request()->headroom(),100us);
  auto original=request();auto result=a::ResponseOperation::begin(std::move(original),f.observer,f.source);
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<a::ResponseOperation>>(result));
  const auto before=f.observer->reads;failure(a::ResponseOperation::begin(std::move(original),f.observer,f.source),a::BoundaryFailure::InvalidInput);
  failure(a::ResponseOperation::begin(request(9),f.observer,f.source),a::BoundaryFailure::WrongOperation);EXPECT_EQ(f.observer->reads,before);
  failure(a::ResponseOperation::begin(request(),nullptr,f.source),a::BoundaryFailure::InvalidInput);
  failure(a::ResponseOperation::begin(request(),f.observer,nullptr),a::BoundaryFailure::InvalidInput);
}
TEST(ResponseOperation, ActualSampleRollbackAndClosedReaderFailuresNeverRecover) {
  for(int kind=0;kind<6;++kind) {
    Fixture f;f.transport();auto lease=f.lease();
    if(kind==0) { f.observer->now=rs::util::Deadline{99us}; }
    if(kind==1) { f.observer->mode=ReadMode::Failed; }
    if(kind==2) { f.observer->mode=ReadMode::Malformed; }
    if(kind==3) { f.observer->mode=ReadMode::Throws; }
    if(kind==4) { f.observer->now=rs::util::Deadline::max(); }
    if(kind==5) { f.observer->now=rs::util::Deadline::min(); }
    const auto expected=kind==0?a::BoundaryFailure::ClockRollback:a::BoundaryFailure::ReadFailed;
    failure(lease.check(),expected);f.observer->mode=ReadMode::Actual;f.observer->now=rs::util::Deadline{200us};f.observer->cancel=true;
    const auto reads=f.observer->reads,cancels=f.observer->cancels;failure(lease.check(),expected);failure(lease.reject_body(),expected);
    failure(f.operation->complete_transport(),expected);EXPECT_EQ(f.operation->fault(),expected);
    EXPECT_EQ(f.observer->reads,reads);EXPECT_EQ(f.observer->cancels,cancels);
  }
}
TEST(ResponseOperation, OriginalDeadlineCancellationAndSourceRetirementOrdering) {
  for(int kind=0;kind<9;++kind) {
    Fixture f;f.transport();auto lease=f.lease();auto expected=a::BoundaryFailure::Cancelled;
    if(kind==0) { f.observer->now=rs::util::Deadline{10ms};expected=a::BoundaryFailure::DeadlineElapsed; }
    if(kind==1) { f.observer->cancel=true; }
    if(kind==2) { f.observer->action=Action::Cancel; }
    if(kind==3) { (void)f.source->retire(a::SourceRetirement::Closed);expected=a::BoundaryFailure::SourceClosed; }
    if(kind==4) { f.observer->action=Action::ChangeSource;expected=a::BoundaryFailure::SourceChanged; }
    if(kind==5) { f.observer->cancel_throws=true;expected=a::BoundaryFailure::ReadFailed; }
    if(kind==6) { f.observer->mode=ReadMode::Failed;f.observer->action=Action::Cancel;expected=a::BoundaryFailure::ReadFailed; }
    if(kind==7 || kind==8) { f.observer->retire_on_cancel=f.observer->cancels+(kind==7?1u:2u);expected=a::BoundaryFailure::SourceChanged; }
    const auto before=f.observer->reads;failure(lease.check(),expected);
    if(kind==1 || kind==3 || kind==5 || kind==7) { EXPECT_EQ(f.observer->reads,before); }
    f.observer->cancel=true;failure(lease.check(),expected);failure(f.operation->finish_processing(),expected);
  }
  Fixture f;failure(f.source->retire(static_cast<a::SourceRetirement>(99)),a::BoundaryFailure::InvalidInput);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(f.source->checkpoint()));
  failure(f.source->retire(a::SourceRetirement::Changed),a::BoundaryFailure::SourceChanged);
  failure(f.source->retire(a::SourceRetirement::Closed),a::BoundaryFailure::SourceChanged);
}
TEST(ResponseOperation, PermitBarriersAndOrderedActualCompletionSamples) {
  Fixture f;auto before=f.observer->reads;failure(f.operation->transport_lease(request()),a::BoundaryFailure::InvalidState);
  failure(f.operation->processing_lease(request()),a::BoundaryFailure::InvalidState);failure(f.operation->complete_transport(),a::BoundaryFailure::InvalidState);
  EXPECT_EQ(f.observer->reads,before);f.transport();
  {
    auto stream=f.lease();auto dispatch=f.lease();before=f.observer->reads;
    failure(f.operation->complete_transport(),a::BoundaryFailure::Borrowed);EXPECT_EQ(f.observer->reads,before);
  }
  f.observer->now=rs::util::Deadline{210us};f.completed();
  {
    auto helper=f.phase();auto extraction=f.phase();before=f.observer->reads;
    failure(f.operation->finish_processing(),a::BoundaryFailure::Borrowed);EXPECT_EQ(f.observer->reads,before);
    failure(f.operation->transport_lease(request()),a::BoundaryFailure::InvalidState);
  }
  f.observer->now=rs::util::Deadline{230us};auto complete=f.operation->finish_processing();ASSERT_TRUE(std::holds_alternative<a::ResponseCompletion>(complete));
  const auto& value=std::get<a::ResponseCompletion>(complete);EXPECT_EQ(value.initial_checkpoint(),rs::util::Deadline{100us});
  EXPECT_EQ(value.transport_completed(),rs::util::Deadline{210us});EXPECT_EQ(value.processing_completed(),rs::util::Deadline{230us});
}
TEST(ResponseOperation, OwningObserverLifetimeWrongThreadAndFatalReentryGuards) {
  Fixture f;f.transport();auto lease=f.lease();auto life=f.observer->life;std::weak_ptr<Observer> weak=f.observer;
  const auto reads=f.observer->reads,cancels=f.observer->cancels;
  std::optional<a::BoundaryFailure> wrong,retired,checked;
  std::thread other([&] { auto r=f.operation->begin_transport();wrong=std::get<a::BoundaryFailure>(r);
    retired=std::get<a::BoundaryFailure>(f.source->retire(a::SourceRetirement::Closed));checked=std::get<a::BoundaryFailure>(lease.check()); });other.join();
  EXPECT_EQ(wrong,a::BoundaryFailure::WrongThread);EXPECT_EQ(retired,a::BoundaryFailure::WrongThread);EXPECT_EQ(checked,a::BoundaryFailure::WrongThread);
  EXPECT_EQ(f.observer->reads,reads);EXPECT_EQ(f.observer->cancels,cancels);EXPECT_FALSE(f.operation->fault());
  f.operation.reset();f.source.reset();f.observer.reset();EXPECT_FALSE(weak.expired());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(lease.check()));failure(lease.reject_body(),a::BoundaryFailure::BodyRejected);
  EXPECT_DEATH({ Fixture bad;bad.transport();auto gate=bad.lease();bad.observer->action=Action::Reenter;(void)gate.check(); },"");
  EXPECT_DEATH({ Fixture bad;bad.transport();std::optional<a::TransportBoundaryLease> gate{bad.lease()};bad.observer->permit=&gate;
    bad.observer->action=Action::DestroyPermit;(void)bad.operation->transport_lease(request()); },"");
  EXPECT_DEATH({ Fixture bad;bad.transport();auto gate=bad.lease();std::thread thread([owned=std::move(gate)]() mutable { auto local=std::move(owned); });thread.join(); },"");
  EXPECT_DEATH({ Fixture bad;std::thread thread([owned=std::move(bad.operation)]() mutable { owned.reset(); });thread.join(); },"");
  EXPECT_DEATH({ Fixture bad;auto source=std::get<std::shared_ptr<a::ResponseSourceGeneration>>(a::ResponseSourceGeneration::create(request().binding()));
    std::thread thread([owned=std::move(source)]() mutable { owned.reset(); });thread.join(); },"");
  EXPECT_EQ(life->observers,0u); // The final live lease still owns dependencies.
  { auto release=std::move(lease); }
  EXPECT_TRUE(weak.expired());EXPECT_EQ(life->observers,1u);
}
TEST(ResponseOperation, AcceptedStreamAndJsonRejectSameOwnerBeforeCompletion) {
  for(int kind=0;kind<3;++kind) {
    Fixture f;f.transport();
    if(kind==0) { auto stream=std::get<a::CheckedStreamOwner>(a::CheckedStreamOwner::create(1,f.lease()));stream.io()->write("ab",2); }
    if(kind==1) { auto stream=std::get<a::CheckedStreamOwner>(a::CheckedStreamOwner::create(1,f.lease()));stream.io()->put('a'); }
    if(kind==2) { auto lease=f.lease();f.observer->now=rs::util::Deadline{99us};failure(lease.check(),a::BoundaryFailure::ClockRollback);failure(lease.reject_body(),a::BoundaryFailure::ClockRollback); }
    failure(f.operation->complete_transport(),kind==2?a::BoundaryFailure::ClockRollback:a::BoundaryFailure::BodyRejected);
  }
  Fixture malformed;malformed.transport();auto body=capture(malformed,"{");malformed.completed();
  auto result=a::parse_aws_db_json_response_checked(a::DbCredentialOperation::ServerlessGetCredentials,a::ResponseShape::ServerlessObject,std::move(body),malformed.phase());
  ASSERT_TRUE(std::holds_alternative<a::CheckedJsonError>(result));EXPECT_EQ(std::get<a::CheckedJsonError>(result).boundary(),a::BoundaryFailure::BodyRejected);
  failure(malformed.operation->finish_processing(),a::BoundaryFailure::BodyRejected);
  Fixture semantic;semantic.transport();auto wrong=capture(semantic,R"({"dbPassword":null,"dbUser":"u","expiration":1})");semantic.completed();
  {
    auto retained=semantic.phase();auto parsed=a::parse_aws_db_json_response_checked(a::DbCredentialOperation::ServerlessGetCredentials,a::ResponseShape::ServerlessObject,std::move(wrong),semantic.phase());
    ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(parsed));auto fields=a::extract_db_fields(a::DbCredentialOperation::ServerlessGetCredentials,std::move(std::get<a::ResponseSnapshot>(parsed)));
    ASSERT_FALSE(fields);failure(retained.reject_body(),a::BoundaryFailure::BodyRejected);failure(retained.check(),a::BoundaryFailure::BodyRejected);
  }
  failure(semantic.operation->finish_processing(),a::BoundaryFailure::BodyRejected);
}
TEST(ResponseOperation, GateConstructionAndPermitCapacityCannotHideAdmission) {
  Fixture f;f.transport();
  for(auto step:{a::detail::ResponsePreparationFailure::BeforeGate,a::detail::ResponsePreparationFailure::BeforePermit}) {
    const auto reads=f.observer->reads;failure(a::detail::ResponseOperationTestAccess::refuse_transport(*f.operation,request(),step),a::BoundaryFailure::AllocationFailed);
    EXPECT_EQ(f.observer->reads,reads);EXPECT_FALSE(f.operation->fault());
  }
  {
    std::vector<a::TransportBoundaryLease> permits;permits.reserve(64);for(unsigned i=0;i<64;++i) { permits.push_back(f.lease()); }
    const auto reads=f.observer->reads;failure(f.operation->transport_lease(request()),a::BoundaryFailure::Borrowed);EXPECT_EQ(f.observer->reads,reads);
  }
  f.completed();
  for(auto step:{a::detail::ResponsePreparationFailure::BeforeGate,a::detail::ResponsePreparationFailure::BeforePermit}) {
    const auto reads=f.observer->reads;failure(a::detail::ResponseOperationTestAccess::refuse_processing(*f.operation,request(),step),a::BoundaryFailure::AllocationFailed);
    EXPECT_EQ(f.observer->reads,reads);EXPECT_FALSE(f.operation->fault());
  }
  {
    std::vector<a::ProcessingBoundaryLease> permits;permits.reserve(64);for(unsigned i=0;i<64;++i) { permits.push_back(f.phase()); }
    const auto reads=f.observer->reads;failure(f.operation->processing_lease(request()),a::BoundaryFailure::Borrowed);EXPECT_EQ(f.observer->reads,reads);
  }
  auto completed=f.operation->finish_processing();ASSERT_TRUE(std::holds_alternative<a::ResponseCompletion>(completed));
  // Boundary factory performs a second checked observation AFTER attach. Failure
  // must publish no lease and release the sole admitted permit before State dies.
  for(bool processing:{false,true}) {
    Fixture attached;attached.transport();if(processing) { attached.completed(); }
    attached.observer->read_failure_at=attached.observer->reads+2;
    if(processing) { failure(attached.operation->processing_lease(request()),a::BoundaryFailure::ReadFailed); }
    else { failure(attached.operation->transport_lease(request()),a::BoundaryFailure::ReadFailed); }
    EXPECT_EQ(attached.operation->fault(),a::BoundaryFailure::ReadFailed);
  }
}
TEST(ResponseOperation, PassiveSingleUseResultAfterParseAndExtractionQuiescence) {
  Fixture f;f.transport();auto body=capture(f,valid);f.observer->now=rs::util::Deadline{200us};f.completed();
  {
    auto retained=f.phase();auto parsed=a::parse_aws_db_json_response_checked(a::DbCredentialOperation::ServerlessGetCredentials,a::ResponseShape::ServerlessObject,std::move(body),f.phase());
    ASSERT_TRUE(std::holds_alternative<a::ResponseSnapshot>(parsed));auto fields=a::extract_db_fields(a::DbCredentialOperation::ServerlessGetCredentials,std::move(std::get<a::ResponseSnapshot>(parsed)));
    ASSERT_TRUE(fields);EXPECT_EQ(fields.value().expiry.microseconds_since_epoch,1000001);
    const auto before=f.observer->reads;failure(f.operation->finish_processing(),a::BoundaryFailure::Borrowed);EXPECT_EQ(f.observer->reads,before);
  }
  f.observer->now=rs::util::Deadline{300us};auto result=f.operation->finish_processing();ASSERT_TRUE(std::holds_alternative<a::ResponseCompletion>(result));
  const auto before=f.observer->reads;failure(f.operation->finish_processing(),a::BoundaryFailure::InvalidState);
  failure(f.operation->begin_transport(),a::BoundaryFailure::InvalidState);failure(f.operation->processing_lease(request()),a::BoundaryFailure::InvalidState);EXPECT_EQ(f.observer->reads,before);
  f.operation.reset();const auto& complete=std::get<a::ResponseCompletion>(result);EXPECT_EQ(complete.initial_checkpoint(),rs::util::Deadline{100us});
  EXPECT_EQ(complete.transport_completed(),rs::util::Deadline{200us});EXPECT_EQ(complete.processing_completed(),rs::util::Deadline{300us});
  EXPECT_TRUE(complete.request().binding().source().identity=="synthetic");EXPECT_EQ(complete.request().deadline(),rs::util::Deadline{10ms});
}
