#pragma once

#include "odbcpp/database/backend_result.h"
#include "odbcpp/util/deadline.h"

namespace rs::core::database {

// Optional, session-owned active probe. The borrowed facet stays valid for the
// session lifetime, including disconnected state. Callers must serialize probes with all
// other session operations; probes retain the caller's absolute deadline.
// Success proves one completed exchange and owns its final session snapshot.
// It does not reset session state, validate credential expiry, grant a pool lease
// or guarantee that the next operation will succeed. No reconnect or replay.
class ISessionHealth {
 public:
  virtual ~ISessionHealth() = default;
  virtual BackendResult<void> check_health(rs::util::Deadline deadline) = 0;
};

} // namespace rs::core::database
