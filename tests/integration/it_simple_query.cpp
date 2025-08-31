// tests/integration/it_simple_query.cpp
#include <gtest/gtest.h>
#include "core/database/database_factory.h"
#include "core/util/deadline.h"
#include <cstdlib>
#include <string>

using namespace rs::core::database;

static std::string env_or(const char* k, const char* defv) {
  const char* v = std::getenv(k);
  return v ? v : defv;
}

TEST(Integration, ConnectAndSelect1) {
  auto conn = DatabaseFactory::create_connection();
  
  ConnectionSettings settings;
  settings.host = env_or("PGHOST","127.0.0.1");
  settings.port = static_cast<uint16_t>(std::stoi(env_or("PGPORT","5432")));
  settings.database = env_or("PGDATABASE","postgres");
  settings.user = env_or("PGUSER","postgres");
  settings.password = env_or("PGPASSWORD","postgres");
  settings.use_ssl = false; // local Postgres in CI: plain
  settings.timeout = std::chrono::seconds(10);

  conn->connect(settings);

  auto dl = rs::util::make_deadline(std::chrono::seconds(5));
  auto result = conn->execute_query("SELECT 1", dl);

  ASSERT_EQ(result.rows.size(), 1u);
  ASSERT_EQ(result.rows[0].size(), 1u);
  EXPECT_EQ(result.rows[0][0], "1");
}
