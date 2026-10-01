#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <span>
#include <memory>
#include <chrono>
#include "core/util/deadline.h"
#include "core/util/result.h"
#include "query_parameter.h"
#include "query_result.h"
#include "backend_result.h"
#include "sql_translation.h"
#include "native_type_info.h"
#include "type_definition.h"
#include "transaction.h"
#include "backend_capabilities.h"
#include "catalog_request.h"

namespace rs::core::database {

// Semantic context only; no ODBC statement codes cross the backend boundary.
enum class ErrorContext { Unknown, CreateTable, CreateView, CreateIndex, DropIndex };

// Limits for one response exchange, including control
// frames. Counts wire bytes, not decoded heap overhead. Product/ODBC exposure
// and row/cell/metadata budgets remain separate contracts.
struct ResponseLimits {
  std::size_t max_wire_bytes{64 * 1024 * 1024};
  std::size_t max_messages{100000};
};

struct ResultLimits {
  std::size_t max_rows{1000000};
  std::size_t max_cells{4000000};
  std::size_t max_columns_per_description{4096};
  std::size_t max_results{1024};
  std::size_t max_metadata_entries{65536};
  std::size_t max_column_name_bytes{1024};
  std::size_t max_metadata_name_bytes{1024 * 1024};
  // Per ErrorResponse/NoticeResponse payload, during startup and queries.
  std::size_t max_diagnostic_bytes{16 * 1024};
};

// Pre-encoding input bounds. Zero allows only empty values/counts.
struct InputLimits {
  std::size_t max_sql_bytes{1024 * 1024};
  std::size_t max_parameters{65535};
  std::size_t max_parameter_bytes{16 * 1024 * 1024};
  std::size_t max_parameter_total_bytes{64 * 1024 * 1024};
  std::size_t max_connection_field_bytes{64 * 1024};
  std::size_t max_request_wire_bytes{128 * 1024 * 1024};
  std::size_t max_startup_wire_bytes{1024 * 1024};
  std::size_t max_auth_wire_bytes{1024 * 1024};
};

struct ConnectionSettings {
  std::string host;
  std::string user;
  std::string password;
  std::string database;
  uint16_t port = 5432;
  std::chrono::milliseconds timeout{15000};
  bool use_ssl = true;
  std::string ssl_ca_file;
  std::string ssl_ca_dir;
  ResponseLimits response_limits;
  ResultLimits result_limits;
  InputLimits input_limits;
  ResponseLimits startup_response_limits{1024 * 1024, 10000};
};

// Shared validation for SDK callers and product option resolution.
inline bool valid_resource_limits(const ConnectionSettings& settings) noexcept {
  const auto& input = settings.input_limits;
  return settings.response_limits.max_wire_bytes >= 5 && settings.response_limits.max_messages > 0 &&
      settings.startup_response_limits.max_wire_bytes >= 5 && settings.startup_response_limits.max_messages > 0 &&
      settings.result_limits.max_results > 0 &&
      input.max_sql_bytes <= 64 * 1024 * 1024 && input.max_parameters <= 65535 &&
      input.max_parameter_bytes <= 64 * 1024 * 1024 && input.max_parameter_total_bytes <= 256 * 1024 * 1024 &&
      input.max_connection_field_bytes <= 1024 * 1024 && input.max_request_wire_bytes <= 1024 * 1024 * 1024 &&
      input.max_startup_wire_bytes <= 1024 * 1024 * 1024 && input.max_auth_wire_bytes <= 1024 * 1024 * 1024;
}

// Synchronous internal interface, serialized by the ODBC handle layer. Backends
// must not retain input views/spans after return. Returned QueryResult values own
// their storage. Metadata views have the lifetimes documented on each method.
// Every operation receives one absolute steady-clock deadline; nested I/O must
// reuse it, never restart the timeout. Session reuse is reported by is_connected,
// independently of diagnostic SQLSTATE. See BACKEND_BOUNDARY.md for retirement.
class IDatabaseConnection {
public:
  virtual ~IDatabaseConnection() = default;
  
