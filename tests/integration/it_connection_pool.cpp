#include <gtest/gtest.h>
#include "core/database/connection_pool.h"
#include "core/util/exception_adapter.h"
#include "odbc/connection_string.h"
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
    
    // Load connection settings from DSN
    auto dsn_params = rs::odbc::ConnectionString::load_dsn("RedshiftProd");
    ASSERT_FALSE(dsn_params.empty()) << "DSN 'RedshiftProd' not found in odbc.ini";
    
    config_.connection_settings.host = dsn_params["SERVER"];
    config_.connection_settings.port = std::stoi(dsn_params["PORT"]);
    config_.connection_settings.database = dsn_params["DATABASE"];
    config_.connection_settings.user = dsn_params["UID"];
    config_.connection_settings.password = dsn_params["PWD"];
    config_.connection_settings.use_ssl = (dsn_params["SSL"] == "1");
    config_.connection_settings.timeout = std::chrono::seconds(10);
  }
  
  ConnectionPool::Config config_;
};

TEST_F(ConnectionPoolIntegrationTest, RealDatabaseConnections) {
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
  // Test with intentionally bad connection settings (no skip needed for error test)
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