#pragma once
#include "i_database_connection.h"
#include "i_protocol_parser.h"
#include "core/transport/async_transport.h"
#include <future>
#include <functional>
#include <map>
#include <atomic>

namespace rs::core::database {

// Async database connection interface
class IAsyncDatabaseConnection : public IDatabaseConnection {
public:
  using ConnectCallback = std::function<void(rs::util::Result<void>)>;
  using QueryCallback = std::function<void(rs::util::Result<QueryResult>)>;
  
  // Async operations with callbacks
  virtual std::unique_ptr<rs::core::transport::AsyncOperation> connect_async(
    const ConnectionSettings& settings, ConnectCallback callback) = 0;
    
  virtual std::unique_ptr<rs::core::transport::AsyncOperation> execute_query_async(
    std::string_view sql, rs::util::Deadline deadline, QueryCallback callback) = 0;
    
  virtual std::unique_ptr<rs::core::transport::AsyncOperation> execute_prepared_async(
    std::string_view sql, std::span<const std::string> params,
    rs::util::Deadline deadline, QueryCallback callback) = 0;
  
  // Future-based interface
  virtual std::future<rs::util::Result<void>> connect_future(const ConnectionSettings& settings) = 0;
  virtual std::future<rs::util::Result<QueryResult>> execute_query_future(
    std::string_view sql, rs::util::Deadline deadline) = 0;
  virtual std::future<rs::util::Result<QueryResult>> execute_prepared_future(
    std::string_view sql, std::span<const std::string> params, rs::util::Deadline deadline) = 0;
};

// Async database connection implementation
class AsyncDatabaseConnection : public IAsyncDatabaseConnection {
public:
  AsyncDatabaseConnection(
    std::unique_ptr<IProtocolParser> parser,
    std::unique_ptr<rs::core::transport::IAsyncTransport> transport = nullptr);
  
  // Sync interface (inherited)
  rs::util::Result<void> connect(const ConnectionSettings& settings) override;
  void disconnect() override;
  bool is_connected() const override;
  
  rs::util::Result<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline) override;
  using IDatabaseConnection::execute_prepared;
  rs::util::Result<QueryResult> execute_prepared(std::string_view sql, 
                                                std::span<const QueryParameter> params,
                                                rs::util::Deadline deadline) override;
  
  std::string get_parameter(std::string_view key) const override;
  std::string get_last_error() const override;
  
  // Async interface
  std::unique_ptr<rs::core::transport::AsyncOperation> connect_async(
    const ConnectionSettings& settings, ConnectCallback callback) override;
    
  std::unique_ptr<rs::core::transport::AsyncOperation> execute_query_async(
    std::string_view sql, rs::util::Deadline deadline, QueryCallback callback) override;
    
  std::unique_ptr<rs::core::transport::AsyncOperation> execute_prepared_async(
    std::string_view sql, std::span<const std::string> params,
    rs::util::Deadline deadline, QueryCallback callback) override;
  
  // Future interface
  std::future<rs::util::Result<void>> connect_future(const ConnectionSettings& settings) override;
  std::future<rs::util::Result<QueryResult>> execute_query_future(
    std::string_view sql, rs::util::Deadline deadline) override;
  std::future<rs::util::Result<QueryResult>> execute_prepared_future(
    std::string_view sql, std::span<const std::string> params, rs::util::Deadline deadline) override;

private:
  std::unique_ptr<IProtocolParser> parser_;
  std::unique_ptr<rs::core::transport::IAsyncTransport> transport_;
  ConnectionSettings settings_;
  std::map<std::string, std::string> server_params_;
  std::string last_error_;
  std::atomic<bool> connected_{false};
  
  // Async helper methods
  void perform_async_authentication(rs::util::Deadline deadline, 
                                   std::function<void(rs::util::Result<void>)> callback);
  void execute_async_query_impl(std::string_view sql, rs::util::Deadline deadline,
                               QueryCallback callback, bool is_prepared = false,
                               std::span<const std::string> params = {});
};

} // namespace rs::core::database
