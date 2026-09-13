#include <gtest/gtest.h>

#include "core/database/generic_database_connection.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/i_transport.h"
#include "tests/mock_protocol_parser.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

class FailingQueryTransport final : public rs::core::transport::ITransport {
 public:
  rs::util::Result<void> connect(std::string_view, uint16_t,
                                 rs::util::Deadline) override {
    return {};
  }

  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> buffer, rs::util::Deadline) override {
    if (send_count_++ != 0) {
      return {rs::util::DbErrorCode::NetworkError,
              "injected connection loss"};
    }
    return rs::core::transport::IOResult{buffer.size(), false};
  }

  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> buffer, rs::util::Deadline) override {
    static constexpr std::byte startup[]{
        std::byte{'R'}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{'Z'}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4}};
    const auto available = std::size(startup) - receive_offset_;
    const auto count = std::min(buffer.size(), available);
    std::copy_n(startup + receive_offset_, count, buffer.begin());
    receive_offset_ += count;
    return rs::core::transport::IOResult{count, false};
  }

  void close() noexcept override {}

 private:
  std::size_t send_count_{0};
  std::size_t receive_offset_{0};
};

class ScriptedBackendTransport final : public rs::core::transport::ITransport {
 public:
  enum class ResponseMode {
    ValidStartup, MalformedStartup, MalformedAuth, AuthRejected,
    MalformedQuery, MalformedStartupReady, MalformedQueryReady,
    MalformedQueryError, MalformedBackendKey, ReadyWithoutAuth,
    StartupRejectedAfterAuth, LoginRejectedAfterAuth,
    DuplicateAuthenticationOk, ChallengeAfterAuthenticationOk,
    ParameterStatusBeforeAuthenticationOk, BackendKeyBeforeAuthenticationOk,
    UnexpectedQueryFrameBeforeAuthenticationOk,
    UnexpectedQueryFrameAfterAuthenticationOk, NoticeAfterAuthenticationOk,
    MalformedNoticeMissingField, MalformedNoticeDuplicateField,
    MalformedNoticeUnterminated
  };

  explicit ScriptedBackendTransport(
      ResponseMode mode = ResponseMode::ValidStartup) {
    if (mode == ResponseMode::MalformedStartup) {
      append_message('S', "missing-terminators", 19);
      return;
    }
    if (mode == ResponseMode::MalformedAuth) {
      append_message('R', "\0\0\0", 3);
      return;
    }
    if (mode == ResponseMode::AuthRejected) {
      constexpr char error[] =
          "SFATAL\0C28P01\0Mpassword authentication failed\0";
      append_message('E', error, sizeof(error));
      return;
    }
    if (mode == ResponseMode::MalformedStartupReady) {
      append_message('Z', "X", 1);
      return;
    }
    if (mode == ResponseMode::ReadyWithoutAuth) {
      append_message('Z', "I", 1);
      return;
    }
    if (mode == ResponseMode::ParameterStatusBeforeAuthenticationOk) {
      append_message('S', "server_version\0" "17.6\0", 20);
      return;
    }
    if (mode == ResponseMode::BackendKeyBeforeAuthenticationOk) {
      constexpr char backend_key[] = "\0\0\0\0\0\0\0\0";
      append_message('K', backend_key, 8);
      return;
    }
    if (mode == ResponseMode::UnexpectedQueryFrameBeforeAuthenticationOk) {
      append_message('C', "SELECT 1\0", 9);
      return;
    }
    append_message('R', "\0\0\0\0", 4);
    if (mode == ResponseMode::UnexpectedQueryFrameAfterAuthenticationOk) {
      append_message('C', "SELECT 1\0", 9);
      return;
    }
    if (mode == ResponseMode::NoticeAfterAuthenticationOk) {
      constexpr char notice[] = "SNOTICE\0C00000\0Mstartup notice\0";
      append_message('N', notice, sizeof(notice));
    }
    if (mode == ResponseMode::MalformedNoticeMissingField) {
      constexpr char notice[] = "SNOTICE\0C00000\0\0";
      append_message('N', notice, sizeof(notice) - 1);
      return;
    }
    if (mode == ResponseMode::MalformedNoticeDuplicateField) {
      constexpr char notice[] =
          "SNOTICE\0C00000\0Mstartup notice\0SNOTICE\0\0";
      append_message('N', notice, sizeof(notice) - 1);
      return;
    }
    if (mode == ResponseMode::MalformedNoticeUnterminated) {
      constexpr char notice[] = "SNOTICE\0C00000\0Mstartup notice";
      append_message('N', notice, sizeof(notice) - 1);
      return;
    }
    if (mode == ResponseMode::DuplicateAuthenticationOk) {
      append_message('R', "\0\0\0\0", 4);
      return;
    }
    if (mode == ResponseMode::ChallengeAfterAuthenticationOk) {
      append_message('R', "\0\0\0\3", 4);
      return;
    }
    if (mode == ResponseMode::StartupRejectedAfterAuth) {
      constexpr char error[] =
          "SERROR\0C22023\0Mstartup option rejected\0";
      append_message('E', error, sizeof(error));
      return;
    }
    if (mode == ResponseMode::LoginRejectedAfterAuth) {
      constexpr char error[] =
          "SFATAL\0C28000\0Mrole does not exist\0";
      append_message('E', error, sizeof(error));
      return;
    }
    append_message('S', "server_version\0" "17.6\0", 20);
    append_message('S', "application_name\0" "odbcpp\0", 24);
    constexpr char backend_key[] = "\0\0\0\0\0\0\0\0";
    append_message('K', backend_key,
                   mode == ResponseMode::MalformedBackendKey ? 7 : 8);
    append_message('Z', "I", 1);
    if (mode == ResponseMode::MalformedQuery) {
      append_message('T', "\0\1", 2);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::MalformedQueryReady) {
      append_message('Z', "IT", 2);
    } else if (mode == ResponseMode::MalformedQueryError) {
      constexpr char error[] = "SERROR\0C42601\0\0";
      append_message('E', error, sizeof(error) - 1);
      append_message('Z', "I", 1);
    }
  }

