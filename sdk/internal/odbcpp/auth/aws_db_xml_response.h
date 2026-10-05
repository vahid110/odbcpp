#pragma once
#include "odbcpp/auth/aws_db_response_fields.h"
#include "odbcpp/auth/bounded_response_stream.h"
namespace rs::core::auth {
enum class XmlFailure {
  UnsupportedOperation, InvalidShape, InvalidBody, InvalidDeadline, ResourceLimit,
  InvalidSyntax, InvalidUtf8, UnsupportedMarkup, InvalidEntity, InvalidNamespace,
  InvalidEnvelope, DuplicateEnvelope, InvalidName, FieldRejected, Cancelled,
  ClockRollback, DeadlineElapsed, AllocationFailed
};
struct XmlError {
  XmlFailure failure;
  std::optional<FieldError> field_error{};
  std::string_view safe_message() const noexcept;
};
template<class T> using XmlOutcome = std::variant<T, XmlError>;
// Limited UTF8 Query XML representation, not a general XML processor or issuer
// proof. Full matching Response/Result with exact default namespace only.
// ASCII names: [A-Za-z_][A-Za-z0-9_.-]*, <=64; colon/prefix refused.
// Consumes source on refusal; scoped borrowing/lifetime/thread confinement apply.
// Caller owns authenticated method pairing and actual prior clock checkpoint.
XmlOutcome<ResponseSnapshot> parse_aws_db_xml_response(
    DbCredentialOperation, ResponseShape, ResponseBytes&&,
    rs::util::Deadline original_deadline, rs::util::Deadline previous_clock_highwater,
    MonotonicClock&, const Cancellation* = nullptr);
} // namespace rs::core::auth
