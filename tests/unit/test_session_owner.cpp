#include <gtest/gtest.h>
#include "core/database/session_owner.h"
#include <array>
#include <atomic>
#include <barrier>
#include <functional>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace rs::core::database::detail {
struct SessionOwnershipTestAccess {
  static SessionOwner make(std::unique_ptr<IDatabaseConnection> session, CredentialToken token,
      std::shared_ptr<const SessionCacheGeneration>(*factory)()) {
    return SessionOwner{std::move(session), std::optional{std::move(token)}, factory};
  }
  static BackendResult<SessionOwner> connect(std::unique_ptr<IDatabaseConnection> session, const ConnectionSettings& settings,
      SessionReusePolicy policy, std::unique_ptr<CredentialContext>(*factory)()) {
    return SessionOwner::connect_authenticated_impl(std::move(session), settings, policy, std::nullopt, factory);
  }
  static SessionOwner with_policy(std::unique_ptr<IDatabaseConnection> session, CredentialToken token,
      SessionReusePolicy policy, rs::util::Deadline(*now)() noexcept) {
    return SessionOwner{std::move(session), std::optional{std::move(token)}, &SessionOwner::make_cache_generation, policy, now};
  }
  static std::shared_ptr<const SessionCacheGeneration> generate() { return SessionOwner::make_cache_generation(); }
};
}
namespace {
using namespace rs::core::database;
template<class T> concept ExposesRawSession = requires(T& lease) { lease.session(); };
template<class T> concept ExposesPhysicalSession = requires(T& lease) { lease.physical_session(); };
static_assert(!ExposesRawSession<SessionLease> && !ExposesPhysicalSession<SessionLease>);
static_assert(!std::is_default_constructible_v<SessionCacheToken>);
static_assert(std::is_copy_constructible_v<SessionCacheToken> && std::is_copy_assignable_v<SessionCacheToken>);
static_assert(std::is_nothrow_move_constructible_v<SessionCacheToken> && std::is_nothrow_move_assignable_v<SessionCacheToken>);
static_assert(!std::is_copy_constructible_v<BackendResult<SessionLease>>);
static_assert(std::is_move_constructible_v<BackendResult<SessionLease>>);
static_assert(!std::is_copy_constructible_v<SessionOwner> && !std::is_copy_assignable_v<SessionOwner>);
static_assert(!std::is_copy_constructible_v<SessionLease> && !std::is_copy_assignable_v<SessionLease>);
static_assert(std::is_nothrow_move_constructible_v<SessionOwner> && std::is_nothrow_move_assignable_v<SessionOwner>);
static_assert(std::is_nothrow_move_constructible_v<SessionLease> && std::is_nothrow_move_assignable_v<SessionLease>);
struct Observed {
  bool catalog_facet{false};
  TransactionCapabilities transactions{true, true, TransactionIsolation::Serializable, {true, true, true, true}};
  std::string advertised_version{"fixture"};
  std::optional<rs::util::Result<std::string>> catalog_result;
  std::function<void(const CatalogRequest&)> on_catalog;
  int observation_exception{};
  bool operation_facets{false};
  int facet_exception{};
  std::optional<BackendResult<void>> transaction_result;
  std::optional<BackendResult<QueryResult>> description_result;
  std::function<void(TransactionAction, rs::util::Deadline)> on_transaction;
  std::function<void(TransactionIsolation, rs::util::Deadline)> on_isolation;
  std::function<void(std::string_view, std::span<const QueryParameterType>, rs::util::Deadline)> on_description;
  std::atomic<int> disconnects{}, destructions{}, queries{}, health{}, resets{};
  bool throw_disconnect{};
  bool initially_connected{true};
  std::atomic<int> connects{};
  int connect_mode{};
  std::function<void(const ConnectionSettings&)> on_connect;
  std::optional<SessionSnapshot> connect_snapshot;
  bool missing_reset{};
  bool missing_health{};
  int health_mode{};
  rs::util::Deadline health_deadline{};
  std::function<void()> on_health;
  std::optional<SessionSnapshot> health_snapshot;
  int reset_mode{};
  rs::util::Deadline reset_deadline{};
  std::function<void()> on_reset;
  std::function<void()> on_passive;
  std::optional<SessionState> passive_state;
  std::optional<bool> passive_connected;
  std::optional<SessionSnapshot> reset_snapshot;
  std::optional<BackendResult<QueryResult>> execution_result;
  int execution_exception{};
  std::function<void(std::string_view, std::span<const QueryParameter>, rs::util::Deadline, bool)> on_execution;
  std::function<void()> on_disconnect;
};
class FakeSession final : public IDatabaseConnection, public ISessionHealth, public ISessionReset, public ITransactionSession, public IStatementDescription, public ICatalogQueries {
 public:
  explicit FakeSession(std::shared_ptr<Observed> observed) : observed_(std::move(observed)), connected_(observed_->initially_connected) {}
  ~FakeSession() override { ++observed_->destructions; }
  BackendResult<void> connect(const ConnectionSettings& settings) override {
    ++observed_->connects; if (observed_->on_connect) observed_->on_connect(settings);
    if (observed_->connect_mode == 1) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::AuthenticationFailed), "owned connect error"};
      error.native_state = "28P01"; error.native_code = 23; error.retry_safe = true;
      error.session_state = SessionState::Idle; error.disposition = SessionDisposition::Reusable; return error;
    }
    if (observed_->connect_mode == 2) throw std::bad_alloc{};
    if (observed_->connect_mode == 3) throw std::runtime_error("SECRET connect fixture");
    if (observed_->connect_mode == 4) throw 42;
    connected_ = observed_->connect_mode != 5;
    if (observed_->connect_snapshot) return BackendResult<void>{*observed_->connect_snapshot};
    return BackendResult<void>{SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}};
  }
  void disconnect() override {
    ++observed_->disconnects; connected_ = false;
    if (observed_->on_disconnect) observed_->on_disconnect();
    if (observed_->throw_disconnect) throw std::runtime_error("disconnect fixture");
  }
  bool is_connected() const override {
    if (observed_->reset_mode == 11) throw std::runtime_error("private connected fixture");
    return observed_->passive_connected.value_or(connected_);
  }
  SessionState session_state() const override {
    if (observed_->on_passive) observed_->on_passive();
    if (observed_->passive_state) return *observed_->passive_state;
    if (observed_->reset_mode == 8) throw std::runtime_error("private passive-state fixture");
    return !connected_ ? SessionState::Disconnected :
        observed_->reset_mode == 7 ? SessionState::Transaction : SessionState::Idle;
  }
  BackendResult<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline) override {
    return execute(sql, {}, deadline, false);
  }
  BackendResult<QueryResult> execute_prepared(std::string_view sql,
      std::span<const QueryParameter> params, rs::util::Deadline deadline) override {
    return execute(sql, params, deadline, true);
  }
  std::string server_version() const override {
    throw_observation(); return observed_->advertised_version;
  }
  const ICatalogQueries* catalog_queries() const noexcept override { return observed_->catalog_facet ? this : nullptr; }
  rs::util::Result<std::string> catalog_query(const CatalogRequest& request) const override {
    if (observed_->on_catalog) observed_->on_catalog(request);
    throw_observation();
    return observed_->catalog_result.value_or(rs::util::Result<std::string>{std::string("SELECT fixture")});
  }
  ISessionHealth* session_health() noexcept override { return observed_->missing_health ? nullptr : this; }
  ISessionReset* session_reset() noexcept override { return observed_->missing_reset ? nullptr : this; }
  SessionResetProfile reset_profile() const noexcept override {
    return observed_->reset_mode == 9 ? static_cast<SessionResetProfile>(99) : SessionResetProfile::SameAuthenticatedServerSession;
  }
  BackendResult<void> check_health(rs::util::Deadline deadline) override {
    ++observed_->health; observed_->health_deadline = deadline;
    if (observed_->on_health) observed_->on_health();
    if (observed_->health_mode == 1) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owned health error"};
      error.native_state = "XX001"; error.native_code = 73; error.retry_safe = true;
      error.operation = BackendOperation::ExecuteDirect; error.session_state = SessionState::Idle;
      error.disposition = SessionDisposition::Reusable; return error;
    }
    if (observed_->health_mode == 2) throw std::bad_alloc{};
    if (observed_->health_mode == 3) throw std::runtime_error("SECRET health fixture");
    if (observed_->health_mode == 4) throw 42;
    if (observed_->health_mode == 5) connected_ = false;
    if (observed_->health_snapshot) return BackendResult<void>{*observed_->health_snapshot};
    return BackendResult<void>{SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}};
  }
  BackendResult<void> reset_session(rs::util::Deadline deadline) override {
    ++observed_->resets; observed_->reset_deadline = deadline;
    if (observed_->on_reset) observed_->on_reset();
    if (observed_->reset_mode == 1) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owned fixture error"};
      error.native_state = "XX000"; error.native_code = 42;
      error.operation = BackendOperation::ExecuteDirect; error.session_state = SessionState::Idle;
      error.disposition = SessionDisposition::Reusable; error.retry_safe = true;
      return error;
    }
    if (observed_->reset_mode == 2) throw std::bad_alloc{};
    if (observed_->reset_mode == 3) throw std::runtime_error("SECRET fixture detail");
    if (observed_->reset_mode == 10) throw 42;
    if (observed_->reset_snapshot) return BackendResult<void>{*observed_->reset_snapshot};
    if (observed_->reset_mode == 4) return {};
    if (observed_->reset_mode == 5) return BackendResult<void>{SessionSnapshot{SessionState::Idle, SessionDisposition::ResetRequired}};
    if (observed_->reset_mode == 6) connected_ = false;
    return BackendResult<void>{SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}};
  }
  ITransactionSession* transaction_session() noexcept override { return observed_->operation_facets ? this : nullptr; }
  IStatementDescription* statement_description() noexcept override { return observed_->operation_facets ? this : nullptr; }
  TransactionCapabilities transaction_capabilities() const override { throw_observation(); return observed_->transactions; }
  BackendResult<void> transaction(TransactionAction action, rs::util::Deadline deadline) override {
    if (observed_->on_transaction) observed_->on_transaction(action, deadline);
    throw_facet();
    return observed_->transaction_result.value_or(BackendResult<void>{{SessionState::Transaction, SessionDisposition::ResetRequired}});
  }
  BackendResult<void> set_transaction_isolation(TransactionIsolation level, rs::util::Deadline deadline) override {
    if (observed_->on_isolation) observed_->on_isolation(level, deadline);
    throw_facet();
    return observed_->transaction_result.value_or(BackendResult<void>{{SessionState::Idle, SessionDisposition::Reusable}});
  }
  BackendResult<QueryResult> describe_statement(std::string_view sql,
      std::span<const QueryParameterType> types, rs::util::Deadline deadline) override {
    if (observed_->on_description) observed_->on_description(sql, types, deadline);
    throw_facet();
    return observed_->description_result.value_or(BackendResult<QueryResult>{QueryResult{}, {SessionState::Idle, SessionDisposition::Reusable}});
  }
 private:
  void throw_observation() const {
    if (observed_->observation_exception == 1) throw std::bad_alloc{};
    if (observed_->observation_exception == 2) throw std::runtime_error("original observation fixture");
    if (observed_->observation_exception == 3) throw 42;
  }
  void throw_facet() {
    if (observed_->facet_exception == 1) throw std::bad_alloc{};
    if (observed_->facet_exception == 2) throw std::runtime_error("original facet fixture");
    if (observed_->facet_exception == 3) throw 42;
  }
  BackendResult<QueryResult> execute(std::string_view sql, std::span<const QueryParameter> params,
      rs::util::Deadline deadline, bool prepared) {
    ++observed_->queries;
    if (observed_->on_execution) observed_->on_execution(sql, params, deadline, prepared);
    if (observed_->execution_exception == 1) throw std::bad_alloc{};
    if (observed_->execution_exception == 2) throw std::runtime_error("original execution fixture");
    if (observed_->execution_exception == 3) throw 42;
    if (observed_->execution_result) return *observed_->execution_result;
    return BackendResult<QueryResult>{QueryResult{}, {SessionState::Idle, SessionDisposition::Reusable}};
  }
  std::shared_ptr<Observed> observed_;
  bool connected_{true};
};
SessionOwner owner_for(const std::shared_ptr<Observed>& observed) {
  return SessionOwner{std::make_unique<FakeSession>(observed)};
}
void retired_once(const Observed& observed) {
  EXPECT_EQ(1, observed.disconnects); EXPECT_EQ(1, observed.destructions);
  EXPECT_EQ(0, observed.health); EXPECT_EQ(0, observed.resets);
}
TEST(SessionOwnerTest, OnlyOneBorrowerAndReturnIsTerminal) {
  auto observed = std::make_shared<Observed>();
  auto owner = owner_for(observed);
  auto lease = owner.try_acquire(); ASSERT_TRUE(lease); ASSERT_TRUE(*lease);
  EXPECT_FALSE(owner.try_acquire());
  ASSERT_TRUE(lease->execute_query("fixture", rs::util::Deadline::max()));
  lease->retire(); EXPECT_FALSE(*lease);
  lease->retire(); lease.reset();
  EXPECT_FALSE(owner.try_acquire()); retired_once(*observed);
}
TEST(SessionOwnerTest, ConcurrentCheckoutHasExactlyOneWinner) {
  auto observed = std::make_shared<Observed>(); auto owner = owner_for(observed);
  constexpr std::size_t count = 12;
  std::barrier start(static_cast<std::ptrdiff_t>(count + 1));
  std::array<std::optional<SessionLease>, count> leases;
  std::array<std::thread, count> threads;
  for (std::size_t index = 0; index < count; ++index) {
    threads[index] = std::thread([&, index] { start.arrive_and_wait(); leases[index] = owner.try_acquire(); });
  }
  start.arrive_and_wait();
  for (auto& thread : threads) thread.join();
  std::size_t winners = 0;
  for (auto& lease : leases) { if (lease) ++winners; }
  EXPECT_EQ(1u, winners); EXPECT_EQ(0, observed->disconnects);
  for (auto& lease : leases) lease.reset();
  EXPECT_FALSE(owner.try_acquire()); retired_once(*observed);
}
TEST(SessionOwnerTest, OwnerCanDieBeforeItsBorrowerWithoutInterruptingIt) {
  auto observed = std::make_shared<Observed>(); std::optional<SessionLease> lease;
  {
    auto owner = owner_for(observed); lease = owner.try_acquire(); ASSERT_TRUE(lease);
  }
  EXPECT_EQ(0, observed->disconnects); EXPECT_EQ(0, observed->destructions);
  ASSERT_TRUE(lease->execute_query("fixture", rs::util::Deadline::max()));
  EXPECT_EQ(1, observed->queries); lease.reset(); retired_once(*observed);
}
TEST(SessionOwnerTest, IdleOwnerAndUnwindingRetireWithoutResetOrHealth) {
  auto idle = std::make_shared<Observed>(); { auto owner = owner_for(idle); }
  retired_once(*idle);
  auto unwound = std::make_shared<Observed>(); auto owner = owner_for(unwound);
  EXPECT_THROW({ auto lease = owner.try_acquire(); throw std::runtime_error("unwind fixture"); }, std::runtime_error);
  EXPECT_FALSE(owner.try_acquire()); retired_once(*unwound);
}
TEST(SessionOwnerTest, LeaseMoveAssignmentRetiresDestinationAndTransfersBorrow) {
  auto first = std::make_shared<Observed>(); auto second = std::make_shared<Observed>();
  auto owner1 = owner_for(first); auto owner2 = owner_for(second);
  auto lease1 = owner1.try_acquire(); auto lease2 = owner2.try_acquire();
  ASSERT_TRUE(lease1); ASSERT_TRUE(lease2);
  SessionLease moved{std::move(*lease1)};
  EXPECT_FALSE(*lease1); EXPECT_TRUE(moved);
  *lease2 = std::move(moved); EXPECT_FALSE(moved); EXPECT_TRUE(*lease2);
  ASSERT_TRUE(lease2->execute_query("transferred", rs::util::Deadline::max()));
  EXPECT_EQ(1, first->queries); EXPECT_EQ(0, second->queries);
  retired_once(*second); EXPECT_EQ(0, first->disconnects);
  auto& same = *lease2; same = std::move(*lease2); EXPECT_TRUE(*lease2);
  lease1.reset(); lease2.reset(); retired_once(*first);
  EXPECT_FALSE(owner1.try_acquire()); EXPECT_FALSE(owner2.try_acquire());
}
TEST(SessionOwnerTest, OwnerMovesClosePriorAdmissionAndKeepLiveLeaseValid) {
  auto first = std::make_shared<Observed>(); auto second = std::make_shared<Observed>();
  auto owner1 = owner_for(first); auto owner2 = owner_for(second);
  auto lease1 = owner1.try_acquire(); ASSERT_TRUE(lease1);
  SessionOwner moved{std::move(owner2)}; EXPECT_FALSE(owner2.try_acquire());
  owner1 = std::move(moved); EXPECT_FALSE(moved.try_acquire());
  EXPECT_EQ(0, first->disconnects); EXPECT_TRUE(*lease1);
  auto lease2 = owner1.try_acquire(); ASSERT_TRUE(lease2);
  auto& same = owner1; same = std::move(owner1); EXPECT_FALSE(owner1.try_acquire());
  lease1.reset(); lease2.reset(); retired_once(*first); retired_once(*second);
}
TEST(SessionOwnerTest, OwnerMoveAssignmentRetiresItsIdleDestination) {
  auto first = std::make_shared<Observed>(); auto second = std::make_shared<Observed>();
  {
    auto owner1 = owner_for(first); auto owner2 = owner_for(second);
    owner1 = std::move(owner2); retired_once(*first);
    EXPECT_FALSE(owner2.try_acquire()); EXPECT_EQ(0, second->disconnects);
  }
  retired_once(*second);
}
TEST(SessionOwnerTest, OwnerDestructionRacingLeaseRetirementDestroysOnce) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    auto observed = std::make_shared<Observed>();
    auto owner = std::make_unique<SessionOwner>(owner_for(observed));
    auto lease = owner->try_acquire(); ASSERT_TRUE(lease);
    std::barrier start(3);
    std::thread closing([&] { start.arrive_and_wait(); owner.reset(); });
    std::thread returning([&] { start.arrive_and_wait(); lease->retire(); });
    start.arrive_and_wait(); closing.join(); returning.join();
    EXPECT_FALSE(*lease); retired_once(*observed);
  }
}
TEST(SessionOwnerTest, BackendDisconnectCanReenterAdmissionWithoutLockingDeadlock) {
  auto observed = std::make_shared<Observed>(); auto owner = owner_for(observed);
  auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
  observed->on_disconnect = [&] { EXPECT_FALSE(owner.try_acquire()); };
  lease->retire(); retired_once(*observed);
  observed->on_disconnect = {};
}
TEST(SessionOwnerTest, NullRejectedAndDisconnectExceptionsContained) {
  EXPECT_THROW(SessionOwner{nullptr}, std::invalid_argument);
  for (const bool leased : {false, true}) {
    auto observed = std::make_shared<Observed>(); observed->throw_disconnect = true;
    { auto owner = owner_for(observed); if (leased) { auto lease = owner.try_acquire(); } }
    retired_once(*observed);
  }
}
} // namespace

