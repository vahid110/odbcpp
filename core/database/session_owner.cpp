#include "session_owner.h"
#include <mutex>
#include <stdexcept>
#include <utility>

namespace rs::core::database {
namespace detail {
struct SessionOwnershipState {
  explicit SessionOwnershipState(std::unique_ptr<IDatabaseConnection> value)
      : session(std::move(value)) {}
  std::mutex mutex;
  std::unique_ptr<IDatabaseConnection> session;
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
    : state_(std::make_shared<detail::SessionOwnershipState>(std::move(session))) {
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
std::optional<SessionLease> SessionOwner::try_acquire() {
  if (!state_) return std::nullopt;
  std::lock_guard lock(state_->mutex);
  if (!state_->accepting || state_->leased || !state_->session) return std::nullopt;
  state_->leased = true;
  return SessionLease{state_};
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