  rs::util::Result<void> connect(std::string_view, uint16_t,
                                 rs::util::Deadline) override {
    return {};
  }

  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> buffer, rs::util::Deadline) override {
    return rs::core::transport::IOResult{buffer.size(), false};
  }

  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> buffer, rs::util::Deadline) override {
    const auto available = input_.size() - offset_;
    const auto count = std::min(buffer.size(), available);
    std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(offset_), count,
                buffer.begin());
    offset_ += count;
    return rs::core::transport::IOResult{count, false};
  }

  void close() noexcept override {}

 private:
  void append_message(char tag, const char* payload, std::size_t size) {
    input_.push_back(static_cast<std::byte>(tag));
    const auto length = static_cast<std::uint32_t>(size + 4);
    input_.push_back(static_cast<std::byte>((length >> 24) & 0xff));
    input_.push_back(static_cast<std::byte>((length >> 16) & 0xff));
    input_.push_back(static_cast<std::byte>((length >> 8) & 0xff));
    input_.push_back(static_cast<std::byte>(length & 0xff));
    const auto* bytes = reinterpret_cast<const std::byte*>(payload);
    input_.insert(input_.end(), bytes, bytes + size);
  }

  std::vector<std::byte> input_;
  std::size_t offset_{0};
};

TEST(ConnectionLivenessTest, FailedServerTripMarksConnectionDead) {
  auto transport = std::make_unique<FailingQueryTransport>();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<odbcpp::test::MockProtocolParser>(),
      std::move(transport));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  ASSERT_TRUE(connection.is_connected());

  const auto result = connection.execute_query(
      "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::NetworkError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, RetainsPostgresqlStartupParameters) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<
          rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>());

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  EXPECT_EQ("17.6", connection.get_parameter("server_version"));
  EXPECT_EQ("odbcpp", connection.get_parameter("application_name"));
  EXPECT_TRUE(connection.get_parameter("missing").empty());
}

TEST(ConnectionLivenessTest, InvalidSqlDoesNotEscapeOrDisconnect) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>());

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const std::string sql("SELECT 1\0;SELECT 2",
                        sizeof("SELECT 1\0;SELECT 2") - 1);
  const auto result = connection.execute_query(
      sql, rs::util::make_deadline(std::chrono::seconds(1)));

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),
            result.error());
  EXPECT_TRUE(connection.is_connected());
}

TEST(ConnectionLivenessTest, RejectsMalformedStartupParameters) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<
          rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedStartup));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedAuthIsNotReportedAsBadCredentials) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedAuth));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, ServerAuthRejectionRemainsCredentialFailure) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::AuthRejected));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(
                rs::util::DbErrorCode::AuthenticationFailed), result.error());
  EXPECT_NE(result.error_message().find("password authentication failed"),
            std::string::npos);
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, ReadyWithoutAuthenticationOkIsProtocolError) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::ReadyWithoutAuth));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, OutOfPhaseStartupMessagesAreProtocolErrors) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {
           Mode::DuplicateAuthenticationOk,
           Mode::ChallengeAfterAuthenticationOk,
           Mode::ParameterStatusBeforeAuthenticationOk,
           Mode::BackendKeyBeforeAuthenticationOk,
           Mode::UnexpectedQueryFrameBeforeAuthenticationOk,
           Mode::UnexpectedQueryFrameAfterAuthenticationOk}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));

    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, NoticeAfterAuthenticationOkAllowsStartup) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::NoticeAfterAuthenticationOk));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  EXPECT_TRUE(connection.is_connected());
  EXPECT_EQ("17.6", connection.get_parameter("server_version"));
}

TEST(ConnectionLivenessTest, MalformedNoticeCannotCompleteStartup) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::MalformedNoticeMissingField,
                          Mode::MalformedNoticeDuplicateField,
                          Mode::MalformedNoticeUnterminated}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));

    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, ErrorAfterAuthenticationIsStartupFailure) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::StartupRejectedAfterAuth));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ConnectionFailed),
            result.error());
  EXPECT_NE(result.error_message().find("startup option rejected"),
            std::string::npos);
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, AuthenticationSqlstateAfterOkIsCredentialFailure) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::LoginRejectedAfterAuth));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(
                rs::util::DbErrorCode::AuthenticationFailed), result.error());
  EXPECT_NE(result.error_message().find("role does not exist"),
            std::string::npos);
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedStartupReadyCannotOpenConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedStartupReady));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedBackendKeyCannotOpenConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedBackendKey));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedQueryReadyClosesLogicalConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedQueryReady));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const auto result = connection.execute_query(
      "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedErrorCannotBecomeSuccessfulQuery) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedQueryError));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const auto result = connection.execute_query(
      "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedQueryResultClosesLogicalConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedQuery));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto connected = connection.connect(settings);
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  ASSERT_TRUE(connection.is_connected());

  const auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
  const auto result = connection.execute_query("SELECT 1", deadline);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());

  const auto retry = connection.execute_query("SELECT 2", deadline);
  ASSERT_TRUE(retry.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::NotConnected),
            retry.error());
}

}  // namespace
