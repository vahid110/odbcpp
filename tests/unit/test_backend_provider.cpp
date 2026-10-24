#include <gtest/gtest.h>

#include "odbcpp/database/backend_provider.h"
#include "core/database/database_factory.h"
#include "core/database/postgres/pg_backend_provider.h"
#include "core/database/postgres/pg_database_connection.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <limits>

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
  EXPECT_EQ(DatabaseFactory::get_compiled_database_type() == DatabaseType::Redshift ? 127 : 63,
      provider_capabilities.max_identifier_length);
  ASSERT_NE(nullptr, session->transaction_session());
  EXPECT_EQ(provider.transaction_capabilities().supported,
            session->transaction_session()->transaction_capabilities().supported);
  ASSERT_FALSE(provider.type_catalog().empty());
  ASSERT_EQ(provider.type_catalog().size(), provider.type_catalog(session->server_version()).size());
  EXPECT_EQ(provider.type_catalog().front().name,
            provider.type_catalog(session->server_version()).front().name);
}

TEST(BackendProviderTest, OptionalConnectionSupportUsesExplicitProfileRatherThanIdentity) {
  postgres::PgBackendProvider labelled_pg{
      {"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      {"host", 5439, "db", true}};
  postgres::PgBackendProvider renamed_redshift{
      {"custom", "Custom Database", "Custom Driver"},
      {"host", 5439, "db", true}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  const auto pg = labelled_pg.supported_connection_options();
  EXPECT_FALSE(pg.read_only);
  EXPECT_FALSE(pg.character_metadata);
  EXPECT_FALSE(pg.boolean_as_character);
  EXPECT_FALSE(pg.detailed_table_types);
  EXPECT_FALSE(pg.catalog_database_scope);
  EXPECT_FALSE(pg.catalog_mode);
  const auto rs = renamed_redshift.supported_connection_options();
  EXPECT_TRUE(rs.read_only);
  EXPECT_TRUE(rs.character_metadata);
  EXPECT_TRUE(rs.boolean_as_character);
  EXPECT_TRUE(rs.detailed_table_types);
  EXPECT_TRUE(rs.catalog_database_scope);
  EXPECT_TRUE(rs.catalog_mode);
}

TEST(BackendProviderTest, AdvertisedOptionalPoliciesMatchProviderValidationBeforeConnecting) {
  postgres::PgBackendProvider pg{{"custom", "Custom", "Custom Driver"},
      {"host", 5432, "db", true}};
  postgres::PgBackendProvider redshift{{"custom", "Custom", "Custom Driver"},
      {"host", 5439, "db", true}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  std::array<ConnectionOptions, 7> choices;
  choices[0].read_only = true;
  choices[1].varchar_threshold = 255;
  choices[2].long_varchar_size = 65535;
  choices[3].boolean_as_character = true;
  choices[4].detailed_table_types = false;
  choices[5].catalog_database_scope = CatalogDatabaseScope::VisibleDatabases;
  choices[6].redshift_catalog_mode = "LEGACY";
  for (std::size_t index = 0; index < choices.size(); ++index) {
    SCOPED_TRACE(index);
    EXPECT_TRUE(redshift.resolve_connection_options(choices[index]));
    EXPECT_FALSE(pg.resolve_connection_options(choices[index]));
  }
  EXPECT_TRUE(pg.resolve_connection_options({}));
  EXPECT_TRUE(redshift.resolve_connection_options({}));
}

TEST(BackendProviderTest, PostgreSqlFamilyProfilesStayIndependent) {
  EXPECT_EQ(0, ConnectionSettings{}.port);
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
  auto pg_settings = postgresql.resolve_connection_options({});
  auto rs_settings = redshift.resolve_connection_options({});
  ASSERT_TRUE(pg_settings); ASSERT_TRUE(rs_settings);
  EXPECT_EQ(5432, pg_settings->port);
  EXPECT_EQ(5439, rs_settings->port);
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
  ConnectionOptions unresolved;
  unresolved.port = 0;
  EXPECT_TRUE(provider.resolve_connection_options(unresolved).has_error());
  unresolved.port = 65535;
  auto maximum_port = provider.resolve_connection_options(unresolved);
  ASSERT_TRUE(maximum_port);
  EXPECT_EQ(65535, maximum_port->port);

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

TEST(BackendProviderTest, RedshiftPolicyRequiresExplicitProfileAndIgnoresPgVersion) {
  using namespace rs::core::database;
  const BackendIdentity identity{"redshift", "Amazon Redshift", "ODBCPP Redshift"};
  const BackendConnectionDefaults defaults{"localhost", 5439, std::nullopt, true};
  postgres::PgBackendProvider labelled_pg{identity, defaults};
  postgres::PgBackendProvider redshift{identity, defaults, std::nullopt,
      postgres::PgCatalogProfile::Redshift};
  EXPECT_EQ(63, labelled_pg.capabilities().max_identifier_length);
  EXPECT_TRUE(labelled_pg.capabilities().create_index);
  EXPECT_EQ(127, redshift.capabilities().max_identifier_length);
  EXPECT_FALSE(redshift.capabilities().create_index);
  EXPECT_FALSE(redshift.capabilities().drop_index);
  EXPECT_FALSE(redshift.capabilities().schema_in_index_definitions);
  for (const auto version : {"", "15.0", "17.11", "invalid"}) {
    int matched = 0;
    for (const auto& type : redshift.type_catalog(version)) {
      if (type.type != ScalarType::Numeric && type.type != ScalarType::Decimal) continue;
      ++matched;
      EXPECT_EQ(38u, type.column_size);
      EXPECT_EQ(0, type.minimum_scale);
      EXPECT_EQ(37, type.maximum_scale);
    }
    EXPECT_EQ(2, matched);
  }
  for (const auto& type : labelled_pg.type_catalog("15.0")) {
    if (type.type != ScalarType::Numeric && type.type != ScalarType::Decimal) continue;
    EXPECT_EQ(1000u, type.column_size);
    EXPECT_EQ(-1000, type.minimum_scale);
    EXPECT_EQ(1000, type.maximum_scale);
  }
}

TEST(BackendProviderTest, RedshiftCatalogModeDefaultExplicitInvalidAndProviderIsolation) {
  postgres::PgBackendProvider redshift{
      BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"host", 5439, "db", true}, std::nullopt,
      postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider postgres{
      BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"host", 5432, "db", true}};
  auto defaults = redshift.resolve_connection_options({});
  ASSERT_TRUE(defaults); EXPECT_EQ(RedshiftCatalogMode::Show, defaults->redshift_catalog_mode);
  auto pg_defaults = postgres.resolve_connection_options({});
  ASSERT_TRUE(pg_defaults); EXPECT_FALSE(pg_defaults->redshift_catalog_mode);
  for (const auto& [value, expected] : {std::pair{"SHOW", RedshiftCatalogMode::Show},
       std::pair{"show", RedshiftCatalogMode::Show}, std::pair{"LEGACY", RedshiftCatalogMode::Legacy},
       std::pair{"LeGaCy", RedshiftCatalogMode::Legacy}}) {
    ConnectionOptions options; options.redshift_catalog_mode = value;
    auto result = redshift.resolve_connection_options(options);
    ASSERT_TRUE(result); EXPECT_EQ(expected, result->redshift_catalog_mode);
    EXPECT_FALSE(postgres.resolve_connection_options(options));
  }
  for (const auto& value : {std::string{}, std::string{"AUTO"}, std::string{"SHOW LEGACY"},
       std::string{" SHOW"}, std::string("SHOW\0", 5)}) {
    ConnectionOptions options; options.redshift_catalog_mode = value;
    EXPECT_FALSE(redshift.resolve_connection_options(options));
  }
  auto again = redshift.resolve_connection_options({});
  ASSERT_TRUE(again); EXPECT_EQ(RedshiftCatalogMode::Show, again->redshift_catalog_mode);
}

TEST(BackendProviderTest, ApplicationNameOwnsAbsentEmptyAndUnicodePerResolution) {
  for (const bool redshift : {false, true}) {
    postgres::PgBackendProvider provider{
        BackendIdentity{redshift ? "redshift" : "postgresql", "test", "test"},
        BackendConnectionDefaults{"host", 5439, "db", false}, std::nullopt,
        redshift ? postgres::PgCatalogProfile::Redshift : postgres::PgCatalogProfile::PostgreSQL};
    auto defaults = provider.resolve_connection_options({});
    ASSERT_TRUE(defaults); EXPECT_FALSE(defaults->application_name);
    ConnectionOptions options;
    std::string input = "analytics-é表😀};literal";
    options.application_name = input;
    auto named = provider.resolve_connection_options(std::move(options));
    ASSERT_TRUE(named); ASSERT_TRUE(named->application_name);
    input.assign("changed");
    EXPECT_EQ("analytics-é表😀};literal", *named->application_name);
    options = {}; options.application_name = "";
    auto empty = provider.resolve_connection_options(std::move(options));
    ASSERT_TRUE(empty); ASSERT_TRUE(empty->application_name);
    EXPECT_TRUE(empty->application_name->empty());
    auto again = provider.resolve_connection_options({});
    ASSERT_TRUE(again); EXPECT_FALSE(again->application_name);
    EXPECT_EQ("analytics-é表😀};literal", *named->application_name);
  }
}

TEST(BackendProviderTest, ApplicationNameRejectsNulAndOversizeWithoutLeakingAndRecovers) {
  const auto& provider = configured_backend_provider();
  ConnectionOptions options;
  options.application_name = std::string("secret\0tail", 11);
  auto invalid = provider.resolve_connection_options(options);
  ASSERT_FALSE(invalid); EXPECT_EQ(rs::util::DbErrorCode::InvalidParameter, invalid.error());
  EXPECT_EQ(std::string::npos, invalid.error_message().find("secret"));
  options.application_name = "secret";
  options.input_limits.max_connection_field_bytes = 5;
  invalid = provider.resolve_connection_options(options);
  ASSERT_FALSE(invalid); EXPECT_EQ(rs::util::DbErrorCode::ResourceLimit, invalid.error());
  EXPECT_EQ(std::string::npos, invalid.error_message().find("secret"));
  options.input_limits.max_connection_field_bytes = 6;
  auto exact = provider.resolve_connection_options(options);
  ASSERT_TRUE(exact); EXPECT_EQ("secret", *exact->application_name);
  options.application_name = ""; options.input_limits.max_connection_field_bytes = 0;
  auto empty = provider.resolve_connection_options(options);
  ASSERT_TRUE(empty); EXPECT_TRUE(empty->application_name->empty());
}

TEST(BackendProviderTest, ReadOnlyIntentIsPerAttemptAndRefusesUnsupportedPostgres) {
  for (const bool redshift : {false, true}) {
    postgres::PgBackendProvider provider{
        BackendIdentity{redshift ? "redshift" : "postgresql", "test", "test"},
        BackendConnectionDefaults{"host", 5439, "db", false}, std::nullopt,
        redshift ? postgres::PgCatalogProfile::Redshift : postgres::PgCatalogProfile::PostgreSQL};
    auto defaults = provider.resolve_connection_options({});
    ASSERT_TRUE(defaults); EXPECT_FALSE(defaults->read_only);
    ConnectionOptions options; options.read_only = true;
    options.application_name = "owning-client";
    auto selected = provider.resolve_connection_options(options);
    if (redshift) {
      ASSERT_TRUE(selected); EXPECT_TRUE(selected->read_only);
      EXPECT_EQ("owning-client", *selected->application_name);
    } else {
      ASSERT_FALSE(selected); EXPECT_EQ(rs::util::DbErrorCode::UnsupportedFeature, selected.error());
    }
    options.read_only = false;
    auto repaired = provider.resolve_connection_options(options);
    ASSERT_TRUE(repaired); EXPECT_FALSE(repaired->read_only);
    auto again = provider.resolve_connection_options({});
    ASSERT_TRUE(again); EXPECT_FALSE(again->read_only);
    if (redshift) { EXPECT_TRUE(selected->read_only); }
  }
}

TEST(CharacterMetadataPolicyTest, StrictBoundaryReportedSizeAndNativeIdentityRemainOwning) {
  const NativeTypeInfo native{ScalarType::VarChar, 256, 0, true};
  const CharacterMetadataPolicy chosen{255, 65535};
  const auto reported = apply_character_metadata_policy(native, chosen);
  EXPECT_EQ(ScalarType::LongVarChar, reported.type);
  EXPECT_EQ(65535u, reported.column_size);
  EXPECT_EQ(ScalarType::VarChar, reported.declared_type);
  EXPECT_EQ(256u, reported.declared_column_size);
  EXPECT_EQ(ScalarType::VarChar, native.type);
  EXPECT_EQ(256u, native.column_size);
  const auto restored = declared_character_metadata(reported);
  EXPECT_EQ(native.type, restored.type); EXPECT_EQ(native.column_size, restored.column_size);
  EXPECT_FALSE(restored.declared_type); EXPECT_FALSE(restored.declared_column_size);
  const auto again = apply_character_metadata_policy(reported, {1, 7});
  EXPECT_EQ(reported.type, again.type); EXPECT_EQ(reported.column_size, again.column_size);
  EXPECT_EQ(reported.declared_column_size, again.declared_column_size);
  const auto boundary = apply_character_metadata_policy({ScalarType::Char, 255, 0, true}, chosen);
  EXPECT_EQ(ScalarType::Char, boundary.type); EXPECT_EQ(255u, boundary.column_size);
  const auto character = apply_character_metadata_policy({ScalarType::Char, 256, 0, true}, {255, 0});
  EXPECT_EQ(ScalarType::LongVarChar, character.type); EXPECT_EQ(256u, character.column_size);
  EXPECT_EQ(ScalarType::Char, character.declared_type);
}

TEST(CharacterMetadataPolicyTest, DisabledUnknownZeroAndOtherFamiliesPreserveActualMetadata) {
  for (const auto type : {ScalarType::Char, ScalarType::VarChar, ScalarType::LongVarChar,
                         ScalarType::Binary, ScalarType::LongVarBinary, ScalarType::Integer,
                         ScalarType::Numeric, ScalarType::Timestamp}) {
    const NativeTypeInfo native{type, 65535, 3, true};
    const auto disabled = apply_character_metadata_policy(native, {0, 7});
    EXPECT_EQ(type, disabled.type); EXPECT_EQ(65535u, disabled.column_size);
    EXPECT_EQ(3, disabled.decimal_digits); EXPECT_TRUE(disabled.known);
    EXPECT_FALSE(disabled.declared_type);
    if (type != ScalarType::Char && type != ScalarType::VarChar) {
      const auto selected = apply_character_metadata_policy(native, {1, 7});
      EXPECT_EQ(type, selected.type); EXPECT_EQ(65535u, selected.column_size);
    }
  }
  for (const auto& native : {NativeTypeInfo{ScalarType::VarChar, 65535, 0, false},
                            NativeTypeInfo{ScalarType::VarChar, 0, 0, true}}) {
    const auto selected = apply_character_metadata_policy(native, {1, 7});
    EXPECT_EQ(native.type, selected.type); EXPECT_EQ(native.column_size, selected.column_size);
    EXPECT_EQ(native.known, selected.known); EXPECT_FALSE(selected.declared_type);
  }
  EXPECT_TRUE(valid_character_metadata_policy({0, 0}));
  const auto maximum = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
  EXPECT_TRUE(valid_character_metadata_policy({maximum, maximum}));
  EXPECT_FALSE(valid_character_metadata_policy({maximum + 1, 0}));
  EXPECT_FALSE(valid_character_metadata_policy({0, maximum + 1}));
}

TEST(BackendProviderTest, VarcharSizingDefaultsSelectedZeroBoundsAndProviderIsolationAreIndependent) {
  postgres::PgBackendProvider redshift{{"redshift", "test", "test"},
      {"host", 5439, "db", false}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider pg{{"postgresql", "test", "test"}, {"host", 5432, "db", false}};
  auto defaults = redshift.resolve_connection_options({}); ASSERT_TRUE(defaults);
  EXPECT_EQ((CharacterMetadataPolicy{255, 65535}), defaults->character_metadata);
  auto neutral = pg.resolve_connection_options({}); ASSERT_TRUE(neutral);
  EXPECT_EQ((CharacterMetadataPolicy{}), neutral->character_metadata);
  EXPECT_EQ((CharacterMetadataPolicy{}), ConnectionSettings{}.character_metadata);
  ConnectionOptions options; options.varchar_threshold = 0; options.long_varchar_size = 0;
  auto disabled = redshift.resolve_connection_options(options); ASSERT_TRUE(disabled);
  EXPECT_EQ((CharacterMetadataPolicy{}), disabled->character_metadata);
  EXPECT_FALSE(pg.resolve_connection_options(options));
  options.varchar_threshold = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
  options.long_varchar_size = 65535;
  auto maximum = redshift.resolve_connection_options(options); ASSERT_TRUE(maximum);
  options.varchar_threshold = *options.varchar_threshold + 1;
  auto overflow = redshift.resolve_connection_options(options); ASSERT_FALSE(overflow);
  EXPECT_EQ("Invalid MaxVarcharSize value", overflow.error_message());
  options.varchar_threshold = 0; options.long_varchar_size = 65536;
  auto invalid_disabled = redshift.resolve_connection_options(options); ASSERT_FALSE(invalid_disabled);
  EXPECT_EQ("Invalid MaxLongVarcharSize value", invalid_disabled.error_message());
  options = {}; options.long_varchar_size = 17;
  auto partial = redshift.resolve_connection_options(options); ASSERT_TRUE(partial);
  EXPECT_EQ((CharacterMetadataPolicy{255, 17}), partial->character_metadata);
  EXPECT_EQ((CharacterMetadataPolicy{255, 65535}), defaults->character_metadata);
  auto fresh = redshift.resolve_connection_options({}); ASSERT_TRUE(fresh);
  EXPECT_EQ(defaults->character_metadata, fresh->character_metadata);
}

TEST(BooleanCharacterPresentation, SdkPreservesLogicalFamilyAndParametersWithoutLongTextPromotion) {
  using namespace rs::core::database;
  const CharacterMetadataPolicy policy{1, 65535, true};
  const NativeTypeInfo native{ScalarType::Boolean, 1, 0, true};
  const auto text = apply_character_metadata_policy(native, policy);
  EXPECT_EQ(ScalarType::VarChar, text.type); EXPECT_EQ(1u, text.column_size);
  EXPECT_EQ(ScalarType::Boolean, text.declared_type); EXPECT_EQ(1u, text.declared_column_size);
  const auto again = apply_character_metadata_policy(text, policy);
  EXPECT_EQ(text.type, again.type); EXPECT_EQ(text.declared_type, again.declared_type);
  const auto restored = declared_character_metadata(text);
  EXPECT_EQ(native.type, restored.type); EXPECT_EQ(native.column_size, restored.column_size);
  EXPECT_FALSE(restored.declared_type);
  EXPECT_EQ(ScalarType::Boolean, apply_character_metadata_policy(native, {}).type);
  EXPECT_EQ(ScalarType::Boolean, apply_character_metadata_policy({ScalarType::Boolean, 0, 0, false}, policy).type);
  EXPECT_EQ(ScalarType::LongVarChar, apply_character_metadata_policy({ScalarType::VarChar, 2, 0, true}, policy).type);
  EXPECT_EQ(ScalarType::Integer, apply_character_metadata_policy({ScalarType::Integer, 10, 0, true}, policy).type);
}

TEST(BooleanCharacterPresentation, ProviderDefaultsOwnExplicitPolicyAndRefuseOtherBackendBeforeIo) {
  using namespace rs::core::database;
  postgres::PgBackendProvider rs{BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"rs", 5439, std::nullopt, false}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider pg{BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"pg", 5432, "postgres", false}};
  auto defaults = rs.resolve_connection_options({}); ASSERT_TRUE(defaults);
  EXPECT_FALSE(defaults->character_metadata.boolean_as_character);
  ConnectionOptions options; options.boolean_as_character = true;
  auto enabled = rs.resolve_connection_options(options); ASSERT_TRUE(enabled);
  const auto owned = enabled->character_metadata;
  options.boolean_as_character = false; EXPECT_TRUE(owned.boolean_as_character);
  EXPECT_FALSE(rs.resolve_connection_options(options)->character_metadata.boolean_as_character);
  options.boolean_as_character = true; auto refused = pg.resolve_connection_options(options);
  ASSERT_FALSE(refused); EXPECT_EQ("BoolsAsChar requires Redshift", refused.error_message());
  options.boolean_as_character = false; ASSERT_TRUE(pg.resolve_connection_options(options));
}

TEST(TableTypePresentation, SdkNormalizedTaxonomyGeneralizesKnownKindsAndRefusesUnknowns) {
  using namespace rs::core::database;
  const auto detailed = catalog_table_types(); ASSERT_EQ(6u, detailed.size());
  EXPECT_TRUE(std::is_sorted(detailed.begin(), detailed.end()));
  const auto generic = catalog_table_types(false); ASSERT_EQ(2u, generic.size());
  EXPECT_EQ("TABLE", generic[0]); EXPECT_EQ("VIEW", generic[1]);
  for (const auto type : detailed) {
    ASSERT_TRUE(catalog_table_type(type)); EXPECT_EQ(type, *catalog_table_type(type));
    ASSERT_TRUE(catalog_table_type(type, false));
    EXPECT_EQ(type == "VIEW" || type == "SYSTEM VIEW" ? "VIEW" : "TABLE", *catalog_table_type(type, false));
  }
  for (const char* invalid : {"", "view", "UNKNOWN TABLE", "GLOBAL TEMPORARY", "secret"}) {
    EXPECT_FALSE(catalog_table_type(invalid)); EXPECT_FALSE(catalog_table_type(invalid, false));
  }
}

TEST(TableTypePresentation, ProviderDefaultsExplicitFalseOwnershipAndOtherProviderIsolation) {
  using namespace rs::core::database;
  postgres::PgBackendProvider rs{BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"rs", 5439, std::nullopt, false}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider pg{BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"pg", 5432, "postgres", false}};
  ASSERT_TRUE(rs.resolve_connection_options({})); EXPECT_TRUE(rs.resolve_connection_options({})->detailed_table_types);
  ConnectionOptions options; options.detailed_table_types = false;
  auto disabled = rs.resolve_connection_options(options); ASSERT_TRUE(disabled); EXPECT_FALSE(disabled->detailed_table_types);
  const auto owned = *disabled; options.detailed_table_types = true;
  EXPECT_FALSE(owned.detailed_table_types); EXPECT_TRUE(rs.resolve_connection_options(options)->detailed_table_types);
  auto unsupported = pg.resolve_connection_options(options); ASSERT_FALSE(unsupported);
  EXPECT_EQ("EnableTableTypes requires Redshift", unsupported.error_message());
  options.detailed_table_types.reset(); ASSERT_TRUE(pg.resolve_connection_options(options));
}

TEST(MetadataDatabaseScope, ProviderDefaultsOwningSelectionIsolationAndInvalidIntent) {
  using namespace rs::core::database;
  postgres::PgBackendProvider rs{BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"rs", 5439, std::nullopt, false}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider pg{BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"pg", 5432, "postgres", false}};
  auto defaults = rs.resolve_connection_options({}); ASSERT_TRUE(defaults);
  EXPECT_EQ(CatalogDatabaseScope::CurrentDatabase, defaults->catalog_database_scope);
  ConnectionOptions options; options.catalog_database_scope = CatalogDatabaseScope::VisibleDatabases;
  auto selected = rs.resolve_connection_options(options); ASSERT_TRUE(selected);
  EXPECT_EQ(CatalogDatabaseScope::VisibleDatabases, selected->catalog_database_scope);
  options.catalog_database_scope = CatalogDatabaseScope::CurrentDatabase;
  EXPECT_EQ(CatalogDatabaseScope::VisibleDatabases, selected->catalog_database_scope);
  auto current = rs.resolve_connection_options(options); ASSERT_TRUE(current);
  EXPECT_EQ(CatalogDatabaseScope::CurrentDatabase, current->catalog_database_scope);
  auto unsupported = pg.resolve_connection_options(options); ASSERT_FALSE(unsupported);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature), unsupported.error());
  options.catalog_database_scope = static_cast<CatalogDatabaseScope>(-1);
  auto invalid = rs.resolve_connection_options(options); ASSERT_FALSE(invalid);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter), invalid.error());
  options.catalog_database_scope.reset(); ASSERT_TRUE(pg.resolve_connection_options(options));
}

