#include "odbcpp/auth/aws/browser_idc_owner.h"
#include "odbcpp/auth/aws/provisioned_native_http_ingress.h"
#include "odbcpp/auth/aws/sts_serverless_fake_http.h"
#include <aws/core/Globals.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/crt/io/HostResolver.h>
#include <aws/sso-oidc/SSOOIDCClient.h>
#include <aws/sso-oidc/model/CreateTokenRequest.h>
#include <aws/sso-oidc/model/CreateTokenResult.h>
#include <aws/sso-oidc/model/RegisterClientRequest.h>
#include <aws/sso-oidc/model/RegisterClientResult.h>
#include <openssl/sha.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <thread>
#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CFURL.h>
#endif

namespace rs::core::auth::aws::browser_idc {
namespace {
constexpr const char* tag = "BrowserIdcOwner";
constexpr const char* scope = "redshift:connect";
constexpr const char* redirect_prefix = "http://127.0.0.1:";
constexpr const char* client_type = "public";
constexpr const char* grant_type = "authorization_code";
constexpr const char* challenge_method = "S256";
constexpr const char* base64url_chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
bool main_entry() noexcept {
#if defined(__APPLE__)
  return ::pthread_main_np() != 0;
#else
  return false;
#endif
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
bool issuer_valid(std::string_view value) noexcept {
  return value.size() > 16 && value.size() <= 512 &&
      value.rfind("https://", 0) == 0 && value.find('\0') == std::string_view::npos;
}
// Approved OIDC host, derived from the validated region; pinned for the SDK.
std::string oidc_host(const std::string& region) {
  std::string lower(region);
  for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  const std::string domain = lower.rfind("cn-", 0) == 0 ? "amazonaws.com.cn" : "amazonaws.com";
  return "oidc." + lower + "." + domain;
}
std::string oidc_endpoint(const std::string& region) {
  return "https://" + oidc_host(region);
}
std::string base64url(const unsigned char* data, std::size_t size) {
  static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  out.reserve(((size + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 2 < size) {
    const unsigned n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
    out.push_back(table[(n >> 6) & 63]);
    out.push_back(table[n & 63]);
    i += 3;
  }
  if (i + 1 == size) {
    const unsigned n = data[i] << 16;
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
  } else if (i + 2 == size) {
    const unsigned n = (data[i] << 16) | (data[i + 1] << 8);
    out.push_back(table[(n >> 18) & 63]);
    out.push_back(table[(n >> 12) & 63]);
    out.push_back(table[(n >> 6) & 63]);
  }
  return out;  // PKCE/OIDC base64url is unpadded.
}
std::string random_base64url(std::size_t length) {
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<std::size_t> dist(0, 63);
  std::string out;
  out.reserve(length);
  for (std::size_t i = 0; i < length; ++i) out.push_back(base64url_chars[dist(gen)]);
  return out;
}
std::string sha256_base64url(std::string_view input) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), hash);
  return base64url(hash, SHA256_DIGEST_LENGTH);
}
std::string url_encode(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      static constexpr char hex[] = "0123456789ABCDEF";
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    }
  }
  return out;
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

namespace {
// Bind 127.0.0.1:<port> and listen. Returns the socket fd or -1.
int loopback_listen(unsigned short port) {
  const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) return -1;
  int reuse = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(listener);
    return -1;
  }
  if (::listen(listener, 1) != 0) {
    ::close(listener);
    return -1;
  }
  return listener;
}
// Accept one loopback connection, read the HTTP GET, extract the query, respond
// with a fixed HTML page. Returns the query string (after '?') or empty.
std::string loopback_accept(int listener, rs::util::Deadline deadline,
    const std::shared_ptr<provisioned_native::WorkerCancellation>& cancel, bool& ok) {
  ok = false;
  struct CloseGuard { int fd; ~CloseGuard() { if (fd >= 0) ::close(fd); } } guard{listener};
  int client = -1;
  for (;;) {
    if (cancel && cancel->cancelled()) return {};
    if (rs::util::remaining(deadline) == std::chrono::milliseconds::zero()) return {};
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(listener, &readfds);
    timeval tv{1, 0};
    const int ready = ::select(listener + 1, &readfds, nullptr, nullptr, &tv);
    if (ready < 0) return {};
    if (ready == 0) continue;
    client = ::accept(listener, nullptr, nullptr);
    if (client >= 0) break;
  }
  struct ClientGuard { int fd; ~ClientGuard() { if (fd >= 0) ::close(fd); } } cguard{client};
  char buffer[8192];
  const ssize_t read = ::recv(client, buffer, sizeof(buffer) - 1, 0);
  if (read <= 0) return {};
  buffer[read] = '\0';
  const std::string request(buffer, static_cast<std::size_t>(read));
  const std::size_t get = request.find("GET ");
  const std::size_t path_end = request.find(" HTTP/", get);
  std::string query;
  if (get != std::string::npos && path_end != std::string::npos) {
    const std::string target_path = request.substr(get + 4, path_end - (get + 4));
    const std::size_t q = target_path.find('?');
    if (q != std::string::npos) query = target_path.substr(q + 1);
  }
  const std::string body =
      "<html><body><p>Sign-in complete. You may close this window and return "
      "to the application.</p></body></html>";
  const std::string response =
      "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " +
      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  ::send(client, response.data(), response.size(), 0);
  ok = true;
  return query;
}
std::string query_param(std::string_view query, std::string_view name) {
  std::string needle(name);
  needle.push_back('=');
  std::size_t pos = 0;
  while (pos < query.size()) {
    const std::size_t amp = query.find('&', pos);
    const std::string_view part = query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
    if (part.rfind(needle, 0) == 0) return std::string(part.substr(needle.size()));
    if (amp == std::string_view::npos) break;
    pos = amp + 1;
  }
  return {};
}
void launch_browser(const std::string& url) {
#if defined(__APPLE__)
  CFURLRef cfurl = CFURLCreateWithBytes(nullptr,
      reinterpret_cast<const UInt8*>(url.data()), url.size(), kCFStringEncodingUTF8, nullptr);
  if (cfurl) {
    LSOpenCFURLRef(cfurl, nullptr);
    CFRelease(cfurl);
  }
#else
  const std::string command = "xdg-open " + url + " >/dev/null 2>&1 &";
  std::system(command.c_str());
#endif
}
} // namespace