namespace {
TEST(SessionOwnerCredentialsTest, BoundAdmissionRequiresExactAuthorityAndGeneration) {
  CredentialContext context; CredentialContext foreign;
  auto old = context.publish_authenticated(); auto current = context.publish_authenticated();
  auto foreign_token = foreign.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  SessionOwner owner{std::make_unique<FakeSession>(observed), current};
  EXPECT_FALSE(owner.try_acquire()); EXPECT_FALSE(owner.try_acquire(foreign_token)); EXPECT_FALSE(owner.try_acquire(old));
  EXPECT_EQ(0, observed->disconnects);
  auto lease = owner.try_acquire(current); ASSERT_TRUE(lease); EXPECT_FALSE(owner.try_acquire(current));
  lease.reset(); retired_once(*observed);
  auto legacy_observed = std::make_shared<Observed>(); auto legacy = owner_for(legacy_observed);
  EXPECT_FALSE(legacy.try_acquire(current)); auto legacy_lease = legacy.try_acquire(); ASSERT_TRUE(legacy_lease);
}
TEST(SessionOwnerCredentialsTest, MatchingRevokedRotatedExpiredAndOrphanedCredentialsRetireIdle) {
  for (int mode = 0; mode < 4; ++mode) {
    auto context = std::make_unique<CredentialContext>();
    auto token = context->publish_authenticated(mode == 2 ? std::optional{rs::util::Deadline::min()} : std::nullopt);
    auto observed = std::make_shared<Observed>(); observed->throw_disconnect = mode == 3;
    SessionOwner owner{std::make_unique<FakeSession>(observed), token};
    if (mode == 0) context->revoke();
    if (mode == 1) { (void)context->publish_authenticated(); }
    if (mode == 3) context.reset();
    EXPECT_FALSE(owner.try_acquire(token)); EXPECT_FALSE(owner.try_acquire(token)); retired_once(*observed);
  }
}
TEST(SessionOwnerCredentialsTest, RevocationClosesAdmissionWithoutInterruptingActiveBorrower) {
  CredentialContext context; auto token = context.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  context.revoke(); EXPECT_FALSE(owner.try_acquire(token));
  EXPECT_EQ(0, observed->disconnects); EXPECT_EQ(0, observed->destructions);
  EXPECT_TRUE(*lease);
  ASSERT_TRUE(lease->execute_query("fixture", rs::util::Deadline::max()));
  (void)context.publish_authenticated(); EXPECT_FALSE(owner.try_acquire(token));
  lease.reset(); retired_once(*observed);
}
TEST(SessionOwnerCredentialsTest, RotationRacingCheckoutOrRetirementNeverDuplicatesOwnership) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    CredentialContext context; auto token = context.publish_authenticated();
    auto observed = std::make_shared<Observed>();
    SessionOwner owner{std::make_unique<FakeSession>(observed), token};
    std::optional<SessionLease> lease;
    std::barrier start(3);
    std::thread acquiring([&] { start.arrive_and_wait(); lease = owner.try_acquire(token); });
    std::thread rotating([&] { start.arrive_and_wait(); (void)context.publish_authenticated(); });
    start.arrive_and_wait(); acquiring.join(); rotating.join();
    EXPECT_FALSE(token.is_current()); EXPECT_FALSE(owner.try_acquire(token));
    if (lease) { EXPECT_EQ(0, observed->disconnects); lease.reset(); }
    retired_once(*observed);
  }
}
} // namespace

namespace {
TEST(SessionOwnerCredentialsTest, NewGenerationCannotRetireOldBindingUntilExactStaleTokenIsPresented) {
  CredentialContext context; auto bound = context.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  SessionOwner owner{std::make_unique<FakeSession>(observed), bound};
  auto fresh = context.publish_authenticated();
  EXPECT_FALSE(owner.try_acquire(fresh)); EXPECT_EQ(0, observed->disconnects);
  EXPECT_EQ(0, observed->destructions); EXPECT_TRUE(fresh.is_current());
  EXPECT_FALSE(owner.try_acquire(bound)); retired_once(*observed);
}
} // namespace

namespace {
TEST(SessionLeaseResetTest, SuccessUsesOneDeadlineAndKeepsExclusiveBorrowWithoutReturn) {
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  credentials.revoke(); // Cleanup of this borrower is not new-borrower admission.
  observed->on_reset = [&] { EXPECT_FALSE(owner.try_acquire(token)); };
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto result = lease->reset_session(deadline); ASSERT_TRUE(result);
  EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
  EXPECT_EQ(deadline, observed->reset_deadline); EXPECT_EQ(1, observed->resets);
  EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects);
  ASSERT_TRUE(lease->execute_query("fixture", deadline));
  lease->retire(); EXPECT_FALSE(owner.try_acquire(token));
  EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
}
TEST(SessionLeaseResetTest, UnsupportedProfileAndExpiredDeadlineRetireWithoutCleanupIO) {
  for (int mode = 0; mode < 3; ++mode) {
    auto observed = std::make_shared<Observed>();
    observed->missing_reset = mode == 0; observed->reset_mode = mode == 1 ? 9 : 0;
    auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
    auto result = lease->reset_session(mode == 2 ? rs::util::Deadline::min() : rs::util::Deadline::max());
    ASSERT_FALSE(result); EXPECT_EQ(mode == 2 ? BackendErrorClass::Timeout : BackendErrorClass::Unsupported,
                                   result.backend_error().error_class);
    EXPECT_EQ(BackendOperation::ResetSession, result.backend_error().operation);
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    EXPECT_FALSE(result.backend_error().retry_safe); EXPECT_FALSE(*lease); EXPECT_FALSE(owner.try_acquire());
    retired_once(*observed);
  }
}
TEST(SessionLeaseResetTest, AllAmbiguousAndThrowingOutcomesRetireAndSuppressRetry) {
  for (const int mode : {1, 2, 3, 4, 5, 6, 7, 8, 10, 11}) {
    auto observed = std::make_shared<Observed>(); observed->reset_mode = mode; observed->throw_disconnect = true;
    auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
    observed->on_disconnect = [&] { EXPECT_FALSE(owner.try_acquire()); };
    auto result = lease->reset_session(rs::util::Deadline::max()); ASSERT_FALSE(result);
    EXPECT_EQ(BackendOperation::ResetSession, result.backend_error().operation);
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    EXPECT_FALSE(result.backend_error().retry_safe); EXPECT_FALSE(*lease);
    EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions); EXPECT_EQ(1, observed->resets);
    if (mode == 1) {
      EXPECT_EQ("owned fixture error", result.error_message()); EXPECT_EQ("XX000", result.backend_error().native_state);
      EXPECT_EQ(42, result.backend_error().native_code);
    } else if (mode == 2) {
      EXPECT_EQ(BackendErrorClass::AllocationFailure, result.backend_error().error_class);
    } else {
      EXPECT_EQ(BackendErrorClass::Protocol, result.backend_error().error_class);
      EXPECT_EQ(std::string::npos, result.error_message().find("SECRET"));
    }
    EXPECT_FALSE(lease->reset_session(rs::util::Deadline::max()));
    EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->resets);
    observed->on_disconnect = {};
  }
}
TEST(SessionLeaseResetTest, BackendCannotTurnLateCompletionIntoSuccessfulCleanup) {
  auto observed = std::make_shared<Observed>(); auto owner = owner_for(observed);
  auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
  observed->on_reset = [&] { std::this_thread::sleep_until(deadline); };
  auto result = lease->reset_session(deadline); ASSERT_FALSE(result);
  EXPECT_EQ(BackendErrorClass::Timeout, result.backend_error().error_class);
  EXPECT_FALSE(*lease); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->resets);
}
TEST(SessionLeaseResetTest, MovedAndRetiredLeasesRejectResetWithoutTouchingTransferredSession) {
  auto observed = std::make_shared<Observed>(); auto owner = owner_for(observed);
  auto lease = owner.try_acquire(); ASSERT_TRUE(lease); SessionLease moved{std::move(*lease)};
  auto missing = lease->reset_session(rs::util::Deadline::max()); ASSERT_FALSE(missing);
  EXPECT_EQ(BackendErrorClass::NotConnected, missing.backend_error().error_class);
  EXPECT_EQ(0, observed->disconnects); EXPECT_EQ(0, observed->resets);
  ASSERT_TRUE(moved.reset_session(rs::util::Deadline::max())); moved.retire();
  EXPECT_FALSE(moved.reset_session(rs::util::Deadline::max())); EXPECT_EQ(1, observed->disconnects);
}
} // namespace

