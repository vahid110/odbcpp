#pragma once

#include "odbc_types.h"
#include "core/database/transaction.h"
#include <optional>

namespace rs::odbc {

inline std::optional<rs::core::database::TransactionIsolation>
transaction_isolation_from_odbc(SQLULEN value) {
  using enum rs::core::database::TransactionIsolation;
  switch (value) {
    case SQL_TXN_READ_UNCOMMITTED: return ReadUncommitted;
    case SQL_TXN_READ_COMMITTED: return ReadCommitted;
    case SQL_TXN_REPEATABLE_READ: return RepeatableRead;
    case SQL_TXN_SERIALIZABLE: return Serializable;
    default: return std::nullopt;
  }
}

inline SQLUINTEGER transaction_isolation_to_odbc(rs::core::database::TransactionIsolation value) {
  using enum rs::core::database::TransactionIsolation;
  switch (value) {
    case ReadUncommitted: return SQL_TXN_READ_UNCOMMITTED;
    case ReadCommitted: return SQL_TXN_READ_COMMITTED;
    case RepeatableRead: return SQL_TXN_REPEATABLE_READ;
    case Serializable: return SQL_TXN_SERIALIZABLE;
  }
  return 0;
}

} // namespace rs::odbc
