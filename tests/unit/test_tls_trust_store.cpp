#include <gtest/gtest.h>

#include "odbcpp/security/platform_trust.h"
#include "odbcpp/security/tls_client.h"
#include "tests/support/tls_test_pki.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace {

namespace pki = odbcpp_test::pki;
using rs::core::security::TlsClient;
using rs::core::security::TlsClientConfig;
using rs::core::security::TlsStep;
using rs::core::security::TlsStepState;

TlsClientConfig custom_file(const std::filesystem::path& file) {
  TlsClientConfig config;
  config.ca_file = pki::to_utf8(file);
  return config;
}

TlsClientConfig custom_directory(const std::filesystem::path& directory) {
  TlsClientConfig config;
  config.ca_directory = pki::to_utf8(directory);
  return config;
}

bool contains(const TlsStep& step, const std::string& text) {
  return step.message.find(text) != std::string::npos;
}

std::filesystem::path hash_entry(const std::filesystem::path& directory,
                                unsigned long hash, std::size_t suffix,
                                const char* kind = "") {
  std::array<char, 32> name{};
  std::snprintf(name.data(), name.size(), "%08lx.%s%zu", hash, kind, suffix);
  return directory / name.data();
}

class TlsTrustStoreTest : public ::testing::Test {
 protected:
  pki::Hierarchy hierarchy;
  pki::Identity unrelated_root = pki::issue({"Unrelated Root", true, {}, {}, ""}, nullptr);
  pki::TempDirectory temp{"client"};
  std::filesystem::path root_file = temp.path() / pki::utf8("root CA \xE2\x9C\x93.pem");
  std::filesystem::path unrelated_file = temp.path() / "unrelated.pem";

  void SetUp() override {
    pki::write_file(root_file, pki::pem(hierarchy.root.cert.get()));
    pki::write_file(unrelated_file, pki::pem(unrelated_root.cert.get()));
  }

  TlsStep connect(TlsClient& client, const std::string& host = "db.example.test") {
    pki::MemoryServer server(hierarchy.leaf, {&hierarchy.intermediate});
    return server.connect(client, host);
  }
};

TEST_F(TlsTrustStoreTest, CustomFileWithUnicodeAndSpacePathVerifiesChainAndHost) {
  TlsClient client;
  client.configure(custom_file(root_file));
  const auto step = connect(client);
  EXPECT_EQ(step.state, TlsStepState::Complete) << step.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, CustomFileVerifiesIpAddressSubjectAlternativeName) {
  TlsClient client;
  client.configure(custom_file(root_file));
  const auto step = connect(client, "127.0.0.1");
  EXPECT_EQ(step.state, TlsStepState::Complete) << step.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, CustomFileRejectsHostAndIpMismatch) {
  for (const std::string host : {"other.example.test", "127.0.0.2", "example.test"}) {
    TlsClient client;
    client.configure(custom_file(root_file));
    const auto step = connect(client, host);
    EXPECT_EQ(step.state, TlsStepState::TlsError) << host;
    EXPECT_TRUE(contains(step, "hostname verification failed")) << host << ": " << step.message;
    EXPECT_FALSE(client.peer_identity_verified()) << host;
  }
}