namespace {
TEST(SessionLeaseResetTest, EveryNonIdleOrNonReusableSuccessSnapshotIsRejected) {
  for (const auto state : {SessionState::Disconnected, SessionState::Idle, SessionState::Transaction,
                           SessionState::FailedTransaction, SessionState::Unknown}) {
    for (const auto disposition : {SessionDisposition::Reusable, SessionDisposition::ResetRequired,
                                  SessionDisposition::Retire}) {
      if (state == SessionState::Idle && disposition == SessionDisposition::Reusable) continue;
      auto observed = std::make_shared<Observed>(); observed->reset_snapshot = SessionSnapshot{state, disposition};
      auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
      auto result = lease->reset_session(rs::util::Deadline::max()); ASSERT_FALSE(result);
      EXPECT_EQ(BackendErrorClass::Protocol, result.backend_error().error_class);
      EXPECT_FALSE(*lease); EXPECT_EQ(1, observed->resets); EXPECT_EQ(1, observed->disconnects);
      EXPECT_EQ(1, observed->destructions); EXPECT_FALSE(owner.try_acquire());
    }
  }
}
TEST(SessionLeaseResetTest, OwnerDestructionAndRevocationDuringResetLeaveActiveBorrowerIntact) {
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  auto owner = std::make_unique<SessionOwner>(std::make_unique<FakeSession>(observed), token);
  auto lease = owner->try_acquire(token); ASSERT_TRUE(lease);
  std::barrier entered(2); std::barrier resume(2);
  observed->on_reset = [&] { entered.arrive_and_wait(); resume.arrive_and_wait(); };
  std::optional<BackendResult<void>> result;
  std::thread resetting([&] { result = lease->reset_session(rs::util::Deadline::max()); });
  entered.arrive_and_wait(); owner.reset(); credentials.revoke();
  EXPECT_EQ(0, observed->disconnects); EXPECT_EQ(0, observed->destructions);
  resume.arrive_and_wait(); resetting.join(); ASSERT_TRUE(result); ASSERT_TRUE(*result);
  EXPECT_TRUE(*lease); ASSERT_TRUE(lease->execute_query("fixture", rs::util::Deadline::max()));
  lease->retire(); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
}
} // namespace

namespace {
TEST(SessionLeaseExecutionTest, DirectAndPreparedInputsAndDeadlineAreForwardedUnchanged) {
  auto observed = std::make_shared<Observed>(); auto owner = owner_for(observed);
  auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
  const std::string sql{"SELECT ?\0tail", 13};
  const std::array<QueryParameter, 3> params{{
    {std::nullopt, QueryParameterType::Text}, {std::string{}, QueryParameterType::Text},
    {std::string{"a\0b", 3}, QueryParameterType::Binary, true}}};
  // No lease preflight changes the deadline: the backend owns operation policy.
  const auto deadline = rs::util::Deadline::min();
  int calls = 0;
  observed->on_execution = [&](std::string_view received_sql, std::span<const QueryParameter> received,
      rs::util::Deadline received_deadline, bool prepared) {
    ++calls; EXPECT_EQ(sql.data(), received_sql.data()); EXPECT_EQ(sql.size(), received_sql.size());
    EXPECT_EQ(sql, received_sql); EXPECT_EQ(deadline, received_deadline);
    if (prepared) {
      ASSERT_EQ(params.data(), received.data()); ASSERT_EQ(params.size(), received.size());
      for (std::size_t index = 0; index < params.size(); ++index) {
        EXPECT_EQ(params[index].value, received[index].value); EXPECT_EQ(params[index].type, received[index].type);
        EXPECT_EQ(params[index].binary_input, received[index].binary_input);
      }
    } else { EXPECT_TRUE(received.empty()); }
    EXPECT_FALSE(owner.try_acquire()); // Reentrant callback must be unlocked.
  };
  ASSERT_TRUE(lease->execute_query(sql, deadline)); ASSERT_TRUE(lease->execute_prepared(sql, params, deadline));
  EXPECT_EQ(2, calls); EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects);
  observed->on_execution = {};
}
TEST(SessionLeaseExecutionTest, OwningResultsAndErrorsSurviveEveryDispositionAndRetirement) {
  for (const bool prepared : {false, true}) {
    for (const bool failure : {false, true}) {
      for (const auto disposition : {SessionDisposition::Reusable, SessionDisposition::ResetRequired, SessionDisposition::Retire}) {
        auto observed = std::make_shared<Observed>();
        const auto state = disposition == SessionDisposition::Reusable ? SessionState::Idle :
            disposition == SessionDisposition::ResetRequired ? SessionState::FailedTransaction : SessionState::Disconnected;
        if (failure) {
          BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owned execution failure"};
          error.native_state = "XX000"; error.native_code = 17; error.retry_safe = true;
          error.operation = prepared ? BackendOperation::ExecutePrepared : BackendOperation::ExecuteDirect;
          error.session_state = state; error.disposition = disposition;
          observed->execution_result = BackendResult<QueryResult>{std::move(error)};
        } else {
          QueryResult value; value.rows = {{std::string{"owned\0row", 9}}};
          value.columns.push_back({"owned_column", std::nullopt});
          observed->execution_result = BackendResult<QueryResult>{std::move(value), {state, disposition}};
        }
        auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
        auto result = prepared ? lease->execute_prepared("fixture", std::span<const QueryParameter>{}, rs::util::Deadline::max()) :
            lease->execute_query("fixture", rs::util::Deadline::max());
        EXPECT_EQ(!failure, result.has_value()); EXPECT_EQ((SessionSnapshot{state, disposition}), result.session_snapshot());
        EXPECT_EQ(disposition != SessionDisposition::Retire, static_cast<bool>(*lease));
        EXPECT_EQ(disposition == SessionDisposition::Retire ? 1 : 0, observed->disconnects);
        EXPECT_FALSE(owner.try_acquire()); lease->retire(); observed->execution_result.reset();
        retired_once(*observed);
        if (failure) {
          EXPECT_EQ("owned execution failure", result.error_message()); EXPECT_EQ("XX000", result.backend_error().native_state);
          EXPECT_EQ(17, result.backend_error().native_code); EXPECT_EQ(true, result.backend_error().retry_safe);
          EXPECT_EQ(prepared ? BackendOperation::ExecutePrepared : BackendOperation::ExecuteDirect, result.backend_error().operation);
        } else {
          ASSERT_EQ(1u, result->rows.size()); EXPECT_EQ(std::string("owned\0row", 9), result->rows[0][0]);
          ASSERT_EQ(1u, result->columns.size()); EXPECT_EQ("owned_column", result->columns[0].name);
        }
      }
    }
  }
}
TEST(SessionLeaseExecutionTest, AllExceptionCategoriesRetireBeforeRethrowingOriginalException) {
  for (const bool prepared : {false, true}) {
    for (int category = 1; category <= 3; ++category) {
      auto observed = std::make_shared<Observed>(); observed->execution_exception = category; observed->throw_disconnect = true;
      auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
      observed->on_disconnect = [&] { EXPECT_FALSE(owner.try_acquire()); };
      const auto invoke = [&] {
        return prepared ? lease->execute_prepared("fixture", std::span<const QueryParameter>{}, rs::util::Deadline::max()) :
            lease->execute_query("fixture", rs::util::Deadline::max());
      };
      if (category == 1) { EXPECT_THROW(invoke(), std::bad_alloc); }
      if (category == 2) {
        try { (void)invoke(); FAIL() << "Expected original execution exception"; }
        catch (const std::runtime_error& error) { EXPECT_STREQ("original execution fixture", error.what()); }
      }
      if (category == 3) {
        try { (void)invoke(); FAIL() << "Expected non-standard execution exception"; }
        catch (int value) { EXPECT_EQ(42, value); }
      }
      EXPECT_FALSE(*lease); retired_once(*observed); observed->on_disconnect = {};
    }
  }
}
TEST(SessionLeaseExecutionTest, MovedAndRetiredLeasesReturnOperationSpecificErrorsWithoutBackendIO) {
  auto observed = std::make_shared<Observed>(); auto owner = owner_for(observed);
  auto lease = owner.try_acquire(); ASSERT_TRUE(lease); SessionLease moved{std::move(*lease)};
  const auto check_missing = [&](SessionLease& missing) {
    auto direct = missing.execute_query("fixture", rs::util::Deadline::max());
    auto prepared = missing.execute_prepared("fixture", std::span<const QueryParameter>{}, rs::util::Deadline::max());
    ASSERT_FALSE(direct); ASSERT_FALSE(prepared);
    for (const auto* result : {&direct, &prepared}) {
      EXPECT_EQ(BackendErrorClass::NotConnected, result->backend_error().error_class);
      EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result->session_snapshot());
      EXPECT_FALSE(result->backend_error().retry_safe);
    }
    EXPECT_EQ(BackendOperation::ExecuteDirect, direct.backend_error().operation);
    EXPECT_EQ(BackendOperation::ExecutePrepared, prepared.backend_error().operation);
  };
  check_missing(*lease); EXPECT_EQ(0, observed->queries); EXPECT_EQ(0, observed->disconnects);
  ASSERT_TRUE(moved.execute_query("fixture", rs::util::Deadline::max()));
  moved.retire(); check_missing(moved); EXPECT_EQ(1, observed->queries); retired_once(*observed);
}
} // namespace

