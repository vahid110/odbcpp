#include <gtest/gtest.h>

#include "odbcpp/database/backend_provider.h"
#include "core/database/postgres/pg_backend_provider.h"

#include <optional>
#include <string>

namespace {

using namespace rs::core::database;

TEST(TlsTrustSelectionTest, DerivesSourceAndRejectsEveryConflict) {
  struct Case {
    bool tls;
    std::optional<bool> system;
    const char* file;
    const char* directory;
    TlsTrustSource source;
    TlsTrustConflict conflict;
  };
  constexpr auto none = TlsTrustConflict::None;
  for (const auto& test : {
           Case{true, std::nullopt, "", "", TlsTrustSource::System, none},
           Case{true, true, "", "", TlsTrustSource::System, none},
           Case{true, std::nullopt, "/ca.pem", "", TlsTrustSource::CustomFile, none},
           Case{true, false, "/ca.pem", "", TlsTrustSource::CustomFile, none},
           Case{true, std::nullopt, "", "/certs", TlsTrustSource::CustomDirectory, none},
           Case{true, false, "", "/certs", TlsTrustSource::CustomDirectory, none},
           Case{true, std::nullopt, "/ca.pem", "/certs", TlsTrustSource::System,
                TlsTrustConflict::FileAndDirectory},
           Case{true, false, "/ca.pem", "/certs", TlsTrustSource::System,
                TlsTrustConflict::FileAndDirectory},
           Case{true, true, "/ca.pem", "", TlsTrustSource::System,
                TlsTrustConflict::SystemWithCustom},
           Case{true, true, "", "/certs", TlsTrustSource::System,
                TlsTrustConflict::SystemWithCustom},
           Case{true, false, "", "", TlsTrustSource::System,
                TlsTrustConflict::CustomWithoutLocation},
           Case{false, std::nullopt, "", "", TlsTrustSource::System, none},
           Case{false, true, "", "", TlsTrustSource::System,
                TlsTrustConflict::SettingsWithoutTls},
           Case{false, false, "", "", TlsTrustSource::System,
                TlsTrustConflict::SettingsWithoutTls},
           Case{false, std::nullopt, "/ca.pem", "", TlsTrustSource::System,
                TlsTrustConflict::SettingsWithoutTls}}) {
    const auto selected =
        select_tls_trust_source(test.tls, test.system, test.file, test.directory);
    EXPECT_EQ(selected.source, test.source) << test.file << "|" << test.directory;
    EXPECT_EQ(selected.conflict, test.conflict) << test.file << "|" << test.directory;
  }
}

TEST(TlsTrustSelectionTest, DiagnosticNamesAreStableCategories) {
  EXPECT_EQ(to_string(TlsTrustSource::System), "system");
  EXPECT_EQ(to_string(TlsTrustSource::CustomFile), "custom_file");
  EXPECT_EQ(to_string(TlsTrustSource::CustomDirectory), "custom_directory");
  static_assert(select_tls_trust_source(true, std::nullopt, "", "").source ==
                TlsTrustSource::System);
}

class RedshiftTrustOptionsTest : public ::testing::Test {
 protected:
  postgres::PgBackendProvider redshift{
      BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"host", 5439, "dev", true}, std::nullopt,
      postgres::PgCatalogProfile::Redshift};

  std::string error_for(ConnectionOptions options) {
    auto resolved = redshift.resolve_connection_options(std::move(options));
    return resolved.has_error() ? resolved.error_message() : std::string{};
  }
};

TEST_F(RedshiftTrustOptionsTest, DefaultsToOperatingSystemTrustStore) {
  auto resolved = redshift.resolve_connection_options({});
  ASSERT_TRUE(resolved.has_value());
  EXPECT_TRUE(resolved->use_ssl);
  EXPECT_TRUE(resolved->ssl_ca_file.empty());
  EXPECT_TRUE(resolved->ssl_ca_dir.empty());
  EXPECT_EQ(select_tls_trust_source(true, std::nullopt, resolved->ssl_ca_file,
                                    resolved->ssl_ca_dir).source,
            TlsTrustSource::System);
}

TEST_F(RedshiftTrustOptionsTest, ExplicitSystemAndCustomSelectionsResolve) {
  ConnectionOptions system;
  system.ssl_use_system_trust_store = true;
  auto system_settings = redshift.resolve_connection_options(system);
  ASSERT_TRUE(system_settings.has_value());
  EXPECT_TRUE(system_settings->ssl_ca_file.empty());

  ConnectionOptions custom;
  custom.ssl_use_system_trust_store = false;
  custom.ssl_ca_file = "/opt/odbc roots/amazon-trust-ca-bundle.crt";
  auto custom_settings = redshift.resolve_connection_options(custom);
  ASSERT_TRUE(custom_settings.has_value());
  EXPECT_EQ(custom_settings->ssl_ca_file, "/opt/odbc roots/amazon-trust-ca-bundle.crt");

  ConnectionOptions cleared;
  cleared.ssl_use_system_trust_store = true;
  cleared.ssl_ca_file = "";
  EXPECT_TRUE(redshift.resolve_connection_options(cleared).has_value());
}

TEST_F(RedshiftTrustOptionsTest, ConflictsFailClosedWithBackendKeywords) {
  ConnectionOptions both;
  both.ssl_use_system_trust_store = true;
  both.ssl_ca_file = "/ca.pem";
  EXPECT_EQ(error_for(both),
            "UseSystemTrustStore=1 cannot be combined with SSLCAFILE or SSLCADIR");

  ConnectionOptions directory;
  directory.ssl_use_system_trust_store = true;
  directory.ssl_ca_dir = "/certs";
  EXPECT_EQ(error_for(directory),
            "UseSystemTrustStore=1 cannot be combined with SSLCAFILE or SSLCADIR");

  ConnectionOptions missing;
  missing.ssl_use_system_trust_store = false;
  EXPECT_EQ(error_for(missing), "UseSystemTrustStore=0 requires SSLCAFILE or SSLCADIR");

  ConnectionOptions plaintext;
  plaintext.use_ssl = false;
  plaintext.ssl_use_system_trust_store = true;
  EXPECT_EQ(error_for(plaintext), "UseSystemTrustStore requires SSL=true");

  ConnectionOptions plaintext_file;
  plaintext_file.use_ssl = false;
  plaintext_file.ssl_ca_file = "/ca.pem";
  EXPECT_EQ(error_for(plaintext_file), "SSLCAFILE and SSLCADIR require SSL=true");

  ConnectionOptions plaintext_both;
  plaintext_both.use_ssl = false;
  plaintext_both.ssl_use_system_trust_store = false;
  plaintext_both.ssl_ca_file = "/ca.pem";
  EXPECT_EQ(error_for(plaintext_both),
            "UseSystemTrustStore, SSLCAFILE and SSLCADIR require SSL=true");

  ConnectionOptions ambiguous;
  ambiguous.ssl_ca_file = "/ca.pem";
  ambiguous.ssl_ca_dir = "/certs";
  EXPECT_EQ(error_for(ambiguous), "SSLCAFILE and SSLCADIR cannot both be specified");
}

TEST_F(RedshiftTrustOptionsTest, PlaintextWithoutTrustSettingsStillResolves) {
  ConnectionOptions plaintext;
  plaintext.use_ssl = false;
  auto resolved = redshift.resolve_connection_options(plaintext);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_FALSE(resolved->use_ssl);
}

}  // namespace
