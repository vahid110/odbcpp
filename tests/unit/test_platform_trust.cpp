#include <gtest/gtest.h>

#include "odbcpp/security/platform_trust.h"
#include "tests/support/tls_test_pki.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

namespace pki = odbcpp_test::pki;
namespace platform = rs::core::security::platform;
using Kind = platform::UnixTrustLocation::Kind;

struct FakeHost {
  std::set<std::string> files;
  std::set<std::string> directories;
  std::map<std::string, std::string> environment;

  platform::UnixTrustProbe probe() const {
    platform::UnixTrustProbe result;
    result.is_file = [this](const std::string& path) { return files.count(path) != 0; };
    result.is_directory = [this](const std::string& path) {
      return directories.count(path) != 0;
    };
    result.environment = [this](const char* name) -> std::optional<std::string> {
      const auto found = environment.find(name);
      if (found == environment.end()) return std::nullopt;
      return found->second;
    };
    return result;
  }
};

TEST(UnixTrustSelectionTest, PrefersFirstDistributionBundleFile) {
  FakeHost host;
  host.files = {"/etc/ssl/cert.pem", "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
                "/etc/pki/tls/certs/ca-bundle.crt"};
  host.directories = {"/etc/ssl/certs"};
  const auto selected = platform::select_unix_trust_locations(host.probe());
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected[0], (platform::UnixTrustLocation{
                             Kind::File, "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", false}));
}

TEST(UnixTrustSelectionTest, DebianBundleOutranksEveryOtherLocation) {
  FakeHost host;
  host.files = {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/cert.pem"};
  const auto selected = platform::select_unix_trust_locations(host.probe());
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected[0].path, "/etc/ssl/certs/ca-certificates.crt");
}

TEST(UnixTrustSelectionTest, FallsBackToHashedDirectory) {
  FakeHost host;
  host.directories = {"/etc/pki/tls/certs"};
  const auto selected = platform::select_unix_trust_locations(host.probe());
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected[0], (platform::UnixTrustLocation{Kind::Directory, "/etc/pki/tls/certs", false}));
}

TEST(UnixTrustSelectionTest, ReportsMissingStoreInsteadOfProviderDefault) {
  FakeHost host;
  EXPECT_TRUE(platform::select_unix_trust_locations(host.probe()).empty());
}

TEST(UnixTrustSelectionTest, EnvironmentOverrideIsSelectedVerbatimEvenIfMissing) {
  FakeHost host;
  host.files = {"/etc/ssl/certs/ca-certificates.crt"};
  host.environment = {{"SSL_CERT_FILE", "/opt/custom roots/ca.pem"}};
  const auto selected = platform::select_unix_trust_locations(host.probe());
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected[0], (platform::UnixTrustLocation{Kind::File, "/opt/custom roots/ca.pem", true}));
}

TEST(UnixTrustSelectionTest, EnvironmentFileAndDirectoryAreBothSelected) {
  FakeHost host;
  host.environment = {{"SSL_CERT_FILE", "/a.pem"}, {"SSL_CERT_DIR", "/hashed"}};
  const auto selected = platform::select_unix_trust_locations(host.probe());
  ASSERT_EQ(selected.size(), 2u);
  EXPECT_EQ(selected[0].kind, Kind::File);
  EXPECT_EQ(selected[1], (platform::UnixTrustLocation{Kind::Directory, "/hashed", true}));
}

TEST(UnixTrustSelectionTest, EnvironmentDirectoryListIsSplitLikeOpenSsl) {
  FakeHost host;
  host.environment = {{"SSL_CERT_DIR", ":/first::/second dir:"}};
  const auto selected = platform::select_unix_trust_locations(host.probe());
  ASSERT_EQ(selected.size(), 2u);
  EXPECT_EQ(selected[0], (platform::UnixTrustLocation{Kind::Directory, "/first", true}));
  EXPECT_EQ(selected[1], (platform::UnixTrustLocation{Kind::Directory, "/second dir", true}));
}

TEST(UnixTrustSelectionTest, SeparatorOnlyDirectoryOverrideDoesNotFallThrough) {
  FakeHost host;
  host.files = {"/etc/ssl/certs/ca-certificates.crt"};
  host.environment = {{"SSL_CERT_DIR", "::"}};
  const auto selected = platform::select_unix_trust_locations(host.probe());
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected[0], (platform::UnixTrustLocation{Kind::Directory, "::", true}));
}

TEST(UnixTrustSelectionTest, EmptyEnvironmentValuesAreIgnored) {
  FakeHost host;
  host.files = {"/etc/ssl/ca-bundle.pem"};
  host.environment = {{"SSL_CERT_FILE", ""}, {"SSL_CERT_DIR", ""}};
  const auto selected = platform::select_unix_trust_locations(host.probe());
  ASSERT_EQ(selected.size(), 1u);
  EXPECT_EQ(selected[0].path, "/etc/ssl/ca-bundle.pem");
  EXPECT_FALSE(selected[0].from_environment);
}