namespace {
std::atomic<int> cache_allocations{};
std::atomic<bool> fail_cache_generation{}, null_cache_generation{};
std::shared_ptr<const detail::SessionCacheGeneration> cache_factory() {
  ++cache_allocations;
  if (fail_cache_generation) throw std::bad_alloc{};
  if (null_cache_generation) return {};
  return detail::SessionOwnershipTestAccess::generate();
}
TEST(SessionCacheScopeTest, RequiresBoundIdleLeaseAndExactScopeAffinity) {
  CredentialContext credentials; auto credential = credentials.publish_authenticated();
  auto first = std::make_shared<Observed>(); auto second = std::make_shared<Observed>();
  SessionOwner owner1{std::make_unique<FakeSession>(first), credential};
  SessionOwner owner2{std::make_unique<FakeSession>(second), credential};
  auto lease1 = owner1.try_acquire(credential); auto lease2 = owner2.try_acquire(credential);
  ASSERT_TRUE(lease1); ASSERT_TRUE(lease2);
  first->on_passive = [&] { EXPECT_FALSE(owner1.try_acquire(credential)); };
  auto ticket1 = lease1->cache_token(); auto ticket2 = lease2->cache_token(); ASSERT_TRUE(ticket1); ASSERT_TRUE(ticket2);
  auto again = lease1->cache_token(); ASSERT_TRUE(again); auto copy = *ticket1;
  EXPECT_TRUE(ticket1->is_current()); EXPECT_TRUE(copy.is_current()); EXPECT_TRUE(lease1->accepts_cache(*again));
  SessionCacheToken moved_copy{std::move(copy)}; EXPECT_FALSE(copy.is_current());
  EXPECT_TRUE(lease1->accepts_cache(moved_copy));
  EXPECT_FALSE(lease1->accepts_cache(*ticket2)); EXPECT_FALSE(lease2->accepts_cache(*ticket1));
  EXPECT_TRUE(ticket1->is_current()); EXPECT_TRUE(ticket2->is_current());
  auto legacy_observed = std::make_shared<Observed>(); auto legacy = owner_for(legacy_observed);
  auto legacy_lease = legacy.try_acquire(); ASSERT_TRUE(legacy_lease);
  EXPECT_FALSE(legacy_lease->cache_token()); EXPECT_FALSE(legacy_lease->accepts_cache(*ticket1));
  EXPECT_EQ(0, first->queries); EXPECT_EQ(0, first->disconnects); first->on_passive = {};
}
TEST(SessionCacheScopeTest, EveryExecutionAndResetAttemptInvalidatesBeforeBackendAccess) {
  CredentialContext credentials; auto credential = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), credential};
  auto lease = owner.try_acquire(credential); ASSERT_TRUE(lease);
  auto ticket = lease->cache_token(); ASSERT_TRUE(ticket);
  observed->on_execution = [&](std::string_view, std::span<const QueryParameter>, rs::util::Deadline, bool) {
    EXPECT_FALSE(ticket->is_current()); EXPECT_FALSE(lease->accepts_cache(*ticket));
  };
  ASSERT_TRUE(lease->execute_query("DDL or session mutation", rs::util::Deadline::max()));
  auto old = *ticket; ticket = lease->cache_token(); ASSERT_TRUE(ticket); EXPECT_FALSE(old.is_current());
  ASSERT_TRUE(lease->execute_prepared("prepared mutation", std::span<const QueryParameter>{}, rs::util::Deadline::max()));
  EXPECT_FALSE(ticket->is_current()); ticket = lease->cache_token(); ASSERT_TRUE(ticket);
  observed->on_reset = [&] { EXPECT_FALSE(ticket->is_current()); };
  ASSERT_TRUE(lease->reset_session(rs::util::Deadline::max())); EXPECT_FALSE(ticket->is_current());
  auto fresh = lease->cache_token(); ASSERT_TRUE(fresh); EXPECT_TRUE(fresh->is_current());
  EXPECT_FALSE(lease->accepts_cache(old)); EXPECT_FALSE(lease->accepts_cache(*ticket));
  observed->on_execution = {}; observed->on_reset = {};
  lease->retire(); EXPECT_FALSE(fresh->is_current()); EXPECT_FALSE(lease->cache_token());
}
TEST(SessionCacheScopeTest, FailedUnsupportedExpiredAndThrowingOperationsCannotPreserveTokens) {
  for (int mode = 0; mode < 6; ++mode) {
    CredentialContext credentials; auto credential = credentials.publish_authenticated();
    auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), credential};
    auto lease = owner.try_acquire(credential); ASSERT_TRUE(lease); auto ticket = lease->cache_token(); ASSERT_TRUE(ticket);
    if (mode == 0) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "recoverable query failure"};
      error.session_state = SessionState::Idle; error.disposition = SessionDisposition::Reusable;
      observed->execution_result = BackendResult<QueryResult>{error};
      EXPECT_FALSE(lease->execute_query("invalid", rs::util::Deadline::max())); EXPECT_TRUE(*lease);
    } else if (mode == 1) {
      observed->execution_exception = 1;
      EXPECT_THROW(lease->execute_query("throw", rs::util::Deadline::max()), std::bad_alloc);
    } else {
      observed->missing_reset = mode == 2; observed->reset_mode = mode == 4 ? 1 : mode == 5 ? 3 : 0;
      EXPECT_FALSE(lease->reset_session(mode == 3 ? rs::util::Deadline::min() : rs::util::Deadline::max()));
    }
    EXPECT_FALSE(ticket->is_current()); EXPECT_FALSE(lease->accepts_cache(*ticket));
    if (mode == 0) { auto fresh = lease->cache_token(); ASSERT_TRUE(fresh); EXPECT_TRUE(fresh->is_current()); }
    else { EXPECT_FALSE(lease->cache_token()); }
  }
}
TEST(SessionCacheScopeTest, PassiveNonIdleAndThrowingEligibilityDeniesWithoutInterruptingBorrower) {
  CredentialContext credentials; auto credential = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), credential};
  auto lease = owner.try_acquire(credential); ASSERT_TRUE(lease);
  for (const auto state : {SessionState::Transaction, SessionState::FailedTransaction, SessionState::Unknown, SessionState::Disconnected}) {
    auto ticket = lease->cache_token(); ASSERT_TRUE(ticket);
    observed->passive_state = state; EXPECT_FALSE(lease->cache_token()); EXPECT_FALSE(ticket->is_current());
    EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects); observed->passive_state.reset();
  }
  for (const int mode : {8, 11}) {
    auto ticket = lease->cache_token(); ASSERT_TRUE(ticket); observed->reset_mode = mode;
    EXPECT_FALSE(lease->cache_token()); EXPECT_FALSE(ticket->is_current());
    EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects); observed->reset_mode = 0;
  }
}
TEST(SessionCacheScopeTest, MovesPreserveOriginWhileDestinationAndOwnerCloseInvalidate) {
  CredentialContext credentials; auto credential = credentials.publish_authenticated();
  auto first = std::make_shared<Observed>(); auto second = std::make_shared<Observed>();
  SessionOwner owner1{std::make_unique<FakeSession>(first), credential};
  SessionOwner owner2{std::make_unique<FakeSession>(second), credential};
  auto lease1 = owner1.try_acquire(credential); auto lease2 = owner2.try_acquire(credential);
  ASSERT_TRUE(lease1); ASSERT_TRUE(lease2); auto ticket1 = lease1->cache_token(); auto ticket2 = lease2->cache_token();
  ASSERT_TRUE(ticket1); ASSERT_TRUE(ticket2);
  SessionOwner moved_owner{std::move(owner1)}; SessionLease moved{std::move(*lease1)};
  EXPECT_TRUE(ticket1->is_current()); EXPECT_TRUE(moved.accepts_cache(*ticket1));
  EXPECT_FALSE(lease1->cache_token()); EXPECT_FALSE(lease1->accepts_cache(*ticket1));
  *lease2 = std::move(moved); EXPECT_FALSE(ticket2->is_current()); EXPECT_TRUE(lease2->accepts_cache(*ticket1));
  auto& same = *lease2; same = std::move(*lease2); EXPECT_TRUE(lease2->accepts_cache(*ticket1));
  // Replacing the owner closes its old scope without interrupting the lease.
  auto idle = owner_for(std::make_shared<Observed>()); moved_owner = std::move(idle);
  EXPECT_FALSE(ticket1->is_current()); EXPECT_FALSE(lease2->cache_token()); EXPECT_TRUE(*lease2);
  ASSERT_TRUE(lease2->execute_query("active borrower", rs::util::Deadline::max()));
}
TEST(SessionCacheScopeTest, CredentialRevokeRotateDestroyAndExpiryInvalidateWithoutDisconnect) {
  for (int mode = 0; mode < 4; ++mode) {
    auto credentials = std::make_unique<CredentialContext>();
    const auto expiry = rs::util::make_deadline(std::chrono::seconds(1));
    auto credential = credentials->publish_authenticated(mode == 3 ? std::optional{expiry} : std::nullopt);
    auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), credential};
    auto lease = owner.try_acquire(credential); ASSERT_TRUE(lease); auto ticket = lease->cache_token(); ASSERT_TRUE(ticket);
    if (mode == 0) credentials->revoke();
    if (mode == 1) { (void)credentials->publish_authenticated(); }
    if (mode == 2) credentials.reset();
    if (mode == 3) std::this_thread::sleep_until(expiry);
    EXPECT_FALSE(ticket->is_current()); EXPECT_FALSE(lease->accepts_cache(*ticket)); EXPECT_FALSE(lease->cache_token());
    EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects);
    ASSERT_TRUE(lease->execute_query("same borrower", rs::util::Deadline::max()));
  }
}
TEST(SessionCacheScopeTest, FailedGenerationAllocationLeavesAbsentScopeAndCanRecover) {
  cache_allocations = 0; fail_cache_generation = false; null_cache_generation = false;
  CredentialContext credentials; auto credential = credentials.publish_authenticated(); auto observed = std::make_shared<Observed>();
  auto owner = detail::SessionOwnershipTestAccess::make(std::make_unique<FakeSession>(observed), credential, &cache_factory);
  auto lease = owner.try_acquire(credential); ASSERT_TRUE(lease); auto ticket = lease->cache_token(); ASSERT_TRUE(ticket);
  auto again = lease->cache_token(); ASSERT_TRUE(again); EXPECT_EQ(1, cache_allocations);
  ASSERT_TRUE(lease->execute_query("invalidate", rs::util::Deadline::max())); EXPECT_FALSE(ticket->is_current());
  fail_cache_generation = true; EXPECT_THROW(lease->cache_token(), std::bad_alloc);
  EXPECT_FALSE(ticket->is_current()); EXPECT_EQ(0, observed->disconnects); EXPECT_EQ(1, observed->queries);
  fail_cache_generation = false; null_cache_generation = true; EXPECT_THROW(lease->cache_token(), std::bad_alloc);
  EXPECT_FALSE(again->is_current()); EXPECT_TRUE(*lease); null_cache_generation = false;
  auto fresh = lease->cache_token(); ASSERT_TRUE(fresh); EXPECT_TRUE(fresh->is_current()); EXPECT_EQ(4, cache_allocations);
}
TEST(SessionCacheScopeTest, OwnerClosureDuringUnlockedEligibilityCannotPublishScope) {
  CredentialContext credentials; auto credential = credentials.publish_authenticated(); auto observed = std::make_shared<Observed>();
  auto owner = std::make_unique<SessionOwner>(std::make_unique<FakeSession>(observed), credential);
  auto lease = owner->try_acquire(credential); ASSERT_TRUE(lease);
  auto ticket = lease->cache_token(); ASSERT_TRUE(ticket);
  std::barrier entered(2); std::barrier resume(2);
  observed->on_passive = [&] { entered.arrive_and_wait(); resume.arrive_and_wait(); };
  std::optional<SessionCacheToken> raced;
  std::thread minting([&] { raced = lease->cache_token(); });
  entered.arrive_and_wait(); owner.reset(); resume.arrive_and_wait(); minting.join();
  EXPECT_FALSE(raced); EXPECT_FALSE(ticket->is_current()); EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects);
  observed->on_passive = {};
}
TEST(SessionCacheScopeTest, ConcurrentValidationCannotResurrectInvalidatedScopeOrKeepSessionAlive) {
  std::optional<SessionCacheToken> orphan;
  for (int iteration = 0; iteration < 50; ++iteration) {
    CredentialContext credentials; auto credential = credentials.publish_authenticated(); auto observed = std::make_shared<Observed>();
    SessionOwner owner{std::make_unique<FakeSession>(observed), credential}; auto lease = owner.try_acquire(credential); ASSERT_TRUE(lease);
    auto ticket = lease->cache_token(); ASSERT_TRUE(ticket); std::barrier start(3);
    std::thread checking([&] { start.arrive_and_wait(); for (int read = 0; read < 500; ++read) (void)ticket->is_current(); });
    std::thread invalidating([&] { start.arrive_and_wait(); (void)lease->execute_query("invalidate", rs::util::Deadline::max()); });
    start.arrive_and_wait(); checking.join(); invalidating.join(); EXPECT_FALSE(ticket->is_current());
    auto fresh = lease->cache_token(); ASSERT_TRUE(fresh); EXPECT_TRUE(fresh->is_current()); EXPECT_FALSE(lease->accepts_cache(*ticket));
    orphan = *fresh; lease->retire(); retired_once(*observed); EXPECT_FALSE(orphan->is_current());
  }
  ASSERT_TRUE(orphan); EXPECT_FALSE(orphan->is_current());
}
} // namespace

namespace {
TEST(SessionCacheScopeTest, ValidationRacingOwnerCloseCredentialRotationAndRetirementIsSafe) {
  for (int mode = 0; mode < 3; ++mode) {
    for (int iteration = 0; iteration < 15; ++iteration) {
      CredentialContext credentials; auto credential = credentials.publish_authenticated(); auto observed = std::make_shared<Observed>();
      auto owner = std::make_unique<SessionOwner>(std::make_unique<FakeSession>(observed), credential);
      auto lease = owner->try_acquire(credential); ASSERT_TRUE(lease); auto ticket = lease->cache_token(); ASSERT_TRUE(ticket);
      std::barrier start(3);
      std::thread checking([&] { start.arrive_and_wait(); for (int read = 0; read < 500; ++read) (void)ticket->is_current(); });
      std::thread invalidating([&] {
        start.arrive_and_wait();
        if (mode == 0) owner.reset();
        if (mode == 1) { (void)credentials.publish_authenticated(); }
        if (mode == 2) lease->retire();
      });
      start.arrive_and_wait(); checking.join(); invalidating.join(); EXPECT_FALSE(ticket->is_current());
      if (mode != 2) { EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects); }
      lease->retire(); retired_once(*observed);
    }
  }
}

