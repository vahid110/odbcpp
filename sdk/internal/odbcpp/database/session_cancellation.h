#pragma once

#include "odbcpp/util/deadline.h"
#include <cstdint>
#include <memory>
#include <string_view>

namespace rs::core::database {

struct CancellationOutcome {
  bool claimed{false};
  bool confirmed{false};
  bool retire{false};
};

// Retained opaque operation endpoint, independent of a mutable/raw session.
// request() may use only its own secondary transport, never the main stream.
class SessionCancellation {
 public:
  virtual ~SessionCancellation() = default;
  virtual bool request() noexcept = 0;
  // Only the executing owner seals publication. No later claim can target a
  // new generation after this atomic completion boundary.
  virtual CancellationOutcome seal() noexcept = 0;
  virtual bool next_phase() noexcept = 0;
};

// Internal optional facet: public IDatabaseConnection vtable is unchanged.
// Acquisition is serialized with ordinary lease access, requests are independent.
class IBackendCancellation {
 public:
  virtual ~IBackendCancellation() = default;
  virtual bool supports_server_cancellation() const noexcept = 0;
  virtual bool cancellation_request_eligible(std::string_view sql) const noexcept = 0;
  virtual std::shared_ptr<SessionCancellation> arm_cancellation(
      std::uint64_t generation, rs::util::Deadline original) = 0;
  virtual bool continue_cancellation(const std::shared_ptr<SessionCancellation>&) noexcept = 0;
  virtual CancellationOutcome finish_cancellation(const std::shared_ptr<SessionCancellation>&) noexcept = 0;
};
} // namespace rs::core::database
