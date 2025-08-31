#pragma once
#include "i_protocol_parser.h"

namespace rs::core::database {

// Mock protocol parser for testing
class MockProtocolParser : public IProtocolParser {
public:
  std::vector<std::byte> create_startup_message(
    const std::string& user, const std::string& database,
    const std::map<std::string, std::string>& params) override {
    return {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  }
  
  std::vector<std::byte> create_simple_query(std::string_view sql) override {
    return {std::byte{0x51}}; // 'Q' for Query
  }
  
  std::vector<std::byte> create_prepared_query(
    std::string_view sql, std::span<const std::string> params) override {
    return {std::byte{0x50}}; // 'P' for Parse
  }
  
  std::vector<std::byte> create_ssl_request() override {
    return {std::byte{0x04}, std::byte{0xd2}, std::byte{0x16}, std::byte{0x2f}};
  }
  
  Message parse_message(const std::vector<std::byte>& data) override {
    Message msg;
    if (!data.empty()) {
      msg.tag = static_cast<char>(data[0]);
      if (data.size() > 1) {
        msg.payload.assign(data.begin() + 1, data.end());
      }
    }
    return msg;
  }
  
  AuthenticationRequest parse_auth_request(const std::vector<std::byte>& payload) override {
    AuthenticationRequest req;
    req.type = AuthenticationRequest::Type::None; // Mock successful auth
    return req;
  }
  
  std::vector<std::byte> create_auth_response(
    const AuthenticationRequest& req, const std::string& password, const std::string& user) override {
    return {}; // No response needed for None auth
  }
  
  bool is_error_response(const Message& msg) override {
    return msg.tag == 'E';
  }
  
  bool is_ready_for_query(const Message& msg) override {
    return msg.tag == 'Z';
  }
  
  std::string extract_error_message(const Message& msg) override {
    return "Mock error message";
  }
  
  std::vector<std::vector<std::string>> extract_query_results(const std::vector<Message>& messages) override {
    // Return mock results
    return {{"mock_result"}};
  }
};

} // namespace rs::core::database