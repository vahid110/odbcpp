#include "odbcpp/auth/aws/sts_assume_role_owner.h"
#include "odbcpp/auth/aws/provisioned_native_http_ingress.h"
#include "odbcpp/auth/aws/sts_serverless_fake_http.h"
#include <aws/core/Globals.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/crt/io/HostResolver.h>
#include <aws/sts/STSClient.h>
#include <aws/sts/model/AssumeRoleRequest.h>
#include <aws/sts/model/AssumeRoleResult.h>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <limits>
#include <new>

namespace rs::core::auth::aws::sts {
namespace {
constexpr const char* tag = "StsAssumeRoleOwner";
bool main_entry() noexcept {
#if defined(__APPLE__)
  return ::pthread_main_np() != 0;
#else
  return false;
#endif
}
std::optional<std::string_view> partition_text(StsAssumeRolePartition value) noexcept {
  switch (value) {
    case StsAssumeRolePartition::Aws: return "aws";
    case StsAssumeRolePartition::AwsCn: return "aws-cn";
    case StsAssumeRolePartition::AwsUsGov: return "aws-us-gov";
  }
  return std::nullopt;
}
bool lower_or_digit(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool region_valid(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128 || value.front() == '-' || value.back() == '-') return false;
  char previous = 0;
  for (char c : value) {
    if ((!lower_or_digit(c) && c != '-') || (c == '-' && previous == '-')) return false;
    previous = c;
  }
  return true;
}
std::string role_arn(const StsAssumeRoleTarget& target) {
  auto part = partition_text(target.partition);
  if (!part) throw std::runtime_error("partition refused");
  return "arn:" + std::string(*part) + ":iam::" + target.account + ":role/" + target.role_name;
}
// Approved HTTPS service endpoint, derived from the validated partition/region.
// This is the only target the owner may send to; it is pinned via endpointOverride
// so SDK endpoint environment overrides cannot redirect the signed request.
std::string sts_endpoint(StsAssumeRolePartition partition, const std::string& region) {
  const char* domain = "amazonaws.com";
  if (partition == StsAssumeRolePartition::AwsCn) domain = "amazonaws.com.cn";
  return "https://sts." + region + "." + domain;
}
std::string sdk_string(const SecretBytes& bytes) {
  std::string out;
  bytes.with_bytes([&](std::span<const std::byte> view) {
    out.assign(reinterpret_cast<const char*>(view.data()), view.size());
  });
  return out;
}
bool empty_file(const std::string& path) {
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) return false;
  if (::close(fd)) return false;
  return true;
}
} // namespace

struct StsAssumeRoleOwner::State {
  StsAssumeRoleTarget target;
  SecretBytes access_key, secret_key, session_token;
  rs::util::Deadline deadline;
  std::shared_ptr<provisioned_native::WorkerCancellation> cancellation;
  std::thread::id creator;
  bool sdk_scope{false}, initialized{false}, entering{false}, consumed{false}, active{false};
  Aws::SDKOptions options;
  std::string staged_directory, staged_config, staged_credentials;
  std::optional<StsAssumeRoleResponseUtc> response;
  State(StsAssumeRoleTarget t, SecretBytes ak, SecretBytes sk, SecretBytes tok,
      rs::util::Deadline d, std::shared_ptr<provisioned_native::WorkerCancellation> c)
      : target(std::move(t)), access_key(std::move(ak)), secret_key(std::move(sk)),
        session_token(std::move(tok)), deadline(d), cancellation(std::move(c)),
        creator(std::this_thread::get_id()) {}
  ~State() {
    // A partially-entered InitAPI has no proven rollback; never claim cleanup.
    if (entering) std::terminate();
    if (initialized) Aws::ShutdownAPI(options);
    if (sdk_scope) provisioned_native::detail::release_sdk_scope();
    if (!staged_config.empty()) ::unlink(staged_config.c_str());
    if (!staged_credentials.empty()) ::unlink(staged_credentials.c_str());
    if (!staged_directory.empty()) ::rmdir(staged_directory.c_str());
    if (active) std::terminate();
  }
};

