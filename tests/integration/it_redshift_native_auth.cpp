// Future separately admitted native acquisition/login only. No ordinary CI run.
#include <gtest/gtest.h>
#include "core/database/postgres/pg_database_connection.h"
#include "odbcpp/transport/tls_transport.h"
#include "odbcpp/auth/aws/named_profile_acquisition.h"
#include "odbcpp/auth/pg_credential_consumer.h"
#include "odbcpp/auth/aws/provisioned_pg_connector.h"
#include <chrono>
#include <cstdlib>
#include <limits>
#include <optional>
#include <locale>
#include <sstream>
#include <string>
namespace {
namespace a=rs::core::auth;
namespace n=a::aws::provisioned_native;
namespace d=rs::core::database;
using Pg=d::postgres::PgDatabaseConnection;
constexpr std::string_view admission="provisioned-native-acquisition-login001";
// Only closed names and existing finite counters: no SDK message, config,
// identity, raw response or model_expiry_ms may enter this diagnostic.
const char* safe_failure_name(n::Failure failure) noexcept {
 switch(failure){
  case n::Failure::InvalidContext:return "InvalidContext";
  case n::Failure::InvalidSource:return "InvalidSource";
  case n::Failure::MainEntryRequired:return "MainEntryRequired";
  case n::Failure::Unavailable:return "Unavailable";
  case n::Failure::Refused:return "Refused";
  case n::Failure::Consumed:return "Consumed";
  case n::Failure::WrongThread:return "WrongThread";
  case n::Failure::Reentrant:return "Reentrant";
  case n::Failure::LocalFailure:return "LocalFailure";
 }
 return "Unknown";
}
const char* safe_boundary_name(a::BoundaryFailure failure) noexcept {
 switch(failure){
  case a::BoundaryFailure::InvalidBoundary:return "InvalidBoundary";
  case a::BoundaryFailure::InvalidInput:return "InvalidInput";
  case a::BoundaryFailure::InvalidState:return "InvalidState";
  case a::BoundaryFailure::WrongOperation:return "WrongOperation";
  case a::BoundaryFailure::WrongThread:return "WrongThread";
  case a::BoundaryFailure::ReadFailed:return "ReadFailed";
  case a::BoundaryFailure::ClockRollback:return "ClockRollback";
  case a::BoundaryFailure::DeadlineElapsed:return "DeadlineElapsed";
  case a::BoundaryFailure::Cancelled:return "Cancelled";
  case a::BoundaryFailure::Overflow:return "Overflow";
  case a::BoundaryFailure::StaleObservation:return "StaleObservation";
  case a::BoundaryFailure::UnknownQuality:return "UnknownQuality";
  case a::BoundaryFailure::Borrowed:return "Borrowed";
  case a::BoundaryFailure::SourceClosed:return "SourceClosed";
  case a::BoundaryFailure::SourceChanged:return "SourceChanged";
  case a::BoundaryFailure::UnsafeDiagnostic:return "UnsafeDiagnostic";
  case a::BoundaryFailure::InvalidDiagnostic:return "InvalidDiagnostic";
  case a::BoundaryFailure::StaleDiagnostic:return "StaleDiagnostic";
  case a::BoundaryFailure::ForeignDispatch:return "ForeignDispatch";
  case a::BoundaryFailure::BodyRejected:return "BodyRejected";
  case a::BoundaryFailure::AllocationFailed:return "AllocationFailed";
 }
 return "Unknown";
}
const char* safe_response_gate_name(n::ResponseGate gate) noexcept {
 switch(gate){
  case n::ResponseGate::None:return "None";
  case n::ResponseGate::CaptureMissing:return "CaptureMissing";
  case n::ResponseGate::CaptureFailure:return "CaptureFailure";
  case n::ResponseGate::Checkpoint:return "Checkpoint";
  case n::ResponseGate::MissingResponse:return "MissingResponse";
  case n::ResponseGate::ForeignResponse:return "ForeignResponse";
  case n::ResponseGate::Non200:return "Non200";
  case n::ResponseGate::ClientError:return "ClientError";
  case n::ResponseGate::Mime:return "Mime";
  case n::ResponseGate::Encoding:return "Encoding";
  case n::ResponseGate::BodyWrapper:return "BodyWrapper";
  case n::ResponseGate::StreamFlags:return "StreamFlags";
  case n::ResponseGate::PutSize:return "PutSize";
  case n::ResponseGate::BodySeek:return "BodySeek";
  case n::ResponseGate::CheckedStreamCreation:return "CheckedStreamCreation";
  case n::ResponseGate::BodyCopy:return "BodyCopy";
  case n::ResponseGate::CheckedStreamSeal:return "CheckedStreamSeal";
 }
 return "Unknown";
}
const char* safe_response_status_name(n::ResponseStatus status) noexcept {
 switch(status){
  case n::ResponseStatus::Unobserved:return "Unobserved";
  case n::ResponseStatus::Success200:return "Success200";
  case n::ResponseStatus::Other2xx:return "Other2xx";
  case n::ResponseStatus::Redirect3xx:return "Redirect3xx";
  case n::ResponseStatus::Client4xx:return "Client4xx";
  case n::ResponseStatus::Server5xx:return "Server5xx";
  case n::ResponseStatus::Other:return "Other";
 }
 return "Unknown";
}
enum class DiagnosticPhase { Creation, Acquisition };
const char* safe_provider_failure_name(n::ProviderFailure failure) noexcept {
  switch(failure) {
    case n::ProviderFailure::AccessDenied:return "AccessDenied";
    case n::ProviderFailure::ExpiredCredentials:return "ExpiredCredentials";
    case n::ProviderFailure::InvalidCredentials:return "InvalidCredentials";
    case n::ProviderFailure::InvalidRequest:return "InvalidRequest";
    case n::ProviderFailure::Throttled:return "Throttled";
    case n::ProviderFailure::Transport:return "Transport";
    case n::ProviderFailure::Timeout:return "Timeout";
    case n::ProviderFailure::Cancelled:return "Cancelled";
    case n::ProviderFailure::Unknown:return "Unknown";
  }
  return "Unknown";
}
std::string safe_counts_trace(DiagnosticPhase phase,const std::optional<n::Failure>& failure,
 const n::Counts& c){
 std::ostringstream out;out.imbue(std::locale::classic());
 out<<"native_auth "<<(phase==DiagnosticPhase::Creation?"creation":"acquisition")
    <<" failure="<<(failure?safe_failure_name(*failure):"None")
    <<" boundary="<<(c.boundary?safe_boundary_name(*c.boundary):"None")
    <<" response_gate="<<safe_response_gate_name(c.first_response_gate)
    <<" response_status="<<safe_response_status_name(c.response_status)
    <<" response_client_error="<<(c.response_client_error?(*c.response_client_error?"True":"False"):"Unobserved")
    <<" initializations="<<c.initializations<<" named_loads="<<c.named_provider_loads
    <<" source_ready="<<c.named_source_ready<<" frozen_reads="<<c.frozen_provider_reads
    <<" request_policy="<<c.request_policy<<" request_exact="<<c.request_exact
    <<" requests="<<c.requests<<" selected_sends="<<c.selected_sends
    <<" foreign_requests="<<c.foreign_requests<<" null_requests="<<c.null_requests
    <<" safe_refusals="<<c.safe_refusals<<" delegate_returns="<<c.delegate_returns
    <<" delegate_destructions="<<c.delegate_destructions<<" active="<<c.active
    <<" wrappers="<<c.wrappers<<" destroyed_wrappers="<<c.destroyed_wrappers
    <<" provider_failure="<<(c.provider_failure?safe_provider_failure_name(*c.provider_failure):"None")
    <<" error_diagnostic_bytes="<<c.error_diagnostic_bytes
    <<" validation_bytes="<<c.validation_bytes<<" model_bytes="<<c.model_bytes
    <<" barrier="<<c.barrier<<" rewound="<<c.rewound<<" put_preserved="<<c.put_preserved
    <<" model_success="<<c.model_success<<" safe_error="<<c.safe_error
    <<" model_user_matches="<<c.model_user_matches<<" model_password_matches="<<c.model_password_matches
    <<" shutdowns="<<c.shutdowns<<" cleanup_before_c="<<c.cleanup_before_c
    <<" c_samples="<<c.c_samples<<" n_samples="<<c.n_samples<<" unknown_only="<<c.unknown_only;
 auto text=out.str();if(text.size()>2048)return "native_auth diagnostic_limit";
 return text;
}

// Selection values only; no environment-supplied DB password or credentials.
struct NativeLoginSpec {
 std::string account,region,cluster,database,requested_user,sql_user,host,ca,
             profile,credentials,config,source,generation;
};
std::optional<std::string> selection(const char* key,std::size_t limit=256){
 const char* value=std::getenv(key);if(!value)return {};
 std::size_t size=0;while(size<=limit&&value[size]!='\0')++size;
 if(size==0||size>limit)return {};
 for(std::size_t i=0;i<size;++i){const auto c=static_cast<unsigned char>(value[i]);if(c<32||c==127)return {};}
 return std::string{value,size};
}
std::optional<NativeLoginSpec> read_spec(){
 const auto account=selection("ODBCPP_REDSHIFT_NATIVE_ACCOUNT");
 const auto region=selection("ODBCPP_REDSHIFT_NATIVE_REGION");
 const auto cluster=selection("ODBCPP_REDSHIFT_NATIVE_CLUSTER");
 const auto database=selection("ODBCPP_REDSHIFT_NATIVE_DATABASE");
 const auto requested=selection("ODBCPP_REDSHIFT_NATIVE_REQUESTED_DB_USER");
 const auto principal=selection("ODBCPP_REDSHIFT_NATIVE_EXPECTED_SQL_USER");
 const auto host=selection("ODBCPP_REDSHIFT_NATIVE_DB_HOST");
 const auto ca=selection("ODBCPP_REDSHIFT_NATIVE_DB_CA_FILE",4096);
 const auto profile=selection("ODBCPP_REDSHIFT_NATIVE_PROFILE");
 const auto credentials=selection("ODBCPP_REDSHIFT_NATIVE_CREDENTIALS_FILE",4096);
 const auto config=selection("ODBCPP_REDSHIFT_NATIVE_CONFIG_FILE",4096);
 const auto source=selection("ODBCPP_REDSHIFT_NATIVE_SOURCE_IDENTITY");
 const auto generation=selection("ODBCPP_REDSHIFT_NATIVE_SOURCE_GENERATION");
 if(!account||!region||!cluster||!database||!requested||!principal||!host||!ca||!profile||!credentials||!config||!source||!generation)return {};
 if(ca->front()!='/'||credentials->front()!='/'||config->front()!='/')return {};
 return NativeLoginSpec{*account,*region,*cluster,*database,*requested,*principal,*host,*ca,*profile,*credentials,*config,*source,*generation};
}
class Observations final:public a::ResponseObservationSource {
 public:
 a::ResponseClockRead read_monotonic() override {
  try{const auto now=rs::util::Clock::now();if(now==rs::util::Deadline::min()||now==rs::util::Deadline::max())return a::ResponseReadFailure::ReadFailed;return now;}
  catch(...){return a::ResponseReadFailure::ReadFailed;}
 }
 bool cancellation_requested() override{return false;}
 // No UTC sample/calibration/eligibility: all quality remains Unknown.
};
struct Disconnect {
 Pg& connection;~Disconnect(){connection.disconnect();}
};
struct LoginOutcome {
 bool entered{},wire_identity{},password_present{},connected{},peer_verified{},disconnected{},unexpected_exception{},
      borrow_passed{},observation_closed{},raw_expiry_positive{};
 n::Counts observation_counts;
 std::optional<d::BackendResult<void>> connection;
 std::optional<d::BackendResult<d::QueryResult>> query;
};
void login_observation(n::Observation& observation,const NativeLoginSpec& spec,
 const std::string& expected_user,const a::Request& request,LoginOutcome& outcome) noexcept {
 try{
  d::ConnectionSettings settings;
  settings.host=spec.host;settings.port=5439;settings.database=spec.database;
  settings.user=expected_user;settings.use_ssl=true;settings.ssl_ca_file=spec.ca;
  auto transport=std::make_unique<rs::core::transport::TLSTransport>();auto* peer=transport.get();
  transport->set_verify(true);transport->set_hostname_verification(true);transport->set_ca_locations(spec.ca,"");
  Pg connection{std::move(transport),std::nullopt,d::postgres::PgCatalogProfile::Redshift};Disconnect disconnect{connection};
  auto composed=n::connect_provisioned_observation_until(connection,settings,observation);
  outcome.entered=composed.entered;outcome.wire_identity=composed.wire_identity;
  outcome.password_present=composed.password_present;outcome.raw_expiry_positive=composed.raw_expiry_positive;
  outcome.borrow_passed=composed.borrow_passed;outcome.observation_closed=composed.observation_closed;
  const bool composed_success=composed.succeeded();
  outcome.observation_counts=composed.counts;outcome.connection=std::move(composed.connection);
  outcome.unexpected_exception=composed.failure==n::ConnectorFailure::LocalFailure;
  if(!composed_success)return;
  outcome.connected=connection.is_connected();outcome.peer_verified=peer->peer_identity_verified();
  if(!outcome.connected||!outcome.peer_verified)return;
  outcome.query.emplace(connection.execute_query("SELECT current_database(), TRIM(current_user), CAST(1 AS INTEGER), CAST(NULL AS INTEGER)",request.deadline()));
  connection.disconnect();outcome.disconnected=!connection.is_connected()&&connection.session_state()==d::SessionState::Disconnected;
 }catch(...){outcome.unexpected_exception=true;}
 // No assertion, exception text or borrowed field can escape this function.
}
}
TEST(ProvisionedNativeAuth, GetClusterCredentialsThenVerifiedTlsLogin){
 // This is the FIRST action: absent/wrong marker performs no other work.
 const char* marker=std::getenv("ODBCPP_REDSHIFT_NATIVE_AUTH_ADMISSION");
 if(!marker||std::string_view(marker)!=admission){GTEST_SKIP()<<"Separate native acquisition/login admission required";}
 ASSERT_TRUE(std::getenv("ODBCPP_AUTH_SDK_OFFLINE_FIXTURE")==nullptr);
 auto spec=read_spec();ASSERT_TRUE(spec.has_value());
 auto context_result=n::Context::create(spec->account,spec->region,spec->cluster,spec->database,spec->requested_user,spec->host,spec->source,spec->generation);
 ASSERT_TRUE(std::holds_alternative<n::Context>(context_result));auto context=std::get<n::Context>(std::move(context_result));
 auto binding_result=a::Binding::create({a::Service::Redshift,spec->host,5439,spec->database,context.expected_user,context.resource_arn,spec->host,"verify-full"},{a::SourceKind::TrustedTemporaryDbIssuer,spec->source,spec->generation},a::Method::TemporaryDatabasePassword);
 ASSERT_TRUE(static_cast<bool>(binding_result));
 // ONE deadline before source/SDK work. Fixed60s, bounded checked addition.
 const auto start=rs::util::Clock::now();
 ASSERT_TRUE(start!=rs::util::Deadline::min()&&start!=rs::util::Deadline::max());
 const auto duration=std::chrono::duration_cast<rs::util::Clock::duration>(std::chrono::seconds{60});
 ASSERT_TRUE(start.time_since_epoch().count()<=std::numeric_limits<rs::util::Clock::rep>::max()-duration.count());
 const auto deadline=start+duration;
 auto request_result=a::Request::create(std::move(binding_result).value(),deadline,std::chrono::seconds{1});ASSERT_TRUE(static_cast<bool>(request_result));auto request=std::move(request_result).value();
 auto observer=std::make_shared<Observations>();auto cancellation=std::make_shared<n::WorkerCancellation>();
 auto generation_result=a::ResponseSourceGeneration::create(request.binding());ASSERT_TRUE(std::holds_alternative<std::shared_ptr<a::ResponseSourceGeneration>>(generation_result));auto generation=std::get<std::shared_ptr<a::ResponseSourceGeneration>>(std::move(generation_result));
 const auto login_request=request;
 const std::string expected_wire_user=context.expected_user;
 auto created=n::NativeOwner::create_from_named_source(std::move(context),std::move(request),observer,generation,cancellation,{spec->profile,spec->credentials,spec->config,spec->source,spec->generation});
 SCOPED_TRACE(safe_counts_trace(DiagnosticPhase::Creation,created.failure,created.counts));
 ASSERT_TRUE(created.owner!=nullptr);ASSERT_FALSE(created.failure.has_value());
 EXPECT_EQ(created.counts.initializations,1U);EXPECT_EQ(created.counts.named_provider_loads,1U);EXPECT_TRUE(created.counts.named_source_ready);
 auto acquired=created.owner->acquire_observation();
 SCOPED_TRACE(safe_counts_trace(DiagnosticPhase::Acquisition,acquired.failure,acquired.counts));
 ASSERT_TRUE(acquired.observation.has_value());ASSERT_FALSE(acquired.failure.has_value());
 const auto counts=acquired.counts;
 EXPECT_TRUE(counts.request_exact);EXPECT_TRUE(counts.request_policy);EXPECT_TRUE(counts.model_success);EXPECT_TRUE(counts.model_user_matches);EXPECT_TRUE(counts.model_password_matches);
 EXPECT_EQ(counts.selected_sends,1U);EXPECT_EQ(counts.delegate_returns,1U);EXPECT_EQ(counts.delegate_destructions,1U);EXPECT_EQ(counts.active,0U);EXPECT_EQ(counts.wrappers,counts.destroyed_wrappers);
 EXPECT_EQ(counts.named_provider_loads,1U);EXPECT_GT(counts.frozen_provider_reads,0U);EXPECT_EQ(counts.shutdowns,1U);EXPECT_TRUE(counts.cleanup_before_c);EXPECT_EQ(counts.c_samples,1U);EXPECT_EQ(counts.n_samples,0U);EXPECT_TRUE(counts.unknown_only);
 LoginOutcome login;
 login_observation(*acquired.observation,*spec,expected_wire_user,login_request,login);
 const bool borrow_passed=login.borrow_passed;const bool raw_expiry_positive=login.raw_expiry_positive;
 EXPECT_TRUE(borrow_passed);EXPECT_TRUE(raw_expiry_positive);EXPECT_TRUE(login.entered);EXPECT_FALSE(login.unexpected_exception);EXPECT_TRUE(login.wire_identity);EXPECT_TRUE(login.password_present);EXPECT_TRUE(login.connected);EXPECT_TRUE(login.peer_verified);EXPECT_TRUE(login.disconnected);
 const bool observation_closed=login.observation_closed;EXPECT_TRUE(observation_closed);EXPECT_EQ(login.observation_counts.n_samples,1U);EXPECT_TRUE(created.owner->close());
 ASSERT_TRUE(login.connection.has_value());ASSERT_TRUE(login.connection->has_value());EXPECT_TRUE((login.connection->session_snapshot()==d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));
 ASSERT_TRUE(login.query.has_value());ASSERT_TRUE(login.query->has_value());EXPECT_TRUE((login.query->session_snapshot()==d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));
 const auto& result=login.query->value();EXPECT_FALSE(result.error.has_value());EXPECT_TRUE(result.additional_results.empty());EXPECT_TRUE(result.cell_errors.empty());ASSERT_EQ(result.columns.size(),4U);ASSERT_EQ(result.rows.size(),1U);ASSERT_EQ(result.rows[0].size(),4U);
 const bool sql_principal_matches_requested=result.rows[0][1].has_value()&&*result.rows[0][1]==spec->requested_user;
 const bool sql_principal_matches_wire=result.rows[0][1].has_value()&&*result.rows[0][1]==expected_wire_user;
 SCOPED_TRACE(std::string("sql_principal_matches_requested=")+(sql_principal_matches_requested?"1":"0")+
     " sql_principal_matches_wire="+(sql_principal_matches_wire?"1":"0"));
 // Owning values are asserted after disconnect and secret/Processing cleanup;
 // Boolean comparisons prevent actual principal/config values in failures.
 EXPECT_TRUE(result.rows[0][0].has_value()&&*result.rows[0][0]==spec->database);EXPECT_TRUE(result.rows[0][1].has_value()&&*result.rows[0][1]==spec->sql_user);EXPECT_TRUE(result.rows[0][2].has_value()&&*result.rows[0][2]=="1");EXPECT_FALSE(result.rows[0][3].has_value());
 for(auto column:{2U,3U}){ASSERT_TRUE(result.columns[column].normalized_type.has_value());EXPECT_TRUE(result.columns[column].normalized_type->known);EXPECT_EQ(result.columns[column].normalized_type->type,d::ScalarType::Integer);}
 // Independent exact-principal server cleanup is mandatory controller work.
 // No admin credentials, broad termination, accounting or self-admission here.
}


