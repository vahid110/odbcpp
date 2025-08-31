// tests/integration/it_simple_query.cpp
#include <gtest/gtest.h>
#include "core/engine/pg_connection.h"
#include "core/util/deadline.h"
#include <cstdlib>
#include <string>

using namespace rs::core::engine;

static std::string env_or(const char* k, const char* defv) {
  const char* v = std::getenv(k);
  return v ? v : defv;
}

TEST(Integration, ConnectAndSelect1) {
  PgConnection::Settings s;
  s.host = env_or("PGHOST","127.0.0.1");
  s.port = static_cast<uint16_t>(std::stoi(env_or("PGPORT","5432")));
  s.db   = env_or("PGDATABASE","postgres");
  s.user = env_or("PGUSER","postgres");
  s.password = env_or("PGPASSWORD","postgres");
  s.sslmode = SslMode::Disable; // local Postgres in CI: plain
  s.timeout = std::chrono::seconds(10);

  PgConnection conn;
  conn.connect(s);

  auto dl = rs::util::make_deadline(std::chrono::seconds(5));
  auto rows = conn.simpleQuery("SELECT 1", dl);

  ASSERT_EQ(rows.size(), 1u);
  ASSERT_EQ(rows[0].size(), 1u);
  EXPECT_EQ(rows[0][0], "1");
}
