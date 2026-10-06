#pragma once
#include "odbcpp/auth/response_operation.h"
#include "odbcpp/auth/aws_db_response_fields.h"
#include <aws/core/auth/AWSCredentials.h>
#include <atomic>
namespace rs::core::auth::aws::provisioned_native {
// Optional SDK-private observational prototype, not Authority/Material/Receipt.
// Exclusively standalone macOS main entry; hard transport interruption absent.
enum class Failure { InvalidContext, InvalidSource, MainEntryRequired, Unavailable,
  Refused, Consumed, WrongThread, Reentrant, LocalFailure };
class Context final {
 public:
  static std::variant<Context,Failure> create(std::string account,std::string region,
      std::string cluster,std::string database,std::string requested_user,std::string db_endpoint,
      std::string source_identity,std::string source_generation);
  const std::string account,region,cluster,database,requested_user,expected_user,
      db_endpoint,source_identity,source_generation,resource_arn,api_endpoint;
 private:
  friend struct FixedFixture;
  Context(std::string,std::string,std::string,std::string,std::string,std::string,std::string,std::string,bool);
};
struct ProtectedNamedSourceSpec;
namespace detail {struct State; class NamedSourceAcquisitionOwner;}
class FrozenNamedSource final {
 public:
  FrozenNamedSource(const FrozenNamedSource&)=delete;
  FrozenNamedSource& operator=(const FrozenNamedSource&)=delete;
  FrozenNamedSource(FrozenNamedSource&&) noexcept;
  FrozenNamedSource& operator=(FrozenNamedSource&&)=delete;
 private:
  friend class NativeOwner;friend struct FixedFixture;friend struct detail::State;
  friend class detail::NamedSourceAcquisitionOwner; // Future protected load, NOT implemented.
  FrozenNamedSource(Aws::Auth::AWSCredentials,std::string,std::string);
  // Explicit reset destroys SDK-allocated strings before ShutdownAPI.
  std::optional<Aws::Auth::AWSCredentials> value_;std::string identity_,generation_;bool owns_{true};
};
class WorkerCancellation final {
 public: void cancel() noexcept { cancelled_.store(true); }
  bool cancelled() const noexcept { return cancelled_.load(); }
 private:std::atomic<bool> cancelled_{false};
};
// First capture/response refusal only; no response strings or error messages.
enum class ResponseGate { None, CaptureMissing, CaptureFailure, Checkpoint,
  MissingResponse, ForeignResponse, Non200, ClientError, Mime, Encoding, BodyWrapper, StreamFlags, PutSize, BodySeek,
  CheckedStreamCreation, BodyCopy, CheckedStreamSeal };
enum class ProviderFailure { AccessDenied, ExpiredCredentials, InvalidCredentials, InvalidRequest,
  Throttled, Transport, Timeout, Cancelled, Unknown };
enum class ResponseStatus { Unobserved, Success200, Other2xx, Redirect3xx,
  Client4xx, Server5xx, Other };
struct Counts {
  ResponseGate first_response_gate{ResponseGate::None};
  ResponseStatus response_status{ResponseStatus::Unobserved};
  std::optional<bool> response_client_error;
  std::optional<ProviderFailure> provider_failure;
  unsigned error_diagnostic_bytes{};

