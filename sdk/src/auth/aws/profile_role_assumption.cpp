#include "odbcpp/auth/aws/profile_role_assumption.h"
#include <aws/core/config/AWSConfigFileProfileConfigLoader.h>
#include <new>

namespace rs::core::auth::aws::profile_role {
namespace {
std::optional<SecretBytes> to_secret(const Aws::String& value) {
  auto made = SecretBytes::create(std::as_bytes(std::span{value.data(), value.size()}));
  if (!made) return std::nullopt;
  return std::move(made).value();
}
}
StsOutcome<StsAssumeRoleResponseUtc> resolve_assumed_role_credentials(
    const std::string& credentials_path, const std::string& profile,
    const std::string& region, const std::string& role_session_name,
    unsigned duration_seconds, rs::util::Deadline deadline,
    std::shared_ptr<provisioned_native::WorkerCancellation> cancel) {
  try {
    if (credentials_path.empty() || profile.empty() || region.empty() || role_session_name.empty() ||
        duration_seconds < 900 || duration_seconds > 3600 || !cancel)
      return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    Aws::Config::AWSConfigFileProfileConfigLoader loader{credentials_path.c_str(), false};
    if (!loader.Load()) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    const auto& profiles = loader.GetProfiles();
    const auto target = profiles.find(profile.c_str());
    if (target == profiles.end()) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    const std::string role_arn = target->second.GetRoleArn();
    if (role_arn.empty()) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    const std::string source_profile = target->second.GetSourceProfile();
    Aws::Auth::AWSCredentials source;
    if (!source_profile.empty()) {
      const auto src = profiles.find(source_profile.c_str());
      if (src == profiles.end()) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
      source = src->second.GetCredentials();
    } else {
      source = target->second.GetCredentials();
    }
    if (source.GetAWSAccessKeyId().empty() || source.GetAWSSecretKey().empty())
      return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    auto arn = parse_sts_role_arn(role_arn);
    if (!arn) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    auto access = to_secret(source.GetAWSAccessKeyId());
    auto secret = to_secret(source.GetAWSSecretKey());
    auto token = to_secret(source.GetSessionToken());
    if (!access || !secret || !token) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    StsAssumeRoleTarget target_{arn->partition, std::move(arn->account), region, std::move(arn->role_name),
        role_session_name, duration_seconds};
    auto owner = sts::StsAssumeRoleOwner::create(std::move(target_), std::move(*access), std::move(*secret),
        std::move(*token), deadline, cancel);
    if (!owner.owner) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    auto result = owner.owner->acquire();
    (void)owner.owner->close();
    return result;
  } catch (const std::bad_alloc&) { return StsAssumeRoleError{StsAssumeRoleFailure::AllocationFailed}; }
  catch (...) { return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse}; }
}
} // namespace rs::core::auth::aws::profile_role
