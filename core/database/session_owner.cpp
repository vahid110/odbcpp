#include "session_owner.h"
#include <mutex>
#include <stdexcept>
#include <utility>

namespace rs::core::database {
namespace detail {
struct SessionOwnershipState {
  explicit SessionOwnershipState(std::unique_ptr<IDatabaseConnection> value, std::optional<CredentialToken> token)
      : session(std::move(value)), credential(std::move(token)) {}
  std::mutex mutex;
  std::unique_ptr<IDatabaseConnection> session;
  const std::optional<CredentialToken> credential;
  bool accepting{true};
  bool leased{false};
};
}
namespace {
// Backends must release transport resources in their destructor. Disconnect is
// best-effort for a faulty external backend; no exception may escape RAII.
void retire_session(std::unique_ptr<IDatabaseConnection> session) noexcept {
  if (!session) return;
  try { session->disconnect(); } catch (...) {}
}
}

SessionOwner::SessionOwner(std::unique_ptr<IDatabaseConnection> session)
    : SessionOwner(std::move(session), std::nullopt) {}
SessionOwner::SessionOwner(std::unique_ptr<IDatabaseConnection> session, CredentialToken token)
    : SessionOwner(std::move(session), std::optional{std::move(token)}) {}
SessionOwner::SessionOwner(std::unique_ptr<IDatabaseConnection> session, std::optional<CredentialToken> token)
    : state_(std::make_shared<detail::SessionOwnershipState>(std::move(session), std::move(token))) {
  if (!state_->session) throw std::invalid_argument("SessionOwner requires a physical session");
}
SessionOwner::SessionOwner(SessionOwner&& other) noexcept
    : state_(std::move(other.state_)) {}
SessionOwner& SessionOwner::operator=(SessionOwner&& other) noexcept {
  if (this != &other) { close(); state_ = std::move(other.state_); }
  return *this;
}
SessionOwner::~SessionOwner() { close(); }
void SessionOwner::close() noexcept {
  auto state = std::move(state_);
  if (!state) return;
  std::unique_ptr<IDatabaseConnection> retired;
  {
    std::lock_guard lock(state->mutex);
    state->accepting = false;
    if (!state->leased) retired = std::move(state->session);
  }
  // Never call an external backend under the ownership mutex.
  retire_session(std::move(retired));
}
std::optional<SessionLease> SessionOwner::try_acquire() { return try_acquire_impl(nullptr); }
std::optional<SessionLease> SessionOwner::try_acquire(const CredentialToken& token) {
  return try_acquire_impl(&token);
}
std::optional<SessionLease> SessionOwner::try_acquire_impl(const CredentialToken* token) {
  if (!state_) return std::nullopt;
  std::unique_ptr<IDatabaseConnection> retired;
  std::optional<SessionLease> lease;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->accepting || !state_->session) return std::nullopt;
    if (state_->credential) {
      // Denials cannot be used to retire another context or newer generation.
      if (!token || !state_->credential->same_generation(*token)) return std::nullopt;
      // Lock order: owner -> authority. Authorities never call back into owners.
      if (!state_->credential->is_current()) {
        state_->accepting = false;
        if (!state_->leased) retired = std::move(state_->session);
      } else if (!state_->leased) {
        state_->leased = true; lease = SessionLease{state_};
      }
    } else if (!token && !state_->leased) {
      state_->leased = true; lease = SessionLease{state_};
    }
  }
  retire_session(std::move(retired));
  return lease;
}

SessionLease::SessionLease(std::shared_ptr<detail::SessionOwnershipState> state) noexcept
    : state_(std::move(state)) {}
SessionLease::SessionLease(SessionLease&& other) noexcept
    : state_(std::move(other.state_)) {}
SessionLease& SessionLease::operator=(SessionLease&& other) noexcept {
  if (this != &other) { retire(); state_ = std::move(other.state_); }
  return *this;
}
SessionLease::~SessionLease() { retire(); }
IDatabaseConnection* SessionLease::session() const noexcept {
  // Only this move-only borrower can remove the session while leased. Owner
  // destruction changes admission, not the active borrower's physical session.
  return state_ ? state_->session.get() : nullptr;
}
void SessionLease::retire() noexcept {
  auto state = std::move(state_);
  if (!state) return;
  std::unique_ptr<IDatabaseConnection> retired;
  {
    std::lock_guard lock(state->mutex);
    state->accepting = false;
    state->leased = false;
    retired = std::move(state->session);
  }
  retire_session(std::move(retired));
}
} // namespace rs::core::database
