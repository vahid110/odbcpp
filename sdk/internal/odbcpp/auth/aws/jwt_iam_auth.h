#pragma once
#include "odbcpp/auth/aws/sts_assume_role_owner.h"
#include "odbcpp/auth/sts_assume_role_response.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace rs::core::auth::aws::jwt_iam {
// JwtIamAuthPlugin: a caller-supplied web identity token (JWT) and role_arn
// become assumed-role AWS credentials through STS AssumeRoleWithWebIdentity.
// The caller then issues database credentials (GetCredentials /
// GetClusterCredentials) exactly as for any other AWS credential source.
inline constexpr std::string_view default_role_session_name = "jwt_redshift_session";

// Database user for GetClusterCredentials, taken from the first present claim
// of DbUser, upn, preferred_username, email (the native driver's order). The
// payload is decoded locally WITHOUT signature verification; only STS verifies
// the token, so this name is a request parameter, never an authentication
// result. Returns nullopt for a malformed token or when no claim is usable.
std::optional<std::string> jwt_db_user(const SecretBytes& web_identity_token);

// An empty role_session_name selects default_role_session_name. No token,
// claim or credential value leaks through the returned outcome.
StsOutcome<StsAssumeRoleResponseUtc> assume_role_with_jwt(const std::string& role_arn,
    SecretBytes web_identity_token, const std::string& region, const std::string& role_session_name,
    unsigned duration_seconds, rs::util::Deadline deadline,
    std::shared_ptr<provisioned_native::WorkerCancellation> cancel);
} // namespace rs::core::auth::aws::jwt_iam