struct BrowserIdcOwner::State {
  BrowserIdcTarget target;
  rs::util::Deadline deadline;
  std::shared_ptr<provisioned_native::WorkerCancellation> cancellation;
  std::thread::id creator;
  bool sdk_scope{false}, initialized{false}, entering{false}, consumed{false}, active{false};
  Aws::SDKOptions options;
  std::string staged_directory, staged_config, staged_credentials;
  State(BrowserIdcTarget t, rs::util::Deadline d, std::shared_ptr<provisioned_native::WorkerCancellation> c)
      : target(std::move(t)), deadline(d), cancellation(std::move(c)), creator(std::this_thread::get_id()) {}
  ~State() {
    if (entering) std::terminate();
    if (initialized) Aws::ShutdownAPI(options);
    if (sdk_scope) provisioned_native::detail::release_sdk_scope();
    if (!staged_config.empty()) ::unlink(staged_config.c_str());
    if (!staged_credentials.empty()) ::unlink(staged_credentials.c_str());
    if (!staged_directory.empty()) ::rmdir(staged_directory.c_str());
    if (active) std::terminate();
  }
};

BrowserIdcOwner::BrowserIdcOwner(std::unique_ptr<State> state) : state_(std::move(state)) {}
BrowserIdcOwner::~BrowserIdcOwner() {
  if (state_ && state_->creator != std::this_thread::get_id()) std::terminate();
}
bool BrowserIdcOwner::close() noexcept {
  if (!state_ || state_->active) return false;
  state_.reset();
  return true;
}

