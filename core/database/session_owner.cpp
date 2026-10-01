#include "session_owner.h"
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace rs::core::database {
namespace detail {
struct SessionCacheGeneration {};
struct SessionOwnershipState {
  using CacheFactory = std::shared_ptr<const SessionCacheGeneration>(*)();
  explicit SessionOwnershipState(std::unique_ptr<IDatabaseConnection> value, std::optional<CredentialToken> token,
      CacheFactory factory)
      : session(std::move(value)), credential(std::move(token)), cache_factory(factory) {}
  std::mutex mutex;
  std::unique_ptr<IDatabaseConnection> session;
  const std::optional<CredentialToken> credential;
  const CacheFactory cache_factory;
  std::shared_ptr<const SessionCacheGeneration> cache_generation;
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
SessionOwner::SessionOwner(std::unique_ptr<IDatabaseConnection> session, std::optional<CredentialToken> token,
    CacheGenerationFactory factory)
    : state_(std::make_shared<detail::SessionOwnershipState>(std::move(session), std::move(token), factory)) {
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
    state->cache_generation.reset();
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
        state_->cache_generation.reset();
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
IDatabaseConnection* SessionLease::physical_session() const noexcept {
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
    state->cache_generation.reset();
    state->leased = false;
    retired = std::move(state->session);
  }
  retire_session(std::move(retired));
}
} // namespace rs::core::database

namespace rs::core::database {
BackendResult<void> SessionLease::reset_session(rs::util::Deadline deadline) {
  struct RetirementGuard {
    SessionLease& lease;
    bool completed{false};
    ~RetirementGuard() { if (!completed) lease.retire(); }
  } guard{*this};
  invalidate_cache();
  const auto fail = [this](rs::util::DbErrorCode code, const char* message) -> BackendResult<void> {
    // Retire before constructing the diagnostic: even allocation failure cannot
    // leave a failed reset borrow accessible. Backend callbacks run unlocked.
    retire();
    BackendError error{rs::util::make_error_code(code), message};
    error.operation = BackendOperation::ResetSession;
    error.session_state = SessionState::Disconnected;
    return error;
  };
  try {
    auto* physical = physical_session();
    if (!physical) return fail(rs::util::DbErrorCode::NotConnected, "No active session lease");
    if (rs::util::Clock::now() >= deadline)
      return fail(rs::util::DbErrorCode::Timeout, "Session reset deadline expired");
    auto* reset = physical->session_reset();
    if (!reset || reset->reset_profile() != SessionResetProfile::SameAuthenticatedServerSession)
      return fail(rs::util::DbErrorCode::UnsupportedFeature, "Session reset profile unavailable");
    auto result = reset->reset_session(deadline);
    if (!result) {
      auto error = std::move(result.backend_error());
      retire();
      error.operation = BackendOperation::ResetSession;
      error.session_state = SessionState::Disconnected;
      error.disposition = SessionDisposition::Retire;
      error.retry_safe.reset();
      return error;
    }
    if (result.session_snapshot() != SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable} ||
        !physical->is_connected() || physical->session_state() != SessionState::Idle)
      return fail(rs::util::DbErrorCode::ProtocolError, "Session reset outcome is inconsistent");
    if (rs::util::Clock::now() >= deadline)
      return fail(rs::util::DbErrorCode::Timeout, "Session reset exceeded deadline");
    guard.completed = true;
    return result;
  } catch (const std::bad_alloc&) {
    return fail(rs::util::DbErrorCode::AllocationFailure, "");
  } catch (...) {
    return fail(rs::util::DbErrorCode::ProtocolError, "Session reset backend threw");
  }
}
} // namespace rs::core::database

namespace rs::core::database {
namespace {
template<class Request>
BackendResult<QueryResult> execute_borrowed(SessionLease& lease, IDatabaseConnection* physical,
    BackendOperation operation, Request&& request) {
  if (!physical) {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::NotConnected), "No active session lease"};
    error.operation = operation;
    error.session_state = SessionState::Disconnected;
    return error;
  }
  try {
    auto result = request(*physical);
    // The result already owns its storage, including failure/native details.
    // No backend callback runs under an ownership or credential mutex.
    if (result.session_snapshot().disposition == SessionDisposition::Retire) lease.retire();
    return result;
  } catch (...) {
    // Preserve the original exception while preventing ambiguous session reuse.
    lease.retire();
    throw;
  }
}
}
BackendResult<QueryResult> SessionLease::execute_query(std::string_view sql, rs::util::Deadline deadline) {
  invalidate_cache();
  return execute_borrowed(*this, physical_session(), BackendOperation::ExecuteDirect,
      [&](IDatabaseConnection& physical) { return physical.execute_query(sql, deadline); });
}
BackendResult<QueryResult> SessionLease::execute_prepared(std::string_view sql,
    std::span<const QueryParameter> params, rs::util::Deadline deadline) {
  invalidate_cache();
  return execute_borrowed(*this, physical_session(), BackendOperation::ExecutePrepared,
      [&](IDatabaseConnection& physical) { return physical.execute_prepared(sql, params, deadline); });
}
} // namespace rs::core::database

