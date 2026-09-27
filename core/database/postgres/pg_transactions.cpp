#include "pg_database_connection.h"

namespace rs::core::database::postgres {

TransactionCapabilities PgDatabaseConnection::transaction_capabilities() const {
  return {true, true, TransactionIsolation::ReadCommitted, {true, true, true, true}};
}

rs::util::Result<void> PgDatabaseConnection::transaction(
    TransactionAction action, rs::util::Deadline deadline) {
  std::string_view command;
  switch (action) {
    case TransactionAction::Begin: command = "BEGIN"; break;
    case TransactionAction::Commit: command = "COMMIT"; break;
    case TransactionAction::Rollback: command = "ROLLBACK"; break;
    default: return {rs::util::DbErrorCode::InvalidParameter, "Invalid transaction action"};
  }
  auto result = execute_query(command, deadline);
  if (result.has_error()) return {result.error(), result.error_message()};
  return {};
}

rs::util::Result<void> PgDatabaseConnection::set_transaction_isolation(
    TransactionIsolation level, rs::util::Deadline deadline) {
  std::string_view name;
  switch (level) {
    case TransactionIsolation::ReadUncommitted: name = "READ UNCOMMITTED"; break;
    case TransactionIsolation::ReadCommitted: name = "READ COMMITTED"; break;
    case TransactionIsolation::RepeatableRead: name = "REPEATABLE READ"; break;
    case TransactionIsolation::Serializable: name = "SERIALIZABLE"; break;
    default: return {rs::util::DbErrorCode::InvalidParameter, "Invalid transaction isolation"};
  }
  auto result = execute_query(
      std::string("SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL ") +
      std::string(name), deadline);
  if (result.has_error()) return {result.error(), result.error_message()};
  return {};
}

} // namespace rs::core::database::postgres