StsAssumeRoleOwner::StsAssumeRoleOwner(std::unique_ptr<State> state) : state_(std::move(state)) {}
StsAssumeRoleOwner::~StsAssumeRoleOwner() {
  if (state_ && state_->creator != std::this_thread::get_id()) std::terminate();
}
bool StsAssumeRoleOwner::close() noexcept {
  if (!state_ || state_->active) return false;
  state_.reset();
  return true;
}

OwnerOutcome StsAssumeRoleOwner::create(StsAssumeRoleTarget target, SecretBytes access_key, SecretBytes secret_key,
    SecretBytes session_token, rs::util::Deadline deadline, std::shared_ptr<provisioned_native::WorkerCancellation> cancel) {
  return create_impl(std::move(target), std::move(access_key), std::move(secret_key), std::move(session_token),
      deadline, std::move(cancel), OwnerInitFixture::None);
}

OwnerOutcome StsAssumeRoleOwner::create_impl(StsAssumeRoleTarget target, SecretBytes access_key, SecretBytes secret_key,
    SecretBytes session_token, rs::util::Deadline deadline, std::shared_ptr<provisioned_native::WorkerCancellation> cancel,
    OwnerInitFixture injected) {
  OwnerOutcome out;
  if (!main_entry()) { out.failure = OwnerFailure::MainEntryRequired; return out; }
  if (!cancel || deadline == rs::util::Deadline::min() || deadline == rs::util::Deadline::max()) {
    out.failure = OwnerFailure::InvalidTarget; return out;
  }
  // R3: reject an already-expired finite deadline before any initialization.
  if (rs::util::remaining(deadline) == std::chrono::milliseconds::zero()) {
    out.failure = OwnerFailure::DeadlineElapsed; return out;
  }
  if (access_key.empty() || secret_key.empty()) { out.failure = OwnerFailure::InvalidSource; return out; }
  if (!partition_text(target.partition) || !region_valid(target.region)) { out.failure = OwnerFailure::InvalidTarget; return out; }
  std::unique_ptr<State> state;
  try {
    // R5: allocate State before taking the process-wide SDK scope so an
    // allocation failure cannot leak the occupied flag.
    if (injected == OwnerInitFixture::AllocationFailure) throw std::bad_alloc{};
    state = std::make_unique<State>(std::move(target), std::move(access_key), std::move(secret_key),
        std::move(session_token), deadline, std::move(cancel));
    if (!provisioned_native::detail::take_sdk_scope()) { out.failure = OwnerFailure::Unavailable; return out; }
    state->sdk_scope = true;
    // Standalone entry precondition; never adopt unrelated SDK users.
    if (Aws::GetDefaultClientBootstrap() != nullptr) { out.failure = OwnerFailure::Unavailable; return out; }
    char pattern[] = "/tmp/odbcpp_native_sts_XXXXXX";
    char* created = ::mkdtemp(pattern);
    if (!created) { out.failure = OwnerFailure::LocalFailure; return out; }
    state->staged_directory = created;
    state->staged_config = state->staged_directory + "/config";
    state->staged_credentials = state->staged_directory + "/credentials";
    if (!empty_file(state->staged_config) || !empty_file(state->staged_credentials)) {
      out.failure = OwnerFailure::LocalFailure; return out;
    }
    if (::setenv("AWS_CONFIG_FILE", state->staged_config.c_str(), 1) ||
        ::setenv("AWS_SHARED_CREDENTIALS_FILE", state->staged_credentials.c_str(), 1) ||
        ::setenv("AWS_EC2_METADATA_DISABLED", "true", 1)) { out.failure = OwnerFailure::LocalFailure; return out; }
    for (const char* key : {"HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy",
                            "AWS_PROFILE", "AWS_DEFAULT_PROFILE", "AWS_WEB_IDENTITY_TOKEN_FILE", "AWS_ROLE_ARN",
                            "AWS_ENDPOINT_URL", "AWS_ENDPOINT_URL_STS"}) {
      if (::unsetenv(key)) { out.failure = OwnerFailure::LocalFailure; return out; }
    }
    state->options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Off;
    auto* ptr = state.get();
    state->options.ioOptions.clientBootstrap_create_fn = [ptr]() noexcept {
      try {
        if (ptr->creator != std::this_thread::get_id() || !main_entry()) std::terminate();
        Aws::Crt::Io::EventLoopGroup group;
        Aws::Crt::Io::DefaultHostResolver resolver(group, 8, 30);
        return Aws::MakeShared<Aws::Crt::Io::ClientBootstrap>(tag, group, resolver);
      } catch (...) { std::terminate(); }
    };
    // Offline-only synthetic HTTP (marker gated); production keeps the SDK CRT
    // HTTP client and the pinned endpointOverride above.
    if (detail::fake_http_enabled()) {
      const std::string endpoint = sts_endpoint(state->target.partition, state->target.region);
      state->options.httpOptions.httpClientFactory_create_fn = [authority = endpoint.substr(8)]() noexcept {
        try { return detail::make_fake_http_factory(authority, true); } catch (...) { std::terminate(); }
      };
    }
    // R4: mark entry before InitAPI; a throw after this point has no proven
    // rollback and must terminate rather than claim a released state.
    state->entering = true;
    if (injected == OwnerInitFixture::AfterEntry) throw std::bad_alloc{};
    Aws::InitAPI(state->options);
    state->entering = false;
    state->initialized = true;
    out.owner.reset(new StsAssumeRoleOwner(std::move(state)));
    return out;
  } catch (const std::bad_alloc&) { if (state && state->entering) std::terminate(); out.failure = OwnerFailure::LocalFailure; return out; }
  catch (...) { if (state && state->entering) std::terminate(); out.failure = OwnerFailure::LocalFailure; return out; }
}

