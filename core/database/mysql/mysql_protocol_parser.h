#pragma once
#include "core/database/i_protocol_parser.h"

namespace rs::core::database::mysql {

// Example of how to add MySQL support
class MySQLProtocolParser : public IProtocolParser {
public:
  // MySQL-specific protocol implementation
  std::vector<std::byte> create_startup_message(
    const std::string& user, 
    const std::string& database,
    const std::map<std::string, std::string>& params) override {
    // MySQL handshake protocol implementation
    throw std::runtime_error("MySQL protocol not implemented yet");
  }
  
  std::vector<std::byte> create_ssl_request() override {
    // MySQL SSL request format
    throw std::runtime_error("MySQL SSL not implemented yet");
  }
  
  AuthenticationRequest parse_auth_request(const std::vector<std::byte>& data) override {
    // Parse MySQL auth challenge
    throw std::runtime_error("MySQL auth parsing not implemented yet");
  }
  
  std::vector<std::byte> create_auth_response(
    const AuthenticationRequest& request,
    const std::string& password,
    const std::string& user) override {
    // MySQL auth response (native password, caching_sha2_password, etc.)
    throw std::runtime_error("MySQL auth response not implemented yet");
  }
  
  std::vector<std::byte> create_simple_query(std::string_view sql) override {
    // MySQL COM_QUERY packet
    throw std::runtime_error("MySQL query not implemented yet");
  }
  
  std::vector<std::byte> create_prepared_query(
    std::string_view sql,
    std::span<const std::string> params) override {
    // MySQL COM_STMT_PREPARE + COM_STMT_EXECUTE
    throw std::runtime_error("MySQL prepared statements not implemented yet");
  }
  
  Message parse_message(const std::vector<std::byte>& data) override {
    // Parse MySQL packet format
    throw std::runtime_error("MySQL message parsing not implemented yet");
  }
  
  bool is_ready_for_query(const Message& msg) override {
    // MySQL doesn't have explicit "ready for query" - check OK packet
    return false;
  }
  
  bool is_error_response(const Message& msg) override {
    // Check for MySQL ERR packet (0xFF)
    return false;
  }
  
  std::string extract_error_message(const Message& msg) override {
    return "MySQL error extraction not implemented";
  }
  
  std::vector<std::vector<std::string>> extract_query_results(
    const std::vector<Message>& messages) override {
    // Parse MySQL result set packets
    return {};
  }
};

} // namespace rs::core::database::mysql