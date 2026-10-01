#include <gtest/gtest.h>
#include "core/database/credential_context.h"
#include <atomic>
#include <barrier>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace rs::core::database::detail {
struct CredentialContextTestAccess {
  static bool valid_at(const CredentialToken& token, rs::util::Clock::time_point time) {
    return token.is_current_at(time);
  }
  static CredentialContext with_factory(CredentialContext::GenerationFactory factory) {
    return CredentialContext{factory};
  }
  static std::shared_ptr<const CredentialGeneration> allocate(std::optional<rs::util::Deadline> expiry) {
    return CredentialContext::make_generation(expiry);
  }
};
}
namespace {
using namespace rs::core::database;
using Access = detail::CredentialContextTestAccess;
static_assert(!std::is_default_constructible_v<CredentialToken>);
static_assert(std::is_copy_constructible_v<CredentialToken>);
static_assert(!std::is_copy_constructible_v<CredentialContext>);
static_assert(std::is_nothrow_move_constructible_v<CredentialContext>);
static_assert(std::is_nothrow_move_assignable_v<CredentialContext>);
std::atomic<bool> fail_generation{};
std::atomic<bool> null_generation{};
std::shared_ptr<const detail::CredentialGeneration> allocation_fixture(std::optional<rs::util::Deadline> expiry) {
  if (fail_generation.exchange(false)) throw std::bad_alloc{};
  if (null_generation.exchange(false)) return {};
  return Access::allocate(expiry);
}
TEST(CredentialContextTest, StartsRevokedAndRotationsNeverResurrectOldCopies) {
  CredentialContext context; EXPECT_FALSE(context.current_token());
  auto first = context.publish_authenticated(); auto copy = first;
  ASSERT_TRUE(context.current_token()); EXPECT_TRUE(first.is_current()); EXPECT_TRUE(copy.is_current());
  std::vector<CredentialToken> old;
  for (int generation = 0; generation < 200; ++generation) {
    old.push_back(context.publish_authenticated());
    for (std::size_t index = 0; index + 1 < old.size(); ++index) EXPECT_FALSE(old[index].is_current());
    EXPECT_TRUE(old.back().is_current()); EXPECT_FALSE(first.is_current());
  }
  context.revoke(); context.revoke(); EXPECT_FALSE(context.current_token());
  for (const auto& token : old) EXPECT_FALSE(token.is_current());
  auto latest = context.publish_authenticated(); EXPECT_TRUE(latest.is_current()); EXPECT_FALSE(copy.is_current());
}
TEST(CredentialContextTest, ExpiryIsExclusiveAndNeverUsesWallTime) {
  CredentialContext context;
  const auto expiry = rs::util::make_deadline(std::chrono::seconds(5));
  auto token = context.publish_authenticated(expiry);
  EXPECT_TRUE(Access::valid_at(token, expiry - rs::util::Clock::duration{1}));
  EXPECT_FALSE(Access::valid_at(token, expiry));
  EXPECT_FALSE(Access::valid_at(token, expiry + rs::util::Clock::duration{1}));
  auto expired = context.publish_authenticated(rs::util::Deadline::min());
  EXPECT_FALSE(expired.is_current()); EXPECT_FALSE(context.current_token()); EXPECT_FALSE(token.is_current());
  auto indefinite = context.publish_authenticated(); EXPECT_TRUE(indefinite.is_current());
}
TEST(CredentialContextTest, DestructionAndMovesInvalidateOnlyReplacedAuthorities) {
  std::optional<CredentialToken> orphan;
  { CredentialContext context; orphan = context.publish_authenticated(); }
  EXPECT_FALSE(orphan->is_current());
  CredentialContext first; CredentialContext second;
  auto first_token = first.publish_authenticated(); auto second_token = second.publish_authenticated();
  CredentialContext moved{std::move(first)};
  EXPECT_FALSE(first.current_token()); EXPECT_TRUE(first_token.is_current());
  EXPECT_THROW(first.publish_authenticated(), std::logic_error); first.revoke();
  second = std::move(moved); EXPECT_FALSE(moved.current_token());
  EXPECT_FALSE(second_token.is_current()); EXPECT_TRUE(first_token.is_current());
  auto& same = second; same = std::move(second); EXPECT_TRUE(first_token.is_current());
  second.revoke(); EXPECT_FALSE(first_token.is_current());
}
TEST(CredentialContextTest, AllocationFailureRevokesPreviousGenerationBeforeThrowing) {
  auto context = Access::with_factory(&allocation_fixture);
  auto old = context.publish_authenticated(); ASSERT_TRUE(old.is_current());
  fail_generation = true;
  EXPECT_THROW(context.publish_authenticated(), std::bad_alloc);
  EXPECT_FALSE(old.is_current()); EXPECT_FALSE(context.current_token());
  auto next = context.publish_authenticated(); EXPECT_TRUE(next.is_current()); EXPECT_FALSE(old.is_current());
  null_generation = true;
  EXPECT_THROW(context.publish_authenticated(), std::bad_alloc);
  EXPECT_FALSE(next.is_current()); EXPECT_FALSE(context.current_token());
}
TEST(CredentialContextTest, ConcurrentPublishRevokeAndValidationRemainConsistent) {
  CredentialContext context; auto original = context.publish_authenticated();
  std::barrier start(4);
  std::atomic<int> observations{};
  std::thread rotating([&] { start.arrive_and_wait(); for (int n = 0; n < 500; ++n) { auto token = context.publish_authenticated(); (void)token.is_current(); } });
  std::thread revoking([&] { start.arrive_and_wait(); for (int n = 0; n < 500; ++n) context.revoke(); });
  std::thread reading([&] {
    start.arrive_and_wait();
    for (int n = 0; n < 1000; ++n) { auto token = context.current_token(); if (token) { (void)token->is_current(); ++observations; } (void)original.is_current(); }
  });
  start.arrive_and_wait(); rotating.join(); revoking.join(); reading.join();
  context.revoke(); EXPECT_FALSE(original.is_current()); EXPECT_FALSE(context.current_token());
}
} // namespace
