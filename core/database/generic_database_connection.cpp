#include "generic_database_connection.h"
#include "core/transport/socket_transport.h"
#include "core/transport/start_tls_transport.h"
#include "core/transport/tls_transport.h"
#include "core/util/exception_adapter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>

namespace rs::core::database {
namespace {

constexpr std::uint32_t kShortFrameLimit = 30000;
constexpr std::uint32_t kLargeFrameLimit = 0x3fffffff;

bool allows_large_frame(char tag) {
  switch (tag) {
    case 'd': case 'D': case 'E': case 'V':
    case 'N': case 'A': case 'T': case 't':
      return true;
    default:
      return false;
  }
}

} // namespace

GenericDatabaseConnection::GenericDatabaseConnection(
    std::unique_ptr<IProtocolParser> parser,
    std::unique_ptr<rs::core::transport::ITransport> transport)
  : parser_(std::move(parser)), transport_(std::move(transport)) {}

rs::util::Result<void> GenericDatabaseConnection::connect(const ConnectionSettings& settings) {
  settings_ = settings;
  server_params_.clear();
  last_server_sqlstate_.clear();
  
  if (!transport_) {
    if (settings.use_ssl) {
      auto tls = std::make_unique<rs::core::transport::TLSTransport>();
      tls->set_verify(true);
      transport_ = std::move(tls);
    } else {
      transport_ = std::make_unique<rs::core::transport::SocketTransport>();
    }
  }
  
  auto deadline = rs::util::make_deadline(settings.timeout);
  
  if (settings.use_ssl) {
    // PostgreSQL/Redshift starts in plain text, sends an SSLRequest, then
    // upgrades the same transport connection in place.
    auto* start_tls = dynamic_cast<
        rs::core::transport::IStartTlsTransport*>(transport_.get());
    if (start_tls == nullptr) {
      return rs::util::Result<void>{
          rs::util::DbErrorCode::InvalidParameter,
          "selected transport does not support PostgreSQL TLS upgrade"};
    }

    auto connect_result = start_tls->connect_plain(
        settings.host, settings.port, deadline);
    if (connect_result.has_error()) {
      const auto code = connect_result.error() ==
              rs::util::make_error_code(rs::util::DbErrorCode::Timeout)
          ? rs::util::DbErrorCode::Timeout
          : rs::util::DbErrorCode::ConnectionFailed;
      return rs::util::Result<void>{
          code, connect_result.error_message()};
    }
    
    // Send SSL request
    auto ssl_req = parser_->create_ssl_request();
    auto write_result = write_message_to_transport_result(
        *transport_, ssl_req, deadline);
    if (write_result.has_error()) {
      return write_result;
    }
    
    // Read SSL response
    std::vector<std::byte> response(1);
    auto recv_result = transport_->recv(response, deadline);
    if (recv_result.has_error()) {
      return rs::util::Result<void>{
          recv_result.error(), recv_result.error_message()};
    }
    
    if (recv_result->n != 1 || response[0] != std::byte{'S'}) {
      return rs::util::Result<void>{rs::util::DbErrorCode::TLSError, "SSL not supported by server"};
    }
    
    auto upgrade_result = start_tls->upgrade_to_tls(settings.host, deadline);
    if (upgrade_result.has_error()) return upgrade_result;
  } else {
    auto connect_result = transport_->connect(settings.host, settings.port, deadline);
    if (connect_result.has_error()) {
      const auto code = connect_result.error() ==
              rs::util::make_error_code(rs::util::DbErrorCode::Timeout)
          ? rs::util::DbErrorCode::Timeout
          : rs::util::DbErrorCode::ConnectionFailed;
      return rs::util::Result<void>{
          code, connect_result.error_message()};
    }
  }
  
  // Send startup message
  std::map<std::string, std::string> params;
  params["application_name"] = "odbcpp";
  auto startup = parser_->create_startup_message(settings.user, settings.database, params);
  auto write_result = write_all_result(startup, deadline);
  if (write_result.has_error()) {
    return write_result;
  }
  
  // Handle authentication
  auto auth_result = perform_authentication_result(deadline);
  if (auth_result.has_error()) {
    return auth_result;
  }
  
  connected_ = true;
  return rs::util::Result<void>{};
}

void GenericDatabaseConnection::disconnect() {
  if (transport_) {
    transport_->close();
  }
  connected_ = false;
  server_params_.clear();
  last_server_sqlstate_.clear();
}

bool GenericDatabaseConnection::is_connected() const {
  return connected_;
}

void GenericDatabaseConnection::mark_transport_failed() noexcept {
  connected_ = false;
}

rs::util::Result<QueryResult> GenericDatabaseConnection::execute_query(std::string_view sql, rs::util::Deadline deadline) {
  if (!connected_) {
    return rs::util::Result<QueryResult>{rs::util::DbErrorCode::NotConnected, "Not connected"};
  }
  
  std::vector<std::byte> query_msg;
  try {
    query_msg = parser_->create_simple_query(sql);
  } catch (const std::exception& error) {
    return rs::util::Result<QueryResult>{
        rs::util::DbErrorCode::InvalidParameter, error.what()};
  }
  auto write_result = write_all_result(query_msg, deadline);
  if (write_result.has_error()) {
    return rs::util::Result<QueryResult>{write_result.error(), write_result.error_message()};
  }

  return read_query_result(deadline);
}

rs::util::Result<QueryResult> GenericDatabaseConnection::execute_prepared(std::string_view sql, 
                                                                            std::span<const QueryParameter> params,
                                                                            rs::util::Deadline deadline) {
  if (!connected_) {
    return rs::util::Result<QueryResult>{rs::util::DbErrorCode::NotConnected, "Not connected"};
  }
  
  std::vector<std::byte> query_msg;
  try {
    query_msg = parser_->create_prepared_query(sql, params);
  } catch (const std::exception& error) {
    return rs::util::Result<QueryResult>{
        rs::util::DbErrorCode::InvalidParameter, error.what()};
  }
  auto write_result = write_all_result(query_msg, deadline);
  if (write_result.has_error()) {
    return rs::util::Result<QueryResult>{write_result.error(), write_result.error_message()};
  }
  
  return read_query_result(deadline);
}

rs::util::Result<QueryResult> GenericDatabaseConnection::describe_statement(
    std::string_view sql,
    std::span<const QueryParameterType> parameter_types,
    rs::util::Deadline deadline) {
  if (!connected_) {
    return rs::util::Result<QueryResult>{
        rs::util::DbErrorCode::NotConnected, "Not connected"};
  }

  std::vector<std::byte> request;
  try {
    request = parser_->create_statement_description(sql, parameter_types);
  } catch (const std::exception& error) {
    return rs::util::Result<QueryResult>{
        rs::util::DbErrorCode::InvalidParameter, error.what()};
  }
  auto write_result = write_all_result(request, deadline);
  if (write_result.has_error()) {
    return rs::util::Result<QueryResult>{
        write_result.error(), write_result.error_message()};
  }
  return read_query_result(deadline);
}

rs::util::Result<QueryResult> GenericDatabaseConnection::read_query_result(
    rs::util::Deadline deadline) {
  std::vector<Message> messages;
  std::optional<std::string> query_error;
  std::string query_error_sqlstate;
  last_server_sqlstate_.clear();

  while (true) {
    auto msg_result = read_message_result(deadline);
    if (msg_result.has_error()) {
      return rs::util::Result<QueryResult>{msg_result.error(), msg_result.error_message()};
    }
    
    try {
      auto msg = parser_->parse_message(*msg_result);
      if (msg.tag == 'S') {
        auto status_result = record_parameter_status(msg);
        if (status_result.has_error()) {
          mark_transport_failed();
          return rs::util::Result<QueryResult>{
              status_result.error(), status_result.error_message()};
        }
      }
      if (parser_->is_error_response(msg) && !query_error) {
        query_error = parser_->extract_error_message(msg);
        query_error_sqlstate = parser_->extract_error_sqlstate(msg);
      }
      messages.push_back(msg);
      if (parser_->is_ready_for_query(msg)) break;
    } catch (const std::exception& error) {
      mark_transport_failed();
      return rs::util::Result<QueryResult>{
          rs::util::DbErrorCode::ProtocolError, error.what()};
    }
  }

  try {
    auto result = parser_->extract_query_result(messages);
    if (query_error &&
        (!result.error_message.empty() || result.additional_results.empty())) {
      last_error_ = *query_error;
      last_server_sqlstate_ = std::move(query_error_sqlstate);
      return rs::util::Result<QueryResult>{
          rs::util::DbErrorCode::QueryFailed, "Query error: " + last_error_};
    }
    return rs::util::Result<QueryResult>{std::move(result)};
  } catch (const std::exception& error) {
    mark_transport_failed();
    return rs::util::Result<QueryResult>{
        rs::util::DbErrorCode::ProtocolError, error.what()};
  }
}

std::string GenericDatabaseConnection::get_parameter(std::string_view key) const {
  auto it = server_params_.find(std::string(key));
  return (it != server_params_.end()) ? it->second : std::string{};
}

std::string GenericDatabaseConnection::get_last_error() const {
  return last_error_;
}

std::string GenericDatabaseConnection::get_last_server_sqlstate() const {
  return last_server_sqlstate_;
}

void GenericDatabaseConnection::write_all(const std::vector<std::byte>& data, rs::util::Deadline deadline) {
  auto result = write_all_result(data, deadline);
  if (result.has_error()) {
    rs::util::unwrap_or_throw(std::move(result));
  }
}

rs::util::Result<void> GenericDatabaseConnection::write_all_result(const std::vector<std::byte>& data, rs::util::Deadline deadline) {
  size_t offset = 0;
  while (offset < data.size()) {
    auto result = transport_->send(std::span<const std::byte>(data.data() + offset, data.size() - offset), deadline);
    if (result.has_error()) {
      mark_transport_failed();
      return rs::util::Result<void>{result.error(), result.error_message()};
    }
    if (result->n == 0) {
      mark_transport_failed();
      return rs::util::Result<void>{rs::util::DbErrorCode::NetworkError, "Write failed"};
    }
    offset += result->n;
  }
  return rs::util::Result<void>{};
}

std::vector<std::byte> GenericDatabaseConnection::read_message(rs::util::Deadline deadline) {
  auto result = read_message_result(deadline);
  if (result.has_error()) {
    rs::util::unwrap_or_throw(std::move(result));
  }
  return std::move(*result);
}

rs::util::Result<std::vector<std::byte>> GenericDatabaseConnection::read_message_result(rs::util::Deadline deadline) {
  // Read message header (1 byte tag + 4 bytes length)
  std::vector<std::byte> header(5);
  size_t offset = 0;
  
  while (offset < 5) {
    auto result = transport_->recv(std::span<std::byte>(header.data() + offset, 5 - offset), deadline);
    if (result.has_error()) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          result.error(), result.error_message()};
    }
    if (result->eof) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{rs::util::DbErrorCode::NetworkError, "Unexpected EOF"};
    }
    if (result->n == 0) continue;
    offset += result->n;
  }
  
  // Extract length
  const auto* p = reinterpret_cast<const unsigned char*>(header.data());
  const std::uint32_t len = (static_cast<std::uint32_t>(p[1]) << 24) |
      (static_cast<std::uint32_t>(p[2]) << 16) |
      (static_cast<std::uint32_t>(p[3]) << 8) | p[4];
  
  const auto limit = allows_large_frame(static_cast<char>(header[0]))
      ? kLargeFrameLimit : kShortFrameLimit;
  if (len < 4 || len > limit) {
    mark_transport_failed();
    return rs::util::Result<std::vector<std::byte>>{rs::util::DbErrorCode::ProtocolError, "Invalid message length"};
  }
  
  // Grow only as bytes arrive; an untrusted length must not preallocate it.
  const auto total_length = static_cast<std::size_t>(len) + 1;
  std::vector<std::byte> message;
  message.reserve(std::min<std::size_t>(total_length, 8192));
  message.insert(message.end(), header.begin(), header.end());

  while (message.size() < total_length) {
    const auto offset = message.size();
    const auto requested = std::min<std::size_t>(total_length - offset, 8192);
    message.resize(offset + requested);
    auto result = transport_->recv(
        std::span<std::byte>(message.data() + offset, requested), deadline);
    if (result.has_error()) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          result.error(), result.error_message()};
    }
    if (result->eof) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{rs::util::DbErrorCode::NetworkError, "Unexpected EOF"};
    }
    if (result->n > requested) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          rs::util::DbErrorCode::ProtocolError,
          "Transport read exceeded requested message bytes"};
    }
    message.resize(offset + result->n);
  }
  
  return rs::util::Result<std::vector<std::byte>>{std::move(message)};
}