TEST_F(TlsTrustStoreTest, CustomFileRejectsUnrelatedCa) {
  TlsClient client;
  client.configure(custom_file(unrelated_file));
  const auto step = connect(client);
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_TRUE(contains(step, "certificate verify failed")) << step.message;
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, IntermediateAloneIsNotATrustAnchor) {
  const auto intermediate_file = temp.path() / "intermediate.pem";
  pki::write_file(intermediate_file, pki::pem(hierarchy.intermediate.cert.get()));
  TlsClient client;
  client.configure(custom_file(intermediate_file));
  const auto step = connect(client);
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, BundleMayCarryMissingIntermediateWithItsRoot) {
  const auto bundle = temp.path() / "bundle.pem";
  pki::write_file(bundle, "comment text is ignored\n" +
                              pki::pem(hierarchy.intermediate.cert.get()) +
                              pki::pem(unrelated_root.cert.get()) +
                              pki::pem(hierarchy.root.cert.get()));
  TlsClient client;
  client.configure(custom_file(bundle));
  pki::MemoryServer leaf_only(hierarchy.leaf, {});
  const auto step = leaf_only.connect(client, "db.example.test");
  EXPECT_EQ(step.state, TlsStepState::Complete) << step.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, NonCaIssuerIsRejected) {
  const auto fake_ca = pki::issue({"Not A CA", false, {}, {}, "serverAuth"}, &hierarchy.root);
  const auto leaf = pki::issue({"db.example.test", false, {"db.example.test"}, {}}, &fake_ca);
  TlsClient client;
  client.configure(custom_file(root_file));
  pki::MemoryServer server(leaf, {&fake_ca});
  const auto step = server.connect(client, "db.example.test");
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, ExpiredServerCertificateIsRejected) {
  const auto expired = pki::issue(
      {"db.example.test", false, {"db.example.test"}, {}, "serverAuth", -7200, -3600},
      &hierarchy.intermediate);
  TlsClient client;
  client.configure(custom_file(root_file));
  pki::MemoryServer server(expired, {&hierarchy.intermediate});
  const auto step = server.connect(client, "db.example.test");
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, InvalidCustomFilesFailBeforeAnyHandshake) {
  const auto missing = temp.path() / "missing.pem";
  const auto empty = temp.path() / "empty.pem";
  const auto garbage = temp.path() / "garbage.pem";
  const auto truncated = temp.path() / "truncated.pem";
  pki::write_file(empty, "");
  pki::write_file(garbage, "not a certificate\n");
  const auto root_pem = pki::pem(hierarchy.root.cert.get());
  pki::write_file(truncated, root_pem.substr(0, root_pem.size() / 2));
  struct Case {
    std::filesystem::path path;
    const char* reason;
  };
  for (const auto& test : {Case{missing, "cannot open file: error:"},
                           Case{temp.path(), "not a regular file"},
                           Case{empty, "file is empty"},
                           Case{garbage, "no PEM certificates found"},
                           Case{truncated, "malformed PEM data"}}) {
    TlsClient client;
    client.configure(custom_file(test.path));
    const auto step = client.begin_memory("db.example.test");
    EXPECT_EQ(step.state, TlsStepState::TlsError) << pki::to_utf8(test.path);
    EXPECT_TRUE(contains(step, "Failed to load CA file: ")) << step.message;
    EXPECT_TRUE(contains(step, test.reason)) << step.message;
    EXPECT_FALSE(client.active());
  }
}

TEST_F(TlsTrustStoreTest, InvalidCustomFileNeverFallsBackToSystemTrust) {
  TlsClient client;
  client.configure(custom_file(temp.path() / "missing.pem"));
  EXPECT_EQ(client.begin_memory("db.example.test").state, TlsStepState::TlsError);
  // The same client stays failed until reconfigured; no implicit default.
  EXPECT_EQ(client.begin_memory("db.example.test").state, TlsStepState::TlsError);
}

TEST_F(TlsTrustStoreTest, HashedDirectoryVerifiesAndMissingDirectoryFails) {
  const auto directory = temp.path() / pki::utf8("hashed dir \xC3\xBC");
  std::filesystem::create_directories(directory);
  pki::write_hashed(directory, hierarchy.root.cert.get());
  {
    TlsClient client;
    client.configure(custom_directory(directory));
    const auto step = connect(client);
    EXPECT_EQ(step.state, TlsStepState::Complete) << step.message;
    EXPECT_TRUE(client.peer_identity_verified());
  }
  {
    TlsClient client;
    client.configure(custom_directory(temp.path() / "no such dir"));
    const auto step = client.begin_memory("db.example.test");
    EXPECT_EQ(step.state, TlsStepState::TlsError);
    EXPECT_TRUE(contains(step, "Failed to load CA directory: ")) << step.message;
  }
  {
    TlsClient client;
    client.configure(custom_directory(root_file));
    const auto step = client.begin_memory("db.example.test");
    EXPECT_EQ(step.state, TlsStepState::TlsError);
    EXPECT_TRUE(contains(step, "not an existing directory")) << step.message;
  }
}

