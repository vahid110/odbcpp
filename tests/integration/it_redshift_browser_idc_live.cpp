#include <gtest/gtest.h>

#include "odbcpp/auth/aws/browser_idc_owner.h"
#include "odbcpp/database/backend_provider.h"
#include "odbcpp/database/i_database_connection.h"
#include "odbcpp/transport/tls_transport.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Admission-gated live BrowserIdc login. The IdC user signs in in a browser
// (authorization-code + PKCE); the returned subject token drives the native-IdP
// login. Every test SKIPs unless all required inputs are present. No credential
// values are logged.
namespace {
namespace a = rs::core::auth;
namespace bidc = rs::core::auth::aws::browser_idc;
namespace pn = rs::core::auth::aws::provisioned_native;
namespace d = rs::core::database;
namespace t = rs::core::transport;

std::optional<std::string> env_text(const char* name, std::size_t maximum) {
  const char* raw = std::getenv(name);
  if (raw == nullptr) return std::nullopt;
  std::size_t size = 0;
  while (size <= maximum && raw[size] != '\0') ++size;
  if (size == 0 || size > maximum) return std::nullopt;
  const std::string value(raw, size);
  if (value.find('\0') != std::string::npos) return std::nullopt;
  return value;
}

struct Admission {
  std::string issuer_url, region, host, database, user, provider, token_type, namespace_id, ca_file;
  unsigned short listen_port = 7890;
  std::uint16_t port = 5439;
};

std::optional<Admission> admission() {
  auto issuer = env_text("ODBCPP_NATIVE_IDC_ISSUER_URL", 512);
  auto region = env_text("ODBCPP_NATIVE_IDC_REGION", 128);
  auto host = env_text("ODBCPP_NATIVE_IDP_HOST", 1024);
  auto database = env_text("ODBCPP_NATIVE_IDP_DATABASE", 1024);
  auto user = env_text("ODBCPP_NATIVE_IDP_USER", 1024);
  auto provider = env_text("ODBCPP_NATIVE_IDP_PROVIDER", 1024);
  auto token_type = env_text("ODBCPP_NATIVE_IDP_TOKEN_TYPE", 1024);
  auto namespace_id = env_text("ODBCPP_NATIVE_IDP_NAMESPACE", 1024);
  auto ca_file = env_text("ODBCPP_NATIVE_IDP_CA", 4096);
  // The database user is optional: like the AWS drivers, BrowserIdc lets the
  // token carry the identity and omits `user` from the startup message.
  if (!issuer || !region || !host || !database || !provider || !token_type || !ca_file)
    return std::nullopt;
  Admission selected;
  selected.issuer_url = std::move(*issuer); selected.region = std::move(*region);
  selected.host = std::move(*host); selected.database = std::move(*database);
  selected.user = user.value_or(std::string{}); selected.provider = std::move(*provider);
  selected.token_type = std::move(*token_type); selected.namespace_id = namespace_id.value_or(std::string{});
  selected.ca_file = std::move(*ca_file);
  if (const auto listen = env_text("ODBCPP_NATIVE_IDC_LISTEN_PORT", 5)) {
    unsigned value = 0;
    const auto parsed = std::from_chars(listen->data(), listen->data() + listen->size(), value);
    if (parsed.ec != std::errc{} || value == 0 || value > 65535) return std::nullopt;
    selected.listen_port = static_cast<unsigned short>(value);
  }
  if (const auto port = env_text("ODBCPP_NATIVE_IDP_PORT", 5)) {
    unsigned value = 0;
    const auto parsed = std::from_chars(port->data(), port->data() + port->size(), value);
    if (parsed.ec != std::errc{} || value == 0 || value > 65535) return std::nullopt;
    selected.port = static_cast<std::uint16_t>(value);
  }
  return selected;
}

TEST(RedshiftBrowserIdcLive, BrowserSignInThenNativeLoginSelect) {
  const auto& provider = d::configured_backend_provider();
  if (provider.identity().id != "redshift") {
    GTEST_SKIP() << "BrowserIdc login requires the Redshift provider";
  }
  const auto admitted = admission();
  if (!admitted) {
    GTEST_SKIP() << "BrowserIdc live scope is not admitted";
  }

  auto made = bidc::BrowserIdcOwner::create(
      bidc::BrowserIdcTarget{admitted->issuer_url, admitted->region, admitted->listen_port,
          std::chrono::seconds{120}},
      rs::util::Clock::now() + std::chrono::seconds{180}, std::make_shared<pn::WorkerCancellation>());
  ASSERT_TRUE(made.owner) << "BrowserIdc owner create failed";
  auto owner = std::move(made.owner);
  auto acquired = owner->acquire();
  ASSERT_TRUE(acquired) << "BrowserIdc OAuth/PKCE acquisition failed";
  EXPECT_GT(acquired.value().expires_at.microseconds_since_epoch, 0);
  std::string token;
  acquired.value().token.with_bytes([&](std::span<const std::byte> view) {
    token.assign(reinterpret_cast<const char*>(view.data()), view.size());
  });
  ASSERT_FALSE(token.empty());
  owner->close();

  d::ConnectionOptions options;
  options.host = admitted->host; options.port = admitted->port;
  options.database = admitted->database;
  if (!admitted->user.empty()) {
    options.user = admitted->user;
  }
  options.use_ssl = true; options.ssl_ca_file = admitted->ca_file;
  options.plugin_name = "BrowserIdcAuthPlugin"; options.token = std::move(token);
  options.provider_name = admitted->provider; options.token_type = admitted->token_type;
  options.identity_namespace = admitted->namespace_id;
  options.timeout = std::chrono::seconds{20};

  auto resolved = provider.resolve_connection_options(std::move(options));
  ASSERT_TRUE(resolved) << resolved.error_message();
  ASSERT_TRUE(resolved->native_idp.has_value());
  EXPECT_TRUE(resolved->password.empty());

  auto transport = std::make_unique<t::TLSTransport>();
  transport->set_verify(true);
  transport->set_hostname_verification(true);
  auto* peer = transport.get();
  auto session = provider.create_session(std::move(transport));
  ASSERT_NE(nullptr, session);
  struct Cleanup { d::IDatabaseConnection& session; ~Cleanup() { session.disconnect(); } } cleanup{*session};

  const auto connected = session->connect(*resolved);
  ASSERT_TRUE(connected) << connected.backend_error().safe_summary();
  ASSERT_TRUE(session->is_connected());
  ASSERT_TRUE(peer->peer_identity_verified());

  const auto deadline = rs::util::make_deadline(std::chrono::seconds{20});
  auto result = session->execute_query("SELECT 1 AS one", deadline);
  ASSERT_TRUE(result) << result.backend_error().safe_summary();
  ASSERT_EQ(1U, result->rows.size());
  ASSERT_EQ(1U, result->rows[0].size());
  ASSERT_TRUE(result->rows[0][0].has_value());
  EXPECT_EQ(std::optional<std::string>{"1"}, result->rows[0][0]);

  session->disconnect();
  EXPECT_FALSE(session->is_connected());
}
}  // namespace
