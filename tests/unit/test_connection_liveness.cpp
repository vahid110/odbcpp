#include <gtest/gtest.h>

#include "core/database/generic_database_connection.h"
#include "core/database/mock_protocol_parser.h"
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

}  // namespace