TEST(SessionReturnTest, ResetConsumesBorrowAndAllowsExclusiveSameOwnerCheckout) {
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  auto old_scope = lease->cache_token(); ASSERT_TRUE(old_scope);
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  observed->on_reset = [&] {
    EXPECT_FALSE(old_scope->is_current()); EXPECT_FALSE(owner.try_acquire(token));
  };
  auto result = lease->return_reusable(deadline); ASSERT_TRUE(result);
  EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
  EXPECT_FALSE(*lease); EXPECT_FALSE(old_scope->is_current()); EXPECT_EQ(deadline, observed->reset_deadline);
  EXPECT_EQ(1, observed->resets); EXPECT_EQ(0, observed->health); EXPECT_EQ(0, observed->disconnects);
  auto next = owner.try_acquire(token); ASSERT_TRUE(next); EXPECT_FALSE(owner.try_acquire(token));
  lease->retire(); lease.reset(); // Detached old lease cannot retire its successor.
  ASSERT_TRUE(*next); auto new_scope = next->cache_token(); ASSERT_TRUE(new_scope);
  EXPECT_FALSE(next->accepts_cache(*old_scope)); EXPECT_TRUE(new_scope->is_current());
  ASSERT_TRUE(next->execute_query("new borrower", deadline));
  observed->on_reset = {}; ASSERT_TRUE(next->return_reusable(deadline));
  auto last = owner.try_acquire(token); ASSERT_TRUE(last); last.reset();
  EXPECT_EQ(2, observed->resets); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
}
TEST(SessionReturnTest, UnboundClosedRevokedRotatedExpiredAndOrphanedCannotReturn) {
  for (int mode = 0; mode < 6; ++mode) {
    SCOPED_TRACE(mode); auto credentials = std::make_unique<CredentialContext>();
    auto token = credentials->publish_authenticated(); auto observed = std::make_shared<Observed>();
    auto owner = mode == 0 ? std::make_unique<SessionOwner>(std::make_unique<FakeSession>(observed)) :
        std::make_unique<SessionOwner>(std::make_unique<FakeSession>(observed), token);
    auto lease = mode == 0 ? owner->try_acquire() : owner->try_acquire(token); ASSERT_TRUE(lease);
    if (mode == 1) owner.reset();
    if (mode == 2) credentials->revoke();
    if (mode == 3) (void)credentials->publish_authenticated();
    if (mode == 4) credentials.reset();
    if (mode == 5) { // A token already expired cannot be admitted at all.
      lease->retire(); token = credentials->publish_authenticated(rs::util::Deadline::min());
      SessionOwner expired{std::make_unique<FakeSession>(observed), token}; EXPECT_FALSE(expired.try_acquire(token));
      continue;
    }
    auto result = lease->return_reusable(rs::util::Deadline::max()); EXPECT_FALSE(result); EXPECT_FALSE(*lease);
    EXPECT_EQ(BackendErrorClass::Authentication, result.backend_error().error_class);
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    retired_once(*observed);
  }
}
TEST(SessionReturnTest, AllResetFailuresRemainTerminal) {
  for (int mode = 0; mode < 13; ++mode) {
    SCOPED_TRACE(mode); CredentialContext credentials; auto token = credentials.publish_authenticated();
    auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), token};
    auto lease = owner.try_acquire(token); ASSERT_TRUE(lease); auto scope = lease->cache_token(); ASSERT_TRUE(scope);
    observed->reset_mode = mode;
    if (mode == 0) observed->missing_reset = true;
    if (mode == 12) observed->reset_snapshot = SessionSnapshot{SessionState::Unknown, SessionDisposition::Reusable};
    auto deadline = mode == 10 ? rs::util::Deadline::min() : rs::util::Deadline::max();
    auto result = lease->return_reusable(deadline); EXPECT_FALSE(result); EXPECT_FALSE(*lease);
    EXPECT_FALSE(scope->is_current()); EXPECT_FALSE(owner.try_acquire(token));
    EXPECT_EQ(BackendOperation::ResetSession, result.backend_error().operation);
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    EXPECT_FALSE(result.backend_error().retry_safe); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
  }
}
TEST(SessionReturnTest, ClosureRotationAndExpiryDuringResetRetireBeforePublication) {
  for (int mode = 0; mode < 4; ++mode) {
    SCOPED_TRACE(mode); CredentialContext credentials;
    const auto expiry = rs::util::make_deadline(std::chrono::seconds(1));
    auto token = credentials.publish_authenticated(mode == 3 ? std::optional{expiry} : std::nullopt);
    auto observed = std::make_shared<Observed>();
    auto owner = std::make_unique<SessionOwner>(std::make_unique<FakeSession>(observed), token);
    auto lease = owner->try_acquire(token); ASSERT_TRUE(lease);
    observed->on_reset = [&] {
      if (mode == 0) owner.reset();
      if (mode == 1) credentials.revoke();
      if (mode == 2) (void)credentials.publish_authenticated();
      if (mode == 3) std::this_thread::sleep_until(expiry);
      EXPECT_EQ(0, observed->disconnects);
    };
    auto result = lease->return_reusable(rs::util::Deadline::max()); EXPECT_FALSE(result); EXPECT_FALSE(*lease);
    EXPECT_EQ(1, observed->resets); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
    if (owner) { EXPECT_FALSE(owner->try_acquire(token)); }
  }
}
TEST(SessionReturnTest, LateResetAndRepeatedReturnCannotGrantReuse) {
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  auto deadline = rs::util::make_deadline(std::chrono::milliseconds(50));
  observed->on_reset = [&] { std::this_thread::sleep_until(deadline); };
  auto late = lease->return_reusable(deadline); ASSERT_FALSE(late);
  EXPECT_EQ(BackendErrorClass::Timeout, late.backend_error().error_class);
  auto repeated = lease->return_reusable(rs::util::Deadline::max()); EXPECT_FALSE(repeated);
  EXPECT_EQ(BackendErrorClass::NotConnected, repeated.backend_error().error_class);
  EXPECT_FALSE(owner.try_acquire(token)); EXPECT_EQ(1, observed->resets); EXPECT_EQ(1, observed->disconnects);
}
TEST(SessionReturnTest, CheckoutAndCredentialRotationRaceNeverDuplicateOrRebind) {
  for (int round = 0; round < 30; ++round) {
    CredentialContext credentials; auto token = credentials.publish_authenticated();
    auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), token};
    auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
    std::barrier start(3); std::optional<SessionLease> next;
    std::thread checkout([&] { start.arrive_and_wait(); next = owner.try_acquire(token); });
    std::thread rotation([&] { start.arrive_and_wait(); (void)credentials.publish_authenticated(); });
    start.arrive_and_wait(); auto result = lease->return_reusable(rs::util::Deadline::max());
    checkout.join(); rotation.join(); EXPECT_FALSE(*lease);
    EXPECT_FALSE(owner.try_acquire(token)); auto fresh = credentials.current_token(); ASSERT_TRUE(fresh);
    EXPECT_FALSE(owner.try_acquire(*fresh)); // New generations cannot reauthenticate an old socket.
    if (next) { ASSERT_TRUE(result); EXPECT_TRUE(*next); next->retire(); }
    EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
  }
}

TEST(SessionReturnTest, ScopesRemintedDuringResetCannotEscapeHandoff) {
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>(); SessionOwner original{std::make_unique<FakeSession>(observed), token};
  auto lease = original.try_acquire(token); ASSERT_TRUE(lease); SessionLease moved{std::move(*lease)};
  SessionOwner owner{std::move(original)}; std::optional<SessionCacheToken> during_reset;
  observed->on_reset = [&] { during_reset = moved.cache_token(); ASSERT_TRUE(during_reset); };
  ASSERT_TRUE(moved.return_reusable(rs::util::Deadline::max())); EXPECT_FALSE(during_reset->is_current());
  auto next = owner.try_acquire(token); ASSERT_TRUE(next); EXPECT_FALSE(next->accepts_cache(*during_reset));
  EXPECT_FALSE(lease->return_reusable(rs::util::Deadline::max()));
  EXPECT_FALSE(moved.return_reusable(rs::util::Deadline::max()));
  ASSERT_TRUE(*next); next->retire(); EXPECT_EQ(1, observed->disconnects);
}
TEST(SessionReturnTest, BlockedResetAllowsCheckoutDenialAndConcurrentRevocation) {
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  std::barrier entered(2), release(2);
  observed->on_reset = [&] { entered.arrive_and_wait(); release.arrive_and_wait(); };
  std::thread other([&] {
    entered.arrive_and_wait(); EXPECT_FALSE(owner.try_acquire(token)); credentials.revoke();
    EXPECT_EQ(0, observed->disconnects); release.arrive_and_wait();
  });
  auto result = lease->return_reusable(rs::util::Deadline::max()); other.join(); EXPECT_FALSE(result);
  EXPECT_FALSE(owner.try_acquire(token)); EXPECT_EQ(1, observed->resets); EXPECT_EQ(1, observed->disconnects);
}

std::atomic<rs::util::Clock::rep> policy_ticks{};
rs::util::Deadline policy_now() noexcept { return rs::util::Deadline{rs::util::Clock::duration{policy_ticks.load()}}; }
void policy_time(std::chrono::seconds time) { policy_ticks = std::chrono::duration_cast<rs::util::Clock::duration>(time).count(); }
SessionOwner policy_owner(std::shared_ptr<Observed> observed, CredentialToken token, SessionReusePolicy policy) {
  return detail::SessionOwnershipTestAccess::with_policy(std::make_unique<FakeSession>(observed), token, policy, &policy_now);
}
TEST(SessionReusePolicyTest, RejectsUnboundedLifetimeAndNonpositiveIdleLimits) {
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  for (auto policy : {SessionReusePolicy{rs::util::Deadline::max(), std::chrono::seconds(1)},
       SessionReusePolicy{rs::util::Deadline::min(), rs::util::Clock::duration::zero()},
       SessionReusePolicy{rs::util::Deadline::min(), -std::chrono::seconds(1)}}) {
    auto observed = std::make_shared<Observed>();
    EXPECT_THROW((SessionOwner{std::make_unique<FakeSession>(observed), token, policy}), std::invalid_argument);
    EXPECT_EQ(1, observed->destructions); EXPECT_EQ(0, observed->health); EXPECT_EQ(0, observed->resets);
  }
}
TEST(SessionReusePolicyTest, ExactLifetimeAndIdleEqualityExpireOnlyForMatchingToken) {
  for (bool idle : {false, true}) {
    policy_time(std::chrono::seconds(0)); CredentialContext credentials, foreign;
    auto wrong_generation = credentials.publish_authenticated(); auto token = credentials.publish_authenticated();
    auto observed = std::make_shared<Observed>();
    auto owner = policy_owner(observed, token, {rs::util::Deadline{} + std::chrono::seconds(idle ? 100 : 10), std::chrono::seconds(10)});
    policy_time(std::chrono::seconds(10));
    EXPECT_FALSE(owner.try_acquire()); EXPECT_FALSE(owner.try_acquire(foreign.publish_authenticated()));
    EXPECT_FALSE(owner.try_acquire(wrong_generation));
    EXPECT_EQ(0, observed->disconnects); EXPECT_FALSE(owner.try_acquire(token)); retired_once(*observed);
  }
}
TEST(SessionReusePolicyTest, ReturnedIdleWindowStartsAtPublicationAndDenialsCannotExtendIt) {
  policy_time(std::chrono::seconds(0)); CredentialContext credentials, foreign; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  auto owner = policy_owner(observed, token, {rs::util::Deadline{} + std::chrono::seconds(100), std::chrono::seconds(10)});
  policy_time(std::chrono::seconds(9)); auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  policy_time(std::chrono::seconds(30)); // Active borrow ignores idle timeout.
  ASSERT_TRUE(lease->return_reusable(rs::util::Deadline::max()));
  policy_time(std::chrono::seconds(39)); auto next = owner.try_acquire(token); ASSERT_TRUE(next);
  observed->on_reset = [] { policy_time(std::chrono::seconds(45)); };
  ASSERT_TRUE(next->return_reusable(rs::util::Deadline::max()));
  auto wrong = foreign.publish_authenticated();
  policy_time(std::chrono::seconds(54)); EXPECT_FALSE(owner.try_acquire(wrong)); EXPECT_FALSE(owner.try_acquire());
  policy_time(std::chrono::seconds(55)); EXPECT_FALSE(owner.try_acquire(token));
  EXPECT_EQ(2, observed->resets); EXPECT_EQ(0, observed->health); EXPECT_EQ(1, observed->disconnects);
}
TEST(SessionReusePolicyTest, LifetimeExpiryInvalidatesScopesWithoutInterruptingBorrow) {
  for (bool observe_cache : {false, true}) {
    policy_time(std::chrono::seconds(0)); CredentialContext credentials; auto token = credentials.publish_authenticated();
    auto observed = std::make_shared<Observed>();
    auto owner = policy_owner(observed, token, {rs::util::Deadline{} + std::chrono::seconds(10), std::chrono::seconds(1)});
    auto lease = owner.try_acquire(token); ASSERT_TRUE(lease); auto scope = lease->cache_token(); ASSERT_TRUE(scope);
    policy_time(std::chrono::seconds(10));
    if (observe_cache) { EXPECT_FALSE(scope->is_current()); EXPECT_FALSE(lease->cache_token()); }
    ASSERT_TRUE(lease->execute_query("existing borrower", rs::util::Deadline::max()));
    EXPECT_FALSE(scope->is_current()); EXPECT_EQ(0, observed->disconnects);
    auto result = lease->return_reusable(rs::util::Deadline::max()); ASSERT_FALSE(result);
    EXPECT_EQ(BackendErrorClass::Timeout, result.backend_error().error_class); EXPECT_FALSE(*lease);
    retired_once(*observed); EXPECT_EQ(1, observed->queries); EXPECT_FALSE(owner.try_acquire(token));
  }
}
TEST(SessionReusePolicyTest, LifetimeCrossingDuringResetPreventsPublication) {
  policy_time(std::chrono::seconds(0)); CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  auto owner = policy_owner(observed, token, {rs::util::Deadline{} + std::chrono::seconds(10), std::chrono::seconds(1)});
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  observed->on_reset = [&] { policy_time(std::chrono::seconds(10)); EXPECT_EQ(0, observed->disconnects); };
  auto result = lease->return_reusable(rs::util::Deadline::max()); ASSERT_FALSE(result);
  EXPECT_EQ(BackendErrorClass::Timeout, result.backend_error().error_class);
  EXPECT_FALSE(owner.try_acquire(token)); EXPECT_EQ(1, observed->resets); EXPECT_EQ(1, observed->disconnects);
}
TEST(SessionReusePolicyTest, HugeIdleLimitDoesNotOverflowAndReturnNeverRenewsLifetime) {
  policy_time(std::chrono::seconds(0)); CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  auto owner = policy_owner(observed, token, {rs::util::Deadline{} + std::chrono::seconds(100), rs::util::Clock::duration::max()});
  policy_time(std::chrono::seconds(50)); auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  ASSERT_TRUE(lease->return_reusable(rs::util::Deadline::max()));
  policy_time(std::chrono::seconds(99)); auto next = owner.try_acquire(token); ASSERT_TRUE(next);
  ASSERT_TRUE(next->return_reusable(rs::util::Deadline::max()));
  policy_time(std::chrono::seconds(100)); EXPECT_FALSE(owner.try_acquire(token));
  EXPECT_EQ(2, observed->resets); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
}

