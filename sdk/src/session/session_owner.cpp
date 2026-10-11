#include "odbcpp/session/session_owner.h"
#include "odbcpp/session/credential_context.h"
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>
#include "odbcpp/util/utf8.h"

namespace rs::core::database {
namespace detail {
struct SessionCacheGeneration {};
struct SessionBorrowIdentity {};
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
        auto identity = std::make_shared<const detail::SessionBorrowIdentity>();
        state_->leased = true; lease = SessionLease{state_, std::move(identity)};
      }
    } else if (!token && !state_->leased) {
      auto identity = std::make_shared<const detail::SessionBorrowIdentity>();
      state_->leased = true; lease = SessionLease{state_, std::move(identity)};
    }
  }
  retire_session(std::move(retired));
  return lease;
}

SessionLease::SessionLease(std::shared_ptr<detail::SessionOwnershipState> state,
    std::shared_ptr<const detail::SessionBorrowIdentity> borrow) noexcept
    : state_(std::move(state)), borrow_(std::move(borrow)) {}
SessionLease::SessionLease(SessionLease&& other) noexcept
    : state_(std::move(other.state_)), borrow_(std::move(other.borrow_)) {}
SessionLease& SessionLease::operator=(SessionLease&& other) noexcept {
  if (this != &other) { retire(); state_ = std::move(other.state_); borrow_ = std::move(other.borrow_); }
  return *this;
}
SessionLease::~SessionLease() { retire(); }
IDatabaseConnection* SessionLease::physical_session() const noexcept {
  // Only this move-only borrower can remove the session while leased. Owner
  // destruction changes admission, not the active borrower's physical session.
  return state_ ? state_->session.get() : nullptr;
}
bool SessionLease::supports_server_cancellation() const noexcept {
  const auto* facet = dynamic_cast<const IBackendCancellation*>(physical_session());
  return facet && facet->supports_server_cancellation();
}
bool SessionLease::cancellation_request_eligible(std::string_view sql) const noexcept {
  const auto* facet = dynamic_cast<const IBackendCancellation*>(physical_session());
  return facet && facet->cancellation_request_eligible(sql);
}
std::shared_ptr<SessionCancellation> SessionLease::arm_cancellation(
    std::uint64_t generation, rs::util::Deadline original) {
  auto* facet = dynamic_cast<IBackendCancellation*>(physical_session());
  return facet ? facet->arm_cancellation(generation, original) : nullptr;
}
bool SessionLease::continue_cancellation(const std::shared_ptr<SessionCancellation>& endpoint) noexcept {
  auto* facet = dynamic_cast<IBackendCancellation*>(physical_session());
  return facet && facet->continue_cancellation(endpoint);
}
CancellationOutcome SessionLease::finish_cancellation(const std::shared_ptr<SessionCancellation>& endpoint) noexcept {
  auto* facet = dynamic_cast<IBackendCancellation*>(physical_session());
  return facet ? facet->finish_cancellation(endpoint) : endpoint ? endpoint->seal() : CancellationOutcome{};
}
void SessionLease::retire() noexcept {
  borrow_.reset();
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
        value.has_prepared_result_sequence = physical.statement_description() &&
            physical.statement_description()->supports_prepared_result_sequence();
        value.has_single_statement_result_shape = physical.statement_description() &&
            physical.statement_description()->supports_single_statement_result_shape();
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
PreparedParameterDescription::PreparedParameterDescription(PreparedParameterDescription&& other) noexcept
    : sql_(std::move(other.sql_)), types_(std::move(other.types_)),
      description_(std::move(other.description_)), owner_(std::move(other.owner_)),
      borrow_(std::move(other.borrow_)), deadline_(other.deadline_), input_(other.input_),
      limits_(other.limits_), set_count_(other.set_count_), snapshot_(other.snapshot_),
      consumed_(std::exchange(other.consumed_, true)) {}
PreparedParameterDescription& PreparedParameterDescription::operator=(PreparedParameterDescription&& other) noexcept {
  if (this != &other) {
    sql_ = std::move(other.sql_); types_ = std::move(other.types_);
    description_ = std::move(other.description_); owner_ = std::move(other.owner_);
    borrow_ = std::move(other.borrow_); deadline_ = other.deadline_;
    input_ = other.input_; limits_ = other.limits_; set_count_ = other.set_count_; snapshot_ = other.snapshot_;
    consumed_ = std::exchange(other.consumed_, true);
  }
  return *this;
}
bool SessionLease::accepts_parameter_description(const PreparedParameterDescription& authority) const noexcept {
  return !authority.consumed_ && borrow_ && authority.owner_.lock() == state_ &&
      authority.borrow_.lock() == borrow_ && physical_session() &&
      authority.set_count_ > 1 && !authority.types_.empty() &&
      (authority.description_.described_result_shape == DescribedResultShape::NoResultSet ||
       authority.description_.described_result_shape == DescribedResultShape::ResultSet);
}
BackendResult<PreparedParameterDescription> SessionLease::describe_parameter_input(std::string_view sql,
    std::vector<QueryParameterType> types, std::size_t count, const InputLimits& input,
    const ResultLimits& limits, rs::util::Deadline deadline, bool allow_results) {
  invalidate_cache();
  return invoke_borrowed<PreparedParameterDescription>(*this, physical_session(), BackendOperation::Describe,
      [&](IDatabaseConnection& physical) -> BackendResult<PreparedParameterDescription> {
        const auto reject = [&](LocalFailure failure, const char* text) -> BackendResult<PreparedParameterDescription> {
          return local_backend_error(failure, text, BackendOperation::Describe, physical.session_state());
        };
        if (count <= 1 || count > limits.max_results || types.empty() ||
            types.size() > input.max_parameters || sql.size() > input.max_sql_bytes)
          return reject(LocalFailure::InvalidInput, "Deferred command description exceeds configured limit");
        if (rs::util::Clock::now() >= deadline)
          return BackendResult<PreparedParameterDescription>{rs::util::DbErrorCode::Timeout, {}};
        auto* facet = physical.statement_description();
        if (!facet || !facet->supports_single_statement_result_shape())
          return reject(LocalFailure::Unsupported, "Authoritative deferred command description unavailable");
        auto description = facet->describe_statement(sql, types, deadline);
        if (!description) return std::move(description.backend_error());
        const auto snapshot = description.session_snapshot();
        const auto& value = *description;
        if (!value.described_result_shape || value.execution_result_shape || value.error ||
            value.statement_kind || value.affected_rows || !value.rows.empty() ||
            !value.additional_results.empty() || !value.cell_errors.empty() ||
            value.normalized_parameter_types.size() != types.size() ||
            value.normalized_parameter_types.size() > limits.max_metadata_entries ||
            snapshot.state == SessionState::Unknown || snapshot.state == SessionState::Disconnected ||
            snapshot.disposition == SessionDisposition::Retire)
          return BackendResult<PreparedParameterDescription>{rs::util::DbErrorCode::ProtocolError, {}};
        if (*value.described_result_shape == DescribedResultShape::ResultSet &&
            (!allow_results || !facet->supports_prepared_result_sequence()))
          return reject(LocalFailure::Unsupported, "Deferred arrays do not return row results");
        if ((*value.described_result_shape != DescribedResultShape::NoResultSet &&
             *value.described_result_shape != DescribedResultShape::ResultSet) ||
            (*value.described_result_shape == DescribedResultShape::NoResultSet && !value.columns.empty()))
          return BackendResult<PreparedParameterDescription>{rs::util::DbErrorCode::ProtocolError, {}};
        // Validate the whole owning description before NEED_DATA. Sequence
        // start accounts it once; this preflight does not consume totals twice.
        if (value.columns.size() > limits.max_columns_per_description ||
            value.columns.size() > limits.max_metadata_entries ||
            value.normalized_parameter_types.size() > limits.max_metadata_entries - value.columns.size())
          return BackendResult<PreparedParameterDescription>{rs::util::DbErrorCode::ResourceLimit, {}};
        std::size_t names = 0;
        for (const auto& column : value.columns) {
          if (!column.normalized_type || column.name.find('\0') != std::string::npos ||
              !rs::util::utf8_code_point_count(column.name))
            return BackendResult<PreparedParameterDescription>{rs::util::DbErrorCode::ProtocolError, {}};
          if (column.name.size() > limits.max_column_name_bytes ||
              column.name.size() > limits.max_metadata_name_bytes - names)
            return BackendResult<PreparedParameterDescription>{rs::util::DbErrorCode::ResourceLimit, {}};
          names += column.name.size();
        }
        if (rs::util::Clock::now() >= deadline)
          return BackendResult<PreparedParameterDescription>{rs::util::DbErrorCode::Timeout, {}};
        return BackendResult<PreparedParameterDescription>{PreparedParameterDescription{
            std::string(sql), std::move(types), std::move(*description), state_, borrow_,
            deadline, input, limits, count, snapshot}, snapshot};
      });
}
BackendResult<PreparedCommandPlan> SessionLease::finalize_parameter_input(
    PreparedParameterDescription&& authority, std::vector<PreparedCommandSet> sets) {
  // Representation/borrow refusal is passive and cannot close a newer lease.
  const auto reject = [&](const char* text) -> BackendResult<PreparedCommandPlan> {
    return local_backend_error(LocalFailure::InvalidInput, text,
        BackendOperation::ExecutePrepared, SessionState::Unknown);
  };
  if (!accepts_parameter_description(authority)) return reject("Deferred command authority is not valid for this borrow");
  authority.consumed_ = true;
  if (sets.size() != authority.set_count_) return reject("Deferred command set count changed");
  std::size_t entries = 0, bytes = 0;
  bool proceeding = false;
  for (const auto& set : sets) {
    if (set.ignored) {
      if (!set.values.empty()) return reject("Ignored deferred set has values");
      continue;
    }
    proceeding = true;
    if (set.values.size() != authority.types_.size() ||
        set.values.size() > authority.input_.max_parameters - entries)
      return reject("Deferred command parameter count changed");
    entries += set.values.size();
    for (std::size_t i = 0; i < set.values.size(); ++i) {
      const auto& value = set.values[i];
      if (value.type != authority.types_[i]) return reject("Deferred command parameter type changed");
      if (value.value) {
        if (value.value->size() > authority.input_.max_parameter_bytes ||
            value.value->size() > authority.input_.max_parameter_total_bytes - bytes)
          return reject("Deferred command parameter bytes exceed limit");
        bytes += value.value->size();
      }
    }
  }
  if (!proceeding) return reject("Deferred command authority has no real sets");
  if (rs::util::Clock::now() >= authority.deadline_)
    return BackendResult<PreparedCommandPlan>{rs::util::DbErrorCode::Timeout, {}};
  // No describe/passive provider callback or transaction here. All storage is
  // owning; construction precedes any adapter BEGIN or publication.
  return BackendResult<PreparedCommandPlan>{PreparedCommandPlan{
      std::move(authority.sql_), std::move(sets), std::move(authority.description_),
      state_, borrow_, authority.deadline_, authority.limits_},
      authority.snapshot_};
}
bool SessionLease::accepts_command_description(const PreparedCommandDescription& authority) const noexcept {
  return accepts_parameter_description(authority) &&
      authority.description_.described_result_shape == DescribedResultShape::NoResultSet;
}
BackendResult<PreparedCommandDescription> SessionLease::describe_command_batch(std::string_view sql,
    std::vector<QueryParameterType> types, std::size_t count, const InputLimits& input,
    const ResultLimits& limits, rs::util::Deadline deadline) {
  return describe_parameter_input(sql, std::move(types), count, input, limits, deadline, false);
}
BackendResult<PreparedCommandPlan> SessionLease::finalize_command_batch(
    PreparedCommandDescription&& authority, std::vector<PreparedCommandSet> sets) {
  if (!accepts_command_description(authority))
    return local_backend_error(LocalFailure::InvalidInput, "Deferred command authority is not valid for this borrow",
        BackendOperation::ExecutePrepared, SessionState::Unknown);
  return finalize_parameter_input(std::move(authority), std::move(sets));
}
PreparedCommandPlan::PreparedCommandPlan(PreparedCommandPlan&& other) noexcept
    : sql_(std::move(other.sql_)), sets_(std::move(other.sets_)),
      description_(std::move(other.description_)), owner_(std::move(other.owner_)),
      borrow_(std::move(other.borrow_)), deadline_(other.deadline_), limits_(other.limits_),
      outcomes_(std::move(other.outcomes_)), consumed_(std::exchange(other.consumed_, true)) {}
PreparedCommandPlan& PreparedCommandPlan::operator=(PreparedCommandPlan&& other) noexcept {
  if (this != &other) {
    sql_ = std::move(other.sql_); sets_ = std::move(other.sets_);
    description_ = std::move(other.description_); owner_ = std::move(other.owner_);
    borrow_ = std::move(other.borrow_); deadline_ = other.deadline_; limits_ = other.limits_;
    outcomes_ = std::move(other.outcomes_); consumed_ = std::exchange(other.consumed_, true);
  }
  return *this;
}
BackendResult<PreparedCommandPlan> SessionLease::prepare_command_batch(std::string_view sql,
    std::vector<PreparedCommandSet> sets, const InputLimits& input,
    const ResultLimits& limits, rs::util::Deadline deadline) {
  return prepare_parameter_batch(sql, std::move(sets), input, limits, deadline, false);
}
BackendResult<PreparedCommandPlan> SessionLease::prepare_parameter_batch(std::string_view sql,
    std::vector<PreparedCommandSet> sets, const InputLimits& input,
    const ResultLimits& limits, rs::util::Deadline deadline, bool allow_results) {
  invalidate_cache();
  return invoke_borrowed<PreparedCommandPlan>(*this, physical_session(), BackendOperation::Describe,
      [&](IDatabaseConnection& physical) -> BackendResult<PreparedCommandPlan> {
        const auto reject = [&](LocalFailure reason, const char* text) -> BackendResult<PreparedCommandPlan> {
          return local_backend_error(reason, text, BackendOperation::Describe, physical.session_state());
        };
        if (sets.empty() || sets.size() > limits.max_results || sql.size() > input.max_sql_bytes)
          return reject(LocalFailure::InvalidInput, "Prepared batch exceeds configured count limit");
        std::size_t entries = 0, bytes = 0;
        const PreparedCommandSet* first = nullptr;
        for (const auto& set : sets) {
          if (set.ignored) {
            if (!set.values.empty()) return reject(LocalFailure::InvalidInput, "Ignored batch set has values");
            continue;
          }
          if (!first) first = &set;
          if (set.values.empty() || set.values.size() != first->values.size() ||
              set.values.size() > input.max_parameters - entries)
            return reject(LocalFailure::InvalidInput, "Prepared batch parameter count is invalid");
          entries += set.values.size();
          for (std::size_t i = 0; i < set.values.size(); ++i) {
            if (set.values[i].type != first->values[i].type)
              return reject(LocalFailure::InvalidInput, "Prepared batch parameter types differ");
            if (const auto& value = set.values[i].value; value) {
              if (value->size() > input.max_parameter_bytes ||
                  value->size() > input.max_parameter_total_bytes - bytes)
                return reject(LocalFailure::InvalidInput, "Prepared batch parameter bytes exceed limit");
              bytes += value->size();
            }
          }
        }
        if (rs::util::Clock::now() >= deadline)
          return BackendResult<PreparedCommandPlan>{rs::util::DbErrorCode::Timeout, {}};
        if (!first) return BackendResult<PreparedCommandPlan>{
            PreparedCommandPlan{std::string(sql), std::move(sets), QueryResult{}, state_, borrow_, deadline, limits},
            passive_outcome(physical.is_connected(), physical.session_state())};
        auto* facet = physical.statement_description();
        if (!facet || !facet->supports_single_statement_result_shape())
          return reject(LocalFailure::Unsupported, "Authoritative prepared-command description unavailable");
        if (rs::util::Clock::now() >= deadline)
          return BackendResult<PreparedCommandPlan>{rs::util::DbErrorCode::Timeout, {}};
        std::vector<QueryParameterType> types;
        types.reserve(first->values.size());
        for (const auto& value : first->values) types.push_back(value.type);
        auto description = facet->describe_statement(sql, types, deadline);
        if (!description) return std::move(description.backend_error());
        const auto snapshot = description.session_snapshot();
        const auto& value = *description;
        if (!value.described_result_shape || value.execution_result_shape || value.error || value.statement_kind || value.affected_rows != 0 || !value.rows.empty() ||
            !value.additional_results.empty() || !value.cell_errors.empty() ||
            value.normalized_parameter_types.size() != types.size() ||
            value.normalized_parameter_types.size() > limits.max_metadata_entries ||
            snapshot.state == SessionState::Unknown || snapshot.state == SessionState::Disconnected ||
            snapshot.disposition == SessionDisposition::Retire)
          return BackendResult<PreparedCommandPlan>{rs::util::DbErrorCode::ProtocolError, {}};
        if (*value.described_result_shape == DescribedResultShape::ResultSet &&
            (!allow_results || !facet->supports_prepared_result_sequence()))
          return reject(LocalFailure::Unsupported, "Parameter arrays do not return row results");
        if ((*value.described_result_shape != DescribedResultShape::NoResultSet &&
             *value.described_result_shape != DescribedResultShape::ResultSet) ||
            (*value.described_result_shape == DescribedResultShape::NoResultSet && !value.columns.empty()))
          return BackendResult<PreparedCommandPlan>{rs::util::DbErrorCode::ProtocolError, {}};
        if (rs::util::Clock::now() >= deadline)
          return BackendResult<PreparedCommandPlan>{rs::util::DbErrorCode::Timeout, {}};
        return BackendResult<PreparedCommandPlan>{PreparedCommandPlan{
            std::string(sql), std::move(sets), std::move(*description), state_, borrow_, deadline, limits}, snapshot};
      });
}

BackendResult<PreparedCommandBatchResult> SessionLease::execute_command_batch(PreparedCommandPlan&& plan) {
  // Refuse before passive callbacks or vector indexing. A returned/reacquired
  // owner is not the same exclusive borrow; moved-from/consumed representations
  // have no authority. This local refusal neither observes nor mutates session.
  if (plan.consumed_ || !borrow_ || plan.owner_.lock() != state_ ||
      plan.borrow_.lock() != borrow_ || !physical_session() ||
      plan.sets_.empty() || plan.outcomes_.size() != plan.sets_.size()) {
    return local_backend_error(LocalFailure::InvalidInput, "Prepared batch is not valid for this borrow",
        BackendOperation::ExecutePrepared, SessionState::Unknown);
  }
  plan.consumed_ = true;
  PreparedCommandBatchResult batch{std::move(plan.outcomes_), 0};
  auto snapshot = SessionSnapshot{SessionState::Unknown, SessionDisposition::Retire};
  std::size_t index = 0;
  std::size_t metadata_entries = plan.description_.normalized_parameter_types.size();
  const auto fail = [&](rs::util::DbErrorCode code) {
    auto& outcome = batch.outcomes[index];
    outcome.state = PreparedCommandOutcome::State::Failed;
    outcome.error.emplace(rs::util::make_error_code(code), std::string{});
    outcome.error->operation = BackendOperation::ExecutePrepared;
    outcome.snapshot = snapshot = {SessionState::Disconnected, SessionDisposition::Retire};
    batch.examined = index + 1;
    retire();
  };
  try {
    snapshot = passive_outcome(physical_session()->is_connected(), physical_session()->session_state());
    if (snapshot.state == SessionState::Unknown || snapshot.state == SessionState::Disconnected) {
      fail(rs::util::DbErrorCode::NotConnected);
      return BackendResult<PreparedCommandBatchResult>{std::move(batch), snapshot};
    }
    for (; index < plan.sets_.size(); ++index) {
      auto& outcome = batch.outcomes[index];
      const auto& set = plan.sets_[index];
      if (set.ignored) {
        outcome.state = PreparedCommandOutcome::State::Skipped;
        outcome.snapshot = snapshot;
        batch.examined = index + 1;
        continue;
      }
      if (rs::util::Clock::now() >= plan.deadline_) { fail(rs::util::DbErrorCode::Timeout); break; }
      auto result = execute_prepared(plan.sql_, set.values, plan.deadline_);
      snapshot = result.session_snapshot();
      outcome.snapshot = snapshot;
      batch.examined = index + 1;
      if (!result) {
        outcome.state = PreparedCommandOutcome::State::Failed;
        outcome.error = std::move(result.backend_error());
        break;
      }
      if (rs::util::Clock::now() >= plan.deadline_) { fail(rs::util::DbErrorCode::Timeout); break; }
      // NoData authority never permits hiding unexpected SELECT/compound output.
      if (!result->columns.empty() || !result->rows.empty() || result->error ||
          !result->additional_results.empty() || !result->cell_errors.empty() || result->described_result_shape ||
          (result->execution_result_shape && result->execution_result_shape != ExecutionResultShape::NoResultSet) ||
          !result->statement_kind || *result->statement_kind == StatementKind::SelectCursor ||
          snapshot.state == SessionState::Unknown || snapshot.state == SessionState::Disconnected ||
          snapshot.disposition == SessionDisposition::Retire) {
        fail(rs::util::DbErrorCode::ProtocolError); break;
      }
      if (result->normalized_parameter_types.size() > plan.limits_.max_metadata_entries - metadata_entries) {
        fail(rs::util::DbErrorCode::ResourceLimit); break;
      }
      metadata_entries += result->normalized_parameter_types.size();
      outcome.state = PreparedCommandOutcome::State::Succeeded;
      outcome.result = std::move(*result);
    }
  } catch (const std::bad_alloc&) {
    fail(rs::util::DbErrorCode::AllocationFailure);
  } catch (...) {
    fail(rs::util::DbErrorCode::ProtocolError);
  }
  return BackendResult<PreparedCommandBatchResult>{std::move(batch), snapshot};
}

PreparedResultSequence::PreparedResultSequence(PreparedResultSequence&& other) noexcept
    : sql_(std::move(other.sql_)), sets_(std::move(other.sets_)), description_(std::move(other.description_)),
      owner_(std::move(other.owner_)), borrow_(std::move(other.borrow_)), deadline_(other.deadline_),
      limits_(other.limits_), decoded_value_bytes_limit_(other.decoded_value_bytes_limit_),
      next_(other.next_), rows_(other.rows_), cells_(other.cells_), metadata_(other.metadata_),
      names_(other.names_), decoded_value_bytes_(other.decoded_value_bytes_), closed_(std::exchange(other.closed_, true)) {}
PreparedResultSequence& PreparedResultSequence::operator=(PreparedResultSequence&& other) noexcept {
  if (this != &other) {
    sql_ = std::move(other.sql_); sets_ = std::move(other.sets_); description_ = std::move(other.description_);
    owner_ = std::move(other.owner_); borrow_ = std::move(other.borrow_); deadline_ = other.deadline_;
    limits_ = other.limits_; decoded_value_bytes_limit_ = other.decoded_value_bytes_limit_;
    next_ = other.next_; rows_ = other.rows_; cells_ = other.cells_; metadata_ = other.metadata_;
    names_ = other.names_; decoded_value_bytes_ = other.decoded_value_bytes_;
    closed_ = std::exchange(other.closed_, true);
  }
  return *this;
}
BackendResult<PreparedResultSequence> SessionLease::start_result_sequence(
    PreparedCommandPlan&& plan, std::size_t decoded_limit) {
  if (plan.consumed_ || !borrow_ || plan.owner_.lock() != state_ || plan.borrow_.lock() != borrow_ ||
      !physical_session() || plan.sets_.empty() || plan.outcomes_.size() != plan.sets_.size() ||
      plan.description_.described_result_shape != DescribedResultShape::ResultSet)
    return local_backend_error(LocalFailure::InvalidInput, "Prepared result plan is not valid for this borrow",
        BackendOperation::ExecutePrepared, SessionState::Unknown);
  plan.consumed_ = true;
  PreparedResultSequence sequence{std::move(plan.sql_), std::move(plan.sets_), std::move(plan.description_),
      state_, borrow_, plan.deadline_, plan.limits_, decoded_limit};
  const auto& description = sequence.description_;
  if (description.columns.size() > sequence.limits_.max_columns_per_description ||
      description.columns.size() > sequence.limits_.max_metadata_entries ||
      description.normalized_parameter_types.size() > sequence.limits_.max_metadata_entries - description.columns.size())
    return BackendResult<PreparedResultSequence>{rs::util::DbErrorCode::ResourceLimit, {}};
  sequence.metadata_ = description.columns.size() + description.normalized_parameter_types.size();
  for (const auto& column : description.columns) {
    if (!column.normalized_type || column.name.find('\0') != std::string::npos ||
        !rs::util::utf8_code_point_count(column.name))
      return BackendResult<PreparedResultSequence>{rs::util::DbErrorCode::ProtocolError, {}};
    if (column.name.size() > sequence.limits_.max_column_name_bytes ||
        column.name.size() > sequence.limits_.max_metadata_name_bytes - sequence.names_)
      return BackendResult<PreparedResultSequence>{rs::util::DbErrorCode::ResourceLimit, {}};
    sequence.names_ += column.name.size();
  }
  return BackendResult<PreparedResultSequence>{std::move(sequence),
      passive_outcome(physical_session()->is_connected(), physical_session()->session_state())};
}
bool SessionLease::accepts_result_sequence(const PreparedResultSequence& sequence) const noexcept {
  return !sequence.closed_ && borrow_ && sequence.owner_.lock() == state_ &&
      sequence.borrow_.lock() == borrow_ && physical_session() &&
      !sequence.sets_.empty() && sequence.next_ <= sequence.sets_.size();
}
BackendResult<PreparedResultStep> SessionLease::advance_result_sequence(PreparedResultSequence& sequence) {
  // Refuse stale/moved/terminal plans before callbacks or input indexing.
  if (!accepts_result_sequence(sequence)) {
    sequence.closed_ = true;
    return local_backend_error(LocalFailure::InvalidInput, "Prepared result sequence is not valid for this borrow",
        BackendOperation::ExecutePrepared, SessionState::Unknown);
  }
  PreparedResultStep step;
  const auto fail = [&](rs::util::DbErrorCode code) {
    sequence.closed_ = true;
    step.error.emplace(rs::util::make_error_code(code), std::string{});
    step.error->operation = BackendOperation::ExecutePrepared;
    step.snapshot = {SessionState::Disconnected, SessionDisposition::Retire};
    retire();
  };
  try {
    step.snapshot = passive_outcome(physical_session()->is_connected(), physical_session()->session_state());
    if (step.snapshot.state == SessionState::Unknown || step.snapshot.state == SessionState::Disconnected ||
        step.snapshot.disposition == SessionDisposition::Retire) {
      fail(rs::util::DbErrorCode::NotConnected);
      return BackendResult<PreparedResultStep>{std::move(step), {SessionState::Disconnected, SessionDisposition::Retire}};
    }
    while (sequence.next_ < sequence.sets_.size() && sequence.sets_[sequence.next_].ignored)
      ++sequence.next_;
    step.examined = sequence.next_;
    if (sequence.next_ == sequence.sets_.size()) {
      sequence.closed_ = true;
      const auto snapshot = step.snapshot;
      return BackendResult<PreparedResultStep>{std::move(step), snapshot};
    }
    step.ordinal = sequence.next_ + 1;
    step.examined = step.ordinal;
    if (rs::util::Clock::now() >= sequence.deadline_) {
      fail(rs::util::DbErrorCode::Timeout);
      return BackendResult<PreparedResultStep>{std::move(step), {SessionState::Disconnected, SessionDisposition::Retire}};
    }
    const auto& set = sequence.sets_[sequence.next_++];
    auto result = execute_prepared(sequence.sql_, set.values, sequence.deadline_);
    step.snapshot = result.session_snapshot();
    if (!result) {
      sequence.closed_ = true;
      step.error = std::move(result.backend_error());
    } else {
      const auto& value = *result;
      if (rs::util::Clock::now() >= sequence.deadline_) fail(rs::util::DbErrorCode::Timeout);
      else if (step.snapshot.state == SessionState::Unknown || step.snapshot.state == SessionState::Disconnected ||
          step.snapshot.disposition == SessionDisposition::Retire || value.error || value.described_result_shape ||
          value.execution_result_shape != ExecutionResultShape::ResultSet || !value.additional_results.empty() ||
          value.columns.size() != sequence.description_.columns.size()) fail(rs::util::DbErrorCode::ProtocolError);
      else {
        const auto consume = [](std::size_t amount, std::size_t limit, std::size_t& used) {
          if (used > limit || amount > limit - used) return false;
          used += amount; return true;
        };
        bool valid = true, bounded = consume(value.rows.size(), sequence.limits_.max_rows, sequence.rows_);
        bounded = consume(value.columns.size(), sequence.limits_.max_metadata_entries, sequence.metadata_) && bounded;
        bounded = consume(value.normalized_parameter_types.size(), sequence.limits_.max_metadata_entries, sequence.metadata_) && bounded;
        for (std::size_t index = 0; index < value.columns.size(); ++index) {
          const auto& column = value.columns[index];
          const auto& expected = sequence.description_.columns[index];
          if (!column.normalized_type || column.name.find('\0') != std::string::npos ||
              !rs::util::utf8_code_point_count(column.name) || !expected.normalized_type) { valid = false; continue; }
          const auto& actual = *column.normalized_type; const auto& described = *expected.normalized_type;
          if (actual.type != described.type || actual.column_size != described.column_size ||
              actual.decimal_digits != described.decimal_digits || actual.known != described.known) valid = false;
          if (column.name.size() > sequence.limits_.max_column_name_bytes) bounded = false;
          bounded = consume(column.name.size(), sequence.limits_.max_metadata_name_bytes, sequence.names_) && bounded;
        }
        for (const auto& row : value.rows) {
          if (row.size() != value.columns.size()) valid = false;
          bounded = consume(row.size(), sequence.limits_.max_cells, sequence.cells_) && bounded;
          // Every non-NULL normalized cell is an owning string, including numeric/date/binary.
          for (const auto& cell : row) if (cell)
            bounded = consume(cell->size(), sequence.decoded_value_bytes_limit_, sequence.decoded_value_bytes_) && bounded;
        }
        if (!value.normalized_parameter_types.empty()) {
          if (value.normalized_parameter_types.size() != sequence.description_.normalized_parameter_types.size()) valid = false;
          else for (std::size_t index = 0; index < value.normalized_parameter_types.size(); ++index) {
            const auto& actual = value.normalized_parameter_types[index];
            const auto& expected = sequence.description_.normalized_parameter_types[index];
            if (actual.type != expected.type || actual.column_size != expected.column_size ||
                actual.decimal_digits != expected.decimal_digits || actual.known != expected.known) valid = false;
          }
        }
        std::optional<CellEncodingError> previous;
        for (const auto& error : value.cell_errors) {
          if (error.row >= value.rows.size() || error.column >= value.columns.size() || error.column >= value.rows[error.row].size() ||
              !value.rows[error.row][error.column] || (previous && *previous >= error)) valid = false;
          previous = error;
        }
        if (!valid) fail(rs::util::DbErrorCode::ProtocolError);
        else if (!bounded) fail(rs::util::DbErrorCode::ResourceLimit);
        else if (rs::util::Clock::now() >= sequence.deadline_) fail(rs::util::DbErrorCode::Timeout);
        else step.result = std::move(*result);
      }
    }
  } catch (const std::bad_alloc&) { fail(rs::util::DbErrorCode::AllocationFailure); }
    catch (...) { fail(rs::util::DbErrorCode::ProtocolError); }
  const auto snapshot = step.snapshot;
  return BackendResult<PreparedResultStep>{std::move(step), snapshot};
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
      borrow_.reset();
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
