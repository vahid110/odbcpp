#include <gtest/gtest.h>
#include "core/database/session_owner.h"
#include <array>
#include <atomic>
#include <barrier>
#include <functional>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
using namespace rs::core::database;
static_assert(!std::is_copy_constructible_v<SessionOwner> && !std::is_copy_assignable_v<SessionOwner>);
static_assert(!std::is_copy_constructible_v<SessionLease> && !std::is_copy_assignable_v<SessionLease>);
static_assert(std::is_nothrow_move_constructible_v<SessionOwner> && std::is_nothrow_move_assignable_v<SessionOwner>);
static_assert(std::is_nothrow_move_constructible_v<SessionLease> && std::is_nothrow_move_assignable_v<SessionLease>);
struct Observed {
  std::atomic<int> disconnects{}, destructions{}, queries{}, health{}, resets{};
  bool throw_disconnect{};
  std::function<void()> on_disconnect;
};
class FakeSession final : public IDatabaseConnection, public ISessionHealth, public ISessionReset {
 public:
  explicit FakeSession(std::shared_ptr<Observed> observed) : observed_(std::move(observed)) {}
  ~FakeSession() override { ++observed_->destructions; }
  BackendResult<void> connect(const ConnectionSettings&) override { return {}; }
  void disconnect() override {
    ++observed_->disconnects; connected_ = false;
    if (observed_->on_disconnect) observed_->on_disconnect();
    if (observed_->throw_disconnect) throw std::runtime_error("disconnect fixture");
  }
  bool is_connected() const override { return connected_; }
  BackendResult<QueryResult> execute_query(std::string_view, rs::util::Deadline) override {
    ++observed_->queries; return QueryResult{};
  }
  BackendResult<QueryResult> execute_prepared(std::string_view,
      std::span<const QueryParameter>, rs::util::Deadline) override { return QueryResult{}; }
  std::string server_version() const override { return "fixture"; }
  ISessionHealth* session_health() noexcept override { return this; }
  ISessionReset* session_reset() noexcept override { return this; }
  SessionResetProfile reset_profile() const noexcept override { return SessionResetProfile::SameAuthenticatedServerSession; }
  BackendResult<void> check_health(rs::util::Deadline) override { ++observed_->health; return {}; }
  BackendResult<void> reset_session(rs::util::Deadline) override { ++observed_->resets; return {}; }
 private:
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
  ASSERT_TRUE(lease->session()->execute_query("fixture", rs::util::Deadline::max()));
  lease->retire(); EXPECT_FALSE(*lease); EXPECT_EQ(nullptr, lease->session());
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
  ASSERT_TRUE(lease->session()->execute_query("fixture", rs::util::Deadline::max()));
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
  auto* physical = lease1->session(); SessionLease moved{std::move(*lease1)};
  EXPECT_FALSE(*lease1); EXPECT_EQ(physical, moved.session());
  *lease2 = std::move(moved); EXPECT_FALSE(moved); EXPECT_EQ(physical, lease2->session());
  retired_once(*second); EXPECT_EQ(0, first->disconnects);
  auto& same = *lease2; same = std::move(*lease2); EXPECT_EQ(physical, same.session());
  lease1.reset(); lease2.reset(); retired_once(*first);
  EXPECT_FALSE(owner1.try_acquire()); EXPECT_FALSE(owner2.try_acquire());
}
TEST(SessionOwnerTest, OwnerMovesClosePriorAdmissionAndKeepLiveLeaseValid) {
  auto first = std::make_shared<Observed>(); auto second = std::make_shared<Observed>();
  auto owner1 = owner_for(first); auto owner2 = owner_for(second);
  auto lease1 = owner1.try_acquire(); ASSERT_TRUE(lease1);
  SessionOwner moved{std::move(owner2)}; EXPECT_FALSE(owner2.try_acquire());
  owner1 = std::move(moved); EXPECT_FALSE(moved.try_acquire());
  EXPECT_EQ(0, first->disconnects); EXPECT_TRUE(lease1->session()->is_connected());
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
  EXPECT_TRUE(lease->session()->is_connected());
  ASSERT_TRUE(lease->session()->execute_query("fixture", rs::util::Deadline::max()));
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
