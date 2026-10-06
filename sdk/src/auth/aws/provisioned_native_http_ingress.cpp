#include "odbcpp/auth/aws/provisioned_native_http_ingress.h"
#include "odbcpp/auth/provisioned_query_transport.h"
#include "odbcpp/auth/checked_response_stream.h"
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/redshift/RedshiftClient.h>
#include <aws/redshift/RedshiftErrorMarshaller.h>
#include <aws/core/utils/xml/XmlSerializer.h>
#include <aws/redshift/model/GetClusterCredentialsRequest.h>
#include <algorithm>
#include <array>
#include <iostream>
#include <streambuf>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <latch>
#include <mutex>
#include "odbcpp/auth/aws/native_body_capture.h"
#include <aws/core/Globals.h>
#include <aws/core/http/crt/CRTHttpClient.h>
namespace rs::core::auth::aws::provisioned_native {
namespace {
constexpr const char* tag="ProvisionedNativeIngress";
std::string envelope(std::string_view leaves,bool iam=false) {
  const std::string method=iam?"GetClusterCredentialsWithIAM":"GetClusterCredentials";
  return "<"+method+"Response xmlns=\"http://redshift.amazonaws.com/doc/2012-12-01/\"><"+method+"Result>"+
      std::string(leaves)+"</"+method+"Result></"+method+"Response>";
}
// Only closed categories escape this private diagnostic boundary. The SDK's
// full error marshaller is deliberately not invoked: it retains/logs Message.
ProviderFailure typed_failure(Aws::Client::CoreErrors value) noexcept {
  using E=Aws::Client::CoreErrors;
  switch(value) {
    case E::ACCESS_DENIED:return ProviderFailure::AccessDenied;
    case E::INVALID_CLIENT_TOKEN_ID:case E::UNRECOGNIZED_CLIENT:case E::INVALID_ACCESS_KEY_ID:
    case E::INVALID_SIGNATURE:case E::SIGNATURE_DOES_NOT_MATCH:case E::INCOMPLETE_SIGNATURE:
    case E::CLIENT_SIGNING_FAILURE:return ProviderFailure::InvalidCredentials;
    case E::INVALID_ACTION:case E::INVALID_PARAMETER_COMBINATION:case E::INVALID_QUERY_PARAMETER:
    case E::INVALID_PARAMETER_VALUE:case E::MISSING_ACTION:case E::MISSING_PARAMETER:
    case E::MALFORMED_QUERY_STRING:case E::VALIDATION:case E::REQUEST_EXPIRED:
    case E::REQUEST_TIME_TOO_SKEWED:return ProviderFailure::InvalidRequest;
    case E::THROTTLING:case E::SLOW_DOWN:return ProviderFailure::Throttled;
    case E::NETWORK_CONNECTION:case E::ENDPOINT_RESOLUTION_FAILURE:return ProviderFailure::Transport;
    case E::REQUEST_TIMEOUT:return ProviderFailure::Timeout;
    case E::USER_CANCELLED:return ProviderFailure::Cancelled;
    default:return ProviderFailure::Unknown;
  }
}
std::string error_envelope(std::string_view code) {
  return "<ErrorResponse><Error><Code>"+std::string(code)+
      "</Code><Message>synthetic-secret-never-diagnose</Message></Error><RequestId>synthetic-identity</RequestId></ErrorResponse>";
}
std::string payload(FixedCase selected) {
  const std::string leaves="<DbUser>IAM:fixture_user</DbUser><DbPassword>synthetic-only</DbPassword><Expiration>2030-03-17T17:46:40.125001Z</Expiration>";
  auto good=envelope(leaves);
  switch(selected) {
    case FixedCase::ServiceDenied:return error_envelope("AccessDeniedException");
    case FixedCase::ServiceExpired:return error_envelope("ExpiredTokenException");
    case FixedCase::ServiceInvalidToken:return error_envelope("InvalidClientTokenId");
    case FixedCase::ServiceRequestExpired:return error_envelope("RequestExpired");
    case FixedCase::ServiceInvalid:return error_envelope("InvalidParameterValue");
    case FixedCase::ServiceThrottled:return error_envelope("ThrottlingException");
    case FixedCase::ServiceDuplicate:return "<ErrorResponse><Error><Code>AccessDenied</Code><Code>ExpiredTokenException</Code></Error></ErrorResponse>";
    case FixedCase::ServiceMissing:return "<ErrorResponse><Error><Message>synthetic-secret</Message></Error></ErrorResponse>";
    case FixedCase::ServiceMalformed:return "<ErrorResponse><Error><Code>AccessDenied";
    case FixedCase::ServiceUnknown:return error_envelope("UnknownSyntheticError");
    case FixedCase::ServiceTwoErrors:return "<ErrorResponse><Error><Code>AccessDenied</Code></Error><Error><Code>AccessDenied</Code></Error></ErrorResponse>";
    case FixedCase::ServiceNestedCode:return "<ErrorResponse><Error><Code><Code>AccessDenied</Code></Code></Error></ErrorResponse>";
    case FixedCase::ServiceDtd:return "<!DOCTYPE ErrorResponse><ErrorResponse><Error><Code>AccessDenied</Code></Error></ErrorResponse>";
    case FixedCase::ServiceLongCode:return error_envelope(std::string(129,'A'));
    case FixedCase::ServiceWrongNamespace:return "<ErrorResponse xmlns=\"https://foreign.invalid/\"><Error><Code>AccessDeniedException</Code></Error></ErrorResponse>";
    case FixedCase::ServiceWrongErrorNamespace:return "<ErrorResponse><Error xmlns=\"https://foreign.invalid/\"><Code>AccessDeniedException</Code></Error></ErrorResponse>";
    case FixedCase::ServiceWrongCodeNamespace:return "<ErrorResponse><Error><Code xmlns=\"https://foreign.invalid/\">AccessDeniedException</Code></Error></ErrorResponse>";
    case FixedCase::ServiceQueryNamespace:return "<ErrorResponse xmlns=\"http://redshift.amazonaws.com/doc/2012-12-01/\"><Error><Code>AccessDeniedException</Code></Error></ErrorResponse>";
    case FixedCase::DuplicateUser:return envelope(leaves+"<DbUser>other</DbUser>");
    case FixedCase::WrongUser:return envelope("<DbUser>IAM:other</DbUser><DbPassword>synthetic-only</DbPassword><Expiration>2030-03-17T17:46:40.125001Z</Expiration>");
    case FixedCase::WrongEnvelope:return envelope(leaves,true);
    case FixedCase::ExactCap:return good+std::string(StreamOwner::max_bytes-good.size(),' ');
    case FixedCase::Overflow:return good+std::string(StreamOwner::max_bytes+1-good.size(),' ');
    default:return good;
  }
}
}
namespace detail {
struct Wire final {
  Wire(rs::core::auth::aws::NativeCaptureWriter value,std::optional<FixedCase> selected,std::shared_ptr<WorkerCancellation> cancelled):writer(std::move(value)),scenario(selected),cancellation(std::move(cancelled)) {}
  rs::core::auth::aws::NativeCaptureWriter writer;const std::optional<FixedCase> scenario;const std::shared_ptr<WorkerCancellation> cancellation;
  rs::core::auth::aws::FrozenNativeBody* reader{}; // creator-only, explicitly revoked before destruction
  std::atomic<unsigned> callbacks{},active{},wrappers{},destroyed{},written{},writer_calls{},reads{},
      validation_bytes{},model_bytes{},error_bytes{},returns{},destructions{},wrappers_at_freeze{};
  bool validating{},diagnosing{},barrier{};std::ios::iostate flags{std::ios::goodbit};
};
}
namespace {
class PassiveBuffer final:public std::streambuf {
 public: explicit PassiveBuffer(std::shared_ptr<detail::Wire> wire):wire_(std::move(wire)) {}
 protected:
  std::streamsize xsputn(const char* bytes,std::streamsize n) override {
    if(n<0) return 0;
    ++wire_->active;struct Active{std::atomic<unsigned>& value;~Active(){--value;}}active{wire_->active};
    ++wire_->writer_calls;auto got=wire_->writer.acquire();
    if(!std::holds_alternative<rs::core::auth::aws::NativeCaptureWriteGuard>(got)) return 0;
    auto guard=std::get<rs::core::auth::aws::NativeCaptureWriteGuard>(std::move(got));
    const auto result=guard.append({reinterpret_cast<const std::byte*>(bytes),static_cast<std::size_t>(n)});
    if(!std::holds_alternative<std::monostate>(result)) return 0;
    wire_->written.fetch_add(static_cast<unsigned>(n));return n;
  }
  int_type overflow(int_type c) override {
    if(traits_type::eq_int_type(c,traits_type::eof())) return traits_type::not_eof(c);
    const auto ch=traits_type::to_char_type(c);return xsputn(&ch,1)==1?c:traits_type::eof();
  }
  int_type underflow() override {
    if(!wire_->reader || position_>=wire_->written.load()) return traits_type::eof();
    std::byte byte{};const auto result=wire_->reader->read(position_,{&byte,1});
    if(!std::holds_alternative<std::monostate>(result)) return traits_type::eof();
    return traits_type::to_int_type(static_cast<char>(byte));
  }
  int_type uflow() override {const auto c=underflow();if(!traits_type::eq_int_type(c,traits_type::eof())) {++position_;count(1);}return c;}
  std::streamsize xsgetn(char* output,std::streamsize n) override {
    if(n<0 || !wire_->reader) return 0;
    const auto remaining=static_cast<std::size_t>(wire_->written.load())-position_;
    const auto wanted=std::min(static_cast<std::size_t>(n),remaining);
    ++wire_->reads;const auto result=wire_->reader->read(position_,{reinterpret_cast<std::byte*>(output),wanted});--wire_->reads;
    if(!std::holds_alternative<std::monostate>(result)) return 0;
    position_+=wanted;count(static_cast<unsigned>(wanted));return static_cast<std::streamsize>(wanted);
  }
  pos_type seekoff(off_type n,std::ios::seekdir direction,std::ios::openmode mode) override {
    const auto size=static_cast<off_type>(wire_->written.load());
    if(mode==std::ios::out && n==0 && direction==std::ios::cur) return pos_type(size);
    if(mode!=std::ios::in || !wire_->reader || wire_->scenario==FixedCase::SeekFailure) return pos_type(off_type(-1));
    const auto base=direction==std::ios::beg?off_type{0}:direction==std::ios::cur?static_cast<off_type>(position_):
        direction==std::ios::end?size:off_type{-1};
    if(base<0 || n< -base || n>size-base) return pos_type(off_type(-1));
    position_=static_cast<std::size_t>(base+n);return pos_type(base+n);
  }
  pos_type seekpos(pos_type n,std::ios::openmode mode) override {return seekoff(static_cast<off_type>(n),std::ios::beg,mode);}
 private:
  void count(unsigned n) noexcept {if(wire_->diagnosing) wire_->error_bytes+=n;else if(wire_->validating) wire_->validation_bytes+=n;else wire_->model_bytes+=n;}
  std::shared_ptr<detail::Wire> wire_;std::size_t position_{};
};
class Body final:public Aws::IOStream {
 public:
  explicit Body(std::shared_ptr<detail::Wire> wire):Aws::IOStream(nullptr),wire_(std::move(wire)),buffer_(wire_) {
    rdbuf(&buffer_);exceptions(std::ios::goodbit);++wire_->wrappers;
  }
  ~Body() override {if(wire_->reads.load() || wire_->active.load()) std::terminate();rdbuf(nullptr);++wire_->destroyed;}
 private: std::shared_ptr<detail::Wire> wire_;PassiveBuffer buffer_;
};
class ExclusiveFakeDelegate final:public Aws::Http::HttpClient {
 public:
  ExclusiveFakeDelegate(std::shared_ptr<detail::Wire> wire,FixedCase scenario):wire_(std::move(wire)),scenario_(scenario) {}
  ~ExclusiveFakeDelegate() override {release_.count_down();if(worker_.joinable()) worker_.join();++wire_->destructions;}
  std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(const std::shared_ptr<Aws::Http::HttpRequest>& request,
      Aws::Utils::RateLimits::RateLimiterInterface*,Aws::Utils::RateLimits::RateLimiterInterface*) const noexcept override {
    try {
      if(scenario_==FixedCase::NullResponse) {++wire_->returns;return nullptr;}
      auto response=Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>(tag,request);
      response->SetResponseCode((scenario_==FixedCase::Non200 || (scenario_>=FixedCase::ServiceDenied && scenario_<=FixedCase::ServiceQueryNamespace))?Aws::Http::HttpResponseCode::BAD_REQUEST:Aws::Http::HttpResponseCode::OK);
      if(scenario_==FixedCase::TransportError || scenario_==FixedCase::TransportTimeout || scenario_==FixedCase::TransportCancelled) {
        response->SetClientErrorType(scenario_==FixedCase::TransportError?Aws::Client::CoreErrors::NETWORK_CONNECTION:
            scenario_==FixedCase::TransportTimeout?Aws::Client::CoreErrors::REQUEST_TIMEOUT:Aws::Client::CoreErrors::USER_CANCELLED);
        response->SetClientErrorMessage("synthetic-secret-never-diagnose");
      }
      response->AddHeader("content-type",scenario_==FixedCase::WrongMime?"application/json":"text/xml");
      if(scenario_==FixedCase::WrongMime) response->AddHeader("content-encoding","gzip");
      if(scenario_==FixedCase::ForeignResponse) {
        auto foreign=Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag,Aws::Http::URI("https://foreign.fixture.invalid/"),Aws::Http::HttpMethod::HTTP_POST);
        response->SetOriginatingRequest(foreign);
      }
      worker_=std::thread([this,response,request]() noexcept {
        release_.wait();++wire_->active;++wire_->callbacks;
        try {
          if(scenario_==FixedCase::CancelInWorker) wire_->cancellation->cancel();
          if(ContinueRequest(*request)) {
            auto data=payload(scenario_);response->GetResponseBody().write(data.data(),static_cast<std::streamsize>(data.size()));
            if(scenario_==FixedCase::BadFlags) response->GetResponseBody().setstate(std::ios::badbit);
          }
          wire_->flags=response->GetResponseBody().rdstate();
        } catch(...) {
          auto guard=wire_->writer.acquire();if(auto* g=std::get_if<rs::core::auth::aws::NativeCaptureWriteGuard>(&guard)) (void)g->reject(rs::core::auth::aws::CaptureFailure::CallbackFailure);
        }
        --wire_->active;
      });
      ++wire_->returns;return response;
    } catch(...) {std::terminate();}
  }
 private:std::shared_ptr<detail::Wire> wire_;FixedCase scenario_;mutable std::latch release_{1};mutable std::thread worker_;
};

// Bounded empty response has no operation borrow and cannot invoke foreign factory.
class EmptyBuffer final:public std::streambuf {
 protected:
  int_type overflow(int_type) override { return traits_type::eof(); }
  std::streamsize xsputn(const char*,std::streamsize) override { return 0; }
};
class EmptyBody final:public Aws::IOStream {
 public: EmptyBody():Aws::IOStream(nullptr) { rdbuf(&buffer_);exceptions(std::ios::goodbit); } // SDK allocator owns this wrapper.
 private: EmptyBuffer buffer_;
};
std::shared_ptr<Aws::Http::HttpResponse> safe_response(const std::shared_ptr<Aws::Http::HttpRequest>& request,
    detail::State& state) {
  auto actual=request;
  if(!actual) actual=Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag,Aws::Http::URI("https://refused.fixture.invalid/"),Aws::Http::HttpMethod::HTTP_POST);
  auto safe=Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag,actual->GetUri(),actual->GetMethod());
  safe->SetResponseStreamFactory([]()->Aws::IOStream* { return Aws::New<EmptyBody>(tag); });
  auto response=Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>(tag,safe);
  response->SetOriginatingRequest(actual);
  response->SetClientErrorType(Aws::Client::CoreErrors::VALIDATION);
  response->SetClientErrorMessage("Provisioned response refused");
  ++state.metrics.safe_refusals;return response;
}
}
} // namespace rs::core::auth::aws::provisioned_native
namespace rs::core::auth::aws::detail {
class ProvisionedNativeCaptureOwner final {
 public:
  struct Result {
    std::shared_ptr<Aws::Http::HttpResponse> response;
    std::shared_ptr<rs::core::auth::aws::provisioned_native::detail::Wire> wire;
    std::optional<FrozenNativeBody> body;
  };
  static Result run(const std::shared_ptr<Aws::Http::HttpRequest>& request,
      rs::core::auth::aws::provisioned_native::detail::State& state,
      Aws::Utils::RateLimits::RateLimiterInterface* read,Aws::Utils::RateLimits::RateLimiterInterface* write) {
    namespace f=rs::core::auth::aws::provisioned_native;
    auto made=NativeBodyCapture::create(NativeBodyCapture::max_bytes);
    if(!std::holds_alternative<NativeBodyCapture>(made)) throw std::bad_alloc{};
    auto owner=std::get<NativeBodyCapture>(std::move(made));auto endpoint=owner.writer();
    if(!std::holds_alternative<NativeCaptureWriter>(endpoint)) std::terminate();
    auto wire=std::make_shared<f::detail::Wire>(std::get<NativeCaptureWriter>(std::move(endpoint)),state.fixed,state.cancellation);
    request->SetResponseStreamFactory([wire]() noexcept->Aws::IOStream* {
      try {
        // Closed fake seam: simulate wrapper construction refusal locally; never
        // unwind into the SDK compiled without exceptions.
        if(wire->scenario==f::FixedCase::AdoptionFailure)throw std::bad_alloc{};
        return Aws::New<f::Body>(f::tag,wire);
      } catch(...) {
        auto value=wire->writer.acquire();
        if(auto* guard=std::get_if<NativeCaptureWriteGuard>(&value))(void)guard->reject(CaptureFailure::CallbackFailure);
        try{return Aws::New<f::EmptyBody>(f::tag);}catch(...){std::terminate();}
      }
    });
    request->SetContinueRequestHandle([wire](const Aws::Http::HttpRequest*) noexcept {
      ++wire->active;struct Active{std::atomic<unsigned>& value;~Active(){--value;}}active{wire->active};
      return !wire->cancellation->cancelled() && !wire->writer.failure();
    });
    std::shared_ptr<Aws::Http::HttpResponse> response;
    if(state.fixed) {
      auto delegate=std::make_unique<f::ExclusiveFakeDelegate>(wire,*state.fixed);
      response=delegate->MakeRequest(request,read,write);
      state.metrics.return_before_tail=wire->written.load()==0 && wire->destructions.load()==0;
      delegate.reset(); // releases/join fake worker; return alone is not handoff
    } else {
      Aws::Client::ClientConfigurationInitValues init;init.shouldDisableIMDS=true;
      Aws::Client::ClientConfiguration config(init);config.disableIMDS=true;
      config.region=state.context.region.c_str();config.scheme=Aws::Http::Scheme::HTTPS;config.verifySSL=true;
      config.proxyHost.clear();config.proxyUserName.clear();config.proxyPassword.clear();
      config.retryStrategy=Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(f::tag,0,0);
      using Native=Aws::Http::CRTHttpClient;
      auto deleter=[](Native* p) noexcept {Aws::Delete(p);};
      std::unique_ptr<Native,decltype(deleter)> delegate{Aws::New<Native>(f::tag,config,*state.bootstrap),deleter};
      if(!delegate || !*delegate) throw std::runtime_error("Private delegate refused");
      response=delegate->MakeRequest(request,read,write);++wire->returns;
      delegate.reset();++wire->destructions; // SDK may block here; not hard D
    }
    if(wire->active.load()!=0 || wire->destructions.load()!=1 || wire->returns.load()!=1) std::terminate();
    if(response) wire->flags=response->GetResponseBody().rdstate();
    wire->barrier=true;wire->wrappers_at_freeze=wire->wrappers.load();
    NativeCaptureHandoff handoff{std::move(owner.state_)};
    auto body=handoff.freeze();Result result{std::move(response),wire,{}};
    if(auto* frozen=std::get_if<FrozenNativeBody>(&body)) result.body.emplace(std::move(*frozen));
    return result;
  }
 private:ProvisionedNativeCaptureOwner()=delete;
};
}
namespace rs::core::auth::aws::provisioned_native {
namespace {
// Narrow Query error subset: one ErrorResponse/Error, one leaf Code. No
// prefix/DTD/custom entities or arbitrary code strings become public diagnostics.
ProviderFailure service_failure(Aws::Http::HttpResponse& response,detail::State& state) noexcept {
  try {
    if(!state.check() || response.GetContentType()!="text/xml" || response.HasHeader("content-encoding") ||
       !dynamic_cast<Body*>(&response.GetResponseBody())) return ProviderFailure::Unknown;
    auto& io=response.GetResponseBody();const auto put=io.tellp();
    if(state.wire->flags!=std::ios::goodbit || io.fail() || io.bad() || put<=0 ||
       put>static_cast<std::streamoff>(StreamOwner::max_bytes)) return ProviderFailure::Unknown;
    io.seekg(0);if(!io) return ProviderFailure::Unknown;
    state.wire->diagnosing=true;
    struct Phase {detail::Wire& wire;~Phase(){wire.diagnosing=false;}} phase{*state.wire};
    Aws::String raw(static_cast<std::size_t>(put),'\0');
    struct Wipe {Aws::String& value;~Wipe(){volatile char* p=value.data();for(std::size_t i=0;i<value.size();++i)p[i]=0;}} wipe{raw};
    io.read(raw.data(),static_cast<std::streamsize>(put));state.sync();
    if(!io || io.gcount()!=put || !state.check()) return ProviderFailure::Unknown;
    if(raw.find("<!")!=Aws::String::npos || raw.find('\0')!=Aws::String::npos) return ProviderFailure::Unknown;
    auto doc=Aws::Utils::Xml::XmlDocument::CreateFromXmlString(raw);
    if(!doc.WasParseSuccessful()) return ProviderFailure::Unknown;
    auto root=doc.GetRootElement();if(root.IsNull() || root.GetName()!="ErrorResponse" || !root.NextNode().IsNull()) return ProviderFailure::Unknown;
    const auto xmlns=root.GetAttributeValue("xmlns");
    if(!xmlns.empty() && xmlns!="http://redshift.amazonaws.com/doc/2012-12-01/") return ProviderFailure::Unknown;
    auto error=root.FirstChild("Error");unsigned errors=0;
    for(auto node=root.FirstChild();!node.IsNull();node=node.NextNode()) {
      if(node.GetName()=="Error") {error=node;++errors;}
      else if(node.GetName()!="RequestId") return ProviderFailure::Unknown;
    }
    if(errors!=1) return ProviderFailure::Unknown;
    const auto error_xmlns=error.GetAttributeValue("xmlns");
    if(!error_xmlns.empty() && error_xmlns!="http://redshift.amazonaws.com/doc/2012-12-01/") return ProviderFailure::Unknown;
    auto code=error.FirstChild("Code");unsigned codes=0;
    for(auto node=error.FirstChild();!node.IsNull();node=node.NextNode()) {
      if(node.GetName()=="Code") {code=node;++codes;}
      else if(node.GetName()!="Message" && node.GetName()!="Type") return ProviderFailure::Unknown;
    }
    if(codes!=1 || !code.FirstChild().IsNull()) return ProviderFailure::Unknown;
    const auto code_xmlns=code.GetAttributeValue("xmlns");
    if(!code_xmlns.empty() && code_xmlns!="http://redshift.amazonaws.com/doc/2012-12-01/") return ProviderFailure::Unknown;
    auto token=code.GetText();
    if(token.empty() || token.size()>128 || !std::all_of(token.begin(),token.end(),[](unsigned char c){
       return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9');})) return ProviderFailure::Unknown;
    const auto category=token=="ExpiredTokenException"?ProviderFailure::ExpiredCredentials:
        typed_failure(Aws::Client::RedshiftErrorMarshaller{}.FindErrorByName(token.c_str()).GetErrorType());
    return state.check()?category:ProviderFailure::Unknown;
  } catch(...) {return ProviderFailure::Unknown;}
}
class IngressHttp final:public Aws::Http::HttpClient {
 public:
  explicit IngressHttp(detail::State& state):state_(state) {}
  explicit IngressHttp(std::shared_ptr<detail::State> state):state_(*state),keep_(std::move(state)) {}
  ~IngressHttp() override {++state_.metrics.destroyed_clients;}
  std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(const std::shared_ptr<Aws::Http::HttpRequest>& request,
      Aws::Utils::RateLimits::RateLimiterInterface* read,Aws::Utils::RateLimits::RateLimiterInterface* write) const noexcept override {
    try {
      if(state_.creator!=std::this_thread::get_id()) std::terminate();
      if(!request) {
        ++state_.metrics.null_requests;
        auto bounded=Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag,Aws::Http::URI("https://refused.fixture.invalid/"),Aws::Http::HttpMethod::HTTP_POST);
        bounded->SetResponseStreamFactory([]()->Aws::IOStream* {return Aws::New<EmptyBody>(tag);});
        return safe_response(bounded,state_);
      }
      const bool selected=request->GetUri().GetScheme()==Aws::Http::Scheme::HTTPS && request->GetMethod()==Aws::Http::HttpMethod::HTTP_POST &&
          std::string_view(request->GetUri().GetAuthority())==(state_.fixed?std::string_view("redshift.fixture.invalid"):std::string_view(state_.context.api_endpoint).substr(8)) && request->GetUri().GetPath()=="/";
      if(!selected) { ++state_.metrics.foreign_requests;return safe_response(request,state_); }
      if(state_.selected_client!=this || state_.selected_request!=request.get() || state_.attempt || !state_.check()) {
        if(state_.dispatch) { state_.reject(); }return safe_response(request,state_);
      }
      auto input=request->GetContentBody();
      if(!input) { state_.reject();return safe_response(request,state_); }
      const auto original=input->tellg();
      if(original<0 || original!=0 || input->bad() || input->fail()){state_.reject();return safe_response(request,state_);}
      std::array<char,4096> query{};input->read(query.data(),static_cast<std::streamsize>(query.size()));
      const auto n=input->gcount();const std::string_view value(query.data(),static_cast<std::size_t>(n));
      state_.metrics.request_exact=value==state_.expected_query;
      const bool read_ok=!input->bad();
      input->clear();input->seekg(original);query.fill(0);
      if(!read_ok || !state_.metrics.request_exact || !*input || input->tellg()!=original || !state_.check()) { state_.reject();return safe_response(request,state_); }
      state_.attempt=true;
      if(state_.fixed==FixedCase::Reentry){auto refused=MakeRequest(request,read,write);if(!refused||!refused->HasClientError())std::terminate();return safe_response(request,state_);}
      ++state_.metrics.selected_sends;
      auto captured=rs::core::auth::aws::detail::ProvisionedNativeCaptureOwner::run(request,state_,read,write);
      state_.wire=std::move(captured.wire);state_.sync();
      auto response=std::move(captured.response);
      const auto refuse_gate=[&](ResponseGate gate){
        if(state_.metrics.first_response_gate==ResponseGate::None)state_.metrics.first_response_gate=gate;
        state_.reject();
        if(!state_.metrics.provider_failure) {
          state_.metrics.provider_failure=state_.metrics.boundary==BoundaryFailure::DeadlineElapsed?ProviderFailure::Timeout:
              state_.metrics.boundary==BoundaryFailure::Cancelled?ProviderFailure::Cancelled:ProviderFailure::Unknown;
        }
        return safe_response(request,state_);
      };
      if(!captured.body) return refuse_gate(ResponseGate::CaptureMissing);
      state_.frozen.emplace(std::move(*captured.body));state_.wire->reader=&*state_.frozen;
      if(state_.frozen->failure()) return refuse_gate(ResponseGate::CaptureFailure);
      // Split the former OR guard in exactly its original short-circuit order.
      if(!state_.check()) return refuse_gate(ResponseGate::Checkpoint);
      if(!response) return refuse_gate(ResponseGate::MissingResponse);
      if(&response->GetOriginatingRequest()!=request.get()) return refuse_gate(ResponseGate::ForeignResponse);
      const auto status=response->GetResponseCode();
      const auto code=static_cast<int>(status);
      state_.metrics.response_status=code==200?ResponseStatus::Success200:
          code>=200&&code<300?ResponseStatus::Other2xx:
          code>=300&&code<400?ResponseStatus::Redirect3xx:
          code>=400&&code<500?ResponseStatus::Client4xx:
          code>=500&&code<600?ResponseStatus::Server5xx:ResponseStatus::Other;
      // Passive error-presence observation also covers non200/invalid codes.
      // Refusal precedence stays status before client error; no repeated check.
      const bool client_error=response->HasClientError();state_.metrics.response_client_error=client_error;
      if(status!=Aws::Http::HttpResponseCode::OK) {
        const auto detail=client_error?typed_failure(response->GetClientErrorType()):service_failure(*response,state_);
        // Every parser return, including malformed/unsupported early exits, must
        // observe the original boundary before terminal BodyRejected can win.
        const bool current=state_.check();
        state_.metrics.provider_failure=current?detail:ProviderFailure::Unknown;
        state_.sync();return refuse_gate(ResponseGate::Non200);
      }
      if(client_error) {state_.metrics.provider_failure=typed_failure(response->GetClientErrorType());return refuse_gate(ResponseGate::ClientError);}
      if(response->GetContentType()!="text/xml") return refuse_gate(ResponseGate::Mime);
      if(response->HasHeader("content-encoding")) return refuse_gate(ResponseGate::Encoding);
      if(!dynamic_cast<Body*>(&response->GetResponseBody())) return refuse_gate(ResponseGate::BodyWrapper);
      auto& io=response->GetResponseBody();
      state_.validating=true;state_.wire->validating=true;
      struct ValidationGuard { detail::State& state;~ValidationGuard() { state.validating=false;if(state.wire) state.wire->validating=false; } } phase{state_};
      const auto put=io.tellp();
      if(state_.wire->flags!=std::ios::goodbit || io.bad() || io.fail()) return refuse_gate(ResponseGate::StreamFlags);
      if(put<=0 || put>static_cast<std::streamoff>(StreamOwner::max_bytes)) return refuse_gate(ResponseGate::PutSize);
      io.seekg(0);if(!io) return refuse_gate(ResponseGate::BodySeek);
      auto copied=CheckedStreamOwner::create(StreamOwner::max_bytes,state_.lease());
      if(!std::holds_alternative<CheckedStreamOwner>(copied)) return refuse_gate(ResponseGate::CheckedStreamCreation);
      auto owner=std::get<CheckedStreamOwner>(std::move(copied));
      std::array<char,4096> chunk{};
      struct Wipe { std::array<char,4096>& bytes;~Wipe() { volatile char* p=bytes.data();for(std::size_t i=0;i<bytes.size();++i) { p[i]=0; } } } wiping{chunk};
      std::streamoff remaining=put;
      while(remaining>0) {
        const auto count=static_cast<std::streamsize>(std::min<std::streamoff>(remaining,chunk.size()));
        io.read(chunk.data(),count);
        if(io.gcount()!=count || !io || !state_.check()) return refuse_gate(ResponseGate::BodyCopy);
        owner.io()->write(chunk.data(),count);remaining-=count;
      }
      auto body=seal_checked_response(std::move(owner));
      if(!std::holds_alternative<ResponseBytes>(body)) return refuse_gate(ResponseGate::CheckedStreamSeal);
      auto validated=prevalidate_provisioned_query_transport(std::get<ResponseBytes>(std::move(body)),
          state_.lease(),state_.context.expected_user);
      if(!std::holds_alternative<ExtractedDbFields>(validated)) { state_.reject();return safe_response(request,state_); }
      io.seekg(0);
      state_.metrics.rewound=io && io.tellg()==0;
      state_.metrics.put_preserved=io && io.tellp()==put;
      if(state_.fixed==FixedCase::LateAfterValidation) { (void)state_.wire->writer.acquire(); }
      state_.sync();
      if(state_.frozen->failure() || !state_.metrics.rewound || !state_.metrics.put_preserved || !state_.check()) { state_.reject();return safe_response(request,state_); }
      state_.fields.emplace(std::get<ExtractedDbFields>(std::move(validated)));
      return response;
    } catch(...) { state_.reject();return safe_response(request,state_); }
  }
 private:detail::State& state_;std::shared_ptr<detail::State> keep_;
};
// SDK initialization may construct the metadata client. It is never a selected
// service client, and no discovery send is permitted by this standalone scope.
class NoSend final:public Aws::Http::HttpClient {
 public:explicit NoSend(detail::State& s):state_(s){}
 ~NoSend()override{++state_.metrics.destroyed_clients;}
 std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(const std::shared_ptr<Aws::Http::HttpRequest>& request,
  Aws::Utils::RateLimits::RateLimiterInterface*,Aws::Utils::RateLimits::RateLimiterInterface*)const noexcept override {
  try{if(state_.creator!=std::this_thread::get_id())std::terminate();state_.reject();return safe_response(request,state_);}catch(...){std::terminate();}
 }
 private:detail::State& state_;
};
class Factory final:public Aws::Http::HttpClientFactory {
 public:
  explicit Factory(detail::State& state):state_(state) {}
  std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(const Aws::Client::ClientConfiguration&) const noexcept override {
    try { ++state_.metrics.clients;if(!state_.service_live)return Aws::MakeShared<NoSend>(tag,state_);return Aws::MakeShared<IngressHttp>(tag,state_); }
    catch(...) { std::terminate(); }
  }
  std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(const Aws::String& uri,Aws::Http::HttpMethod method,
      const Aws::IOStreamFactory& factory) const noexcept override { return CreateHttpRequest(Aws::Http::URI(uri),method,factory); }
  std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(const Aws::Http::URI& uri,Aws::Http::HttpMethod method,
      const Aws::IOStreamFactory& factory) const noexcept override {
    try {
      ++state_.metrics.requests;
      auto request=Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag,uri,method);
      request->SetResponseStreamFactory(factory);
      if(std::string_view(uri.GetAuthority())==(state_.fixed?std::string_view("redshift.fixture.invalid"):std::string_view(state_.context.api_endpoint).substr(8))) {
        if(state_.selected_request) { state_.reject(); }
        else state_.selected_request=request.get();
      }
      return request;
    } catch(...) { std::terminate(); }
  }
 private:detail::State& state_;
};
thread_local std::shared_ptr<detail::State> runtime_route;
class RuntimeNoSend final:public Aws::Http::HttpClient {
 public:
  std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(const std::shared_ptr<Aws::Http::HttpRequest>&,
      Aws::Utils::RateLimits::RateLimiterInterface*,Aws::Utils::RateLimits::RateLimiterInterface*)const noexcept override {
    try {
      auto safe=Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag,Aws::Http::URI("https://refused.fixture.invalid/"),Aws::Http::HttpMethod::HTTP_POST);
      safe->SetResponseStreamFactory([]()->Aws::IOStream*{return Aws::New<EmptyBody>(tag);});
      auto response=Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>(tag,safe);
      response->SetClientErrorType(Aws::Client::CoreErrors::VALIDATION);response->SetClientErrorMessage("Unbound provisioned request refused");return response;
    }catch(...){std::terminate();}
  }
};
class RuntimeFactory final:public Aws::Http::HttpClientFactory {
 public:explicit RuntimeFactory(std::weak_ptr<detail::RuntimeState> runtime):runtime_(std::move(runtime)){}
  std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(const Aws::Client::ClientConfiguration&) const noexcept override {
    try {
      auto state=runtime_route;
      if(state && state->service_live){++state->metrics.clients;return Aws::MakeShared<IngressHttp>(tag,std::move(state));}
      detail::note_runtime_idle(runtime_,false);return Aws::MakeShared<RuntimeNoSend>(tag);
    }catch(...){std::terminate();}
  }
  std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(const Aws::String& uri,Aws::Http::HttpMethod method,const Aws::IOStreamFactory& factory)const noexcept override{return CreateHttpRequest(Aws::Http::URI(uri),method,factory);}
  std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(const Aws::Http::URI& uri,Aws::Http::HttpMethod method,const Aws::IOStreamFactory& factory)const noexcept override{
    try {
      auto state=runtime_route;
      if(state && state->service_live)return Factory{*state}.CreateHttpRequest(uri,method,factory);
      detail::note_runtime_idle(runtime_,true);
      auto safe=Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag,uri,method);
      safe->SetResponseStreamFactory([]()->Aws::IOStream*{return Aws::New<EmptyBody>(tag);});return safe;
    }catch(...){std::terminate();}
  }
 private:std::weak_ptr<detail::RuntimeState> runtime_;
};

}
namespace detail {
RuntimeRoute::RuntimeRoute(std::shared_ptr<State> state):state_(std::move(state)){
 if(!state_||state_->creator!=std::this_thread::get_id()||runtime_route)std::terminate();runtime_route=state_;
}
RuntimeRoute::~RuntimeRoute(){if(state_->creator!=std::this_thread::get_id()||runtime_route!=state_)std::terminate();runtime_route.reset();}
std::shared_ptr<Aws::Http::HttpClientFactory> make_runtime_factory(std::weak_ptr<RuntimeState> runtime){return Aws::MakeShared<RuntimeFactory>(tag,std::move(runtime));}
std::shared_ptr<Aws::Http::HttpClientFactory> make_factory(State& s){return Aws::MakeShared<Factory>(tag,s);}
void State::sync() noexcept {
 if(!wire) return;
 metrics.active=wire->active.load();metrics.wrappers=wire->wrappers.load();metrics.destroyed_wrappers=wire->destroyed.load();
 metrics.validation_bytes=wire->validation_bytes.load();metrics.model_bytes=wire->model_bytes.load();metrics.error_diagnostic_bytes=wire->error_bytes.load();
 metrics.delegate_returns=wire->returns.load();metrics.delegate_destructions=wire->destructions.load();metrics.barrier=wire->barrier;
}
void detach_reader(State& s) noexcept {
 s.sync();if(s.wire){if(s.wire->reads.load() || s.wire->active.load() || s.wire->wrappers.load()!=s.wire->destroyed.load())std::terminate();s.wire->reader=nullptr;}s.frozen.reset();
}
void late_writer(State& s) noexcept {if(s.wire)(void)s.wire->writer.acquire();}
void check_capture(State& s) noexcept {if(s.wire && s.wire->writer.failure()){if(s.dispatch)s.reject();else if(!s.operation->fault())std::terminate();}}
}
}