#include "odbcpp/auth/aws/provisioned_sdk_runtime.h"
#include <aws/core/config/AWSConfigFileProfileConfigLoader.h>
#include <array>
namespace {
// Test-local source bridge only. SDK cache reads stay on the already-selected
// empty config; the one explicit filename read uses retained owned descriptors.
class RuntimeNativeObserver final:public a::ResponseObservationSource {
 public:
  RuntimeNativeObserver(rs::util::Deadline deadline,std::shared_ptr<n::WorkerCancellation> cancel)
      :deadline_(deadline),cancel_(std::move(cancel)){}
  a::ResponseClockRead read_monotonic() override {
    if(failed_)return a::ResponseReadFailure::ReadFailed;
    try {
      const auto now=rs::util::Clock::now();
      if(now==rs::util::Deadline::min()||now==rs::util::Deadline::max()||
          (highwater_&&now<*highwater_)){failed_=true;return a::ResponseReadFailure::ReadFailed;}
      highwater_=now;return now;
    }catch(...){failed_=true;return a::ResponseReadFailure::ReadFailed;}
  }
  bool cancellation_requested() override {return cancel_->cancelled();}
  bool checkpoint() {
    const auto now=read_monotonic();
    if(!std::holds_alternative<rs::util::Deadline>(now)||cancel_->cancelled()||
        std::get<rs::util::Deadline>(now)>=deadline_){failed_=true;return false;}
    return true;
  }
 private:
  rs::util::Deadline deadline_;std::shared_ptr<n::WorkerCancellation> cancel_;
  std::optional<rs::util::Deadline> highwater_;bool failed_{};
};
enum class RuntimeSourceFailure { Checkpoint, StageChanged, Rewind, Load, Selection, Credentials, LocalFailure };
const char* runtime_source_failure_name(RuntimeSourceFailure failure) noexcept {
  switch(failure){
    case RuntimeSourceFailure::Checkpoint:return "Checkpoint";
    case RuntimeSourceFailure::StageChanged:return "StageChanged";
    case RuntimeSourceFailure::Rewind:return "Rewind";
    case RuntimeSourceFailure::Load:return "Load";
    case RuntimeSourceFailure::Selection:return "Selection";
    case RuntimeSourceFailure::Credentials:return "Credentials";
    case RuntimeSourceFailure::LocalFailure:return "LocalFailure";
  }
  return "Unknown";
}
struct RuntimeSigningInputs {
  std::optional<std::array<n::FrozenSigningSource,2>> sources;
  std::optional<RuntimeSourceFailure> failure;
  unsigned load_attempts{};
};
RuntimeSigningInputs load_runtime_signing_inputs(n::detail::ProtectedNamedStage& stage,
    const NativeLoginSpec& spec,RuntimeNativeObserver& observer,
    const a::ResponseSourceGeneration& generation) noexcept {
  RuntimeSigningInputs out;
  try {
    const auto check=[&](){return observer.checkpoint()&&
        std::holds_alternative<std::monostate>(generation.checkpoint());};
    if(!check()){out.failure=RuntimeSourceFailure::Checkpoint;return out;}
    if(!stage.unchanged()){out.failure=RuntimeSourceFailure::StageChanged;return out;}
    if(!stage.rewind_for_provider()){out.failure=RuntimeSourceFailure::Rewind;return out;}
    if(!check()){out.failure=RuntimeSourceFailure::Checkpoint;return out;}
    Aws::Config::AWSConfigFileProfileConfigLoader loader{stage.credentials_path().c_str(),false};
    ++out.load_attempts;
    const bool loaded=loader.Load();
    // Check even a failed parse before publishing a source category.
    if(!check()){out.failure=RuntimeSourceFailure::Checkpoint;return out;}
    if(!stage.unchanged()){out.failure=RuntimeSourceFailure::StageChanged;return out;}
    if(!loaded){out.failure=RuntimeSourceFailure::Load;return out;}
    const auto& profiles=loader.GetProfiles();const auto found=profiles.find(spec.profile.c_str());
    if(profiles.size()!=1||found==profiles.end()){out.failure=RuntimeSourceFailure::Selection;return out;}
    const auto& value=found->second.GetCredentials();
    if(value.GetAWSAccessKeyId().empty()||value.GetAWSSecretKey().empty()||
        value.GetAWSAccessKeyId().size()>n::FrozenSigningSource::max_field_bytes||
        value.GetAWSSecretKey().size()>n::FrozenSigningSource::max_field_bytes||
        value.GetSessionToken().size()>n::FrozenSigningSource::max_field_bytes){
      out.failure=RuntimeSourceFailure::Credentials;return out;
    }
    const auto source=[&]()->std::variant<n::FrozenSigningSource,n::Failure>{
      const auto secret=[](const Aws::String& bytes){return a::SecretBytes::create(
          std::as_bytes(std::span{bytes.data(),bytes.size()}));};
      auto access=secret(value.GetAWSAccessKeyId());auto key=secret(value.GetAWSSecretKey());
      auto token=secret(value.GetSessionToken());
      if(!access||!key||!token)return n::Failure::InvalidSource;
      return n::FrozenSigningSource::create(spec.source,spec.generation,
          std::move(access).value(),std::move(key).value(),std::move(token).value());
    };
    auto first=source();auto second=source();
    if(!std::holds_alternative<n::FrozenSigningSource>(first)||
        !std::holds_alternative<n::FrozenSigningSource>(second)){
      out.failure=RuntimeSourceFailure::Credentials;return out;
    }
    if(!check()){out.failure=RuntimeSourceFailure::Checkpoint;return out;}
    if(!stage.unchanged()){out.failure=RuntimeSourceFailure::StageChanged;return out;}
    out.sources.emplace(std::array<n::FrozenSigningSource,2>{
        std::get<n::FrozenSigningSource>(std::move(first)),std::get<n::FrozenSigningSource>(std::move(second))});
    return out;
    // loader/map/SDK credential strings die while runtime is active, before sends.
  }catch(...){out.sources.reset();out.failure=RuntimeSourceFailure::LocalFailure;return out;}
}
}