TEST_F(TlsTrustStoreTest, DirectoryWithoutMatchingHashRejectsPeer) {
  const auto directory = temp.path() / "unrelated dir";
  std::filesystem::create_directories(directory);
  pki::write_hashed(directory, unrelated_root.cert.get());
  TlsClient client;
  client.configure(custom_directory(directory));
  const auto step = connect(client);
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, HashedDirectorySupportsNonAcpUnicodePath) {
  const auto directory = temp.path() / pki::utf8("hash \xE8\xA1\xA8 \xF0\x9F\x8C\x90");
  std::filesystem::create_directories(directory);
  pki::write_hashed(directory, hierarchy.root.cert.get());
  TlsClient client;
  client.configure(custom_directory(directory));
  const auto step = connect(client);
  EXPECT_EQ(step.state, TlsStepState::Complete) << step.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, HashedDirectoryLoadsContiguousSubjectCollisions) {
  const auto directory = temp.path() / "collisions";
  std::filesystem::create_directories(directory);
  const auto collision = pki::issue({"ODBCPP Test Root", true, {}, {}, ""}, nullptr);
  ASSERT_EQ(::X509_NAME_cmp(::X509_get_subject_name(collision.cert.get()),
                             ::X509_get_subject_name(hierarchy.root.cert.get())), 0);
  const auto hash = ::X509_subject_name_hash(hierarchy.root.cert.get());
  ASSERT_EQ(hash, ::X509_subject_name_hash(collision.cert.get()));
  pki::write_file(hash_entry(directory, hash, 0), pki::pem(collision.cert.get()));
  {
    TlsClient wrong_key;
    wrong_key.configure(custom_directory(directory));
    EXPECT_EQ(connect(wrong_key).state, TlsStepState::TlsError);
    EXPECT_FALSE(wrong_key.peer_identity_verified());
  }
  pki::write_file(hash_entry(directory, hash, 1), pki::pem(hierarchy.root.cert.get()));
  TlsClient client;
  client.configure(custom_directory(directory));
  const auto step = connect(client);
  EXPECT_EQ(step.state, TlsStepState::Complete) << step.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, HashedDirectoryStopsAtGapAndRetriesMissingEntryLazily) {
  const auto directory = temp.path() / "gap";
  std::filesystem::create_directories(directory);
  const auto hash = ::X509_subject_name_hash(hierarchy.root.cert.get());
  pki::write_file(hash_entry(directory, hash, 1), pki::pem(hierarchy.root.cert.get()));
  TlsClient client;
  client.configure(custom_directory(directory));
  EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
  client.reset_session();
  std::filesystem::rename(hash_entry(directory, hash, 1), hash_entry(directory, hash, 0));
  const auto accepted = connect(client);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, HashedDirectoryIgnoresWrongHashAndCrlSuffixForCertificates) {
  const auto directory = temp.path() / "wrong names";
  std::filesystem::create_directories(directory);
  const auto hash = ::X509_subject_name_hash(hierarchy.root.cert.get());
  const auto pem = pki::pem(hierarchy.root.cert.get());
  pki::write_file(hash_entry(directory, hash ^ 1UL, 0), pem);
  pki::write_file(hash_entry(directory, hash, 0, "r"), pem);
  pki::write_file(directory / "root.pem", pem);
  TlsClient client;
  client.configure(custom_directory(directory));
  EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, MalformedHashEntriesCannotSupplyTheMissingIssuer) {
  const auto directory = temp.path() / "malformed without issuer";
  std::filesystem::create_directories(directory);
  const auto hash = ::X509_subject_name_hash(hierarchy.root.cert.get());
  pki::write_file(hash_entry(directory, hash, 0), "not a certificate\n");
  {
    TlsClient client;
    client.configure(custom_directory(directory));
    EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
    EXPECT_FALSE(client.peer_identity_verified());
  }
  const auto collision = pki::issue({"ODBCPP Test Root", true, {}, {}, ""}, nullptr);
  ASSERT_EQ(hash, ::X509_subject_name_hash(collision.cert.get()));
  pki::write_file(hash_entry(directory, hash, 0), pki::pem(collision.cert.get()) +
      "-----BEGIN CERTIFICATE-----\nnot valid base64!\n-----END CERTIFICATE-----\n");
  TlsClient wrong_key;
  wrong_key.configure(custom_directory(directory));
  EXPECT_EQ(connect(wrong_key).state, TlsStepState::TlsError);
  EXPECT_FALSE(wrong_key.peer_identity_verified());
}

