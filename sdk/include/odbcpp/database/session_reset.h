#pragma once

#include "odbcpp/database/backend_result.h"
#include "odbcpp/util/deadline.h"

namespace rs::core::database {

// Cleanup within the original authenticated physical session. This profile
// does not validate credential freshness/generations or authorize a new borrower.
enum class SessionResetProfile { SameAuthenticatedServerSession };

// Optional session-owned facet, stable for the session lifetime. Callers must
// serialize reset with all session operations. One original absolute deadline
// covers every cleanup exchange. Failure retires; success owns an Idle snapshot.
// No reconnect/replay. Shared policy still owns credential/cache isolation and
// exclusive leases; this narrow profile alone is not pooling qualification.
class ISessionReset {
 public:
  virtual ~ISessionReset() = default;
  virtual SessionResetProfile reset_profile() const noexcept = 0;
  virtual BackendResult<void> reset_session(rs::util::Deadline deadline) = 0;
};

} // namespace rs::core::database