OwnerOutcome BrowserIdcOwner::create(BrowserIdcTarget target, rs::util::Deadline deadline,
    std::shared_ptr<provisioned_native::WorkerCancellation> cancel) {
  OwnerOutcome out;
  if (!main_entry()) { out.failure = OwnerFailure::MainEntryRequired; return out; }
  if (!cancel || deadline == rs::util::Deadline::min() || deadline == rs::util::Deadline::max()) {
    out.failure = OwnerFailure::InvalidTarget; return out;
  }
  if (rs::util::remaining(deadline) == std::chrono::milliseconds::zero()) {
    out.failure = OwnerFailure::DeadlineElapsed; return out;
  }
  if (!issuer_valid(target.issuer_url) || !region_valid(target.region) ||
      target.listen_port == 0 || target.response_timeout.count() < 10) {
    out.failure = OwnerFailure::InvalidTarget; return out;
  }
  std::unique_ptr<State> state;
  try {
    state = std::make_unique<State>(std::move(target), deadline, std::move(cancel));
    if (!provisioned_native::detail::take_sdk_scope()) { out.failure = OwnerFailure::Unavailable; return out; }
    state->sdk_scope = true;
    if (Aws::GetDefaultClientBootstrap() != nullptr) { out.failure = OwnerFailure::Unavailable; return out; }
    char pattern[] = "/tmp/odbcpp_browser_idc_XXXXXX";
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
                            "AWS_ENDPOINT_URL", "AWS_ENDPOINT_URL_SSO_OIDC"}) {
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
    if (rs::core::auth::aws::detail::fake_http_enabled()) {
      const std::string authority = oidc_host(state->target.region);
      state->options.httpOptions.httpClientFactory_create_fn = [authority]() noexcept {
        try { return rs::core::auth::aws::detail::make_fake_http_factory(authority, false); } catch (...) { std::terminate(); }
      };
    }
    state->entering = true;
    Aws::InitAPI(state->options);
    state->entering = false;
    state->initialized = true;
    out.owner.reset(new BrowserIdcOwner(std::move(state)));
    return out;
  } catch (const std::bad_alloc&) { if (state && state->entering) std::terminate(); out.failure = OwnerFailure::LocalFailure; return out; }
  catch (...) { if (state && state->entering) std::terminate(); out.failure = OwnerFailure::LocalFailure; return out; }
}