#ifdef _WIN32
// These fail-stop assertions specify the SDK's Windows UTF-8 directory lookup.
// POSIX uses native OpenSSL: newer releases can skip malformed hash files and
// load a valid later suffix. That is not acceptance of the malformed bytes.
TEST_F(TlsTrustStoreTest, HashedDirectoryStopsAtMalformedEntry) {
  const auto directory = temp.path() / "malformed prefix";
  std::filesystem::create_directories(directory);
  const auto hash = ::X509_subject_name_hash(hierarchy.root.cert.get());
  pki::write_file(hash_entry(directory, hash, 0), "not a certificate\n");
  pki::write_file(hash_entry(directory, hash, 1), pki::pem(hierarchy.root.cert.get()));
  TlsClient client;
  client.configure(custom_directory(directory));
  EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, HashedDirectoryStopsAfterValidCollisionWithMalformedTail) {
  const auto directory = temp.path() / "malformed tail";
  std::filesystem::create_directories(directory);
  const auto collision = pki::issue({"ODBCPP Test Root", true, {}, {}, ""}, nullptr);
  const auto hash = ::X509_subject_name_hash(hierarchy.root.cert.get());
  ASSERT_EQ(hash, ::X509_subject_name_hash(collision.cert.get()));
  const auto malformed = pki::pem(collision.cert.get()) +
      "-----BEGIN CERTIFICATE-----\nnot valid base64!\n-----END CERTIFICATE-----\n";
  pki::write_file(hash_entry(directory, hash, 0), malformed);
  pki::write_file(hash_entry(directory, hash, 1), pki::pem(hierarchy.root.cert.get()));
  TlsClient client;
  client.configure(custom_directory(directory));
  EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
  // A fresh context with a well-formed prefix can load the next suffix;
  // do not require OpenSSL to invalidate a previously cached subject.
  pki::write_file(hash_entry(directory, hash, 0), pki::pem(collision.cert.get()));
  TlsClient repaired;
  repaired.configure(custom_directory(directory));
  const auto accepted = connect(repaired);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(repaired.peer_identity_verified());
}
#endif

TEST_F(TlsTrustStoreTest, HashedDirectoryReadsNewFileAfterContextCreation) {
  const auto directory = temp.path() / "late issuer";
  std::filesystem::create_directories(directory);
  TlsClient client;
  client.configure(custom_directory(directory));
  ASSERT_EQ(client.begin_memory("db.example.test").state, TlsStepState::Complete);
  pki::write_hashed(directory, hierarchy.root.cert.get());
  const auto accepted = connect(client);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, HashedDirectorySiblingCopiesOnlyLoadedTrust) {
  const auto directory = temp.path() / "snapshot only used issuer";
  std::filesystem::create_directories(directory);
  pki::write_hashed(directory, hierarchy.root.cert.get());
  pki::write_hashed(directory, unrelated_root.cert.get());
  TlsClient client;
  client.configure(custom_directory(directory));
  ASSERT_EQ(connect(client).state, TlsStepState::Complete);
  auto sibling = client.verified_sibling();
  ASSERT_NE(sibling, nullptr);
  EXPECT_TRUE(sibling->frozen_context());
  std::filesystem::remove_all(directory);
  const auto accepted = connect(*sibling);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(sibling->peer_identity_verified());
  sibling->reset_session();
  pki::MemoryServer unrelated(
      pki::issue({"db.example.test", false, {"db.example.test"}, {}}, &unrelated_root), {});
  const auto rejected = unrelated.connect(*sibling, "db.example.test");
  EXPECT_EQ(rejected.state, TlsStepState::TlsError);
  EXPECT_FALSE(sibling->peer_identity_verified());
}

