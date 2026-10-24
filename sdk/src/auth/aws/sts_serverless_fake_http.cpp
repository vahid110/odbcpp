#include "odbcpp/auth/aws/sts_serverless_fake_http.h"
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/http/standard/StandardHttpRequest.h>
#include <aws/core/http/standard/StandardHttpResponse.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <cstdlib>
#include <chrono>
#include <streambuf>
#include <thread>

namespace rs::core::auth::aws::detail {
namespace {
constexpr const char* tag = "StsServerlessFakeHttp";

// Minimal empty body for refused/denied responses; never exposes source data.
class RefuseBuffer final : public std::streambuf {
 protected:
  int_type overflow(int_type) override { return traits_type::eof(); }
  std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
};
class RefuseBody final : public Aws::IOStream {
 public:
  RefuseBody() : Aws::IOStream(nullptr) { rdbuf(&buffer_); }
 private:
  RefuseBuffer buffer_;
};

struct Config {
  std::atomic<unsigned> scenario{static_cast<unsigned>(FakeHttpScenario::Success)};
  std::atomic<unsigned> delay_ms{0};
  std::shared_ptr<provisioned_native::WorkerCancellation> cancellation;
};
Config& config() noexcept {
  static Config value;
  return value;
}

std::string sts_xml() {
  return "<AssumeRoleResponse xmlns=\"https://sts.amazonaws.com/doc/2011-06-15/\">"
      "<AssumeRoleResult><Credentials>"
      "<AccessKeyId>ASIA_ASSUMED_KEY</AccessKeyId>"
      "<SecretAccessKey>assumed-secret-key</SecretAccessKey>"
      "<SessionToken>assumed-session-token</SessionToken>"
      "<Expiration>2030-03-17T17:46:40Z</Expiration>"
      "</Credentials><AssumedRoleUser>"
      "<Arn>arn:aws:sts::123456789012:assumed-role/MyRole/session</Arn>"
      "<AssumedRoleId>AROAASSUMED:session</AssumedRoleId>"
      "</AssumedRoleUser></AssumeRoleResult>"
      "<ResponseMetadata><RequestId>synthetic-request-id</RequestId></ResponseMetadata>"
      "</AssumeRoleResponse>";
}
std::string serverless_json() {
  // redshift-serverless GetCredentials returns the expiry/refresh timestamps as
  // numeric epoch seconds (double), not ISO strings.
  return "{\"dbUser\":\"IAM:assumed-user\",\"dbPassword\":\"assumed-password\","
      "\"dbName\":\"fixture_db\",\"expiration\":1900000000.0,"
      "\"nextRefreshTime\":1900000000.0}";
}

std::shared_ptr<Aws::Http::HttpResponse> client_error_response(
    const std::shared_ptr<Aws::Http::HttpRequest>& request, Aws::Client::CoreErrors error) {
  auto safe = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag, request->GetUri(), request->GetMethod());
  safe->SetResponseStreamFactory([]() -> Aws::IOStream* { return Aws::New<RefuseBody>(tag); });
  auto response = Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>(tag, safe);
  response->SetOriginatingRequest(request);
  response->SetResponseCode(Aws::Http::HttpResponseCode::BAD_REQUEST);
  response->SetClientErrorType(error);
  response->SetClientErrorMessage("Synthetic endpoint refused");
  return response;
}

class FakeHttpClient final : public Aws::Http::HttpClient {
 public:
  FakeHttpClient(std::string authority, bool sts) : authority_(std::move(authority)), sts_(sts) {}
  std::shared_ptr<Aws::Http::HttpResponse> MakeRequest(
      const std::shared_ptr<Aws::Http::HttpRequest>& request,
      Aws::Utils::RateLimits::RateLimiterInterface*, Aws::Utils::RateLimits::RateLimiterInterface*) const noexcept override {
    try {
      auto& obs = fake_http_observation();
      ++obs.sends;
      if (!request) return client_error_response(Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag, Aws::Http::URI("https://refused.fixture.invalid/"), Aws::Http::HttpMethod::HTTP_POST), Aws::Client::CoreErrors::VALIDATION);
      const auto& uri = request->GetUri();
      if (uri.GetScheme() != Aws::Http::Scheme::HTTPS) { ++obs.foreign_scheme; return client_error_response(request, Aws::Client::CoreErrors::VALIDATION); }
      if (std::string(uri.GetAuthority().c_str(), uri.GetAuthority().size()) != authority_) { ++obs.foreign_authority; return client_error_response(request, Aws::Client::CoreErrors::VALIDATION); }
      ++obs.approved;
      if (sts_) ++obs.assume_role; else ++obs.get_credentials;
      const auto scenario = static_cast<FakeHttpScenario>(config().scenario.load());
      if (scenario == FakeHttpScenario::Denied) { ++obs.denied; return client_error_response(request, Aws::Client::CoreErrors::ACCESS_DENIED); }
      if (scenario == FakeHttpScenario::CancelDuringTransport) {
        if (config().cancellation) config().cancellation->cancel();
      }
      if (scenario == FakeHttpScenario::LateCompletion) {
        ++obs.late_completion;
        const auto delay = config().delay_ms.load();
        if (delay) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
      }
      auto response = Aws::MakeShared<Aws::Http::Standard::StandardHttpResponse>(tag, request);
      response->SetResponseCode(Aws::Http::HttpResponseCode::OK);
      response->AddHeader("content-type", sts_ ? "text/xml" : "application/x-amz-json-1.1");
      std::string body = sts_ ? sts_xml() : serverless_json();
      response->GetResponseBody().write(body.data(), static_cast<std::streamsize>(body.size()));
      return response;
    } catch (...) { std::terminate(); }
  }
 private:
  std::string authority_;
  bool sts_;
};

