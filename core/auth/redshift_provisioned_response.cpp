#include "redshift_provisioned_response.h"
#include <limits>
#include <new>

namespace rs::core::auth {
namespace {
std::optional<std::string_view> partition(ProvisionedPartition value) noexcept {
  switch (value) {
    case ProvisionedPartition::Aws: return "aws";
    case ProvisionedPartition::AwsCn: return "aws-cn";
    case ProvisionedPartition::AwsUsGov: return "aws-us-gov";
  }
  return std::nullopt;
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
bool cluster_valid(std::string_view value) noexcept {
  if (value.empty() || value.size() > 63) return false;
  for (char c : value) if (!lower_or_digit(c) && !(c >= 'A' && c <= 'Z') && c != '-') return false;
  return true;
}
bool name_valid(std::string_view value) noexcept {
  if (value.empty() || value.size() > 64) return false;
  const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  if (!letter(value.front())) return false;
  for (char c : value) if (!letter(c) && !(c >= '0' && c <= '9') && c != '_' && c != '+' &&
      c != '.' && c != '@' && c != '-') return false;
  return true;
}
bool public_user(std::string_view value) noexcept {
  if (value.size() != 6) return false;
  constexpr std::string_view reserved = "public";
  for (std::size_t i = 0; i != value.size(); ++i) {
    char c = value[i]; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != reserved[i]) return false;
  }
  return true;
}
std::optional<ProvisionedError> validate(const Request& request, const ProvisionedTarget& target) {
  if (request.binding().invariant_error()) return ProvisionedError{ProvisionedFailure::InvalidAcquisition};
  const auto& binding = request.binding();
  if (binding.target().service != Service::Redshift || binding.method() != Method::TemporaryDatabasePassword ||
      binding.source().kind != SourceKind::TrustedTemporaryDbIssuer)
    return ProvisionedError{ProvisionedFailure::UnsupportedMethod};
  const auto part = partition(target.partition);
  if (!part) return ProvisionedError{ProvisionedFailure::UnsupportedPartition};
  if (!target.auto_create || *target.auto_create || target.groups)
    return ProvisionedError{ProvisionedFailure::UnsupportedPolicy};
  if (target.account.size() != 12 || !region_valid(target.region) || !cluster_valid(target.cluster_identifier) ||
      !name_valid(target.requested_user) || public_user(target.requested_user) || !name_valid(target.database) ||
      target.duration_seconds < 900 || target.duration_seconds > 3600 ||
      target.database != binding.target().database || "IAM:" + target.requested_user != binding.target().principal)
    return ProvisionedError{ProvisionedFailure::InvalidAcquisition};
  for (char c : target.account) if (c < '0' || c > '9') return ProvisionedError{ProvisionedFailure::InvalidAcquisition};
  // Exact construction validates the full ARN grammar and every tuple component;
  // authenticated operation/result pairing remains the selected issuer's obligation.
  std::string expected = "arn:";
  expected += *part; expected += ":redshift:";
  expected += target.region; expected += ':'; expected += target.account;
  expected += ":cluster:"; expected += target.cluster_identifier;
  if (expected != binding.target().resource_id) return ProvisionedError{ProvisionedFailure::InvalidAcquisition};
  return std::nullopt;
}
}
std::string_view ProvisionedError::safe_message() const noexcept {
  switch (failure) {
    case ProvisionedFailure::UnsupportedMethod: return "Provisioned credential method unsupported";
    case ProvisionedFailure::UnsupportedPartition: return "Provisioned partition policy unsupported";
    case ProvisionedFailure::UnsupportedPolicy: return "Provisioned credential policy unsupported";
    case ProvisionedFailure::InvalidAcquisition: return "Provisioned acquisition policy malformed";
    case ProvisionedFailure::InvalidResponse: return "Provisioned credential response malformed";
    case ProvisionedFailure::PrincipalMismatch: return "Provisioned credential principal mismatch";
    case ProvisionedFailure::ExpiryRange: return "Provisioned credential expiry range invalid";
    case ProvisionedFailure::TimeConversionFailed: return "Provisioned credential time refused";
    case ProvisionedFailure::Cancelled: return "Provisioned credential admission cancelled";
    case ProvisionedFailure::AllocationFailed: return "Provisioned credential allocation unavailable";
  }
  return "Provisioned credential response malformed";
}
ProvisionedOutcome<ProvisionedAcquisition> ProvisionedAcquisition::create(Request original, ProvisionedTarget target) {
  try {
    if (auto error = validate(original, target)) return *error;
    return ProvisionedAcquisition{std::move(original), std::move(target)};
  } catch (const std::bad_alloc&) { return ProvisionedError{ProvisionedFailure::AllocationFailed}; }
}
std::optional<ProvisionedError> ProvisionedAcquisition::invariant_error() const { return validate(request_, target_); }
bool ProvisionedAcquisition::matches(const Request& other) const noexcept {
  return !request_.binding().invariant_error() && !other.binding().invariant_error() &&
      request_.binding().target() == other.binding().target() && request_.binding().source() == other.binding().source() &&
      request_.binding().method() == other.binding().method() && request_.deadline() == other.deadline() &&
      request_.headroom() == other.headroom();
}
namespace {
std::optional<ProvisionedError> prevalidate(const ProvisionedAcquisition& acquisition, const std::string& user,
    const SecretBytes& password, const Cancellation* cancel) {
  if (cancel && cancel->stop_requested()) return ProvisionedError{ProvisionedFailure::Cancelled};
  if (auto error = acquisition.invariant_error()) return *error;
  if (password.empty() || user.empty()) return ProvisionedError{ProvisionedFailure::InvalidResponse};
  if (user != acquisition.request().binding().target().principal)
    return ProvisionedError{ProvisionedFailure::PrincipalMismatch};
  return std::nullopt;
}
// Only called after operation-owned prevalidation and positive expiry validation.
ProvisionedOutcome<Material> finish_utc(const ProvisionedAcquisition& acquisition, ProvisionedResponseUtc&& response,
    const TemporaryClockSample& sample, const TemporaryTimePolicy& policy, const Cancellation* cancel) {
  auto owned = std::move(response);
  auto time = convert_temporary_db_validity(acquisition.request(), *owned.expiration_utc, sample, policy, cancel);
  if (!time) return ProvisionedError{ProvisionedFailure::TimeConversionFailed, time.failure()};
  auto material = Material::create(acquisition.request().binding(), MaterialKind::TemporaryDatabasePassword,
      std::move(owned.user), std::move(owned.password), time.value());
  if (!material) return ProvisionedError{ProvisionedFailure::InvalidResponse};
  if (cancel && cancel->stop_requested()) return ProvisionedError{ProvisionedFailure::Cancelled};
  return std::move(material).value();
}
}
ProvisionedOutcome<Material> project_provisioned_response(const ProvisionedAcquisition& acquisition,
    ProvisionedResponse&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response); // Before every refusal, including cancellation.
  try {
    if (auto error = prevalidate(acquisition, owned.user, owned.password, cancel)) return *error;
    if (!owned.expiration_epoch_milliseconds || *owned.expiration_epoch_milliseconds <= 0 ||
        *owned.expiration_epoch_milliseconds > (std::numeric_limits<std::int64_t>::max)() / 1000)
      return ProvisionedError{ProvisionedFailure::ExpiryRange};
    ProvisionedResponseUtc utc{std::move(owned.user), std::move(owned.password),
        UtcInstant{*owned.expiration_epoch_milliseconds * 1000}};
    return finish_utc(acquisition, std::move(utc), sample, policy, cancel);
  } catch (const std::bad_alloc&) { return ProvisionedError{ProvisionedFailure::AllocationFailed}; }
}
ProvisionedOutcome<Material> project_provisioned_response_utc(const ProvisionedAcquisition& acquisition,
    ProvisionedResponseUtc&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response); // Before every refusal, including cancellation.
  try {
    if (auto error = prevalidate(acquisition, owned.user, owned.password, cancel)) return *error;
    if (!owned.expiration_utc || owned.expiration_utc->microseconds_since_epoch <= 0)
      return ProvisionedError{ProvisionedFailure::ExpiryRange};
    return finish_utc(acquisition, std::move(owned), sample, policy, cancel);
  } catch (const std::bad_alloc&) { return ProvisionedError{ProvisionedFailure::AllocationFailed}; }
}
} // namespace rs::core::auth