TEST(SessionReusePolicyTest, PassiveCacheCheckCrossingLifetimeAndMovesPreservePolicy) {
  policy_time(std::chrono::seconds(0)); CredentialContext credentials; auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>();
  auto original = policy_owner(observed, token, {rs::util::Deadline{} + std::chrono::seconds(10), std::chrono::seconds(1)});
  SessionOwner owner{std::move(original)}; auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  SessionLease moved{std::move(*lease)};
  observed->on_passive = [] { policy_time(std::chrono::seconds(10)); };
  EXPECT_FALSE(moved.cache_token()); EXPECT_EQ(0, observed->disconnects);
  ASSERT_TRUE(moved.reset_session(rs::util::Deadline::max())); // Explicit cleanup remains available for active borrow.
  EXPECT_FALSE(moved.return_reusable(rs::util::Deadline::max())); EXPECT_FALSE(owner.try_acquire(token));
  EXPECT_EQ(1, observed->resets); EXPECT_EQ(1, observed->disconnects);
}
TEST(SessionReusePolicyTest, ConcurrentExpiryAndCheckoutCannotDuplicatePhysicalOwnership) {
  for (int i = 0; i < 30; ++i) {
    policy_time(std::chrono::seconds(0)); CredentialContext credentials; auto token = credentials.publish_authenticated();
    auto observed = std::make_shared<Observed>();
    auto owner = policy_owner(observed, token, {rs::util::Deadline{} + std::chrono::seconds(10), std::chrono::seconds(10)});
    std::barrier start(3); std::optional<SessionLease> lease;
    std::thread checkout([&] { start.arrive_and_wait(); lease = owner.try_acquire(token); });
    std::thread expiry([&] { start.arrive_and_wait(); policy_time(std::chrono::seconds(10)); });
    start.arrive_and_wait(); checkout.join(); expiry.join(); EXPECT_FALSE(owner.try_acquire(token));
    if (lease) { EXPECT_EQ(0, observed->disconnects); ASSERT_TRUE(lease->execute_query("active borrower", rs::util::Deadline::max())); lease->retire(); }
    retired_once(*observed);
  }
}

TEST(SessionHealthAdmissionTest, ProbeDeliversExclusiveLeaseWithOriginalDeadlineAndNoReset) {
  CredentialContext credentials; auto token = credentials.publish_authenticated(); auto observed = std::make_shared<Observed>();
  SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  observed->on_health = [&] { EXPECT_FALSE(owner.try_acquire(token)); };
  auto result = owner.acquire_healthy(token, deadline); ASSERT_TRUE(result); EXPECT_TRUE(*result);
  EXPECT_EQ(deadline, observed->health_deadline); EXPECT_EQ(1, observed->health); EXPECT_EQ(0, observed->resets);
  EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
  EXPECT_FALSE(owner.try_acquire(token)); auto scope = result->cache_token(); ASSERT_TRUE(scope);
  ASSERT_TRUE(result->execute_query("checked borrower", deadline));
  ASSERT_TRUE(result->return_reusable(deadline)); EXPECT_FALSE(*result); EXPECT_FALSE(scope->is_current());
  auto next = owner.acquire_healthy(token, deadline); ASSERT_TRUE(next); EXPECT_EQ(2, observed->health);
  result->retire(); EXPECT_TRUE(*next); next->retire(); EXPECT_EQ(1, observed->disconnects);
}
TEST(SessionHealthAdmissionTest, ForeignWrongBusyUnboundAndExpiredAdmissionPerformNoProbe) {
  CredentialContext credentials, foreign; auto older = credentials.publish_authenticated(); auto token = credentials.publish_authenticated();
  auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  for (auto denied : {older, foreign.publish_authenticated()}) {
    auto result = owner.acquire_healthy(denied, rs::util::Deadline::max()); EXPECT_FALSE(result);
    EXPECT_EQ(BackendOperation::CheckHealth, result.backend_error().operation);
  }
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  EXPECT_FALSE(owner.acquire_healthy(token, rs::util::Deadline::max())); EXPECT_EQ(0, observed->disconnects);
  EXPECT_EQ(0, observed->health); lease->retire();
  auto unbound_observed = std::make_shared<Observed>(); auto unbound = owner_for(unbound_observed);
  EXPECT_FALSE(unbound.acquire_healthy(token, rs::util::Deadline::max())); EXPECT_EQ(0, unbound_observed->disconnects);
  auto unbound_lease = unbound.try_acquire(); ASSERT_TRUE(unbound_lease); unbound_lease->retire();
  policy_time(std::chrono::seconds(0)); auto expired_observed = std::make_shared<Observed>();
  auto expired = policy_owner(expired_observed, token, {rs::util::Deadline{}, std::chrono::seconds(1)});
  EXPECT_FALSE(expired.acquire_healthy(token, rs::util::Deadline::max())); retired_once(*expired_observed);
}
TEST(SessionHealthAdmissionTest, MissingExpiredMalformedPassiveAndThrownProbesRetire) {
  for (int mode = 0; mode < 16; ++mode) {
    SCOPED_TRACE(mode); CredentialContext credentials; auto token = credentials.publish_authenticated();
    auto observed = std::make_shared<Observed>(); SessionOwner owner{std::make_unique<FakeSession>(observed), token};
    if (mode == 0) observed->missing_health = true;
    if (mode >= 1 && mode <= 5) observed->health_mode = mode;
    if (mode == 6) observed->health_snapshot = SessionSnapshot{SessionState::Unknown, SessionDisposition::Reusable};
    if (mode == 7) observed->health_snapshot = SessionSnapshot{SessionState::Idle, SessionDisposition::ResetRequired};
    if (mode == 8) observed->health_snapshot = SessionSnapshot{SessionState::Transaction, SessionDisposition::Reusable};
    if (mode == 9) observed->passive_state = SessionState::Transaction;
    if (mode == 10) observed->reset_mode = 8; // Passive state throws.
    if (mode == 11) observed->reset_mode = 11; // Passive connectivity throws.
    if (mode == 13) observed->health_snapshot = SessionSnapshot{SessionState::Idle, SessionDisposition::Retire};
    if (mode == 14) observed->health_snapshot = SessionSnapshot{SessionState::FailedTransaction, SessionDisposition::Reusable};
    if (mode == 15) observed->health_snapshot = SessionSnapshot{SessionState::Disconnected, SessionDisposition::Reusable};
    auto result = owner.acquire_healthy(token, mode == 12 ? rs::util::Deadline::min() : rs::util::Deadline::max());
    ASSERT_FALSE(result); EXPECT_FALSE(owner.try_acquire(token));
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    EXPECT_EQ(BackendOperation::CheckHealth, result.backend_error().operation); EXPECT_FALSE(result.backend_error().retry_safe);
    EXPECT_EQ(std::string::npos, result.error_message().find("SECRET"));
    if (mode == 1) { EXPECT_EQ("XX001", result.backend_error().native_state); EXPECT_EQ(73, result.backend_error().native_code); EXPECT_EQ("owned health error", result.error_message()); }
    if (mode == 2) { EXPECT_EQ(BackendErrorClass::AllocationFailure, result.backend_error().error_class); }
    if (mode == 12) { EXPECT_EQ(BackendErrorClass::Timeout, result.backend_error().error_class); }
    EXPECT_EQ(0, observed->resets); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
  }
}
TEST(SessionHealthAdmissionTest, ClosureRotationRevocationAndLifetimeCrossingDuringProbeFailClosed) {
  for (int mode = 0; mode < 4; ++mode) {
    SCOPED_TRACE(mode); policy_time(std::chrono::seconds(0)); CredentialContext credentials; auto token = credentials.publish_authenticated();
    auto observed = std::make_shared<Observed>();
    auto owner = std::make_unique<SessionOwner>(policy_owner(observed, token,
        {rs::util::Deadline{} + std::chrono::seconds(10), std::chrono::seconds(1)}));
    observed->on_health = [&] {
      if (mode == 0) owner.reset();
      if (mode == 1) credentials.revoke();
      if (mode == 2) (void)credentials.publish_authenticated();
      if (mode == 3) policy_time(std::chrono::seconds(10));
      EXPECT_EQ(0, observed->disconnects);
    };
    auto result = owner->acquire_healthy(token, rs::util::Deadline::max()); ASSERT_FALSE(result);
    if (owner) { EXPECT_FALSE(owner->try_acquire(token)); }
    EXPECT_EQ(1, observed->health); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
  }
}
TEST(SessionHealthAdmissionTest, DeadlineCrossingAndConcurrentCheckoutCannotDeliverUncheckedLease) {
  for (bool expire : {false, true}) {
    CredentialContext credentials; auto token = credentials.publish_authenticated(); auto observed = std::make_shared<Observed>();
    SessionOwner owner{std::make_unique<FakeSession>(observed), token}; std::barrier entered(2), release(2);
    const auto deadline = rs::util::make_deadline(std::chrono::milliseconds(50));
    observed->on_health = [&] { entered.arrive_and_wait(); release.arrive_and_wait(); };
    std::thread other([&] {
      entered.arrive_and_wait(); EXPECT_FALSE(owner.try_acquire(token));
      if (expire) std::this_thread::sleep_until(deadline);
      release.arrive_and_wait();
    });
    auto result = owner.acquire_healthy(token, expire ? deadline : rs::util::Deadline::max()); other.join();
    EXPECT_EQ(!expire, bool(result));
    if (expire) { EXPECT_EQ(BackendErrorClass::Timeout, result.backend_error().error_class); }
    else { result->retire(); }
    EXPECT_EQ(1, observed->health); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
  }
}

