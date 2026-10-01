#include "generic_database_connection.h"
#include "core/transport/socket_transport.h"
#include "core/transport/start_tls_transport.h"
#include "core/transport/tls_transport.h"
#include "core/transport/tls_configurable_transport.h"
#include "core/util/exception_adapter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <new>
#include <stdexcept>

namespace rs::core::database {
namespace {

constexpr std::uint32_t kShortFrameLimit = 30000;
constexpr std::uint32_t kLargeFrameLimit = 0x3fffffff;

bool allows_large_frame(char tag) {
  switch (tag) {
    case 'D': case 'E': case 'V':
    case 'N': case 'A': case 'T': case 't':
      return true;
    default:
      return false;
  }
}

bool has_binary_columns(const QueryResult& result) {
  const auto binary = [](const ResultColumnMetadata& column) {
    return column.format_code == 1;
  };
  if (std::any_of(result.columns.begin(), result.columns.end(), binary)) {
    return true;
  }
  for (const auto& additional : result.additional_results) {
    if (std::any_of(additional.columns.begin(), additional.columns.end(),
                    binary)) {
      return true;
    }
  }
  return false;
}

rs::util::DbErrorCode connect_error_code(const std::error_code& error) {
  if (error == rs::util::make_error_code(rs::util::DbErrorCode::Timeout)) {
    return rs::util::DbErrorCode::Timeout;
  }
  if (error == rs::util::make_error_code(
                   rs::util::DbErrorCode::InvalidParameter)) {
    return rs::util::DbErrorCode::InvalidParameter;
  }
  return rs::util::DbErrorCode::ConnectionFailed;
}

} // namespace

GenericDatabaseConnection::GenericDatabaseConnection(
    std::unique_ptr<IProtocolParser> parser,
    std::unique_ptr<rs::core::transport::ITransport> transport)
  : parser_(std::move(parser)), transport_(std::move(transport)) {}

std::size_t GenericDatabaseConnection::count_parameter_markers(
    std::string_view sql) const {
  return parser_->count_parameter_markers(sql);
}

SqlTranslationResult GenericDatabaseConnection::translate_sql(
    std::string_view sql) const {
  return parser_->translate_sql(sql);
}

NativeTypeInfo GenericDatabaseConnection::describe_type(
    std::uint32_t id, std::int16_t size, std::int32_t modifier) const {
  return parser_->describe_type(id, size, modifier);
}

BackendResult<ResolvedTypeMap> GenericDatabaseConnection::resolve_types(
    std::span<const std::uint32_t> ids, rs::util::Deadline) {
  ResolvedTypeMap types;
  for (const auto id : ids) types.emplace(id, describe_type(id, -1, -1));
  return types;
}

BackendResult<void> GenericDatabaseConnection::connect(const ConnectionSettings& settings) {
  BackendResult<void> result;
  try {
    result = connect_impl(settings);
  } catch (const std::bad_alloc&) {
    mark_transport_failed();
    result = {rs::util::DbErrorCode::AllocationFailure, {}};
  }
  if (result.has_error()) {
    auto& error = result.backend_error();
    if (error.operation == BackendOperation::Unknown) error.operation = BackendOperation::Connect;
    error.session_state = session_state_;
    error.disposition = !connected_ ? SessionDisposition::Retire :
        session_state_ == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired;
  }
  return result;
}

BackendResult<void> GenericDatabaseConnection::connect_impl(const ConnectionSettings& settings) {
  if (!valid_resource_limits(settings)) {
    return {rs::util::DbErrorCode::InvalidParameter, "Resource limits are outside supported bounds"};
  }
  const auto& input = settings.input_limits;
  for (const auto* field : {&settings.host, &settings.user, &settings.password,
                           &settings.database, &settings.ssl_ca_file, &settings.ssl_ca_dir}) {
    if (field->size() > input.max_connection_field_bytes) {
      return {rs::util::DbErrorCode::ResourceLimit, "Database connection input limit exceeded"};
    }
  }
  if (settings.password.find('\0') != std::string::npos) {
    return {rs::util::DbErrorCode::InvalidParameter,
            "PostgreSQL authentication credential contains an "
            "embedded NUL byte"};
  }
  if (!settings.ssl_ca_file.empty() && !settings.ssl_ca_dir.empty()) {
    return {rs::util::DbErrorCode::InvalidParameter,
            "TLS CA file and directory cannot both be specified"};
  }
  if (!settings.use_ssl &&
      (!settings.ssl_ca_file.empty() || !settings.ssl_ca_dir.empty())) {
    return {rs::util::DbErrorCode::InvalidParameter,
            "TLS CA settings require TLS"};
  }
  std::map<std::string, std::string> params;
  params["application_name"] = "odbcpp";
  params["client_encoding"] = "UTF8";
  params["DateStyle"] = "ISO, YMD";
  std::vector<std::byte> startup;
  try {
    startup = parser_->create_startup_message(
        settings.user, settings.database, params, settings.input_limits.max_startup_wire_bytes);
  } catch (const RequestWireLimitExceeded&) {
    return {rs::util::DbErrorCode::ResourceLimit, "Database startup request limit exceeded"};
  } catch (const std::bad_alloc&) {
    return {rs::util::DbErrorCode::AllocationFailure, {}};
  } catch (const std::invalid_argument& error) {
    return {rs::util::DbErrorCode::InvalidParameter, error.what()};
  }

  settings_ = settings;
  server_params_.clear();
  peer_identity_verified_ = false;
  
  if (!transport_) {
    if (settings.use_ssl) {
      auto tls = std::make_unique<rs::core::transport::TLSTransport>();
      tls->set_verify(true);
      transport_ = std::move(tls);
    } else {
      transport_ = std::make_unique<rs::core::transport::SocketTransport>();
    }
  }

  struct FailedConnectCleanup {
    GenericDatabaseConnection& connection;
    bool complete = false;
    ~FailedConnectCleanup() {
      if (!complete) connection.disconnect();
    }
  } cleanup{*this};
  
  auto deadline = rs::util::make_deadline(settings.timeout);
  
  if (settings.use_ssl) {
    // PostgreSQL/Redshift starts in plain text, sends an SSLRequest, then
    // upgrades the same transport connection in place.
    auto* start_tls = dynamic_cast<
        rs::core::transport::IStartTlsTransport*>(transport_.get());
    if (start_tls == nullptr) {
      return BackendResult<void>{
          rs::util::DbErrorCode::InvalidParameter,
          "selected transport does not support PostgreSQL TLS upgrade"};
    }
    auto* tls_configuration = dynamic_cast<
        rs::core::transport::ITlsConfigurableTransport*>(transport_.get());
    if (tls_configuration != nullptr) {
      tls_configuration->set_ca_locations(
          settings.ssl_ca_file, settings.ssl_ca_dir);
    } else if (!settings.ssl_ca_file.empty() || !settings.ssl_ca_dir.empty()) {
      return {rs::util::DbErrorCode::InvalidParameter,
              "selected TLS transport does not support custom CA settings"};
    }

    auto connect_result = start_tls->connect_plain(
        settings.host, settings.port, deadline);
    if (connect_result.has_error()) {
      return BackendResult<void>{
          connect_error_code(connect_result.error()),
          connect_result.error_message()};
    }
    
    // Send SSL request
    auto ssl_req = parser_->create_ssl_request();
    auto write_result = write_all_result(ssl_req, deadline);
    if (write_result.has_error()) {
      return {write_result.error(), write_result.error_message()};
    }
    
    // Read SSL response
    std::vector<std::byte> response(1);
    auto recv_result = transport_->recv(response, deadline);
    if (recv_result.has_error()) {
      return BackendResult<void>{
          recv_result.error(), recv_result.error_message()};
    }
    if (recv_result->n > response.size()) {
      return {rs::util::DbErrorCode::ProtocolError,
              "Transport read exceeded requested SSL response bytes"};
    }
    if (recv_result->eof) {
      return {rs::util::DbErrorCode::NetworkError,
              "Unexpected EOF during SSL negotiation"};
    }
    if (recv_result->n == 0) {
      return {rs::util::DbErrorCode::NetworkError,
              "SSL negotiation read made no progress"};
    }
    if (response[0] == std::byte{'N'}) {
      return {rs::util::DbErrorCode::TLSError,
              "SSL not supported by server"};
    }
    if (response[0] != std::byte{'S'}) {
      return {rs::util::DbErrorCode::ProtocolError,
              "Invalid PostgreSQL SSL negotiation response"};
    }
    
    auto upgrade_result = start_tls->upgrade_to_tls(settings.host, deadline);
    if (upgrade_result.has_error()) return {upgrade_result.error(), upgrade_result.error_message()};
    peer_identity_verified_ = start_tls->peer_identity_verified();
  } else {
    auto connect_result = transport_->connect(settings.host, settings.port, deadline);
    if (connect_result.has_error()) {
      return BackendResult<void>{
          connect_error_code(connect_result.error()),
          connect_result.error_message()};
    }
  }
  
  // Send startup message
  auto write_result = write_all_result(startup, deadline);
  if (write_result.has_error()) {
    return {write_result.error(), write_result.error_message()};
  }
  
  // Handle authentication
  auto auth_result = perform_authentication_result(deadline);
  if (auth_result.has_error()) {
    return auth_result;
  }
  
  connected_ = true;
  cleanup.complete = true;
  return BackendResult<void>{};
}

void GenericDatabaseConnection::disconnect() {
  if (transport_) {
    transport_->close();
  }
  connected_ = false;
  session_state_ = SessionState::Disconnected;
  server_params_.clear();
  peer_identity_verified_ = false;
}

bool GenericDatabaseConnection::is_connected() const {
  return connected_;
}

void GenericDatabaseConnection::mark_transport_failed() noexcept {
  const bool was_connected = connected_;
  connected_ = false;
  session_state_ = SessionState::Disconnected;
  if (was_connected && transport_) transport_->close();
}

BackendResult<QueryResult> GenericDatabaseConnection::finish_operation(
    BackendResult<QueryResult> result, BackendOperation operation) {
  if (result.has_error()) {
    auto& error = result.backend_error();
    error.operation = operation;
    const bool ambiguous = error.error_class == BackendErrorClass::Timeout ||
        error.error_class == BackendErrorClass::Transport ||
        error.error_class == BackendErrorClass::Protocol ||
        error.error_class == BackendErrorClass::Tls ||
        error.error_class == BackendErrorClass::Unknown ||
        error.error_class == BackendErrorClass::ResourceLimit;
    if (ambiguous) mark_transport_failed();
    error.session_state = session_state_;
    error.disposition = !connected_ ? SessionDisposition::Retire :
        session_state_ == SessionState::Idle ? SessionDisposition::Reusable :
        SessionDisposition::ResetRequired;
  } else {
    const auto annotate = [&](QueryResult& item) {
      if (!item.error) return;
      item.error->operation = operation;
      item.error->session_state = session_state_;
      item.error->disposition = !connected_ ? SessionDisposition::Retire :
          session_state_ == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired;
    };
    annotate(*result);
    for (auto& item : result->additional_results) annotate(item);
  }
  return result;
}

BackendResult<QueryResult> GenericDatabaseConnection::reject_request_limit(BackendOperation operation) const {
  BackendResult<QueryResult> result{rs::util::DbErrorCode::ResourceLimit, "Database request input limit exceeded"};
  auto& error = result.backend_error();
  error.operation = operation;
  error.session_state = session_state_;
  // Preflight has performed no I/O; the current owner's session is unchanged.
  error.disposition = !connected_ ? SessionDisposition::Retire :
      session_state_ == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired;
  return result;
}

BackendResult<QueryResult> GenericDatabaseConnection::execute_query(
    std::string_view sql, rs::util::Deadline deadline) {
  if (connected_ && sql.size() > settings_.input_limits.max_sql_bytes) {
    return reject_request_limit(BackendOperation::ExecuteDirect);
  }
  try {
    return finish_operation(execute_query_impl(sql, deadline), BackendOperation::ExecuteDirect);
  } catch (const RequestWireLimitExceeded&) {
    return reject_request_limit(BackendOperation::ExecuteDirect);
  } catch (const std::bad_alloc&) {
    // Request I/O may have started; never expose an ambiguous session as live.
    mark_transport_failed();
    return finish_operation({rs::util::DbErrorCode::AllocationFailure, {}}, BackendOperation::ExecuteDirect);
  }
}

BackendResult<QueryResult> GenericDatabaseConnection::execute_prepared(
    std::string_view sql, std::span<const QueryParameter> params, rs::util::Deadline deadline) {
  const auto& limits = settings_.input_limits;
  if (connected_) {
    if (sql.size() > limits.max_sql_bytes || params.size() > limits.max_parameters) {
      return reject_request_limit(BackendOperation::ExecutePrepared);
    }
    std::size_t bytes = 0;
    for (const auto& param : params) {
      const auto size = param.value ? param.value->size() : 0;
      if (size > limits.max_parameter_bytes || size > limits.max_parameter_total_bytes - bytes) {
        return reject_request_limit(BackendOperation::ExecutePrepared);
      }
      bytes += size;
    }
  }
  try {
    return finish_operation(execute_prepared_impl(sql, params, deadline), BackendOperation::ExecutePrepared);
  } catch (const RequestWireLimitExceeded&) {
    return reject_request_limit(BackendOperation::ExecutePrepared);
  } catch (const std::bad_alloc&) {
    // Request I/O may have started; never expose an ambiguous session as live.
    mark_transport_failed();
    return finish_operation({rs::util::DbErrorCode::AllocationFailure, {}}, BackendOperation::ExecutePrepared);
  }
}

BackendResult<QueryResult> GenericDatabaseConnection::describe_statement(
    std::string_view sql, std::span<const QueryParameterType> types, rs::util::Deadline deadline) {
  if (connected_ && (sql.size() > settings_.input_limits.max_sql_bytes ||
                    types.size() > settings_.input_limits.max_parameters)) {
    return reject_request_limit(BackendOperation::Describe);
  }
  try {
    return finish_operation(describe_statement_impl(sql, types, deadline), BackendOperation::Describe);
  } catch (const RequestWireLimitExceeded&) {
    return reject_request_limit(BackendOperation::Describe);
  } catch (const std::bad_alloc&) {
    // Request I/O may have started; never expose an ambiguous session as live.
    mark_transport_failed();
    return finish_operation({rs::util::DbErrorCode::AllocationFailure, {}}, BackendOperation::Describe);
  }
}

BackendResult<QueryResult> GenericDatabaseConnection::execute_query_impl(std::string_view sql, rs::util::Deadline deadline) {
  if (!connected_) {
    return BackendResult<QueryResult>{rs::util::DbErrorCode::NotConnected, "Not connected"};
  }
  
  std::vector<std::byte> query_msg;
  try {
    query_msg = parser_->create_simple_query(sql, settings_.input_limits.max_request_wire_bytes);
  } catch (const RequestWireLimitExceeded&) {
    throw;
  } catch (const std::bad_alloc&) {
    return {rs::util::DbErrorCode::AllocationFailure, {}};
  } catch (const std::exception& error) {
    return BackendResult<QueryResult>{
        rs::util::DbErrorCode::InvalidParameter, error.what()};
  }
  auto write_result = write_all_result(query_msg, deadline);
  if (write_result.has_error()) {
    return BackendResult<QueryResult>{write_result.error(), write_result.error_message()};
  }

  return read_query_result(deadline, ResponseKind::SimpleExecution);
}

BackendResult<QueryResult> GenericDatabaseConnection::execute_prepared_impl(std::string_view sql,
                                                                            std::span<const QueryParameter> params,
                                                                            rs::util::Deadline deadline) {
  if (!connected_) {
    return BackendResult<QueryResult>{rs::util::DbErrorCode::NotConnected, "Not connected"};
  }
  
  std::vector<std::byte> query_msg;
  try {
    query_msg = parser_->create_prepared_query(sql, params, settings_.input_limits.max_request_wire_bytes);
  } catch (const RequestWireLimitExceeded&) {
    throw;
  } catch (const std::bad_alloc&) {
    return {rs::util::DbErrorCode::AllocationFailure, {}};
  } catch (const std::exception& error) {
    return BackendResult<QueryResult>{
        rs::util::DbErrorCode::InvalidParameter, error.what()};
  }
  auto write_result = write_all_result(query_msg, deadline);
  if (write_result.has_error()) {
    return BackendResult<QueryResult>{write_result.error(), write_result.error_message()};
  }
  
  return read_query_result(deadline, ResponseKind::PreparedExecution);
}

BackendResult<QueryResult> GenericDatabaseConnection::describe_statement_impl(
    std::string_view sql,
    std::span<const QueryParameterType> parameter_types,
    rs::util::Deadline deadline) {
  if (!connected_) {
    return BackendResult<QueryResult>{
        rs::util::DbErrorCode::NotConnected, "Not connected"};
  }

  std::vector<std::byte> request;
  try {
    request = parser_->create_statement_description(sql, parameter_types, settings_.input_limits.max_request_wire_bytes);
  } catch (const RequestWireLimitExceeded&) {
    throw;
  } catch (const std::bad_alloc&) {
    return {rs::util::DbErrorCode::AllocationFailure, {}};
  } catch (const std::exception& error) {
    return BackendResult<QueryResult>{
        rs::util::DbErrorCode::InvalidParameter, error.what()};
  }
  auto write_result = write_all_result(request, deadline);
  if (write_result.has_error()) {
    return BackendResult<QueryResult>{
        write_result.error(), write_result.error_message()};
  }
  return read_query_result(deadline, ResponseKind::Description);
}

BackendResult<QueryResult> GenericDatabaseConnection::read_query_result(
    rs::util::Deadline deadline, ResponseKind kind) {
  enum class DescriptionPhase { Parse, Parameters, Result, Complete, Error };
  std::vector<Message> messages;
  std::optional<std::string> query_error;
  std::string query_error_sqlstate;
  bool saw_completion = false;
  auto description_phase = DescriptionPhase::Parse;

  std::size_t wire_bytes = 0;
  std::size_t message_count = 0;
  std::size_t rows = 0, cells = 0, results = 0;
  std::size_t metadata_entries = 0, metadata_name_bytes = 0;
  while (true) {
    if (message_count == settings_.response_limits.max_messages) {
      mark_transport_failed();
      return {rs::util::DbErrorCode::ResourceLimit, "Database response message limit exceeded"};
    }
    auto msg_result = read_message_result(deadline, settings_.response_limits.max_wire_bytes - wire_bytes);
    if (msg_result.has_error()) {
      return BackendResult<QueryResult>{msg_result.error(), msg_result.error_message()};
    }
    
    wire_bytes += msg_result->size();
    ++message_count;
    try {
      auto msg = parser_->parse_message(*msg_result);
      if (query_error && msg.tag != 'N' && msg.tag != 'S' &&
          msg.tag != 'A' && msg.tag != 'Z') {
        throw std::runtime_error(
            "PostgreSQL result frame arrived after ErrorResponse");
      }
      if (msg.tag == 'G' || msg.tag == 'H' || msg.tag == 'W') {
        disconnect();
        return BackendResult<QueryResult>{
            rs::util::DbErrorCode::UnsupportedFeature,
            "PostgreSQL COPY streaming is not supported"};
      }
      if (msg.tag == 'R' || msg.tag == 'K') {
        mark_transport_failed();
        return BackendResult<QueryResult>{
            rs::util::DbErrorCode::ProtocolError,
            "PostgreSQL startup frame arrived during query"};
      }
      if (kind == ResponseKind::Description) {
        switch (msg.tag) {
          case '1':
            if (description_phase != DescriptionPhase::Parse) {
              throw std::runtime_error("PostgreSQL ParseComplete out of sequence");
            }
            description_phase = DescriptionPhase::Parameters;
            break;
          case 't':
            if (description_phase != DescriptionPhase::Parameters) {
              throw std::runtime_error(
                  "PostgreSQL ParameterDescription out of sequence");
            }
            description_phase = DescriptionPhase::Result;
            break;
          case 'T':
          case 'n':
            if (description_phase != DescriptionPhase::Result) {
              throw std::runtime_error(
                  "PostgreSQL statement description out of sequence");
            }
            description_phase = DescriptionPhase::Complete;
            break;
          case 'E':
            description_phase = DescriptionPhase::Error;
            break;
          case 'N':
          case 'S':
          case 'A':
          case 'Z':
            break;
          default:
            throw std::runtime_error(
                "Unexpected PostgreSQL statement description frame");
        }
      } else {
        switch (msg.tag) {
          case '1': // ParseComplete
          case '2': // BindComplete
          case 't': // ParameterDescription
          case 'n': // NoData
            if (kind == ResponseKind::SimpleExecution) {
              throw std::runtime_error(
                  "PostgreSQL extended-query frame arrived during simple query");
            }
            break;
          case 'T': // RowDescription
          case 'D': // DataRow
          case 'C': // CommandComplete
          case 'I': // EmptyQueryResponse
          case 'E': // ErrorResponse
          case 'N': // NoticeResponse
          case 'S': // ParameterStatus
          case 'A': // NotificationResponse
          case 'Z': // ReadyForQuery
            break;
          default:
            throw std::runtime_error(
                "Unexpected PostgreSQL query response frame");
        }
      }
      // This session implements PostgreSQL-family framing. Count before
      // retaining frames and before the parser reserves decoded containers.
      const auto& limits = settings_.result_limits;
      const auto count = [&]() -> std::size_t {
        if (msg.payload.size() < 2) throw std::runtime_error("Incomplete PostgreSQL result count");
        return (std::to_integer<unsigned>(msg.payload[0]) << 8) |
            std::to_integer<unsigned>(msg.payload[1]);
      };
      bool exceeded = false;
      if (msg.tag == 'T' || msg.tag == 't') {
        const auto columns = count();
        exceeded = columns > limits.max_columns_per_description ||
            columns > limits.max_metadata_entries - metadata_entries;
        if (!exceeded) metadata_entries += columns;
        if (!exceeded && msg.tag == 'T') {
          std::size_t offset = 2;
          for (std::size_t column = 0; column < columns; ++column) {
            const auto start = offset;
            while (offset < msg.payload.size() && msg.payload[offset] != std::byte{0}) ++offset;
            if (offset == msg.payload.size()) throw std::runtime_error("Unterminated PostgreSQL column name");
            const auto name_bytes = offset - start;
            if (name_bytes > limits.max_column_name_bytes ||
                name_bytes > limits.max_metadata_name_bytes - metadata_name_bytes) {
              exceeded = true;
              break;
            }
            metadata_name_bytes += name_bytes;
            ++offset;
            // PostgreSQL RowDescription has 18 fixed bytes after each name.
            if (msg.payload.size() - offset < 18) throw std::runtime_error("Incomplete PostgreSQL column metadata");
            offset += 18;
          }
          if (!exceeded && offset != msg.payload.size()) throw std::runtime_error("Trailing PostgreSQL column metadata");
        }
      } else if (msg.tag == 'D') {
        const auto columns = count();
        exceeded = rows == limits.max_rows || columns > limits.max_cells - cells;
        if (!exceeded) { ++rows; cells += columns; }
      } else if (msg.tag == 'C' || msg.tag == 'E' || msg.tag == 'I') {
        exceeded = results == limits.max_results;
        if (!exceeded) ++results;
      }
      if (exceeded) {
        mark_transport_failed();
        return {rs::util::DbErrorCode::ResourceLimit, "Database decoded result count limit exceeded"};
      }
      if (msg.tag == 'S') {
        auto status_result = record_parameter_status(msg);
        if (status_result.has_error()) {
          mark_transport_failed();
          return BackendResult<QueryResult>{
              status_result.error(), status_result.error_message()};
        }
      }
      if (parser_->is_error_response(msg) && !query_error) {
        query_error = parser_->extract_error_message(msg);
        query_error_sqlstate = parser_->extract_error_sqlstate(msg);
      }
      if (msg.tag == 'C' || msg.tag == 'E' || msg.tag == 'I') {
        saw_completion = true;
      }
      messages.push_back(msg);
      if (parser_->is_ready_for_query(msg)) {
        session_state_ = msg.payload.size() != 1 ? SessionState::Unknown :
            msg.payload[0] == std::byte{'I'} ? SessionState::Idle :
            msg.payload[0] == std::byte{'T'} ? SessionState::Transaction :
            SessionState::FailedTransaction;
        if (kind != ResponseKind::Description && !saw_completion) {
          mark_transport_failed();
          return BackendResult<QueryResult>{
              rs::util::DbErrorCode::ProtocolError,
              "PostgreSQL query ended without a completion response"};
        }
        if (kind == ResponseKind::Description &&
            description_phase != DescriptionPhase::Complete &&
            description_phase != DescriptionPhase::Error) {
          mark_transport_failed();
          return BackendResult<QueryResult>{
              rs::util::DbErrorCode::ProtocolError,
              "PostgreSQL statement description was incomplete"};
        }
        break;
      }
    } catch (const std::bad_alloc&) {
      mark_transport_failed();
      return {rs::util::DbErrorCode::AllocationFailure, {}};
    } catch (const std::exception& error) {
      mark_transport_failed();
      return BackendResult<QueryResult>{
          rs::util::DbErrorCode::ProtocolError, error.what()};
    }
  }

  try {
    auto result = parser_->extract_query_result(messages);
    if (has_binary_columns(result)) {
      return BackendResult<QueryResult>{
          rs::util::DbErrorCode::UnsupportedFeature,
          "PostgreSQL binary result format is not supported"};
    }
    if (query_error &&
        (result.error.has_value() || result.additional_results.empty())) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),
                         "Query error: " + *query_error};
      error.native_state = std::move(query_error_sqlstate);
      return error;
    }
    return BackendResult<QueryResult>{std::move(result)};
  } catch (const std::bad_alloc&) {
    mark_transport_failed();
    return {rs::util::DbErrorCode::AllocationFailure, {}};
  } catch (const std::exception& error) {
    mark_transport_failed();
    return BackendResult<QueryResult>{
        rs::util::DbErrorCode::ProtocolError, error.what()};
  }
}

