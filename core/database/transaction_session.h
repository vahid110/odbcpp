#pragma once

#include "transaction.h"
#include "backend_result.h"
#include "core/util/deadline.h"

namespace rs::core::database {

// Optional session-owned facet. A borrowed pointer is stable for the session
// lifetime, including disconnected state; facet presence does not imply open.
// Calls remain serialized and use the caller's original absolute deadline.
class ITransactionSession {
 public:
  virtual ~ITransactionSession() = default;
  virtual TransactionCapabilities transaction_capabilities() const = 0;
  virtual BackendResult<void> transaction(TransactionAction action,
      rs::util::Deadline deadline) = 0;
  virtual BackendResult<void> set_transaction_isolation(TransactionIsolation level,
      rs::util::Deadline deadline) = 0;
};

} // namespace rs::core::database
