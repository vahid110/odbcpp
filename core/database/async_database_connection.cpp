#include "async_database_connection.h"
#include "core/transport/thread_pool_transport.h"
#include "mock_protocol_parser.h"

namespace rs::core::database {

AsyncDatabaseConnection::AsyncDatabaseConnection(
    std::unique_ptr<IProtocolParser> parser,
    std::unique_ptr<rs::core::transport::IAsyncTransport> transport)
  : parser_(std::move(parser)), transport_(std::move(transport)) {
  
  if (!transport_) {
    transport_ = std::make_unique<rs::core::transport::ThreadPoolTransport>();
  }
  
  // Create a mock parser if none provided (for testing)
  if (!parser_) {
    parser_ = std::make_unique<MockProtocolParser>();
  }
}

// Sync interface implementations (delegate to transport)
rs::util::Result<void> AsyncDatabaseConnection::connect(const ConnectionSettings& settings) {
  auto deadline = rs::util::make_deadline(settings.timeout);
  
  // 1. Connect to server
  auto connect_result = transport_->connect(settings.host, settings.port, deadline);
  if (connect_result.has_error()) {
    return connect_result;
  }
  
  try {
    // 2. Send startup message
    std::map<std::string, std::string> params;
    params["user"] = settings.user;
    params["database"] = settings.database;
    
    auto startup_msg = parser_->create_startup_message(settings.user, settings.database, params);
    auto send_result = transport_->send(startup_msg, deadline);
    if (send_result.has_error()) {
      return rs::util::Result<void>(rs::util::DbErrorCode::NetworkError, send_result.error_message());
    }
    
    // 3. Handle authentication
    std::vector<std::byte> buffer(8192);
    auto recv_result = transport_->recv(buffer, deadline);
    if (recv_result.has_error()) {
      return rs::util::Result<void>(rs::util::DbErrorCode::NetworkError, recv_result.error_message());
    }
    
    std::vector<std::byte> auth_data(buffer.begin(), buffer.begin() + recv_result->n);
    auto auth_msg = parser_->parse_message(auth_data);
    
    if (parser_->is_error_response(auth_msg)) {
      return rs::util::Result<void>(rs::util::DbErrorCode::AuthenticationFailed, parser_->extract_error_message(auth_msg));
    }
    
    auto auth_req = parser_->parse_auth_request(auth_msg.payload);
    if (auth_req.type != rs::core::database::AuthenticationRequest::Type::None) {
      auto auth_response = parser_->create_auth_response(auth_req, settings.password, settings.user);
      if (!auth_response.empty()) {
        auto auth_send = transport_->send(auth_response, deadline);
        if (auth_send.has_error()) {
          return rs::util::Result<void>(rs::util::DbErrorCode::NetworkError, auth_send.error_message());
        }
      }
    }
    
    // 4. Handle authentication response and server parameters
    auto auth_recv = transport_->recv(buffer, deadline);
    if (auth_recv.has_error()) {
      return rs::util::Result<void>(rs::util::DbErrorCode::NetworkError, auth_recv.error_message());
    }
    
    // Parse all messages in the response (multiple S, K, Z messages)
    std::vector<std::byte> all_data(buffer.begin(), buffer.begin() + auth_recv->n);
    size_t offset = 0;
    bool ready = false;
    
    while (offset < all_data.size() && !ready) {
      if (offset + 5 > all_data.size()) break; // Need at least tag + length
      
      char tag = static_cast<char>(all_data[offset]);
      uint32_t length = (static_cast<uint32_t>(all_data[offset+1]) << 24) |
                       (static_cast<uint32_t>(all_data[offset+2]) << 16) |
                       (static_cast<uint32_t>(all_data[offset+3]) << 8) |
                       static_cast<uint32_t>(all_data[offset+4]);
      
      if (offset + 1 + length > all_data.size()) break; // Incomplete message
      
      std::vector<std::byte> msg_data(all_data.begin() + offset, all_data.begin() + offset + 1 + length);
      auto msg = parser_->parse_message(msg_data);
      
      if (parser_->is_error_response(msg)) {
        return rs::util::Result<void>(rs::util::DbErrorCode::ConnectionFailed, parser_->extract_error_message(msg));
      }
      
      if (parser_->is_ready_for_query(msg)) {
        ready = true;
      }
      
      offset += 1 + length;
    }
    
    if (!ready) {
      return rs::util::Result<void>(rs::util::DbErrorCode::ProtocolError, "Did not receive ReadyForQuery message");
    }
    
    connected_.store(true);
    settings_ = settings;
    return rs::util::Result<void>();
    
  } catch (const std::exception& e) {
    return rs::util::Result<void>(rs::util::DbErrorCode::ProtocolError, e.what());
  }
}

void AsyncDatabaseConnection::disconnect() {
  transport_->close();
  connected_.store(false);
}

bool AsyncDatabaseConnection::is_connected() const {
  return connected_.load();
}

rs::util::Result<QueryResult> AsyncDatabaseConnection::execute_query(std::string_view sql, rs::util::Deadline deadline) {
  if (!is_connected()) {
    return rs::util::Result<QueryResult>(rs::util::DbErrorCode::NotConnected, "Not connected to database");
  }
  
  try {
    // Create query message using protocol parser
    auto query_msg = parser_->create_simple_query(sql);
    
    // Send query via transport
    auto send_result = transport_->send(query_msg, deadline);
    if (send_result.has_error()) {
      return rs::util::Result<QueryResult>(rs::util::DbErrorCode::NetworkError, send_result.error_message());
    }
    
    // Receive response messages
    std::vector<Message> messages;
    std::vector<std::byte> buffer(8192);
    
    auto recv_result = transport_->recv(buffer, deadline);
    if (recv_result.has_error()) {
      return rs::util::Result<QueryResult>(rs::util::DbErrorCode::NetworkError, recv_result.error_message());
    }
    
    // Parse all messages in the response
    std::vector<std::byte> all_data(buffer.begin(), buffer.begin() + recv_result->n);
    size_t offset = 0;
    bool ready = false;
    
    while (offset < all_data.size() && !ready) {
      if (offset + 5 > all_data.size()) break;
      
      char tag = static_cast<char>(all_data[offset]);
      uint32_t length = (static_cast<uint32_t>(all_data[offset+1]) << 24) |
                       (static_cast<uint32_t>(all_data[offset+2]) << 16) |
                       (static_cast<uint32_t>(all_data[offset+3]) << 8) |
                       static_cast<uint32_t>(all_data[offset+4]);
      
      if (offset + 1 + length > all_data.size()) break;
      
      std::vector<std::byte> msg_data(all_data.begin() + offset, all_data.begin() + offset + 1 + length);
      auto msg = parser_->parse_message(msg_data);
      messages.push_back(msg);
      
      if (parser_->is_error_response(msg)) {
        return rs::util::Result<QueryResult>(rs::util::DbErrorCode::QueryFailed, parser_->extract_error_message(msg));
      }
      
      if (parser_->is_ready_for_query(msg)) {
        ready = true;
      }
      
      offset += 1 + length;
    }
    
    // Extract results from messages
    QueryResult result;
    result.rows = parser_->extract_query_results(messages);
    
    return rs::util::Result<QueryResult>{std::move(result)};
    
  } catch (const std::exception& e) {
    return rs::util::Result<QueryResult>(rs::util::DbErrorCode::ProtocolError, e.what());
  }
}

rs::util::Result<QueryResult> AsyncDatabaseConnection::execute_prepared(std::string_view sql, 
                                                                       std::span<const QueryParameter> params,
                                                                       rs::util::Deadline deadline) {
  if (!is_connected()) {
    return rs::util::Result<QueryResult>(rs::util::DbErrorCode::NotConnected, "Not connected to database");
  }
  
  try {
    // Create prepared query message using Parse/Bind/Execute protocol
    auto prepared_msg = parser_->create_prepared_query(sql, params);
    
    // Send prepared query via transport
    auto send_result = transport_->send(prepared_msg, deadline);
    if (send_result.has_error()) {
      return rs::util::Result<QueryResult>(rs::util::DbErrorCode::NetworkError, send_result.error_message());
    }
    
    // Receive response messages
    std::vector<Message> messages;
    std::vector<std::byte> buffer(8192);
    
    auto recv_result = transport_->recv(buffer, deadline);
    if (recv_result.has_error()) {
      return rs::util::Result<QueryResult>(rs::util::DbErrorCode::NetworkError, recv_result.error_message());
    }
    
    // Parse all messages in the response
    std::vector<std::byte> all_data(buffer.begin(), buffer.begin() + recv_result->n);
    size_t offset = 0;
    bool ready = false;
    
    while (offset < all_data.size() && !ready) {
      if (offset + 5 > all_data.size()) break;
      
      char tag = static_cast<char>(all_data[offset]);
      uint32_t length = (static_cast<uint32_t>(all_data[offset+1]) << 24) |
                       (static_cast<uint32_t>(all_data[offset+2]) << 16) |
                       (static_cast<uint32_t>(all_data[offset+3]) << 8) |
                       static_cast<uint32_t>(all_data[offset+4]);
      
      if (offset + 1 + length > all_data.size()) break;
      
      std::vector<std::byte> msg_data(all_data.begin() + offset, all_data.begin() + offset + 1 + length);
      auto msg = parser_->parse_message(msg_data);
      messages.push_back(msg);
      
      if (parser_->is_error_response(msg)) {
        return rs::util::Result<QueryResult>(rs::util::DbErrorCode::QueryFailed, parser_->extract_error_message(msg));
      }
      
      if (parser_->is_ready_for_query(msg)) {
        ready = true;
      }
      
      offset += 1 + length;
    }
    
    // Extract results from messages
    QueryResult result;
    result.rows = parser_->extract_query_results(messages);
    
    return rs::util::Result<QueryResult>{std::move(result)};
    
  } catch (const std::exception& e) {
    return rs::util::Result<QueryResult>(rs::util::DbErrorCode::ProtocolError, e.what());
  }
}

std::string AsyncDatabaseConnection::get_parameter(std::string_view key) const {
  auto it = server_params_.find(std::string(key));
  return (it != server_params_.end()) ? it->second : std::string{};
}

std::string AsyncDatabaseConnection::get_last_error() const {
  return last_error_;
}

// Async interface implementations
std::unique_ptr<rs::core::transport::AsyncOperation> AsyncDatabaseConnection::connect_async(
    const ConnectionSettings& settings, ConnectCallback callback) {
  
  settings_ = settings;
  auto deadline = rs::util::make_deadline(settings.timeout);
  
  return transport_->connect_async(settings.host, settings.port, deadline,
    [this, callback](rs::util::Result<void> result) {
      if (result.has_value()) {
        connected_.store(true);
      }
      callback(std::move(result));
    });
}

std::unique_ptr<rs::core::transport::AsyncOperation> AsyncDatabaseConnection::execute_query_async(
    std::string_view sql, rs::util::Deadline deadline, QueryCallback callback) {
  
  execute_async_query_impl(sql, deadline, callback, false);
  return nullptr; // Simplified - would return actual operation
}

std::unique_ptr<rs::core::transport::AsyncOperation> AsyncDatabaseConnection::execute_prepared_async(
    std::string_view sql, std::span<const std::string> params,
    rs::util::Deadline deadline, QueryCallback callback) {
  
  execute_async_query_impl(sql, deadline, callback, true, params);
  return nullptr; // Simplified - would return actual operation
}

// Future interface implementations
std::future<rs::util::Result<void>> AsyncDatabaseConnection::connect_future(const ConnectionSettings& settings) {
  auto promise = std::make_shared<std::promise<rs::util::Result<void>>>();
  auto future = promise->get_future();
  
  connect_async(settings, [promise](rs::util::Result<void> result) {
    promise->set_value(std::move(result));
  });
  
  return future;
}

std::future<rs::util::Result<QueryResult>> AsyncDatabaseConnection::execute_query_future(
    std::string_view sql, rs::util::Deadline deadline) {
  
  auto promise = std::make_shared<std::promise<rs::util::Result<QueryResult>>>();
  auto future = promise->get_future();
  
  execute_query_async(sql, deadline, [promise](rs::util::Result<QueryResult> result) {
    promise->set_value(std::move(result));
  });
  
  return future;
}

std::future<rs::util::Result<QueryResult>> AsyncDatabaseConnection::execute_prepared_future(
    std::string_view sql, std::span<const std::string> params, rs::util::Deadline deadline) {
  
  auto promise = std::make_shared<std::promise<rs::util::Result<QueryResult>>>();
  auto future = promise->get_future();
  
  execute_prepared_async(sql, params, deadline, [promise](rs::util::Result<QueryResult> result) {
    promise->set_value(std::move(result));
  });
  
  return future;
}

// Private helper methods
void AsyncDatabaseConnection::execute_async_query_impl(std::string_view sql, rs::util::Deadline deadline,
                                                     QueryCallback callback, bool is_prepared,
                                                     std::span<const std::string> params) {
  
  // Simplified async query execution
  // In production, this would:
  // 1. Send query message via async transport
  // 2. Receive response messages
  // 3. Parse results using protocol parser
  // 4. Call callback with results
  
  std::thread([sql = std::string(sql), callback]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Simulate async work
    
    QueryResult result;
    result.rows = {{std::string(sql) + "_result"}};
    callback(rs::util::Result<QueryResult>{std::move(result)});
  }).detach();
}

} // namespace rs::core::database