std::string GenericDatabaseConnection::get_parameter(std::string_view key) const {
  auto it = server_params_.find(std::string(key));
  return (it != server_params_.end()) ? it->second : std::string{};
}

void GenericDatabaseConnection::write_all(const std::vector<std::byte>& data, rs::util::Deadline deadline) {
  auto result = write_all_result(data, deadline);
  if (result.has_error()) {
    rs::util::unwrap_or_throw(std::move(result));
  }
}

rs::util::Result<void> GenericDatabaseConnection::write_all_result(const std::vector<std::byte>& data, rs::util::Deadline deadline) {
  size_t offset = 0;
  while (offset < data.size()) {
    const auto requested = data.size() - offset;
    auto result = transport_->send(std::span<const std::byte>(data.data() + offset, requested), deadline);
    if (result.has_error()) {
      mark_transport_failed();
      return rs::util::Result<void>{result.error(), result.error_message()};
    }
    if (result->n > requested) {
      mark_transport_failed();
      return rs::util::Result<void>{
          rs::util::DbErrorCode::ProtocolError,
          "Transport write exceeded requested message bytes"};
    }
    if (result->n == 0) {
      mark_transport_failed();
      return rs::util::Result<void>{rs::util::DbErrorCode::NetworkError, "Write failed"};
    }
    offset += result->n;
  }
  return rs::util::Result<void>{};
}

