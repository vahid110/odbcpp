#include <gtest/gtest.h>

#include "core/database/backend_provider.h"
#include "core/database/database_factory.h"
#include "core/database/postgres/pg_backend_provider.h"

#include <chrono>
#include <memory>

namespace {
using namespace rs::core::database;

TEST(BackendProviderTest, CompiledProductOwnsIdentityAndDefaults) {
  const auto& provider = configured_backend_provider();
  const auto& identity = provider.identity();
  const auto& defaults = provider.connection_defaults();

  EXPECT_EQ("localhost", defaults.host);
  EXPECT_EQ(5432, defaults.port);
  ASSERT_TRUE(defaults.database.has_value());
  EXPECT_EQ("postgres", *defaults.database);
  EXPECT_TRUE(defaults.use_ssl);

  if (DatabaseFactory::get_compiled_database_type() == DatabaseType::Redshift) {
    EXPECT_EQ("redshift", identity.id);
    EXPECT_EQ("Amazon Redshift", identity.display_name);
    EXPECT_EQ("ODBCPP Redshift", identity.driver_name);
  } else {
    EXPECT_EQ("postgresql", identity.id);
    EXPECT_EQ("PostgreSQL", identity.display_name);
    EXPECT_EQ("ODBCPP PostgreSQL", identity.driver_name);
  }
}

TEST(BackendProviderTest, StaticProfileMatchesNewSessionWithoutConnecting) {
  const auto& provider = configured_backend_provider();
  auto session = provider.create_session(nullptr);
  ASSERT_NE(nullptr, session);
  EXPECT_FALSE(session->is_connected());

  const auto provider_capabilities = provider.capabilities();
  const auto session_capabilities = session->capabilities();
  EXPECT_EQ(provider_capabilities.dbms_name, session_capabilities.dbms_name);
  EXPECT_EQ(provider_capabilities.identifier_quote,
            session_capabilities.identifier_quote);
  EXPECT_EQ(provider_capabilities.max_identifier_length,
            session_capabilities.max_identifier_length);
  EXPECT_EQ(provider.transaction_capabilities().supported,
            session->transaction_capabilities().supported);
  ASSERT_FALSE(provider.type_catalog().empty());
  ASSERT_EQ(provider.type_catalog().size(), session->type_catalog().size());
  EXPECT_EQ(provider.type_catalog().front().name,
            session->type_catalog().front().name);
}

TEST(BackendProviderTest, PostgreSqlFamilyProfilesStayIndependent) {
  postgres::PgBackendProvider postgresql{
      BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"pg-host", 5432, "postgres", true}};
  postgres::PgBackendProvider redshift{
      BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"rs-host", 5439, std::nullopt, true}};

  EXPECT_EQ("PostgreSQL", postgresql.capabilities().dbms_name);
  EXPECT_EQ("Amazon Redshift", redshift.capabilities().dbms_name);
  EXPECT_EQ("pg-host", postgresql.connection_defaults().host);
  EXPECT_EQ(5439, redshift.connection_defaults().port);
  EXPECT_EQ("PostgreSQL",
            postgresql.create_session(nullptr)->capabilities().dbms_name);
  EXPECT_EQ("Amazon Redshift",
            redshift.create_session(nullptr)->capabilities().dbms_name);
}

TEST(BackendProviderTest, ResolvesDefaultsOverridesAndTlsPolicy) {
  postgres::PgBackendProvider provider{
      BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"default-host", 5432, "default-db", true}};

  auto defaults = provider.resolve_connection_options({});
  ASSERT_TRUE(defaults.has_value());
  EXPECT_EQ("default-host", defaults->host);
  EXPECT_EQ(5432, defaults->port);
  EXPECT_EQ("default-db", defaults->database);
  EXPECT_TRUE(defaults->use_ssl);

  ConnectionOptions overrides;
  overrides.host = "chosen-host";
  overrides.port = 15432;
  overrides.database = "chosen-db";
  overrides.user = "alice";
  overrides.password = "secret";
  overrides.use_ssl = true;
  overrides.ssl_ca_file = "/ca.pem";
  overrides.timeout = std::chrono::milliseconds{321};
  auto selected = provider.resolve_connection_options(std::move(overrides));
  ASSERT_TRUE(selected.has_value());
  EXPECT_EQ("chosen-host", selected->host);
  EXPECT_EQ(15432, selected->port);
  EXPECT_EQ("chosen-db", selected->database);
  EXPECT_EQ("alice", selected->user);
  EXPECT_EQ("secret", selected->password);
  EXPECT_EQ("/ca.pem", selected->ssl_ca_file);
  EXPECT_EQ(std::chrono::milliseconds{321}, selected->timeout);

  ConnectionOptions ambiguous;
  ambiguous.ssl_ca_file = "/ca.pem";
  ambiguous.ssl_ca_dir = "/certs";
  EXPECT_TRUE(provider.resolve_connection_options(std::move(ambiguous)).has_error());

  ConnectionOptions plaintext;
  plaintext.use_ssl = false;
  plaintext.ssl_ca_file = "/ca.pem";
  EXPECT_TRUE(provider.resolve_connection_options(std::move(plaintext)).has_error());
}

TEST(BackendProviderTest, SessionOwnsProductIdentityAfterProviderDestruction) {
  std::unique_ptr<IDatabaseConnection> session;
  {
    std::string display = "Temporary PostgreSQL";
    postgres::PgBackendProvider provider{
        BackendIdentity{"temporary", display, "Temporary Driver"},
        BackendConnectionDefaults{"localhost", 5432, "postgres", true}};
    session = provider.create_session(nullptr);
    display.assign("changed");
  }
  ASSERT_NE(nullptr, session);
  EXPECT_EQ("Temporary PostgreSQL", session->capabilities().dbms_name);
}

}  // namespace

TEST(BackendProviderTest, ResolvesResourceProfilesAndRejectsUnsafeCeilings) {
  using namespace rs::core::database;
  const auto& provider = configured_backend_provider();
  ConnectionOptions options;
  options.input_limits.max_sql_bytes = 32;
  options.response_limits.max_wire_bytes = 4096;
  options.startup_response_limits.max_messages = 7;
  options.result_limits.max_metadata_entries = 11;
  const auto resolved = provider.resolve_connection_options(options);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved->input_limits.max_sql_bytes, 32u);
  EXPECT_EQ(resolved->response_limits.max_wire_bytes, 4096u);
  EXPECT_EQ(resolved->startup_response_limits.max_messages, 7u);
  EXPECT_EQ(resolved->result_limits.max_metadata_entries, 11u);
  options.input_limits.max_parameters = 65536;
  EXPECT_FALSE(provider.resolve_connection_options(options));
  options.input_limits.max_parameters = 0;
  options.response_limits.max_wire_bytes = 4;
  EXPECT_FALSE(provider.resolve_connection_options(options));
}
