#pragma once
#include "odbcpp/auth/aws_db_response_fields.h"
#include "odbcpp/auth/bounded_response_stream.h"
namespace rs::core::auth {
enum class JsonFailure {
  UnsupportedOperation, InvalidShape, InvalidBody, InvalidDeadline, ResourceLimit,
  InvalidSyntax, InvalidUtf8, InvalidEscape, FieldRejected, Cancelled,
  ClockRollback, DeadlineElapsed, AllocationFailed
};
struct JsonError {
  JsonFailure failure;
  std::optional<FieldError> field_error{};
  std::string_view safe_message() const noexcept;
};
template<class T> using JsonOutcome = std::variant<T, JsonError>;
// Pure representation parser, never issuer/provenance/Receipt proof. Consumes raw
// body on refusal. Serverless JSON only; XML methods require separate parsers.
// Thread confined: observers outlive call, scoped byte views must not escape or
// reenter/move owners. Caller owns authentic operation pairing and retained clock
// checkpoint; an arbitrary supplied checkpoint cannot establish stage continuity.
JsonOutcome<ResponseSnapshot> parse_aws_db_json_response(
    DbCredentialOperation, ResponseShape, ResponseBytes&&,
    rs::util::Deadline original_deadline, rs::util::Deadline previous_clock_highwater,
    MonotonicClock&, const Cancellation* = nullptr);
} // namespace rs::core::auth
