#pragma once
#include "i_database_connection.h"
#include "i_protocol_parser.h"
#include "native_type_resolution.h"
#include "core/transport/i_transport.h"
#include <memory>

namespace rs::core::database {

class GenericDatabaseConnection : public IDatabaseConnection, public IStatementDescription {
public:
  GenericDatabaseConnection(
    std::unique_ptr<IProtocolParser> parser,
    std::unique_ptr<rs::core::transport::ITransport> transport = nullptr);
  
  BackendResult<void> connect(const ConnectionSettings& settings) override;
  void disconnect() override;
  bool is_connected() const override;
  SessionState session_state() const override { return session_state_; }
  // Private parser-composition helpers, absent from the SDK session interface.
  std::size_t count_parameter_markers(std::string_view sql) const;
  SqlTranslationResult translate_sql(std::string_view sql) const;
  // Private PostgreSQL-family codec/resolver hooks; never called by ODBC.
  virtual NativeTypeInfo describe_type(std::uint32_t id, std::int16_t size,
                                       std::int32_t modifier) const;
  
  virtual std::optional<std::string> normalize_result_value(
      ScalarType type, std::string_view value) const {
    return parser_->normalize_result_value(type, value);
  }

  BackendCapabilities capabilities() const override { return {}; }

  std::optional<std::string> normalize_error_sqlstate(
      std::string_view, ErrorContext) const override { return std::nullopt; }

  // Resolve all native IDs atomically using the original operation deadline.
  virtual BackendResult<ResolvedTypeMap> resolve_types(
      std::span<const std::uint32_t> ids, rs::util::Deadline deadline);

  BackendResult<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline) override;
  using IDatabaseConnection::execute_prepared;
  BackendResult<QueryResult> execute_prepared(std::string_view sql,
                                                std::span<const QueryParameter> params,
                                                rs::util::Deadline deadline) override;
  IStatementDescription* statement_description() noexcept override { return this; }
  BackendResult<QueryResult> describe_statement(
      std::string_view sql,
      std::span<const QueryParameterType> parameter_types,
      rs::util::Deadline deadline) override;
  
  std::string server_version() const override { return get_parameter("server_version"); }
  // Private PostgreSQL ParameterStatus storage; not a portable SDK service.
  virtual std::string get_parameter(std::string_view key) const;

private:
  BackendResult<QueryResult> reject_request_limit(BackendOperation operation) const;
  BackendResult<void> connect_impl(const ConnectionSettings& settings);
  SessionState session_state_{SessionState::Disconnected};
  BackendResult<QueryResult> finish_operation(BackendResult<QueryResult> result,
      BackendOperation operation);
  BackendResult<QueryResult> execute_query_impl(std::string_view sql,
      rs::util::Deadline deadline);
  BackendResult<QueryResult> execute_prepared_impl(std::string_view sql,
      std::span<const QueryParameter> params, rs::util::Deadline deadline);
  BackendResult<QueryResult> describe_statement_impl(std::string_view sql,
      std::span<const QueryParameterType> types, rs::util::Deadline deadline);
  enum class ResponseKind { SimpleExecution, PreparedExecution, Description };
  std::unique_ptr<IProtocolParser> parser_;
  std::unique_ptr<rs::core::transport::ITransport> transport_;
  ConnectionSettings settings_;
  std::map<std::string, std::string> server_params_;
  bool connected_ = false;
  bool peer_identity_verified_ = false;
  
  // Exception-based methods (for backward compatibility)
  void write_all(const std::vector<std::byte>& data, rs::util::Deadline deadline);
  std::vector<std::byte> read_message(rs::util::Deadline deadline);
  
  // Result-based methods (internal implementation)
  rs::util::Result<void> write_all_result(const std::vector<std::byte>& data, rs::util::Deadline deadline);
  rs::util::Result<std::vector<std::byte>> read_message_result(rs::util::Deadline deadline,
      std::size_t remaining_bytes = static_cast<std::size_t>(-1));
  BackendResult<void> perform_authentication_result(rs::util::Deadline deadline);
  rs::util::Result<void> record_parameter_status(const Message& msg);
  BackendResult<QueryResult> read_query_result(
      rs::util::Deadline deadline, ResponseKind kind);
  void mark_transport_failed() noexcept;
};

} // namespace rs::core::database
