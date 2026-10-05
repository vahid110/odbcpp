#include <gtest/gtest.h>
#include "core/database/generic_database_connection.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "odbcpp/transport/async_tls_transport.h"
#include "odbcpp/transport/thread_pool_transport.h"
#ifdef __linux__
#include "odbcpp/transport/epoll_transport.h"
#endif
#include <array>
#include <cstdlib>
#include <stdexcept>

namespace {
using namespace rs::core::database;
using namespace std::chrono_literals;

std::string required(const char* name) {
  const char* value = std::getenv(name);
  if (!value || !*value) throw std::runtime_error(std::string(name) + " is required");
  return value;
}

// Observe the real protocol without replacing authentication behavior.
class ObservedParser : public postgres::PgProtocolParser {
public:
  unsigned sasl = 0, final = 0;
  AuthenticationRequest parse_auth_request(const std::vector<std::byte>& bytes) override {
    auto request = PgProtocolParser::parse_auth_request(bytes);
    if (request.type == AuthenticationRequest::Type::SASL) ++sasl;
    if (request.type == AuthenticationRequest::Type::SASLFinal) ++final;
    return request;
  }
};

class AwsLcPostgresLive : public ::testing::TestWithParam<std::string> {
protected:
  ConnectionSettings settings;
  ObservedParser* parser = nullptr;
  std::unique_ptr<GenericDatabaseConnection> connection;
  void SetUp() override {
    settings.host = "127.0.0.1";
    const auto port = std::stoul(required("ODBCPP_AWSLC_PG_PORT"));
    ASSERT_GT(port, 0u);
    ASSERT_LE(port, 65535u);
    settings.port = static_cast<std::uint16_t>(port);
    settings.user = required("ODBCPP_AWSLC_PG_USER");
    settings.password = required("ODBCPP_AWSLC_PG_PASSWORD");
    settings.database = "postgres";
    settings.use_ssl = true;
    settings.ssl_ca_file = required("ODBCPP_AWSLC_PG_CA");
    settings.timeout = 5s;
    auto owned = std::make_unique<ObservedParser>();
    parser = owned.get();
    std::unique_ptr<rs::core::transport::ITransport> transport;
    using namespace rs::core::transport;
    if (GetParam() == "ThreadPool") {
      transport = std::make_unique<AsyncTlsTransport>(std::make_unique<ThreadPoolTransport>());
    }
#ifdef __linux__
    else if (GetParam() == "Epoll") {
      transport = std::make_unique<AsyncTlsTransport>(std::make_unique<EpollTransport>());
    }
#endif
    else {
      ASSERT_EQ(GetParam(), "Sync");
    }
    connection = std::make_unique<GenericDatabaseConnection>(std::move(owned), std::move(transport));
  }
  void TearDown() override { if (connection) connection->disconnect(); }

  void verify_query() {
    const auto result = connection->execute_query(
        "SELECT 42, NULL::text, ssl FROM pg_stat_ssl WHERE pid=pg_backend_pid()",
        rs::util::make_deadline(5s));
    ASSERT_TRUE(result.has_value()) << result.error_message();
    ASSERT_EQ(result->rows.size(), 1u);
    ASSERT_EQ(result->rows[0].size(), 3u);
    EXPECT_EQ(result->rows[0][0], "42");
    EXPECT_FALSE(result->rows[0][1].has_value());
    EXPECT_EQ(result->rows[0][2], "1");
    EXPECT_TRUE(result->cell_errors.empty());
    EXPECT_GT(parser->sasl, 0u) << "fixture must negotiate SCRAM, not trust/MD5";
    EXPECT_GT(parser->final, 0u) << "fixture must complete the SCRAM exchange";
  }
};

TEST_P(AwsLcPostgresLive, VerifiedScramAndPreparedQuery) {
  const auto connected = connection->connect(settings);
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  verify_query();
  const std::array<QueryParameter, 2> parameters{{
      {std::string("quoted ' value"), QueryParameterType::Text},
      {std::nullopt, QueryParameterType::Text}}};
  const auto result = connection->execute_prepared("SELECT ?::text, ?::text", parameters,
                                                  rs::util::make_deadline(5s));
  ASSERT_TRUE(result.has_value()) << result.error_message();
  ASSERT_EQ(result->rows.size(), 1u);
  ASSERT_EQ(result->rows[0].size(), 2u);
  EXPECT_EQ(result->rows[0][0], parameters[0].value);
  EXPECT_FALSE(result->rows[0][1].has_value());
  connection->disconnect();
  EXPECT_FALSE(connection->is_connected());
}

TEST_P(AwsLcPostgresLive, WrongPasswordRetiresSessionAndAllowsFreshScram) {
  auto rejected = settings;
  rejected.password += "-incorrect";
  const auto result = connection->connect(rejected);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.error(), rs::util::make_error_code(rs::util::DbErrorCode::AuthenticationFailed));
  EXPECT_FALSE(connection->is_connected());
  const auto connected = connection->connect(settings);
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  verify_query();
  EXPECT_EQ(parser->sasl, 2u);
}

TEST_P(AwsLcPostgresLive, RejectsWrongTrustBeforeAuthenticationAndRecovers) {
  auto rejected = settings;
  rejected.ssl_ca_file = required("ODBCPP_AWSLC_PG_WRONG_CA");
  const auto result = connection->connect(rejected);
  ASSERT_TRUE(result.has_error());
  EXPECT_NE(result.error_message().find("CERTIFICATE_VERIFY_FAILED"), std::string::npos)
      << result.error_message();
  EXPECT_FALSE(connection->is_connected());
  EXPECT_EQ(parser->sasl, 0u);
  const auto connected = connection->connect(settings);
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  verify_query();
}

TEST_P(AwsLcPostgresLive, RejectsHostnameBeforeAuthenticationAndRecovers) {
  auto rejected = settings;
  // The fixture certificate contains only IP:127.0.0.1, never DNS:localhost.
  rejected.host = "localhost";
  const auto result = connection->connect(rejected);
  ASSERT_TRUE(result.has_error());
  EXPECT_NE(result.error_message().find("hostname verification failed"), std::string::npos)
      << result.error_message();
  EXPECT_FALSE(connection->is_connected());
  EXPECT_EQ(parser->sasl, 0u);
  const auto connected = connection->connect(settings);
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  verify_query();
}
INSTANTIATE_TEST_SUITE_P(Transports, AwsLcPostgresLive,
    ::testing::Values(std::string("Sync"), std::string("ThreadPool")
#ifdef __linux__
                      , std::string("Epoll")
#endif
    ), [](const ::testing::TestParamInfo<std::string>& info) { return info.param; });
} // namespace
