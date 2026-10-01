#include "pg_database_connection.h"
#include "pg_backend_provider.h"

namespace rs::core::database::postgres {

TransactionCapabilities pg_transaction_capabilities() noexcept {
  return {true, true, TransactionIsolation::ReadCommitted,
          {true, true, true, true}};
}

TransactionCapabilities PgDatabaseConnection::transaction_capabilities() const {
  return pg_transaction_capabilities();
}

BackendResult<void> PgDatabaseConnection::transaction(
    TransactionAction action, rs::util::Deadline deadline) {
  std::string_view command;
  auto operation = BackendOperation::Transaction;
  switch (action) {
    case TransactionAction::Begin: command = "BEGIN"; operation = BackendOperation::BeginTransaction; break;
    case TransactionAction::Commit: command = "COMMIT"; operation = BackendOperation::CommitTransaction; break;
    case TransactionAction::Rollback: command = "ROLLBACK"; operation = BackendOperation::RollbackTransaction; break;
    default: return local_backend_error(LocalFailure::InvalidInput,
        "Invalid transaction action", operation, session_state());
  }
  auto result = execute_query(command, deadline);
  if (result.has_error()) {
    auto error = std::move(result.backend_error());
    error.operation = operation;
    return error;
  }
  return {};
}

BackendResult<void> PgDatabaseConnection::set_transaction_isolation(
    TransactionIsolation level, rs::util::Deadline deadline) {
  std::string_view name;
  switch (level) {
    case TransactionIsolation::ReadUncommitted: name = "READ UNCOMMITTED"; break;
    case TransactionIsolation::ReadCommitted: name = "READ COMMITTED"; break;
    case TransactionIsolation::RepeatableRead: name = "REPEATABLE READ"; break;
    case TransactionIsolation::Serializable: name = "SERIALIZABLE"; break;
    default: return local_backend_error(LocalFailure::InvalidInput,
        "Invalid transaction isolation", BackendOperation::SetTransactionIsolation, session_state());
  }
  auto result = execute_query(
      std::string("SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL ") +
      std::string(name), deadline);
  if (result.has_error()) {
    auto error = std::move(result.backend_error());
    error.operation = BackendOperation::SetTransactionIsolation;
    return error;
  }
  return {};
}

} // namespace rs::core::database::postgres