void GenericDatabaseConnection::perform_authentication(rs::util::Deadline deadline) {
  auto result = perform_authentication_result(deadline);
  if (result.has_error()) {
    rs::util::unwrap_or_throw(std::move(result));
  }
}

rs::util::Result<void> GenericDatabaseConnection::perform_authentication_result(rs::util::Deadline deadline) {
  bool authenticated = false;
  try {
    while (true) {
      auto msg_result = read_message_result(deadline);
      if (msg_result.has_error()) {
        return rs::util::Result<void>{msg_result.error(), msg_result.error_message()};
      }
    
      auto msg = parser_->parse_message(*msg_result);
    
      if (msg.tag == 'R') { // Authentication
        if (authenticated) {
          return rs::util::Result<void>{
              rs::util::DbErrorCode::ProtocolError,
              "Authentication request arrived after AuthenticationOk"};
        }
        auto auth_req = parser_->parse_auth_request(msg.payload);
      
        if (auth_req.type == AuthenticationRequest::Type::None) {
          authenticated = true;
          continue; // Authentication successful
        }
      
        auto auth_response = parser_->create_auth_response(
            auth_req, settings_.password, settings_.user);
        if (!auth_response.empty()) {
          auto write_result = write_all_result(auth_response, deadline);
          if (write_result.has_error()) {
            return write_result;
          }
        }
      }
      else if (msg.tag == 'S') { // ParameterStatus
        if (!authenticated) {
          return rs::util::Result<void>{
              rs::util::DbErrorCode::ProtocolError,
              "ParameterStatus arrived before AuthenticationOk"};
        }
        auto status_result = record_parameter_status(msg);
        if (status_result.has_error()) return status_result;
      }
      else if (msg.tag == 'K') { // BackendKeyData
        if (!authenticated) {
          return rs::util::Result<void>{
              rs::util::DbErrorCode::ProtocolError,
              "BackendKeyData arrived before AuthenticationOk"};
        }
      }
      else if (parser_->is_error_response(msg)) {
        last_error_ = parser_->extract_error_message(msg);
        const auto sqlstate = parser_->extract_error_sqlstate(msg);
        const bool authentication_error =
            !authenticated || sqlstate.starts_with("28");
        return rs::util::Result<void>{
            authentication_error ? rs::util::DbErrorCode::AuthenticationFailed
                                 : rs::util::DbErrorCode::ConnectionFailed,
            (authentication_error ? "Authentication failed: "
                                  : "Startup failed: ") +
                last_error_};
      }
      else if (parser_->is_ready_for_query(msg)) {
        if (!authenticated) {
          return rs::util::Result<void>{
              rs::util::DbErrorCode::ProtocolError,
              "ReadyForQuery arrived before AuthenticationOk"};
        }
        break; // Ready for queries
      }
      else if (msg.tag == 'N' && authenticated) {
        continue; // NoticeResponse may accompany backend startup
      }
      else {
        return rs::util::Result<void>{
            rs::util::DbErrorCode::ProtocolError,
            "Unexpected PostgreSQL startup message"};
      }
    }
  } catch (const std::exception& error) {
    last_error_ = error.what();
    return rs::util::Result<void>{
        rs::util::DbErrorCode::ProtocolError,
        "Invalid authentication exchange: " + last_error_};
  }
  return rs::util::Result<void>{};
}

