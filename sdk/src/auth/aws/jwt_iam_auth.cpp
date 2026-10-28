#include "odbcpp/auth/aws/jwt_iam_auth.h"
#include <aws/core/utils/json/JsonSerializer.h>
#include <new>

namespace rs::core::auth::aws::jwt_iam {
namespace {
constexpr std::size_t max_db_user_bytes = 128; // Redshift identifier limit
int base64url_value(char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}
// Unpadded base64url (RFC 7515); trailing '=' padding is tolerated.
std::optional<std::string> base64url_decode(std::string_view text) {
  while (!text.empty() && text.back() == '=') text.remove_suffix(1);
  if (text.size() % 4 == 1) return std::nullopt;
  std::string out;
  out.reserve(text.size() * 3 / 4);
  unsigned buffer = 0;
  int bits = 0;
  for (char c : text) {
    const int value = base64url_value(c);
    if (value < 0) return std::nullopt;
    buffer = (buffer << 6) | static_cast<unsigned>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return out;
}
bool db_user_valid(std::string_view value) noexcept {
  if (value.empty() || value.size() > max_db_user_bytes) return false;
  for (unsigned char c : value) if (c < 0x20 || c == 0x7f) return false;
  return true;
}
} // namespace

std::optional<std::string> jwt_db_user(const SecretBytes& web_identity_token) {
  try {
    std::optional<std::string> payload;
    web_identity_token.with_bytes([&](std::span<const std::byte> bytes) {
      const std::string_view jwt(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      const auto first = jwt.find('.');
      if (first == std::string_view::npos) return;
      const auto second = jwt.find('.', first + 1);
      if (second == std::string_view::npos || jwt.find('.', second + 1) != std::string_view::npos) return;
      payload = base64url_decode(jwt.substr(first + 1, second - first - 1));
    });
    if (!payload) return std::nullopt;
    const Aws::Utils::Json::JsonValue json(Aws::String(payload->data(), payload->size()));
    if (!json.WasParseSuccessful()) return std::nullopt;
    const auto view = json.View();
    if (!view.IsObject()) return std::nullopt;
    for (const char* claim : {"DbUser", "upn", "preferred_username", "email"}) {
      if (!view.ValueExists(claim) || !view.GetObject(claim).IsString()) continue;
      const auto value = view.GetString(claim);
      if (value.empty()) continue;
      if (!db_user_valid(std::string_view(value.data(), value.size()))) return std::nullopt;
      return std::string(value.data(), value.size());
    }
    return std::nullopt;
  } catch (...) { return std::nullopt; }
}

StsOutcome<StsAssumeRoleResponseUtc> assume_role_with_jwt(const std::string& role_arn,
    SecretBytes web_identity_token, const std::string& region, const std::string& role_session_name,
    unsigned duration_seconds, rs::util::Deadline deadline,
    std::shared_ptr<provisioned_native::WorkerCancellation> cancel) {
  try {
    if (web_identity_token.empty() || region.empty() || duration_seconds < 900 || duration_seconds > 3600 || !cancel)
      return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    auto arn = parse_sts_role_arn(role_arn);
    if (!arn) return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    std::string session = role_session_name.empty() ? std::string(default_role_session_name) : role_session_name;
    StsAssumeRoleTarget target{arn->partition, std::move(arn->account), region, std::move(arn->role_name),
        std::move(session), duration_seconds};
    auto owner = sts::StsAssumeRoleOwner::create_web_identity(std::move(target), std::move(web_identity_token),
        deadline, std::move(cancel));
    if (!owner.owner) {
      if (owner.failure == sts::OwnerFailure::DeadlineElapsed)
        return StsAssumeRoleError{StsAssumeRoleFailure::DeadlineElapsed};
      return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse};
    }
    auto result = owner.owner->acquire();
    (void)owner.owner->close();
    return result;
  } catch (const std::bad_alloc&) { return StsAssumeRoleError{StsAssumeRoleFailure::AllocationFailed}; }
  catch (...) { return StsAssumeRoleError{StsAssumeRoleFailure::InvalidResponse}; }
}
} // namespace rs::core::auth::aws::jwt_iam
