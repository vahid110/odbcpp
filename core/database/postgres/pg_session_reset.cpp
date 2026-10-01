#include "pg_database_connection.h"
#include <new>

namespace rs::core::database::postgres {

BackendResult<void> PgDatabaseConnection::reset_session(rs::util::Deadline deadline) {
  if (!reset_profile_) {
    return local_backend_error(LocalFailure::Unsupported,
        "Session reset profile is unavailable", BackendOperation::ResetSession, session_state());
  }
  const auto retire = [this](BackendError error) -> BackendResult<void> {
    disconnect();
    error.operation = BackendOperation::ResetSession;
    error.session_state = SessionState::Disconnected;
    error.disposition = SessionDisposition::Retire;
    error.retry_safe.reset();
    return error;
  };
  try {
    const auto state = session_state();
    if (state == SessionState::Disconnected) {
      return retire(BackendError{rs::util::make_error_code(rs::util::DbErrorCode::NotConnected), "Not connected"});
    }
    if (state == SessionState::Unknown) {
      return retire(BackendError{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError), "Unknown reset session state"});
    }
    const auto cleanup = [&](std::string_view sql) -> BackendResult<void> {
      auto result = execute_cleanup_query(sql, sql, deadline);
      if (!result) return retire(std::move(result.backend_error()));
      if (result.session_snapshot() != SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable} ||
          !result->rows.empty() || !result->columns.empty() || result->affected_rows != 0 ||
          !result->additional_results.empty() || !result->cell_errors.empty() || result->error ||
          !result->normalized_parameter_types.empty() || result->statement_kind != StatementKind::Unknown) {
        return retire(BackendError{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
                                  "Unexpected session reset response"});
      }
      return BackendResult<void>{result.session_snapshot()};
    };
    // DISCARD ALL is forbidden in a transaction. Do not COMMIT borrower work.
    if (state == SessionState::Transaction || state == SessionState::FailedTransaction) {
      auto rolled_back = cleanup("ROLLBACK");
      if (!rolled_back) return rolled_back;
    }
    return cleanup("DISCARD ALL");
  } catch (const std::bad_alloc&) {
    return retire(BackendError{rs::util::make_error_code(rs::util::DbErrorCode::AllocationFailure), {}});
  }
}

} // namespace rs::core::database::postgres
