#pragma once
#include "odbcpp/auth/temporary_db_validity.h"
#include <optional>
#include <string>
#include <variant>

namespace rs::core::auth {
// Private SDK-free STS AssumeRole credential-source candidate, not AWS
// provenance or deployment support. AssumeRole yields AWS signing credentials
// (access key/secret/session token), NOT a database login Material, so this
// contract projects to its own owning credential type instead of Material.
// Local syntax checks do NOT implement full AWS ARN/name validation; the future
// authenticated STS issuer must do that separately.
enum class StsAssumeRolePartition { Aws, AwsCn, AwsUsGov };
enum class StsAssumeRoleFailure {
  UnsupportedPartition, InvalidAcquisition, InvalidResponse,
  ExpiryRange, TimeConversionFailed, Cancelled, AllocationFailed, DeadlineElapsed
};
struct StsAssumeRoleError {
  StsAssumeRoleFailure failure;
  std::optional<TimeFailure> time_failure{};
  std::string_view safe_message() const noexcept;
};
template<class T> class StsOutcome final {
 public:
  StsOutcome(T value) : value_(std::move(value)) {}
  StsOutcome(StsAssumeRoleError error) : value_(error) {}
  explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
  T& value() & { return std::get<T>(value_); }
  const T& value() const& { return std::get<T>(value_); }
  T&& value() && { return std::get<T>(std::move(value_)); }
  StsAssumeRoleError error() const { return std::get<StsAssumeRoleError>(value_); }
 private:
  std::variant<T, StsAssumeRoleError> value_;
};
struct StsAssumeRoleTarget {
  StsAssumeRolePartition partition;
  std::string account, region, role_name, role_session_name;
  unsigned duration_seconds;
  bool operator==(const StsAssumeRoleTarget&) const = default;
};
// Local split of arn:<partition>:iam::<12-digit account>:role/<name, optional
// path>. Syntax only (the role-name subset above); STS decides whether the role
// exists and trusts the caller.
struct StsRoleArn {
  StsAssumeRolePartition partition;
  std::string account, role_name;
};
std::optional<StsRoleArn> parse_sts_role_arn(std::string_view arn);
// Dedicated operation policy, not evidence that any AWS call occurred. The
// explicitly injected production issuer must retain authenticated pairing.
class StsAssumeRoleAcquisition final {
 public:
  static constexpr std::string_view operation = "AssumeRole";
  static StsOutcome<StsAssumeRoleAcquisition> create(rs::util::Deadline deadline,
      rs::util::Clock::duration headroom, StsAssumeRoleTarget target);
  StsAssumeRoleAcquisition(const StsAssumeRoleAcquisition&) = default;
  StsAssumeRoleAcquisition(StsAssumeRoleAcquisition&&) = default;
  StsAssumeRoleAcquisition& operator=(const StsAssumeRoleAcquisition&) = delete;
  StsAssumeRoleAcquisition& operator=(StsAssumeRoleAcquisition&&) = delete;
  rs::util::Deadline deadline() const noexcept { return deadline_; }
  rs::util::Clock::duration headroom() const noexcept { return headroom_; }
  const StsAssumeRoleTarget& target() const noexcept { return target_; }
  std::optional<StsAssumeRoleError> invariant_error() const noexcept;
  // Exact acquisition match; this is policy comparison, not issuer proof.
  bool matches(const StsAssumeRoleAcquisition& other) const noexcept;
 private:
  StsAssumeRoleAcquisition(rs::util::Deadline deadline, rs::util::Clock::duration headroom,
      StsAssumeRoleTarget target)
      : deadline_(deadline), headroom_(headroom), target_(std::move(target)) {}
  rs::util::Deadline deadline_;
  rs::util::Clock::duration headroom_;
  StsAssumeRoleTarget target_;
};
// Untrusted owning response data. Only an explicitly injected TrustedIssuer can
// give its exact operation/result pairing production meaning. No public proof flag.
struct StsAssumeRoleResponse {
  SecretBytes access_key_id, secret_access_key, session_token;
  std::optional<std::int64_t> expiration_epoch_milliseconds;
  std::string assumed_role_arn;
};
// Exact microsecond candidate, not SDK/issuer proof. No implicit ms conversion.
struct StsAssumeRoleResponseUtc {
  SecretBytes access_key_id, secret_access_key, session_token;
  std::optional<UtcInstant> expiration_utc;
  std::string assumed_role_arn;
};
// Projected, validated AWS signing credential (NOT a database login Material).
struct StsAssumedCredential {
  SecretBytes access_key_id, secret_access_key, session_token;
  Validity validity;
  std::string assumed_role_arn;
};
// Consumes into local ownership before validation. Thread-confined SecretBytes
// consumption/move preconditions apply. Failure releases only our owned copies.
StsOutcome<StsAssumedCredential> project_sts_assume_role_response(const StsAssumeRoleAcquisition&,
    StsAssumeRoleResponse&&, const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* = nullptr);
StsOutcome<StsAssumedCredential> project_sts_assume_role_response_utc(const StsAssumeRoleAcquisition&,
    StsAssumeRoleResponseUtc&&, const TemporaryClockSample&, const TemporaryTimePolicy&,
    const Cancellation* = nullptr);
} // namespace rs::core::auth
