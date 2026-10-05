#include "odbcpp/session/credential_context.h"
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace rs::core::database {
namespace detail {
struct CredentialGeneration {
  explicit CredentialGeneration(std::optional<rs::util::Deadline> value) : expiry(value) {}
  const std::optional<rs::util::Deadline> expiry;
};
struct CredentialAuthority {
  using Factory = std::shared_ptr<const CredentialGeneration>(*)(std::optional<rs::util::Deadline>);
  explicit CredentialAuthority(Factory value) : factory(value) {}
  std::mutex mutex;
  std::shared_ptr<const CredentialGeneration> current;
  const Factory factory;
};
}
CredentialToken::CredentialToken(std::weak_ptr<detail::CredentialAuthority> authority,
    std::shared_ptr<const detail::CredentialGeneration> generation) noexcept
    : authority_(std::move(authority)), generation_(std::move(generation)) {}
bool CredentialToken::same_generation(const CredentialToken& other) const noexcept {
  return generation_ && generation_ == other.generation_ &&
      !authority_.owner_before(other.authority_) && !other.authority_.owner_before(authority_);
}
bool CredentialToken::is_current_at(rs::util::Clock::time_point now) const noexcept {
  auto authority = authority_.lock();
  if (!authority || !generation_) return false;
  std::lock_guard lock(authority->mutex);
  return authority->current == generation_ &&
      (!generation_->expiry || now < *generation_->expiry);
}
bool CredentialToken::is_current() const noexcept {
  auto authority = authority_.lock();
  if (!authority || !generation_) return false;
  std::lock_guard lock(authority->mutex);
  // Read time after locking: a contended validation must not use stale time.
  return authority->current == generation_ &&
      (!generation_->expiry || rs::util::Clock::now() < *generation_->expiry);
}
std::shared_ptr<const detail::CredentialGeneration> CredentialContext::make_generation(
    std::optional<rs::util::Deadline> expiry) {
  return std::make_shared<const detail::CredentialGeneration>(expiry);
}
CredentialContext::CredentialContext() : CredentialContext(&make_generation) {}
CredentialContext::CredentialContext(GenerationFactory factory)
    : authority_(std::make_shared<detail::CredentialAuthority>(factory)) {}
CredentialContext::CredentialContext(CredentialContext&& other) noexcept
    : authority_(std::move(other.authority_)) {}
CredentialContext& CredentialContext::operator=(CredentialContext&& other) noexcept {
  if (this != &other) { revoke(); authority_ = std::move(other.authority_); }
  return *this;
}
CredentialContext::~CredentialContext() { revoke(); }
CredentialToken CredentialContext::publish_authenticated(std::optional<rs::util::Deadline> expiry) {
  if (!authority_) throw std::logic_error("CredentialContext is moved from");
  std::lock_guard lock(authority_->mutex);
  // Fail closed before allocation: never leave old credentials valid after a
  // failed attempted refresh. Production factory is local allocation only;
  // the private test factory must not reenter or call any backend/owner.
  authority_->current.reset();
  authority_->current = authority_->factory(expiry);
  if (!authority_->current) throw std::bad_alloc{};
  return CredentialToken{authority_, authority_->current};
}
std::optional<CredentialToken> CredentialContext::current_token() const noexcept {
  if (!authority_) return std::nullopt;
  std::lock_guard lock(authority_->mutex);
  const auto& current = authority_->current;
  if (!current || (current->expiry && rs::util::Clock::now() >= *current->expiry)) return std::nullopt;
  return CredentialToken{authority_, current};
}
void CredentialContext::revoke() noexcept {
  if (!authority_) return;
  std::lock_guard lock(authority_->mutex);
  authority_->current.reset();
}
} // namespace rs::core::database