std::vector<std::byte> GenericDatabaseConnection::read_message(rs::util::Deadline deadline) {
  auto result = read_message_result(deadline);
  if (result.has_error()) {
    rs::util::unwrap_or_throw(std::move(result));
  }
  return std::move(*result);
}

rs::util::Result<std::vector<std::byte>> GenericDatabaseConnection::read_message_result(rs::util::Deadline deadline, std::size_t remaining_bytes) {
  if (remaining_bytes < 5) {
    mark_transport_failed();
    return {rs::util::DbErrorCode::ResourceLimit, "Database response byte limit exceeded"};
  }
  // Read message header (1 byte tag + 4 bytes length)
  std::vector<std::byte> header(5);
  size_t offset = 0;
  
  while (offset < 5) {
    const auto requested = header.size() - offset;
    auto result = transport_->recv(std::span<std::byte>(header.data() + offset, requested), deadline);
    if (result.has_error()) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          result.error(), result.error_message()};
    }
    if (result->eof) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{rs::util::DbErrorCode::NetworkError, "Unexpected EOF"};
    }
    if (result->n > requested) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          rs::util::DbErrorCode::ProtocolError,
          "Transport read exceeded requested message bytes"};
    }
    if (result->n == 0) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          rs::util::DbErrorCode::NetworkError,
          "Transport read made no progress"};
    }
    offset += result->n;
  }
  
  // Extract length
  const auto* p = reinterpret_cast<const unsigned char*>(header.data());
  const std::uint32_t len = (static_cast<std::uint32_t>(p[1]) << 24) |
      (static_cast<std::uint32_t>(p[2]) << 16) |
      (static_cast<std::uint32_t>(p[3]) << 8) | p[4];
  
  const auto limit = allows_large_frame(static_cast<char>(header[0]))
      ? kLargeFrameLimit : kShortFrameLimit;
  if (len < 4 || len > limit) {
    mark_transport_failed();
    return rs::util::Result<std::vector<std::byte>>{rs::util::DbErrorCode::ProtocolError, "Invalid message length"};
  }
  if (header[0] == std::byte{'d'} || header[0] == std::byte{'c'}) {
    disconnect();
    return rs::util::Result<std::vector<std::byte>>{
        rs::util::DbErrorCode::ProtocolError,
        "PostgreSQL COPY frame arrived outside COPY mode"};
  }
  
  // Grow only as bytes arrive; an untrusted length must not preallocate it.
  const auto total_length = static_cast<std::size_t>(len) + 1;
  if (total_length > remaining_bytes) {
    mark_transport_failed();
    return {rs::util::DbErrorCode::ResourceLimit, "Database response byte limit exceeded"};
  }
  if ((header[0] == std::byte{'E'} || header[0] == std::byte{'N'}) &&
      static_cast<std::size_t>(len - 4) > settings_.result_limits.max_diagnostic_bytes) {
    mark_transport_failed();
    return {rs::util::DbErrorCode::ResourceLimit, "Database diagnostic byte limit exceeded"};
  }
  std::vector<std::byte> message;
  message.reserve(std::min<std::size_t>(total_length, 8192));
  message.insert(message.end(), header.begin(), header.end());

  while (message.size() < total_length) {
    const auto message_offset = message.size();
    const auto requested = std::min<std::size_t>(
        total_length - message_offset, 8192);
    message.resize(message_offset + requested);
    auto result = transport_->recv(
        std::span<std::byte>(message.data() + message_offset, requested),
        deadline);
    if (result.has_error()) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          result.error(), result.error_message()};
    }
    if (result->eof) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{rs::util::DbErrorCode::NetworkError, "Unexpected EOF"};
    }
    if (result->n > requested) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          rs::util::DbErrorCode::ProtocolError,
          "Transport read exceeded requested message bytes"};
    }
    if (result->n == 0) {
      mark_transport_failed();
      return rs::util::Result<std::vector<std::byte>>{
          rs::util::DbErrorCode::NetworkError,
          "Transport read made no progress"};
    }
    message.resize(message_offset + result->n);
  }
  
  return rs::util::Result<std::vector<std::byte>>{std::move(message)};
}

