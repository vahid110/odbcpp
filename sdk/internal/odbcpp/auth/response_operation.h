#pragma once
#include "odbcpp/auth/checked_response_boundary.h"
#include <thread>
namespace rs::core::auth {
// Closed failure-aware observation, no noexcept Deadline-only clock bridge.
enum class ResponseReadFailure { ReadFailed };
using ResponseClockRead=std::variant<rs::util::Deadline,ResponseReadFailure>;
class ResponseObservationSource {
 public:
  virtual ~ResponseObservationSource() noexcept=default;
  virtual ResponseClockRead read_monotonic()=0;
  virtual bool cancellation_requested()=0;
};
enum class SourceRetirement { Closed, Changed };
// Local lifetime/generation anchor, never authenticated issuer or TLS proof.
// Retirement/destruction are creator-thread confined. Shared ownership does not
// synchronize mutable state or authorize concurrent use of the observer.
class ResponseSourceGeneration final {
 public:
  static BoundaryResult<std::shared_ptr<ResponseSourceGeneration>> create(const Binding&) noexcept;
  ResponseSourceGeneration(const ResponseSourceGeneration&)=delete;
  ResponseSourceGeneration& operator=(const ResponseSourceGeneration&)=delete;
  ~ResponseSourceGeneration();
  const SourceInput& identity() const noexcept { return identity_; }
  BoundaryStatus retire(SourceRetirement) noexcept;
  BoundaryStatus checkpoint() const noexcept;
 private:
  explicit ResponseSourceGeneration(SourceInput identity):identity_(std::move(identity)),thread_(std::this_thread::get_id()) {}
  const SourceInput identity_;
  const std::thread::id thread_;
  std::optional<BoundaryFailure> retired_;
};
// Passive response checkpoints, deliberately NOT p/a/u/b or a UTC time sample.
// No eligibility, expiry, credential, Material, Receipt or authority surface.
class ResponseCompletion final {
 public:
  const Request& request() const noexcept { return request_; }
  rs::util::Deadline initial_checkpoint() const noexcept { return initial_; }
  rs::util::Deadline transport_completed() const noexcept { return completed_; }
  rs::util::Deadline processing_completed() const noexcept { return final_; }
 private:
  friend class ResponseOperation;
  ResponseCompletion(Request request,rs::util::Deadline initial,rs::util::Deadline completed,rs::util::Deadline final) noexcept
      :request_(std::move(request)),initial_(initial),completed_(completed),final_(final) {}
  Request request_;
  rs::util::Deadline initial_,completed_,final_;
};
namespace detail {
struct ResponseOperationState;
enum class ResponsePreparationFailure { BeforeGate, BeforePermit };
class ResponseOperationTestAccess;
}
class ResponseOperation final {
 public:
  static BoundaryResult<std::unique_ptr<ResponseOperation>> begin(Request,
      std::shared_ptr<ResponseObservationSource>,std::shared_ptr<ResponseSourceGeneration>) noexcept;
  ResponseOperation(const ResponseOperation&)=delete;
  ResponseOperation& operator=(const ResponseOperation&)=delete;
  ~ResponseOperation();
  BoundaryStatus begin_transport() noexcept;
  BoundaryResult<TransportBoundaryLease> transport_lease(const Request&) noexcept;
  // A separate transport_lease is also the dedicated dispatch permit: it must
  // outlive all external request/stream/callback access, then release before c.
  BoundaryStatus complete_transport() noexcept;
  BoundaryResult<ProcessingBoundaryLease> processing_lease(const Request&) noexcept;
  // Helper lease release is not extraction quiescence; retain a sibling
  // Processing lease until extraction/secret cleanup ends, before sampling n.
  BoundaryResult<ResponseCompletion> finish_processing() noexcept;
  std::optional<BoundaryFailure> fault() const noexcept;
 private:
  friend class detail::ResponseOperationTestAccess;
  explicit ResponseOperation(std::shared_ptr<detail::ResponseOperationState> state) noexcept:state_(std::move(state)) {}
  BoundaryResult<TransportBoundaryLease> transport_impl(const Request&,std::optional<detail::ResponsePreparationFailure>) noexcept;
  BoundaryResult<ProcessingBoundaryLease> processing_impl(const Request&,std::optional<detail::ResponsePreparationFailure>) noexcept;
  std::shared_ptr<detail::ResponseOperationState> state_;
};
namespace detail {
class ResponseOperationTestAccess final {
 public:
  static BoundaryResult<TransportBoundaryLease> refuse_transport(ResponseOperation&,const Request&,ResponsePreparationFailure) noexcept;
  static BoundaryResult<ProcessingBoundaryLease> refuse_processing(ResponseOperation&,const Request&,ResponsePreparationFailure) noexcept;
};
}
// Operation gates own observer/source until last permit releases. Wrong-thread
// calls refuse without observing; destruction/reentry fatal. Observers may block:
// cooperative checks establish NO hard transport deadline/quiescence guarantee.
} // namespace rs::core::auth