TEST(NativeIdpResolution, ProviderDerivesIdpTypeAndMapsTokenMaterial) {
  using namespace rs::core::database;
  postgres::PgBackendProvider rs{BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"rs", 5439, std::nullopt, false}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider pg{BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"pg", 5432, "postgres", false}};
  ConnectionOptions options; options.host = "cluster.example";
  options.plugin_name = "IdpTokenAuthPlugin"; options.token = "jwt-token";
  options.provider_name = "odbcpp_idc"; options.token_type = "ACCESS_TOKEN"; options.identity_namespace = "awsidc";
  auto resolved = rs.resolve_connection_options(options); ASSERT_TRUE(resolved);
  ASSERT_TRUE(resolved->native_idp.has_value());
  EXPECT_EQ("jwt-token", resolved->native_idp->token);
  EXPECT_EQ("AwsIdc", resolved->native_idp->idp_type);
  EXPECT_EQ("odbcpp_idc", resolved->native_idp->provider_name);
  EXPECT_EQ("ACCESS_TOKEN", resolved->native_idp->token_type);
  EXPECT_EQ("awsidc", resolved->native_idp->identity_namespace);
  EXPECT_TRUE(resolved->password.empty());
  // Case-insensitive selector and BrowserIdc map to the same wire type.
  options.plugin_name = "browseridcauthplugin";
  auto browser = rs.resolve_connection_options(options); ASSERT_TRUE(browser);
  EXPECT_EQ("AwsIdc", browser->native_idp->idp_type);
  // PostgreSQL provider refuses native IdP.
  auto refused = pg.resolve_connection_options(options); ASSERT_FALSE(refused);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature), refused.error());
}
TEST(NativeIdpResolution, MissingConflictAndUnsupportedPluginRefuse) {
  using namespace rs::core::database;
  postgres::PgBackendProvider rs{BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"rs", 5439, std::nullopt, false}, std::nullopt, postgres::PgCatalogProfile::Redshift};
  ConnectionOptions base; base.plugin_name = "IdpTokenAuthPlugin"; base.token = "jwt-token";
  base.provider_name = "odbcpp_idc"; base.token_type = "ACCESS_TOKEN"; base.identity_namespace = "awsidc";
  for (unsigned which = 0; which < 5; ++which) {
    auto missing = base;
    if (which == 0) missing.token.reset();
    if (which == 1) missing.plugin_name.reset();
    if (which == 2) missing.provider_name.reset();
    if (which == 3) missing.token_type.reset();
    if (which == 4) missing.identity_namespace.reset();
    EXPECT_FALSE(rs.resolve_connection_options(missing));
  }
  auto conflict = base; conflict.password = "pw";
  EXPECT_FALSE(rs.resolve_connection_options(conflict));
  auto unknown = base; unknown.plugin_name = "Okta";
  EXPECT_FALSE(rs.resolve_connection_options(unknown));
  auto nul = base; nul.token = std::string("a\0b", 3);
  EXPECT_FALSE(rs.resolve_connection_options(nul));
  // No native-IdP options leaves native_idp unset.
  ASSERT_TRUE(rs.resolve_connection_options({}));
  EXPECT_FALSE(rs.resolve_connection_options({})->native_idp.has_value());
}