TEST(ProvisionedNativeAuth, TwoSequentialCredentialsAndTlsLoginsOneRuntime){
  // FIRST action: no clock, selections, files or SDK before this fixed marker.
  const char* marker=std::getenv("ODBCPP_REDSHIFT_NATIVE_RUNTIME_AUTH_ADMISSION");
  if(!marker||std::string_view(marker)!="provisioned-runtime-two-login-v1"){
    GTEST_SKIP()<<"Separate sequential runtime acquisition/login admission required";
  }
  ASSERT_TRUE(std::getenv("ODBCPP_AUTH_SDK_OFFLINE_FIXTURE")==nullptr);
  auto spec=read_spec();ASSERT_TRUE(spec.has_value());
  const char* config=std::getenv("AWS_CONFIG_FILE");
  const char* credentials=std::getenv("AWS_SHARED_CREDENTIALS_FILE");
  // These frozen paths must both select the retained snapshot's empty config,
  // never the named credentials file. This test performs no environment mutation.
  ASSERT_TRUE(config&&credentials&&spec->config==config&&spec->config==credentials);
  auto context_result=n::Context::create(spec->account,spec->region,spec->cluster,
      spec->database,spec->requested_user,spec->host,spec->source,spec->generation);
  ASSERT_TRUE(std::holds_alternative<n::Context>(context_result));
  const auto context=std::get<n::Context>(std::move(context_result));
  auto binding=a::Binding::create({a::Service::Redshift,spec->host,5439,spec->database,
      context.expected_user,context.resource_arn,spec->host,"verify-full"},
      {a::SourceKind::TrustedTemporaryDbIssuer,spec->source,spec->generation},a::Method::TemporaryDatabasePassword);
  ASSERT_TRUE(static_cast<bool>(binding));
  const auto start=rs::util::Clock::now();
  ASSERT_TRUE(start!=rs::util::Deadline::min()&&start!=rs::util::Deadline::max());
  const auto duration=std::chrono::duration_cast<rs::util::Clock::duration>(std::chrono::seconds{60});
  ASSERT_TRUE(start.time_since_epoch().count()<=std::numeric_limits<rs::util::Clock::rep>::max()-duration.count());
  auto requested=a::Request::create(std::move(binding).value(),start+duration,std::chrono::seconds{1});
  ASSERT_TRUE(static_cast<bool>(requested));const auto request=std::move(requested).value();
  auto cancellation=std::make_shared<n::WorkerCancellation>();
  auto observer=std::make_shared<RuntimeNativeObserver>(request.deadline(),cancellation);
  auto generation_result=a::ResponseSourceGeneration::create(request.binding());
  ASSERT_TRUE(std::holds_alternative<std::shared_ptr<a::ResponseSourceGeneration>>(generation_result));
  auto generation=std::get<std::shared_ptr<a::ResponseSourceGeneration>>(std::move(generation_result));
  ASSERT_TRUE(observer->checkpoint());
  std::unique_ptr<n::detail::ProtectedNamedStage> stage;
  try{stage=n::detail::ProtectedNamedStage::create(
      {spec->profile,spec->credentials,spec->config,spec->source,spec->generation},context,request);}
  catch(...){FAIL()<<"Runtime named source policy refused";}
  ASSERT_TRUE(stage!=nullptr);ASSERT_TRUE(observer->checkpoint());
  ASSERT_TRUE(stage->unchanged());ASSERT_TRUE(stage->rewind_for_cache());ASSERT_TRUE(observer->checkpoint());
  // Declaration order retains source descriptors through global SDK destruction.
  auto started=n::ProvisionedSdkRuntime::create();
  ASSERT_TRUE(started.runtime!=nullptr);ASSERT_FALSE(started.failure.has_value());
  auto& runtime=*started.runtime;
  ASSERT_TRUE(observer->checkpoint());EXPECT_EQ(1U,runtime.counts().initializations);
  EXPECT_EQ(0U,runtime.counts().shutdowns);EXPECT_FALSE(runtime.counts().occupied);
  auto signing=load_runtime_signing_inputs(*stage,*spec,*observer,*generation);
  SCOPED_TRACE(std::string("runtime_source_failure=")+
      (signing.failure?runtime_source_failure_name(*signing.failure):"None"));
  ASSERT_FALSE(signing.failure.has_value());ASSERT_TRUE(signing.sources.has_value());
  EXPECT_EQ(1U,signing.load_attempts);
  std::array<LoginOutcome,2> logins;unsigned completed=0;
  const auto one=[&](unsigned index){
    SCOPED_TRACE(index);
    ASSERT_TRUE(observer->checkpoint());ASSERT_TRUE(stage->unchanged());
    auto made=runtime.create_operation(context,request,observer,generation,cancellation,
        std::move((*signing.sources)[index]));
    SCOPED_TRACE(safe_counts_trace(DiagnosticPhase::Creation,made.failure,made.counts));
    ASSERT_TRUE(made.owner!=nullptr);ASSERT_FALSE(made.failure.has_value());
    auto acquired=made.owner->acquire_observation();
    SCOPED_TRACE(safe_counts_trace(DiagnosticPhase::Acquisition,acquired.failure,acquired.counts));
    ASSERT_TRUE(acquired.observation.has_value());ASSERT_FALSE(acquired.failure.has_value());
    const auto& counts=acquired.counts;
    EXPECT_TRUE(counts.request_exact);EXPECT_TRUE(counts.request_policy);EXPECT_TRUE(counts.model_success);
    EXPECT_TRUE(counts.model_user_matches);EXPECT_TRUE(counts.model_password_matches);
    EXPECT_EQ(1U,counts.requests);EXPECT_EQ(1U,counts.selected_sends);
    EXPECT_EQ(1U,counts.delegate_returns);EXPECT_EQ(1U,counts.delegate_destructions);
    EXPECT_EQ(0U,counts.active);EXPECT_EQ(counts.wrappers,counts.destroyed_wrappers);
    EXPECT_EQ(0U,counts.initializations);EXPECT_EQ(0U,counts.shutdowns);EXPECT_EQ(0U,counts.named_provider_loads);
    EXPECT_GT(counts.frozen_provider_reads,0U);EXPECT_TRUE(counts.cleanup_before_c);
    EXPECT_EQ(1U,counts.c_samples);EXPECT_EQ(0U,counts.n_samples);EXPECT_TRUE(counts.unknown_only);
    if(HasFailure())return;
    login_observation(*acquired.observation,*spec,context.expected_user,request,logins[index]);
    const auto& login=logins[index];
    EXPECT_TRUE(login.borrow_passed);EXPECT_TRUE(login.raw_expiry_positive);EXPECT_TRUE(login.entered);
    EXPECT_FALSE(login.unexpected_exception);EXPECT_TRUE(login.wire_identity);EXPECT_TRUE(login.password_present);
    EXPECT_TRUE(login.connected);EXPECT_TRUE(login.peer_verified);EXPECT_TRUE(login.disconnected);
    EXPECT_TRUE(login.observation_closed);EXPECT_EQ(1U,login.observation_counts.n_samples);
    EXPECT_TRUE(made.owner->close());EXPECT_FALSE(runtime.counts().occupied);
    ASSERT_TRUE(login.connection.has_value());ASSERT_TRUE(login.connection->has_value());
    EXPECT_TRUE((login.connection->session_snapshot()==d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));
    ASSERT_TRUE(login.query.has_value());ASSERT_TRUE(login.query->has_value());
    EXPECT_TRUE((login.query->session_snapshot()==d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));
    const auto& result=login.query->value();EXPECT_FALSE(result.error.has_value());
    EXPECT_TRUE(result.additional_results.empty());EXPECT_TRUE(result.cell_errors.empty());
    ASSERT_EQ(4U,result.columns.size());ASSERT_EQ(1U,result.rows.size());ASSERT_EQ(4U,result.rows[0].size());
    EXPECT_TRUE(result.rows[0][0].has_value()&&*result.rows[0][0]==spec->database);
    EXPECT_TRUE(result.rows[0][1].has_value()&&*result.rows[0][1]==spec->sql_user);
    EXPECT_TRUE(result.rows[0][2].has_value()&&*result.rows[0][2]=="1");EXPECT_FALSE(result.rows[0][3].has_value());
    for(auto column:{2U,3U}){
      ASSERT_TRUE(result.columns[column].normalized_type.has_value());
      EXPECT_TRUE(result.columns[column].normalized_type->known);
      EXPECT_EQ(d::ScalarType::Integer,result.columns[column].normalized_type->type);
    }
    ASSERT_TRUE(observer->checkpoint());ASSERT_TRUE(stage->unchanged());
    if(!HasFailure())++completed;
  };
  for(unsigned index=0;index<2;++index){one(index);if(HasFailure())break;}
  // All per-operation locals are gone before this global shutdown. No replay.
  signing.sources.reset();EXPECT_FALSE(runtime.counts().occupied);
  EXPECT_EQ(1U,runtime.counts().initializations);EXPECT_EQ(0U,runtime.counts().shutdowns);
  EXPECT_TRUE(runtime.close());EXPECT_EQ(1U,runtime.counts().shutdowns);EXPECT_TRUE(runtime.counts().closed);
  ASSERT_TRUE(observer->checkpoint());ASSERT_TRUE(stage->unchanged());ASSERT_EQ(2U,completed);
  // Both QueryResults remain owning after disconnect, later acquisition and SDK shutdown.
  for(const auto& login:logins){
    ASSERT_TRUE(login.query.has_value());ASSERT_TRUE(login.query->has_value());
    const auto& rows=login.query->value().rows;ASSERT_EQ(1U,rows.size());ASSERT_EQ(4U,rows[0].size());
    EXPECT_TRUE(rows[0][0].has_value()&&*rows[0][0]==spec->database);
    EXPECT_TRUE(rows[0][1].has_value()&&*rows[0][1]==spec->sql_user);
    EXPECT_TRUE(rows[0][2].has_value()&&*rows[0][2]=="1");EXPECT_FALSE(rows[0][3].has_value());
  }
  // Root controller must independently verify exact-principal zero activity,
  // child quiescence/input teardown and pause; this case grants no admission.
}
