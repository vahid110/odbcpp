#pragma once
#include "odbcpp/util/deadline.h"
#include <memory>
#include <optional>

namespace rs::core::database {
namespace detail {
struct CredentialAuthority;
struct CredentialGeneration;
struct CredentialContextTestAccess;
}
class CredentialContext;
class SessionOwner;

// Private in-process capability: no secret, principal string, serialized ID or
// numeric epoch. The trusted coordinator binds one authority to one complete
// immutable server/principal/authentication/TLS policy context. This token does
// not itself prove that binding or authenticate a session/hostile plugin.
class CredentialToken final {
 public:
  CredentialToken(const CredentialToken&) = default;
  CredentialToken& operator=(const CredentialToken&) = default;
  CredentialToken(CredentialToken&&) noexcept = default;
  CredentialToken& operator=(CredentialToken&&) noexcept = default;
  bool is_current() const noexcept;
 private:
  friend class CredentialContext;
  friend class SessionOwner;
  friend struct detail::CredentialContextTestAccess;
  CredentialToken(std::weak_ptr<detail::CredentialAuthority>,
      std::shared_ptr<const detail::CredentialGeneration>) noexcept;
  bool same_generation(const CredentialToken&) const noexcept;
  bool is_current_at(rs::util::Clock::time_point) const noexcept;
  std::weak_ptr<detail::CredentialAuthority> authority_;
  std::shared_ptr<const detail::CredentialGeneration> generation_;
};

// Internal composition authority, initially revoked. Publish only after trusted
// authentication/refresh validation for the SAME immutable security context.
// A new generation invalidates all old copies, including if allocation fails.
// Optional expiry is monotonic and exclusive: now == expiry is invalid.
// Publish/revoke/current_token may run concurrently; same-object moves and
// destruction require external ordering. Destruction revokes outstanding tokens.
class CredentialContext final {
 public:
  CredentialContext();
  CredentialContext(const CredentialContext&) = delete;
  CredentialContext& operator=(const CredentialContext&) = delete;
  CredentialContext(CredentialContext&&) noexcept;
  CredentialContext& operator=(CredentialContext&&) noexcept;
  ~CredentialContext();
  CredentialToken publish_authenticated(std::optional<rs::util::Deadline> expiry = std::nullopt);
  std::optional<CredentialToken> current_token() const noexcept;
  void revoke() noexcept;
 private:
  friend struct detail::CredentialContextTestAccess;
  using GenerationFactory = std::shared_ptr<const detail::CredentialGeneration>(*)(std::optional<rs::util::Deadline>);
  explicit CredentialContext(GenerationFactory);
  static std::shared_ptr<const detail::CredentialGeneration> make_generation(std::optional<rs::util::Deadline>);
  std::shared_ptr<detail::CredentialAuthority> authority_;
};
} // namespace rs::core::database
