#pragma once
#include "odbcpp/database/session_cancellation.h"
#include "odbcpp/transport/cancellation_wait.h"
#include <string_view>

namespace rs::core::database {
// Main-owner-only framing hooks. None are callable through the public endpoint.
class SessionCancellationWire : public SessionCancellation {
 public:
  virtual std::shared_ptr<rs::core::transport::CancellationWait> wait_control() const noexcept = 0;
  // Resolver catalog I/O is an unsupported cancellation phase. Enter before
  // building/dispatching it; refusal preserves an already accepted local claim.
  virtual bool enter_resolver() noexcept = 0;
  virtual void leave_resolver() noexcept = 0;
  virtual bool before_wire() noexcept = 0;
  virtual void sent() noexcept = 0;
  virtual void error_observed(std::string_view native_state) noexcept = 0;
  virtual CancellationOutcome drained(bool legal_ready) noexcept = 0;
  // Query timeout (#229), main owner only, after the original deadline expired
  // while this operation's request was fully dispatched and running. Sends one
  // CancelRequest bounded by `grace` and returns true only when it was
  // delivered; the owner may then drain the main stream until `grace`. Refuses
  // any other phase, an earlier claim or error, and a sealed operation. This is
  // a driver claim, never reported as an application SQLCancel claim.
  virtual bool request_at_timeout(rs::util::Deadline grace) noexcept = 0;
  // True once drained() confirmed the server cancelled this timed-out request.
  virtual bool timeout_cancelled() const noexcept = 0;
};

} // namespace rs::core::database