TEST(SessionHealthAdmissionTest, ConcurrentHealthyAdmissionsHaveOnlyOneProbeAndOwningResult) {
  CredentialContext credentials; auto token = credentials.publish_authenticated(); auto observed = std::make_shared<Observed>();
  SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  constexpr int count = 8; std::barrier start(count + 1);
  std::array<std::optional<BackendResult<SessionLease>>, count> results;
  std::array<std::thread, count> threads;
  for (int i = 0; i < count; ++i) threads[i] = std::thread([&, i] { start.arrive_and_wait(); results[i] = owner.acquire_healthy(token, rs::util::Deadline::max()); });
  start.arrive_and_wait(); for (auto& thread : threads) thread.join();
  int winners{};
  for (auto& result : results) {
    ASSERT_TRUE(result);
    if (*result) { ++winners; SessionLease lease = std::move(*result).value(); ASSERT_TRUE(lease); lease.retire(); }
  }
  EXPECT_EQ(1, winners); EXPECT_EQ(1, observed->health); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
}
TEST(SessionHealthAdmissionTest, CredentialExpiryDuringProbeCannotDeliverLease) {
  CredentialContext credentials; const auto expiry = rs::util::make_deadline(std::chrono::seconds(1));
  auto token = credentials.publish_authenticated(expiry); auto observed = std::make_shared<Observed>();
  SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  observed->on_health = [&] { std::this_thread::sleep_until(expiry); EXPECT_EQ(0, observed->disconnects); };
  auto result = owner.acquire_healthy(token, rs::util::Deadline::max()); EXPECT_FALSE(result);
  EXPECT_FALSE(owner.try_acquire(token)); EXPECT_EQ(1, observed->health); EXPECT_EQ(1, observed->disconnects);
}

SessionReusePolicy managed_policy() { return {rs::util::make_deadline(std::chrono::seconds(10)), std::chrono::seconds(5)}; }
std::shared_ptr<Observed> fresh_observed() { auto observed = std::make_shared<Observed>(); observed->initially_connected = false; return observed; }
TEST(ManagedSessionTest, AuthenticationPrecedesAuthorityAndSettingsAreBorrowedUnchanged) {
  auto observed = fresh_observed(); ConnectionSettings settings{}; settings.timeout = std::chrono::seconds(5);
  observed->on_connect = [&](const auto& input) { EXPECT_EQ(&settings, &input); EXPECT_EQ(0, observed->health); };
  auto result = SessionOwner::connect_authenticated(std::make_unique<FakeSession>(observed), settings, managed_policy());
  ASSERT_TRUE(result); EXPECT_EQ(1, observed->connects); EXPECT_EQ(0, observed->health);
  EXPECT_FALSE(result->try_acquire()); auto lease = result->acquire_healthy(rs::util::Deadline::max()); ASSERT_TRUE(lease);
  ASSERT_TRUE(lease->return_reusable(rs::util::Deadline::max()));
  SessionOwner moved = std::move(result).value(); result->revoke_credentials();
  auto next = moved.acquire_healthy(rs::util::Deadline::max()); ASSERT_TRUE(next);
  moved.revoke_credentials(); EXPECT_EQ(0, observed->disconnects);
  ASSERT_TRUE(next->execute_query("surviving borrower", rs::util::Deadline::max()));
  EXPECT_FALSE(next->return_reusable(rs::util::Deadline::max())); EXPECT_FALSE(moved.acquire_healthy(rs::util::Deadline::max()));
  EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
}
TEST(ManagedSessionTest, IdleRevocationAndOwnerDestructionRetireWhileActiveBorrowSurvives) {
  for (bool active : {false, true}) {
    auto observed = fresh_observed(); std::optional<SessionLease> lease;
    {
      auto owner = SessionOwner::connect_authenticated(std::make_unique<FakeSession>(observed), ConnectionSettings{}, managed_policy()); ASSERT_TRUE(owner);
      if (active) { auto result = owner->acquire_healthy(rs::util::Deadline::max()); ASSERT_TRUE(result); lease = std::move(result).value(); }
      else { owner->revoke_credentials(); EXPECT_EQ(1, observed->disconnects); EXPECT_FALSE(owner->acquire_healthy(rs::util::Deadline::max())); }
    }
    if (active) { EXPECT_EQ(0, observed->disconnects); ASSERT_TRUE(lease->execute_query("owner gone", rs::util::Deadline::max())); lease.reset(); }
    EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
  }
}
TEST(ManagedSessionTest, NullDirtyInvalidPolicyAndPreexpiredBoundsNeverAuthenticate) {
  ConnectionSettings settings{};
  EXPECT_FALSE(SessionOwner::connect_authenticated(nullptr, settings, managed_policy()));
  for (int mode = 0; mode < 5; ++mode) {
    auto observed = fresh_observed(); auto policy = managed_policy(); std::optional<rs::util::Deadline> expiry;
    if (mode == 0) observed->initially_connected = true;
    if (mode == 1) policy.retire_at = rs::util::Deadline::min();
    if (mode == 2) policy.max_idle = rs::util::Clock::duration::zero();
    if (mode == 3) expiry = rs::util::Deadline::min();
    if (mode == 4) settings.timeout = std::chrono::milliseconds::zero();
    auto result = SessionOwner::connect_authenticated(std::make_unique<FakeSession>(observed), settings, policy, expiry);
    ASSERT_FALSE(result); EXPECT_EQ(0, observed->connects); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
    EXPECT_EQ(BackendOperation::Connect, result.backend_error().operation);
  }
}
TEST(ManagedSessionTest, FailedMalformedAndThrownAuthenticationNeverPublishesOwner) {
  for (int mode = 1; mode < 10; ++mode) {
    SCOPED_TRACE(mode); auto observed = fresh_observed(); observed->connect_mode = mode;
    if (mode == 6) observed->connect_snapshot = SessionSnapshot{};
    if (mode == 7) observed->connect_snapshot = SessionSnapshot{SessionState::Idle, SessionDisposition::ResetRequired};
    if (mode == 8) observed->on_connect = [&](const auto&) { observed->reset_mode = 8; };
    if (mode == 9) observed->on_connect = [&](const auto&) { observed->reset_mode = 11; };
    auto result = SessionOwner::connect_authenticated(std::make_unique<FakeSession>(observed), ConnectionSettings{}, managed_policy());
    ASSERT_FALSE(result); EXPECT_EQ(1, observed->connects); EXPECT_EQ(0, observed->health); EXPECT_EQ(1, observed->disconnects);
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    EXPECT_FALSE(result.backend_error().retry_safe); EXPECT_EQ(BackendOperation::Connect, result.backend_error().operation);
    EXPECT_EQ(std::string::npos, result.error_message().find("SECRET"));
    if (mode == 1) { EXPECT_EQ("28P01", result.backend_error().native_state); EXPECT_EQ(23, result.backend_error().native_code); }
    if (mode == 2) { EXPECT_EQ(BackendErrorClass::AllocationFailure, result.backend_error().error_class); }
  }
}
TEST(ManagedSessionTest, AuthorityAllocationFailureAndNullFactoryRetireAuthenticatedPhysical) {
  for (bool null_factory : {false, true}) {
    auto observed = fresh_observed();
    auto factory = null_factory ? +[]() -> std::unique_ptr<CredentialContext> { return {}; } :
        +[]() -> std::unique_ptr<CredentialContext> { throw std::bad_alloc{}; };
    auto result = detail::SessionOwnershipTestAccess::connect(std::make_unique<FakeSession>(observed), ConnectionSettings{}, managed_policy(), factory);
    ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::AllocationFailure, result.backend_error().error_class);
    EXPECT_EQ(1, observed->connects); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
  }
}
TEST(ManagedSessionTest, CredentialLifetimeAndConnectionDeadlineCrossingPreventPublication) {
  for (bool credential : {false, true}) {
    auto observed = fresh_observed(); ConnectionSettings settings{}; settings.timeout = std::chrono::milliseconds(50);
    const auto expiry = rs::util::make_deadline(std::chrono::milliseconds(50));
    if (credential) settings.timeout = std::chrono::seconds(5);
    observed->on_connect = [&](const auto&) { std::this_thread::sleep_until(expiry + std::chrono::milliseconds(20)); };
    auto result = SessionOwner::connect_authenticated(std::make_unique<FakeSession>(observed), settings, managed_policy(),
        credential ? std::optional{expiry} : std::nullopt);
    ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::Timeout, result.backend_error().error_class);
    EXPECT_EQ(1, observed->connects); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
  }
}
} // namespace

namespace {
template<class Check>
void check_facet_result(SessionLease& lease, int operation, rs::util::Deadline deadline, Check&& check) {
  const std::array<QueryParameterType, 1> types{QueryParameterType::Text};
  if (operation == 0) check(lease.transaction(TransactionAction::Begin, deadline));
  else if (operation == 1) check(lease.set_transaction_isolation(TransactionIsolation::Serializable, deadline));
  else check(lease.describe_statement("SELECT ?", types, deadline));
}
TEST(SessionLeaseFacetsTest, ForwardsInputsAndDeadlineExclusivelyAndInvalidatesCacheBeforeCalls) {
  for (int operation = 0; operation != 3; ++operation) {
    auto observed = std::make_shared<Observed>(); observed->operation_facets = true;
    CredentialContext credentials; auto token = credentials.publish_authenticated();
    SessionOwner owner{std::make_unique<FakeSession>(observed), token};
    auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
    auto scope = lease->cache_token(); ASSERT_TRUE(scope);
    const auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
    int calls = 0;
    const auto during_call = [&](rs::util::Deadline actual) {
      ++calls; EXPECT_EQ(deadline, actual); EXPECT_FALSE(scope->is_current());
      EXPECT_FALSE(owner.try_acquire(token)); // Callback must run outside owner locks.
    };
    observed->on_transaction = [&](TransactionAction action, auto actual) {
      EXPECT_EQ(TransactionAction::Begin, action); during_call(actual);
    };
    observed->on_isolation = [&](TransactionIsolation level, auto actual) {
      EXPECT_EQ(TransactionIsolation::Serializable, level); during_call(actual);
    };
    observed->on_description = [&](auto sql, auto types, auto actual) {
      EXPECT_EQ("SELECT ?", sql); ASSERT_EQ(1u, types.size());
      EXPECT_EQ(QueryParameterType::Text, types[0]); during_call(actual);
    };
    check_facet_result(*lease, operation, deadline, [&](auto result) {
      ASSERT_TRUE(result); EXPECT_NE(SessionDisposition::Retire, result.session_snapshot().disposition);
    });
    EXPECT_EQ(1, calls); EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects);
  }
}
TEST(SessionLeaseFacetsTest, MissingFacetsPreserveBorrowButInvalidateScopeAndEmptyLeaseReturnsNotConnected) {
  for (int operation = 0; operation != 3; ++operation) {
    auto observed = std::make_shared<Observed>();
    CredentialContext credentials; auto token = credentials.publish_authenticated();
    SessionOwner owner{std::make_unique<FakeSession>(observed), token};
    auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
    auto scope = lease->cache_token(); ASSERT_TRUE(scope);
    check_facet_result(*lease, operation, rs::util::Deadline::max(), [&](auto result) {
      ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::Unsupported, result.backend_error().error_class);
      EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
    });
    EXPECT_TRUE(*lease); EXPECT_FALSE(scope->is_current()); EXPECT_EQ(0, observed->disconnects);
    lease->retire();
    check_facet_result(*lease, operation, rs::util::Deadline::max(), [&](auto result) {
      ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::NotConnected, result.backend_error().error_class);
      EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
      EXPECT_EQ(operation == 0 ? BackendOperation::Transaction : operation == 1 ?
          BackendOperation::SetTransactionIsolation : BackendOperation::Describe, result.backend_error().operation);
    });
    retired_once(*observed);
  }
}
TEST(SessionLeaseFacetsTest, OwningNativeErrorsPreserveSameBorrowOrRetireExactlyAsReported) {
  for (int operation = 0; operation != 3; ++operation) {
    for (auto disposition : {SessionDisposition::Reusable, SessionDisposition::ResetRequired, SessionDisposition::Retire}) {
      auto observed = std::make_shared<Observed>(); observed->operation_facets = true;
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owned facet error"};
      error.native_state = "XX123"; error.native_code = 321; error.retry_safe = false;
      error.operation = BackendOperation::CommitTransaction;
      error.session_state = SessionState::FailedTransaction; error.disposition = disposition;
      observed->transaction_result = error; observed->description_result = error;
      auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
      check_facet_result(*lease, operation, rs::util::Deadline::max(), [&](auto result) {
        ASSERT_FALSE(result);
        EXPECT_EQ(disposition != SessionDisposition::Retire, bool(*lease));
        EXPECT_EQ(disposition == SessionDisposition::Retire ? 1 : 0, observed->disconnects);
        lease->retire(); observed->transaction_result.reset(); observed->description_result.reset();
        EXPECT_EQ("owned facet error", result.backend_error().message);
        EXPECT_EQ("XX123", result.backend_error().native_state); EXPECT_EQ(321, result.backend_error().native_code);
        EXPECT_EQ(false, result.backend_error().retry_safe);
        EXPECT_EQ(BackendOperation::CommitTransaction, result.backend_error().operation);
        EXPECT_EQ(disposition, result.session_snapshot().disposition);
      });
      retired_once(*observed);
    }
  }
}
TEST(SessionLeaseFacetsTest, AmbiguousSuccessAndAllExceptionKindsRetireWithoutReplay) {
  for (int operation = 0; operation != 3; ++operation) {
    for (int exception = 0; exception != 4; ++exception) {
      auto observed = std::make_shared<Observed>(); observed->operation_facets = true;
      observed->facet_exception = exception;
      observed->transaction_result = BackendResult<void>{};
      observed->description_result = BackendResult<QueryResult>{QueryResult{}};
      auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
      auto invoke = [&] { check_facet_result(*lease, operation, rs::util::Deadline::max(), [](auto result) {
        EXPECT_TRUE(result); EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
      }); };
      if (exception == 0) invoke();
      else if (exception == 1) { EXPECT_THROW(invoke(), std::bad_alloc); }
      else if (exception == 2) { EXPECT_THROW(invoke(), std::runtime_error); }
      else { EXPECT_THROW(invoke(), int); }
      EXPECT_FALSE(*lease); EXPECT_FALSE(owner.try_acquire()); retired_once(*observed);
    }
  }
}
}