TEST(PlatformChainEvaluationTest, RejectsMalformedChainShapes) {
  platform::EvaluationOptions options;
  EXPECT_FALSE(platform::evaluate_server_chain({}, options).trusted);
  std::vector<platform::DerCertificate> too_long(
      platform::max_peer_chain_certificates + 1, platform::DerCertificate{0x30});
  const auto long_result = platform::evaluate_server_chain(too_long, options);
  EXPECT_FALSE(long_result.trusted);
  EXPECT_NE(long_result.reason.find("too long"), std::string::npos);
  EXPECT_FALSE(platform::evaluate_server_chain({platform::DerCertificate{}}, options).trusted);
}

class NativeChainEvaluationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!platform::native_verifier_available()) {
      GTEST_SKIP() << "no native verifier; Unix trust uses distribution bundles";
    }
  }

  static platform::EvaluationOptions anchored(const pki::Identity& root) {
    platform::EvaluationOptions options;
    options.network_timeout = std::chrono::milliseconds::zero();
    options.test_anchors.push_back(pki::der(root.cert.get()));
    return options;
  }

  bool anchors_supported() {
#if defined(_WIN32)
    return false;
#else
    return true;
#endif
  }

  pki::Hierarchy hierarchy;
};

TEST_F(NativeChainEvaluationTest, OperatingSystemStoreRejectsPrivateHierarchy) {
  platform::EvaluationOptions options;
  options.network_timeout = std::chrono::milliseconds::zero();
  const auto result = platform::evaluate_server_chain(
      {pki::der(hierarchy.leaf.cert.get()), pki::der(hierarchy.intermediate.cert.get())}, options);
  EXPECT_FALSE(result.trusted);
  EXPECT_FALSE(result.reason.empty());
}

TEST_F(NativeChainEvaluationTest, AcceptsServerChainToInjectedAnchor) {
  if (!anchors_supported()) {
    GTEST_SKIP() << "test anchors are macOS-only";
  }
  const auto result = platform::evaluate_server_chain(
      {pki::der(hierarchy.leaf.cert.get()), pki::der(hierarchy.intermediate.cert.get())},
      anchored(hierarchy.root));
  EXPECT_TRUE(result.trusted) << result.reason;
}

TEST_F(NativeChainEvaluationTest, RequiresPeerSuppliedIntermediateWithoutNetwork) {
  if (!anchors_supported()) {
    GTEST_SKIP() << "test anchors are macOS-only";
  }
  const auto result = platform::evaluate_server_chain(
      {pki::der(hierarchy.leaf.cert.get())}, anchored(hierarchy.root));
  EXPECT_FALSE(result.trusted);
}

TEST_F(NativeChainEvaluationTest, RejectsUnrelatedAnchor) {
  if (!anchors_supported()) {
    GTEST_SKIP() << "test anchors are macOS-only";
  }
  const auto unrelated = pki::issue({"Unrelated Root", true, {}, {}, ""}, nullptr);
  const auto result = platform::evaluate_server_chain(
      {pki::der(hierarchy.leaf.cert.get()), pki::der(hierarchy.intermediate.cert.get())},
      anchored(unrelated));
  EXPECT_FALSE(result.trusted);
}

TEST_F(NativeChainEvaluationTest, RejectsCertificateWithoutServerAuthenticationPurpose) {
  if (!anchors_supported()) {
    GTEST_SKIP() << "test anchors are macOS-only";
  }
  const auto client_only = pki::issue(
      {"db.example.test", false, {"db.example.test"}, {}, "clientAuth"}, &hierarchy.intermediate);
  const auto result = platform::evaluate_server_chain(
      {pki::der(client_only.cert.get()), pki::der(hierarchy.intermediate.cert.get())},
      anchored(hierarchy.root));
  EXPECT_FALSE(result.trusted);
}

TEST_F(NativeChainEvaluationTest, RejectsNonCaIntermediate) {
  if (!anchors_supported()) {
    GTEST_SKIP() << "test anchors are macOS-only";
  }
  const auto not_ca = pki::issue({"Not A CA", false, {}, {}, "serverAuth"}, &hierarchy.root);
  const auto leaf = pki::issue({"db.example.test", false, {"db.example.test"}, {}}, &not_ca);
  const auto result = platform::evaluate_server_chain(
      {pki::der(leaf.cert.get()), pki::der(not_ca.cert.get())}, anchored(hierarchy.root));
  EXPECT_FALSE(result.trusted);
}

TEST_F(NativeChainEvaluationTest, RejectsExpiredLeaf) {
  if (!anchors_supported()) {
    GTEST_SKIP() << "test anchors are macOS-only";
  }
  const auto expired = pki::issue(
      {"db.example.test", false, {"db.example.test"}, {}, "serverAuth", -7200, -3600},
      &hierarchy.intermediate);
  const auto result = platform::evaluate_server_chain(
      {pki::der(expired.cert.get()), pki::der(hierarchy.intermediate.cert.get())},
      anchored(hierarchy.root));
  EXPECT_FALSE(result.trusted);
}

TEST_F(NativeChainEvaluationTest, RejectsUndecodableCertificate) {
  const auto result = platform::evaluate_server_chain(
      {platform::DerCertificate{0x30, 0x03, 0x02, 0x01, 0x01}}, platform::EvaluationOptions{});
  EXPECT_FALSE(result.trusted);
}

}  // namespace
