#pragma once
#include "authentication.h"
#include "query_wire.h"
#include "core/transport/tls_configurable_transport.h"

namespace rs::core::database::mysql {
// Bounded internal S3 session. No provider/ODBC registration, prepared statements,
// warning delivery, pooling, or multi-result support. Owns the transport exclusively.
class MySqlSession final : public IDatabaseConnection {
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
  BackendResult<QueryResult> execute_prepared(std::string_view,std::span<const QueryParameter>,rs::util::Deadline) override {
    return local_backend_error(LocalFailure::Unsupported,"MySQL prepared execution is not implemented",
        BackendOperation::ExecutePrepared,state_);
  }
  BackendResult<QueryResult> execute_query(std::string_view sql,rs::util::Deadline deadline) override {
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
      auto result=read_result(reader);
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
  rs::util::Result<QueryResult> read_result(Reader& reader) {
    using rs::util::DbErrorCode;
    auto first=reader.next();if (!first) return {first.error()};
    if ((*first)[0]==std::byte{255}) return server_error(*first);
    QueryResult result;
    if ((*first)[0]==std::byte{0}) {
      auto done=query_detail::completion(*first,false);if (!done) return {done.error()};
      result.affected_rows=done->affected;result.statement_kind=StatementKind::Unknown;set_state(done->status);return result;
    }
    query_detail::Cursor c(*first);std::uint64_t count{};
    if (!c.length(count) || c.remaining() || !count) return {DbErrorCode::ProtocolError};
    if (count>result_limits_.max_columns_per_description || count>result_limits_.max_metadata_entries/6)
      return {DbErrorCode::ResourceLimit};
    std::size_t names{};
    for (std::uint64_t i=0;i<count;++i) {
      auto packet=reader.next();if (!packet) return {packet.error()};
      if ((*packet)[0]==std::byte{255}) return server_error(*packet);
      std::size_t metadata_bytes{};
      auto column=query_detail::column(*packet,result_limits_,&metadata_bytes);if (!column) return {column.error()};
      if (metadata_bytes>result_limits_.max_metadata_name_bytes-names) return {DbErrorCode::ResourceLimit};
      names+=metadata_bytes;result.columns.push_back(std::move(*column));
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
      auto row=query_detail::row(*packet,result.columns,result.rows.size(),result.cell_errors);
      if (!row) return {row.error()};
      result.rows.push_back(std::move(*row));
    }
  }
  rs::util::Result<QueryResult> server_error(std::span<const std::byte> bytes) const {
    if (bytes.size()>result_limits_.max_diagnostic_bytes) return {rs::util::DbErrorCode::ResourceLimit};
    if (bytes.size()<9 || bytes[3]!=std::byte{'#'}) return {rs::util::DbErrorCode::ProtocolError};
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
  BackendError retiring_failure(std::error_code code) {
    // Snapshot reflects the retirement performed by the guard before return.
    auto error=failure(code,BackendOperation::ExecuteDirect);error.session_state=SessionState::Disconnected;return error;
  }
  std::unique_ptr<rs::core::transport::ITransport> transport_;
  bool connected_{};SessionState state_{SessionState::Disconnected};std::string version_;
  ResponseLimits response_limits_;ResultLimits result_limits_;InputLimits input_limits_;
};
}
