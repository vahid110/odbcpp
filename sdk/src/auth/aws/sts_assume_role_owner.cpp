#include "odbcpp/auth/aws/sts_assume_role_owner.h"
#include "odbcpp/auth/aws/provisioned_native_http_ingress.h"
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
std::string role_arn(const StsAssumeRoleTarget& target) {
  auto part = partition_text(target.partition);
  if (!part) throw std::runtime_error("partition refused");
  return "arn:" + std::string(*part) + ":iam::" + target.account + ":role/" + target.role_name;
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
  bool sdk_scope{false}, initialized{false}, consumed{false}, active{false};
  Aws::SDKOptions options;
  std::string staged_directory, staged_config, staged_credentials;
  std::optional<StsAssumeRoleResponseUtc> response;
  State(StsAssumeRoleTarget t, SecretBytes ak, SecretBytes sk, SecretBytes tok,
      rs::util::Deadline d, std::shared_ptr<provisioned_native::WorkerCancellation> c)
      : target(std::move(t)), access_key(std::move(ak)), secret_key(std::move(sk)),
        session_token(std::move(tok)), deadline(d), cancellation(std::move(c)),
        creator(std::this_thread::get_id()) {}
  ~State() {
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
  OwnerOutcome out;
  if (!main_entry()) { out.failure = OwnerFailure::MainEntryRequired; return out; }
  if (!cancel || deadline == rs::util::Deadline::min() || deadline == rs::util::Deadline::max()) {
    out.failure = OwnerFailure::InvalidTarget; return out;
  }
  if (access_key.empty() || secret_key.empty()) { out.failure = OwnerFailure::InvalidSource; return out; }
  if (!partition_text(target.partition)) { out.failure = OwnerFailure::InvalidTarget; return out; }
  try {
    if (!provisioned_native::detail::take_sdk_scope()) { out.failure = OwnerFailure::Unavailable; return out; }
    auto state = std::make_unique<State>(std::move(target), std::move(access_key), std::move(secret_key),
        std::move(session_token), deadline, std::move(cancel));
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
                            "AWS_PROFILE", "AWS_DEFAULT_PROFILE"}) {
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
    Aws::InitAPI(state->options);
    state->initialized = true;
    out.owner.reset(new StsAssumeRoleOwner(std::move(state)));
    return out;
  } catch (const std::bad_alloc&) { out.failure = OwnerFailure::LocalFailure; return out; }
  catch (...) { out.failure = OwnerFailure::LocalFailure; return out; }
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
    config.requestTimeoutMs = 1000;
    config.connectTimeoutMs = 1000;
    Aws::STS::STSClient client(provider, config);
    Aws::STS::Model::AssumeRoleRequest request;
    request.SetRoleArn(arn.c_str());
    request.SetRoleSessionName(s.target.role_session_name.c_str());
    request.SetDurationSeconds(s.target.duration_seconds);
    const auto result = client.AssumeRole(request);
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

} // namespace rs::core::auth::aws::sts
