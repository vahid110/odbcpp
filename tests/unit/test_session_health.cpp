#include <gtest/gtest.h>
#include "core/database/postgres/pg_database_connection.h"

namespace {
using namespace rs::core::database;
class HealthProbe final : public postgres::PgDatabaseConnection {
 public:
  QueryResult response;
  SessionSnapshot snapshot{SessionState::Idle, SessionDisposition::Reusable};
  std::optional<BackendError> failure;
  rs::util::Deadline observed_deadline{};
  std::string command;
  int calls{}, disconnects{};
  HealthProbe() {
    response.columns = {{"?column?", NativeTypeInfo{ScalarType::Integer, 10, 0, true}}};
    response.rows = {{"1"}};
  }
  BackendResult<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline) override {
    ++calls; command = sql; observed_deadline = deadline;
    if (failure) return *failure;
    return {response, snapshot};
  }
  void disconnect() override { ++disconnects; PgDatabaseConnection::disconnect(); }
};

TEST(SessionHealthTest, OptionalFacetPreservesDeadlineAndOwningSnapshot) {
  HealthProbe backend;
  IDatabaseConnection& session = backend;
  ASSERT_NE(nullptr, session.session_health());
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(3));
  for (const auto state : {SessionState::Idle, SessionState::Transaction}) {
    backend.snapshot = {state, state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired};
    auto result = session.session_health()->check_health(deadline);
    ASSERT_TRUE(result);
    EXPECT_EQ(backend.snapshot, result.session_snapshot());
    EXPECT_EQ("SELECT 1", backend.command);
    EXPECT_EQ(deadline, backend.observed_deadline);
    const auto saved = result.session_snapshot();
    backend.snapshot = {};
    EXPECT_EQ(saved, result.session_snapshot());
  }
  EXPECT_EQ(2, backend.calls);
  EXPECT_EQ(0, backend.disconnects);
}

TEST(SessionHealthTest, FailureRetainsDetailsDispositionAndHealthContext) {
  for (const auto code : {rs::util::DbErrorCode::QueryFailed, rs::util::DbErrorCode::NetworkError,
                         rs::util::DbErrorCode::Timeout, rs::util::DbErrorCode::NotConnected}) {
    HealthProbe backend;
    BackendError error{rs::util::make_error_code(code), "owned diagnostic"};
    error.operation = BackendOperation::ExecuteDirect;
    error.native_state = "25P02"; error.native_code = 42;
    error.session_state = code == rs::util::DbErrorCode::QueryFailed ? SessionState::FailedTransaction : SessionState::Disconnected;
    error.disposition = code == rs::util::DbErrorCode::QueryFailed ? SessionDisposition::ResetRequired : SessionDisposition::Retire;
    backend.failure = error;
    const auto result = backend.check_health(rs::util::Deadline::max());
    ASSERT_FALSE(result);
    EXPECT_EQ(BackendOperation::CheckHealth, result.backend_error().operation);
    EXPECT_EQ(error.code, result.error());
    EXPECT_EQ(error.message, result.error_message());
    EXPECT_EQ(error.native_state, result.backend_error().native_state);
    EXPECT_EQ(error.native_code, result.backend_error().native_code);
    EXPECT_EQ((SessionSnapshot{error.session_state, error.disposition}), result.session_snapshot());
    EXPECT_FALSE(result.backend_error().retry_safe.has_value());
    EXPECT_EQ(1, backend.calls);
  }
}

TEST(SessionHealthTest, UnexpectedSuccessRetiresInsteadOfGrantingReuse) {
  for (int mode = 0; mode < 9; ++mode) {
    HealthProbe backend;
    switch (mode) {
      case 0: backend.response.columns.clear(); break;
      case 1: backend.response.rows.clear(); break;
      case 2: backend.response.rows[0].clear(); break;
      case 3: backend.response.rows[0][0] = std::nullopt; break;
      case 4: backend.response.rows[0][0] = "2"; break;
      case 5: backend.response.cell_errors.push_back({0, 0}); break;
      case 6: backend.response.additional_results.emplace_back(); break;
      case 7: backend.response.error.emplace(rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "unexpected"); break;
      case 8: backend.snapshot = {SessionState::FailedTransaction, SessionDisposition::ResetRequired}; break;
    }
    const auto result = backend.check_health(rs::util::Deadline::max());
    ASSERT_FALSE(result) << mode;
    EXPECT_EQ(BackendErrorClass::Protocol, result.backend_error().error_class);
    EXPECT_EQ(BackendOperation::CheckHealth, result.backend_error().operation);
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    EXPECT_EQ(1, backend.disconnects);
  }
}
} // namespace
