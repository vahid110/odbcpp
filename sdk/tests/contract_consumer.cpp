#include <core/database/backend_provider.h>
#include <core/transport/i_transport.h>

using namespace rs::core::database;

// Build-only consumer proof: no concrete protocol, crypto, ODBC or composition.
class ConsumerSession final : public IDatabaseConnection {
 public:
  BackendResult<void> connect(const ConnectionSettings&) override {
    open_ = true;
    return BackendResult<void>{SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}};
  }
  void disconnect() override { open_ = false; }
  bool is_connected() const override { return open_; }
  SessionState session_state() const override { return open_ ? SessionState::Idle : SessionState::Disconnected; }
  BackendResult<QueryResult> execute_query(std::string_view sql, rs::util::Deadline) override {
    if (!open_) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::NotConnected), "Closed session"};
      error.session_state = SessionState::Disconnected;
      error.disposition = SessionDisposition::Retire;
      return error;
    }
    QueryResult result;
    result.columns = {{"text", NativeTypeInfo{ScalarType::VarChar, 32, 0, true}}};
    result.rows = {{std::string(sql)}};
    return BackendResult<QueryResult>{std::move(result), {SessionState::Idle, SessionDisposition::Reusable}};
  }
  BackendResult<QueryResult> execute_prepared(std::string_view sql,
      std::span<const QueryParameter>, rs::util::Deadline deadline) override {
    return execute_query(sql, deadline);
  }
  std::string server_version() const override { return "consumer"; }
 private:
  bool open_{};
};

int main() {
  if (ConnectionSettings{}.port != 0) return 5;
  ConsumerSession concrete;
  IDatabaseConnection& session = concrete;
  if (session.session_reset() || session.session_health() || session.catalog_queries() || session.transaction_session() || session.statement_description()) return 1;
  auto closed = session.execute_query("before open", rs::util::Deadline::max());
  if (!closed.has_error() || closed.session_snapshot().disposition != SessionDisposition::Retire) return 2;
  if (!session.connect(ConnectionSettings{})) return 3;
  std::string input = "owned";
  auto result = session.execute_query(input, rs::util::Deadline::max());
  input.assign("changed");
  session.disconnect();
  if (!result || result->rows.at(0).at(0) != "owned" ||
      result.session_snapshot() != SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}) return 4;
  return 0;
}
