#include <gtest/gtest.h>

#include "core/database/backend_provider.h"
#include "core/database/database_factory.h"
#include "core/database/postgres/pg_backend_provider.h"
#include "core/database/postgres/pg_database_connection.h"

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
  EXPECT_EQ(provider.identity().display_name, provider_capabilities.dbms_name);
  EXPECT_FALSE(provider_capabilities.identifier_quote.empty());
  EXPECT_EQ(63, provider_capabilities.max_identifier_length);
  ASSERT_NE(nullptr, session->transaction_session());
  EXPECT_EQ(provider.transaction_capabilities().supported,
            session->transaction_session()->transaction_capabilities().supported);
  ASSERT_FALSE(provider.type_catalog().empty());
  ASSERT_EQ(provider.type_catalog().size(), provider.type_catalog(session->server_version()).size());
  EXPECT_EQ(provider.type_catalog().front().name,
            provider.type_catalog(session->server_version()).front().name);
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
  EXPECT_FALSE(postgresql.create_session(nullptr)->is_connected());
  EXPECT_FALSE(redshift.create_session(nullptr)->is_connected());
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

TEST(BackendProviderTest, ProviderOwnsIdentityAndSessionDoesNotBorrowStaticPolicy) {
  std::unique_ptr<IDatabaseConnection> session;
  {
    std::string display = "Temporary PostgreSQL";
    postgres::PgBackendProvider provider{
        BackendIdentity{"temporary", display, "Temporary Driver"},
        BackendConnectionDefaults{"localhost", 5432, "postgres", true}};
    session = provider.create_session(nullptr);
    display.assign("changed");
    EXPECT_EQ("Temporary PostgreSQL", provider.capabilities().dbms_name);
  }
  ASSERT_NE(nullptr, session);
  EXPECT_FALSE(session->is_connected());
  ASSERT_NE(nullptr, session->catalog_queries());
  EXPECT_TRUE(session->catalog_queries()->catalog_query(TablesCatalogRequest{}));
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

TEST(BackendProviderTest, DialectIsImmutableAndIndependentOfSessionCreation) {
  const auto& provider = configured_backend_provider();
  const auto& dialect = provider.sql_dialect();
  const auto* address = &dialect;
  EXPECT_EQ(1u, dialect.count_parameter_markers(
      R"sql(SELECT '?', "?", $$?$$, $tag$?$tag$, ? /* ? /* ? */ */ -- ?
)sql"));
  EXPECT_EQ(0u, dialect.count_parameter_markers(""));
  auto translated = dialect.translate_sql("SELECT {fn UCASE('x')}, {d '2024-02-29'}");
  ASSERT_TRUE(translated); EXPECT_EQ("SELECT UPPER('x'), DATE '2024-02-29'", translated.sql);
  const auto invalid = dialect.translate_sql("SELECT {d '2023-02-29'}");
  EXPECT_FALSE(invalid); EXPECT_EQ(SqlTranslationError::InvalidDatetime, invalid.error);
  const auto unsupported = dialect.translate_sql("{?= call answer()}");
  EXPECT_FALSE(unsupported); EXPECT_EQ(SqlTranslationError::Unsupported, unsupported.error);
  {
    auto session = provider.create_session(nullptr);
    ASSERT_TRUE(session); EXPECT_FALSE(session->is_connected());
    EXPECT_EQ(address, &provider.sql_dialect());
    EXPECT_TRUE(dialect.translate_sql("SELECT {d '2024-02-29'}"));
  }
  EXPECT_EQ(address, &provider.sql_dialect());
  EXPECT_EQ("SELECT UPPER('x'), DATE '2024-02-29'", translated.sql);
}

TEST(BackendProviderTest, ResetProfileIsPostgreSqlOnlyAndNotInferredForRedshift) {
  using namespace rs::core::database;
  postgres::PgBackendProvider pg{{"renamed-provider", "PostgreSQL", "ODBCPP PostgreSQL"},
      {"localhost", 5432, "postgres", true}, SessionResetProfile::SameAuthenticatedServerSession};
  postgres::PgBackendProvider redshift{{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      {"localhost", 5439, std::nullopt, true}};
  postgres::PgBackendProvider default_profile{{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      {"localhost", 5432, "postgres", true}};
  EXPECT_EQ(nullptr, default_profile.create_session(nullptr)->session_reset());
  EXPECT_EQ(nullptr, postgres::PgDatabaseConnection{}.session_reset());
  auto pg_session = pg.create_session(nullptr);
  auto rs_session = redshift.create_session(nullptr);
  ASSERT_NE(nullptr, pg_session->session_reset());
  EXPECT_EQ(nullptr, rs_session->session_reset());
  EXPECT_EQ(SessionResetProfile::SameAuthenticatedServerSession, pg_session->session_reset()->reset_profile());
}
