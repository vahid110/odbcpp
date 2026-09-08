#include <gtest/gtest.h>

#include "core/transport/socket_transport.h"
#include "core/transport/async_tls_transport.h"
#include "core/transport/tls_transport.h"
#include "core/transport/transport_factory.h"
#include "core/transport/transport_options.h"
#ifdef __linux__
#include "core/transport/epoll_transport.h"
#elif defined(_WIN32)
#include "core/transport/iocp_transport.h"
#endif
#include "odbc/connection_string.h"
#include "odbc/odbc_handles.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace {

using rs::core::transport::AsyncEngine;
using rs::core::transport::DeadlineModel;
using rs::core::transport::SocketTransport;
using rs::core::transport::TLSTransport;
using rs::core::transport::TransportFactory;
using rs::core::transport::TransportMode;
using rs::core::transport::TransportOptions;

TEST(TransportOptionsTest, UsesDocumentedDefaults) {
  const auto options = TransportOptions::resolve({}, {}, {});

  EXPECT_EQ(options.mode, TransportMode::Auto);
  EXPECT_EQ(options.async_max_inflight, 64u);
  EXPECT_EQ(options.async_queue_depth, 256u);
  EXPECT_EQ(options.async_engine, AsyncEngine::Auto);
  EXPECT_EQ(options.deadline_model, DeadlineModel::Strict);
}

TEST(TransportOptionsTest, AppliesDriverThenDsnThenConnectionPrecedence) {
  const std::map<std::string, std::string> driver{
      {"TransportMode", "Sync"},
      {"AsyncMaxInflight", "16"},
      {"AsyncQueueDepth", "128"},
      {"AsyncEngine", "IOCP"},
      {"DeadlineModel", "SocketTimeout"},
  };
  const std::map<std::string, std::string> dsn{
      {"TRANSPORTMODE", "Async"},
      {"ASYNCMAXINFLIGHT", "32"},
      {"ASYNCENGINE", "Epoll"},
  };
  const std::map<std::string, std::string> connection{
      {"transportmode", "sync"},
      {"asyncqueuedepth", "512"},
      {"deadlinemodel", "strict"},
  };

  const auto options = TransportOptions::resolve(driver, dsn, connection);
  EXPECT_EQ(options.mode, TransportMode::Sync);
  EXPECT_EQ(options.async_max_inflight, 32u);
  EXPECT_EQ(options.async_queue_depth, 512u);
  EXPECT_EQ(options.async_engine, AsyncEngine::Epoll);
  EXPECT_EQ(options.deadline_model, DeadlineModel::Strict);
}

TEST(TransportOptionsTest, RejectsInvalidKnownValues) {
  EXPECT_THROW(
      TransportOptions::resolve({}, {}, {{"TransportMode", "Sometimes"}}),
      std::invalid_argument);
  EXPECT_THROW(
      TransportOptions::resolve({}, {}, {{"AsyncQueueDepth", "0"}}),
      std::invalid_argument);
  EXPECT_THROW(
      TransportOptions::resolve(
          {}, {}, {{"AsyncMaxInflight", "257"}, {"AsyncQueueDepth", "256"}}),
      std::invalid_argument);
}

TEST(TransportFactoryTest, AutoUsesSafeSyncTransportUntilNativeAsyncExists) {
  TransportOptions options;
  auto transport = TransportFactory::create(options, false);

  EXPECT_EQ(TransportFactory::resolve_mode(options), TransportMode::Sync);
  auto* socket = dynamic_cast<SocketTransport*>(transport.get());
  ASSERT_NE(socket, nullptr);
  EXPECT_EQ(socket->deadline_model(), DeadlineModel::Strict);
}

TEST(TransportFactoryTest, CreatesTlsWithSelectedDeadlineModel) {
  TransportOptions options;
  options.mode = TransportMode::Sync;
  options.deadline_model = DeadlineModel::SocketTimeout;
  auto transport = TransportFactory::create(options, true);

  auto* tls = dynamic_cast<TLSTransport*>(transport.get());
  ASSERT_NE(tls, nullptr);
  EXPECT_EQ(tls->deadline_model(), DeadlineModel::SocketTimeout);
}