rs::util::Result<void> GenericDatabaseConnection::record_parameter_status(
    const Message& msg) {
  const auto key_end = std::find(
      msg.payload.begin(), msg.payload.end(), std::byte{0});
  const auto value_begin = key_end == msg.payload.end()
      ? msg.payload.end() : std::next(key_end);
  const auto value_end = std::find(
      value_begin, msg.payload.end(), std::byte{0});
  if (key_end == msg.payload.begin() || key_end == msg.payload.end() ||
      value_end == msg.payload.end() ||
      std::next(value_end) != msg.payload.end()) {
    return rs::util::Result<void>{
        rs::util::DbErrorCode::ProtocolError,
        "Malformed PostgreSQL ParameterStatus message"};
  }
  const auto* bytes = reinterpret_cast<const char*>(msg.payload.data());
  const auto key_size = static_cast<std::size_t>(
      std::distance(msg.payload.begin(), key_end));
  const auto value_offset = key_size + 1;
  const auto value_size = static_cast<std::size_t>(
      std::distance(value_begin, value_end));
  server_params_[std::string(bytes, key_size)] =
      std::string(bytes + value_offset, value_size);
  return {};
}

void GenericDatabaseConnection::write_message_to_transport(
    rs::core::transport::ITransport& transport,
    const std::vector<std::byte>& data, 
    rs::util::Deadline deadline) {
  auto result = write_message_to_transport_result(transport, data, deadline);
  if (result.has_error()) {
    rs::util::unwrap_or_throw(std::move(result));
  }
}

rs::util::Result<void> GenericDatabaseConnection::write_message_to_transport_result(
    rs::core::transport::ITransport& transport,
    const std::vector<std::byte>& data, 
    rs::util::Deadline deadline) {
  
  size_t offset = 0;
  while (offset < data.size()) {
    auto result = transport.send(std::span<const std::byte>(data.data() + offset, data.size() - offset), deadline);
    if (result.has_error()) {
      mark_transport_failed();
      return rs::util::Result<void>{result.error(), result.error_message()};
    }
    if (result->n == 0) {
      mark_transport_failed();
      return rs::util::Result<void>{rs::util::DbErrorCode::NetworkError, "Write failed"};
    }
    offset += result->n;
  }
  return rs::util::Result<void>{};
}

} // namespace rs::core::database
