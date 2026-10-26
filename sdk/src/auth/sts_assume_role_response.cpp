#include "odbcpp/auth/sts_assume_role_response.h"
#include <limits>
#include <new>

namespace rs::core::auth {
namespace {
std::optional<std::string_view> partition(StsAssumeRolePartition value) noexcept {
  switch (value) {
    case StsAssumeRolePartition::Aws: return "aws";
    case StsAssumeRolePartition::AwsCn: return "aws-cn";
    case StsAssumeRolePartition::AwsUsGov: return "aws-us-gov";
  }
  return std::nullopt;
}
bool finite(rs::util::Deadline t) noexcept {
  return t != rs::util::Deadline::min() && t != rs::util::Deadline::max();
}
bool lower_or_digit(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool region_valid(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128 || value.front() == '-' || value.back() == '-') return false;
  char previous = 0;
  for (char c : value) {
    if ((!lower_or_digit(c) && c != '-') || (c == '-' && previous == '-')) return false;
    previous = c;
  }
  return true;
}
// Local IAM role-name grammar subset (including an optional path prefix), not
// AWS-complete reserved-word validation.
bool role_name_valid(std::string_view value) noexcept {
  if (value.empty() || value.size() > 512 || value.front() == '/' || value.back() == '/') return false;
  char previous = 0;
  for (char c : value) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '+' || c == '=' || c == ',' || c == '.' || c == '@' || c == '_' || c == '-' || c == '/';
    if (!ok) return false;
    if (c == '/' && previous == '/') return false; // no empty path segment
    previous = c;
  }
  return true;
}
bool role_session_name_valid(std::string_view value) noexcept {
  if (value.size() < 2 || value.size() > 64) return false;
  return role_name_valid(value);
}
bool account_valid(std::string_view value) noexcept {
  if (value.size() != 12) return false;
  for (char c : value) if (c < '0' || c > '9') return false;
  return true;
}
std::optional<StsAssumeRoleError> validate(rs::util::Deadline deadline, rs::util::Clock::duration headroom,
    const StsAssumeRoleTarget& target) {
  if (!partition(target.partition)) return StsAssumeRoleError{StsAssumeRoleFailure::UnsupportedPartition};
  if (!finite(deadline) || headroom < rs::util::Clock::duration::zero() ||
      headroom > std::chrono::hours{24}) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidAcquisition};
  if (!account_valid(target.account) || !region_valid(target.region) ||
      !role_name_valid(target.role_name) || !role_session_name_valid(target.role_session_name) ||
      target.duration_seconds < 900 || target.duration_seconds > 3600)
    return StsAssumeRoleError{StsAssumeRoleFailure::InvalidAcquisition};
  return std::nullopt;
}
bool secret_ok(const SecretBytes& secret) noexcept {
  bool ok = !secret.empty();
  secret.with_bytes([&](std::span<const std::byte> bytes) {
    for (auto byte : bytes) if (byte == std::byte{0}) { ok = false; break; }
  });
  return ok;
}
bool arn_ok(std::string_view value) noexcept {
  if (value.empty() || value.size() > 2048) return false;
  for (unsigned char c : value) if (c < 0x20 || c == 0x7f) return false;
  return true;
}
}
std::optional<StsRoleArn> parse_sts_role_arn(std::string_view arn) {
  if (!arn.starts_with("arn:")) return std::nullopt;
  arn.remove_prefix(4);
  auto next = [&]() -> std::optional<std::string_view> {
    const auto p = arn.find(':');
    if (p == std::string_view::npos) return std::nullopt;
    const auto part = arn.substr(0, p);
    arn.remove_prefix(p + 1);
    return part;
  };
  StsRoleArn out{};
  const auto part = next();
  if (part == "aws") out.partition = StsAssumeRolePartition::Aws;
  else if (part == "aws-cn") out.partition = StsAssumeRolePartition::AwsCn;
  else if (part == "aws-us-gov") out.partition = StsAssumeRolePartition::AwsUsGov;
  else return std::nullopt;
  if (next() != "iam") return std::nullopt;
  const auto region = next(); // iam ARNs have an empty region field
  if (!region || !region->empty()) return std::nullopt;
  const auto account = next();
  if (!account || !account_valid(*account)) return std::nullopt;
  if (!arn.starts_with("role/")) return std::nullopt;
  arn.remove_prefix(5);
  if (!role_name_valid(arn)) return std::nullopt;
  out.account.assign(*account);
  out.role_name.assign(arn);
  return out;
}
std::string_view StsAssumeRoleError::safe_message() const noexcept {
  switch (failure) {
    case StsAssumeRoleFailure::UnsupportedPartition: return "STS AssumeRole partition unsupported";
    case StsAssumeRoleFailure::InvalidAcquisition: return "STS AssumeRole acquisition policy malformed";
    case StsAssumeRoleFailure::InvalidResponse: return "STS AssumeRole credential response malformed";
    case StsAssumeRoleFailure::ExpiryRange: return "STS AssumeRole credential expiry range invalid";
    case StsAssumeRoleFailure::TimeConversionFailed: return "STS AssumeRole credential time refused";
    case StsAssumeRoleFailure::Cancelled: return "STS AssumeRole credential admission cancelled";
    case StsAssumeRoleFailure::AllocationFailed: return "STS AssumeRole credential allocation unavailable";
    case StsAssumeRoleFailure::DeadlineElapsed: return "STS AssumeRole credential deadline elapsed";
  }
  return "STS AssumeRole credential response malformed";
}
StsOutcome<StsAssumeRoleAcquisition> StsAssumeRoleAcquisition::create(
    rs::util::Deadline deadline, rs::util::Clock::duration headroom, StsAssumeRoleTarget target) {
  try {
    if (auto error = validate(deadline, headroom, target)) return *error;
    return StsAssumeRoleAcquisition{deadline, headroom, std::move(target)};
  } catch (const std::bad_alloc&) { return StsAssumeRoleError{StsAssumeRoleFailure::AllocationFailed}; }
}
std::optional<StsAssumeRoleError> StsAssumeRoleAcquisition::invariant_error() const noexcept {
  return validate(deadline_, headroom_, target_);
}
bool StsAssumeRoleAcquisition::matches(const StsAssumeRoleAcquisition& other) const noexcept {
  return deadline_ == other.deadline_ && headroom_ == other.headroom_ && target_ == other.target_;
}
namespace {
std::optional<StsAssumeRoleError> prevalidate(const StsAssumeRoleAcquisition& acquisition,
    const SecretBytes& access_key_id, const SecretBytes& secret_access_key, const SecretBytes& session_token,
    const std::string& assumed_role_arn, const Cancellation* cancel) {
  if (cancel && cancel->stop_requested()) return StsAssumeRoleError{StsAssumeRoleFailure::Cancelled};
  if (auto error = acquisition.invariant_error()) return *error;
  if (!secret_ok(access_key_id) || !secret_ok(secret_access_key) || !secret_ok(session_token) ||
      !arn_ok(assumed_role_arn)) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
  return std::nullopt;
}
// Only called after operation-owned prevalidation and positive expiry validation.
StsOutcome<StsAssumedCredential> finish_utc(const StsAssumeRoleAcquisition& acquisition,
    StsAssumeRoleResponseUtc&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response);
  auto time = convert_temporary_validity(acquisition.deadline(), acquisition.headroom(), *owned.expiration_utc,
      sample, policy, cancel);
  if (!time) return StsAssumeRoleError{StsAssumeRoleFailure::TimeConversionFailed, time.failure()};
  StsAssumedCredential credential{std::move(owned.access_key_id), std::move(owned.secret_access_key),
      std::move(owned.session_token), time.value(), std::move(owned.assumed_role_arn)};
  if (cancel && cancel->stop_requested()) return StsAssumeRoleError{StsAssumeRoleFailure::Cancelled};
  return credential;
}
}
StsOutcome<StsAssumedCredential> project_sts_assume_role_response(const StsAssumeRoleAcquisition& acquisition,
    StsAssumeRoleResponse&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response); // Before every refusal, including cancellation.
  try {
    if (auto error = prevalidate(acquisition, owned.access_key_id, owned.secret_access_key, owned.session_token,
            owned.assumed_role_arn, cancel)) return *error;
    if (!owned.expiration_epoch_milliseconds || *owned.expiration_epoch_milliseconds <= 0 ||
        *owned.expiration_epoch_milliseconds > (std::numeric_limits<std::int64_t>::max)() / 1000)
      return StsAssumeRoleError{StsAssumeRoleFailure::ExpiryRange};
    StsAssumeRoleResponseUtc utc{std::move(owned.access_key_id), std::move(owned.secret_access_key),
        std::move(owned.session_token), UtcInstant{*owned.expiration_epoch_milliseconds * 1000},
        std::move(owned.assumed_role_arn)};
    return finish_utc(acquisition, std::move(utc), sample, policy, cancel);
  } catch (const std::bad_alloc&) { return StsAssumeRoleError{StsAssumeRoleFailure::AllocationFailed}; }
}
StsOutcome<StsAssumedCredential> project_sts_assume_role_response_utc(const StsAssumeRoleAcquisition& acquisition,
    StsAssumeRoleResponseUtc&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response); // Before every refusal, including cancellation.
  try {
    if (auto error = prevalidate(acquisition, owned.access_key_id, owned.secret_access_key, owned.session_token,
            owned.assumed_role_arn, cancel)) return *error;
    if (!owned.expiration_utc || owned.expiration_utc->microseconds_since_epoch <= 0)
      return StsAssumeRoleError{StsAssumeRoleFailure::ExpiryRange};
    return finish_utc(acquisition, std::move(owned), sample, policy, cancel);
  } catch (const std::bad_alloc&) { return StsAssumeRoleError{StsAssumeRoleFailure::AllocationFailed}; }
}
} // namespace rs::core::auth
