#pragma once
#include "authentication.h"
#include "error_wire.h"
#include "query_wire.h"
#include "prepared_wire.h"
#include "core/transport/tls_configurable_transport.h"

namespace rs::core::database::mysql {
// Bounded internal S3 session. No provider/ODBC registration, statement caching,
// warning delivery, pooling, or multi-result support. Owns the transport exclusively.
class MySqlSession final : public IDatabaseConnection, public ITransactionSession {
 public:
  explicit MySqlSession(std::unique_ptr<rs::core::transport::ITransport> transport)
      : transport_(std::move(transport)) {}
  ~MySqlSession() override { disconnect(); }
  MySqlSession(const MySqlSession&)=delete;
  MySqlSession& operator=(const MySqlSession&)=delete;
  bool is_connected() const override { return connected_; }
  SessionState session_state() const override { return state_; }
  std::string server_version() const override { return connected_?version_:std::string{}; }
  void disconnect() override {
    if (connected_) transport_->close();
    connected_=false;state_=SessionState::Disconnected;version_.clear();
  }
  BackendResult<void> connect(const ConnectionSettings& settings) override {
    if (settings.redshift_catalog_mode)
      return local_backend_error(LocalFailure::InvalidInput,
          "RedshiftCatalogMode requires Redshift", BackendOperation::Connect, session_state());
    if (connected_) return local_backend_error(LocalFailure::InvalidInput,
        "MySQL session is already connected",BackendOperation::Connect,state_);
    if (!transport_ || !settings.use_ssl || !valid_resource_limits(settings) || settings.timeout.count()<=0 ||
        settings.host.empty() || settings.host.find('\0')!=std::string::npos || !settings.port ||
        settings.user.empty() || settings.user.find('\0')!=std::string::npos ||
        settings.password.find('\0')!=std::string::npos || settings.ssl_ca_file.find('\0')!=std::string::npos ||
        settings.ssl_ca_dir.find('\0')!=std::string::npos || settings.database.find('\0')!=std::string::npos ||
        !rs::util::utf8_code_point_count(settings.database))
      return failure(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),BackendOperation::Connect);
    if (settings.user.size()>256 || settings.password.size()>=connection_packet_limit ||
        settings.ssl_ca_file.size()>settings.input_limits.max_connection_field_bytes ||
        settings.ssl_ca_dir.size()>settings.input_limits.max_connection_field_bytes ||
        settings.database.size()>settings.input_limits.max_connection_field_bytes ||
        settings.host.size()>settings.input_limits.max_connection_field_bytes ||
        settings.user.size()>settings.input_limits.max_connection_field_bytes ||
        settings.password.size()>settings.input_limits.max_connection_field_bytes)
      return failure(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),BackendOperation::Connect);
    // Authentication has a fixed five-packet bound, separate from command limits.
    // Reject tighter startup budgets until the authentication helper accepts them.
    constexpr std::size_t authentication_bound=5*(connection_packet_limit+4);
    if (settings.startup_response_limits.max_wire_bytes<authentication_bound ||
        settings.startup_response_limits.max_messages<5 || settings.input_limits.max_auth_wire_bytes<authentication_bound ||
        settings.input_limits.max_startup_wire_bytes<authentication_bound)
      return failure(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),BackendOperation::Connect);
    const auto selection_wire=settings.database.empty()?0:settings.database.size()+5;
    if (!settings.database.empty() && (settings.database.size()>=connection_packet_limit ||
        selection_wire>settings.input_limits.max_request_wire_bytes ||
        selection_wire>settings.input_limits.max_startup_wire_bytes-authentication_bound ||
        settings.startup_response_limits.max_messages<=5 ||
        settings.startup_response_limits.max_wire_bytes-authentication_bound<11))
      return failure(rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),BackendOperation::Connect);
    bool authentication_owns_cleanup=false;
    bool authenticated_live=false;
    try {
      auto* configurable=dynamic_cast<rs::core::transport::ITlsConfigurableTransport*>(transport_.get());
      if (!configurable) return failure(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature),BackendOperation::Connect);
      configurable->set_ca_locations(settings.ssl_ca_file,settings.ssl_ca_dir);
      const auto deadline=rs::util::make_deadline(settings.timeout);
      authentication_owns_cleanup=true;
      auto authenticated=authenticate_verified_tls(*transport_,settings.host,settings.port,settings.user,settings.password,
          deadline);
      if (!authenticated) return failure(authenticated.error(),BackendOperation::Authenticate);
      authenticated_live=true;
      result_limits_=settings.result_limits;
      if (!settings.database.empty()) {
        auto selected=select_database(settings.database,deadline,
            ResponseLimits{settings.startup_response_limits.max_wire_bytes-authentication_bound,
                           settings.startup_response_limits.max_messages-5});
        if (!selected) {
          transport_->close();authenticated_live=false;
          return failure(selected.error(),BackendOperation::Startup);
        }
      }
      connected_=true;state_=SessionState::Idle;
      version_=std::move(authenticated->verified.greeting.server_version);
      response_limits_=settings.response_limits;result_limits_=settings.result_limits;input_limits_=settings.input_limits;
      return BackendResult<void>{snapshot()};
    } catch (const std::bad_alloc&) {
      if (!authentication_owns_cleanup || authenticated_live) transport_->close();
      connected_=false;state_=SessionState::Disconnected;version_.clear();
      return failure(rs::util::make_error_code(rs::util::DbErrorCode::AllocationFailure),BackendOperation::Connect);
    } catch (...) {
      if (!authentication_owns_cleanup || authenticated_live) transport_->close();
      connected_=false;state_=SessionState::Disconnected;version_.clear();
      return failure(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),BackendOperation::Connect);
    }
  }
  ITransactionSession* transaction_session() noexcept override { return this; }
  TransactionCapabilities transaction_capabilities() const override {
    // Pinned InnoDB proof profile. DDL can implicitly commit in MySQL.
    return {true,false,TransactionIsolation::RepeatableRead,{true,true,true,true}};
  }
  BackendResult<void> transaction(TransactionAction action,rs::util::Deadline deadline) override {
    std::string_view command;auto operation=BackendOperation::Transaction;
    SessionState expected=SessionState::Idle;
    switch (action) {
      case TransactionAction::Begin:
        command="START TRANSACTION";operation=BackendOperation::BeginTransaction;expected=SessionState::Transaction;break;
      case TransactionAction::Commit: command="COMMIT AND NO CHAIN NO RELEASE";operation=BackendOperation::CommitTransaction;break;
      case TransactionAction::Rollback: command="ROLLBACK AND NO CHAIN NO RELEASE";operation=BackendOperation::RollbackTransaction;break;
      default:return local_backend_error(LocalFailure::InvalidInput,"Invalid MySQL transaction action",operation,state_);
    }
    if (connected_ && action==TransactionAction::Begin && state_!=SessionState::Idle)
      // A second START TRANSACTION would implicitly commit the active transaction.
      return local_backend_error(LocalFailure::InvalidInput,"MySQL transaction is already active",operation,state_);
    return transaction_command(command,operation,expected,deadline);
  }
  BackendResult<void> set_transaction_isolation(TransactionIsolation level,rs::util::Deadline deadline) override {
    constexpr auto operation=BackendOperation::SetTransactionIsolation;
    std::string_view command;
    switch (level) {
      case TransactionIsolation::ReadUncommitted:command="SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED";break;
      case TransactionIsolation::ReadCommitted:command="SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED";break;
      case TransactionIsolation::RepeatableRead:command="SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ";break;
      case TransactionIsolation::Serializable:command="SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE";break;
      default:return local_backend_error(LocalFailure::InvalidInput,"Invalid MySQL isolation level",operation,state_);
    }
    if (connected_ && state_!=SessionState::Idle)
      return local_backend_error(LocalFailure::InvalidInput,"MySQL isolation change requires idle session",operation,state_);
    return transaction_command(command,operation,SessionState::Idle,deadline);
  }
  BackendResult<QueryResult> execute_prepared(std::string_view sql,std::span<const QueryParameter> parameters,
                                             rs::util::Deadline deadline) override {
    using rs::util::DbErrorCode;
    constexpr auto operation=BackendOperation::ExecutePrepared;
    if (!connected_) return failure(rs::util::make_error_code(DbErrorCode::NotConnected),operation);
    if (sql.empty() || sql.find('\0')!=std::string_view::npos || !rs::util::utf8_code_point_count(sql))
      return local_backend_error(LocalFailure::InvalidInput,"Invalid MySQL prepared input",operation,state_);
    if (sql.size()>input_limits_.max_sql_bytes || sql.size()>=connection_packet_limit)
      return local_failure(rs::util::make_error_code(DbErrorCode::ResourceLimit),operation);
    struct Retire {
      MySqlSession& session;bool accepted{};
      ~Retire() { if (!accepted) session.disconnect(); }
    } retire{*this};
    try {
      // Validate and encode parameters before any protocol mutation. No SQL substitution.
      const auto prepare_size=sql.size()+5;
      if (prepare_size>input_limits_.max_request_wire_bytes ||
          input_limits_.max_request_wire_bytes-prepare_size<9) {
        BackendResult<QueryResult> rejected{local_failure(rs::util::make_error_code(DbErrorCode::ResourceLimit),operation)};
        retire.accepted=true;return rejected;
      }
      auto execution_limits=input_limits_;
      execution_limits.max_request_wire_bytes-=prepare_size+9;
      auto execution=prepared_detail::execute_request(0,parameters,execution_limits);
      if (!execution) {
        BackendResult<QueryResult> rejected{local_failure(execution.error(),operation)};
        retire.accepted=true;return rejected;
      }
      std::vector<std::byte> request(prepare_size);
      authentication_detail::frame(request,0);request[4]=std::byte{22};
      for (std::size_t i=0;i<sql.size();++i) request[5+i]=static_cast<std::byte>(sql[i]);
      auto sent=authentication_detail::send_all(*transport_,request,deadline);
      if (!sent) return retiring_failure(sent.error(),operation);
      Reader reader{*transport_,deadline,response_limits_};
      auto first=reader.next();if (!first) return retiring_failure(first.error(),operation);
      if ((*first)[0]==std::byte{255}) return retiring_failure(server_error(*first).error(),operation);
      auto prepared=prepared_detail::parse_prepare(*first);
      if (!prepared) return retiring_failure(prepared.error(),operation);
      if (prepared->parameters>input_limits_.max_parameters ||
          prepared->columns>result_limits_.max_columns_per_description ||
          static_cast<std::size_t>(prepared->columns)+prepared->parameters>result_limits_.max_metadata_entries/6)
        return retiring_failure(rs::util::make_error_code(DbErrorCode::ResourceLimit),operation);
      std::size_t names{},entries{};
      std::vector<NativeTypeInfo> parameter_types;
      auto metadata=read_prepared_metadata(reader,prepared->parameters,names,entries,&parameter_types);
      if (!metadata) return retiring_failure(metadata.error(),operation);
      metadata=read_prepared_metadata(reader,prepared->columns,names,entries,nullptr);
      if (!metadata) return retiring_failure(metadata.error(),operation);
      std::array<std::byte,9> close{};authentication_detail::frame(close,0);close[4]=std::byte{25};
      for (std::size_t i=0;i<4;++i) {
        const auto byte=static_cast<std::byte>((prepared->statement_id>>(8*i))&255);
        (*execution)[5+i]=byte;close[5+i]=byte;
      }
      if (prepared->parameters!=parameters.size()) {
        sent=authentication_detail::send_all(*transport_,close,deadline);
        if (!sent) return retiring_failure(sent.error(),operation);
        auto checked=verified_completion(deadline);if (!checked) return retiring_failure(checked.error(),operation);
        BackendResult<QueryResult> rejected{local_backend_error(LocalFailure::InvalidInput,
            "MySQL parameter count mismatch",operation,state_)};
        retire.accepted=true;return rejected;
      }
      sent=authentication_detail::send_all(*transport_,*execution,deadline);
      if (!sent) return retiring_failure(sent.error(),operation);
      reader.sequence=1; // New command, same cumulative response budget and deadline.
      auto result=read_result(reader,true,&names,&entries);
      if (!result) return retiring_failure(result.error(),operation);
      result->normalized_parameter_types=std::move(parameter_types);
      sent=authentication_detail::send_all(*transport_,close,deadline);
      if (!sent) return retiring_failure(sent.error(),operation);
      auto checked=verified_completion(deadline);if (!checked) return retiring_failure(checked.error(),operation);
      BackendResult<QueryResult> completed{std::move(*result),snapshot()};
      retire.accepted=true;return completed;
    } catch (const std::bad_alloc&) { return retiring_failure(rs::util::make_error_code(DbErrorCode::AllocationFailure),operation); }
      catch (...) { return retiring_failure(rs::util::make_error_code(DbErrorCode::ProtocolError),operation); }
  }
  BackendResult<QueryResult> execute_query(std::string_view sql,rs::util::Deadline deadline) override {
    return execute_direct(sql,deadline,false);
  }
 private:
  BackendResult<QueryResult> execute_direct(std::string_view sql,rs::util::Deadline deadline,bool control) {
    using rs::util::DbErrorCode;
    if (!connected_) return failure(rs::util::make_error_code(DbErrorCode::NotConnected),BackendOperation::ExecuteDirect);
    if (sql.empty() || sql.find('\0')!=std::string_view::npos || !rs::util::utf8_code_point_count(sql))
      return local_backend_error(LocalFailure::InvalidInput,"Invalid MySQL query input",BackendOperation::ExecuteDirect,state_);
    if (sql.size()>input_limits_.max_sql_bytes || sql.size()>=connection_packet_limit ||
        sql.size()+5>input_limits_.max_request_wire_bytes) {
      auto error=BackendError{rs::util::make_error_code(DbErrorCode::ResourceLimit),"MySQL query input limit exceeded"};
      error.operation=BackendOperation::ExecuteDirect;error.session_state=state_;error.disposition=snapshot().disposition;
      return error;
    }
    struct Retire {
      MySqlSession& session;bool accepted{};
      ~Retire() { if (!accepted) session.disconnect(); }
    } retire{*this};
    try {
      std::vector<std::byte> request(sql.size()+5);
      authentication_detail::frame(request,0);request[4]=std::byte{3};
      for (std::size_t i=0;i<sql.size();++i) request[5+i]=static_cast<std::byte>(sql[i]);
      auto sent=authentication_detail::send_all(*transport_,request,deadline);
      if (!sent) return retiring_failure(sent.error());
      Reader reader{*transport_,deadline,response_limits_};
      auto result=read_result(reader,false,nullptr,nullptr,control);
      if (!result) return retiring_failure(result.error());
      if (rs::util::Clock::now()>=deadline) return retiring_failure(rs::util::make_error_code(DbErrorCode::Timeout));
      auto* tls=dynamic_cast<rs::core::transport::IStartTlsTransport*>(transport_.get());
      if (!tls || !tls->peer_identity_verified()) return retiring_failure(rs::util::make_error_code(DbErrorCode::TLSError));
      retire.accepted=true;
      return BackendResult<QueryResult>{std::move(*result),snapshot()};
    } catch (const std::bad_alloc&) { return retiring_failure(rs::util::make_error_code(DbErrorCode::AllocationFailure)); }
      catch (...) { return retiring_failure(rs::util::make_error_code(DbErrorCode::ProtocolError)); }
  }
 private:
  struct Reader {
    rs::core::transport::ITransport& transport;rs::util::Deadline deadline;ResponseLimits limits;
    std::size_t wire{},messages{};std::uint8_t sequence{1};
    rs::util::Result<void> exact(std::span<std::byte> bytes) {
      using rs::util::DbErrorCode;
      while (!bytes.empty()) {
        if (rs::util::Clock::now()>=deadline) return {DbErrorCode::Timeout};
        auto* tls=dynamic_cast<rs::core::transport::IStartTlsTransport*>(&transport);
        if (!tls || !tls->peer_identity_verified()) return {DbErrorCode::TLSError};
        auto read=transport.recv(bytes,deadline);
        if (!read) return {read.error()};
        if (read->n>bytes.size()) return {DbErrorCode::ProtocolError};
        if (!read->n || read->eof) return {DbErrorCode::NetworkError};
        bytes=bytes.subspan(read->n);
      }
      return {};
    }
    rs::util::Result<std::vector<std::byte>> next() {
      using rs::util::DbErrorCode;
      if (messages>=limits.max_messages || limits.max_wire_bytes-wire<4) return {DbErrorCode::ResourceLimit};
      std::array<std::byte,4> header{};auto read=exact(header);if (!read) return {read.error()};
      wire+=4;++messages;
      const auto size=std::to_integer<std::size_t>(header[0]) |
          (std::to_integer<std::size_t>(header[1])<<8) | (std::to_integer<std::size_t>(header[2])<<16);
      if (header[3]!=static_cast<std::byte>(sequence++ ) || !size) return {DbErrorCode::ProtocolError};
      if (size>connection_packet_limit || size>limits.max_wire_bytes-wire) return {DbErrorCode::ResourceLimit};
      std::vector<std::byte> packet(size);wire+=size;
      read=exact(packet);if (!read) return {read.error()};return packet;
    }
  };
  // COM_INIT_DB sends the schema as a native protocol field, never SQL text.
  // Authentication and selection share one deadline and a bounded startup budget.
  rs::util::Result<void> select_database(std::string_view database,rs::util::Deadline deadline,
                                      ResponseLimits limits) {
    using rs::util::DbErrorCode;
    std::vector<std::byte> request(database.size()+5);
    authentication_detail::frame(request,0);request[4]=std::byte{2};
    for (std::size_t i=0;i<database.size();++i) request[5+i]=static_cast<std::byte>(database[i]);
    auto sent=authentication_detail::send_all(*transport_,request,deadline);
    if (!sent) return {sent.error()};
    Reader reader{*transport_,deadline,limits};
    auto packet=reader.next();if (!packet) return {packet.error()};
    if ((*packet)[0]==std::byte{255}) return {server_error(*packet).error()};
    auto done=query_detail::completion(*packet,false);if (!done) return {done.error()};
    if (done->affected || done->insert_id || (done->status&1)) return {DbErrorCode::ProtocolError};
    if (rs::util::Clock::now()>=deadline) return {DbErrorCode::Timeout};
    auto* tls=dynamic_cast<rs::core::transport::IStartTlsTransport*>(transport_.get());
    if (!tls || !tls->peer_identity_verified()) return {DbErrorCode::TLSError};
    return {};
  }
  rs::util::Result<void> verified_completion(rs::util::Deadline deadline) const {
    if (rs::util::Clock::now()>=deadline) return {rs::util::DbErrorCode::Timeout};
    auto* tls=dynamic_cast<rs::core::transport::IStartTlsTransport*>(transport_.get());
    if (!tls || !tls->peer_identity_verified()) return {rs::util::DbErrorCode::TLSError};
    return {};
  }
  rs::util::Result<void> read_prepared_metadata(Reader& reader,std::size_t count,
      std::size_t& names,std::size_t& entries,std::vector<NativeTypeInfo>* types) {
    using rs::util::DbErrorCode;
    for (std::size_t i=0;i<count;++i) {
      auto packet=reader.next();if (!packet) return {packet.error()};
      if ((*packet)[0]==std::byte{255}) return {server_error(*packet).error()};
      std::size_t bytes{};
      auto column=query_detail::column(*packet,result_limits_,&bytes);
      if (!column) return {column.error()};
      // Result-only support must not admit native decimal/DATE parameters.
      if (types && (column->normalized_type->type==ScalarType::Decimal ||
          column->normalized_type->type==ScalarType::Date)) return {DbErrorCode::UnsupportedFeature};
      if (bytes>result_limits_.max_metadata_name_bytes-names ||
          result_limits_.max_metadata_entries-entries<6) return {DbErrorCode::ResourceLimit};
      names+=bytes;entries+=6;
      if (types) types->push_back(*column->normalized_type);
    }
    if (count) {
      auto packet=reader.next();if (!packet) return {packet.error()};
      auto done=query_detail::completion(*packet,true);if (!done) return {done.error()};
      set_state(done->status);
    }
    return {};
  }
  rs::util::Result<QueryResult> read_result(Reader& reader,bool binary=false,
      std::size_t* accumulated_names=nullptr,std::size_t* accumulated_entries=nullptr,bool control=false) {
    using rs::util::DbErrorCode;
    auto first=reader.next();if (!first) return {first.error()};
    if ((*first)[0]==std::byte{255}) return server_error(*first);
    QueryResult result;
    if ((*first)[0]==std::byte{0}) {
      auto done=query_detail::completion(*first,false);if (!done) return {done.error()};
      if (control && done->insert_id) return {DbErrorCode::ProtocolError};
      result.affected_rows=done->affected;result.statement_kind=StatementKind::Unknown;set_state(done->status);return result;
    }
    if (control) return {DbErrorCode::ProtocolError};
    query_detail::Cursor c(*first);std::uint64_t count{};
    if (!c.length(count) || c.remaining() || !count) return {DbErrorCode::ProtocolError};
    if (count>result_limits_.max_columns_per_description || count>result_limits_.max_metadata_entries/6)
      return {DbErrorCode::ResourceLimit};
    std::size_t names=accumulated_names?*accumulated_names:0;
    std::size_t entries=accumulated_entries?*accumulated_entries:0;
    std::vector<prepared_detail::NativeColumn> native;
    for (std::uint64_t i=0;i<count;++i) {
      auto packet=reader.next();if (!packet) return {packet.error()};
      if ((*packet)[0]==std::byte{255}) return server_error(*packet);
      std::size_t metadata_bytes{};
      prepared_detail::NativeColumn wire;
      auto column=query_detail::column(*packet,result_limits_,&metadata_bytes,&wire.type,&wire.unsigned_value);if (!column) return {column.error()};
      if (metadata_bytes>result_limits_.max_metadata_name_bytes-names) return {DbErrorCode::ResourceLimit};
      if (result_limits_.max_metadata_entries-entries<6) return {DbErrorCode::ResourceLimit};
      names+=metadata_bytes;entries+=6;result.columns.push_back(std::move(*column));
      native.push_back(wire);
    }
    auto metadata_end=reader.next();if (!metadata_end) return {metadata_end.error()};
    auto end=query_detail::completion(*metadata_end,true);if (!end) return {end.error()};
    while (true) {
      auto packet=reader.next();if (!packet) return {packet.error()};
      if ((*packet)[0]==std::byte{255}) return server_error(*packet);
      if ((*packet)[0]==std::byte{254} && packet->size()<9) {
        end=query_detail::completion(*packet,true);if (!end) return {end.error()};
        set_state(end->status);result.statement_kind=StatementKind::SelectCursor;return result;
      }
      if (result.rows.size()>=result_limits_.max_rows ||
          result.rows.size()>=result_limits_.max_cells/result.columns.size()) return {DbErrorCode::ResourceLimit};
      auto row=binary?prepared_detail::binary_row(*packet,native,result.columns,result.rows.size(),result.cell_errors):
          query_detail::row(*packet,result.columns,result.rows.size(),result.cell_errors,native);
      if (!row) return {row.error()};
      result.rows.push_back(std::move(*row));
    }
  }
  rs::util::Result<QueryResult> server_error(std::span<const std::byte> bytes) const {
    if (bytes.size()>result_limits_.max_diagnostic_bytes) return {rs::util::DbErrorCode::ResourceLimit};
    if (!valid_protocol41_error_packet(bytes)) return {rs::util::DbErrorCode::ProtocolError};
    return {rs::util::DbErrorCode::QueryFailed,"MySQL server rejected query"};
  }
  void set_state(std::uint16_t status) { state_=(status&1)?SessionState::Transaction:SessionState::Idle; }
  SessionSnapshot snapshot() const {
    return {state_,state_==SessionState::Idle?SessionDisposition::Reusable:
        state_==SessionState::Disconnected?SessionDisposition::Retire:SessionDisposition::ResetRequired};
  }
  BackendError failure(std::error_code code,BackendOperation operation) const {
    BackendError error{code,"MySQL session operation failed"};error.operation=operation;
    error.session_state=state_;error.disposition=SessionDisposition::Retire;return error;
  }
  BackendResult<void> transaction_command(std::string_view command,BackendOperation operation,
      SessionState expected,rs::util::Deadline deadline) {
    auto result=execute_direct(command,deadline,true);
    if (!result) {
      auto error=std::move(result.backend_error());error.operation=operation;return error;
    }
    if (!result->columns.empty() || !result->rows.empty() || result->affected_rows ||
        !result->cell_errors.empty() || result.session_snapshot().state!=expected) {
      disconnect();return failure(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),operation);
    }
    return BackendResult<void>{result.session_snapshot()};
  }
  BackendError local_failure(std::error_code code,BackendOperation operation) const {
    auto error=failure(code,operation);error.disposition=snapshot().disposition;return error;
  }
  BackendError retiring_failure(std::error_code code,BackendOperation operation=BackendOperation::ExecuteDirect) {
    // Snapshot reflects the retirement performed by the guard before return.
    auto error=failure(code,operation);error.session_state=SessionState::Disconnected;return error;
  }
  std::unique_ptr<rs::core::transport::ITransport> transport_;
  bool connected_{};SessionState state_{SessionState::Disconnected};std::string version_;
  ResponseLimits response_limits_;ResultLimits result_limits_;InputLimits input_limits_;
};
}
