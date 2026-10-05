#include "odbcpp/auth/aws_db_xml_response.h"
#include "odbcpp/auth/detail/aws_db_xml_parser.h"
#include <cstdint>
#include <memory>
#include <new>
namespace rs::core::auth {
std::string_view XmlError::safe_message() const noexcept {
  switch(failure) {
    case XmlFailure::UnsupportedOperation:return "XML credential operation unsupported";
    case XmlFailure::InvalidShape:return "XML credential shape invalid";
    case XmlFailure::InvalidBody:return "XML credential body invalid";
    case XmlFailure::InvalidDeadline:return "XML credential deadline invalid";
    case XmlFailure::ResourceLimit:return "XML credential resource limit";
    case XmlFailure::InvalidSyntax:return "XML credential syntax invalid";
    case XmlFailure::InvalidUtf8:return "XML credential UTF8 invalid";
    case XmlFailure::UnsupportedMarkup:return "XML credential markup unsupported";
    case XmlFailure::InvalidEntity:return "XML credential entity invalid";
    case XmlFailure::InvalidNamespace:return "XML credential namespace invalid";
    case XmlFailure::InvalidEnvelope:return "XML credential envelope invalid";
    case XmlFailure::DuplicateEnvelope:return "XML credential envelope duplicate";
    case XmlFailure::InvalidName:return "XML credential name invalid";
    case XmlFailure::FieldRejected:return "XML credential field rejected";
    case XmlFailure::Cancelled:return "XML credential cancelled";
    case XmlFailure::ClockRollback:return "XML credential clock rollback";
    case XmlFailure::DeadlineElapsed:return "XML credential deadline elapsed";
    case XmlFailure::AllocationFailed:return "XML credential allocation failed";
  }
  return "XML credential failure invalid";
}
namespace {
using detail::credential_xml::Refusal;
class ScalarCheckpoint final {
 public:
  ScalarCheckpoint(rs::util::Deadline deadline,rs::util::Deadline checkpoint,MonotonicClock& clock,const Cancellation* stop)
      :deadline_(deadline),highwater_(checkpoint),clock_(clock),stop_(stop){}
  void check(){
    if(stop_&&stop_->stop_requested()) {detail::credential_xml::fail(XmlFailure::Cancelled);}
    const auto now=clock_.now();if(now<highwater_) {detail::credential_xml::fail(XmlFailure::ClockRollback);}highwater_=now;
    if(now>=deadline_) {detail::credential_xml::fail(XmlFailure::DeadlineElapsed);}
  }
 private:
  rs::util::Deadline deadline_,highwater_;MonotonicClock& clock_;const Cancellation* stop_;
};
}
XmlOutcome<ResponseSnapshot> parse_aws_db_xml_response(DbCredentialOperation operation,ResponseShape shape,
    ResponseBytes&& supplied,rs::util::Deadline deadline,rs::util::Deadline checkpoint,MonotonicClock& clock,const Cancellation* stop){
  try{
    ResponseBytes body(std::move(supplied));
    if(operation!=DbCredentialOperation::ClusterGetCredentials&&operation!=DbCredentialOperation::ClusterGetCredentialsWithIam)return XmlError{XmlFailure::UnsupportedOperation,{}};
    if((operation==DbCredentialOperation::ClusterGetCredentials&&shape!=ResponseShape::ClusterResult)||
       (operation==DbCredentialOperation::ClusterGetCredentialsWithIam&&shape!=ResponseShape::ClusterWithIamResult))return XmlError{XmlFailure::InvalidShape,{}};
    if(body.size()>StreamOwner::max_bytes)return XmlError{XmlFailure::ResourceLimit,{}};
    if(deadline==rs::util::Deadline::max())return XmlError{XmlFailure::InvalidDeadline,{}};
    std::optional<ResponseSnapshot> result;
    body.with_bytes([&](auto bytes){ScalarCheckpoint gate(deadline,checkpoint,clock,stop);detail::credential_xml::Parser parser(bytes,shape,gate);result.emplace(parser.parse());});
    return std::move(*result);
  }catch(const Refusal& r){return r.error;}catch(const std::bad_alloc&){return XmlError{XmlFailure::AllocationFailed,{}};}
  catch(...){return XmlError{XmlFailure::InvalidBody,{}};}
}
} // namespace rs::core::auth
