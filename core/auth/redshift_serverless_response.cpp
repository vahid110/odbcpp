#include "redshift_serverless_response.h"
#include <limits>
#include <new>

namespace rs::core::auth {
namespace {
std::optional<std::string_view> partition(AwsPartition value) noexcept {
  switch (value) {
    case AwsPartition::Aws: return "aws";
    case AwsPartition::AwsCn: return "aws-cn";
    case AwsPartition::AwsUsGov: return "aws-us-gov";
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
bool id_valid(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128) return false;
  for (char c : value) if (!lower_or_digit(c) && !(c >= 'A' && c <= 'Z') && c != '-') return false;
  return true;
}
bool name_valid(std::string_view value) noexcept {
  if (value.size() < 3 || value.size() > 64) return false;
  for (char c : value) if (!lower_or_digit(c) && c != '-') return false;
  return true;
}
std::optional<ResponseError> validate(const Request& request, const ServerlessTarget& target) {
  if (request.binding().invariant_error()) return ResponseError{ResponseFailure::InvalidAcquisition};
  const auto& binding = request.binding();
  if (binding.target().service != Service::Redshift || binding.method() != Method::TemporaryDatabasePassword ||
      binding.source().kind != SourceKind::TrustedTemporaryDbIssuer)
    return ResponseError{ResponseFailure::UnsupportedMethod};
  const auto part = partition(target.partition);
  if (!part) return ResponseError{ResponseFailure::UnsupportedPartition};
  if (target.account.size() != 12 || !region_valid(target.region) || !id_valid(target.workgroup_id) ||
      !name_valid(target.workgroup_name) || target.duration_seconds < 900 || target.duration_seconds > 3600 ||
      target.database != binding.target().database) return ResponseError{ResponseFailure::InvalidAcquisition};
  for (char c : target.account) if (c < '0' || c > '9') return ResponseError{ResponseFailure::InvalidAcquisition};
  // Exact construction validates the full ARN grammar and every tuple component;
  // the name-to-ID association remains a trusted acquisition owner's obligation.
  std::string expected = "arn:";
  expected += *part; expected += ":redshift-serverless:";
  expected += target.region; expected += ':'; expected += target.account;
  expected += ":workgroup/"; expected += target.workgroup_id;
  if (expected != binding.target().resource_id) return ResponseError{ResponseFailure::InvalidAcquisition};
  return std::nullopt;
}
}
std::string_view ResponseError::safe_message() const noexcept {
  switch (failure) {
    case ResponseFailure::UnsupportedMethod: return "Serverless credential method unsupported";
    case ResponseFailure::UnsupportedPartition: return "Serverless partition policy unsupported";
    case ResponseFailure::InvalidAcquisition: return "Serverless acquisition policy malformed";
    case ResponseFailure::InvalidResponse: return "Serverless credential response malformed";
    case ResponseFailure::PrincipalMismatch: return "Serverless credential principal mismatch";
    case ResponseFailure::ExpiryRange: return "Serverless credential expiry range invalid";
    case ResponseFailure::TimeConversionFailed: return "Serverless credential time refused";
    case ResponseFailure::Cancelled: return "Serverless credential admission cancelled";
    case ResponseFailure::AllocationFailed: return "Serverless credential allocation unavailable";
  }
  return "Serverless credential response malformed";
}
ResponseOutcome<ServerlessAcquisition> ServerlessAcquisition::create(Request original, ServerlessTarget target) {
  try {
    if (auto error = validate(original, target)) return *error;
    return ServerlessAcquisition{std::move(original), std::move(target)};
  } catch (const std::bad_alloc&) { return ResponseError{ResponseFailure::AllocationFailed}; }
}
std::optional<ResponseError> ServerlessAcquisition::invariant_error() const { return validate(request_, target_); }
bool ServerlessAcquisition::matches(const Request& other) const noexcept {
  return !request_.binding().invariant_error() && !other.binding().invariant_error() &&
      request_.binding().target() == other.binding().target() && request_.binding().source() == other.binding().source() &&
      request_.binding().method() == other.binding().method() && request_.deadline() == other.deadline() &&
      request_.headroom() == other.headroom();
}
namespace {
std::optional<ResponseError> prevalidate(const ServerlessAcquisition& acquisition, const std::string& user,
    const SecretBytes& password, const Cancellation* cancel) {
  if (cancel && cancel->stop_requested()) return ResponseError{ResponseFailure::Cancelled};
  if (auto error = acquisition.invariant_error()) return *error;
  if (password.empty() || user.empty()) return ResponseError{ResponseFailure::InvalidResponse};
  if (user != acquisition.request().binding().target().principal)
    return ResponseError{ResponseFailure::PrincipalMismatch};
  return std::nullopt;
}
// Only called after operation-owned prevalidation and positive expiry validation.
ResponseOutcome<Material> finish_utc(const ServerlessAcquisition& acquisition, ServerlessResponseUtc&& response,
    const TemporaryClockSample& sample, const TemporaryTimePolicy& policy, const Cancellation* cancel) {
  auto owned = std::move(response);
  auto time = convert_temporary_db_validity(acquisition.request(), *owned.expiration_utc, sample, policy, cancel);
  if (!time) return ResponseError{ResponseFailure::TimeConversionFailed, time.failure()};
  auto material = Material::create(acquisition.request().binding(), MaterialKind::TemporaryDatabasePassword,
      std::move(owned.user), std::move(owned.password), time.value());
  if (!material) return ResponseError{ResponseFailure::InvalidResponse};
  if (cancel && cancel->stop_requested()) return ResponseError{ResponseFailure::Cancelled};
  return std::move(material).value();
}
}
ResponseOutcome<Material> project_serverless_response(const ServerlessAcquisition& acquisition,
    ServerlessResponse&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response); // Before every refusal, including cancellation.
  try {
    if (auto error = prevalidate(acquisition, owned.user, owned.password, cancel)) return *error;
    if (!owned.expiration_epoch_milliseconds || *owned.expiration_epoch_milliseconds <= 0 ||
        *owned.expiration_epoch_milliseconds > (std::numeric_limits<std::int64_t>::max)() / 1000)
      return ResponseError{ResponseFailure::ExpiryRange};
    ServerlessResponseUtc utc{std::move(owned.user), std::move(owned.password),
        UtcInstant{*owned.expiration_epoch_milliseconds * 1000}};
    return finish_utc(acquisition, std::move(utc), sample, policy, cancel);
  } catch (const std::bad_alloc&) { return ResponseError{ResponseFailure::AllocationFailed}; }
}
ResponseOutcome<Material> project_serverless_response_utc(const ServerlessAcquisition& acquisition,
    ServerlessResponseUtc&& response, const TemporaryClockSample& sample, const TemporaryTimePolicy& policy,
    const Cancellation* cancel) {
  auto owned = std::move(response); // Before every refusal, including cancellation.
  try {
    if (auto error = prevalidate(acquisition, owned.user, owned.password, cancel)) return *error;
    if (!owned.expiration_utc || owned.expiration_utc->microseconds_since_epoch <= 0)
      return ResponseError{ResponseFailure::ExpiryRange};
    return finish_utc(acquisition, std::move(owned), sample, policy, cancel);
  } catch (const std::bad_alloc&) { return ResponseError{ResponseFailure::AllocationFailed}; }
}
} // namespace rs::core::auth
