#include "generic_database_connection.h"
#include "core/transport/socket_transport.h"
#include "core/transport/tls_transport.h"
#include "core/util/exception_adapter.h"

namespace rs::core::database {

GenericDatabaseConnection::GenericDatabaseConnection(
    std::unique_ptr<IProtocolParser> parser,
    std::unique_ptr<rs::core::transport::ITransport> transport)
  : parser_(std::move(parser)), transport_(std::move(transport)) {}

rs::util::Result<void> GenericDatabaseConnection::connect(const ConnectionSettings& settings) {
  return rs::util::try_catch([&]() {
  settings_ = settings;
  
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
    // For PostgreSQL/Redshift: plain connect + SSL request + upgrade
    auto plain_transport = std::make_unique<rs::core::transport::SocketTransport>();
    plain_transport->connect(settings.host, settings.port, deadline);
    
    // Send SSL request
    auto ssl_req = parser_->create_ssl_request();
    write_message_to_transport(*plain_transport, ssl_req, deadline);
    
    // Read SSL response
    std::vector<std::byte> response(1);
    auto result = plain_transport->recv(response, deadline);
    if (result.n != 1 || response[0] != std::byte{'S'}) {
      throw std::runtime_error("SSL not supported by server");
    }
    
    // Upgrade to TLS
    auto* tls_transport = static_cast<rs::core::transport::TLSTransport*>(transport_.get());
    tls_transport->upgrade_from(plain_transport->release(), settings.host, deadline);
  } else {
    transport_->connect(settings.host, settings.port, deadline);
  }
  
  // Send startup message
  std::map<std::string, std::string> params;
  params["application_name"] = "odbcpp";
  auto startup = parser_->create_startup_message(settings.user, settings.database, params);
  write_all(startup, deadline);
  
  // Handle authentication
  perform_authentication(deadline);
  
    connected_ = true;
  });
}

void GenericDatabaseConnection::disconnect() {
  if (transport_) {
    transport_->close();
  }
  connected_ = false;
}

bool GenericDatabaseConnection::is_connected() const {
  return connected_;
}

rs::util::Result<QueryResult> GenericDatabaseConnection::execute_query(std::string_view sql, rs::util::Deadline deadline) {
  return rs::util::try_catch([&]() {
    if (!connected_) throw std::runtime_error("Not connected");
    
    auto query_msg = parser_->create_simple_query(sql);
    write_all(query_msg, deadline);
    
    std::vector<Message> messages;
    
    while (true) {
      auto raw_msg = read_message(deadline);
      auto msg = parser_->parse_message(raw_msg);
      
      if (parser_->is_error_response(msg)) {
        last_error_ = parser_->extract_error_message(msg);
        throw std::runtime_error("Query error: " + last_error_);
      }
      
      messages.push_back(msg);
      
      if (parser_->is_ready_for_query(msg)) {
        break;
      }
    }
    
    QueryResult result;
    result.rows = parser_->extract_query_results(messages);
    return result;
  });
}

rs::util::Result<QueryResult> GenericDatabaseConnection::execute_prepared(std::string_view sql, 
                                                                            std::span<const std::string> params,
                                                                            rs::util::Deadline deadline) {
  return rs::util::try_catch([&]() {
    if (!connected_) throw std::runtime_error("Not connected");
    
    auto query_msg = parser_->create_prepared_query(sql, params);
    write_all(query_msg, deadline);
    
    std::vector<Message> messages;
    
    while (true) {
      auto raw_msg = read_message(deadline);
      auto msg = parser_->parse_message(raw_msg);
      
      if (parser_->is_error_response(msg)) {
        last_error_ = parser_->extract_error_message(msg);
        throw std::runtime_error("Query error: " + last_error_);
      }
      
      messages.push_back(msg);
      
      if (parser_->is_ready_for_query(msg)) {
        break;
      }
    }
    
    QueryResult result;
    result.rows = parser_->extract_query_results(messages);
    return result;
  });
}

std::string GenericDatabaseConnection::get_parameter(std::string_view key) const {
  auto it = server_params_.find(std::string(key));
  return (it != server_params_.end()) ? it->second : std::string{};
}

std::string GenericDatabaseConnection::get_last_error() const {
  return last_error_;
}

void GenericDatabaseConnection::write_all(const std::vector<std::byte>& data, rs::util::Deadline deadline) {
  size_t offset = 0;
  while (offset < data.size()) {
    auto result = transport_->send(std::span<const std::byte>(data.data() + offset, data.size() - offset), deadline);
    if (result.n == 0) throw std::runtime_error("Write failed");
    offset += result.n;
  }
}

std::vector<std::byte> GenericDatabaseConnection::read_message(rs::util::Deadline deadline) {
  // Read message header (1 byte tag + 4 bytes length)
  std::vector<std::byte> header(5);
  size_t offset = 0;
  
  while (offset < 5) {
    auto result = transport_->recv(std::span<std::byte>(header.data() + offset, 5 - offset), deadline);
    if (result.eof) throw std::runtime_error("Unexpected EOF");
    if (result.n == 0) continue;
    offset += result.n;
  }
  
  // Extract length
  const auto* p = reinterpret_cast<const unsigned char*>(header.data());
  uint32_t len = (p[1] << 24) | (p[2] << 16) | (p[3] << 8) | p[4];
  
  if (len < 4) throw std::runtime_error("Invalid message length");
  
  // Read payload
  std::vector<std::byte> message(1 + len);
  message[0] = header[0]; // tag
  std::memcpy(message.data() + 1, header.data() + 1, 4); // length
  
  size_t payload_len = len - 4;
  offset = 5;
  
  while (offset < message.size()) {
    auto result = transport_->recv(std::span<std::byte>(message.data() + offset, message.size() - offset), deadline);
    if (result.eof) throw std::runtime_error("Unexpected EOF");
    if (result.n == 0) continue;
    offset += result.n;
  }
  
  return message;
}

void GenericDatabaseConnection::perform_authentication(rs::util::Deadline deadline) {
  while (true) {
    auto raw_msg = read_message(deadline);
    auto msg = parser_->parse_message(raw_msg);
    
    if (msg.tag == 'R') { // Authentication
      auto auth_req = parser_->parse_auth_request(msg.payload);
      
      if (auth_req.type == AuthenticationRequest::Type::None) {
        continue; // Authentication successful
      }
      
      auto auth_response = parser_->create_auth_response(auth_req, settings_.password, settings_.user);
      if (!auth_response.empty()) {
        write_all(auth_response, deadline);
      }
    }
    else if (msg.tag == 'S') { // ParameterStatus
      // Parse and store server parameters
      // Simplified for now
    }
    else if (msg.tag == 'K') { // BackendKeyData
      // Store backend key data
    }
    else if (parser_->is_error_response(msg)) {
      last_error_ = parser_->extract_error_message(msg);
      throw std::runtime_error("Authentication failed: " + last_error_);
    }
    else if (parser_->is_ready_for_query(msg)) {
      break; // Ready for queries
    }
  }
}

void GenericDatabaseConnection::write_message_to_transport(
    rs::core::transport::ITransport& transport,
    const std::vector<std::byte>& data, 
    rs::util::Deadline deadline) {
  
  size_t offset = 0;
  while (offset < data.size()) {
    auto result = transport.send(std::span<const std::byte>(data.data() + offset, data.size() - offset), deadline);
    if (result.n == 0) throw std::runtime_error("Write failed");
    offset += result.n;
  }
}

} // namespace rs::core::database