class FakeHttpFactory final : public Aws::Http::HttpClientFactory {
 public:
  FakeHttpFactory(std::string authority, bool sts) : authority_(std::move(authority)), sts_(sts) {}
  std::shared_ptr<Aws::Http::HttpClient> CreateHttpClient(const Aws::Client::ClientConfiguration&) const noexcept override {
    try { return Aws::MakeShared<FakeHttpClient>(tag, authority_, sts_); }
    catch (...) { std::terminate(); }
  }
  std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(const Aws::String& uri, Aws::Http::HttpMethod method,
      const Aws::IOStreamFactory& factory) const noexcept override { return CreateHttpRequest(Aws::Http::URI(uri), method, factory); }
  std::shared_ptr<Aws::Http::HttpRequest> CreateHttpRequest(const Aws::Http::URI& uri, Aws::Http::HttpMethod method,
      const Aws::IOStreamFactory& factory) const noexcept override {
    try {
      auto request = Aws::MakeShared<Aws::Http::Standard::StandardHttpRequest>(tag, uri, method);
      request->SetResponseStreamFactory(factory);
      return request;
    } catch (...) { std::terminate(); }
  }
 private:
  std::string authority_;
  bool sts_;
};
} // namespace

FakeHttpObservation& fake_http_observation() noexcept {
  static FakeHttpObservation value;
  return value;
}
bool fake_http_enabled() noexcept {
  const char* marker = ::getenv("ODBCPP_AUTH_SDK_OFFLINE_FIXTURE");
  return marker && std::string_view(marker) == "synthetic-fake-http-only";
}
void fake_http_configure(FakeHttpScenario scenario, unsigned delay_ms,
    std::shared_ptr<provisioned_native::WorkerCancellation> cancel) {
  config().scenario.store(static_cast<unsigned>(scenario));
  config().delay_ms.store(delay_ms);
  config().cancellation = std::move(cancel);
}
std::shared_ptr<Aws::Http::HttpClientFactory> make_fake_http_factory(std::string expected_authority, bool sts) {
  return Aws::MakeShared<FakeHttpFactory>(tag, std::move(expected_authority), sts);
}
} // namespace rs::core::auth::aws::detail
