// tests/integration/it_simple_query.cpp
#include <gtest/gtest.h>
#include "core/database/async_database_connection.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/thread_pool_transport.h"
#include "core/util/deadline.h"
#include "core/util/exception_adapter.h"
#include <cstdlib>
#include <string>

using namespace rs::core::database;

static std::string env_or(const char* k, const char* defv) {
  const char* v = std::getenv(k);
  return v ? v : defv;
}

TEST(Integration, ConnectAndSelect1) {
  auto transport = std::make_unique<rs::core::transport::ThreadPoolTransport>(4);
  auto parser = std::make_unique<rs::core::database::postgres::PgProtocolParser>();
  auto conn = std::make_unique<rs::core::database::AsyncDatabaseConnection>(std::move(parser), std::move(transport));
  
  ConnectionSettings settings;
  settings.host = "vahidsbr-redshift-cluster.cxzokcavspmr.us-east-1.redshift.amazonaws.com";
  settings.port = 5439;
  settings.database = "dev";
  settings.user = "awsuser";
  settings.password = "Testing1234";
  settings.use_ssl = false;
  settings.timeout = std::chrono::seconds(10);

  rs::util::unwrap_or_throw(conn->connect(settings));

  auto dl = rs::util::make_deadline(std::chrono::seconds(5));
  auto result = rs::util::unwrap_or_throw(conn->execute_query("SELECT 1", dl));

  ASSERT_EQ(result.rows.size(), 1u);
  ASSERT_EQ(result.rows[0].size(), 1u);
  EXPECT_EQ(result.rows[0][0], "1");
}
