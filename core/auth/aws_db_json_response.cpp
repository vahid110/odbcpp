#include "core/auth/aws_db_json_response.h"
#include "core/auth/detail/aws_db_json_parser.h"
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
namespace rs::core::auth {
std::string_view JsonError::safe_message() const noexcept {
  switch (failure) {
    case JsonFailure::UnsupportedOperation: return "JSON credential operation unsupported";
    case JsonFailure::InvalidShape: return "JSON credential shape invalid";
    case JsonFailure::InvalidBody: return "JSON credential body invalid";
    case JsonFailure::InvalidDeadline: return "JSON credential deadline invalid";
    case JsonFailure::ResourceLimit: return "JSON credential resource limit";
    case JsonFailure::InvalidSyntax: return "JSON credential syntax invalid";
    case JsonFailure::InvalidUtf8: return "JSON credential UTF8 invalid";
    case JsonFailure::InvalidEscape: return "JSON credential escape invalid";
    case JsonFailure::FieldRejected: return "JSON credential field rejected";
    case JsonFailure::Cancelled: return "JSON credential cancelled";
    case JsonFailure::ClockRollback: return "JSON credential clock rollback";
    case JsonFailure::DeadlineElapsed: return "JSON credential deadline elapsed";
    case JsonFailure::AllocationFailed: return "JSON credential allocation failed";
  }
  return "JSON credential failure invalid";
}
namespace {
class ScalarCheckpoint {
 public:
  ScalarCheckpoint(rs::util::Deadline deadline,rs::util::Deadline highwater,
      MonotonicClock& clock,const Cancellation* stop)
      : deadline_(deadline),highwater_(highwater),clock_(clock),stop_(stop) {}
  void check() {
    if(stop_ && stop_->stop_requested()) detail::credential_json::refuse(JsonFailure::Cancelled);
    const auto now=clock_.now();
    if(now<highwater_) detail::credential_json::refuse(JsonFailure::ClockRollback);
    highwater_=now;
    if(now>=deadline_) detail::credential_json::refuse(JsonFailure::DeadlineElapsed);
  }
 private:
  rs::util::Deadline deadline_,highwater_;
  MonotonicClock& clock_; const Cancellation* stop_;
};
}
JsonOutcome<ResponseSnapshot> parse_aws_db_json_response(DbCredentialOperation operation,
    ResponseShape shape,ResponseBytes&& supplied,rs::util::Deadline deadline,
    rs::util::Deadline checkpoint,MonotonicClock& clock,const Cancellation* stop) {
  try {
    ResponseBytes body(std::move(supplied));
    if(operation!=DbCredentialOperation::ServerlessGetCredentials) return JsonError{JsonFailure::UnsupportedOperation,{}};
    if(shape!=ResponseShape::ServerlessObject) return JsonError{JsonFailure::InvalidShape,{}};
    if(body.size()>StreamOwner::max_bytes) return JsonError{JsonFailure::ResourceLimit,{}};
    if(deadline==rs::util::Deadline::max()) return JsonError{JsonFailure::InvalidDeadline,{}};
    std::optional<ResponseSnapshot> result;
    body.with_bytes([&](auto bytes) {ScalarCheckpoint gate(deadline,checkpoint,clock,stop);detail::credential_json::Parser parser(bytes,gate);result.emplace(parser.parse());});
    return std::move(*result);
  } catch(const detail::credential_json::Refusal& r) {return r.error;}
  catch(const std::bad_alloc&) {return JsonError{JsonFailure::AllocationFailed,{}};}
  catch(...) {return JsonError{JsonFailure::InvalidBody,{}};}
}
} // namespace rs::core::auth
