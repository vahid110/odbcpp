#pragma once
#include "temporary_db_validity.h"

namespace rs::core::auth {
// Private SDK-free candidate policy, not AWS provenance or deployment support.
enum class AwsPartition { Aws, AwsCn, AwsUsGov };
enum class ResponseFailure {
  UnsupportedMethod, UnsupportedPartition, InvalidAcquisition, InvalidResponse,
  PrincipalMismatch, ExpiryRange, TimeConversionFailed, Cancelled, AllocationFailed
};
struct ResponseError {
  ResponseFailure failure;
  std::optional<TimeFailure> time_failure{};
  std::string_view safe_message() const noexcept;
};
template<class T> class ResponseOutcome final {
 public:
  ResponseOutcome(T value) : value_(std::move(value)) {}
  ResponseOutcome(ResponseError error) : value_(error) {}
  explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
  T& value() & { return std::get<T>(value_); }
  const T& value() const& { return std::get<T>(value_); }
  T&& value() && { return std::get<T>(std::move(value_)); }
  ResponseError error() const { return std::get<ResponseError>(value_); }
 private:
  std::variant<T, ResponseError> value_;
};
struct ServerlessTarget {
  AwsPartition partition;
  std::string account, region, workgroup_id, workgroup_name, database;
  unsigned duration_seconds;
};
class ServerlessAcquisition final {
 public:
  static ResponseOutcome<ServerlessAcquisition> create(Request original, ServerlessTarget target);
  ServerlessAcquisition(const ServerlessAcquisition&) = default;
  ServerlessAcquisition(ServerlessAcquisition&&) = default;
  ServerlessAcquisition& operator=(const ServerlessAcquisition&) = delete;
  ServerlessAcquisition& operator=(ServerlessAcquisition&&) = delete;
  const Request& request() const noexcept { return request_; }
  const ServerlessTarget& target() const noexcept { return target_; }
  std::optional<ResponseError> invariant_error() const;
  // Exact bridge request match; this is policy comparison, not issuer proof.
  bool matches(const Request&) const noexcept;
 private:
  ServerlessAcquisition(Request request, ServerlessTarget target)
      : request_(std::move(request)), target_(std::move(target)) {}
  Request request_;
  ServerlessTarget target_;
};
// Untrusted owning response data. Only an explicitly injected TrustedIssuer can
// give its exact operation/result pairing production meaning. No public proof flag.
struct ServerlessResponse {
  std::string user;
  SecretBytes password;
  std::optional<std::int64_t> expiration_epoch_milliseconds;
};
// Exact microsecond candidate, not SDK/issuer proof. No implicit ms conversion.
struct ServerlessResponseUtc {
  std::string user;
  SecretBytes password;
  std::optional<UtcInstant> expiration_utc;
};
ResponseOutcome<Material> project_serverless_response_utc(const ServerlessAcquisition&,
    ServerlessResponseUtc&&, const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* = nullptr);
// Consumes into local ownership before validation. Thread-confined SecretBytes
// consumption/move preconditions apply. Failure releases only our owned copies.
ResponseOutcome<Material> project_serverless_response(const ServerlessAcquisition&,
    ServerlessResponse&&, const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* = nullptr);
} // namespace rs::core::auth
