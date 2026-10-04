#include "redshift_withiam_response.h"
#include <limits>
#include <new>

namespace rs::core::auth {
namespace {
std::optional<std::string_view> partition(WithIamPartition value) noexcept {
  switch (value) {
    case WithIamPartition::Aws: return "aws";
    case WithIamPartition::AwsCn: return "aws-cn";
    case WithIamPartition::AwsUsGov: return "aws-us-gov";
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
std::optional<WithIamError> validate(const Request& request, const WithIamTarget& target) {
  if (request.binding().invariant_error()) return WithIamError{WithIamFailure::InvalidAcquisition};
  const auto& binding = request.binding();
  if (binding.target().service != Service::Redshift || binding.method() != Method::TemporaryDatabasePassword ||
      binding.source().kind != SourceKind::TrustedTemporaryDbIssuer)
    return WithIamError{WithIamFailure::UnsupportedMethod};
  const auto part = partition(target.partition);
  if (!part) return WithIamError{WithIamFailure::UnsupportedPartition};
  if (target.account.size() != 12 || !region_valid(target.region) || !cluster_valid(target.cluster_identifier) ||
      !name_valid(target.database) || target.expected_principal.empty() || target.expected_principal.size() > 127 ||
      target.duration_seconds < 900 || target.duration_seconds > 3600 ||
      target.database != binding.target().database || target.expected_principal != binding.target().principal)
    return WithIamError{WithIamFailure::InvalidAcquisition};
  for (char c : target.account) if (c < '0' || c > '9') return WithIamError{WithIamFailure::InvalidAcquisition};
  // Exact construction validates the full ARN grammar and every tuple component;
  // authenticated operation/result pairing remains the selected issuer's obligation.
  std::string expected = "arn:";
  expected += *part; expected += ":redshift:";
  expected += target.region; expected += ':'; expected += target.account;
  expected += ":cluster:"; expected += target.cluster_identifier;
  if (expected != binding.target().resource_id) return WithIamError{WithIamFailure::InvalidAcquisition};
  return std::nullopt;
}
}
std::string_view WithIamError::safe_message() const noexcept {
  switch (failure) {
    case WithIamFailure::UnsupportedMethod: return "WithIAM credential method unsupported";
    case WithIamFailure::UnsupportedPartition: return "WithIAM partition policy unsupported";
    case WithIamFailure::InvalidAcquisition: return "WithIAM acquisition policy malformed";
    case WithIamFailure::InvalidResponse: return "WithIAM credential response malformed";
    case WithIamFailure::PrincipalMismatch: return "WithIAM credential principal mismatch";
    case WithIamFailure::ExpiryRange: return "WithIAM credential expiry range invalid";
    case WithIamFailure::TimeConversionFailed: return "WithIAM credential time refused";
    case WithIamFailure::Cancelled: return "WithIAM credential admission cancelled";
    case WithIamFailure::AllocationFailed: return "WithIAM credential allocation unavailable";
  }
  return "WithIAM credential response malformed";
}
WithIamOutcome<WithIamAcquisition> WithIamAcquisition::create(Request original, WithIamTarget target) {
  try {
    if (auto error = validate(original, target)) return *error;
    return WithIamAcquisition{std::move(original), std::move(target)};
  } catch (const std::bad_alloc&) { return WithIamError{WithIamFailure::AllocationFailed}; }
}
std::optional<WithIamError> WithIamAcquisition::invariant_error() const { return validate(request_, target_); }
bool WithIamAcquisition::matches(const Request& other) const noexcept {
  return !request_.binding().invariant_error() && !other.binding().invariant_error() &&
      request_.binding().target() == other.binding().target() && request_.binding().source() == other.binding().source() &&
      request_.binding().method() == other.binding().method() && request_.deadline() == other.deadline() &&
      request_.headroom() == other.headroom();
}
namespace {
std::optional<WithIamError> prevalidate(const WithIamAcquisition& acquisition, const std::string& user,
    const SecretBytes& password, const Cancellation* cancel) {
  if (cancel && cancel->stop_requested()) return WithIamError{WithIamFailure::Cancelled};
  if (auto error = acquisition.invariant_error()) return *error;
  if (password.empty() || user.empty()) return WithIamError{WithIamFailure::InvalidResponse};
  if (user != acquisition.request().binding().target().principal)
    return WithIamError{WithIamFailure::PrincipalMismatch};
  return std::nullopt;
}
// Only called after operation-owned prevalidation and positive expiry validation.
WithIamOutcome<Material> finish_utc(const WithIamAcquisition& acquisition, WithIamResponseUtc&& response,
    const TemporaryClockSample& sample, const TemporaryTimePolicy& policy, const Cancellation* cancel) {
  auto owned = std::move(response);
  auto time = convert_temporary_db_validity(acquisition.request(), *owned.expiration_utc, sample, policy, cancel);
  if (!time) return WithIamError{WithIamFailure::TimeConversionFailed, time.failure()};
  auto material = Material::create(acquisition.request().binding(), MaterialKind::TemporaryDatabasePassword,
      std::move(owned.user), std::move(owned.password), time.value());
  if (!material) return WithIamError{WithIamFailure::InvalidResponse};
  if (cancel && cancel->stop_requested()) return WithIamError{WithIamFailure::Cancelled};
  return std::move(material).value();
}
}
WithIamOutcome<Material> project_withiam_response(const WithIamAcquisition& acquisition,
    WithIamResponse&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response); // Before every refusal, including cancellation.
  try {
    if (auto error = prevalidate(acquisition, owned.user, owned.password, cancel)) return *error;
    if (!owned.expiration_epoch_milliseconds || *owned.expiration_epoch_milliseconds <= 0 ||
        *owned.expiration_epoch_milliseconds > (std::numeric_limits<std::int64_t>::max)() / 1000)
      return WithIamError{WithIamFailure::ExpiryRange};
    WithIamResponseUtc utc{std::move(owned.user), std::move(owned.password),
        UtcInstant{*owned.expiration_epoch_milliseconds * 1000}};
    return finish_utc(acquisition, std::move(utc), sample, policy, cancel);
  } catch (const std::bad_alloc&) { return WithIamError{WithIamFailure::AllocationFailed}; }
}
WithIamOutcome<Material> project_withiam_response_utc(const WithIamAcquisition& acquisition,
    WithIamResponseUtc&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response); // Before every refusal, including cancellation.
  try {
    if (auto error = prevalidate(acquisition, owned.user, owned.password, cancel)) return *error;
    if (!owned.expiration_utc || owned.expiration_utc->microseconds_since_epoch <= 0)
      return WithIamError{WithIamFailure::ExpiryRange};
    return finish_utc(acquisition, std::move(owned), sample, policy, cancel);
  } catch (const std::bad_alloc&) { return WithIamError{WithIamFailure::AllocationFailed}; }
}
} // namespace rs::core::auth
