#include <gtest/gtest.h>
#include "odbcpp/auth/aws/provisioned_sdk_runtime.h"
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/client/ClientConfiguration.h>
#include <cstdlib>
#include <cstring>
#include <latch>
#include <thread>
using namespace std::chrono_literals;
namespace a=rs::core::auth;namespace n=a::aws::provisioned_native;
namespace {
class Observer final:public a::ResponseObservationSource {
 public:
  rs::util::Deadline now{10s};unsigned reads{};std::function<void()> hook;
  a::ResponseClockRead read_monotonic()override{++reads;if(hook)hook();return now;}
  bool cancellation_requested()override{return false;}
};
a::SecretBytes secret(std::string_view s){auto x=a::SecretBytes::create(std::as_bytes(std::span{s.data(),s.size()}));if(!x)std::terminate();return std::move(x).value();}
n::FrozenSigningSource signing(std::string generation="g1"){
 auto made=n::FrozenSigningSource::create("synthetic-source",std::move(generation),secret("synthetic-access-key"),secret("synthetic-secret-key"),secret(""));
 if(!std::holds_alternative<n::FrozenSigningSource>(made))std::terminate();return std::get<n::FrozenSigningSource>(std::move(made));
}
struct Input {
 n::RuntimeFixtureSource selected; n::Context context;
 std::shared_ptr<Observer> observer=std::make_shared<Observer>();
 std::shared_ptr<n::WorkerCancellation> cancel=std::make_shared<n::WorkerCancellation>();
 std::shared_ptr<a::ResponseSourceGeneration> generation;
 explicit Input(n::RuntimeFixtureSource choice=n::RuntimeFixtureSource::First):selected(choice),context(n::FixedRuntimeFixture::context(choice)){
  auto made=a::ResponseSourceGeneration::create(n::FixedFixture::request(context).binding());
  if(!std::holds_alternative<std::shared_ptr<a::ResponseSourceGeneration>>(made))std::terminate();generation=std::get<std::shared_ptr<a::ResponseSourceGeneration>>(std::move(made));
 }
 n::CreateOutcome make(n::ProvisionedSdkRuntime& runtime,n::FixedCase fixed=n::FixedCase::Happy){return n::FixedRuntimeFixture::create(runtime,fixed,observer,generation,cancel,selected);}
};
std::unique_ptr<n::ProvisionedSdkRuntime> runtime(){auto made=n::ProvisionedSdkRuntime::create();if(!made.runtime)throw std::runtime_error("Fixed runtime startup refused");return std::move(made.runtime);}
void finish(n::CreateOutcome& created){if(created.owner){EXPECT_TRUE(created.owner->close());}}
}
TEST(ProvisionedSdkRuntime, SingleLifetimeTwoDistinctOwningOperations){
 auto sdk=runtime();EXPECT_EQ(1U,sdk->counts().initializations);EXPECT_EQ(0U,sdk->counts().shutdowns);
 std::string saved;std::int64_t expiry{};
 for(auto source:{n::RuntimeFixtureSource::First,n::RuntimeFixtureSource::Second}){
  Input input(source);auto made=input.make(*sdk);ASSERT_TRUE(made.owner);auto acquired=made.owner->acquire_observation();ASSERT_TRUE(acquired.observation);
  EXPECT_TRUE(acquired.counts.request_exact);EXPECT_TRUE(acquired.counts.request_policy);EXPECT_EQ(1U,acquired.counts.selected_sends);
  EXPECT_EQ(0U,acquired.counts.initializations);EXPECT_EQ(0U,acquired.counts.shutdowns);EXPECT_TRUE(acquired.counts.cleanup_before_c);
  EXPECT_TRUE(acquired.observation->bound_request()->binding().source().generation==input.context.source_generation);
  EXPECT_TRUE(acquired.observation->with_fields([&](const auto& fields){saved=fields.user;expiry=fields.expiry.microseconds_since_epoch;EXPECT_FALSE(fields.password.empty());}));
  EXPECT_TRUE(acquired.observation->close());EXPECT_EQ(1U,acquired.observation->counts().n_samples);finish(made);
  // Closed observation counts remain owning after owner cleanup/slot release.
  EXPECT_EQ(1U,acquired.observation->counts().n_samples);EXPECT_FALSE(sdk->counts().occupied);
 }
 EXPECT_TRUE(saved=="IAM:fixture_user");EXPECT_EQ(1900000000125001LL,expiry);
 EXPECT_TRUE(sdk->close());EXPECT_EQ(1U,sdk->counts().shutdowns);EXPECT_TRUE(sdk->close());EXPECT_EQ(1U,sdk->counts().shutdowns);
}
TEST(ProvisionedSdkRuntime, WorkerOperationDoesNotChangeEnvironment){
 auto sdk=runtime();const std::string config=std::getenv("AWS_CONFIG_FILE"),credentials=std::getenv("AWS_SHARED_CREDENTIALS_FILE");
 bool succeeded=false,wrong_close=true;
 std::thread worker([&]{Input input;auto made=input.make(*sdk);if(!made.owner)return;wrong_close=sdk->close();auto acquired=made.owner->acquire_observation();succeeded=acquired.observation.has_value();if(acquired.observation){EXPECT_TRUE(acquired.observation->close());}finish(made);});worker.join();
 EXPECT_TRUE(succeeded);EXPECT_FALSE(wrong_close);EXPECT_TRUE(config==std::getenv("AWS_CONFIG_FILE"));EXPECT_TRUE(credentials==std::getenv("AWS_SHARED_CREDENTIALS_FILE"));
 EXPECT_EQ(1U,sdk->counts().initializations);EXPECT_EQ(0U,sdk->counts().shutdowns);EXPECT_TRUE(sdk->close());
}
TEST(ProvisionedSdkRuntime, OccupiedSlotRefusesBeforeObservationAndReleasesOnlyAfterOwner){
 auto sdk=runtime();std::latch ready{1},release{1};bool acquired_ok=false;
 std::thread worker([&]{Input input;auto made=input.make(*sdk);std::optional<n::Outcome> acquired;
  if(made.owner){acquired.emplace(made.owner->acquire_observation());acquired_ok=acquired->observation.has_value();}ready.count_down();release.wait();
  if(acquired&&acquired->observation){EXPECT_TRUE(acquired->observation->close());}finish(made);});
 ready.wait();EXPECT_TRUE(acquired_ok);EXPECT_TRUE(sdk->counts().occupied);EXPECT_FALSE(sdk->close());
 Input another;auto denied=another.make(*sdk);EXPECT_FALSE(denied.owner);EXPECT_EQ(n::Failure::Unavailable,denied.failure);EXPECT_EQ(0U,another.observer->reads);
 release.count_down();worker.join();EXPECT_FALSE(sdk->counts().occupied);
 Input last;auto made=last.make(*sdk);ASSERT_TRUE(made.owner);auto result=made.owner->acquire_observation();ASSERT_TRUE(result.observation);EXPECT_TRUE(result.observation->close());finish(made);EXPECT_TRUE(sdk->close());
}
TEST(ProvisionedSdkRuntime, InitAndUnboundFactoryRefuseWithoutPoisoningSelectedRequest){
 auto sdk=runtime();EXPECT_GE(sdk->counts().idle_clients,1U);
 {
  Aws::Client::ClientConfigurationInitValues init;init.shouldDisableIMDS=true;Aws::Client::ClientConfiguration config(init);config.disableIMDS=true;
  auto client=Aws::Http::CreateHttpClient(config);auto request=Aws::Http::CreateHttpRequest(Aws::String("https://foreign.fixture.invalid/"),Aws::Http::HttpMethod::HTTP_POST,[]()->Aws::IOStream*{return nullptr;});
  auto response=client->MakeRequest(request,nullptr,nullptr);ASSERT_TRUE(response);EXPECT_TRUE(response->HasClientError());
 }
 EXPECT_GE(sdk->counts().idle_requests,1U);
 Input input;auto made=input.make(*sdk,n::FixedCase::ForeignThenSelected);ASSERT_TRUE(made.owner);auto result=made.owner->acquire_observation();ASSERT_TRUE(result.observation);
 EXPECT_EQ(1U,result.counts.foreign_requests);EXPECT_EQ(1U,result.counts.safe_refusals);EXPECT_EQ(1U,result.counts.selected_sends);EXPECT_TRUE(result.counts.model_success);
 EXPECT_TRUE(result.observation->close());finish(made);EXPECT_TRUE(sdk->close());
}
TEST(ProvisionedSdkRuntime, ClosedBoundedSourcesAndExactBindingRefuseNoIo){
 for(auto pair:{std::pair{std::string_view(""),std::string_view("key")},std::pair{std::string_view("key"),std::string_view("")},std::pair{std::string_view("k\0x",3),std::string_view("key")}}){
  auto invalid=n::FrozenSigningSource::create("synthetic-source","g1",secret(pair.first),secret(pair.second),secret(""));EXPECT_TRUE(std::holds_alternative<n::Failure>(invalid));
 }
 auto large=n::FrozenSigningSource::create("synthetic-source","g1",secret(std::string(4097,'x')),secret("key"),secret(""));EXPECT_TRUE(std::holds_alternative<n::Failure>(large));
 auto sdk=runtime();Input input;
 auto wrong=sdk->create_operation(input.context,n::FixedFixture::request(input.context),input.observer,input.generation,input.cancel,signing("g2"));
 EXPECT_FALSE(wrong.owner);EXPECT_EQ(n::Failure::InvalidSource,wrong.failure);EXPECT_EQ(0U,input.observer->reads);EXPECT_FALSE(sdk->counts().occupied);
 auto original=signing();auto moved=std::move(original);
 auto empty=sdk->create_operation(input.context,n::FixedFixture::request(input.context),input.observer,input.generation,input.cancel,std::move(original));
 EXPECT_EQ(n::Failure::InvalidSource,empty.failure);EXPECT_EQ(0U,input.observer->reads);
 auto context=n::Context::create("000000000000","eu-north-1","other-cluster","fixture_db","fixture_user","db.fixture.invalid","synthetic-source","g1");ASSERT_TRUE(std::holds_alternative<n::Context>(context));
 auto mismatch=sdk->create_operation(std::get<n::Context>(std::move(context)),n::FixedFixture::request(input.context),input.observer,input.generation,input.cancel,std::move(moved));
 EXPECT_EQ(n::Failure::InvalidContext,mismatch.failure);EXPECT_EQ(0U,input.observer->reads);EXPECT_FALSE(sdk->counts().occupied);
 EXPECT_TRUE(sdk->close());auto closed=input.make(*sdk);EXPECT_EQ(n::Failure::Unavailable,closed.failure);EXPECT_EQ(0U,input.observer->reads);
}
TEST(ProvisionedSdkRuntime, OriginalDeadlineCancellationRollbackRetirementDoNotHeal){
 auto sdk=runtime();
 {Input input;input.observer->now=rs::util::Deadline{20s};auto made=input.make(*sdk);EXPECT_FALSE(made.owner);EXPECT_EQ(n::Failure::Refused,made.failure);EXPECT_FALSE(sdk->counts().occupied);}
 {Input input;input.cancel->cancel();auto made=input.make(*sdk);EXPECT_FALSE(made.owner);EXPECT_EQ(n::Failure::Refused,made.failure);}
 {Input input;auto made=input.make(*sdk);ASSERT_TRUE(made.owner);input.observer->now=rs::util::Deadline{9s};auto refused=made.owner->acquire_observation();EXPECT_FALSE(refused.observation);EXPECT_EQ(a::BoundaryFailure::ClockRollback,refused.counts.boundary);input.observer->now=rs::util::Deadline{11s};input.cancel->cancel();EXPECT_EQ(a::BoundaryFailure::ClockRollback,made.owner->counts().boundary);finish(made);}
 {Input input;auto made=input.make(*sdk);ASSERT_TRUE(made.owner);auto result=made.owner->acquire_observation();ASSERT_TRUE(result.observation);
  const auto passed=result.observation->with_fields([&](const auto&){input.observer->now=rs::util::Deadline{20s};});EXPECT_FALSE(passed);EXPECT_TRUE(result.counts.model_success);EXPECT_EQ(a::BoundaryFailure::DeadlineElapsed,result.observation->counts().boundary);
  input.observer->now=rs::util::Deadline{11s};EXPECT_FALSE(result.observation->with_fields([](const auto&){}));EXPECT_TRUE(result.observation->close());finish(made);}
 {Input input;auto made=input.make(*sdk);ASSERT_TRUE(made.owner);(void)input.generation->retire(a::SourceRetirement::Closed);auto refused=made.owner->acquire_observation();EXPECT_FALSE(refused.observation);EXPECT_EQ(a::BoundaryFailure::SourceClosed,refused.counts.boundary);finish(made);}
 EXPECT_FALSE(sdk->counts().occupied);EXPECT_TRUE(sdk->close());
}
TEST(ProvisionedSdkRuntime, RefusalQuiescenceAndReadExceptionAllowFreshOperation){
 auto sdk=runtime();
 for(auto fixed:{n::FixedCase::AdoptionFailure,n::FixedCase::ServiceDenied,n::FixedCase::LateAfterModel}){
  SCOPED_TRACE(static_cast<unsigned>(fixed));Input input;auto made=input.make(*sdk,fixed);ASSERT_TRUE(made.owner);auto result=made.owner->acquire_observation();EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);
  EXPECT_EQ(0U,result.counts.active);EXPECT_EQ(result.counts.wrappers,result.counts.destroyed_wrappers);EXPECT_EQ(1U,result.counts.delegate_destructions);finish(made);EXPECT_FALSE(sdk->counts().occupied);
 }
 {Input input;input.observer->hook=[](){throw std::runtime_error("SYNTHETIC_SECRET_DO_NOT_PRINT");};auto made=input.make(*sdk);EXPECT_FALSE(made.owner);EXPECT_EQ(n::Failure::Refused,made.failure);EXPECT_FALSE(sdk->counts().occupied);}
 Input input;auto made=input.make(*sdk);ASSERT_TRUE(made.owner);auto result=made.owner->acquire_observation();ASSERT_TRUE(result.observation);EXPECT_TRUE(result.observation->close());finish(made);EXPECT_TRUE(sdk->close());
}
TEST(ProvisionedSdkRuntime, DestructionGuardsAreFatalBeforeOwnedStateFreed){
 // Local injection exercises our failure boundary, not a real SDK allocation
 // failure/rollback. The parent has no initialized SDK before each death child.
 auto before=n::FixedRuntimeFixture::create_init_failure(n::RuntimeInitFixture::BeforeEntry);
 EXPECT_FALSE(before.runtime);EXPECT_EQ(n::Failure::LocalFailure,before.failure);
 {auto recovered=runtime();EXPECT_EQ(1U,recovered->counts().initializations);EXPECT_TRUE(recovered->close());}
 EXPECT_DEATH({auto refused=n::FixedRuntimeFixture::create_init_failure(n::RuntimeInitFixture::AfterEntry);(void)refused;},".*");
 EXPECT_DEATH({auto sdk=runtime();Input input;auto made=input.make(*sdk);if(!made.owner)std::terminate();sdk.reset();},".*");
 EXPECT_DEATH({auto sdk=runtime();std::thread wrong([value=std::move(sdk)]()mutable{value.reset();});wrong.join();},".*");
 EXPECT_DEATH({auto sdk=runtime();Input input;auto made=input.make(*sdk);if(!made.owner)std::terminate();input.observer->hook=[&](){made.owner.reset();};(void)made.owner->acquire_observation();},".*");
}
