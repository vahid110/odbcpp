#pragma once
#include "odbcpp/auth/aws/sts_assume_role_owner.h"
#include "odbcpp/auth/sts_assume_role_response.h"
#include <memory>
#include <string>

namespace rs::core::auth::aws::profile_role {
// Resolves a profile that declares role_arn (with inline static keys or a
// source_profile section) into assumed-role temporary credentials via STS
// AssumeRole. Self-contained: the caller supplies the profile file path and
// region. No secret value or profile path leaks through the returned outcome.
// The trusted-clock projection stays with the caller (mirroring the split
// between StsAssumeRoleOwner and project_sts_assume_role_response).
StsOutcome<StsAssumeRoleResponseUtc> resolve_assumed_role_credentials(
    const std::string& credentials_path, const std::string& profile,
    const std::string& region, const std::string& role_session_name,
    unsigned duration_seconds, rs::util::Deadline deadline,
    std::shared_ptr<provisioned_native::WorkerCancellation> cancel);
} // namespace rs::core::auth::aws::profile_role
