#include <gtest/gtest.h>
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include <chrono>
#include <thread>
using namespace std::chrono_literals;
namespace a=rs::core::auth;
namespace p=a::aws::provisioned_native;
namespace {
class Observer final:public a::ResponseObservationSource {
 public:a::ResponseClockRead read_monotonic()override{++reads;if(returned && returned())return rs::util::Deadline{20s};return now;}
  bool cancellation_requested()override{return false;}
  rs::util::Deadline now{10s};unsigned reads{};std::function<bool()> returned;
};
struct FixtureSetup {
 std::shared_ptr<Observer> observer=std::make_shared<Observer>();
 std::shared_ptr<p::WorkerCancellation> cancel=std::make_shared<p::WorkerCancellation>();
 std::shared_ptr<a::ResponseSourceGeneration> generation;
 FixtureSetup(){auto request=p::FixedFixture::request(p::FixedFixture::context());auto made=a::ResponseSourceGeneration::create(request.binding());if(!std::holds_alternative<std::shared_ptr<a::ResponseSourceGeneration>>(made))std::terminate();generation=std::get<std::shared_ptr<a::ResponseSourceGeneration>>(std::move(made));}
 p::CreateOutcome make(p::FixedCase scenario){return p::FixedFixture::create(scenario,observer,generation,cancel);}
};
void refused(p::FixedCase scenario,a::BoundaryFailure expected=a::BoundaryFailure::BodyRejected){
 FixtureSetup setup;auto created=setup.make(scenario);ASSERT_TRUE(created.owner);auto result=created.owner->acquire_observation();
 EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);EXPECT_TRUE(result.counts.safe_error);
 EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_EQ(result.counts.boundary,expected);
 EXPECT_EQ(result.counts.delegate_returns,1U);EXPECT_EQ(result.counts.delegate_destructions,1U);
 EXPECT_EQ(result.counts.active,0U);EXPECT_EQ(result.counts.wrappers,result.counts.destroyed_wrappers);
 EXPECT_TRUE(created.owner->close());EXPECT_EQ(created.owner->counts().shutdowns,1U);
}
}
TEST(ProvisionedNativeOwner, ExactQueryFrozenSourceAndMicrosecondObservation){
 FixtureSetup setup;auto created=setup.make(p::FixedCase::Happy);ASSERT_TRUE(created.owner);
 auto result=created.owner->acquire_observation();ASSERT_TRUE(result.observation);EXPECT_FALSE(result.failure);
 const auto counts=result.counts;EXPECT_TRUE(counts.request_exact);EXPECT_TRUE(counts.request_policy);EXPECT_TRUE(counts.model_success);EXPECT_TRUE(counts.model_user_matches);EXPECT_TRUE(counts.model_password_matches);EXPECT_TRUE(counts.bootstrap_matches);
 EXPECT_EQ(counts.selected_sends,1U);EXPECT_GT(counts.frozen_provider_reads,0U);EXPECT_GT(counts.validation_bytes,0U);EXPECT_GT(counts.model_bytes,0U);
 EXPECT_EQ(counts.clients,counts.destroyed_clients);EXPECT_EQ(counts.shutdowns,1U);EXPECT_EQ(counts.c_samples,1U);EXPECT_EQ(counts.n_samples,0U);EXPECT_TRUE(counts.cleanup_before_c);EXPECT_TRUE(counts.unknown_only);
 std::int64_t expiry{};bool password=false;
 ASSERT_TRUE(result.observation->with_fields([&](const a::ExtractedDbFields& fields){EXPECT_EQ(fields.user,"IAM:fixture_user");expiry=fields.expiry.microseconds_since_epoch;fields.password.with_bytes([&](auto bytes){password=bytes.size()==14;});}));
 EXPECT_TRUE(password);EXPECT_EQ(expiry,1900000000125001LL);ASSERT_TRUE(counts.model_expiry_ms);
 EXPECT_EQ(*counts.model_expiry_ms,1900000000125LL);EXPECT_EQ(expiry-*counts.model_expiry_ms*1000,1LL);
 EXPECT_FALSE(created.owner->close());ASSERT_TRUE(result.observation->close());EXPECT_EQ(result.observation->counts().n_samples,1U);
 EXPECT_TRUE(created.owner->close());EXPECT_EQ(created.owner->counts().shutdowns,1U);
}
TEST(ProvisionedNativeOwner, NullAndForeignFallbackDoNotConsumeSelectedSlot){
 for(auto scenario:{p::FixedCase::NullThenSelected,p::FixedCase::ForeignThenSelected}){
  SCOPED_TRACE(static_cast<unsigned>(scenario));FixtureSetup setup;auto created=setup.make(scenario);ASSERT_TRUE(created.owner);
  auto result=created.owner->acquire_observation();ASSERT_TRUE(result.observation);EXPECT_EQ(result.counts.safe_refusals,1U);
  EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_EQ(result.counts.null_requests+result.counts.foreign_requests,1U);
  EXPECT_TRUE(result.counts.model_success);EXPECT_GT(result.counts.model_bytes,0U);EXPECT_TRUE(result.observation->close());EXPECT_TRUE(created.owner->close());
 }
}
TEST(ProvisionedNativeOwner, ReturnBeforeTailCannotFreezeUntilDedicatedDestruction){
 FixtureSetup setup;auto created=setup.make(p::FixedCase::Happy);ASSERT_TRUE(created.owner);auto result=created.owner->acquire_observation();ASSERT_TRUE(result.observation);
 EXPECT_TRUE(result.counts.return_before_tail);EXPECT_TRUE(result.counts.barrier);EXPECT_EQ(result.counts.delegate_returns,1U);EXPECT_EQ(result.counts.delegate_destructions,1U);
 EXPECT_EQ(result.counts.active,0U);EXPECT_EQ(result.counts.wrappers,1U);EXPECT_EQ(result.counts.destroyed_wrappers,1U);
 EXPECT_TRUE(result.observation->close());EXPECT_TRUE(created.owner->close());
}
TEST(ProvisionedNativeOwner, GrammarPrincipalAndMetadataRefuseBeforeModel){
 for(auto scenario:{p::FixedCase::DuplicateUser,p::FixedCase::WrongUser,p::FixedCase::WrongEnvelope,p::FixedCase::WrongMime,p::FixedCase::Non200,p::FixedCase::ForeignResponse}){
  SCOPED_TRACE(static_cast<unsigned>(scenario));refused(scenario);
 }
}
TEST(ProvisionedNativeOwner, BoundedStorageSeekFlagsAndAdoptionOwnership){
 {FixtureSetup setup;auto created=setup.make(p::FixedCase::ExactCap);ASSERT_TRUE(created.owner);auto result=created.owner->acquire_observation();ASSERT_TRUE(result.observation);
  EXPECT_EQ(result.counts.validation_bytes,131072U);EXPECT_TRUE(result.counts.rewound);EXPECT_TRUE(result.counts.put_preserved);
  EXPECT_TRUE(result.observation->close());EXPECT_TRUE(created.owner->close());}
 for(auto scenario:{p::FixedCase::Overflow,p::FixedCase::BadFlags,p::FixedCase::SeekFailure,p::FixedCase::NullResponse,p::FixedCase::AdoptionFailure}){
  SCOPED_TRACE(static_cast<unsigned>(scenario));refused(scenario);
 }
}
TEST(ProvisionedNativeOwner, LateCaptureFailureCannotPublishEvenAfterModelSuccess){
 refused(p::FixedCase::LateAfterValidation);
 FixtureSetup setup;auto created=setup.make(p::FixedCase::LateAfterModel);ASSERT_TRUE(created.owner);auto result=created.owner->acquire_observation();
 EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);EXPECT_TRUE(result.counts.model_success);EXPECT_GT(result.counts.model_bytes,0U);
 EXPECT_EQ(result.counts.boundary,a::BoundaryFailure::BodyRejected);EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_TRUE(created.owner->close());
}
TEST(ProvisionedNativeOwner, OriginalDeadlineCancellationRollbackAndRetirementRemainTerminal){
 for(unsigned kind=0;kind<4;++kind){
  SCOPED_TRACE(kind);FixtureSetup setup;auto created=setup.make(p::FixedCase::Happy);ASSERT_TRUE(created.owner);
  a::BoundaryFailure expected=a::BoundaryFailure::DeadlineElapsed;
  if(kind==0)setup.observer->now=rs::util::Deadline{20s};
  if(kind==1){setup.cancel->cancel();expected=a::BoundaryFailure::Cancelled;}
  if(kind==2){setup.observer->now=rs::util::Deadline{9s};expected=a::BoundaryFailure::ClockRollback;}
  if(kind==3){(void)setup.generation->retire(a::SourceRetirement::Closed);expected=a::BoundaryFailure::SourceClosed;}
  auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);EXPECT_EQ(result.counts.selected_sends,0U);
  setup.observer->now=rs::util::Deadline{10s};setup.cancel->cancel();auto again=created.owner->acquire_observation();EXPECT_FALSE(again.observation);EXPECT_EQ(again.counts.selected_sends,0U);
  EXPECT_TRUE(created.owner->close());EXPECT_EQ(created.owner->counts().boundary,expected);
 }
 {FixtureSetup setup;auto created=setup.make(p::FixedCase::Happy);ASSERT_TRUE(created.owner);
  setup.observer->returned=[&]{return created.owner->counts().delegate_returns==1;};
  auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);
  EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_EQ(result.counts.boundary,a::BoundaryFailure::DeadlineElapsed);
  setup.observer->returned={};EXPECT_TRUE(created.owner->close());}
 refused(p::FixedCase::CancelInWorker,a::BoundaryFailure::Cancelled);
 {FixtureSetup setup;setup.observer->now=rs::util::Deadline{20s};auto created=setup.make(p::FixedCase::Happy);EXPECT_FALSE(created.owner);EXPECT_TRUE(created.failure);EXPECT_EQ(created.counts.initializations,0U);}
}
TEST(ProvisionedNativeOwner, SingleConsumeMoveOutstandingBorrowAndReentryFence){
 {FixtureSetup setup;auto created=setup.make(p::FixedCase::Happy);ASSERT_TRUE(created.owner);unsigned before=setup.observer->reads;std::optional<p::Failure> wrong;
  std::thread worker([&]{wrong=created.owner->acquire_observation().failure;});worker.join();EXPECT_EQ(wrong,p::Failure::WrongThread);EXPECT_EQ(setup.observer->reads,before);
  auto result=created.owner->acquire_observation();ASSERT_TRUE(result.observation);
  p::Observation moved{std::move(*result.observation)};EXPECT_FALSE(result.observation->with_fields([](const auto&){}));
  auto again=created.owner->acquire_observation();EXPECT_EQ(again.failure,p::Failure::Consumed);EXPECT_EQ(again.counts.selected_sends,1U);
  EXPECT_FALSE(created.owner->close());EXPECT_TRUE(moved.close());EXPECT_FALSE(moved.close());EXPECT_TRUE(created.owner->close());
  EXPECT_EQ(created.owner->counts().clients,created.owner->counts().destroyed_clients);}
 {FixtureSetup setup;auto created=setup.make(p::FixedCase::Reentry);ASSERT_TRUE(created.owner);auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);
  EXPECT_EQ(result.counts.selected_sends,0U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_EQ(result.counts.safe_refusals,2U);EXPECT_TRUE(created.owner->close());}
 auto invalid=p::Context::create("000000000000","AWS_GLOBAL","cluster","db","user","db.invalid","source","g1");EXPECT_TRUE(std::holds_alternative<p::Failure>(invalid));
}

