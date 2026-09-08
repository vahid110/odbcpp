#include <gtest/gtest.h>

#include "core/database/postgres/scram_sha256.h"

#include <stdexcept>
#include <string>

namespace {

using rs::core::database::postgres::ScramSha256Client;

constexpr const char* kClientNonce = "rOprNGfwEbeRWgbNEkqO";
constexpr const char* kServerFirst =
    "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
    "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";

TEST(ScramSha256Test, MatchesRfc7677Exchange) {
  ScramSha256Client client("user", "pencil", kClientNonce);
  EXPECT_EQ("n,,n=user,r=rOprNGfwEbeRWgbNEkqO",
            client.client_first_message());
  EXPECT_EQ(
      "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
      "p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=",
      client.receive_server_first(kServerFirst));
  EXPECT_NO_THROW(client.verify_server_final(
      "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="));
}

TEST(ScramSha256Test, EscapesUsername) {
  ScramSha256Client client("comma,user=name", "secret", "nonce");
  EXPECT_EQ("n,,n=comma=2Cuser=3Dname,r=nonce",
            client.client_first_message());
}

TEST(ScramSha256Test, RejectsInvalidServerNonce) {
  ScramSha256Client client("user", "pencil", kClientNonce);
  EXPECT_THROW(client.receive_server_first(
      "r=unrelated,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096"),
      std::runtime_error);
}

TEST(ScramSha256Test, RejectsInvalidIterationCount) {
  ScramSha256Client client("user", "pencil", kClientNonce);
  EXPECT_THROW(client.receive_server_first(
      "r=rOprNGfwEbeRWgbNEkqOserver,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=0"),
      std::runtime_error);
}

TEST(ScramSha256Test, RejectsInvalidServerSignature) {
  ScramSha256Client client("user", "pencil", kClientNonce);
  client.receive_server_first(kServerFirst);
  EXPECT_THROW(client.verify_server_final(
      "v=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="),
      std::runtime_error);
  EXPECT_THROW(client.verify_server_final("e=invalid-proof"),
               std::runtime_error);
}

TEST(ScramSha256Test, GeneratesProtocolSafeNonces) {
  const auto first = rs::core::database::postgres::generate_scram_nonce();
  const auto second = rs::core::database::postgres::generate_scram_nonce();
  EXPECT_FALSE(first.empty());
  EXPECT_EQ(std::string::npos, first.find(','));
  EXPECT_NE(first, second);
}

} // namespace
