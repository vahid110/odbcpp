#include <gtest/gtest.h>
#include "sdk/tests/session_baseline/runner.h"
#include "core/database/backend_provider.h"
#include "core/transport/i_transport.h"
#include "odbc/connection_string.h"
#include "tests/test_connection_config.h"

TEST(SessionBaselineIntegrationTest, MandatoryPostgreSQLUsesIsolatedSharedRunner) {
  using namespace rs::core::database;
  const auto& provider = configured_backend_provider();
  const auto params = rs::odbc::ConnectionString::resolve(
      odbcpp::test::configured_connection_string(), provider.identity().driver_name).effective_parameters;
  ConnectionOptions options;
  if (params.count("SERVER")) options.host = params.at("SERVER");
  if (params.count("PORT")) options.port = static_cast<std::uint16_t>(std::stoul(params.at("PORT")));
  if (params.count("DATABASE")) options.database = params.at("DATABASE");
  if (params.count("UID")) options.user = params.at("UID");
  if (params.count("PWD")) options.password = params.at("PWD");
  ASSERT_TRUE(params.count("SSL"));
  ASSERT_TRUE(params.at("SSL") == "0" || params.at("SSL") == "1");
  options.use_ssl = params.at("SSL") == "1";
  if (params.count("SSLCAFILE")) options.ssl_ca_file = params.at("SSLCAFILE");
  if (params.count("SSLCADIR")) options.ssl_ca_dir = params.at("SSLCADIR");
  auto settings = provider.resolve_connection_options(std::move(options));
  ASSERT_TRUE(settings) << "Mandatory baseline fixture configuration failed";
  odbcpp::test::SessionBaselineFixture fixture{
      "SELECT 'baseline'::varchar, NULL::varchar, ''::varchar",
      "SELECT $1::varchar, $2::varchar, $3::varchar", "SELECT 1 / 0",
      {{std::string("baseline"), QueryParameterType::Text}, {std::nullopt, QueryParameterType::Text},
       {std::string{}, QueryParameterType::Text}},
      {std::string("baseline"), std::nullopt, std::string{}},
      {ScalarType::VarChar, ScalarType::VarChar, ScalarType::VarChar}};
  EXPECT_EQ(std::string_view{}, odbcpp::test::run_session_baseline(
      [&provider] { return provider.create_session(nullptr); }, *settings, fixture));
}
