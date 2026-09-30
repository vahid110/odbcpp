#include <gtest/gtest.h>

#include "core/security/tls_client.h"

#include <array>
#include <string_view>

namespace {

using rs::core::security::TlsClient;
using rs::core::security::TlsClientConfig;
using rs::core::security::TlsStepState;

TEST(TlsClientTest, RejectsOperationsWithoutAnActiveSession) {
  TlsClient client;
  std::array<std::byte, 16> buffer{};

  EXPECT_EQ(client.handshake().state, TlsStepState::TlsError);
  EXPECT_EQ(client.read(buffer).state, TlsStepState::TlsError);
  EXPECT_EQ(client.write(buffer).state, TlsStepState::TlsError);
  EXPECT_EQ(client.drain_ciphertext(buffer).state, TlsStepState::TlsError);
  EXPECT_EQ(client.provide_ciphertext(buffer).state, TlsStepState::TlsError);
}

TEST(TlsClientTest, MemorySessionProducesAClientHandshakeFlight) {
  TlsClient client;
  TlsClientConfig config;
  config.verify_peer = false;
  config.verify_hostname = false;
  client.configure(config);

  ASSERT_EQ(client.begin_memory("db.example.test").state,
            TlsStepState::Complete);
  EXPECT_EQ(client.handshake().state, TlsStepState::WantRead);
  ASSERT_TRUE(client.ciphertext_pending());

  std::array<std::byte, 4096> output{};
  const auto drained = client.drain_ciphertext(output);
  EXPECT_EQ(drained.state, TlsStepState::Complete);
  EXPECT_GT(drained.processed, 0u);
}

TEST(TlsClientTest, RejectsEmbeddedNulBeforeCreatingASession) {
  TlsClient client;
  TlsClientConfig config;
  config.verify_peer = false;
  client.configure(config);
  constexpr char malformed[] = "db.example.test\0unexpected";

  const auto result = client.begin_memory(
      std::string_view(malformed, sizeof(malformed) - 1));
  EXPECT_EQ(result.state, TlsStepState::TlsError);
  EXPECT_FALSE(client.active());
}

}  // namespace