FieldOutcome<BrowserIdcToken> BrowserIdcOwner::acquire() {
  if (!state_) return FieldError{FieldFailure::InvalidShape};
  auto& s = *state_;
  if (s.creator != std::this_thread::get_id()) return FieldError{FieldFailure::InvalidShape};
  if (s.active || s.consumed) return FieldError{FieldFailure::InvalidShape};
  s.active = true; s.consumed = true;
  struct Guard { bool& active; ~Guard() { active = false; } } guard{s.active};
  try {
    const auto cancelled = [&] { return s.cancellation && s.cancellation->cancelled(); };
    const auto expired = [&] { return rs::util::remaining(s.deadline) == std::chrono::milliseconds::zero(); };
    if (cancelled()) return FieldError{FieldFailure::Cancelled};
    if (expired()) return FieldError{FieldFailure::Cancelled};

    auto provider = Aws::MakeShared<Aws::Auth::AnonymousAWSCredentialsProvider>(tag);
    Aws::Client::ClientConfigurationInitValues init;
    init.shouldDisableIMDS = true;
    Aws::SSOOIDC::SSOOIDCClientConfiguration config(init);
    config.disableIMDS = true;
    config.region = s.target.region.c_str();
    config.scheme = Aws::Http::Scheme::HTTPS;
    config.verifySSL = true;
    config.proxyHost.clear(); config.proxyUserName.clear(); config.proxyPassword.clear();
    config.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>(tag, 0, 0);
    config.endpointOverride = oidc_endpoint(s.target.region).c_str();
    const auto budget = rs::util::remaining(s.deadline);
    if (budget == std::chrono::milliseconds::zero()) return FieldError{FieldFailure::Cancelled};
    config.requestTimeoutMs = static_cast<long>(budget.count());
    config.connectTimeoutMs = static_cast<long>(budget.count());
    Aws::SSOOIDC::SSOOIDCClient client(provider, config);

    // RegisterClient -> clientId + clientSecret.
    Aws::SSOOIDC::Model::RegisterClientRequest register_request;
    register_request.SetClientName("ODBCPP Redshift driver");
    register_request.SetClientType(client_type);
    register_request.SetScopes(Aws::Vector<Aws::String>{scope});
    register_request.SetIssuerUrl(s.target.issuer_url.c_str());
    register_request.SetRedirectUris(Aws::Vector<Aws::String>{
        std::string(redirect_prefix) + std::to_string(s.target.listen_port)});
    register_request.SetGrantTypes(Aws::Vector<Aws::String>{grant_type});
    register_request.SetContinueRequestHandler([cancel = s.cancellation, deadline = s.deadline](const Aws::Http::HttpRequest*) {
      return (!cancel || !cancel->cancelled()) && rs::util::remaining(deadline) > std::chrono::milliseconds::zero();
    });
    if (cancelled() || expired()) return FieldError{FieldFailure::Cancelled};
    const auto register_result = client.RegisterClient(register_request);
    if (cancelled() || expired()) return FieldError{FieldFailure::Cancelled};
    if (!register_result.IsSuccess()) return FieldError{FieldFailure::InvalidShape};
    const std::string client_id(register_result.GetResult().GetClientId().c_str(),
        register_result.GetResult().GetClientId().size());
    const std::string client_secret(register_result.GetResult().GetClientSecret().c_str(),
        register_result.GetResult().GetClientSecret().size());
    if (client_id.empty() || client_secret.empty()) return FieldError{FieldFailure::InvalidShape};

    const std::string code_verifier = random_base64url(60);
    const std::string code_challenge = sha256_base64url(code_verifier);
    const std::string state = random_base64url(24);
    const std::string redirect_uri = std::string(redirect_prefix) + std::to_string(s.target.listen_port);

    // Authorization code: loopback + browser, or the offline fake seam.
    std::string authorization_code;
    if (rs::core::auth::aws::detail::fake_http_enabled() && !rs::core::auth::aws::detail::fake_browser_auth_code().empty()) {
      authorization_code = rs::core::auth::aws::detail::fake_browser_auth_code();
    } else {
      const std::string authorize_url = "https://" + oidc_host(s.target.region) +
          "/authorize?response_type=code" + "&client_id=" + url_encode(client_id) +
          "&redirect_uri=" + url_encode(redirect_uri) + "&scopes=" + url_encode(scope) +
          "&state=" + url_encode(state) + "&code_challenge=" + url_encode(code_challenge) +
          "&code_challenge_method=" + challenge_method;
      const int listener = loopback_listen(s.target.listen_port);
      if (listener < 0) return FieldError{FieldFailure::InvalidShape};
      launch_browser(authorize_url);
      bool loopback_ok = false;
      const std::string query = loopback_accept(listener, s.deadline, s.cancellation, loopback_ok);
      if (!loopback_ok || cancelled() || expired()) return FieldError{FieldFailure::Cancelled};
      authorization_code = query_param(query, "code");
      const std::string got_state = query_param(query, "state");
      if (authorization_code.empty() || got_state != state) return FieldError{FieldFailure::InvalidShape};
    }

    // CreateToken -> access token + expiry.
    Aws::SSOOIDC::Model::CreateTokenRequest token_request;
    token_request.SetClientId(client_id.c_str());
    token_request.SetClientSecret(client_secret.c_str());
    token_request.SetCode(authorization_code.c_str());
    token_request.SetGrantType(grant_type);
    token_request.SetCodeVerifier(code_verifier.c_str());
    token_request.SetRedirectUri(redirect_uri.c_str());
    token_request.SetContinueRequestHandler([cancel = s.cancellation, deadline = s.deadline](const Aws::Http::HttpRequest*) {
      return (!cancel || !cancel->cancelled()) && rs::util::remaining(deadline) > std::chrono::milliseconds::zero();
    });
    if (cancelled() || expired()) return FieldError{FieldFailure::Cancelled};
    const auto token_result = client.CreateToken(token_request);
    if (cancelled() || expired()) return FieldError{FieldFailure::Cancelled};
    if (!token_result.IsSuccess()) return FieldError{FieldFailure::InvalidShape};
    const Aws::String& access_token = token_result.GetResult().GetAccessToken();
    if (access_token.empty()) return FieldError{FieldFailure::InvalidToken};
    const int expires_in = token_result.GetResult().GetExpiresIn();
    if (expires_in <= 0) return FieldError{FieldFailure::InvalidTimestamp};
    auto token_secret = SecretBytes::create(std::as_bytes(std::span{access_token.data(), access_token.size()}));
    if (!token_secret) return FieldError{FieldFailure::AllocationFailed};
    if (cancelled()) return FieldError{FieldFailure::Cancelled};
    const auto now = std::chrono::system_clock::now();
    const auto expires_at = now + std::chrono::seconds(expires_in);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        expires_at.time_since_epoch()).count();
    return BrowserIdcToken{std::move(token_secret).value(), UtcInstant{micros}};
  } catch (const std::bad_alloc&) { return FieldError{FieldFailure::AllocationFailed}; }
  catch (...) { return FieldError{FieldFailure::InvalidShape}; }
}

} // namespace rs::core::auth::aws::browser_idc
