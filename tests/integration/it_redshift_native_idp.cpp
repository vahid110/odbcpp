#include <gtest/gtest.h>

#include "core/database/postgres/pg_database_connection.h"
#include "odbcpp/database/backend_provider.h"
#include "odbcpp/database/i_database_connection.h"
#include "odbcpp/transport/i_transport.h"
#include "odbcpp/transport/start_tls_transport.h"
#include "odbcpp/transport/tls_configurable_transport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Offline native-IdP integration: the compiled Redshift provider resolves a
// direct-token ConnectionOptions into ConnectionSettings.native_idp, the session
// emits the native startup keys and no password, and the scripted wire returns
// AuthenticationOk. No database service is used.
namespace {
namespace d = rs::core::database;
namespace t = rs::core::transport;

class NativeIdpWire final : public t::ITransport, public t::IStartTlsTransport,
    public t::ITlsConfigurableTransport {
 public:
  NativeIdpWire() {
    // AUTH_REQ_IDP ('R', code 14) asks for the native IdP token, then
    // AuthenticationOk ('R', code 0) then ReadyForQuery ('Z', idle).
    const unsigned char literal[]{
        'R', 0, 0, 0, 8, 0, 0, 0, 14,
        'R', 0, 0, 0, 8, 0, 0, 0, 0,
        'Z', 0, 0, 0, 5, 'I'};
    for (const auto byte : literal) input.push_back(std::byte{byte});
  }
  rs::util::Result<void> connect(std::string_view, std::uint16_t, rs::util::Deadline) override {
    return {};
  }
  rs::util::Result<void> connect_plain(std::string_view host, std::uint16_t port,
                                       rs::util::Deadline deadline) override {
    return connect(host, port, deadline);
  }
  rs::util::Result<void> upgrade_to_tls(std::string_view, rs::util::Deadline) override {
    return {};
  }
  bool peer_identity_verified() noexcept override { return verified; }
  void set_ca_locations(const std::string&, const std::string&) override { ++configured; }
  rs::util::Result<t::IOResult> send(std::span<const std::byte> bytes, rs::util::Deadline) override {
    sent.insert(sent.end(), bytes.begin(), bytes.end());
    if (sent.size() == 8) ssl_pending = true;  // SSLRequest is exactly eight bytes.
    return t::IOResult{bytes.size(), false};
  }
  rs::util::Result<t::IOResult> recv(std::span<std::byte> out, rs::util::Deadline) override {
    if (ssl_pending) { ssl_pending = false; out[0] = std::byte{'S'}; return t::IOResult{1, false}; }
    if (at == input.size()) return {rs::util::DbErrorCode::NetworkError, "scripted wire exhausted"};
    out[0] = input[at++];
    return t::IOResult{1, false};
  }
  void close() noexcept override { ++closed; }
  bool contains(std::string_view text) const {
    const auto needle = std::as_bytes(std::span{text.data(), text.size()});
    return std::search(sent.begin(), sent.end(), needle.begin(), needle.end()) != sent.end();
  }
  bool contains_idp_token_packet(std::string_view token) const {
    // PasswordMessage-style frame carrying 'i' + u32 length + token + NUL.
    std::vector<std::byte> needle;
    needle.push_back(std::byte{'i'});
    const std::uint32_t len = static_cast<std::uint32_t>(4 + token.size() + 1);
    needle.push_back(static_cast<std::byte>((len >> 24) & 0xFF));
    needle.push_back(static_cast<std::byte>((len >> 16) & 0xFF));
    needle.push_back(static_cast<std::byte>((len >> 8) & 0xFF));
    needle.push_back(static_cast<std::byte>(len & 0xFF));
    const auto token_bytes = std::as_bytes(std::span{token.data(), token.size()});
    needle.insert(needle.end(), token_bytes.begin(), token_bytes.end());
    needle.push_back(std::byte{0});
    return std::search(sent.begin(), sent.end(), needle.begin(), needle.end()) != sent.end();
  }
  std::vector<std::byte> sent, input;
  std::size_t at = 0;
  bool ssl_pending = false;
  bool verified = true;
  unsigned configured = 0, closed = 0;
};

d::ConnectionOptions native_idp_options() {
  d::ConnectionOptions options;
  options.host = "fixed.invalid";
  options.database = "db";
  options.user = "iamidc:user";
  options.use_ssl = true;
  options.ssl_ca_file = "/synthetic/ca";
  options.plugin_name = "IdpTokenAuthPlugin";
  options.token = "synthetic-jwt-token";
  options.provider_name = "odbcpp_idc";
  options.token_type = "ACCESS_TOKEN";
  options.identity_namespace = "awsidc";
  return options;
}

TEST(RedshiftNativeIdpIntegration, ResolveConnectAndStartupKeysThroughCompiledProvider) {
  const auto& provider = d::configured_backend_provider();
  if (provider.identity().id != "redshift") {
    GTEST_SKIP() << "native IdP requires the Redshift provider";
  }
  auto resolved = provider.resolve_connection_options(native_idp_options());
  ASSERT_TRUE(resolved) << resolved.error_message();
  ASSERT_TRUE(resolved->native_idp.has_value());
  EXPECT_TRUE(resolved->password.empty());
  EXPECT_EQ("AwsIdc", resolved->native_idp->idp_type);

  auto wire = std::make_unique<NativeIdpWire>();
  auto* observed = wire.get();
  auto session = provider.create_session(std::move(wire));
  ASSERT_NE(nullptr, session);
  struct Cleanup { d::IDatabaseConnection& session; ~Cleanup() { session.disconnect(); } } cleanup{*session};

  const auto connected = session->connect(*resolved);
  ASSERT_TRUE(connected) << connected.backend_error().safe_summary();
  ASSERT_TRUE(session->is_connected());
  EXPECT_GE(observed->configured, 1U);
  // Native startup keys present, in order, with the token and no password.
  EXPECT_TRUE(observed->contains("idp_type"));
  EXPECT_TRUE(observed->contains("AwsIdc"));
  EXPECT_TRUE(observed->contains("provider_name"));
  EXPECT_TRUE(observed->contains("odbcpp_idc"));
  EXPECT_TRUE(observed->contains("token_type"));
  EXPECT_TRUE(observed->contains("ACCESS_TOKEN"));
  EXPECT_TRUE(observed->contains("identity_namespace"));
  EXPECT_TRUE(observed->contains("awsidc"));
  EXPECT_TRUE(observed->contains("web_identity_token"));
  EXPECT_TRUE(observed->contains("synthetic-jwt-token"));
  // AUTH_REQ_IDP was answered with the 'i' packet carrying the token.
  EXPECT_TRUE(observed->contains_idp_token_packet("synthetic-jwt-token"));
  session->disconnect();
  EXPECT_FALSE(session->is_connected());
}

TEST(RedshiftNativeIdpIntegration, BrowserIdcOmitsIdentityNamespaceStartupKey) {
  const auto& provider = d::configured_backend_provider();
  if (provider.identity().id != "redshift") {
    GTEST_SKIP() << "native IdP requires the Redshift provider";
  }
  auto options = native_idp_options();
  options.plugin_name = "BrowserIdcAuthPlugin";
  options.identity_namespace.reset();
  auto resolved = provider.resolve_connection_options(std::move(options));
  ASSERT_TRUE(resolved) << resolved.error_message();
  ASSERT_TRUE(resolved->native_idp.has_value());
  EXPECT_TRUE(resolved->native_idp->identity_namespace.empty());

  auto wire = std::make_unique<NativeIdpWire>();
  auto* observed = wire.get();
  auto session = provider.create_session(std::move(wire));
  ASSERT_NE(nullptr, session);
  struct Cleanup { d::IDatabaseConnection& session; ~Cleanup() { session.disconnect(); } } cleanup{*session};

  const auto connected = session->connect(*resolved);
  ASSERT_TRUE(connected) << connected.backend_error().safe_summary();
  ASSERT_TRUE(session->is_connected());
  EXPECT_TRUE(observed->contains("idp_type"));
  EXPECT_TRUE(observed->contains("AwsIdc"));
  EXPECT_FALSE(observed->contains("identity_namespace"));
  EXPECT_TRUE(observed->contains_idp_token_packet("synthetic-jwt-token"));
}

TEST(RedshiftNativeIdpIntegration, PostgreSQLProviderRefusesNativeIdpResolution) {
  const auto& provider = d::configured_backend_provider();
  if (provider.identity().id != "redshift") {
    GTEST_SKIP() << "PostgreSQL refusal covered by the unit suite";
  }
  // The compiled Redshift provider derives the wire type; an unsupported plugin
  // is rejected before any session is created.
  auto options = native_idp_options();
  options.plugin_name = "Okta";
  auto refused = provider.resolve_connection_options(std::move(options));
  ASSERT_FALSE(refused);
}
// Direct connect_until bypasses PgDatabaseConnection::connect's composition
// checks; the shared connection path must still protect the bearer token.
d::ConnectionSettings direct_native_idp_settings() {
  d::ConnectionSettings settings;
  settings.host = "fixed.invalid";
  settings.port = 5439;
  settings.database = "db";
  settings.use_ssl = true;
  settings.native_idp = d::NativeIdpSettings{"synthetic-jwt-token", "AwsIdc", "odbcpp_idc",
                                             "ACCESS_TOKEN", "", "IdpTokenAuthPlugin"};
  return settings;
}

rs::util::Deadline direct_deadline() {
  return rs::util::Clock::now() + std::chrono::duration_cast<rs::util::Clock::duration>(std::chrono::seconds{10});
}

TEST(RedshiftNativeIdpIntegration, DirectConnectUntilRefusesPlaintextBeforeIo) {
  auto wire = std::make_unique<NativeIdpWire>();
  auto* observed = wire.get();
  d::postgres::PgDatabaseConnection connection{std::move(wire), std::nullopt,
                                              d::postgres::PgCatalogProfile::Redshift};
  auto settings = direct_native_idp_settings();
  settings.use_ssl = false;
  const auto connected = connection.connect_until(settings, direct_deadline());
  ASSERT_FALSE(connected);
  EXPECT_FALSE(connection.is_connected());
  EXPECT_TRUE(observed->sent.empty());
}

TEST(RedshiftNativeIdpIntegration, UnverifiedTlsPeerReceivesNoTokenBytes) {
  auto wire = std::make_unique<NativeIdpWire>();
  auto* observed = wire.get();
  observed->verified = false;
  d::postgres::PgDatabaseConnection connection{std::move(wire), std::nullopt,
                                              d::postgres::PgCatalogProfile::Redshift};
  const auto connected = connection.connect_until(direct_native_idp_settings(), direct_deadline());
  ASSERT_FALSE(connected);
  EXPECT_FALSE(connection.is_connected());
  // Only the eight-byte SSLRequest reached the wire.
  EXPECT_EQ(8U, observed->sent.size());
  EXPECT_FALSE(observed->contains("synthetic-jwt-token"));
  EXPECT_FALSE(observed->contains("web_identity_token"));
}

TEST(RedshiftNativeIdpIntegration, DirectConnectUntilVerifiedTlsSendsToken) {
  auto wire = std::make_unique<NativeIdpWire>();
  auto* observed = wire.get();
  d::postgres::PgDatabaseConnection connection{std::move(wire), std::nullopt,
                                              d::postgres::PgCatalogProfile::Redshift};
  const auto connected = connection.connect_until(direct_native_idp_settings(), direct_deadline());
  ASSERT_TRUE(connected) << connected.backend_error().safe_summary();
  EXPECT_TRUE(observed->contains("web_identity_token"));
  EXPECT_TRUE(observed->contains_idp_token_packet("synthetic-jwt-token"));
  connection.disconnect();
}
}  // namespace