  virtual BackendResult<void> connect(const ConnectionSettings& settings) = 0;
  virtual void disconnect() = 0;
  virtual bool is_connected() const = 0;
  // Passive protocol state, never a network probe or a pooling guarantee.
  virtual SessionState session_state() const {
    return is_connected() ? SessionState::Unknown : SessionState::Disconnected;
  }

  // Count bind markers using this backend's SQL lexical rules, without I/O.
  virtual std::size_t count_parameter_markers(std::string_view sql) const = 0;
  // Pure translation: no network I/O or mutation of connection state.
  virtual SqlTranslationResult translate_sql(std::string_view sql) const = 0;
  // Pure native-to-normalized metadata interpretation, without I/O.
  virtual NativeTypeInfo describe_type(std::uint32_t id, std::int16_t size,
                                       std::int32_t modifier) const = 0;
  
  // Pure, owning conversion of a non-NULL native cell. Binary becomes raw bytes;
  // Boolean becomes "0"/"1"; other text is preserved. nullopt means malformed
  // encoding, not SQL NULL. No I/O, state mutation or session retirement.
  virtual std::optional<std::string> normalize_result_value(
      ScalarType type, std::string_view value) const = 0;

  // Construct backend SQL without I/O; execution retains the caller's normal
  // deadline, diagnostics and cursor handling. Unsupported catalogs return an error.
  virtual rs::util::Result<std::string> catalog_query(
      const CatalogRequest& request) const = 0;

  // No I/O. Returned definitions and strings remain valid for the connection lifetime.
  // An empty catalog means the backend advertises no types.
  virtual std::span<const TypeDefinition> type_catalog() const = 0;

  // Resolve every requested native ID, retaining a fallback for missing types.
  // May perform backend metadata I/O using the caller's existing deadline.
  // On failure no partial map is returned; callers must not update their cache.
  virtual BackendResult<ResolvedTypeMap> resolve_types(
      std::span<const std::uint32_t> ids, rs::util::Deadline deadline) = 0;

  // Pure metadata snapshot: no I/O or session mutation.
  virtual BackendCapabilities capabilities() const = 0;

  // No I/O. Interpret a native server state using known statement context.
  // Return an owned, normalized five-character SQLSTATE, or no mapping so the
  // caller retains its operation-specific fallback. Never infer reuse from it.
  virtual std::optional<std::string> normalize_error_sqlstate(
      std::string_view native_state, ErrorContext context) const = 0;

  virtual TransactionCapabilities transaction_capabilities() const = 0;
  // Execute exactly one backend transaction command using the caller's deadline.
  // Return the backend error unchanged; the shared layer owns ODBC state changes.
  virtual BackendResult<void> transaction(TransactionAction action,
      rs::util::Deadline deadline) = 0;
  virtual BackendResult<void> set_transaction_isolation(TransactionIsolation level,
      rs::util::Deadline deadline) = 0;

  virtual BackendResult<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline) = 0;
  virtual BackendResult<QueryResult> execute_prepared(std::string_view sql,
                                                       std::span<const QueryParameter> params,
                                                       rs::util::Deadline deadline) = 0;
  virtual BackendResult<QueryResult> describe_statement(
      std::string_view sql,
      std::span<const QueryParameterType> parameter_types,
      rs::util::Deadline deadline) = 0;

  BackendResult<QueryResult> execute_prepared(
      std::string_view sql, std::span<const std::string> params,
      rs::util::Deadline deadline) {
    std::vector<QueryParameter> converted;
    converted.reserve(params.size());
    for (const auto& value : params) {
      converted.push_back(QueryParameter{value, QueryParameterType::Unspecified});
    }
    return execute_prepared(sql, converted, deadline);
  }
  
  virtual std::string get_parameter(std::string_view key) const = 0;
};

} // namespace rs::core::database