#if defined(_WIN32)
TEST_F(TlsTrustStoreTest, HashedDirectoryEntryLimitCannotBeBypassedByCachedCertificate) {
  const auto directory = temp.path() / "bounded suffixes";
  std::filesystem::create_directories(directory);
  const auto hash = ::X509_subject_name_hash(hierarchy.root.cert.get());
  const auto pem = pki::pem(hierarchy.root.cert.get());
  for (std::size_t suffix = 0; suffix <= 256; ++suffix) {
    pki::write_file(hash_entry(directory, hash, suffix), pem);
  }
  TlsClient client;
  client.configure(custom_directory(directory));
  const auto rejected = connect(client);
  EXPECT_EQ(rejected.state, TlsStepState::TlsError);
  EXPECT_TRUE(contains(rejected, "CA directory lookup")) << rejected.message;
  EXPECT_FALSE(client.peer_identity_verified());
  client.reset_session();
  ASSERT_TRUE(std::filesystem::remove(hash_entry(directory, hash, 256)));
  EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
  // An explicit new context can use the now-valid bounded sequence.
  client.configure(custom_directory(directory));
  const auto accepted = connect(client);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, HashedDirectoryReadBudgetCannotBeBypassedByCachedCertificate) {
  const auto directory = temp.path() / "bounded input";
  std::filesystem::create_directories(directory);
  const auto hash = ::X509_subject_name_hash(hierarchy.root.cert.get());
  pki::write_file(hash_entry(directory, hash, 0), pki::pem(hierarchy.root.cert.get()));
  pki::write_file(hash_entry(directory, hash, 1), std::string(16 * 1024 * 1024, ' '));
  TlsClient client;
  client.configure(custom_directory(directory));
  const auto rejected = connect(client);
  EXPECT_EQ(rejected.state, TlsStepState::TlsError);
  EXPECT_TRUE(contains(rejected, "CA directory lookup")) << rejected.message;
  EXPECT_FALSE(client.peer_identity_verified());
  client.reset_session();
  ASSERT_TRUE(std::filesystem::remove(hash_entry(directory, hash, 1)));
  EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
  client.configure(custom_directory(directory));
  const auto accepted = connect(client);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(client.peer_identity_verified());
}
#endif

TEST_F(TlsTrustStoreTest, FileAndDirectoryTogetherAreRejected) {
  TlsClientConfig config = custom_file(root_file);
  config.ca_directory = pki::to_utf8(temp.path());
  TlsClient client;
  client.configure(config);
  const auto step = client.begin_memory("db.example.test");
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_TRUE(contains(step, "cannot both be specified")) << step.message;
}

TEST_F(TlsTrustStoreTest, ReconfiguredConnectionRereadsRotatedBundle) {
  const auto rotating = temp.path() / "rotating.pem";
  pki::write_file(rotating, pki::pem(unrelated_root.cert.get()));
  TlsClient client;
  client.configure(custom_file(rotating));
  EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
  client.reset_session();

  pki::write_file(rotating, pki::pem(hierarchy.root.cert.get()));
  client.configure(custom_file(rotating));
  const auto accepted = connect(client);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  client.reset_session();

  pki::write_file(rotating, pki::pem(unrelated_root.cert.get()));
  client.configure(custom_file(rotating));
  EXPECT_EQ(connect(client).state, TlsStepState::TlsError);
}

TEST_F(TlsTrustStoreTest, VerifiedSiblingKeepsCustomTrustSnapshot) {
  const auto snapshot = temp.path() / "snapshot.pem";
  pki::write_file(snapshot, pki::pem(hierarchy.root.cert.get()));
  TlsClient client;
  client.configure(custom_file(snapshot));
  ASSERT_EQ(connect(client).state, TlsStepState::Complete);
  auto sibling = client.verified_sibling();
  ASSERT_NE(sibling, nullptr);
  EXPECT_TRUE(sibling->frozen_context());
  std::filesystem::remove(snapshot);

  const auto step = connect(*sibling);
  EXPECT_EQ(step.state, TlsStepState::Complete) << step.message;
  EXPECT_TRUE(sibling->peer_identity_verified());
  sibling->reset_session();
  EXPECT_EQ(connect(*sibling, "other.example.test").state, TlsStepState::TlsError);
}

