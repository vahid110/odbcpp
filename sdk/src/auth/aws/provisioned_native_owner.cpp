#include "odbcpp/auth/aws/provisioned_native_http_ingress.h"
#include "odbcpp/auth/aws/named_profile_acquisition.h"
#include <aws/core/Globals.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/crt/io/HostResolver.h>
#include <aws/redshift/RedshiftClient.h>
#include <aws/redshift/model/GetClusterCredentialsRequest.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
namespace rs::core::auth::aws::provisioned_native {
namespace {
constexpr const char* tag="ProvisionedNativeOwner";
std::atomic<bool> occupied{false};
bool name(std::string_view s) noexcept {
  if(s.empty() || s.size()>128) return false;
  for(unsigned char c:s) if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return false;
  return true;
}
bool account_valid(std::string_view s) noexcept {if(s.size()!=12)return false;for(char c:s)if(c<'0'||c>'9')return false;return true;}
bool main_entry() noexcept {
#if defined(__APPLE__)
 return ::pthread_main_np()!=0;
#else
 return false;
#endif
}
class CombinedObserver final:public ResponseObservationSource {
 public:CombinedObserver(std::shared_ptr<ResponseObservationSource> s,std::shared_ptr<WorkerCancellation> c):source_(std::move(s)),cancel_(std::move(c)){}
  ResponseClockRead read_monotonic()override{return source_->read_monotonic();}
  bool cancellation_requested()override{return cancel_->cancelled() || source_->cancellation_requested();}
 private:std::shared_ptr<ResponseObservationSource> source_;std::shared_ptr<WorkerCancellation> cancel_;
};
bool bound(const Context& c,const Request& r) noexcept {
 if(r.binding().invariant_error() || r.deadline()==rs::util::Deadline::min() || r.deadline()==rs::util::Deadline::max()) return false;
 const auto& t=r.binding().target();const auto& s=r.binding().source();
 return t.service==Service::Redshift && t.endpoint==c.db_endpoint && t.tls_identity==c.db_endpoint && t.trust_policy=="verify-full" &&
  t.port==5439 && t.database==c.database && t.principal==c.expected_user && t.resource_id==c.resource_arn &&
  s.identity==c.source_identity && s.generation==c.source_generation && s.kind==SourceKind::TrustedTemporaryDbIssuer &&
  r.binding().method()==Method::TemporaryDatabasePassword;
}
void empty_file(const std::string& path){int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)throw std::runtime_error("Staging refused");if(::close(fd))throw std::runtime_error("Staging refused");}
}
Context::Context(std::string a,std::string r,std::string c,std::string d,std::string u,std::string e,std::string s,std::string g,bool fake)
 :account(std::move(a)),region(std::move(r)),cluster(std::move(c)),database(std::move(d)),requested_user(std::move(u)),
  expected_user("IAM:"+requested_user),db_endpoint(std::move(e)),source_identity(std::move(s)),source_generation(std::move(g)),
  resource_arn("arn:aws:redshift:"+region+":"+account+":cluster:"+cluster),
  api_endpoint(fake?"https://redshift.fixture.invalid":"https://redshift."+region+".amazonaws.com"){}
std::variant<Context,Failure> Context::create(std::string a,std::string r,std::string c,std::string d,std::string u,std::string e,std::string s,std::string g){
 // Narrow AWS-commercial regional syntax only, not availability/reserved-word validation.
 if(!account_valid(a)||!name(r)||r=="aws-global"||r=="AWS_GLOBAL"||r.find('-')==r.npos||r.back()<'0'||r.back()>'9'||
   !name(c)||!name(d)||!name(u)||!name(e)||!name(s)||!name(g)) return Failure::InvalidContext;
 return Context{std::move(a),std::move(r),std::move(c),std::move(d),std::move(u),std::move(e),std::move(s),std::move(g),false};
}
FrozenNamedSource::FrozenNamedSource(Aws::Auth::AWSCredentials value,std::string identity,std::string generation)
 :value_(std::move(value)),identity_(std::move(identity)),generation_(std::move(generation)){}
