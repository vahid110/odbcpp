#include <gtest/gtest.h>
#include "odbc/setup_model.h"
using namespace rs::odbc::setup;
namespace {
Fields valid() { Fields f; f.dsn=L"Test"; f.server=L"localhost"; f.database=L"postgres"; return f; }
TEST(SetupModel, PersistsSettingsWithoutCredentials) {
  auto f=valid(); f.password=L"secret"; f.user=L"alice";
  const auto a=persisted(f);
  EXPECT_EQ(a.size(),5u); EXPECT_EQ(a.at(L"UID"),L"alice"); EXPECT_EQ(a.at(L"SSL"),L"1");
  EXPECT_EQ(a.count(L"PWD"),0u); EXPECT_EQ(a.count(L"PASSWORD"),0u);
}
TEST(SetupModel, EscapesTestCredentialsAndUnicode) {
  auto f=valid(); f.password=L"caf\u00e9;}tail";
  EXPECT_NE(connection_string(f).find(L"PWD={caf\u00e9;}}tail};"),std::wstring::npos);
  EXPECT_EQ(braced(L""),L"{}");
}
TEST(SetupModel, RejectsInvalidNames) {
  for(const auto name:{L"",L"bad;name",L"bad\\name",L"ODBC Data Sources",L"odbc data sources"}) {
    auto f=valid(); f.dsn=name; EXPECT_THROW(validate(f),std::invalid_argument);
  }
  auto f=valid(); f.dsn=std::wstring(33,L'x'); EXPECT_THROW(validate(f),std::invalid_argument);
}
TEST(SetupModel, ValidatesPortBounds) {
  for(const auto port:{L"",L"0",L"65536",L"-1",L"5432x",L"999999"}) {
    auto f=valid(); f.port=port; EXPECT_THROW(validate(f),std::invalid_argument);
  }
  for (const auto port : {L"1", L"65535"}) {
    auto f = valid();
    f.port = port;
    EXPECT_NO_THROW(validate(f));
  }
}
TEST(SetupModel, RejectsMissingAndControlFields) {
  auto f=valid(); f.server=L""; EXPECT_THROW(validate(f),std::invalid_argument);
  f=valid(); f.database=L""; EXPECT_THROW(validate(f),std::invalid_argument);
  f=valid(); f.user=L"line\nbreak"; EXPECT_THROW(validate(f),std::invalid_argument);
  f=valid(); f.password=std::wstring(1025,L'x'); EXPECT_THROW(validate(f),std::invalid_argument);
}
TEST(SetupModel, AcceptsUnicodeAndOptionalUser) {
  auto f=valid(); f.dsn=L"\u6d4b\u8bd5"; f.database=L"caf\u00e9"; f.ssl=false;
  EXPECT_NO_THROW(validate(f)); EXPECT_EQ(persisted(f).at(L"SSL"),L"0");
}
TEST(SetupModel, UsesCompiledProviderIdentityAndDefaults) {
  const auto& provider = rs::core::database::configured_backend_provider();
  const Fields f;
  EXPECT_EQ(driver_name(), from_utf8(provider.identity().driver_name));
  EXPECT_EQ(f.server, from_utf8(provider.connection_defaults().host));
  EXPECT_EQ(f.port, std::to_wstring(provider.connection_defaults().port));
  EXPECT_EQ(f.database, from_utf8(provider.connection_defaults().database.value_or("")));
  EXPECT_EQ(f.ssl, provider.connection_defaults().use_ssl);
  EXPECT_TRUE(f.advanced.empty());
}
TEST(SetupModel, TestsExactlyThePersistedSettingsAndTransientPassword) {
  auto f = valid();
  f.password = L"temporary;}secret";
  overlay(f, {{L"APPLICATIONNAME", L"Desktop"}, {L"SSLCAFILE", L"C:\\certs\\ca.pem"},
      {L"LOGINTIMEOUT", L"12"}, {L"QUERYTIMEOUT", L"0"}, {L"KEEPALIVE", L"true"},
      {L"LOGLEVEL", L"Debug"}, {L"LOGFILE", L"C:\\logs\\driver.log"},
      {L"MAXRESULTROWS", L"1000"}, {L"CUSTOM_OPTION", L"x;}y"}});
  const auto stored = persisted(f);
  std::wstring expected = L"DRIVER=" + braced(driver_name()) + L";";
  for (const auto& [key, value] : stored) {
    expected += key + L"=" + braced(value) + L";";
  }
  expected += L"PWD=" + braced(f.password) + L";";
  EXPECT_EQ(connection_string(f), expected);
  EXPECT_EQ(stored.at(L"MAXRESULTROWS"), L"1000");
  EXPECT_EQ(stored.at(L"CUSTOM_OPTION"), L"x;}y");
  EXPECT_NE(connection_string(f).find(L"CUSTOM_OPTION={x;}}y};"), std::wstring::npos);
}
TEST(SetupModel, NormalizesAliasesAtEachPrecedenceLayer) {
  auto f = valid();
  overlay(f, {{L"Server", L"first"}, {L"Database", L"first_db"}, {L"UID", L"first_user"},
      {L"LOGINTIMEOUT", L"20"}, {L"QUERYTIMEOUT", L"30"}});
  overlay(f, {{L"host", L"second"}, {L"db", L"second_db"}, {L"user", L"second_user"},
      {L"lt", L"2"}, {L"qt", L"3"}, {L"password", L"transient"}});
  EXPECT_EQ(f.server, L"second");
  EXPECT_EQ(f.database, L"second_db");
  EXPECT_EQ(f.user, L"second_user");
  EXPECT_EQ(f.password, L"transient");
  EXPECT_EQ(f.advanced.at(L"LOGINTIMEOUT"), L"2");
  EXPECT_EQ(f.advanced.at(L"QUERYTIMEOUT"), L"3");
  EXPECT_EQ(f.advanced.count(L"LT"), 0u);
  overlay(f, {{L"SERVER", L"primary"}, {L"HOST", L"alias"},
      {L"LOGINTIMEOUT", L"5"}, {L"LT", L"8"}});
  EXPECT_EQ(f.server, L"primary");
  EXPECT_EQ(f.advanced.at(L"LOGINTIMEOUT"), L"5");
}
TEST(SetupModel, PreservesExplicitBlankAndResetsToInherited) {
  auto f = valid();
  EXPECT_EQ(persisted(f).count(L"APPLICATIONNAME"), 0u);
  overlay(f, {{L"APPLICATIONNAME", L""}, {L"SSLCAFILE", L""}, {L"LOGINTIMEOUT", L"0"}});
  EXPECT_EQ(persisted(f).at(L"APPLICATIONNAME"), L"");
  EXPECT_EQ(persisted(f).at(L"SSLCAFILE"), L"");
  f.removed.insert(L"APPLICATIONNAME");
  EXPECT_EQ(persisted(f).count(L"APPLICATIONNAME"), 0u);
  EXPECT_EQ(connection_string(f).find(L"APPLICATIONNAME="), std::wstring::npos);
  overlay(f, {{L"APPLICATIONNAME", L"new"}});
  EXPECT_EQ(f.removed.count(L"APPLICATIONNAME"), 0u);
  EXPECT_EQ(persisted(f).at(L"APPLICATIONNAME"), L"new");
}
TEST(SetupModel, ProtectsRoutingCredentialsAndBasicSettings) {
  auto f = valid();
  f.password = L"transient";
  f.advanced = {{L"DSN", L"other"}, {L"DRIVER", L"other"}, {L"FILEDSN", L"other"},
      {L"SAVEFILE", L"other"}, {L"PWD", L"legacy"}, {L"PASSWORD", L"legacy"},
      {L"UID", L"shadow"}, {L"HOST", L"shadow"}};
  const auto stored = persisted(f);
  EXPECT_EQ(stored.size(), 5u);
  EXPECT_EQ(stored.at(L"UID"), L"");
  EXPECT_EQ(stored.at(L"Server"), L"localhost");
  EXPECT_EQ(connection_string(f).find(L"legacy"), std::wstring::npos);
  EXPECT_EQ(connection_string(f).find(L"DSN="), std::wstring::npos);
  EXPECT_NE(connection_string(f).find(L"PWD={transient};"), std::wstring::npos);
  overlay(f, {{L"DRIVER", L"other"}, {L"FILEDSN", L"other"}, {L"bad;key", L"value"}});
  EXPECT_EQ(f.advanced.count(L"bad;key"), 0u);
}
TEST(SetupModel, PropertyExposureMatchesCompiledBackendSupport) {
  const auto support = rs::core::database::configured_backend_provider().supported_connection_options();
  EXPECT_TRUE(supported_property(L"LT"));
  EXPECT_TRUE(supported_property(L"SSLCAFILE"));
  EXPECT_FALSE(supported_property(L"MAXRESULTROWS"));
  EXPECT_EQ(supported_property(L"READONLY"), support.read_only);
  EXPECT_EQ(supported_property(L"MAXVARCHARSIZE"), support.character_metadata);
  EXPECT_EQ(supported_property(L"MAXLONGVARCHARSIZE"), support.character_metadata);
  EXPECT_EQ(supported_property(L"BOOLSASCHAR"), support.boolean_as_character);
  EXPECT_EQ(supported_property(L"ENABLETABLETYPES"), support.detailed_table_types);
  EXPECT_EQ(supported_property(L"DATABASEMETADATACURRENTDBONLY"), support.catalog_database_scope);
  EXPECT_EQ(supported_property(L"REDSHIFTCATALOGMODE"), support.catalog_mode);
  for (const auto key : {L"READONLY", L"MAXVARCHARSIZE", L"MAXLONGVARCHARSIZE", L"BOOLSASCHAR",
       L"ENABLETABLETYPES", L"DATABASEMETADATACURRENTDBONLY", L"REDSHIFTCATALOGMODE"}) {
    auto f = valid();
    f.advanced[key] = std::wstring(key) == L"REDSHIFTCATALOGMODE" ? L"SHOW" : L"1";
    if (supported_property(key)) {
      EXPECT_NO_THROW(validate(f));
    } else {
      EXPECT_THROW(validate(f), std::invalid_argument);
    }
  }
}
TEST(SetupModel, ValidatesAdvancedRuntimeBoundsWithoutEchoingValues) {
  for (const auto& property : properties()) {
    auto f = valid();
    if (property.kind == PropertyKind::Unsigned) {
      f.advanced[property.key] = std::to_wstring(property.maximum);
      EXPECT_NO_THROW(validate(f));
      f.advanced[property.key] += L"0";
      EXPECT_THROW(validate(f), std::invalid_argument);
      f.advanced[property.key] = L"";
      EXPECT_THROW(validate(f), std::invalid_argument);
      f.advanced[property.key] = L"-1";
      EXPECT_THROW(validate(f), std::invalid_argument);
    }
    if (property.kind == PropertyKind::Boolean) {
      for (const auto value : {L"0", L"1", L"true", L"FALSE", L" true "}) {
        f.advanced[property.key] = value;
        EXPECT_NO_THROW(validate(f));
      }
      f.advanced[property.key] = L"yes";
      EXPECT_THROW(validate(f), std::invalid_argument);
    }
    if (property.kind == PropertyKind::Choice) {
      for (const auto& choice : property.choices) {
        f.advanced[property.key] = choice;
        EXPECT_NO_THROW(validate(f));
      }
      f.advanced[property.key] = L"unknown";
      EXPECT_THROW(validate(f), std::invalid_argument);
    }
  }
  auto f = valid();
  f.advanced[L"LOGLEVEL"] = L"WARNING";
  EXPECT_NO_THROW(validate(f));
  f.advanced[L"LOGLEVEL"] = L"private-secret-value";
  try {
    validate(f);
    FAIL() << "Invalid advanced value accepted";
  } catch (const std::invalid_argument& error) {
    EXPECT_EQ(std::string(error.what()).find("private-secret-value"), std::string::npos);
  }
}
TEST(SetupModel, RejectsInvalidAdvancedTextAndKeyword) {
  auto f = valid();
  f.advanced[L"APPLICATIONNAME"] = std::wstring(1024, L'x');
  EXPECT_NO_THROW(validate(f));
  f.advanced[L"APPLICATIONNAME"] += L"x";
  EXPECT_THROW(validate(f), std::invalid_argument);
  f.advanced.clear();
  f.advanced[L"CUSTOM"] = L"line\nbreak";
  EXPECT_NO_THROW(validate(f));
  EXPECT_EQ(persisted(f).at(L"CUSTOM"), L"line\nbreak");
  f.advanced.clear();
  f.advanced[L"BAD;KEY"] = L"value";
  EXPECT_THROW(validate(f), std::invalid_argument);
}
TEST(SetupModel, PreservesUnexposedSettingsWithinInstallerLimits) {
  auto f = valid();
  f.advanced[L"CUSTOM"] = std::wstring(32766, L'x');
  EXPECT_NO_THROW(validate(f));
  EXPECT_EQ(persisted(f).at(L"CUSTOM"), f.advanced.at(L"CUSTOM"));
  f.advanced[L"CUSTOM"] += L"x";
  EXPECT_THROW(validate(f), std::invalid_argument);
  f.advanced[L"CUSTOM"] = std::wstring(L"a\0b", 3);
  EXPECT_THROW(validate(f), std::invalid_argument);
  f.advanced[L"CUSTOM"] = L"\tvalue\nwith\rcontrols";
  EXPECT_NO_THROW(validate(f));
  EXPECT_EQ(persisted(f).at(L"CUSTOM"), f.advanced.at(L"CUSTOM"));
  f.advanced[L"APPLICATIONNAME"] = L"line\nbreak";
  EXPECT_THROW(validate(f), std::invalid_argument);
}
TEST(SetupModel, ValidatesTlsTrustCombinationsAndReset) {
  auto f = valid();
  f.advanced[L"SSLCAFILE"] = L"ca.pem";
  EXPECT_NO_THROW(validate(f));
  f.advanced[L"SSLCADIR"] = L"certs";
  EXPECT_THROW(validate(f), std::invalid_argument);
  f.removed.insert(L"SSLCADIR");
  EXPECT_NO_THROW(validate(f));
  f.ssl = false;
  EXPECT_THROW(validate(f), std::invalid_argument);
  f.removed.insert(L"SSLCAFILE");
  EXPECT_NO_THROW(validate(f));
  f.removed.clear();
  f.advanced[L"SSLCAFILE"] = L"";
  f.advanced[L"SSLCADIR"] = L"";
  EXPECT_NO_THROW(validate(f));
}
TEST(SetupModel, ConvertsValidUtf8AndRejectsMalformedText) {
  EXPECT_EQ(from_utf8("caf\xc3\xa9"), L"caf\u00e9");
  const auto astral = from_utf8("\xf0\x9f\x98\x80");
  EXPECT_EQ(astral.size(), sizeof(wchar_t) == 2 ? 2u : 1u);
  for (const auto invalid : {"\x80", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"}) {
    EXPECT_THROW(from_utf8(invalid), std::invalid_argument);
  }
  std::wstring secret = L"temporary";
  secure_clear(secret);
  EXPECT_TRUE(secret.empty());
}
}
