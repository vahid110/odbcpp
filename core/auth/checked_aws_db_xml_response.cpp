#include "core/auth/checked_aws_db_xml_response.h"
#include "core/auth/detail/aws_db_xml_parser.h"
#include <exception>
#include <new>
#include <type_traits>
namespace rs::core::auth {
namespace {
struct BoundaryRefusal { BoundaryFailure failure; };
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
BoundaryFailure reject(ProcessingBoundaryLease& lease) noexcept {
  const auto status=lease.reject_body();const auto* f=std::get_if<BoundaryFailure>(&status);
  if(!f || !terminal(*f)) { std::terminate(); }
  const auto confirmed=lease.check();const auto* again=std::get_if<BoundaryFailure>(&confirmed);
  if(!again || *again!=*f) { std::terminate(); }return *f;
}
class RejectionGuard final {
 public:
  explicit RejectionGuard(ProcessingBoundaryLease&& supplied) noexcept
      : lease(std::move(supplied)),exceptions(std::uncaught_exceptions()) {}
  ~RejectionGuard() {
    if(lease.request() && (!success || std::uncaught_exceptions()>exceptions)) { (void)reject(lease); }
  }
  ProcessingBoundaryLease lease;bool success{};
 private:
  const int exceptions;
};
class ProcessingCheckpoint final {
 public:
  explicit ProcessingCheckpoint(ProcessingBoundaryLease& lease) noexcept : lease_(lease) {}
  void check() {
    const auto status=lease_.check();
    if(const auto* f=std::get_if<BoundaryFailure>(&status)) { throw BoundaryRefusal{*f}; }
  }
 private:
  ProcessingBoundaryLease& lease_;
};
CheckedXmlResult<ResponseSnapshot> parse_impl(DbCredentialOperation operation,ResponseShape shape,
    ResponseBytes&& supplied,ProcessingBoundaryLease&& phase,
    std::optional<detail::CheckedXmlConstructionFailure> injected) {
  RejectionGuard guard(std::move(phase));
  // This current ResponseBytes move allocates nothing and only throws for active
  // scoped consumption. Cannot return ordinarily while that caller view is live.
  std::optional<ResponseBytes> body;
  try { body.emplace(std::move(supplied)); }
  catch(...) { if(guard.lease.request()) { (void)reject(guard.lease); }std::terminate(); }
  if(!guard.lease.request()) { return CheckedXmlError{BoundaryFailure::InvalidBoundary}; }
  ProcessingCheckpoint checkpoint(guard.lease);
  try {
    checkpoint.check();
    if(operation!=DbCredentialOperation::ClusterGetCredentials && operation!=DbCredentialOperation::ClusterGetCredentialsWithIam) { detail::credential_xml::fail(XmlFailure::UnsupportedOperation); }
    if((operation==DbCredentialOperation::ClusterGetCredentials && shape!=ResponseShape::ClusterResult) || (operation==DbCredentialOperation::ClusterGetCredentialsWithIam && shape!=ResponseShape::ClusterWithIamResult)) { detail::credential_xml::fail(XmlFailure::InvalidShape); }
    if(body->size()>StreamOwner::max_bytes) { detail::credential_xml::fail(XmlFailure::ResourceLimit); }
    if(injected && *injected!=detail::CheckedXmlConstructionFailure::BeforeParse &&
       *injected!=detail::CheckedXmlConstructionFailure::BeforePublication) { detail::credential_xml::fail(XmlFailure::InvalidBody); }
    if(injected==detail::CheckedXmlConstructionFailure::BeforeParse) { throw std::bad_alloc{}; }
    std::optional<ResponseSnapshot> snapshot;
    body->with_bytes([&](auto bytes) {
      detail::credential_xml::Parser parser(bytes,shape,checkpoint);snapshot.emplace(parser.parse());
    }); // Parser/Scratch/raw borrow ended, owning fields only.
    if(injected==detail::CheckedXmlConstructionFailure::BeforePublication) { throw std::bad_alloc{}; }
    if(!body->clear()) { std::terminate(); }
    checkpoint.check(); // After raw cleanup, before successful publication.
    CheckedXmlResult<ResponseSnapshot> result(std::move(*snapshot));
    guard.success=true;return result;
  } catch(const BoundaryRefusal& refused) { return CheckedXmlError{refused.failure}; }
  catch(const detail::credential_xml::Refusal& refused) { return CheckedXmlError{refused.error,reject(guard.lease)}; }
  catch(const std::bad_alloc&) { return CheckedXmlError{XmlError{XmlFailure::AllocationFailed,{}},reject(guard.lease)}; }
  catch(...) { return CheckedXmlError{XmlError{XmlFailure::InvalidBody,{}},reject(guard.lease)}; }
  // Body destroyed BEFORE guard's Borrow, including every refusal path.
}
}
CheckedXmlResult<ResponseSnapshot> parse_aws_db_xml_response_checked(DbCredentialOperation operation,
    ResponseShape shape,ResponseBytes&& body,ProcessingBoundaryLease&& phase) {
  return parse_impl(operation,shape,std::move(body),std::move(phase),{});
}
CheckedXmlResult<ResponseSnapshot> detail::CheckedXmlTestAccess::refuse_processing(DbCredentialOperation operation,
    ResponseShape shape,ResponseBytes&& body,ProcessingBoundaryLease&& phase,CheckedXmlConstructionFailure step) {
  return parse_impl(operation,shape,std::move(body),std::move(phase),step);
}
static_assert(std::is_nothrow_move_constructible_v<ResponseSnapshot>);
static_assert(std::is_nothrow_move_constructible_v<CheckedXmlResult<ResponseSnapshot>>);
static_assert(std::is_nothrow_constructible_v<CheckedXmlResult<ResponseSnapshot>,ResponseSnapshot&&>);
}
