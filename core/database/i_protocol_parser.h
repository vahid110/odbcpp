#pragma once
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <cstddef>
#include <map>
#include "core/util/deadline.h"
#include "query_parameter.h"
#include "query_result.h"

namespace rs::core::database {

struct AuthenticationRequest {
  enum class Type { None, Cleartext, MD5, SASL, SASLContinue, SASLFinal }
      type = Type::None;
  std::vector<std::byte> challenge_data;
};

struct Message {
  char tag = 0;
  std::vector<std::byte> payload;
};

class IProtocolParser {
public:
  virtual ~IProtocolParser() = default;
  
  // Connection establishment
  virtual std::vector<std::byte> create_startup_message(
    const std::string& user, 
    const std::string& database,
    const std::map<std::string, std::string>& params) = 0;
  
  virtual std::vector<std::byte> create_ssl_request() = 0;
  
  // Authentication
  virtual AuthenticationRequest parse_auth_request(const std::vector<std::byte>& data) = 0;
  virtual std::vector<std::byte> create_auth_response(
    const AuthenticationRequest& request,
    const std::string& password,
    const std::string& user) = 0;
  
  // Query execution
  virtual std::vector<std::byte> create_simple_query(std::string_view sql) = 0;
  virtual std::vector<std::byte> create_prepared_query(
    std::string_view sql,
    std::span<const QueryParameter> params) = 0;
  virtual std::vector<std::byte> create_statement_description(
    std::string_view sql,
    std::span<const QueryParameterType> parameter_types) = 0;
  
  // Message parsing
  virtual Message parse_message(const std::vector<std::byte>& data) = 0;
  virtual bool is_ready_for_query(const Message& msg) = 0;
  virtual bool is_error_response(const Message& msg) = 0;
  virtual std::string extract_error_message(const Message& msg) = 0;
  virtual ResultRows extract_query_results(
    const std::vector<Message>& messages) = 0;
  virtual QueryResult extract_query_result(const std::vector<Message>& messages) {
    QueryResult result;
    result.rows = extract_query_results(messages);
    return result;
  }
};

} // namespace rs::core::database