TEST_F(TlsTrustStoreTest, SystemTrustRejectsPrivateCa) {
  if (!rs::core::security::platform::native_verifier_available() &&
      rs::core::security::platform::select_unix_trust_locations(
          rs::core::security::platform::default_unix_trust_probe()).empty()) {
    GTEST_SKIP() << "no operating system trust store on this host";
  }
  TlsClient client;
  client.configure(TlsClientConfig{});
  const auto step = connect(client);
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
  if (rs::core::security::platform::native_verifier_available()) {
    EXPECT_TRUE(contains(step, "operating system trust store")) << step.message;
  }
}

#if defined(__APPLE__)
// Exercises the TLS client's OS-verifier callback end to end (chain export,
// leaf deduplication, verify result) with injected anchors; OS policy applies.
class NativeVerifierHandshakeTest : public TlsTrustStoreTest {
 protected:
  void anchor_client(TlsClient& client, const pki::Identity& root) {
    client.configure(TlsClientConfig{});
    client.replace_platform_anchors_for_testing({pki::der(root.cert.get())});
  }
};

TEST_F(NativeVerifierHandshakeTest, AcceptsChainToAnchorAndKeepsHostCheck) {
  TlsClient client;
  anchor_client(client, hierarchy.root);
  const auto accepted = connect(client);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(client.peer_identity_verified());
  client.reset_session();
  const auto ip = connect(client, "127.0.0.1");
  EXPECT_EQ(ip.state, TlsStepState::Complete) << ip.message;
  client.reset_session();
  const auto mismatch = connect(client, "other.example.test");
  EXPECT_EQ(mismatch.state, TlsStepState::TlsError);
  EXPECT_TRUE(contains(mismatch, "hostname verification failed")) << mismatch.message;
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(NativeVerifierHandshakeTest, RejectsUnrelatedAnchorAndWrongPurpose) {
  {
    TlsClient client;
    anchor_client(client, unrelated_root);
    const auto step = connect(client);
    EXPECT_EQ(step.state, TlsStepState::TlsError);
    EXPECT_TRUE(contains(step, "operating system trust store")) << step.message;
  }
  {
    const auto client_only = pki::issue(
        {"db.example.test", false, {"db.example.test"}, {}, "clientAuth"}, &hierarchy.intermediate);
    TlsClient client;
    anchor_client(client, hierarchy.root);
    pki::MemoryServer server(client_only, {&hierarchy.intermediate});
    const auto step = server.connect(client, "db.example.test");
    EXPECT_EQ(step.state, TlsStepState::TlsError);
    EXPECT_FALSE(client.peer_identity_verified());
  }
}

// In-memory handshake with an explicit verification deadline, set immediately
// before the handshake so that it elapses while the OS evaluates the chain.
TlsStep handshake_with_deadline(TlsClient& client, const pki::Identity& leaf,
                                const pki::Identity& intermediate,
                                std::chrono::nanoseconds budget) {
  SSL_CTX* context = ::SSL_CTX_new(::TLS_server_method());
  ::SSL_CTX_use_certificate(context, leaf.cert.get());
  ::SSL_CTX_use_PrivateKey(context, leaf.key.get());
  ::SSL_CTX_add1_chain_cert(context, intermediate.cert.get());
  SSL* server = ::SSL_new(context);
  BIO* input = ::BIO_new(::BIO_s_mem());
  BIO* output = ::BIO_new(::BIO_s_mem());
  ::SSL_set_bio(server, input, output);
  ::SSL_set_accept_state(server);
  TlsStep step{TlsStepState::TlsError, 0, 0, "handshake did not converge"};
  if (client.begin_memory("db.example.test").state == TlsStepState::Complete) {
    client.set_verification_deadline(std::chrono::steady_clock::now() + budget);
    std::array<std::byte, 16384> buffer{};
    for (int round = 0; round < 64; ++round) {
      step = client.handshake();
      for (;;) {
        const auto drained = client.drain_ciphertext(buffer);
        if (drained.state != TlsStepState::Complete || drained.processed == 0) break;
        ::BIO_write(input, buffer.data(), static_cast<int>(drained.processed));
      }
      if (step.state != TlsStepState::WantRead && step.state != TlsStepState::WantWrite) break;
      (void)::SSL_do_handshake(server);
      for (int count = 0; (count = ::BIO_read(output, buffer.data(), static_cast<int>(buffer.size()))) > 0;) {
        client.provide_ciphertext(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count)));
      }
    }
  }
  ::SSL_free(server);
  ::SSL_CTX_free(context);
  return step;
}