FrozenNamedSource::FrozenNamedSource(FrozenNamedSource&& other) noexcept
 :value_(std::move(other.value_)),identity_(std::move(other.identity_)),generation_(std::move(other.generation_)),owns_(std::exchange(other.owns_,false)){}
namespace detail {
State::State(Context c,Request r,std::shared_ptr<ResponseObservationSource> read,std::shared_ptr<ResponseSourceGeneration> gen,
 std::shared_ptr<WorkerCancellation> cancel,FrozenNamedSource&& frozen_source,std::optional<FixedCase> test)
 :context(std::move(c)),request(std::move(r)),observer(std::move(read)),generation(std::move(gen)),cancellation(std::move(cancel)),
  source(std::move(frozen_source)),fixed(test),creator(std::this_thread::get_id()){}
TransportBoundaryLease State::lease(){auto result=operation->transport_lease(request);if(!std::holds_alternative<TransportBoundaryLease>(result))std::terminate();return std::get<TransportBoundaryLease>(std::move(result));}
bool State::check() noexcept {
 if(creator!=std::this_thread::get_id())std::terminate();
 if(!dispatch)return false;
 if(initialized && (!bootstrap || !*bootstrap || Aws::GetDefaultClientBootstrap()!=bootstrap.get())){reject();return false;}
 const auto result=dispatch->check();if(auto* f=std::get_if<BoundaryFailure>(&result))metrics.boundary=*f;
 return std::holds_alternative<std::monostate>(result);
}
void State::reject() noexcept {
 if(!dispatch)std::terminate();
 const auto value=dispatch->reject_body();const auto confirmed=dispatch->check();
 const auto* a=std::get_if<BoundaryFailure>(&value);const auto* b=std::get_if<BoundaryFailure>(&confirmed);
 if(!a||!b||*a!=*b||operation->fault()!=*a)std::terminate();metrics.boundary=*a;
}
void State::shutdown_sdk() noexcept {
 if(creator!=std::this_thread::get_id() || service_live)std::terminate();
 if(initialized){
  check_capture(*this);detach_reader(*this);
  source.value_.reset();source.owns_=false;
  bootstrap.reset();Aws::ShutdownAPI(options);initialized=false;++metrics.shutdowns;
 }
}
void State::cleanup() noexcept {
 if(creator!=std::this_thread::get_id() || active || observing || service_live)std::terminate();
 shutdown_sdk();
 if(protected_stage){protected_stage.reset();metrics.named_stage_released=true;}
 if(!staged_config.empty())::unlink(staged_config.c_str());if(!staged_credentials.empty())::unlink(staged_credentials.c_str());
 if(!staged_directory.empty())::rmdir(staged_directory.c_str());
 if(token){occupied.store(false);token=false;}
}
State::~State(){cleanup();}
class FrozenProvider final:public Aws::Auth::AWSCredentialsProvider {
 public:explicit FrozenProvider(State& s):state_(s){}
 Aws::Auth::AWSCredentials GetAWSCredentials()override{++state_.metrics.frozen_provider_reads;return state_.source_value();}
 private:State& state_;
};
Aws::Auth::AWSCredentials State::source_value()const{if(!source_ready||!source.value_)std::terminate();return *source.value_;}
}
namespace {
std::shared_ptr<detail::State> prepare(Context c,Request r,std::shared_ptr<ResponseObservationSource> read,
 std::shared_ptr<ResponseSourceGeneration> gen,std::shared_ptr<WorkerCancellation> cancel,FrozenNamedSource&& frozen,
 std::optional<FixedCase> test,Failure& failure,std::optional<ProtectedNamedSourceSpec> named={},Counts* failure_counts=nullptr){
 if(!main_entry()){failure=Failure::MainEntryRequired;return {};}
 // Standalone entry precondition; this is not synchronization with unrelated SDK users.
 if(Aws::GetDefaultClientBootstrap()!=nullptr){failure=Failure::Unavailable;return {};}
 if(!bound(c,r)||!read||!gen||!cancel){failure=Failure::InvalidContext;return {};}
 auto state=std::make_shared<detail::State>(std::move(c),std::move(r),std::move(read),std::move(gen),std::move(cancel),std::move(frozen),test);
 auto combined=std::make_shared<CombinedObserver>(state->observer,state->cancellation);
 auto result=ResponseOperation::begin(state->request,combined,state->generation);
 if(!std::holds_alternative<std::unique_ptr<ResponseOperation>>(result)){failure=Failure::Refused;return {};}
 state->operation=std::get<std::unique_ptr<ResponseOperation>>(std::move(result));
 if(!std::holds_alternative<std::monostate>(state->operation->begin_transport())){failure=Failure::Refused;return {};}
 state->dispatch.emplace(state->lease());
 if(!state->check()){failure=Failure::Refused;return {};}
 if(occupied.exchange(true)){failure=Failure::Unavailable;return {};}
 state->token=true;
 try{
  if(named){
   state->source_ready=false;
   state->protected_stage=detail::ProtectedNamedStage::create(*named,state->context,state->request);
   if(!state->check()){failure=Failure::Refused;state->cleanup();if(failure_counts)*failure_counts=state->metrics;return {};}
  }
  if(!named){
  char pattern[]="/tmp/odbcpp_native_provisioned_XXXXXX";char* created=::mkdtemp(pattern);if(!created)throw std::runtime_error("Staging refused");
  state->staged_directory=created;state->staged_config=state->staged_directory+"/config";state->staged_credentials=state->staged_directory+"/credentials";
  empty_file(state->staged_config);empty_file(state->staged_credentials);
  }
  const auto& config_path=state->protected_stage?state->protected_stage->config_path():state->staged_config;
  const auto& credential_path=state->protected_stage?state->protected_stage->credentials_path():state->staged_credentials;
  if(::setenv("AWS_CONFIG_FILE",config_path.c_str(),1)||::setenv("AWS_SHARED_CREDENTIALS_FILE",credential_path.c_str(),1)||::setenv("AWS_EC2_METADATA_DISABLED","true",1))throw std::runtime_error("Environment refused");
  for(const char* key:{"HTTP_PROXY","HTTPS_PROXY","ALL_PROXY","http_proxy","https_proxy","all_proxy","AWS_PROFILE","AWS_DEFAULT_PROFILE"})if(::unsetenv(key))throw std::runtime_error("Environment refused");
  state->options.loggingOptions.logLevel=Aws::Utils::Logging::LogLevel::Off;auto* ptr=state.get();
  state->options.ioOptions.clientBootstrap_create_fn=[ptr]() noexcept {
   try{if(ptr->creator!=std::this_thread::get_id()||!main_entry())std::terminate();
    Aws::Crt::Io::EventLoopGroup group;Aws::Crt::Io::DefaultHostResolver resolver(group,8,30);
    ptr->bootstrap=Aws::MakeShared<Aws::Crt::Io::ClientBootstrap>(tag,group,resolver);++ptr->metrics.bootstrap_factories;
    if(ptr->bootstrap && *ptr->bootstrap)ptr->bootstrap->EnableBlockingShutdown();return ptr->bootstrap;
   }catch(...){std::terminate();}
  };
  state->options.httpOptions.httpClientFactory_create_fn=[ptr]()noexcept{try{return detail::make_factory(*ptr);}catch(...){std::terminate();}};
  if(!state->check()){failure=Failure::Refused;state->cleanup();if(failure_counts)*failure_counts=state->metrics;return {};}
  if(named&&(!state->protected_stage->rewind_for_cache()||!state->check())){state->reject();failure=Failure::InvalidSource;state->cleanup();if(failure_counts)*failure_counts=state->metrics;return {};}
  Aws::InitAPI(state->options);state->initialized=true;++state->metrics.initializations;
  state->metrics.bootstrap_matches=state->bootstrap && *state->bootstrap && state->bootstrap.get()==Aws::GetDefaultClientBootstrap();
  if(!state->check()){failure=Failure::Refused;state->cleanup();if(failure_counts)*failure_counts=state->metrics;return {};}
  if(named&&!detail::NamedSourceAcquisitionOwner::load(*state)){failure=Failure::InvalidSource;state->cleanup();if(failure_counts)*failure_counts=state->metrics;return {};}
  return state;
 }catch(...){state->reject();failure=Failure::LocalFailure;state->cleanup();if(failure_counts)*failure_counts=state->metrics;return {};}
}
}
NativeOwner::NativeOwner(std::shared_ptr<detail::State> s):state_(std::move(s)){}
NativeOwner::~NativeOwner(){if(state_){if(state_->observing)std::terminate();state_->cleanup();}}
CreateOutcome NativeOwner::create(Context c,Request r,std::shared_ptr<ResponseObservationSource> read,
 std::shared_ptr<ResponseSourceGeneration> gen,std::shared_ptr<WorkerCancellation> cancel,FrozenNamedSource&& supplied){
 // Consume owning source before all refusals. It is not an authenticated receipt.
 auto frozen=std::move(supplied);CreateOutcome out;
 if(!frozen.owns_||frozen.identity_!=c.source_identity||frozen.generation_!=c.source_generation||!frozen.value_||frozen.value_->GetAWSAccessKeyId().empty()||frozen.value_->GetAWSSecretKey().empty()) {out.failure=Failure::InvalidSource;return out;}
 try{Failure failure=Failure::LocalFailure;auto state=prepare(std::move(c),std::move(r),std::move(read),std::move(gen),std::move(cancel),std::move(frozen),{},failure);
  if(!state){out.failure=failure;return out;}out.counts=state->metrics;out.owner.reset(new NativeOwner{std::move(state)});return out;
 }catch(...){out.failure=Failure::LocalFailure;return out;}
}
CreateOutcome NativeOwner::create_from_named_source(Context c,Request r,std::shared_ptr<ResponseObservationSource> read,
 std::shared_ptr<ResponseSourceGeneration> gen,std::shared_ptr<WorkerCancellation> cancel,ProtectedNamedSourceSpec spec){
 CreateOutcome out;
 try{auto pending=detail::NamedSourceAcquisitionOwner::pending(c);Failure failure=Failure::LocalFailure;
  auto state=prepare(std::move(c),std::move(r),std::move(read),std::move(gen),std::move(cancel),std::move(pending),{},failure,std::move(spec),&out.counts);
  if(!state){out.failure=failure;return out;}out.counts=state->metrics;out.owner.reset(new NativeOwner{std::move(state)});return out;
 }catch(...){out.failure=Failure::LocalFailure;return out;}
}
CreateOutcome FixedFixture::create_named(ProtectedNamedSourceSpec spec,std::shared_ptr<ResponseObservationSource> read,
 std::shared_ptr<ResponseSourceGeneration> gen,std::shared_ptr<WorkerCancellation> cancel){
 CreateOutcome out;const char* marker=::getenv("ODBCPP_AUTH_SDK_OFFLINE_FIXTURE");
 if(!marker||std::string_view(marker)!="synthetic-fake-http-only"){out.failure=Failure::Unavailable;return out;}
 try{auto c=context();auto r=request(c);auto pending=detail::NamedSourceAcquisitionOwner::pending(c);Failure failure=Failure::LocalFailure;
  auto state=prepare(std::move(c),std::move(r),std::move(read),std::move(gen),std::move(cancel),std::move(pending),FixedCase::Happy,failure,std::move(spec),&out.counts);
  if(!state){out.failure=failure;return out;}out.counts=state->metrics;out.owner.reset(new NativeOwner{std::move(state)});return out;
 }catch(...){out.failure=Failure::LocalFailure;return out;}
}
Context FixedFixture::context(){return Context{"000000000000","eu-north-1","fixture-cluster","fixture_db","fixture_user","db.fixture.invalid","synthetic-source","g1",true};}
Request FixedFixture::request(const Context& c){auto binding=Binding::create({Service::Redshift,c.db_endpoint,5439,c.database,c.expected_user,c.resource_arn,c.db_endpoint,"verify-full"},{SourceKind::TrustedTemporaryDbIssuer,c.source_identity,c.source_generation},Method::TemporaryDatabasePassword);if(!binding)std::terminate();auto made=Request::create(std::move(binding).value(),rs::util::Deadline{std::chrono::seconds{20}},std::chrono::seconds{1});if(!made)std::terminate();return std::move(made).value();}
CreateOutcome FixedFixture::create(FixedCase test,std::shared_ptr<ResponseObservationSource> read,std::shared_ptr<ResponseSourceGeneration> gen,std::shared_ptr<WorkerCancellation> cancel){
 CreateOutcome out;const char* marker=::getenv("ODBCPP_AUTH_SDK_OFFLINE_FIXTURE");
 if(!marker||std::string_view(marker)!="synthetic-fake-http-only"||static_cast<unsigned>(test)>static_cast<unsigned>(FixedCase::Reentry)){out.failure=Failure::Unavailable;return out;}
 try{auto c=context();auto r=request(c);FrozenNamedSource frozen{Aws::Auth::AWSCredentials{"synthetic-access-key","synthetic-secret-key"},c.source_identity,c.source_generation};
  Failure failure=Failure::LocalFailure;auto state=prepare(std::move(c),std::move(r),std::move(read),std::move(gen),std::move(cancel),std::move(frozen),test,failure);
  if(!state){out.failure=failure;return out;}out.counts=state->metrics;out.owner.reset(new NativeOwner{std::move(state)});return out;
 }catch(...){out.failure=Failure::LocalFailure;return out;}
}
Outcome NativeOwner::acquire_observation(){
 Outcome out;if(!state_){out.failure=Failure::Consumed;return out;}auto& s=*state_;
 if(s.creator!=std::this_thread::get_id()){out.failure=Failure::WrongThread;return out;}
 if(s.active){s.reject();out.failure=Failure::Reentrant;return out;}
 if(s.consumed){out.failure=Failure::Consumed;out.counts=s.metrics;return out;}
 s.active=true;struct Active{bool& flag;~Active(){flag=false;}}active{s.active};s.consumed=true;
 if(!s.source_ready){s.reject();out.failure=Failure::InvalidSource;out.counts=s.metrics;return out;}
 if(!s.check()){out.failure=Failure::Refused;out.counts=s.metrics;return out;}
 try{
  s.service_live=true;struct Service{bool& flag;~Service(){flag=false;}}service{s.service_live};
  {
   Aws::Client::ClientConfigurationInitValues init;init.shouldDisableIMDS=true;Aws::Redshift::RedshiftClientConfiguration config(init);config.disableIMDS=true;
   config.region=s.context.region.c_str();config.endpointOverride=s.context.api_endpoint.c_str();config.scheme=Aws::Http::Scheme::HTTPS;config.verifySSL=true;
   config.proxyHost.clear();config.proxyUserName.clear();config.proxyPassword.clear();config.retryStrategy=Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(tag,0,0);
   config.requestTimeoutMs=1000;config.connectTimeoutMs=1000;
   auto provider=Aws::MakeShared<detail::FrozenProvider>(tag,s);Aws::Redshift::RedshiftClient client(provider,nullptr,config);s.selected_client=client.GetHttpClient().get();
   if(s.fixed==FixedCase::NullThenSelected){auto refused=client.GetHttpClient()->MakeRequest({},nullptr,nullptr);if(!refused||!refused->HasClientError())std::terminate();}
   if(s.fixed==FixedCase::ForeignThenSelected){auto foreign=Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag,Aws::Http::URI("https://foreign.fixture.invalid/"),Aws::Http::HttpMethod::HTTP_POST);auto refused=client.GetHttpClient()->MakeRequest(foreign,nullptr,nullptr);if(!refused||!refused->HasClientError())std::terminate();}
   Aws::Redshift::Model::GetClusterCredentialsRequest request;request.SetDbName(s.context.database.c_str());request.SetDbUser(s.context.requested_user.c_str());request.SetClusterIdentifier(s.context.cluster.c_str());request.SetDurationSeconds(900);request.SetAutoCreate(false);
   s.expected_query=request.SerializePayload().c_str();
   s.metrics.request_policy=s.expected_query.find("DurationSeconds=900&AutoCreate=false&Version=2012-12-01")!=std::string::npos && s.expected_query.find("DbGroups")==std::string::npos && s.expected_query.find("CustomDomain")==std::string::npos;
   if(!s.metrics.request_policy)s.reject();
   if(!s.check()){s.reject();}else{
    const auto result=client.GetClusterCredentials(request);s.metrics.model_success=result.IsSuccess();
    s.metrics.safe_error=!result.IsSuccess() && result.GetError().GetErrorType()==Aws::Redshift::RedshiftErrors::VALIDATION;
    if(result.IsSuccess()){
     s.metrics.model_expiry_ms=result.GetResult().GetExpiration().Millis();
     if(s.fields){
      s.metrics.model_user_matches=std::string_view(result.GetResult().GetDbUser())==s.fields->user;
      s.fields->password.with_bytes([&](auto bytes){const auto& model=result.GetResult().GetDbPassword();s.metrics.model_password_matches=model.size()==bytes.size() && std::memcmp(model.data(),bytes.data(),bytes.size())==0;});
      if(!s.metrics.model_user_matches || !s.metrics.model_password_matches)s.reject();
     }else{s.reject();}
    }
   }
   if(s.fixed==FixedCase::LateAfterModel && s.wire){detail::late_writer(s);}
   detail::check_capture(s);(void)s.check();s.sync();
  } // model/client/request/provider wrappers gone, Transport still held
  s.service_live=false;
  detail::detach_reader(s);s.shutdown_sdk();s.metrics.cleanup_before_c=true;
  s.dispatch.reset();const auto c=s.operation->complete_transport();if(std::holds_alternative<std::monostate>(c))++s.metrics.c_samples;
  s.metrics.boundary=s.operation->fault();
  if(s.operation->fault()||!s.metrics.model_success||!s.fields){s.fields.reset();out.failure=Failure::Refused;out.counts=s.metrics;return out;}
  auto lease=s.operation->processing_lease(s.request);if(!std::holds_alternative<ProcessingBoundaryLease>(lease))std::terminate();
  auto fields=std::move(*s.fields);s.fields.reset();s.observing=true;
  out.observation.emplace(Observation{state_,std::get<ProcessingBoundaryLease>(std::move(lease)),std::move(fields)});out.counts=s.metrics;return out;
 }catch(...){if(s.dispatch)s.reject();s.fields.reset();detail::detach_reader(s);out.failure=Failure::LocalFailure;out.counts=s.metrics;return out;}
}
bool NativeOwner::close()noexcept{if(!state_||state_->creator!=std::this_thread::get_id()||state_->active||state_->observing||state_->service_live)return false;state_->cleanup();closed_=state_->metrics;state_.reset();return true;}
Counts NativeOwner::counts()const noexcept{return state_?state_->metrics:closed_;}
Observation::Observation(std::shared_ptr<detail::State> s,ProcessingBoundaryLease&& keep,ExtractedDbFields&& fields):state_(std::move(s)),keep_(std::move(keep)),fields_(std::move(fields)){}
Observation::Observation(Observation&& other){if(other.viewing_||(other.state_&&other.state_->creator!=std::this_thread::get_id()))std::terminate();state_=std::move(other.state_);if(other.keep_)keep_.emplace(std::move(*other.keep_));fields_=std::move(other.fields_);other.keep_.reset();other.fields_.reset();}
Observation::~Observation(){(void)close();}
bool Observation::with_fields(const std::function<void(const ExtractedDbFields&)>& fn)const{if(!state_||!fields_||!fn)return false;if(viewing_||state_->creator!=std::this_thread::get_id())std::terminate();if(!keep_||!std::holds_alternative<std::monostate>(keep_->check()))return false;viewing_=true;struct View{bool& flag;~View(){flag=false;}}view{viewing_};fn(*fields_);return std::holds_alternative<std::monostate>(keep_->check());}
bool Observation::close()noexcept{if(!state_||!fields_)return false;if(viewing_||state_->creator!=std::this_thread::get_id())std::terminate();fields_.reset();keep_.reset();auto n=state_->operation->finish_processing();if(std::holds_alternative<ResponseCompletion>(n))++state_->metrics.n_samples;else if(!state_->operation->fault())std::terminate();state_->observing=false;return true;}
Counts Observation::counts()const noexcept{return state_?state_->metrics:Counts{};}
} // namespace
