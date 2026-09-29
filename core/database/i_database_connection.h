#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <span>
#include <memory>
#include <chrono>
#include "core/util/deadline.h"
#include "core/util/result.h"
#include "query_parameter.h"
#include "query_result.h"
#include "sql_translation.h"
#include "native_type_info.h"
#include "type_definition.h"
#include "transaction.h"
#include "backend_capabilities.h"
#include "catalog_request.h"

namespace rs::core::database {

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
};

class IDatabaseConnection {
public:
  virtual ~IDatabaseConnection() = default;
  
  virtual rs::util::Result<void> connect(const ConnectionSettings& settings) = 0;
  virtual void disconnect() = 0;
  virtual bool is_connected() const = 0;

  // Count bind markers using this backend's SQL lexical rules, without I/O.
  virtual std::size_t count_parameter_markers(std::string_view sql) const = 0;
  // Pure translation: no network I/O or mutation of connection state.
  virtual SqlTranslationResult translate_sql(std::string_view sql) const = 0;
  // Pure native-to-normalized metadata interpretation, without I/O.
  virtual NativeTypeInfo describe_type(std::uint32_t id, std::int16_t size,
                                       std::int32_t modifier) const = 0;
  
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
  virtual rs::util::Result<ResolvedTypeMap> resolve_types(
      std::span<const std::uint32_t> ids, rs::util::Deadline deadline) = 0;

  // Pure metadata snapshot: no I/O or session mutation.
  virtual BackendCapabilities capabilities() const = 0;

  virtual TransactionCapabilities transaction_capabilities() const = 0;
  // Execute exactly one backend transaction command using the caller's deadline.
  // Return the backend error unchanged; the shared layer owns ODBC state changes.
  virtual rs::util::Result<void> transaction(TransactionAction action,
      rs::util::Deadline deadline) = 0;
  virtual rs::util::Result<void> set_transaction_isolation(TransactionIsolation level,
      rs::util::Deadline deadline) = 0;

  virtual rs::util::Result<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline) = 0;
  virtual rs::util::Result<QueryResult> execute_prepared(std::string_view sql, 
                                                       std::span<const QueryParameter> params,
                                                       rs::util::Deadline deadline) = 0;
  virtual rs::util::Result<QueryResult> describe_statement(
      std::string_view sql,
      std::span<const QueryParameterType> parameter_types,
      rs::util::Deadline deadline) = 0;

  rs::util::Result<QueryResult> execute_prepared(
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
  virtual std::string get_last_error() const = 0;
  virtual std::string get_last_server_sqlstate() const { return {}; }
};

} // namespace rs::core::database
