#pragma once
#include "core/auth/auth_core.h"
#include <memory>
#include <variant>

namespace rs::core::auth {
// Private local checkpoint contract. No clock value, quality or issuer authority.
enum class BoundaryFailure {
  InvalidBoundary, InvalidInput, InvalidState, WrongOperation, WrongThread,
  ReadFailed, ClockRollback, DeadlineElapsed, Cancelled, Overflow,
  StaleObservation, UnknownQuality, Borrowed, SourceClosed, SourceChanged,
  UnsafeDiagnostic, InvalidDiagnostic, StaleDiagnostic, ForeignDispatch,
  BodyRejected, AllocationFailed
};
using BoundaryStatus = std::variant<std::monostate, BoundaryFailure>;
template<class T> using BoundaryResult = std::variant<T, BoundaryFailure>;
namespace detail {
// Internal injection seam, never installed API or protection against a malicious
// implementation. Subclasses own their immutable Request and phase dependencies.
class TransportBoundaryGate {
 public:
  virtual ~TransportBoundaryGate() noexcept = default;
  virtual const Request* request() const noexcept = 0;
  virtual BoundaryStatus observe_checkpoint() = 0;
  virtual BoundaryStatus reject_body_fault() = 0;
};
class ProcessingBoundaryGate {
 public:
  virtual ~ProcessingBoundaryGate() noexcept = default;
  virtual const Request* request() const noexcept = 0;
  virtual BoundaryStatus observe_checkpoint() = 0;
  virtual BoundaryStatus reject_body_fault() = 0;
};
class ResponseLeaseFactory;
}
class TransportBoundaryLease final {
 public:
  TransportBoundaryLease(const TransportBoundaryLease&) = delete;
  TransportBoundaryLease& operator=(const TransportBoundaryLease&) = delete;
  TransportBoundaryLease(TransportBoundaryLease&&) noexcept;
  TransportBoundaryLease& operator=(TransportBoundaryLease&&) = delete;
  ~TransportBoundaryLease();
  const Request* request() const noexcept;
  BoundaryStatus check() noexcept;
  BoundaryStatus reject_body() noexcept;
 private:
  friend class detail::ResponseLeaseFactory;
  explicit TransportBoundaryLease(std::unique_ptr<detail::TransportBoundaryGate>) noexcept;
  std::unique_ptr<detail::TransportBoundaryGate> gate_;
};
class ProcessingBoundaryLease final {
 public:
  ProcessingBoundaryLease(const ProcessingBoundaryLease&) = delete;
  ProcessingBoundaryLease& operator=(const ProcessingBoundaryLease&) = delete;
  ProcessingBoundaryLease(ProcessingBoundaryLease&&) noexcept;
  ProcessingBoundaryLease& operator=(ProcessingBoundaryLease&&) = delete;
  ~ProcessingBoundaryLease();
  const Request* request() const noexcept;
  BoundaryStatus check() noexcept;
  BoundaryStatus reject_body() noexcept;
 private:
  friend class detail::ResponseLeaseFactory;
  explicit ProcessingBoundaryLease(std::unique_ptr<detail::ProcessingBoundaryGate>) noexcept;
  std::unique_ptr<detail::ProcessingBoundaryGate> gate_;
};
namespace detail {
// Internal friend factory only. Consume before refusal. Construction must retain
// a same-owner rejection handle once a phase is admitted. Wrong-thread/reentry
// destruction remains concrete owner's fatal misuse, not a new concurrency API.
class ResponseLeaseFactory final {
 public:
  static BoundaryResult<TransportBoundaryLease> bind_transport(
      std::unique_ptr<TransportBoundaryGate>) noexcept;
  static BoundaryResult<ProcessingBoundaryLease> bind_processing(
      std::unique_ptr<ProcessingBoundaryGate>) noexcept;
};
}
} // namespace rs::core::auth
