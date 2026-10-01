#include <gtest/gtest.h>
#include "core/database/postgres/pg_database_connection.h"
#include <new>

namespace {
using namespace rs::core::database;
class ResetProbe final : public postgres::PgDatabaseConnection {
 public:
  SessionState state{SessionState::Idle};
  std::vector<std::string> commands;
  std::vector<rs::util::Deadline> deadlines;
  int failure_at{}, malformed{}, disconnects{};
  bool allocation_failure{};
  explicit ResetProbe(bool enabled = true) : PgDatabaseConnection(nullptr, enabled ? std::optional{SessionResetProfile::SameAuthenticatedServerSession} : std::nullopt) {}
  SessionState session_state() const override { return state; }
  void disconnect() override { state = SessionState::Disconnected; ++disconnects; }
  BackendResult<QueryResult> execute_cleanup_query(std::string_view sql, std::string_view expected, rs::util::Deadline deadline) override {
    EXPECT_EQ(sql, expected);
    if (allocation_failure) throw std::bad_alloc{};
    commands.emplace_back(sql); deadlines.push_back(deadline);
    if (static_cast<int>(commands.size()) == failure_at) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owned reset failure"};
      error.operation = BackendOperation::ExecuteDirect;
      error.native_state = "42501"; error.native_code = 42;
      error.session_state = SessionState::Idle; error.disposition = SessionDisposition::Reusable;
      error.retry_safe = true;
      return error;
    }
    state = SessionState::Idle;
    QueryResult result; result.statement_kind = StatementKind::Unknown;
    SessionSnapshot snapshot{SessionState::Idle, SessionDisposition::Reusable};
    switch (malformed) {
      case 1: result.rows = {{"1"}}; break;
      case 2: result.columns = {{"unexpected", {}}}; break;
      case 3: result.affected_rows = 1; break;
      case 4: result.additional_results.emplace_back(); break;
      case 5: result.cell_errors.push_back({0, 0}); break;
      case 6: result.normalized_parameter_types.push_back({}); break;
      case 7: result.statement_kind.reset(); break;
      case 8: snapshot = {SessionState::Transaction, SessionDisposition::ResetRequired}; break;
      case 9: result.error.emplace(rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "unexpected"); break;
      case 10: result.statement_kind = StatementKind::SelectCursor; break;
    }
    return {std::move(result), snapshot};
  }
};
TEST(SessionResetTest, CleansIdleAndTransactionsUnderOneOriginalDeadline) {
  for (const auto state : {SessionState::Idle, SessionState::Transaction, SessionState::FailedTransaction}) {
    ResetProbe session; session.state = state;
    ASSERT_NE(nullptr, session.session_reset());
    EXPECT_EQ(SessionResetProfile::SameAuthenticatedServerSession, session.session_reset()->reset_profile());
    const auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
    auto result = session.session_reset()->reset_session(deadline);
    ASSERT_TRUE(result);
    const std::vector<std::string> expected = state == SessionState::Idle ?
        std::vector<std::string>{"DISCARD ALL"} : std::vector<std::string>{"ROLLBACK", "DISCARD ALL"};
    EXPECT_EQ(expected, session.commands);
    EXPECT_EQ(std::vector<rs::util::Deadline>(expected.size(), deadline), session.deadlines);
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
    session.disconnect();
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
  }
}
TEST(SessionResetTest, AnyServerFailureRetiresAndStopsCleanupWithoutReplay) {
  for (const auto state : {SessionState::Idle, SessionState::Transaction}) {
    const int exchanges = state == SessionState::Idle ? 1 : 2;
    for (int step = 1; step <= exchanges; ++step) {
      ResetProbe session; session.state = state; session.failure_at = step;
      const auto result = session.reset_session(rs::util::Deadline::max());
      ASSERT_FALSE(result);
      EXPECT_EQ(step, static_cast<int>(session.commands.size()));
      EXPECT_EQ(1, session.disconnects);
      EXPECT_EQ(BackendOperation::ResetSession, result.backend_error().operation);
      EXPECT_EQ("owned reset failure", result.error_message());
      EXPECT_EQ("42501", result.backend_error().native_state);
      EXPECT_EQ(42, result.backend_error().native_code);
      EXPECT_FALSE(result.backend_error().retry_safe.has_value());
      EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    }
  }
}
TEST(SessionResetTest, UnexpectedSuccessAndAllocationFailureRetire) {
  for (int mode = 1; mode <= 11; ++mode) {
    ResetProbe session;
    session.malformed = mode; session.allocation_failure = mode == 11;
    const auto result = session.reset_session(rs::util::Deadline::max());
    ASSERT_FALSE(result);
    EXPECT_EQ(mode == 11 ? BackendErrorClass::AllocationFailure : BackendErrorClass::Protocol, result.backend_error().error_class);
    EXPECT_EQ(BackendOperation::ResetSession, result.backend_error().operation);
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    EXPECT_EQ(1, session.disconnects);
  }
}
TEST(SessionResetTest, MissingProfileAndInvalidPassiveStateDoNotSendCleanup) {
  ResetProbe unsupported(false);
  EXPECT_EQ(nullptr, unsupported.session_reset());
  EXPECT_EQ(BackendErrorClass::Unsupported, unsupported.reset_session(rs::util::Deadline::max()).backend_error().error_class);
  EXPECT_TRUE(unsupported.commands.empty()); EXPECT_EQ(0, unsupported.disconnects);
  for (const auto state : {SessionState::Unknown, SessionState::Disconnected}) {
    ResetProbe session; session.state = state;
    const auto result = session.reset_session(rs::util::Deadline::max());
    ASSERT_FALSE(result); EXPECT_TRUE(session.commands.empty());
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
  }
}
} // namespace