TEST_F(NativeVerifierHandshakeTest, TrustedVerdictAfterDeadlineIsRejected) {
  TlsClient control;
  anchor_client(control, hierarchy.root);
  const auto accepted = handshake_with_deadline(control, hierarchy.leaf, hierarchy.intermediate,
                                                std::chrono::seconds(10));
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(control.peer_identity_verified());

  TlsClient late;
  anchor_client(late, hierarchy.root);
  const auto rejected = handshake_with_deadline(late, hierarchy.leaf, hierarchy.intermediate,
                                                std::chrono::nanoseconds(1));
  EXPECT_EQ(rejected.state, TlsStepState::TlsError);
  EXPECT_TRUE(contains(rejected, "verification completed after the connection deadline"))
      << rejected.message;
  EXPECT_FALSE(late.peer_identity_verified());
}

TEST_F(NativeVerifierHandshakeTest, MissingIntermediateIsNotFetched) {
  TlsClient client;
  anchor_client(client, hierarchy.root);
  pki::MemoryServer leaf_only(hierarchy.leaf, {});
  const auto step = leaf_only.connect(client, "db.example.test");
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_FALSE(client.peer_identity_verified());
}

TEST_F(NativeVerifierHandshakeTest, SiblingReevaluatesThroughOperatingSystemPolicy) {
  TlsClient client;
  anchor_client(client, hierarchy.root);
  ASSERT_EQ(connect(client).state, TlsStepState::Complete);
  auto sibling = client.verified_sibling();
  ASSERT_NE(sibling, nullptr);
  EXPECT_TRUE(sibling->frozen_context());
  const auto accepted = connect(*sibling);
  EXPECT_EQ(accepted.state, TlsStepState::Complete) << accepted.message;
  EXPECT_TRUE(sibling->peer_identity_verified());
  sibling->reset_session();
  pki::MemoryServer impostor(
      pki::issue({"db.example.test", false, {"db.example.test"}, {}}, &unrelated_root), {});
  const auto rejected = impostor.connect(*sibling, "db.example.test");
  EXPECT_EQ(rejected.state, TlsStepState::TlsError);
  EXPECT_TRUE(contains(rejected, "operating system trust store")) << rejected.message;
}
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const std::string& value) : name_(name) {
    if (const char* previous = std::getenv(name)) previous_ = previous;
    ::setenv(name, value.c_str(), 1);
  }
  ~ScopedEnvironment() {
    if (previous_) {
      ::setenv(name_, previous_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

 private:
  const char* name_;
  std::optional<std::string> previous_;
};

TEST_F(TlsTrustStoreTest, UnixSystemTrustHonorsAdministratorBundleOverride) {
  ScopedEnvironment file("SSL_CERT_FILE", pki::to_utf8(root_file));
  TlsClient client;
  client.configure(TlsClientConfig{});
  const auto step = connect(client);
  EXPECT_EQ(step.state, TlsStepState::Complete) << step.message;
  EXPECT_TRUE(client.peer_identity_verified());
}

TEST_F(TlsTrustStoreTest, UnixSystemTrustOverrideFailureDoesNotFallThrough) {
  ScopedEnvironment file("SSL_CERT_FILE", pki::to_utf8(temp.path() / "absent.pem"));
  TlsClient client;
  client.configure(TlsClientConfig{});
  const auto step = client.begin_memory("db.example.test");
  EXPECT_EQ(step.state, TlsStepState::TlsError);
  EXPECT_TRUE(contains(step, "Failed to load operating system trust store")) << step.message;
}
#endif

}  // namespace
