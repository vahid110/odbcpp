#include <gtest/gtest.h>
#include "core/database/backend_provider.h"
#include "core/database/session_owner.h"
#include "core/transport/i_transport.h"
#include "odbc/connection_string.h"
#include "tests/test_connection_config.h"
#include <algorithm>
#include <array>
#include <thread>

namespace {
using namespace rs::core::database;
TEST(SessionOwnerIntegrationTest, LiveBorrowSurvivesOwnerAndRetirementClosesPhysicalSession) {
  const auto& provider = configured_backend_provider();
  const auto params = rs::odbc::ConnectionString::resolve(
      odbcpp::test::configured_connection_string(), provider.identity().driver_name).effective_parameters;
  ConnectionOptions options;
  if (params.count("SERVER")) options.host = params.at("SERVER");
  if (params.count("PORT")) options.port = static_cast<std::uint16_t>(std::stoul(params.at("PORT")));
  if (params.count("DATABASE")) options.database = params.at("DATABASE");
  if (params.count("UID")) options.user = params.at("UID");
  if (params.count("PWD")) options.password = params.at("PWD");
  if (params.count("SSL")) {
    ASSERT_TRUE(params.at("SSL") == "1" || params.at("SSL") == "0") << "Fixture SSL must be explicit 0 or 1";
    options.use_ssl = params.at("SSL") == "1";
  }
  if (params.count("SSLCAFILE")) options.ssl_ca_file = params.at("SSLCAFILE");
  if (params.count("SSLCADIR")) options.ssl_ca_dir = params.at("SSLCADIR");
  auto settings = provider.resolve_connection_options(std::move(options));
  ASSERT_TRUE(settings) << "Mandatory PostgreSQL ownership fixture configuration failed";
  auto physical = provider.create_session(nullptr);
  ASSERT_TRUE(physical->connect(*settings)) << "Mandatory PostgreSQL ownership fixture connection failed";
  // Trusted coordinator publishes only after this physical authentication.
  CredentialContext credentials; auto token = credentials.publish_authenticated();
  std::optional<SessionLease> lease;
  std::optional<SessionCacheToken> owner_ticket;
  {
    SessionOwner owner{std::move(physical), token};
    EXPECT_FALSE(owner.try_acquire());
    lease = owner.try_acquire(token); ASSERT_TRUE(lease); EXPECT_FALSE(owner.try_acquire(token));
    owner_ticket = lease->cache_token(); ASSERT_TRUE(owner_ticket); EXPECT_TRUE(owner_ticket->is_current());
  }
  EXPECT_FALSE(owner_ticket->is_current());
  // Neither owner destruction nor credential revocation interrupts this borrower.
  credentials.revoke(); EXPECT_FALSE(token.is_current());
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto pid_result = lease->execute_query("SELECT pg_backend_pid()", deadline);
  ASSERT_TRUE(pid_result); ASSERT_EQ(1u, pid_result->rows.size()); ASSERT_EQ(1u, pid_result->rows[0].size());
  ASSERT_TRUE(pid_result->rows[0][0]); const auto pid = *pid_result->rows[0][0];
  ASSERT_FALSE(pid.empty()); ASSERT_TRUE(std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; }));
  auto observer = provider.create_session(nullptr); ASSERT_TRUE(observer->connect(*settings));
  auto active = observer->execute_query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + pid, deadline);
  ASSERT_TRUE(active); ASSERT_EQ("1", active->rows.at(0).at(0));
  // Even successful backend reset cannot grant return/requeue in this primitive.
  ASSERT_TRUE(lease->reset_session(deadline));
  const std::array<QueryParameter, 1> prepared_params{{{std::string("retained borrower"), QueryParameterType::Text}}};
  auto prepared = lease->execute_prepared("SELECT $1::text", prepared_params, deadline);
  ASSERT_TRUE(prepared); ASSERT_EQ(1u, prepared->rows.size());
  EXPECT_EQ("retained borrower", prepared->rows[0].at(0));
  lease->retire(); EXPECT_FALSE(*lease);
  bool gone = false;
  while (std::chrono::steady_clock::now() < deadline) {
    auto inactive = observer->execute_query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + pid, deadline);
    ASSERT_TRUE(inactive);
    if (inactive->rows.at(0).at(0) == "0") { gone = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(gone) << "Retired physical PostgreSQL session remained active";
  // An expired coordinator cleanup deadline must retire a real physical session,
  // even though its backend reset facet exists and its socket is still healthy.
  auto rejected_physical = provider.create_session(nullptr); ASSERT_TRUE(rejected_physical->connect(*settings));
  SessionOwner rejected_owner{std::move(rejected_physical)};
  auto rejected_lease = rejected_owner.try_acquire(); ASSERT_TRUE(rejected_lease);
  const auto rejection_deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto rejected_pid = rejected_lease->execute_query("SELECT pg_backend_pid()", rejection_deadline);
  ASSERT_TRUE(rejected_pid); ASSERT_EQ(1u, rejected_pid->rows.size()); ASSERT_EQ(1u, rejected_pid->rows[0].size());
  ASSERT_TRUE(rejected_pid->rows[0][0]); const auto rejected_pid_text = *rejected_pid->rows[0][0];
  ASSERT_FALSE(rejected_pid_text.empty());
  ASSERT_TRUE(std::all_of(rejected_pid_text.begin(), rejected_pid_text.end(), [](char c) { return c >= '0' && c <= '9'; }));
  auto rejection = rejected_lease->reset_session(rs::util::Deadline::min()); ASSERT_FALSE(rejection);
  EXPECT_EQ(BackendErrorClass::Timeout, rejection.backend_error().error_class);
  EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), rejection.session_snapshot());
  EXPECT_FALSE(*rejected_lease); EXPECT_FALSE(rejected_owner.try_acquire());
  gone = false;
  while (std::chrono::steady_clock::now() < rejection_deadline) {
    auto inactive = observer->execute_query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + rejected_pid_text, rejection_deadline);
    ASSERT_TRUE(inactive);
    if (inactive->rows.at(0).at(0) == "0") { gone = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(gone) << "Rejected reset left a live physical PostgreSQL session";
  // Fresh authenticated physical scope: every query/reset attempt invalidates
  // prior metadata/statement scope, including recoverable server errors.
  auto cache_physical = provider.create_session(nullptr); ASSERT_TRUE(cache_physical->connect(*settings));
  auto cache_credential = credentials.publish_authenticated();
  SessionOwner cache_owner{std::move(cache_physical), cache_credential};
  auto cache_lease = cache_owner.try_acquire(cache_credential); ASSERT_TRUE(cache_lease);
  auto scope = cache_lease->cache_token(); ASSERT_TRUE(scope); EXPECT_TRUE(scope->is_current());
  const auto cache_deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto cache_pid = cache_lease->execute_query("SELECT pg_backend_pid()", cache_deadline);
  ASSERT_TRUE(cache_pid); ASSERT_EQ(1u, cache_pid->rows.size()); ASSERT_EQ(1u, cache_pid->rows[0].size());
  ASSERT_TRUE(cache_pid->rows[0][0]); const auto cache_pid_text = *cache_pid->rows[0][0];
  ASSERT_FALSE(cache_pid_text.empty());
  ASSERT_TRUE(std::all_of(cache_pid_text.begin(), cache_pid_text.end(), [](char c) { return c >= '0' && c <= '9'; }));
  EXPECT_FALSE(scope->is_current()); auto prior = *scope;
  scope = cache_lease->cache_token(); ASSERT_TRUE(scope); EXPECT_TRUE(scope->is_current());
  ASSERT_TRUE(cache_lease->reset_session(cache_deadline)); EXPECT_FALSE(scope->is_current());
  scope = cache_lease->cache_token(); ASSERT_TRUE(scope); EXPECT_TRUE(scope->is_current());
  EXPECT_FALSE(cache_lease->execute_query("SELECT (", cache_deadline)); EXPECT_FALSE(scope->is_current());
  scope = cache_lease->cache_token(); ASSERT_TRUE(scope); EXPECT_TRUE(scope->is_current());
  EXPECT_FALSE(cache_lease->accepts_cache(prior)); cache_lease->retire(); EXPECT_FALSE(scope->is_current());
  gone = false;
  while (std::chrono::steady_clock::now() < cache_deadline) {
    auto inactive = observer->execute_query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + cache_pid_text, cache_deadline);
    ASSERT_TRUE(inactive);
    if (inactive->rows.at(0).at(0) == "0") { gone = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(gone) << "Cache token retained a retired physical PostgreSQL session";
  // Explicit return proves reuse of the same authenticated backend, while
  // clearing transaction-local and session state before the next borrower.
  auto reusable_physical = provider.create_session(nullptr); ASSERT_TRUE(reusable_physical->connect(*settings));
  CredentialContext reusable_credentials; auto reusable_token = reusable_credentials.publish_authenticated();
  SessionOwner reusable_owner{std::move(reusable_physical), reusable_token};
  auto first = reusable_owner.try_acquire(reusable_token); ASSERT_TRUE(first);
  const auto reuse_deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto first_pid = first->execute_query("SELECT pg_backend_pid()", reuse_deadline); ASSERT_TRUE(first_pid);
  const auto reuse_pid = first_pid->rows.at(0).at(0);
  ASSERT_TRUE(first->execute_query("CREATE TEMP TABLE odbcpp_return_fixture(v int)", reuse_deadline));
  ASSERT_TRUE(first->execute_query("SET application_name = 'odbcpp dirty borrower'", reuse_deadline));
  auto first_scope = first->cache_token(); ASSERT_TRUE(first_scope);
  ASSERT_TRUE(first->execute_query("BEGIN", reuse_deadline));
  ASSERT_TRUE(first->return_reusable(reuse_deadline)); EXPECT_FALSE(*first); EXPECT_FALSE(first_scope->is_current());
  auto second = reusable_owner.try_acquire(reusable_token); ASSERT_TRUE(second);
  first.reset(); // Old borrower destruction cannot close the returned connection.
  auto second_pid = second->execute_query("SELECT pg_backend_pid()", reuse_deadline); ASSERT_TRUE(second_pid);
  EXPECT_EQ(reuse_pid, second_pid->rows.at(0).at(0));
  auto clean = second->execute_query("SELECT to_regclass('pg_temp.odbcpp_return_fixture') IS NULL, "
      "current_setting('application_name') <> 'odbcpp dirty borrower'", reuse_deadline);
  ASSERT_TRUE(clean); EXPECT_EQ("1", clean->rows.at(0).at(0)); EXPECT_EQ("1", clean->rows.at(0).at(1));
  ASSERT_TRUE(second->return_reusable(reuse_deadline));
  reusable_credentials.revoke(); EXPECT_FALSE(reusable_owner.try_acquire(reusable_token));
  gone = false;
  while (std::chrono::steady_clock::now() < reuse_deadline) {
    auto inactive = observer->execute_query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + *reuse_pid, reuse_deadline);
    ASSERT_TRUE(inactive);
    if (inactive->rows.at(0).at(0) == "0") { gone = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(gone) << "Revoked returned physical session remained active";
  // Policy observes idle expiry at checkout; no background timer is implied.
  auto timed_physical = provider.create_session(nullptr); ASSERT_TRUE(timed_physical->connect(*settings));
  CredentialContext timed_credentials; auto timed_token = timed_credentials.publish_authenticated();
  SessionOwner timed_owner{std::move(timed_physical), timed_token,
      {rs::util::make_deadline(std::chrono::seconds(10)), std::chrono::milliseconds(100)}};
  auto timed_lease = timed_owner.try_acquire(timed_token); ASSERT_TRUE(timed_lease);
  const auto timed_deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto timed_pid = timed_lease->execute_query("SELECT pg_backend_pid()", timed_deadline); ASSERT_TRUE(timed_pid);
  const auto timed_pid_text = timed_pid->rows.at(0).at(0); ASSERT_TRUE(timed_pid_text);
  ASSERT_TRUE(timed_lease->return_reusable(timed_deadline));
  std::this_thread::sleep_for(std::chrono::milliseconds(110)); EXPECT_FALSE(timed_owner.try_acquire(timed_token));
  gone = false;
  while (std::chrono::steady_clock::now() < timed_deadline) {
    auto inactive = observer->execute_query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + *timed_pid_text, timed_deadline);
    ASSERT_TRUE(inactive);
    if (inactive->rows.at(0).at(0) == "0") { gone = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(gone) << "Expired idle physical session survived matching checkout";
  // Checked admission probes a real backend without resetting/reconnecting it.
  // A terminated returned backend must fail the next probe and never reissue.
  auto health_physical = provider.create_session(nullptr); ASSERT_TRUE(health_physical->connect(*settings));
  CredentialContext health_credentials; auto health_token = health_credentials.publish_authenticated();
  SessionOwner health_owner{std::move(health_physical), health_token};
  const auto health_deadline = rs::util::make_deadline(std::chrono::seconds(5));
  auto checked = health_owner.acquire_healthy(health_token, health_deadline); ASSERT_TRUE(checked);
  EXPECT_FALSE(health_owner.try_acquire(health_token));
  auto health_pid = checked->execute_query("SELECT pg_backend_pid()", health_deadline); ASSERT_TRUE(health_pid);
  const auto health_pid_text = health_pid->rows.at(0).at(0); ASSERT_TRUE(health_pid_text);
  ASSERT_TRUE(checked->return_reusable(health_deadline));
  auto checked_again = health_owner.acquire_healthy(health_token, health_deadline); ASSERT_TRUE(checked_again);
  auto same_health_pid = checked_again->execute_query("SELECT pg_backend_pid()", health_deadline); ASSERT_TRUE(same_health_pid);
  EXPECT_EQ(health_pid_text, same_health_pid->rows.at(0).at(0));
  ASSERT_TRUE(checked_again->return_reusable(health_deadline));
  auto killed = observer->execute_query("SELECT pg_terminate_backend(" + *health_pid_text + ")", health_deadline);
  ASSERT_TRUE(killed); EXPECT_EQ("1", killed->rows.at(0).at(0));
  // SIGTERM delivery is asynchronous; confirm exit before probing the socket.
  gone = false;
  while (std::chrono::steady_clock::now() < health_deadline) {
    auto inactive = observer->execute_query("SELECT count(*) FROM pg_stat_activity WHERE pid = " + *health_pid_text, health_deadline);
    ASSERT_TRUE(inactive);
    if (inactive->rows.at(0).at(0) == "0") { gone = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(gone) << "PostgreSQL health fixture backend did not exit after termination";
  auto rejected_health = health_owner.acquire_healthy(health_token, health_deadline); ASSERT_FALSE(rejected_health);
  EXPECT_EQ(BackendOperation::CheckHealth, rejected_health.backend_error().operation);
  EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), rejected_health.session_snapshot());
  EXPECT_FALSE(health_owner.try_acquire(health_token));
  observer->disconnect();
}
} // namespace