namespace rs::core::database {
namespace {
bool cache_current(const std::shared_ptr<detail::SessionOwnershipState>& state,
    const std::shared_ptr<const detail::SessionCacheGeneration>& generation) noexcept {
  if (!state || !generation) return false;
  std::lock_guard lock(state->mutex);
  // Fixed lock order: owner -> credential authority; no backend calls here.
  if (!state->accepting || !state->leased || !state->session ||
      state->cache_generation != generation || !state->credential) return false;
  if (!state->credential->is_current()) {
    state->accepting = false;
    state->cache_generation.reset();
    return false;
  }
  return true;
}
}
SessionCacheToken::SessionCacheToken(std::weak_ptr<detail::SessionOwnershipState> state,
    std::weak_ptr<const detail::SessionCacheGeneration> generation) noexcept
    : state_(std::move(state)), generation_(std::move(generation)) {}
bool SessionCacheToken::is_current() const noexcept {
  return cache_current(state_.lock(), generation_.lock());
}
std::shared_ptr<const detail::SessionCacheGeneration> SessionOwner::make_cache_generation() {
  return std::make_shared<const detail::SessionCacheGeneration>();
}
std::optional<SessionCacheToken> SessionLease::cache_token() {
  if (!state_) return std::nullopt;
  IDatabaseConnection* physical{};
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->accepting || !state_->leased || !state_->session || !state_->credential) return std::nullopt;
    if (!state_->credential->is_current()) {
      state_->accepting = false; state_->cache_generation.reset(); return std::nullopt;
    }
    physical = state_->session.get();
  }
  // Passive backend calls must not execute under the ownership/authority lock.
  // Owner close cannot remove this leased session; same-lease operations remain
  // caller-serialized. This is local Idle scope eligibility, not a health probe.
  bool idle{};
  try { idle = physical->is_connected() && physical->session_state() == SessionState::Idle; }
  catch (...) { invalidate_cache(); return std::nullopt; }
  std::lock_guard lock(state_->mutex);
  if (!idle || !state_->accepting || !state_->leased || state_->session.get() != physical) {
    state_->cache_generation.reset(); return std::nullopt;
  }
  if (!state_->credential->is_current()) {
    state_->accepting = false; state_->cache_generation.reset(); return std::nullopt;
  }
  if (!state_->cache_generation) {
    // No old scope exists here. Allocation/null-factory failure leaves it absent.
    // Production factory only allocates identity; private test factories must
    // never reenter or call backend/owner/credential operations under this lock.
    state_->cache_generation = state_->cache_factory();
    if (!state_->cache_generation) throw std::bad_alloc{};
  }
  return SessionCacheToken{state_, state_->cache_generation};
}
bool SessionLease::accepts_cache(const SessionCacheToken& token) const noexcept {
  if (!state_ || state_ != token.state_.lock()) return false;
  return cache_current(state_, token.generation_.lock());
}
void SessionLease::invalidate_cache() noexcept {
  if (!state_) return;
  std::lock_guard lock(state_->mutex);
  state_->cache_generation.reset();
}
} // namespace rs::core::database