BackendResult<void> GenericDatabaseConnection::perform_authentication_result(rs::util::Deadline deadline) {
  bool authenticated = false;
  auto failure = [&](auto code, std::string message) {
    BackendResult<void> result{code, std::move(message)};
    result.backend_error().operation = authenticated ? BackendOperation::Startup : BackendOperation::Authenticate;
    return result;
  };
  std::size_t wire_bytes = 0;
  std::size_t message_count = 0;
  try {
    while (true) {
      if (message_count == settings_.startup_response_limits.max_messages) {
        return failure(rs::util::DbErrorCode::ResourceLimit, "Database startup message limit exceeded");
      }
      auto msg_result = read_message_result(deadline,
          settings_.startup_response_limits.max_wire_bytes - wire_bytes);
      if (msg_result.has_error()) {
        return failure(msg_result.error(), msg_result.error_message());
      }
    
      wire_bytes += msg_result->size();
      ++message_count;
      auto msg = parser_->parse_message(*msg_result);
    
      if (msg.tag == 'R') { // Authentication
        if (authenticated) {
          return failure(
              rs::util::DbErrorCode::ProtocolError,
              "Authentication request arrived after AuthenticationOk");
        }
        auto auth_req = parser_->parse_auth_request(msg.payload);
      
        if (auth_req.type == AuthenticationRequest::Type::None) {
          authenticated = true;
          continue; // Authentication successful
        }
      
        auto auth_response = parser_->create_auth_response(
            auth_req, settings_.password, settings_.user,
            peer_identity_verified_, settings_.input_limits.max_auth_wire_bytes);
        if (!auth_response.empty()) {
          auto write_result = write_all_result(auth_response, deadline);
          if (write_result.has_error()) {
            return failure(write_result.error(), write_result.error_message());
          }
        }
      }
      else if (msg.tag == 'S') { // ParameterStatus
        if (!authenticated) {
          return failure(
              rs::util::DbErrorCode::ProtocolError,
              "ParameterStatus arrived before AuthenticationOk");
        }
        auto status_result = record_parameter_status(msg);
        if (status_result.has_error()) return failure(status_result.error(), status_result.error_message());
      }
      else if (msg.tag == 'K') { // BackendKeyData
        if (!authenticated) {
          return failure(
              rs::util::DbErrorCode::ProtocolError,
              "BackendKeyData arrived before AuthenticationOk");
        }
      }
      else if (parser_->is_error_response(msg)) {
        const auto message = parser_->extract_error_message(msg);
        const auto sqlstate = parser_->extract_error_sqlstate(msg);
        const bool authentication_error =
            !authenticated || sqlstate.starts_with("28");
        auto result = failure(
            authentication_error ? rs::util::DbErrorCode::AuthenticationFailed
                                 : rs::util::DbErrorCode::ConnectionFailed,
            (authentication_error ? "Authentication failed: "
                                  : "Startup failed: ") +
                message);
        result.backend_error().native_state = sqlstate;
        result.backend_error().operation = authentication_error ? BackendOperation::Authenticate : BackendOperation::Startup;
        return result;
      }
      else if (parser_->is_ready_for_query(msg)) {
        if (!authenticated) {
          return failure(
              rs::util::DbErrorCode::ProtocolError,
              "ReadyForQuery arrived before AuthenticationOk");
        }
        session_state_ = msg.payload.size() != 1 ? SessionState::Unknown :
            msg.payload[0] == std::byte{'I'} ? SessionState::Idle :
            msg.payload[0] == std::byte{'T'} ? SessionState::Transaction :
            SessionState::FailedTransaction;
        break; // Ready for queries
      }
      else if (msg.tag == 'N' && authenticated) {
        continue; // NoticeResponse may accompany backend startup
      }
      else {
        return failure(
            rs::util::DbErrorCode::ProtocolError,
            "Unexpected PostgreSQL startup message");
      }
    }
  } catch (const RequestWireLimitExceeded&) {
    return failure(rs::util::DbErrorCode::ResourceLimit, "Database authentication request limit exceeded");
  } catch (const std::bad_alloc&) {
    return failure(rs::util::DbErrorCode::AllocationFailure, {});
  } catch (const std::exception& error) {
    return failure(
        rs::util::DbErrorCode::ProtocolError,
        std::string("Invalid authentication exchange: ") + error.what());
  }
  return {};
}

