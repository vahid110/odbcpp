#include "core/auth/checked_response_boundary.h"
#include <exception>
#include <utility>

namespace rs::core::auth {
namespace {
bool terminal(BoundaryFailure f) noexcept {
  switch(f) {
    case BoundaryFailure::ReadFailed: case BoundaryFailure::ClockRollback:
    case BoundaryFailure::DeadlineElapsed: case BoundaryFailure::Cancelled:
    case BoundaryFailure::Overflow: case BoundaryFailure::StaleObservation:
    case BoundaryFailure::SourceClosed: case BoundaryFailure::SourceChanged:
    case BoundaryFailure::UnsafeDiagnostic: case BoundaryFailure::InvalidDiagnostic:
    case BoundaryFailure::StaleDiagnostic: case BoundaryFailure::ForeignDispatch:
    case BoundaryFailure::BodyRejected: return true;
    case BoundaryFailure::InvalidBoundary: case BoundaryFailure::InvalidInput:
    case BoundaryFailure::InvalidState: case BoundaryFailure::WrongOperation:
    case BoundaryFailure::WrongThread: case BoundaryFailure::UnknownQuality:
    case BoundaryFailure::Borrowed: case BoundaryFailure::AllocationFailed: return false;
  }
  std::terminate();
}
template<class Gate> BoundaryStatus unexpected(Gate& gate) noexcept {
  // No independent latch or generic healthy-owner failure. Rejection must be a
  // terminal same-owner refusal, confirmed by its subsequent checkpoint check.
  try {
    const auto rejected=gate.reject_body_fault();
    const auto* f=std::get_if<BoundaryFailure>(&rejected);
    if(!f || !terminal(*f)) { std::terminate(); }
    const auto confirmed=gate.observe_checkpoint();
    const auto* observed=std::get_if<BoundaryFailure>(&confirmed);
    if(!observed || *observed!=*f) { std::terminate(); }
    return *f;
  } catch(...) { std::terminate(); }
}
template<class Gate> BoundaryStatus dispatch(Gate* gate,bool reject) noexcept {
  if(!gate) { return BoundaryFailure::InvalidBoundary; }
  try { return reject?gate->reject_body_fault():gate->observe_checkpoint(); }
  catch(...) { return unexpected(*gate); }
}
bool valid(const Request* r) noexcept {
  return r && !r->binding().invariant_error() &&
      r->deadline()!=rs::util::Deadline::max() && r->deadline()!=rs::util::Deadline::min() &&
      r->headroom()>=rs::util::Clock::duration::zero() && r->headroom()<=std::chrono::hours{24};
}
template<class Gate> BoundaryStatus prepare(Gate* gate) noexcept {
  if(!gate) { return BoundaryFailure::InvalidBoundary; }
  if(!valid(gate->request())) {
    // Non-null gates may already own an admitted Borrow. Refuse terminally,
    // rather than silently destroying the only active rejection handle.
    return unexpected(*gate); // Confirm same-owner terminal refusal, or fatal misuse.
  }
  return dispatch(gate,false);
}
}
TransportBoundaryLease::TransportBoundaryLease(std::unique_ptr<detail::TransportBoundaryGate> gate) noexcept : gate_(std::move(gate)) {}
TransportBoundaryLease::TransportBoundaryLease(TransportBoundaryLease&&) noexcept = default;
TransportBoundaryLease::~TransportBoundaryLease() = default;
const Request* TransportBoundaryLease::request() const noexcept { return gate_?gate_->request():nullptr; }
BoundaryStatus TransportBoundaryLease::check() noexcept { return dispatch(gate_.get(),false); }
BoundaryStatus TransportBoundaryLease::reject_body() noexcept { return dispatch(gate_.get(),true); }
ProcessingBoundaryLease::ProcessingBoundaryLease(std::unique_ptr<detail::ProcessingBoundaryGate> gate) noexcept : gate_(std::move(gate)) {}
ProcessingBoundaryLease::ProcessingBoundaryLease(ProcessingBoundaryLease&&) noexcept = default;
ProcessingBoundaryLease::~ProcessingBoundaryLease() = default;
const Request* ProcessingBoundaryLease::request() const noexcept { return gate_?gate_->request():nullptr; }
BoundaryStatus ProcessingBoundaryLease::check() noexcept { return dispatch(gate_.get(),false); }
BoundaryStatus ProcessingBoundaryLease::reject_body() noexcept { return dispatch(gate_.get(),true); }
BoundaryResult<TransportBoundaryLease> detail::ResponseLeaseFactory::bind_transport(std::unique_ptr<TransportBoundaryGate> gate) noexcept {
  const auto status=prepare(gate.get());
  if(const auto* f=std::get_if<BoundaryFailure>(&status)) { return *f; }
  return TransportBoundaryLease(std::move(gate));
}
BoundaryResult<ProcessingBoundaryLease> detail::ResponseLeaseFactory::bind_processing(std::unique_ptr<ProcessingBoundaryGate> gate) noexcept {
  const auto status=prepare(gate.get());
  if(const auto* f=std::get_if<BoundaryFailure>(&status)) { return *f; }
  return ProcessingBoundaryLease(std::move(gate));
}
} // namespace rs::core::auth
