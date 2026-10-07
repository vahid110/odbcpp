#include "odbcpp/session/session_owner.h"
#include "odbcpp/session/credential_context.h"
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace rs::core::database {
namespace detail {
struct SessionCacheGeneration {};
struct SessionOwnershipState {
  using NowFactory = rs::util::Deadline(*)() noexcept;
  using CacheFactory = std::shared_ptr<const SessionCacheGeneration>(*)();
  explicit SessionOwnershipState(std::unique_ptr<IDatabaseConnection> value, std::optional<CredentialToken> token,
      CacheFactory factory, std::optional<SessionReusePolicy> policy, NowFactory clock)
      : session(std::move(value)), credential(std::move(token)), cache_factory(factory), reuse_policy(policy), now(clock), idle_since(clock()) {
    if (policy && (policy->retire_at == rs::util::Deadline::max() ||
        policy->max_idle <= rs::util::Clock::duration::zero()))
      throw std::invalid_argument("Session reuse policy requires finite lifetime and positive idle limit");
  }
  std::mutex mutex;
  std::unique_ptr<IDatabaseConnection> session;
  const std::optional<CredentialToken> credential;
  std::unique_ptr<CredentialContext> authentication;
  const CacheFactory cache_factory;
  std::shared_ptr<const SessionCacheGeneration> cache_generation;
  const std::optional<SessionReusePolicy> reuse_policy;
  const NowFactory now; // Local noexcept clock only; private fixtures must not reenter.
  rs::util::Deadline idle_since;
  bool lifetime_expired(rs::util::Deadline sample) const noexcept {
    return reuse_policy && sample >= reuse_policy->retire_at;
  }
  bool idle_expired(rs::util::Deadline sample) const noexcept {
    // Subtract only real monotonic samples, never user-supplied extrema/durations.
    return reuse_policy && !leased && sample >= idle_since && sample - idle_since >= reuse_policy->max_idle;
  }
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
SessionOwner::SessionOwner(std::unique_ptr<IDatabaseConnection> session, CredentialToken token, SessionReusePolicy policy)
    : SessionOwner(std::move(session), std::optional{std::move(token)}, &make_cache_generation, policy) {}
SessionOwner::SessionOwner(std::unique_ptr<IDatabaseConnection> session, std::optional<CredentialToken> token,
    CacheGenerationFactory factory, std::optional<SessionReusePolicy> policy, NowFactory clock)
    try : state_(std::make_shared<detail::SessionOwnershipState>(std::move(session), std::move(token), factory, policy, clock)) {
  if (!state_->session) throw std::invalid_argument("SessionOwner requires a physical session");
} catch (...) {
  retire_session(std::move(session));
  throw;
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
    if (state->authentication) state->authentication->revoke();
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
      if (!state_->credential->is_current() || state_->lifetime_expired(state_->now()) ||
          state_->idle_expired(state_->now())) {
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
template<class T, class Request>
BackendResult<T> invoke_borrowed(SessionLease& lease, IDatabaseConnection* physical,
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
  return invoke_borrowed<QueryResult>(*this, physical_session(), BackendOperation::ExecuteDirect,
      [&](IDatabaseConnection& physical) { return physical.execute_query(sql, deadline); });
}
BackendResult<QueryResult> SessionLease::execute_prepared(std::string_view sql,
    std::span<const QueryParameter> params, rs::util::Deadline deadline) {
  invalidate_cache();
  return invoke_borrowed<QueryResult>(*this, physical_session(), BackendOperation::ExecutePrepared,
      [&](IDatabaseConnection& physical) { return physical.execute_prepared(sql, params, deadline); });
}
namespace {
SessionSnapshot passive_outcome(bool connected, SessionState state) {
  return {state, !connected ? SessionDisposition::Retire :
      state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired};
}
BackendError inconsistent_observation(BackendOperation operation) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError), "Inconsistent passive session state"};
  error.operation = operation;
  error.session_state = SessionState::Unknown;
  return error;
}
}
BackendResult<SessionObservation> SessionLease::inspect() {
  return invoke_borrowed<SessionObservation>(*this, physical_session(), BackendOperation::InspectSession,
      [](IDatabaseConnection& physical) -> BackendResult<SessionObservation> {
        SessionObservation value;
        value.connected = physical.is_connected(); value.state = physical.session_state();
        if (value.connected == (value.state == SessionState::Disconnected))
          return inconsistent_observation(BackendOperation::InspectSession);
        value.server_version = physical.server_version();
        if (auto* facet = physical.transaction_session()) value.transactions = facet->transaction_capabilities();
        value.has_statement_description_facet = physical.statement_description() != nullptr;
        value.has_catalog_query_facet = physical.catalog_queries() != nullptr;
        value.has_catalog_execution_facet = physical.catalog_execution() != nullptr;
        value.has_health_facet = physical.session_health() != nullptr;
        value.has_reset_facet = physical.session_reset() != nullptr;
        const auto outcome = passive_outcome(value.connected, value.state);
        return BackendResult<SessionObservation>{std::move(value), outcome};
      });
}
BackendResult<void> SessionLease::check_health(rs::util::Deadline deadline) {
  invalidate_cache();
  return invoke_borrowed<void>(*this, physical_session(), BackendOperation::CheckHealth,
      [&](IDatabaseConnection& physical) -> BackendResult<void> {
        if (auto* facet = physical.session_health()) return facet->check_health(deadline);
        return local_backend_error(LocalFailure::Unsupported, "Session health facet unavailable",
            BackendOperation::CheckHealth, physical.session_state());
      });
}
BackendResult<std::string> SessionLease::catalog_query(const CatalogRequest& request) {
  return invoke_borrowed<std::string>(*this, physical_session(), BackendOperation::BuildCatalogQuery,
      [&](IDatabaseConnection& physical) -> BackendResult<std::string> {
        const auto connected = physical.is_connected(); const auto state = physical.session_state();
        if (connected == (state == SessionState::Disconnected))
          return inconsistent_observation(BackendOperation::BuildCatalogQuery);
        const auto outcome = passive_outcome(connected, state);
        if (!connected) {
          BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::NotConnected), "Session is disconnected"};
          error.operation = BackendOperation::BuildCatalogQuery; error.session_state = state; return error;
        }
        const auto* facet = physical.catalog_queries();
        if (!facet) return local_backend_error(LocalFailure::Unsupported, "Session catalog facet unavailable",
            BackendOperation::BuildCatalogQuery, state);
        auto query = facet->catalog_query(request);
        if (!query) {
          BackendError error{query.error(), query.error_message()};
          error.operation = BackendOperation::BuildCatalogQuery;
          error.session_state = state; error.disposition = outcome.disposition;
          return error;
        }
        return BackendResult<std::string>{std::move(*query), outcome};
      });
}
BackendResult<bool> SessionLease::selects_catalog_request(const CatalogRequest& request) {
  return invoke_borrowed<bool>(*this, physical_session(), BackendOperation::InspectSession,
      [&](IDatabaseConnection& physical) -> BackendResult<bool> {
        const auto connected = physical.is_connected();
        const auto state = physical.session_state();
        if (connected == (state == SessionState::Disconnected))
          return inconsistent_observation(BackendOperation::InspectSession);
        auto* facet = physical.catalog_execution();
        return BackendResult<bool>{facet && facet->selects_catalog_request(request),
                                   passive_outcome(connected, state)};
      });
}
BackendResult<QueryResult> SessionLease::execute_catalog(const CatalogRequest& request,
    rs::util::Deadline deadline) {
  invalidate_cache();
  return invoke_borrowed<QueryResult>(*this, physical_session(), BackendOperation::ExecuteCatalog,
      [&](IDatabaseConnection& physical) -> BackendResult<QueryResult> {
        if (auto* facet = physical.catalog_execution()) return facet->execute_catalog(request, deadline);
        return local_backend_error(LocalFailure::Unsupported, "Session catalog execution facet unavailable",
            BackendOperation::ExecuteCatalog, physical.session_state());
      });
}
BackendResult<void> SessionLease::transaction(TransactionAction action, rs::util::Deadline deadline) {
  invalidate_cache();
  return invoke_borrowed<void>(*this, physical_session(), BackendOperation::Transaction,
      [&](IDatabaseConnection& physical) -> BackendResult<void> {
        if (auto* facet = physical.transaction_session()) return facet->transaction(action, deadline);
        return local_backend_error(LocalFailure::Unsupported, "Session transaction facet unavailable",
            BackendOperation::Transaction, physical.session_state());
      });
}
BackendResult<void> SessionLease::set_transaction_isolation(TransactionIsolation level, rs::util::Deadline deadline) {
  invalidate_cache();
  return invoke_borrowed<void>(*this, physical_session(), BackendOperation::SetTransactionIsolation,
      [&](IDatabaseConnection& physical) -> BackendResult<void> {
        if (auto* facet = physical.transaction_session()) return facet->set_transaction_isolation(level, deadline);
        return local_backend_error(LocalFailure::Unsupported, "Session transaction facet unavailable",
            BackendOperation::SetTransactionIsolation, physical.session_state());
      });
}
BackendResult<QueryResult> SessionLease::describe_statement(std::string_view sql,
    std::span<const QueryParameterType> types, rs::util::Deadline deadline) {
  invalidate_cache();
  return invoke_borrowed<QueryResult>(*this, physical_session(), BackendOperation::Describe,
      [&](IDatabaseConnection& physical) -> BackendResult<QueryResult> {
        if (auto* facet = physical.statement_description()) return facet->describe_statement(sql, types, deadline);
        return local_backend_error(LocalFailure::Unsupported, "Session description facet unavailable",
            BackendOperation::Describe, physical.session_state());
      });
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
  if (!state->credential->is_current() || state->lifetime_expired(state->now())) {
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
    if (!state_->credential->is_current() || state_->lifetime_expired(state_->now())) {
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
  if (!state_->credential->is_current() || state_->lifetime_expired(state_->now())) {
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
  if (state_->lifetime_expired(state_->now())) state_->accepting = false;
}
} // namespace rs::core::database

namespace rs::core::database {
BackendResult<void> SessionLease::return_reusable(rs::util::Deadline deadline) {
  struct RetirementGuard {
    SessionLease& lease;
    bool completed{false};
    ~RetirementGuard() { if (!completed) lease.retire(); }
  } guard{*this};
  invalidate_cache();
  const auto fail = [this](rs::util::DbErrorCode code, const char* message) -> BackendResult<void> {
    retire();
    BackendError error{rs::util::make_error_code(code), message};
    error.operation = BackendOperation::ResetSession;
    error.session_state = SessionState::Disconnected;
    return error;
  };
  if (!state_) return fail(rs::util::DbErrorCode::NotConnected, "No active session lease");
  auto state = state_;
  bool eligible{}, lifetime_expired{};
  IDatabaseConnection* physical{};
  {
    std::lock_guard lock(state->mutex);
    lifetime_expired = state->lifetime_expired(state->now());
    eligible = state->accepting && state->leased && state->session && state->credential &&
        state->credential->is_current() && !lifetime_expired;
    physical = state->session.get();
  }
  // Never retire or invoke the backend while holding an ownership lock.
  if (!eligible) return fail(lifetime_expired ? rs::util::DbErrorCode::Timeout : rs::util::DbErrorCode::AuthenticationFailed,
      "Session return admission unavailable");
  auto reset = reset_session(deadline);
  if (!reset) return reset;
  bool expired{};
  {
    std::lock_guard lock(state->mutex);
    const auto now = state->now();
    expired = rs::util::Clock::now() >= deadline || state->lifetime_expired(now);
    eligible = !expired && state_ == state && state->accepting && state->leased &&
        state->session.get() == physical && state->credential &&
        state->credential->is_current();
    if (eligible) {
      // Publication linearizes here. A concurrent rotation after this point is
      // detected on the next checkout; it never rebinds the physical session.
      state->cache_generation.reset();
      state->idle_since = now;
      state->leased = false;
      state_.reset();
    }
  }
  if (!eligible) return fail(expired ? rs::util::DbErrorCode::Timeout : rs::util::DbErrorCode::AuthenticationFailed,
      "Session return admission expired or closed");
  guard.completed = true;
  return reset;
}
} // namespace rs::core::database

namespace rs::core::database {
BackendResult<SessionLease> SessionOwner::acquire_healthy(const CredentialToken& token, rs::util::Deadline deadline) {
  auto lease = try_acquire(token);
  const auto fail = [&lease](rs::util::DbErrorCode code, const char* message) -> BackendResult<SessionLease> {
    if (lease) lease->retire();
    BackendError error{rs::util::make_error_code(code), message};
    error.operation = BackendOperation::CheckHealth;
    error.session_state = SessionState::Disconnected;
    return error;
  };
  if (!lease) return fail(rs::util::DbErrorCode::AuthenticationFailed, "Session health admission unavailable");
  // Local RAII lease retires on every failure or exception, including diagnostic
  // allocation. Success transfers it exactly once; no physical pointer escapes.
  auto state = lease->state_;
  auto* physical = lease->physical_session();
  const auto admitted = [&] {
    std::lock_guard lock(state->mutex);
    const bool current = state->credential && state->credential->is_current();
    const bool expired = rs::util::Clock::now() >= deadline || state->lifetime_expired(state->now());
    return std::pair{!expired && state->accepting && state->leased && state->session.get() == physical && current, expired};
  };
  try {
    lease->invalidate_cache();
    const auto [initial_eligible, initially_expired] = admitted();
    if (!initial_eligible) return fail(initially_expired ? rs::util::DbErrorCode::Timeout : rs::util::DbErrorCode::AuthenticationFailed,
        "Session health admission expired or closed");
    auto* health = physical->session_health();
    if (!health) return fail(rs::util::DbErrorCode::UnsupportedFeature, "Session health facet unavailable");
    auto result = health->check_health(deadline);
    if (!result) {
      auto error = std::move(result.backend_error());
      lease->retire();
      error.operation = BackendOperation::CheckHealth;
      error.session_state = SessionState::Disconnected;
      error.disposition = SessionDisposition::Retire;
      error.retry_safe.reset();
      return error;
    }
    if (result.session_snapshot() != SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable} ||
        !physical->is_connected() || physical->session_state() != SessionState::Idle)
      return fail(rs::util::DbErrorCode::ProtocolError, "Session health outcome is inconsistent");
    bool eligible{}, expired{};
    {
      std::lock_guard lock(state->mutex);
      const bool current = state->credential && state->credential->is_current();
      expired = rs::util::Clock::now() >= deadline || state->lifetime_expired(state->now());
      eligible = !expired && state->accepting && state->leased && state->session.get() == physical && current;
      state->cache_generation.reset();
    }
    if (!eligible) return fail(expired ? rs::util::DbErrorCode::Timeout : rs::util::DbErrorCode::AuthenticationFailed,
        "Session health admission expired or closed");
    return BackendResult<SessionLease>{std::move(*lease), result.session_snapshot()};
  } catch (const std::bad_alloc&) {
    return fail(rs::util::DbErrorCode::AllocationFailure, "");
  } catch (...) {
    return fail(rs::util::DbErrorCode::ProtocolError, "Session health backend threw");
  }
}
} // namespace rs::core::database

namespace rs::core::database {
BackendResult<SessionOwner> SessionOwner::connect_authenticated(std::unique_ptr<IDatabaseConnection> physical,
    const ConnectionSettings& settings, SessionReusePolicy policy, std::optional<rs::util::Deadline> credential_expiry) {
  return connect_authenticated_impl(std::move(physical), settings, policy, credential_expiry,
      +[]() -> std::unique_ptr<CredentialContext> { return std::make_unique<CredentialContext>(); });
}
BackendResult<SessionOwner> SessionOwner::connect_authenticated_impl(std::unique_ptr<IDatabaseConnection> physical,
    const ConnectionSettings& settings, SessionReusePolicy policy, std::optional<rs::util::Deadline> credential_expiry,
    AuthorityFactory create_authority) {
  struct RetirementGuard {
    std::unique_ptr<IDatabaseConnection>& physical;
    ~RetirementGuard() { retire_session(std::move(physical)); }
  } guard{physical};
  const auto fail = [](rs::util::DbErrorCode code, const char* message) -> BackendResult<SessionOwner> {
    BackendError error{rs::util::make_error_code(code), message};
    error.operation = BackendOperation::Connect;
    error.session_state = SessionState::Disconnected;
    return error;
  };
  const auto deadline = rs::util::make_deadline(settings.timeout);
  try {
    if (!physical) return fail(rs::util::DbErrorCode::InvalidParameter, "No provider session");
    if (policy.retire_at == rs::util::Deadline::max() || policy.max_idle <= rs::util::Clock::duration::zero())
      return fail(rs::util::DbErrorCode::InvalidParameter, "Invalid authenticated session reuse policy");
    const auto expired = [&] {
      const auto now = rs::util::Clock::now();
      return now >= deadline || now >= policy.retire_at || (credential_expiry && now >= *credential_expiry);
    };
    if (expired()) return fail(rs::util::DbErrorCode::Timeout, "Authenticated session admission expired");
    if (physical->is_connected() || physical->session_state() != SessionState::Disconnected)
      return fail(rs::util::DbErrorCode::InvalidParameter, "Provider session must be fresh and disconnected");
    auto connected = physical->connect(settings);
    if (!connected) {
      auto error = std::move(connected.backend_error());
      error.operation = BackendOperation::Connect;
      error.session_state = SessionState::Disconnected;
      error.disposition = SessionDisposition::Retire;
      error.retry_safe.reset();
      return error;
    }
    if (connected.session_snapshot() != SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable} ||
        !physical->is_connected() || physical->session_state() != SessionState::Idle)
      return fail(rs::util::DbErrorCode::ProtocolError, "Authenticated session connection outcome is inconsistent");
    if (expired()) return fail(rs::util::DbErrorCode::Timeout, "Authenticated session connection exceeded admission lifetime");
    auto authority = create_authority();
    if (!authority) return fail(rs::util::DbErrorCode::AllocationFailure, "");
    auto token = authority->publish_authenticated(credential_expiry);
    // Allocation/publication may itself cross a bound. Never expose that owner.
    if (expired() || !token.is_current()) return fail(rs::util::DbErrorCode::Timeout, "Authenticated session publication expired");
    SessionOwner owner{std::move(physical), token, policy};
    owner.state_->authentication = std::move(authority);
    if (expired()) return fail(rs::util::DbErrorCode::Timeout, "Authenticated session construction expired");
    return BackendResult<SessionOwner>{std::move(owner), connected.session_snapshot()};
  } catch (const std::bad_alloc&) {
    return fail(rs::util::DbErrorCode::AllocationFailure, "");
  } catch (...) {
    return fail(rs::util::DbErrorCode::ProtocolError, "Authenticated session backend threw");
  }
}
BackendResult<SessionLease> SessionOwner::acquire_healthy(rs::util::Deadline deadline) {
  std::optional<CredentialToken> token;
  if (state_) {
    std::lock_guard lock(state_->mutex);
    if (state_->authentication && state_->credential) token = state_->credential;
  }
  if (token) return acquire_healthy(*token, deadline);
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::AuthenticationFailed), "Managed session authentication unavailable"};
  error.operation = BackendOperation::CheckHealth;
  error.session_state = SessionState::Disconnected;
  return error;
}
void SessionOwner::revoke_credentials() noexcept {
  if (!state_) return;
  std::unique_ptr<IDatabaseConnection> retired;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->authentication) return;
    state_->authentication->revoke();
    state_->accepting = false;
    state_->cache_generation.reset();
    if (!state_->leased) retired = std::move(state_->session);
  }
  retire_session(std::move(retired));
}
} // namespace rs::core::database
