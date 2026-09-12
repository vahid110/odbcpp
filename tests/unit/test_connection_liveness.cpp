#include <gtest/gtest.h>

#include "core/database/generic_database_connection.h"
#include "core/database/mock_protocol_parser.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/transport/i_transport.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
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
    static constexpr std::byte ready[]{
        std::byte{'Z'}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4}};
    const auto available = std::size(ready) - receive_offset_;
    const auto count = std::min(buffer.size(), available);
    std::copy_n(ready + receive_offset_, count, buffer.begin());
    receive_offset_ += count;
    return rs::core::transport::IOResult{count, false};
  }

  void close() noexcept override {}

 private:
  std::size_t send_count_{0};
  std::size_t receive_offset_{0};
};

class StartupParameterTransport final : public rs::core::transport::ITransport {
 public:
  explicit StartupParameterTransport(bool malformed = false) {
    if (malformed) {
      append_message('S', "missing-terminators", 19);
      return;
    }
    append_message('S', "server_version\0" "17.6\0", 20);
    append_message('S', "application_name\0" "odbcpp\0", 24);
    append_message('Z', "I", 1);
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
      std::make_unique<rs::core::database::MockProtocolParser>(),
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
      std::make_unique<StartupParameterTransport>());

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  EXPECT_EQ("17.6", connection.get_parameter("server_version"));
  EXPECT_EQ("odbcpp", connection.get_parameter("application_name"));
  EXPECT_TRUE(connection.get_parameter("missing").empty());
}

TEST(ConnectionLivenessTest, RejectsMalformedStartupParameters) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<
          rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<StartupParameterTransport>(true));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

}  // namespace
