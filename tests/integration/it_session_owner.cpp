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
  {
    SessionOwner owner{std::move(physical), token};
    EXPECT_FALSE(owner.try_acquire());
    lease = owner.try_acquire(token); ASSERT_TRUE(lease); EXPECT_FALSE(owner.try_acquire(token));
  }
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
  observer->disconnect();
}
} // namespace