rs::util::Result<void> GenericDatabaseConnection::record_parameter_status(
    const Message& msg) {
  const auto key_end = std::find(
      msg.payload.begin(), msg.payload.end(), std::byte{0});
  const auto value_begin = key_end == msg.payload.end()
      ? msg.payload.end() : std::next(key_end);
  const auto value_end = std::find(
      value_begin, msg.payload.end(), std::byte{0});
  if (key_end == msg.payload.begin() || key_end == msg.payload.end() ||
      value_end == msg.payload.end() ||
      std::next(value_end) != msg.payload.end()) {
    return rs::util::Result<void>{
        rs::util::DbErrorCode::ProtocolError,
        "Malformed PostgreSQL ParameterStatus message"};
  }
  const auto* bytes = reinterpret_cast<const char*>(msg.payload.data());
  const auto key_size = static_cast<std::size_t>(
      std::distance(msg.payload.begin(), key_end));
  const auto value_offset = key_size + 1;
  const auto value_size = static_cast<std::size_t>(
      std::distance(value_begin, value_end));
  server_params_[std::string(bytes, key_size)] =
      std::string(bytes + value_offset, value_size);
  return {};
}

rs::util::Result<std::string> GenericDatabaseConnection::catalog_query(
    const CatalogRequest&) const {
  return {rs::util::DbErrorCode::UnsupportedFeature,
          "Catalog discovery is not supported by this backend"};
}

TransactionCapabilities GenericDatabaseConnection::transaction_capabilities() const {
  return {};
}

BackendResult<void> GenericDatabaseConnection::transaction(
    TransactionAction, rs::util::Deadline) {
  return local_backend_error(LocalFailure::Unsupported,
      "Transactions are not supported by this backend", BackendOperation::Transaction, session_state());
}

BackendResult<void> GenericDatabaseConnection::set_transaction_isolation(
    TransactionIsolation, rs::util::Deadline) {
  return local_backend_error(LocalFailure::Unsupported,
      "Transaction isolation is not supported by this backend", BackendOperation::SetTransactionIsolation, session_state());
}

} // namespace rs::core::database
