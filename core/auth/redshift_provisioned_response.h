#pragma once
#include "temporary_db_validity.h"

namespace rs::core::auth {
// Private SDK-free GetClusterCredentials candidate, not AWS provenance or
// deployment support. Local syntax checks do NOT implement full AWS reserved-
// word/name validation. The future authenticated issuer must do that separately.
enum class ProvisionedPartition { Aws, AwsCn, AwsUsGov };
enum class ProvisionedFailure {
  UnsupportedMethod, UnsupportedPartition, UnsupportedPolicy, InvalidAcquisition, InvalidResponse,
  PrincipalMismatch, ExpiryRange, TimeConversionFailed, Cancelled, AllocationFailed
};
struct ProvisionedError {
  ProvisionedFailure failure;
  std::optional<TimeFailure> time_failure{};
  std::string_view safe_message() const noexcept;
};
template<class T> class ProvisionedOutcome final {
 public:
  ProvisionedOutcome(T value) : value_(std::move(value)) {}
  ProvisionedOutcome(ProvisionedError error) : value_(error) {}
  explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
  T& value() & { return std::get<T>(value_); }
  const T& value() const& { return std::get<T>(value_); }
  T&& value() && { return std::get<T>(std::move(value_)); }
  ProvisionedError error() const { return std::get<ProvisionedError>(value_); }
 private:
  std::variant<T, ProvisionedError> value_;
};
struct ProvisionedTarget {
  ProvisionedPartition partition;
  std::string account, region, cluster_identifier, database, requested_user;
  std::optional<bool> auto_create;
  // Omitted is distinct from an explicitly specified empty group list.
  std::optional<std::vector<std::string>> groups;
  unsigned duration_seconds;
};
class ProvisionedAcquisition final {
 public:
  // Dedicated operation policy, not evidence of which API issued a response.
  // The explicitly injected production issuer must retain authenticated pairing.
  static constexpr std::string_view operation = "GetClusterCredentials";
  static ProvisionedOutcome<ProvisionedAcquisition> create(Request original, ProvisionedTarget target);
  ProvisionedAcquisition(const ProvisionedAcquisition&) = default;
  ProvisionedAcquisition(ProvisionedAcquisition&&) = default;
  ProvisionedAcquisition& operator=(const ProvisionedAcquisition&) = delete;
  ProvisionedAcquisition& operator=(ProvisionedAcquisition&&) = delete;
  const Request& request() const noexcept { return request_; }
  const ProvisionedTarget& target() const noexcept { return target_; }
  std::optional<ProvisionedError> invariant_error() const;
  // Exact bridge request match; this is policy comparison, not issuer proof.
  bool matches(const Request&) const noexcept;
 private:
  ProvisionedAcquisition(Request request, ProvisionedTarget target)
      : request_(std::move(request)), target_(std::move(target)) {}
  Request request_;
  ProvisionedTarget target_;
};
// Untrusted owning response data. Only an explicitly injected TrustedIssuer can
// give its exact operation/result pairing production meaning. No public proof flag.
struct ProvisionedResponse {
  std::string user;
  SecretBytes password;
  std::optional<std::int64_t> expiration_epoch_milliseconds;
};
// Exact microsecond candidate, not SDK/issuer proof. No implicit ms conversion.
struct ProvisionedResponseUtc {
  std::string user;
  SecretBytes password;
  std::optional<UtcInstant> expiration_utc;
};
ProvisionedOutcome<Material> project_provisioned_response_utc(const ProvisionedAcquisition&,
    ProvisionedResponseUtc&&, const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* = nullptr);
// Consumes into local ownership before validation. Thread-confined SecretBytes
// consumption/move preconditions apply. Failure releases only our owned copies.
ProvisionedOutcome<Material> project_provisioned_response(const ProvisionedAcquisition&,
    ProvisionedResponse&&, const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* = nullptr);
} // namespace rs::core::auth