TEST(ProvisionedNativeOwner, ClosedResponseGateDiagnosticsPreserveRefusalAndPrecedence){
 struct Sample {p::FixedCase scenario;p::ResponseGate gate;p::ResponseStatus status;std::optional<bool> client_error;};
 for(const auto& sample:{Sample{p::FixedCase::WrongMime,p::ResponseGate::Mime,p::ResponseStatus::Success200,false},
     Sample{p::FixedCase::BadFlags,p::ResponseGate::StreamFlags,p::ResponseStatus::Success200,false},
     Sample{p::FixedCase::SeekFailure,p::ResponseGate::BodySeek,p::ResponseStatus::Success200,false},
     Sample{p::FixedCase::Non200,p::ResponseGate::Non200,p::ResponseStatus::Client4xx,false},
     Sample{p::FixedCase::ForeignResponse,p::ResponseGate::ForeignResponse,p::ResponseStatus::Unobserved,{}},
     Sample{p::FixedCase::NullResponse,p::ResponseGate::MissingResponse,p::ResponseStatus::Unobserved,{}},
     Sample{p::FixedCase::Overflow,p::ResponseGate::CaptureMissing,p::ResponseStatus::Unobserved,{}},
     Sample{p::FixedCase::AdoptionFailure,p::ResponseGate::CaptureMissing,p::ResponseStatus::Unobserved,{}}}){
  SCOPED_TRACE(static_cast<unsigned>(sample.scenario));FixtureSetup setup;auto created=setup.make(sample.scenario);ASSERT_TRUE(created.owner);
  auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);
  EXPECT_EQ(result.counts.first_response_gate,sample.gate);EXPECT_EQ(result.counts.response_status,sample.status);
  EXPECT_EQ(result.counts.response_client_error,sample.client_error);
  EXPECT_EQ(result.counts.validation_bytes,0U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_FALSE(result.counts.model_success);
  EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_EQ(result.counts.delegate_returns,1U);EXPECT_EQ(result.counts.delegate_destructions,1U);
  EXPECT_EQ(result.counts.active,0U);EXPECT_EQ(result.counts.wrappers,result.counts.destroyed_wrappers);
  EXPECT_TRUE(created.owner->close());EXPECT_EQ(created.owner->counts().first_response_gate,sample.gate);
 }
 FixtureSetup setup;auto created=setup.make(p::FixedCase::Happy);ASSERT_TRUE(created.owner);
 auto result=created.owner->acquire_observation();ASSERT_TRUE(result.observation);
 EXPECT_EQ(result.counts.first_response_gate,p::ResponseGate::None);
 EXPECT_EQ(result.counts.response_status,p::ResponseStatus::Success200);EXPECT_EQ(result.counts.response_client_error,false);
 EXPECT_TRUE(result.counts.model_success);EXPECT_GT(result.counts.validation_bytes,0U);EXPECT_GT(result.counts.model_bytes,0U);
 EXPECT_TRUE(result.observation->close());EXPECT_TRUE(created.owner->close());
}

