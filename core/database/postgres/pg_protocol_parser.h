#pragma once
#include "core/database/i_protocol_parser.h"
#include "pg_messages.h"
#include "pg_value.h"
#include "scram_sha256.h"
#include <map>
#include <memory>
#include <stdexcept>

namespace rs::core::database::postgres {

class PgProtocolParser : public IProtocolParser {
public:
  std::optional<std::string> normalize_result_value(
      ScalarType type, std::string_view value) const override {
    return normalize_pg_result_value(type, value);
  }

  // Connection establishment
  std::vector<std::byte> create_startup_message(
    const std::string& user, 
    const std::string& database,
    const std::map<std::string, std::string>& params, std::size_t max_wire_bytes = 1024 * 1024) override;
  
  std::vector<std::byte> create_ssl_request() override;
  
  // Authentication
  void clear_authentication_state() noexcept override;
  AuthenticationRequest parse_auth_request(const std::vector<std::byte>& data) override;
  std::vector<std::byte> create_auth_response(
    const AuthenticationRequest& request,
    const std::string& password,
    const std::string& user,
    bool peer_identity_verified, std::size_t max_wire_bytes = 1024 * 1024) override;
  
  SqlTranslationResult translate_sql(std::string_view sql) const override;
  NativeTypeInfo describe_type(std::uint32_t id, std::int16_t size,
                               std::int32_t modifier) const override;

  // Query execution
  std::size_t count_parameter_markers(std::string_view sql) const override {
    return parameter_marker_count(sql);
  }
  std::vector<std::byte> create_simple_query(std::string_view sql, std::size_t max_wire_bytes = 256 * 1024 * 1024) override;
  std::vector<std::byte> create_prepared_query(
    std::string_view sql,
    std::span<const QueryParameter> params, std::size_t max_wire_bytes = 256 * 1024 * 1024) override;
  std::vector<std::byte> create_statement_description(
    std::string_view sql,
    std::span<const QueryParameterType> parameter_types, std::size_t max_wire_bytes = 256 * 1024 * 1024) override;
  static std::size_t parameter_marker_count(std::string_view sql);
  
  // Message parsing
  Message parse_message(const std::vector<std::byte>& data) override;
  bool is_ready_for_query(const Message& msg) override;
  bool is_error_response(const Message& msg) override;
  std::string extract_error_message(const Message& msg) override;
  std::string extract_error_sqlstate(const Message& msg) override;
  ResultRows extract_query_results(
    const std::vector<Message>& messages) override;
  ParsedQueryResult extract_query_result(const std::vector<Message>& messages) override;

private:
  static std::string md5_hex(const void* data, size_t n);
  static rs::pg::Authentication decode_auth(const std::vector<std::byte>& payload);
  static rs::pg::ErrorResponse decode_error_fields(
      const std::vector<std::byte>& payload);
  std::unique_ptr<ScramSha256Client> scram_client_;
  bool scram_server_verified_{false};
};

} // namespace rs::core::database::postgres
