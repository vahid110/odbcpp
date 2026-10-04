#pragma once
#include "temporary_db_validity.h"

namespace rs::core::auth {
// Private SDK-free GetClusterCredentialsWithIAM candidate, not AWS provenance or
// deployment support. Local syntax checks do NOT implement full AWS reserved-
// word/name validation. The future authenticated issuer must do that separately.
enum class WithIamPartition { Aws, AwsCn, AwsUsGov };
enum class WithIamFailure {
  UnsupportedMethod, UnsupportedPartition, InvalidAcquisition, InvalidResponse,
  PrincipalMismatch, ExpiryRange, TimeConversionFailed, Cancelled, AllocationFailed
};
struct WithIamError {
  WithIamFailure failure;
  std::optional<TimeFailure> time_failure{};
  std::string_view safe_message() const noexcept;
};
template<class T> class WithIamOutcome final {
 public:
  WithIamOutcome(T value) : value_(std::move(value)) {}
  WithIamOutcome(WithIamError error) : value_(error) {}
  explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
  T& value() & { return std::get<T>(value_); }
  const T& value() const& { return std::get<T>(value_); }
  T&& value() && { return std::get<T>(std::move(value_)); }
  WithIamError error() const { return std::get<WithIamError>(value_); }
 private:
  std::variant<T, WithIamError> value_;
};
struct WithIamTarget {
  WithIamPartition partition;
  std::string account, region, cluster_identifier, database, expected_principal;
  unsigned duration_seconds;
};
class WithIamAcquisition final {
 public:
  // Dedicated operation policy, not evidence of which API issued a response.
  // Untagged matching response data cannot prove the issuing API. The selected
  // production issuer retains authenticated operation/result pairing responsibility.
  static constexpr std::string_view operation = "GetClusterCredentialsWithIAM";
  static WithIamOutcome<WithIamAcquisition> create(Request original, WithIamTarget target);
  WithIamAcquisition(const WithIamAcquisition&) = default;
  WithIamAcquisition(WithIamAcquisition&&) = default;
  WithIamAcquisition& operator=(const WithIamAcquisition&) = delete;
  WithIamAcquisition& operator=(WithIamAcquisition&&) = delete;
  const Request& request() const noexcept { return request_; }
  const WithIamTarget& target() const noexcept { return target_; }
  std::optional<WithIamError> invariant_error() const;
  // Exact bridge request match; this is policy comparison, not issuer proof.
  bool matches(const Request&) const noexcept;
 private:
  WithIamAcquisition(Request request, WithIamTarget target)
      : request_(std::move(request)), target_(std::move(target)) {}
  Request request_;
  WithIamTarget target_;
};
// Untrusted owning response data. Only an explicitly injected TrustedIssuer can
// give its exact operation/result pairing production meaning. No public proof flag.
struct WithIamResponse {
  std::string user;
  SecretBytes password;
  std::optional<std::int64_t> expiration_epoch_milliseconds;
};
// Exact microsecond candidate, not SDK/issuer proof. No implicit ms conversion.
struct WithIamResponseUtc {
  std::string user;
  SecretBytes password;
  std::optional<UtcInstant> expiration_utc;
};
WithIamOutcome<Material> project_withiam_response_utc(const WithIamAcquisition&,
    WithIamResponseUtc&&, const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* = nullptr);
// Consumes into local ownership before validation. Thread-confined SecretBytes
// consumption/move preconditions apply. Failure releases only our owned copies.
WithIamOutcome<Material> project_withiam_response(const WithIamAcquisition&,
    WithIamResponse&&, const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* = nullptr);
} // namespace rs::core::auth
