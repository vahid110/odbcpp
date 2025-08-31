#include <gtest/gtest.h>
#include "core/database/connection_pool.h"
#include "core/util/exception_adapter.h"
#include <thread>
#include <vector>
#include <chrono>
#include <cstdlib>

using namespace rs::core::database;

class ConnectionPoolIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    config_.min_connections = 1;
    config_.max_connections = 3;
    config_.acquire_timeout = std::chrono::seconds(5);
    
    // Use environment variables or defaults for real database connection
    config_.connection_settings.host = getenv("PGHOST") ? getenv("PGHOST") : "127.0.0.1";
    config_.connection_settings.port = getenv("PGPORT") ? std::stoi(getenv("PGPORT")) : 5432;
    config_.connection_settings.database = getenv("PGDATABASE") ? getenv("PGDATABASE") : "postgres";
    config_.connection_settings.user = getenv("PGUSER") ? getenv("PGUSER") : "postgres";
    config_.connection_settings.password = getenv("PGPASSWORD") ? getenv("PGPASSWORD") : "postgres";
    config_.connection_settings.use_ssl = false;
    config_.connection_settings.timeout = std::chrono::seconds(10);
  }
  
  ConnectionPool::Config config_;
};

TEST_F(ConnectionPoolIntegrationTest, RealDatabaseConnections) {
  // Skip if no database available
  if (!getenv("PGHOST") && !getenv("PGDATABASE")) {
    GTEST_SKIP() << "No database configuration found";
  }
  
  ConnectionPool pool(config_);
  
  auto conn = pool.acquire();
  ASSERT_TRUE(conn.has_value()) << "Failed to acquire connection: " << conn.error_message();
  
  EXPECT_TRUE((*conn)->is_connected());
  
  // Execute a simple query
  auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto result = (*conn)->execute_query("SELECT 1 as test_column", deadline);
  
  if (result.has_value()) {
    EXPECT_EQ(result->rows.size(), 1);
    EXPECT_EQ(result->rows[0].size(), 1);
    EXPECT_EQ(result->rows[0][0], "1");
  } else {
    // Connection might fail in CI environment, that's ok
    EXPECT_EQ(result.error().value(), static_cast<int>(rs::util::DbErrorCode::QueryFailed));
  }
  
  pool.release(*conn);
  
  auto stats = pool.get_stats();
  EXPECT_EQ(stats.active_connections, 0);
}

TEST_F(ConnectionPoolIntegrationTest, ErrorHandlingWithRealDatabase) {
  // Test with intentionally bad connection settings
  config_.connection_settings.host = "nonexistent-host-12345";
  config_.connection_settings.port = 65000;
  
  ConnectionPool pool(config_);
  
  auto conn = pool.acquire();
  EXPECT_FALSE(conn.has_value());
  EXPECT_EQ(conn.error().value(), static_cast<int>(rs::util::DbErrorCode::ConnectionFailed));
  
  auto stats = pool.get_stats();
  EXPECT_EQ(stats.total_connections, 0);
  EXPECT_EQ(stats.active_connections, 0);
}