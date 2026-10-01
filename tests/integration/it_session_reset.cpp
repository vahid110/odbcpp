#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "tests/test_connection_config.h"

namespace {
SQLCHAR* test_dsn() { return odbcpp::test::configured_connection_string_data(); }
class SessionResetIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    const auto result = SQLConnect(connection_, test_dsn(), SQL_NTS,
                                   nullptr, 0, nullptr, 0);
    ASSERT_EQ(SQL_SUCCESS, result) << "Mandatory PostgreSQL reset connection failed";
  }

  void TearDown() override {
    if (statement_) SQLFreeHandle(SQL_HANDLE_STMT, statement_);
    if (connection_) {
      SQLDisconnect(connection_);
      SQLFreeHandle(SQL_HANDLE_DBC, connection_);
    }
    if (environment_) SQLFreeHandle(SQL_HANDLE_ENV, environment_);
  }

  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  SQLHSTMT statement_{SQL_NULL_HSTMT};
};

TEST_F(SessionResetIntegrationTest, ResetRestoresServerBaselineAndClearsSessionResources) {
  using namespace rs::core::database;
  auto handle = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(connection_);
  ASSERT_NE(nullptr, handle);
  auto* session = handle->get_db_connection();
  ASSERT_NE(nullptr, session);
  auto* reset = session->session_reset();
  ASSERT_NE(nullptr, reset);
  const auto deadline = [] { return rs::util::make_deadline(std::chrono::seconds(5)); };
  const auto original_name = session->execute_query("SHOW application_name", deadline());
  const auto original_isolation = session->execute_query("SHOW default_transaction_isolation", deadline());
  const auto original_encoding = session->execute_query("SHOW client_encoding", deadline());
  const auto original_user = session->execute_query("SELECT current_user, session_user", deadline());
  ASSERT_TRUE(original_name); ASSERT_TRUE(original_isolation); ASSERT_TRUE(original_encoding); ASSERT_TRUE(original_user);
  ASSERT_TRUE(session->execute_query("SET application_name = 'odbcpp_reset_fixture'", deadline()));
  ASSERT_TRUE(session->execute_query("SET default_transaction_isolation = 'serializable'", deadline()));
  ASSERT_TRUE(session->execute_query("PREPARE odbcpp_reset_fixture AS SELECT 1", deadline()));
  ASSERT_TRUE(session->execute_query("CREATE TEMP TABLE odbcpp_reset_fixture(value integer)", deadline()));
  ASSERT_TRUE(session->execute_query("DO $$ BEGIN PERFORM pg_advisory_lock(9021001); END $$", deadline()));
  ASSERT_TRUE(session->transaction_session()->transaction(TransactionAction::Begin, deadline()));
  ASSERT_TRUE(session->execute_query("INSERT INTO odbcpp_reset_fixture VALUES (42)", deadline()));
  const auto outcome = reset->reset_session(deadline());
  ASSERT_TRUE(outcome) << outcome.error_message();
  EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), outcome.session_snapshot());
  EXPECT_EQ(reset, session->session_reset());
  const auto name = session->execute_query("SHOW application_name", deadline());
  const auto isolation = session->execute_query("SHOW default_transaction_isolation", deadline());
  const auto encoding = session->execute_query("SHOW client_encoding", deadline());
  const auto user = session->execute_query("SELECT current_user, session_user", deadline());
  ASSERT_TRUE(name); ASSERT_TRUE(isolation); ASSERT_TRUE(encoding); ASSERT_TRUE(user);
  EXPECT_EQ(original_name->rows, name->rows);
  EXPECT_EQ(original_isolation->rows, isolation->rows);
  EXPECT_EQ(original_encoding->rows, encoding->rows);
  EXPECT_EQ(original_user->rows, user->rows);
  const auto temp = session->execute_query("SELECT to_regclass('pg_temp.odbcpp_reset_fixture') IS NULL", deadline());
  ASSERT_TRUE(temp); ASSERT_EQ(1u, temp->rows.size()); ASSERT_EQ(1u, temp->rows[0].size());
  EXPECT_EQ("1", temp->rows[0][0]);
  const auto prepared = session->execute_query("SELECT count(*) FROM pg_prepared_statements WHERE name = 'odbcpp_reset_fixture'", deadline());
  ASSERT_TRUE(prepared); EXPECT_EQ("0", prepared->rows.at(0).at(0));
  const auto lock = session->execute_query("SELECT pg_advisory_unlock(9021001)", deadline());
  ASSERT_TRUE(lock); EXPECT_EQ("0", lock->rows.at(0).at(0));
}

TEST_F(SessionResetIntegrationTest, ResetRollsBackFailedTransactionAndTimeoutRetires) {
  using namespace rs::core::database;
  auto handle = rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(connection_);
  ASSERT_NE(nullptr, handle);
  auto* session = handle->get_db_connection();
  ASSERT_NE(nullptr, session);
  auto* reset = session->session_reset();
  ASSERT_NE(nullptr, reset);
  const auto deadline = [] { return rs::util::make_deadline(std::chrono::seconds(5)); };
  ASSERT_TRUE(session->transaction_session()->transaction(TransactionAction::Begin, deadline()));
  ASSERT_FALSE(session->execute_query("SELECT 1 / 0", deadline()));
  const auto recovered = reset->reset_session(deadline());
  ASSERT_TRUE(recovered);
  EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), recovered.session_snapshot());
  ASSERT_TRUE(session->session_health()->check_health(deadline()));
  const auto timed_out = reset->reset_session(rs::util::Deadline::min());
  ASSERT_FALSE(timed_out);
  EXPECT_EQ(BackendErrorClass::Timeout, timed_out.backend_error().error_class);
  EXPECT_EQ(BackendOperation::ResetSession, timed_out.backend_error().operation);
  EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), timed_out.session_snapshot());
  EXPECT_FALSE(session->is_connected());
  EXPECT_EQ(reset, session->session_reset());
}

} // namespace
