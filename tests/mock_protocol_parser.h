#pragma once

#include "core/database/i_protocol_parser.h"

namespace odbcpp::test {

class MockProtocolParser final
    : public rs::core::database::IProtocolParser {
 public:
  std::vector<std::byte> create_startup_message(
      const std::string&, const std::string&,
      const std::map<std::string, std::string>&) override {
    return {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
  }

  std::vector<std::byte> create_simple_query(std::string_view) override {
    return {std::byte{0x51}};
  }

  std::vector<std::byte> create_prepared_query(
      std::string_view,
      std::span<const rs::core::database::QueryParameter>) override {
    return {std::byte{0x50}};
  }

  std::vector<std::byte> create_statement_description(
      std::string_view,
      std::span<const rs::core::database::QueryParameterType>) override {
    return {std::byte{0x44}};
  }

  std::vector<std::byte> create_ssl_request() override {
    return {std::byte{0x04}, std::byte{0xd2}, std::byte{0x16},
            std::byte{0x2f}};
  }

  rs::core::database::Message parse_message(
      const std::vector<std::byte>& data) override {
    rs::core::database::Message message;
    if (data.empty()) return message;
    message.tag = static_cast<char>(data.front());
    message.payload.assign(data.begin() + 1, data.end());
    return message;
  }

  rs::core::database::AuthenticationRequest parse_auth_request(
      const std::vector<std::byte>&) override {
    return {};
  }

  std::vector<std::byte> create_auth_response(
      const rs::core::database::AuthenticationRequest&,
      const std::string&, const std::string&) override {
    return {};
  }

  bool is_error_response(
      const rs::core::database::Message& message) override {
    return message.tag == 'E';
  }

  bool is_ready_for_query(
      const rs::core::database::Message& message) override {
    return message.tag == 'Z';
  }

  std::string extract_error_message(
      const rs::core::database::Message&) override {
    return "injected protocol error";
  }

  rs::core::database::ResultRows extract_query_results(
      const std::vector<rs::core::database::Message>&) override {
    return {{"mock_result"}};
  }
};

} // namespace odbcpp::test