TEST(ProvisionedNativeOwner, ProviderCategoriesKeepServiceRefusalAndSecretPrivacy){
 struct Sample {p::FixedCase scenario;p::ProviderFailure failure;};
 for(const auto& sample:{Sample{p::FixedCase::ServiceDenied,p::ProviderFailure::AccessDenied},
     Sample{p::FixedCase::ServiceExpired,p::ProviderFailure::ExpiredCredentials},
     Sample{p::FixedCase::ServiceInvalidToken,p::ProviderFailure::InvalidCredentials},
     Sample{p::FixedCase::ServiceRequestExpired,p::ProviderFailure::InvalidRequest},
     Sample{p::FixedCase::ServiceInvalid,p::ProviderFailure::InvalidRequest},
     Sample{p::FixedCase::ServiceThrottled,p::ProviderFailure::Throttled}}){
  SCOPED_TRACE(static_cast<unsigned>(sample.scenario));FixtureSetup setup;auto created=setup.make(sample.scenario);ASSERT_TRUE(created.owner);
  auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);
  EXPECT_EQ(result.counts.provider_failure,sample.failure);EXPECT_EQ(result.counts.first_response_gate,p::ResponseGate::Non200);
  EXPECT_EQ(result.counts.boundary,a::BoundaryFailure::BodyRejected);EXPECT_TRUE(result.counts.safe_error);
  EXPECT_GT(result.counts.error_diagnostic_bytes,0U);EXPECT_LE(result.counts.error_diagnostic_bytes,128U*1024U);
  EXPECT_EQ(result.counts.validation_bytes,0U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_FALSE(result.counts.model_success);
  EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_EQ(result.counts.active,0U);EXPECT_EQ(result.counts.wrappers,result.counts.destroyed_wrappers);
  EXPECT_TRUE(created.owner->close());EXPECT_EQ(created.owner->counts().provider_failure,sample.failure);
 }
}
TEST(ProvisionedNativeOwner, MalformedDuplicateMissingAndUnknownCodesNeverGuessAccessDenied){
 for(auto scenario:{p::FixedCase::ServiceDuplicate,p::FixedCase::ServiceMissing,p::FixedCase::ServiceMalformed,
      p::FixedCase::ServiceUnknown,p::FixedCase::ServiceTwoErrors,p::FixedCase::ServiceNestedCode,
      p::FixedCase::ServiceDtd,p::FixedCase::ServiceLongCode,p::FixedCase::Non200}){
  SCOPED_TRACE(static_cast<unsigned>(scenario));FixtureSetup setup;auto created=setup.make(scenario);ASSERT_TRUE(created.owner);
  auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);EXPECT_TRUE(result.failure);
  EXPECT_EQ(result.counts.provider_failure,p::ProviderFailure::Unknown);EXPECT_EQ(result.counts.first_response_gate,p::ResponseGate::Non200);
  EXPECT_EQ(result.counts.validation_bytes,0U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_TRUE(result.counts.safe_error);
  EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_TRUE(created.owner->close());
 }
}
TEST(ProvisionedNativeOwner, TypedTransportErrorsNeverReadSecretBearingMessageOrBody){
 struct Sample {p::FixedCase scenario;p::ProviderFailure failure;};
 for(const auto& sample:{Sample{p::FixedCase::TransportError,p::ProviderFailure::Transport},
     Sample{p::FixedCase::TransportTimeout,p::ProviderFailure::Timeout},
     Sample{p::FixedCase::TransportCancelled,p::ProviderFailure::Cancelled}}){
  FixtureSetup setup;auto created=setup.make(sample.scenario);ASSERT_TRUE(created.owner);
  auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);EXPECT_EQ(result.counts.provider_failure,sample.failure);
  EXPECT_EQ(result.counts.first_response_gate,p::ResponseGate::ClientError);EXPECT_EQ(result.counts.response_client_error,true);
  EXPECT_EQ(result.counts.error_diagnostic_bytes,0U);EXPECT_EQ(result.counts.validation_bytes,0U);EXPECT_EQ(result.counts.model_bytes,0U);
  EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_TRUE(result.counts.safe_error);EXPECT_TRUE(created.owner->close());
 }
}
TEST(ProvisionedNativeOwner, ProviderDiagnosticCannotOverrideOriginalDeadlineOrHappyOwnership){
 {FixtureSetup setup;auto created=setup.make(p::FixedCase::ServiceDenied);ASSERT_TRUE(created.owner);
  setup.observer->returned=[&]{return created.owner->counts().delegate_returns==1;};
  auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);
  EXPECT_EQ(result.counts.boundary,a::BoundaryFailure::DeadlineElapsed);EXPECT_EQ(result.counts.provider_failure,p::ProviderFailure::Timeout);
  EXPECT_EQ(result.counts.error_diagnostic_bytes,0U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_EQ(result.counts.selected_sends,1U);
  setup.observer->returned={};EXPECT_TRUE(created.owner->close());}
 {FixtureSetup setup;auto created=setup.make(p::FixedCase::Happy);ASSERT_TRUE(created.owner);
  auto result=created.owner->acquire_observation();ASSERT_TRUE(result.observation);EXPECT_FALSE(result.counts.provider_failure);
  EXPECT_EQ(result.counts.error_diagnostic_bytes,0U);EXPECT_TRUE(result.counts.model_success);
  EXPECT_TRUE(result.observation->with_fields([](const a::ExtractedDbFields& fields){
   EXPECT_EQ(fields.expiry.microseconds_since_epoch,1900000000125001LL);EXPECT_EQ(fields.user,"IAM:fixture_user");}));
  EXPECT_TRUE(result.observation->close());EXPECT_TRUE(created.owner->close());}
}

