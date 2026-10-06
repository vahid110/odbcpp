#include "odbcpp/auth/aws/provisioned_sdk_runtime.h"
#include "odbcpp/auth/aws/provisioned_native_http_ingress.h"
#include <aws/core/Globals.h>
#include <aws/crt/io/HostResolver.h>
#include <array>
#include <cstdlib>
#include <pthread.h>
namespace rs::core::auth::aws::provisioned_native {
namespace {
constexpr const char* tag="ProvisionedSdkRuntime";
bool main_entry() noexcept {
#if defined(__APPLE__)
 return ::pthread_main_np()!=0;
#else
 return false;
#endif
}
bool atom(std::string_view s) noexcept {
 if(s.empty()||s.size()>128)return false;
 for(unsigned char c:s)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;
 return true;
}
bool bytes(const SecretBytes& b,bool required) {
 if((required&&b.empty())||b.size()>FrozenSigningSource::max_field_bytes)return false;
 bool valid=true;b.with_bytes([&](auto view){for(auto c:view)if(c==std::byte{0})valid=false;});return valid;
}
constexpr std::array forbidden{"HTTP_PROXY","HTTPS_PROXY","ALL_PROXY","http_proxy","https_proxy","all_proxy",
 "AWS_PROFILE","AWS_DEFAULT_PROFILE","AWS_WEB_IDENTITY_TOKEN_FILE","AWS_ROLE_ARN","AWS_ENDPOINT_URL","AWS_ENDPOINT_URL_REDSHIFT",
 "AWS_ACCESS_KEY_ID","AWS_SECRET_ACCESS_KEY","AWS_SESSION_TOKEN"};
bool forbidden_present() noexcept {for(const char* k:forbidden)if(const char* v=::getenv(k);v&&*v)return true;return false;}
std::optional<std::string> path_environment(const char* key){
 const char* v=::getenv(key);if(!v||*v!='/')return {};std::size_t n=0;while(n<=4096&&v[n])++n;if(n>4096)return {};
 for(std::size_t i=0;i<n;++i)if(static_cast<unsigned char>(v[i])<32||v[i]==127)return {};
 return std::string(v,n);
}
Aws::String sdk_string(const SecretBytes& b){Aws::String out;b.with_bytes([&](auto v){if(!v.empty())out.assign(reinterpret_cast<const char*>(v.data()),v.size());});return out;}
}
namespace detail {
struct RuntimeState final {
 enum class Slot { Idle, Occupied, Closed };
 const std::thread::id creator=std::this_thread::get_id();
 const std::string config,credentials;
 Aws::SDKOptions options;std::shared_ptr<Aws::Crt::Io::ClientBootstrap> bootstrap;
 std::atomic<Slot> slot{Slot::Idle};std::atomic<unsigned> idle_clients{},idle_requests{};
 std::atomic<unsigned> initializations{},shutdowns{};bool scope{},initialized{},initializing{};
 RuntimeState(std::string c,std::string k):config(std::move(c)),credentials(std::move(k)){}
 bool environment()const noexcept {
  const char* c=::getenv("AWS_CONFIG_FILE");const char* k=::getenv("AWS_SHARED_CREDENTIALS_FILE");const char* m=::getenv("AWS_EC2_METADATA_DISABLED");
  return c&&k&&m&&config==c&&credentials==k&&std::string_view(m)=="true"&&!forbidden_present();
 }
 ~RuntimeState(){if(initializing||initialized||scope)std::terminate();}
};
bool runtime_usable(const std::shared_ptr<RuntimeState>& s)noexcept {
 return s&&s->slot.load()==RuntimeState::Slot::Occupied&&s->initialized&&s->environment()&&s->bootstrap&&*s->bootstrap&&Aws::GetDefaultClientBootstrap()==s->bootstrap.get();
}
void release_runtime_slot(const std::shared_ptr<RuntimeState>& s) noexcept {
 auto expected=RuntimeState::Slot::Occupied;if(!s||!s->slot.compare_exchange_strong(expected,RuntimeState::Slot::Idle))std::terminate();
}
std::shared_ptr<Aws::Crt::Io::ClientBootstrap> runtime_bootstrap(const std::shared_ptr<RuntimeState>& s){return s->bootstrap;}
void note_runtime_idle(const std::weak_ptr<RuntimeState>& weak,bool request)noexcept {
 if(auto s=weak.lock()){if(request)++s->idle_requests;else ++s->idle_clients;}
}
}
FrozenSigningSource::FrozenSigningSource(std::string i,std::string g,SecretBytes&& a,SecretBytes&& s,SecretBytes&& t)
 :identity_(std::move(i)),generation_(std::move(g)),access_(std::move(a)),secret_(std::move(s)),token_(std::move(t)){}
std::variant<FrozenSigningSource,Failure> FrozenSigningSource::create(std::string i,std::string g,SecretBytes&& a,SecretBytes&& s,SecretBytes&& t){
 auto access=std::move(a);auto secret=std::move(s);auto token=std::move(t);
 try{if(!atom(i)||!atom(g)||!bytes(access,true)||!bytes(secret,true)||!bytes(token,false))return Failure::InvalidSource;
  return FrozenSigningSource{std::move(i),std::move(g),std::move(access),std::move(secret),std::move(token)};
 }catch(...){return Failure::InvalidSource;}
}
ProvisionedSdkRuntime::ProvisionedSdkRuntime(std::shared_ptr<detail::RuntimeState> s):state_(std::move(s)){}
RuntimeCreateOutcome ProvisionedSdkRuntime::create(){return create_impl({});}
RuntimeCreateOutcome ProvisionedSdkRuntime::create_impl(std::optional<RuntimeInitFixture> injected){
 RuntimeCreateOutcome out;if(!main_entry()){out.failure=Failure::MainEntryRequired;return out;}
 if(Aws::GetDefaultClientBootstrap()!=nullptr){out.failure=Failure::Unavailable;return out;}
 std::shared_ptr<detail::RuntimeState> s;
 try {
  auto config=path_environment("AWS_CONFIG_FILE"),credentials=path_environment("AWS_SHARED_CREDENTIALS_FILE");
  if(!config||!credentials||forbidden_present()){out.failure=Failure::InvalidContext;return out;}
  s=std::make_shared<detail::RuntimeState>(std::move(*config),std::move(*credentials));
  if(!s->environment()){out.failure=Failure::InvalidContext;return out;}
  // Allocate the public owner before taking global scope or initializing SDK.
  out.runtime.reset(new ProvisionedSdkRuntime{s});
  if(!detail::take_sdk_scope()){out.runtime.reset();out.failure=Failure::Unavailable;return out;}
  s->scope=true;s->options.loggingOptions.logLevel=Aws::Utils::Logging::LogLevel::Off;
  std::weak_ptr<detail::RuntimeState> weak=s;
  s->options.httpOptions.httpClientFactory_create_fn=[weak]()noexcept{try{return detail::make_runtime_factory(weak);}catch(...){std::terminate();}};
  s->options.ioOptions.clientBootstrap_create_fn=[weak]()noexcept{
   try{auto state=weak.lock();if(!state||state->creator!=std::this_thread::get_id()||!main_entry())std::terminate();
    Aws::Crt::Io::EventLoopGroup group;Aws::Crt::Io::DefaultHostResolver resolver(group,8,30);
    state->bootstrap=Aws::MakeShared<Aws::Crt::Io::ClientBootstrap>(tag,group,resolver);
    if(state->bootstrap&&*state->bootstrap)state->bootstrap->EnableBlockingShutdown();return state->bootstrap;
   }catch(...){std::terminate();}
  };
  if(injected==RuntimeInitFixture::BeforeEntry)throw std::bad_alloc{};
  // Pinned InitAPI increments its global count before initialization and has
  // no proven exception rollback. From this boundary on, never release scope
  // or claim recoverable cleanup for a failed initialization attempt.
  s->initializing=true;
  if(injected==RuntimeInitFixture::AfterEntry)throw std::bad_alloc{};
  Aws::InitAPI(s->options);s->initializing=false;s->initialized=true;++s->initializations;
  if(!s->bootstrap||!*s->bootstrap||Aws::GetDefaultClientBootstrap()!=s->bootstrap.get()||!s->environment()){
   out.runtime->close();out.runtime.reset();out.failure=Failure::Unavailable;
  }
  return out;
 }catch(...){if(s&&s->initializing)std::terminate();if(out.runtime)out.runtime.reset();out.failure=Failure::LocalFailure;return out;}
}
ProvisionedSdkRuntime::~ProvisionedSdkRuntime(){
 if(state_->creator!=std::this_thread::get_id())std::terminate();
 if(state_->slot.load()==detail::RuntimeState::Slot::Occupied)std::terminate();
 if(!close())std::terminate();
}
bool ProvisionedSdkRuntime::close()noexcept {
 if(state_->creator!=std::this_thread::get_id())return false;
 if(state_->initializing)std::terminate();
 auto expected=detail::RuntimeState::Slot::Idle;
 if(!state_->slot.compare_exchange_strong(expected,detail::RuntimeState::Slot::Closed))return expected==detail::RuntimeState::Slot::Closed;
 if(state_->initialized){state_->bootstrap.reset();Aws::ShutdownAPI(state_->options);state_->initialized=false;++state_->shutdowns;}
 if(state_->scope){detail::release_sdk_scope();state_->scope=false;}return true;
}
RuntimeCounts ProvisionedSdkRuntime::counts()const noexcept {
 const auto slot=state_->slot.load();return {state_->initializations.load(),state_->shutdowns.load(),state_->idle_clients.load(),state_->idle_requests.load(),slot==detail::RuntimeState::Slot::Occupied,slot==detail::RuntimeState::Slot::Closed};
}
CreateOutcome ProvisionedSdkRuntime::create_operation(Context c,Request r,std::shared_ptr<ResponseObservationSource> observer,
 std::shared_ptr<ResponseSourceGeneration> gen,std::shared_ptr<WorkerCancellation> cancel,FrozenSigningSource&& source){
 return make(std::move(c),std::move(r),std::move(observer),std::move(gen),std::move(cancel),std::move(source),{});
}
CreateOutcome ProvisionedSdkRuntime::make(Context&& c,Request&& r,std::shared_ptr<ResponseObservationSource> observer,
 std::shared_ptr<ResponseSourceGeneration> gen,std::shared_ptr<WorkerCancellation> cancel,FrozenSigningSource&& supplied,std::optional<FixedCase> fixed){
 auto source=std::move(supplied);CreateOutcome out;
 auto expected=detail::RuntimeState::Slot::Idle;
 if(!state_->slot.compare_exchange_strong(expected,detail::RuntimeState::Slot::Occupied)){out.failure=Failure::Unavailable;return out;}
 bool handed=false;
 try {
  if(!detail::runtime_usable(state_)||!atom(source.identity_)||!atom(source.generation_)||source.identity_!=c.source_identity||source.generation_!=c.source_generation||
     !bytes(source.access_,true)||!bytes(source.secret_,true)||!bytes(source.token_,false)){
   detail::release_runtime_slot(state_);out.failure=Failure::InvalidSource;return out;
  }
  std::optional<FrozenNamedSource> frozen;
  {
   auto value=Aws::Auth::AWSCredentials{sdk_string(source.access_),sdk_string(source.secret_),sdk_string(source.token_)};
   frozen.emplace(FrozenNamedSource{std::move(value),source.identity_,source.generation_});
  } // All temporary SDK strings/credentials gone before State may release slot.
  handed=true;
  return NativeOwner::create_in_runtime(state_,std::move(c),std::move(r),std::move(observer),std::move(gen),std::move(cancel),std::move(*frozen),fixed);
 }catch(...){if(!handed)detail::release_runtime_slot(state_);out.failure=Failure::LocalFailure;return out;}
}
RuntimeCreateOutcome FixedRuntimeFixture::create_init_failure(RuntimeInitFixture fault){
 RuntimeCreateOutcome out;const char* marker=::getenv("ODBCPP_AUTH_SDK_OFFLINE_FIXTURE");
 if(!marker||std::string_view(marker)!="synthetic-fake-http-only"||static_cast<unsigned>(fault)>static_cast<unsigned>(RuntimeInitFixture::AfterEntry)){out.failure=Failure::Unavailable;return out;}
 return ProvisionedSdkRuntime::create_impl(fault);
}
Context FixedRuntimeFixture::context(RuntimeFixtureSource source){
 if(source==RuntimeFixtureSource::First)return FixedFixture::context();
 if(source!=RuntimeFixtureSource::Second)std::terminate();
 return Context{"000000000000","eu-north-1","fixture-cluster","fixture_db","fixture_user","db.fixture.invalid","synthetic-source","g2",true};
}
CreateOutcome FixedRuntimeFixture::create(ProvisionedSdkRuntime& runtime,FixedCase fixed,std::shared_ptr<ResponseObservationSource> observer,
 std::shared_ptr<ResponseSourceGeneration> gen,std::shared_ptr<WorkerCancellation> cancel,RuntimeFixtureSource selected){
 CreateOutcome out;const char* marker=::getenv("ODBCPP_AUTH_SDK_OFFLINE_FIXTURE");
 if(!marker||std::string_view(marker)!="synthetic-fake-http-only"||static_cast<unsigned>(fixed)>static_cast<unsigned>(FixedCase::Reentry)||static_cast<unsigned>(selected)>static_cast<unsigned>(RuntimeFixtureSource::Second)){out.failure=Failure::Unavailable;return out;}
 auto c=context(selected);auto request=FixedFixture::request(c);
 auto secret=[](std::string_view text){auto x=SecretBytes::create(std::as_bytes(std::span{text.data(),text.size()}));if(!x)std::terminate();return std::move(x).value();};
 auto made=FrozenSigningSource::create(c.source_identity,c.source_generation,secret("synthetic-access-key"),secret("synthetic-secret-key"),secret(""));
 if(!std::holds_alternative<FrozenSigningSource>(made)){out.failure=Failure::InvalidSource;return out;}
 return runtime.make(std::move(c),std::move(request),std::move(observer),std::move(gen),std::move(cancel),std::get<FrozenSigningSource>(std::move(made)),fixed);
}
} // namespace
