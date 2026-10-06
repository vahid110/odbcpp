#include "odbcpp/auth/provisioned_query_transport.h"
#include "odbcpp/auth/detail/aws_db_xml_parser.h"
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
BoundaryFailure reject(TransportBoundaryLease& lease) noexcept {
  const auto status=lease.reject_body();const auto* f=std::get_if<BoundaryFailure>(&status);
  if(!f || !terminal(*f)) { std::terminate(); }
  const auto confirmed=lease.check();const auto* again=std::get_if<BoundaryFailure>(&confirmed);
  if(!again || *again!=*f) { std::terminate(); }return *f;
}
class RejectionGuard final {
 public:
  explicit RejectionGuard(TransportBoundaryLease&& supplied) noexcept
      : lease(std::move(supplied)),exceptions(std::uncaught_exceptions()) {}
  ~RejectionGuard() {
    if(lease.request() && (!success || std::uncaught_exceptions()>exceptions)) { (void)reject(lease); }
  }
  TransportBoundaryLease lease;bool success{};
 private:
  const int exceptions;
};
class TransportCheckpoint final {
 public:
  explicit TransportCheckpoint(TransportBoundaryLease& lease) noexcept : lease_(lease) {}
  void check() {
    const auto status=lease_.check();
    if(const auto* f=std::get_if<BoundaryFailure>(&status)) { throw BoundaryRefusal{*f}; }
  }
 private:
  TransportBoundaryLease& lease_;
};
TransportQueryResult prevalidate_impl(ResponseBytes&& supplied,TransportBoundaryLease&& phase,
    std::string_view expected_user) {
  RejectionGuard guard(std::move(phase));
  std::optional<ResponseBytes> body;
  try { body.emplace(std::move(supplied)); }
  catch(...) { if(guard.lease.request()) { (void)reject(guard.lease); }std::terminate(); }
  if(!guard.lease.request()) { return TransportQueryError{BoundaryFailure::InvalidBoundary}; }
  TransportCheckpoint checkpoint(guard.lease);
  try {
    checkpoint.check();
    if(expected_user.empty() || expected_user!=guard.lease.request()->binding().target().principal) {
      return TransportQueryError{FieldError{FieldFailure::InvalidUser},reject(guard.lease)};
    }
    if(body->size()>StreamOwner::max_bytes) { detail::credential_xml::fail(XmlFailure::ResourceLimit); }
    std::optional<ResponseSnapshot> snapshot;
    body->with_bytes([&](auto bytes) {
      detail::credential_xml::Parser parser(bytes,ResponseShape::ClusterResult,checkpoint);
      snapshot.emplace(parser.parse());
    }); // parser, scratch and raw view quiescent
    if(!body->clear()) { std::terminate(); }
    checkpoint.check();
    auto fields=extract_db_fields(DbCredentialOperation::ClusterGetCredentials,std::move(*snapshot));
    snapshot.reset();
    checkpoint.check();
    if(!fields) { return TransportQueryError{fields.error(),reject(guard.lease)}; }
    if(expected_user.empty() || fields.value().user!=expected_user) {
      return TransportQueryError{FieldError{FieldFailure::InvalidUser},reject(guard.lease)};
    }
    TransportQueryResult result(std::move(fields).value());
    checkpoint.check();
    guard.success=true;return result;
  } catch(const BoundaryRefusal& refused) { return TransportQueryError{refused.failure}; }
  catch(const detail::credential_xml::Refusal& refused) { return TransportQueryError{refused.error,reject(guard.lease)}; }
  catch(const std::bad_alloc&) { return TransportQueryError{XmlError{XmlFailure::AllocationFailed,{}},reject(guard.lease)}; }
  catch(...) { return TransportQueryError{XmlError{XmlFailure::InvalidBody,{}},reject(guard.lease)}; }
}
}
TransportQueryResult prevalidate_provisioned_query_transport(ResponseBytes&& body,
    TransportBoundaryLease&& phase,std::string_view expected_user) {
  return prevalidate_impl(std::move(body),std::move(phase),expected_user);
}
} // namespace rs::core::auth
