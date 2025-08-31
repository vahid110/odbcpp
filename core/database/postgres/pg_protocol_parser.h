#pragma once
#include "core/database/i_protocol_parser.h"
#include "pg_messages.h"
#include <map>
#include <stdexcept>

namespace rs::core::database::postgres {

class PgProtocolParser : public IProtocolParser {
public:
  // Connection establishment
  std::vector<std::byte> create_startup_message(
    const std::string& user, 
    const std::string& database,
    const std::map<std::string, std::string>& params) override;
  
  std::vector<std::byte> create_ssl_request() override;
  
  // Authentication
  AuthenticationRequest parse_auth_request(const std::vector<std::byte>& data) override;
  std::vector<std::byte> create_auth_response(
    const AuthenticationRequest& request,
    const std::string& password,
    const std::string& user) override;
  
  // Query execution
  std::vector<std::byte> create_simple_query(std::string_view sql) override;
  std::vector<std::byte> create_prepared_query(
    std::string_view sql,
    std::span<const std::string> params) override;
  
  // Message parsing
  Message parse_message(const std::vector<std::byte>& data) override;
  bool is_ready_for_query(const Message& msg) override;
  bool is_error_response(const Message& msg) override;
  std::string extract_error_message(const Message& msg) override;
  std::vector<std::vector<std::string>> extract_query_results(
    const std::vector<Message>& messages) override;

private:
  static std::string md5_hex(const void* data, size_t n);
  static rs::pg::Authentication decode_auth(const std::vector<std::byte>& payload);
  static rs::pg::ErrorResponse decode_error(const std::vector<std::byte>& payload);
};

} // namespace rs::core::database::postgres