TEST(ProvisionedNativeOwner, DeadlineDuringErrorInspectionRetainsFaultAndNeverPublishesCategory){
 FixtureSetup setup;auto created=setup.make(p::FixedCase::ServiceDenied);ASSERT_TRUE(created.owner);
 setup.observer->returned=[&]{return created.owner->counts().error_diagnostic_bytes>0;};
 auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);
 EXPECT_EQ(result.counts.boundary,a::BoundaryFailure::DeadlineElapsed);
 EXPECT_EQ(result.counts.provider_failure,p::ProviderFailure::Unknown);
 EXPECT_EQ(result.counts.first_response_gate,p::ResponseGate::Non200);
 EXPECT_GT(result.counts.error_diagnostic_bytes,0U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_EQ(result.counts.validation_bytes,0U);
 EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_TRUE(result.counts.safe_error);
 setup.observer->returned={};EXPECT_TRUE(created.owner->close());
}

TEST(ProvisionedNativeOwner, MalformedLateDiagnosticChecksBoundaryBeforeTerminalBodyRefusal){
 FixtureSetup setup;auto created=setup.make(p::FixedCase::ServiceMalformed);ASSERT_TRUE(created.owner);
 unsigned diagnostic_checks=0;
 setup.observer->returned=[&]{
  return created.owner->counts().error_diagnostic_bytes>0 && ++diagnostic_checks>=2;
 };
 auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);
 EXPECT_EQ(diagnostic_checks,2U);EXPECT_EQ(result.counts.boundary,a::BoundaryFailure::DeadlineElapsed);
 EXPECT_EQ(result.counts.provider_failure,p::ProviderFailure::Unknown);
 EXPECT_EQ(result.counts.first_response_gate,p::ResponseGate::Non200);
 EXPECT_GT(result.counts.error_diagnostic_bytes,0U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_EQ(result.counts.validation_bytes,0U);
 EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_TRUE(result.counts.safe_error);
 setup.observer->returned={};EXPECT_TRUE(created.owner->close());
 EXPECT_EQ(created.owner->counts().boundary,a::BoundaryFailure::DeadlineElapsed);
}
TEST(ProvisionedNativeOwner, ErrorNamespaceMustBeEmptyOrExactRedshiftQueryNamespace){
 for(auto scenario:{p::FixedCase::ServiceWrongNamespace,p::FixedCase::ServiceWrongErrorNamespace,
      p::FixedCase::ServiceWrongCodeNamespace,p::FixedCase::ServiceQueryNamespace}){
  FixtureSetup setup;auto created=setup.make(scenario);ASSERT_TRUE(created.owner);
  auto result=created.owner->acquire_observation();EXPECT_FALSE(result.observation);
  EXPECT_EQ(result.counts.provider_failure,scenario==p::FixedCase::ServiceQueryNamespace?
      p::ProviderFailure::AccessDenied:p::ProviderFailure::Unknown);
  EXPECT_EQ(result.counts.first_response_gate,p::ResponseGate::Non200);EXPECT_EQ(result.counts.boundary,a::BoundaryFailure::BodyRejected);
  EXPECT_GT(result.counts.error_diagnostic_bytes,0U);EXPECT_EQ(result.counts.model_bytes,0U);EXPECT_EQ(result.counts.validation_bytes,0U);
  EXPECT_EQ(result.counts.selected_sends,1U);EXPECT_TRUE(result.counts.safe_error);EXPECT_TRUE(created.owner->close());
 }
}