  unsigned named_provider_loads{};bool named_source_ready{},named_stage_released{};
  unsigned initializations{},shutdowns{},bootstrap_factories{},clients{},destroyed_clients{},
      requests{},selected_sends{},foreign_requests{},null_requests{},safe_refusals{},
      frozen_provider_reads{},delegate_returns{},delegate_destructions{},active{},
      wrappers{},destroyed_wrappers{},validation_bytes{},model_bytes{},c_samples{},n_samples{};
  bool request_exact{},request_policy{},return_before_tail{},barrier{},rewound{},put_preserved{},
      model_success{},model_user_matches{},model_password_matches{},safe_error{},bootstrap_matches{},cleanup_before_c{},unknown_only{true};
  std::optional<BoundaryFailure> boundary;std::optional<std::int64_t> model_expiry_ms;
};
class Observation final {
 public:
  Observation(const Observation&)=delete;Observation& operator=(const Observation&)=delete;
  Observation(Observation&&);Observation& operator=(Observation&&)=delete;
  ~Observation();
  // Immutable original request while this creator-thread observation is active.
  // Consistency only; no issuer/expiry authority. Null after move or close.
  const Request* bound_request() const noexcept;
  bool with_fields(const std::function<void(const ExtractedDbFields&)>&) const;
  bool close() noexcept;Counts counts() const noexcept;
 private:
  friend class NativeOwner;
  Observation(std::shared_ptr<detail::State>,ProcessingBoundaryLease&&,ExtractedDbFields&&);
  std::shared_ptr<detail::State> state_;mutable std::optional<ProcessingBoundaryLease> keep_;
  std::optional<ExtractedDbFields> fields_;mutable bool viewing_{};
};
struct Outcome {std::optional<Observation> observation;std::optional<Failure> failure;Counts counts;};
struct CreateOutcome;
class NativeOwner final {
 public:
  static CreateOutcome create(Context,Request,std::shared_ptr<ResponseObservationSource>,
      std::shared_ptr<ResponseSourceGeneration>,std::shared_ptr<WorkerCancellation>,FrozenNamedSource&&);
  static CreateOutcome create_from_named_source(Context,Request,std::shared_ptr<ResponseObservationSource>,
      std::shared_ptr<ResponseSourceGeneration>,std::shared_ptr<WorkerCancellation>,ProtectedNamedSourceSpec);
  NativeOwner(const NativeOwner&)=delete;NativeOwner& operator=(const NativeOwner&)=delete;
  ~NativeOwner();
  Outcome acquire_observation();bool close() noexcept;Counts counts() const noexcept;
 private:
  friend struct FixedFixture;
  explicit NativeOwner(std::shared_ptr<detail::State>);
  std::shared_ptr<detail::State> state_;Counts closed_{};
};
struct CreateOutcome {std::unique_ptr<NativeOwner> owner;std::optional<Failure> failure;Counts counts;};
// Fixed test entry; no configurable endpoint/delegate/provider/live flag.
enum class FixedCase { Happy, DuplicateUser, WrongUser, WrongEnvelope, ExactCap, Overflow,
  BadFlags, SeekFailure, NullResponse, ForeignResponse, WrongMime, Non200,
  NullThenSelected, ForeignThenSelected, LateAfterValidation, LateAfterModel,
  AdoptionFailure, CancelInWorker, ServiceDenied, ServiceExpired, ServiceInvalidToken, ServiceRequestExpired, ServiceInvalid,
  ServiceThrottled, ServiceDuplicate, ServiceMissing, ServiceMalformed, ServiceUnknown, ServiceTwoErrors, ServiceNestedCode, ServiceDtd, ServiceLongCode, ServiceWrongNamespace, ServiceWrongErrorNamespace, ServiceWrongCodeNamespace, ServiceQueryNamespace,
  TransportError, TransportTimeout, TransportCancelled, Reentry };
struct FixedFixture final {
  static Context context();static Request request(const Context&);
  static CreateOutcome create_named(ProtectedNamedSourceSpec,std::shared_ptr<ResponseObservationSource>,
      std::shared_ptr<ResponseSourceGeneration>,std::shared_ptr<WorkerCancellation>);
  // Closed fake-only bridge tests: same fixed context/body/source, finite D.
  static CreateOutcome create_for_connector(FixedCase,rs::util::Deadline,
      std::shared_ptr<ResponseObservationSource>,std::shared_ptr<ResponseSourceGeneration>,
      std::shared_ptr<WorkerCancellation>);
  static CreateOutcome create(FixedCase,std::shared_ptr<ResponseObservationSource>,
      std::shared_ptr<ResponseSourceGeneration>,std::shared_ptr<WorkerCancellation>);
};
} // namespace
