#pragma once
#include <vector>
#include <string>
#include <string_view>
#include <span>
#include <cstddef>
#include <map>
#include <stdexcept>
#include "core/util/deadline.h"
#include "query_parameter.h"
#include "query_result.h"
#include "sql_translation.h"
#include "native_type_info.h"

namespace rs::core::database {

// Only request encoders throw this, before any request bytes are sent.
class RequestWireLimitExceeded : public std::length_error {
public:
  RequestWireLimitExceeded() : std::length_error("Database encoded request limit exceeded") {}
};

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
  
  // Private codec hook: parsers with canonical values need no conversion.
  virtual std::optional<std::string> normalize_result_value(
      ScalarType type, std::string_view value) const {
    if (type == ScalarType::Boolean && value != "0" && value != "1") return std::nullopt;
    return std::string(value);
  }

  // Connection establishment
  virtual std::vector<std::byte> create_startup_message(
    const std::string& user, 
    const std::string& database,
    const std::map<std::string, std::string>& params, std::size_t max_wire_bytes = 1024 * 1024) = 0;
  
  virtual std::vector<std::byte> create_ssl_request() = 0;
  
  // Authentication
  virtual AuthenticationRequest parse_auth_request(const std::vector<std::byte>& data) = 0;
  // peer_identity_verified is true only when the active transport has verified
  // both its certificate chain and peer host identity. Authentication methods
  // that expose reusable credentials must fail closed when it is false.
  virtual std::vector<std::byte> create_auth_response(
    const AuthenticationRequest& request,
    const std::string& password,
    const std::string& user,
    bool peer_identity_verified, std::size_t max_wire_bytes = 1024 * 1024) = 0;
  
  // Query execution
  virtual std::size_t count_parameter_markers(std::string_view sql) const = 0;
  // Pure translation: no network I/O or mutation of connection state.
  virtual SqlTranslationResult translate_sql(std::string_view sql) const = 0;
  // Pure native-to-normalized metadata interpretation, without I/O.
  virtual NativeTypeInfo describe_type(std::uint32_t id, std::int16_t size,
                                       std::int32_t modifier) const = 0;
  virtual std::vector<std::byte> create_simple_query(std::string_view sql,
      std::size_t max_wire_bytes = 256 * 1024 * 1024) = 0;
  virtual std::vector<std::byte> create_prepared_query(
    std::string_view sql,
    std::span<const QueryParameter> params,
    std::size_t max_wire_bytes = 256 * 1024 * 1024) = 0;
  virtual std::vector<std::byte> create_statement_description(
    std::string_view sql,
    std::span<const QueryParameterType> parameter_types,
    std::size_t max_wire_bytes = 256 * 1024 * 1024) = 0;
  
  // Message parsing
  virtual Message parse_message(const std::vector<std::byte>& data) = 0;
  virtual bool is_ready_for_query(const Message& msg) = 0;
  virtual bool is_error_response(const Message& msg) = 0;
  virtual std::string extract_error_message(const Message& msg) = 0;
  virtual std::string extract_error_sqlstate(const Message&) { return {}; }
  virtual ResultRows extract_query_results(
    const std::vector<Message>& messages) = 0;
  virtual QueryResult extract_query_result(const std::vector<Message>& messages) {
    QueryResult result;
    result.rows = extract_query_results(messages);
    return result;
  }
};

} // namespace rs::core::database