TEST(TransportFactoryTest, CreatesAvailablePlatformAsyncBackend) {
  TransportOptions options;
  options.mode = TransportMode::Async;
#ifdef __linux__
  auto transport = TransportFactory::create(options, false);
  auto* epoll = dynamic_cast<rs::core::transport::EpollTransport*>(
      transport.get());
  ASSERT_NE(epoll, nullptr);
  EXPECT_EQ(epoll->max_inflight(), 64u);
  EXPECT_EQ(epoll->queue_depth(), 256u);
#elif defined(_WIN32)
  auto transport = TransportFactory::create(options, false);
  auto* iocp = dynamic_cast<rs::core::transport::IocpTransport*>(
      transport.get());
  ASSERT_NE(iocp, nullptr);
  EXPECT_EQ(iocp->max_inflight(), 64u);
  EXPECT_EQ(iocp->queue_depth(), 256u);
#else
  EXPECT_THROW(TransportFactory::create(options, false), std::invalid_argument);
#endif
}

TEST(TransportFactoryTest, WrapsPlatformAsyncBackendWithTls) {
  TransportOptions options;
  options.mode = TransportMode::Async;
#if defined(__linux__) || defined(_WIN32)
  auto transport = TransportFactory::create(options, true);
  auto* tls = dynamic_cast<rs::core::transport::AsyncTlsTransport*>(
      transport.get());
  ASSERT_NE(tls, nullptr);
  EXPECT_EQ(tls->max_inflight(), 64u);
  EXPECT_EQ(tls->queue_depth(), 256u);
#else
  EXPECT_THROW(TransportFactory::create(options, true),
               std::invalid_argument);
#endif
}

TEST(ODBCTransportSelectionTest, ReportsUnavailablePlatformAsyncEngine) {
  rs::odbc::ODBCConnection connection(nullptr);
#ifdef __linux__
  const auto result = connection.connect(
      "SERVER=127.0.0.1;PORT=1;TransportMode=Async;AsyncEngine=IOCP",
      "", "");
#else
  const auto result = connection.connect(
      "SERVER=127.0.0.1;PORT=1;TransportMode=Async;AsyncEngine=Epoll",
      "", "");
#endif

  EXPECT_EQ(result, SQL_ERROR);
  EXPECT_NE(connection.get_error_message().find("AsyncEngine"),
            std::string::npos);
}

class ConnectionResolutionTest : public ::testing::Test {
protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
        ("odbcpp-transport-options-" + std::to_string(std::rand()));
    std::filesystem::create_directories(directory_);

    std::ofstream dsn_file(directory_ / "odbc.ini");
    dsn_file << "[TransportOptionsTest]\n"
                "Driver=ODBCPP Test Driver\n"
                "Server=dsn.example\n"
                "TransportMode=Async\n"
                "DeadlineModel=Strict\n";

    std::ofstream driver_file(directory_ / "odbcinst.ini");
    driver_file << "[ODBCPP Test Driver]\n"
                   "TransportMode=Sync\n"
                   "AsyncMaxInflight=12\n"
                   "AsyncQueueDepth=120\n"
                   "DeadlineModel=SocketTimeout\n";

#ifdef _WIN32
    _putenv_s("ODBCSYSINI", directory_.string().c_str());
#else
    setenv("ODBCSYSINI", directory_.string().c_str(), 1);
#endif
  }

  void TearDown() override {
#ifdef _WIN32
    _putenv_s("ODBCSYSINI", "");
#else
    unsetenv("ODBCSYSINI");
#endif
    std::filesystem::remove_all(directory_);
  }

  std::filesystem::path directory_;
};

TEST_F(ConnectionResolutionTest, PreservesLayersAndConnectionOverridesDsn) {
  const auto resolved = rs::odbc::ConnectionString::resolve(
      "DSN=TransportOptionsTest;Server=connection.example;TransportMode=Sync",
      "Unused Default");

  ASSERT_EQ(resolved.driver_name, "ODBCPP Test Driver");
  EXPECT_EQ(resolved.driver_parameters.at("TRANSPORTMODE"), "Sync");
  EXPECT_EQ(resolved.dsn_parameters.at("TRANSPORTMODE"), "Async");
  EXPECT_EQ(resolved.connection_parameters.at("TRANSPORTMODE"), "Sync");
  EXPECT_EQ(resolved.effective_parameters.at("SERVER"), "connection.example");

  const auto options = TransportOptions::resolve(
      resolved.driver_parameters, resolved.dsn_parameters,
      resolved.connection_parameters);
  EXPECT_EQ(options.mode, TransportMode::Sync);
  EXPECT_EQ(options.async_max_inflight, 12u);
  EXPECT_EQ(options.async_queue_depth, 120u);
  EXPECT_EQ(options.deadline_model, DeadlineModel::Strict);
}

} // namespace