namespace {
TEST(SessionLeaseFacetsTest, MissingFacetPassiveStateAndAccessorExceptionNeverGrantReturn) {
  for (int operation = 0; operation != 3; ++operation) {
    for (auto state : {SessionState::Idle, SessionState::Transaction, SessionState::FailedTransaction,
                       SessionState::Unknown, SessionState::Disconnected}) {
      auto observed = std::make_shared<Observed>();
      auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
      observed->passive_state = state;
      check_facet_result(*lease, operation, rs::util::Deadline::max(), [&](auto result) {
        ASSERT_FALSE(result); EXPECT_EQ(state, result.session_snapshot().state);
        EXPECT_EQ(state == SessionState::Disconnected ? SessionDisposition::Retire :
            state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired,
            result.session_snapshot().disposition);
      });
      EXPECT_EQ(state != SessionState::Disconnected, bool(*lease));
      EXPECT_FALSE(owner.try_acquire()); // No implicit return, even for Idle/Reusable.
      lease->retire(); retired_once(*observed);
    }
    auto observed = std::make_shared<Observed>();
    auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
    observed->reset_mode = 8; // Passive-state accessor throws.
    EXPECT_THROW(check_facet_result(*lease, operation, rs::util::Deadline::max(), [](auto) {}), std::runtime_error);
    EXPECT_FALSE(*lease); retired_once(*observed);
  }
}
TEST(SessionLeaseFacetsTest, DescriptionMetadataSurvivesAutomaticRetirement) {
  auto observed = std::make_shared<Observed>(); observed->operation_facets = true;
  QueryResult metadata; metadata.columns.push_back({"owned schema", std::nullopt});
  observed->description_result = BackendResult<QueryResult>{metadata, {SessionState::Unknown, SessionDisposition::Retire}};
  auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
  auto result = lease->describe_statement("SELECT 1", {}, rs::util::Deadline::max());
  ASSERT_TRUE(result); EXPECT_FALSE(*lease); retired_once(*observed);
  observed->description_result.reset(); metadata.columns.clear();
  ASSERT_EQ(1u, result->columns.size()); EXPECT_EQ("owned schema", result->columns[0].name);
}
}

namespace {
TEST(SessionLeaseObservationTest, OwnedReadAndCatalogConstructionPreserveScopeAndExclusiveBorrow) {
  auto observed = std::make_shared<Observed>(); observed->operation_facets = true; observed->catalog_facet = true;
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  auto scope = lease->cache_token(); ASSERT_TRUE(scope);
  TablesCatalogRequest request; request.schema = "literal schema"; request.table = "pattern%";
  observed->on_catalog = [&](const CatalogRequest& actual) {
    EXPECT_EQ(request.schema, std::get<TablesCatalogRequest>(actual).schema);
    EXPECT_EQ(request.table, std::get<TablesCatalogRequest>(actual).table);
    EXPECT_TRUE(scope->is_current()); EXPECT_FALSE(owner.try_acquire(token));
  };
  observed->on_passive = [&] { EXPECT_FALSE(owner.try_acquire(token)); };
  auto inspection = lease->inspect(); ASSERT_TRUE(inspection);
  EXPECT_TRUE(inspection->connected); EXPECT_EQ(SessionState::Idle, inspection->state);
  EXPECT_EQ("fixture", inspection->server_version); EXPECT_TRUE(inspection->transactions.supported);
  EXPECT_TRUE(inspection->transactions.supports(TransactionIsolation::Serializable));
  EXPECT_TRUE(inspection->has_statement_description_facet); EXPECT_TRUE(inspection->has_catalog_query_facet);
  auto query = lease->catalog_query(request); ASSERT_TRUE(query); EXPECT_EQ("SELECT fixture", *query);
  EXPECT_TRUE(scope->is_current()); EXPECT_EQ(0, observed->queries);
  observed->on_passive = {}; lease->retire(); retired_once(*observed);
  observed->advertised_version.clear(); observed->transactions = {};
  EXPECT_EQ("fixture", inspection->server_version); EXPECT_TRUE(inspection->transactions.supported);
  EXPECT_EQ("SELECT fixture", *query);
}
TEST(SessionLeaseObservationTest, MissingFacetsAndLocalErrorsDoNotClaimSupportOrInvalidateScopes) {
  auto observed = std::make_shared<Observed>();
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  auto scope = lease->cache_token(); ASSERT_TRUE(scope);
  auto inspection = lease->inspect(); ASSERT_TRUE(inspection);
  EXPECT_FALSE(inspection->transactions.supported); EXPECT_FALSE(inspection->has_statement_description_facet);
  EXPECT_FALSE(inspection->has_catalog_query_facet);
  auto missing = lease->catalog_query(TablesCatalogRequest{}); ASSERT_FALSE(missing);
  EXPECT_EQ(BackendErrorClass::Unsupported, missing.backend_error().error_class);
  EXPECT_TRUE(scope->is_current()); EXPECT_TRUE(*lease);
  observed->catalog_facet = true;
  for (auto code : {rs::util::DbErrorCode::InvalidParameter, rs::util::DbErrorCode::UnsupportedFeature}) {
    observed->catalog_result = rs::util::Result<std::string>{code, "owned construction error"};
    auto failure = lease->catalog_query(TablesCatalogRequest{}); ASSERT_FALSE(failure);
    EXPECT_EQ(rs::util::make_error_code(code), failure.error());
    EXPECT_EQ(BackendOperation::BuildCatalogQuery, failure.backend_error().operation);
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), failure.session_snapshot());
    EXPECT_TRUE(scope->is_current()); EXPECT_TRUE(*lease);
    observed->catalog_result.reset(); EXPECT_EQ("owned construction error", failure.error_message());
  }
}
TEST(SessionLeaseObservationTest, PassiveStatesAreConservativeAndDisconnectedReadsRetire) {
  for (bool catalog : {false, true}) {
    for (auto state : {SessionState::Idle, SessionState::Transaction, SessionState::FailedTransaction,
                       SessionState::Unknown, SessionState::Disconnected}) {
      auto observed = std::make_shared<Observed>(); observed->catalog_facet = true;
      auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
      observed->passive_state = state;
      observed->passive_connected = state != SessionState::Disconnected;
      if (catalog) {
        auto result = lease->catalog_query(TablesCatalogRequest{});
        if (state == SessionState::Disconnected) { EXPECT_FALSE(result); EXPECT_EQ(BackendErrorClass::NotConnected, result.backend_error().error_class); }
        else { ASSERT_TRUE(result); EXPECT_EQ(state, result.session_snapshot().state);
          EXPECT_EQ(state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired, result.session_snapshot().disposition); }
      } else {
        auto result = lease->inspect();
        ASSERT_TRUE(result); EXPECT_EQ(state, result->state);
        EXPECT_EQ(state == SessionState::Disconnected ? SessionDisposition::Retire :
            state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired, result.session_snapshot().disposition);
      }
      EXPECT_EQ(state != SessionState::Disconnected, bool(*lease));
      EXPECT_FALSE(owner.try_acquire()); lease->retire(); retired_once(*observed);
    }
  }
}
TEST(SessionLeaseObservationTest, ExceptionsRetireAndEmptyLeasesReturnTypedNotConnected) {
  for (bool catalog : {false, true}) {
    for (int exception = 1; exception != 4; ++exception) {
      auto observed = std::make_shared<Observed>(); observed->catalog_facet = true;
      auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
      observed->observation_exception = exception;
      auto invoke = [&] { if (catalog) (void)lease->catalog_query(TablesCatalogRequest{}); else (void)lease->inspect(); };
      if (exception == 1) { EXPECT_THROW(invoke(), std::bad_alloc); }
      else if (exception == 2) { EXPECT_THROW(invoke(), std::runtime_error); }
      else { EXPECT_THROW(invoke(), int); }
      EXPECT_FALSE(*lease); retired_once(*observed);
      if (catalog) {
        auto result = lease->catalog_query(TablesCatalogRequest{}); ASSERT_FALSE(result);
        EXPECT_EQ(BackendOperation::BuildCatalogQuery, result.backend_error().operation);
      } else {
        auto result = lease->inspect(); ASSERT_FALSE(result);
        EXPECT_EQ(BackendOperation::InspectSession, result.backend_error().operation);
      }
    }
  }
}
}

namespace {
TEST(SessionLeaseObservationTest, ContradictoryConnectedStateRetiresBeforeFacetAccess) {
  for (bool catalog : {false, true}) {
    for (bool connected : {false, true}) {
      auto observed = std::make_shared<Observed>(); observed->catalog_facet = true;
      auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
      observed->passive_connected = connected;
      observed->passive_state = connected ? SessionState::Disconnected : SessionState::Idle;
      observed->on_catalog = [](const CatalogRequest&) { FAIL() << "Contradictory state must not build SQL"; };
      const auto check = [](auto result) {
        ASSERT_FALSE(result); EXPECT_EQ(BackendErrorClass::Protocol, result.backend_error().error_class);
        EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
      };
      if (catalog) check(lease->catalog_query(TablesCatalogRequest{}));
      else check(lease->inspect());
      EXPECT_FALSE(*lease); retired_once(*observed);
    }
  }
}
}

namespace {
TEST(SessionLeaseFacetsTest, ActiveHealthPreservesBorrowerTransactionStateAndInvalidatesScope) {
  auto observed = std::make_shared<Observed>();
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  SessionOwner owner{std::make_unique<FakeSession>(observed), token};
  auto lease = owner.try_acquire(token); ASSERT_TRUE(lease);
  auto scope = lease->cache_token(); ASSERT_TRUE(scope);
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  observed->health_snapshot = SessionSnapshot{SessionState::Transaction, SessionDisposition::ResetRequired};
  observed->on_health = [&] { EXPECT_FALSE(scope->is_current()); EXPECT_FALSE(owner.try_acquire(token)); };
  auto health = lease->check_health(deadline); ASSERT_TRUE(health);
  EXPECT_EQ(deadline, observed->health_deadline); EXPECT_EQ(1, observed->health);
  EXPECT_EQ((SessionSnapshot{SessionState::Transaction, SessionDisposition::ResetRequired}), health.session_snapshot());
  EXPECT_TRUE(*lease); EXPECT_EQ(0, observed->disconnects);
}
TEST(SessionLeaseFacetsTest, ActiveHealthMissingFacetAndTerminalErrorsRespectExclusiveLifetime) {
  for (int mode : {0, 2, 3, 4, 6}) {
    auto observed = std::make_shared<Observed>(); observed->missing_health = mode == 0;
    observed->health_mode = mode;
    if (mode == 6) observed->health_snapshot = SessionSnapshot{};
    auto owner = owner_for(observed); auto lease = owner.try_acquire(); ASSERT_TRUE(lease);
    if (mode == 0) {
      auto result = lease->check_health(rs::util::Deadline::max()); ASSERT_FALSE(result);
      EXPECT_EQ(BackendErrorClass::Unsupported, result.backend_error().error_class); EXPECT_TRUE(*lease);
    } else if (mode == 2) { EXPECT_THROW(lease->check_health(rs::util::Deadline::max()), std::bad_alloc); }
    else if (mode == 3) { EXPECT_THROW(lease->check_health(rs::util::Deadline::max()), std::runtime_error); }
    else if (mode == 4) { EXPECT_THROW(lease->check_health(rs::util::Deadline::max()), int); }
    else { ASSERT_TRUE(lease->check_health(rs::util::Deadline::max())); EXPECT_FALSE(*lease); }
    lease->retire(); EXPECT_EQ(1, observed->disconnects); EXPECT_EQ(1, observed->destructions);
    auto closed = lease->check_health(rs::util::Deadline::max()); ASSERT_FALSE(closed);
    EXPECT_EQ(BackendOperation::CheckHealth, closed.backend_error().operation);
  }
}
}