StsOutcome<StsAssumeRoleResponseUtc> StsAssumeRoleOwner::acquire() {
  if (!state_) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
  auto& s = *state_;
  if (s.creator != std::this_thread::get_id()) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
  if (s.active || s.consumed) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
  s.active = true; s.consumed = true;
  struct Guard { bool& active; ~Guard() { active = false; } } guard{s.active};
  try {
    if (s.cancellation && s.cancellation->cancelled()) return StsAssumeRoleError{StsAssumeRoleFailure::Cancelled};
    // R3: refuse a deadline that expired between create and acquire (zero sends).
    if (rs::util::remaining(s.deadline) == std::chrono::milliseconds::zero())
      return StsAssumeRoleError{StsAssumeRoleFailure::DeadlineElapsed};
    const std::string arn = role_arn(s.target);
    const std::string ak = sdk_string(s.access_key);
    const std::string sk = sdk_string(s.secret_key);
    const std::string tok = sdk_string(s.session_token);
    auto provider = Aws::MakeShared<Aws::Auth::SimpleAWSCredentialsProvider>(tag, ak.c_str(), sk.c_str(), tok.c_str());
    Aws::Client::ClientConfigurationInitValues init;
    init.shouldDisableIMDS = true;
    Aws::STS::STSClientConfiguration config(init);
    config.disableIMDS = true;
    config.region = s.target.region.c_str();
    config.scheme = Aws::Http::Scheme::HTTPS;
    config.verifySSL = true;
    config.proxyHost.clear();
    config.proxyUserName.clear();
    config.proxyPassword.clear();
    config.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(tag, 0, 0);
    // R2: pin the approved endpoint; SDK endpoint env overrides are excluded.
    config.endpointOverride = sts_endpoint(s.target.partition, s.target.region).c_str();
    // R3: remaining-budget transport controls under the original deadline.
    const auto budget = rs::util::remaining(s.deadline);
    if (budget == std::chrono::milliseconds::zero())
      return StsAssumeRoleError{StsAssumeRoleFailure::DeadlineElapsed};
    config.requestTimeoutMs = static_cast<long>(budget.count());
    config.connectTimeoutMs = static_cast<long>(budget.count());
    Aws::STS::STSClient client(provider, config);
    Aws::STS::Model::AssumeRoleRequest request;
    request.SetRoleArn(arn.c_str());
    request.SetRoleSessionName(s.target.role_session_name.c_str());
    request.SetDurationSeconds(s.target.duration_seconds);
    // R3: attach the original deadline/cancellation to the SDK continuation so
    // the synchronous call is observed during transport, not only before/after.
    request.SetContinueRequestHandler([cancel = s.cancellation, deadline = s.deadline](const Aws::Http::HttpRequest*) {
      return (!cancel || !cancel->cancelled()) && rs::util::remaining(deadline) > std::chrono::milliseconds::zero();
    });
    // R3: recheck immediately before invocation; reject a budget consumed by
    // source/provider/config preparation rather than dispatching with zero.
    if (rs::util::remaining(s.deadline) == std::chrono::milliseconds::zero())
      return StsAssumeRoleError{StsAssumeRoleFailure::DeadlineElapsed};
    const auto result = client.AssumeRole(request);
    // R3: refuse a late successful completion under the same original deadline.
    if (rs::util::remaining(s.deadline) == std::chrono::milliseconds::zero())
      return StsAssumeRoleError{StsAssumeRoleFailure::DeadlineElapsed};
    if (s.cancellation && s.cancellation->cancelled()) return StsAssumeRoleError{StsAssumeRoleFailure::Cancelled};
    if (!result.IsSuccess()) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    const auto& creds = result.GetResult().GetCredentials();
    const auto millis = creds.GetExpiration().Millis();
    if (millis <= 0 || millis > (std::numeric_limits<std::int64_t>::max)() / 1000)
      return StsAssumeRoleError{StsAssumeRoleFailure::ExpiryRange};
    auto make_secret = [](const Aws::String& value) -> SecretBytes {
      auto made = SecretBytes::create(std::as_bytes(std::span{value.data(), value.size()}));
      if (!made) throw std::bad_alloc();
      return std::move(made).value();
    };
    SecretBytes access_key_id = make_secret(creds.GetAccessKeyId());
    SecretBytes secret_access_key = make_secret(creds.GetSecretAccessKey());
    SecretBytes session_token = make_secret(creds.GetSessionToken());
    if (access_key_id.empty() || secret_access_key.empty() || session_token.empty())
      return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    std::string assumed_role_arn = result.GetResult().GetAssumedRoleUser().GetArn();
    if (s.cancellation && s.cancellation->cancelled()) return StsAssumeRoleError{StsAssumeRoleFailure::Cancelled};
    return StsAssumeRoleResponseUtc{std::move(access_key_id), std::move(secret_access_key), std::move(session_token),
        UtcInstant{static_cast<std::int64_t>(millis) * 1000}, std::move(assumed_role_arn)};
  } catch (const std::bad_alloc&) { return StsAssumeRoleError{StsAssumeRoleFailure::AllocationFailed}; }
  catch (...) { return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse}; }
}

OwnerOutcome StsOwnerFixture::create_init_failure(OwnerInitFixture fault) {
  OwnerOutcome out;
  const char* marker = ::getenv("ODBCPP_AUTH_SDK_OFFLINE_FIXTURE");
  if (!marker || std::string_view(marker) != "synthetic-fake-http-only" ||
      static_cast<unsigned>(fault) > static_cast<unsigned>(OwnerInitFixture::AfterEntry)) {
    out.failure = OwnerFailure::Unavailable; return out;
  }
  auto secret = [](std::string_view text) {
    auto made = SecretBytes::create(std::as_bytes(std::span{text.data(), text.size()}));
    if (!made) std::terminate();
    return std::move(made).value();
  };
  StsAssumeRoleTarget target{StsAssumeRolePartition::Aws, "123456789012", "eu-north-1", "MyRole", "session", 900};
  return StsAssumeRoleOwner::create_impl(std::move(target), secret("synthetic-access-key"),
      secret("synthetic-secret-key"), secret(""), rs::util::Clock::now() + std::chrono::seconds{20},
      std::make_shared<provisioned_native::WorkerCancellation>(), fault);
}

} // namespace rs::core::auth::aws::sts
