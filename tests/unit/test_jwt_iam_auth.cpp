#include <gtest/gtest.h>
#include "odbcpp/auth/aws/jwt_iam_auth.h"
#include "odbcpp/auth/aws/serverless_credentials_owner.h"
#include "odbcpp/auth/aws/sts_assume_role_owner.h"
#include "odbcpp/auth/aws/sts_serverless_fake_http.h"
#include <chrono>
#include <string>
#include <thread>
using namespace std::chrono_literals;
namespace a = rs::core::auth;
namespace jwt = a::aws::jwt_iam;
namespace sts = a::aws::sts;
namespace sl = a::aws::serverless;
namespace detail = a::aws::detail;
namespace pn = a::aws::provisioned_native;
namespace {
constexpr const char* role_arn = "arn:aws:iam::123456789012:role/JwtRole";
a::SecretBytes secret(std::string_view text) {
  auto made = a::SecretBytes::create(std::as_bytes(std::span{text.data(), text.size()}));
  if (!made) std::terminate();
  return std::move(made).value();
}
std::string base64url(std::string_view text) {
  static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  unsigned buffer = 0;
  int bits = 0;
  for (unsigned char c : text) {
    buffer = (buffer << 8) | c;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      out.push_back(alphabet[(buffer >> bits) & 63]);
    }
  }
  if (bits > 0) out.push_back(alphabet[(buffer << (6 - bits)) & 63]);
  return out;
}
// Unsigned synthetic JWT; only STS verifies signatures, and the fake never does.
a::SecretBytes token(std::string_view payload) {
  const std::string jwt = base64url(R"({"alg":"RS256","typ":"JWT"})") + "." + base64url(payload) + ".c2lnbmF0dXJl";
  return secret(jwt);
}
rs::util::Deadline deadline_ms(long long ms) { return rs::util::Clock::now() + std::chrono::milliseconds(ms); }
a::StsAssumeRoleTarget target() {
  return {a::StsAssumeRolePartition::Aws, "123456789012", "eu-north-1", "JwtRole", "jwt_redshift_session", 900};
}
std::shared_ptr<pn::WorkerCancellation> cancel() { return std::make_shared<pn::WorkerCancellation>(); }
} // namespace

TEST(JwtDbUserClaim, FollowsNativeDriverClaimOrder) {
  EXPECT_EQ(std::optional<std::string>{"db_alice"},
      jwt::jwt_db_user(token(R"({"email":"a@example.com","upn":"upn_alice","DbUser":"db_alice"})")));
  EXPECT_EQ(std::optional<std::string>{"upn_alice"},
      jwt::jwt_db_user(token(R"({"email":"a@example.com","preferred_username":"pref","upn":"upn_alice"})")));
  EXPECT_EQ(std::optional<std::string>{"pref"},
      jwt::jwt_db_user(token(R"({"email":"a@example.com","preferred_username":"pref"})")));
  EXPECT_EQ(std::optional<std::string>{"a@example.com"}, jwt::jwt_db_user(token(R"({"email":"a@example.com"})")));
}

TEST(JwtDbUserClaim, SkipsEmptyAndNonStringClaims) {
  EXPECT_EQ(std::optional<std::string>{"a@example.com"},
      jwt::jwt_db_user(token(R"({"DbUser":"","upn":42,"preferred_username":null,"email":"a@example.com"})")));
}

TEST(JwtDbUserClaim, RefusesMalformedTokensAndUnusableClaims) {
  EXPECT_FALSE(jwt::jwt_db_user(token(R"({"sub":"no-db-user-claim"})")));
  EXPECT_FALSE(jwt::jwt_db_user(token("not json")));
  EXPECT_FALSE(jwt::jwt_db_user(token(R"(["DbUser"])")));
  EXPECT_FALSE(jwt::jwt_db_user(token(std::string(R"({"DbUser":")") + std::string(129, 'u') + "\"}")));
  EXPECT_FALSE(jwt::jwt_db_user(token(R"({"DbUser":"bad\u0001name"})")));
  EXPECT_FALSE(jwt::jwt_db_user(secret("only.two")));
  EXPECT_FALSE(jwt::jwt_db_user(secret("a.b.c.d")));
  EXPECT_FALSE(jwt::jwt_db_user(secret("e30.!!!.sig")));
  EXPECT_FALSE(jwt::jwt_db_user(secret("")));
}

// The web identity call is sent unsigned, to the pinned STS endpoint, once.
TEST(StsWebIdentityOwner, UnsignedApprovedSendYieldsAssumedCredentials) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = sts::StsAssumeRoleOwner::create_web_identity(target(), token(R"({"email":"a@example.com"})"),
      deadline_ms(20000), cancel());
  ASSERT_TRUE(made.owner);
  auto result = made.owner->acquire();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().access_key_id.empty());
  EXPECT_FALSE(result.value().session_token.empty());
  ASSERT_TRUE(result.value().expiration_utc.has_value());
  const auto& obs = detail::fake_http_observation();
  EXPECT_EQ(1U, obs.sends.load());
  EXPECT_EQ(1U, obs.approved.load());
  EXPECT_EQ(1U, obs.web_identity.load());
  EXPECT_EQ(0U, obs.signed_requests.load());
  EXPECT_EQ(0U, obs.foreign_scheme.load());
  EXPECT_EQ(0U, obs.foreign_authority.load());
  EXPECT_TRUE(made.owner->close());
}

// The ordinary AssumeRole path stays signed and is not a web identity call.
TEST(StsWebIdentityOwner, AssumeRoleRemainsSigned) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = sts::StsAssumeRoleOwner::create(target(), secret("synthetic-access-key"),
      secret("synthetic-secret-key"), secret(""), deadline_ms(20000), cancel());
  ASSERT_TRUE(made.owner);
  ASSERT_TRUE(made.owner->acquire());
  EXPECT_EQ(1U, detail::fake_http_observation().signed_requests.load());
  EXPECT_EQ(0U, detail::fake_http_observation().web_identity.load());
  EXPECT_TRUE(made.owner->close());
}

TEST(StsWebIdentityOwner, EmptyTokenIsRefusedBeforeInitialization) {
  detail::fake_http_observation().reset();
  auto made = sts::StsAssumeRoleOwner::create_web_identity(target(), secret(""), deadline_ms(20000), cancel());
  EXPECT_FALSE(made.owner);
  EXPECT_EQ(sts::OwnerFailure::InvalidSource, made.failure);
  EXPECT_EQ(0U, detail::fake_http_observation().sends.load());
}

TEST(StsWebIdentityOwner, DeniedAssumptionRefuses) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Denied, 0, nullptr);
  auto made = sts::StsAssumeRoleOwner::create_web_identity(target(), token(R"({"email":"a@example.com"})"),
      deadline_ms(20000), cancel());
  ASSERT_TRUE(made.owner);
  auto result = made.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::StsAssumeRoleFailure::InvalidResponse, result.error().failure);
  EXPECT_EQ(1U, detail::fake_http_observation().denied.load());
  EXPECT_TRUE(made.owner->close());
}

TEST(StsWebIdentityOwner, CancellationDuringTransportRefuses) {
  detail::fake_http_observation().reset();
  auto c = cancel();
  detail::fake_http_configure(detail::FakeHttpScenario::CancelDuringTransport, 0, c);
  auto made = sts::StsAssumeRoleOwner::create_web_identity(target(), token(R"({"email":"a@example.com"})"),
      deadline_ms(20000), c);
  ASSERT_TRUE(made.owner);
  auto result = made.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::StsAssumeRoleFailure::Cancelled, result.error().failure);
  EXPECT_TRUE(made.owner->close());
}

TEST(StsWebIdentityOwner, ExpiryBetweenCreateAndAcquireRefusesWithZeroSends) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto made = sts::StsAssumeRoleOwner::create_web_identity(target(), token(R"({"email":"a@example.com"})"),
      deadline_ms(30), cancel());
  ASSERT_TRUE(made.owner);
  std::this_thread::sleep_for(60ms);
  auto result = made.owner->acquire();
  EXPECT_FALSE(result);
  EXPECT_EQ(a::StsAssumeRoleFailure::DeadlineElapsed, result.error().failure);
  EXPECT_EQ(0U, detail::fake_http_observation().sends.load());
  EXPECT_TRUE(made.owner->close());
}

TEST(JwtIamAuthPlugin, RoleArnTokenAndDefaultsReachAssumedCredentials) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto result = jwt::assume_role_with_jwt(role_arn, token(R"({"email":"a@example.com"})"), "eu-north-1", "",
      900, deadline_ms(20000), cancel());
  ASSERT_TRUE(result);
  EXPECT_FALSE(result.value().session_token.empty());
  EXPECT_EQ(1U, detail::fake_http_observation().web_identity.load());
  EXPECT_EQ(0U, detail::fake_http_observation().signed_requests.load());
}

TEST(JwtIamAuthPlugin, InvalidInputsAreRefusedWithZeroSends) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  const auto t = [] { return token(R"({"email":"a@example.com"})"); };
  EXPECT_FALSE(jwt::assume_role_with_jwt("arn:aws:iam::123456789012:user/alice", t(), "eu-north-1", "", 900,
      deadline_ms(20000), cancel()));
  EXPECT_FALSE(jwt::assume_role_with_jwt(role_arn, secret(""), "eu-north-1", "", 900, deadline_ms(20000), cancel()));
  EXPECT_FALSE(jwt::assume_role_with_jwt(role_arn, t(), "", "", 900, deadline_ms(20000), cancel()));
  EXPECT_FALSE(jwt::assume_role_with_jwt(role_arn, t(), "eu-north-1", "", 899, deadline_ms(20000), cancel()));
  EXPECT_FALSE(jwt::assume_role_with_jwt(role_arn, t(), "eu-north-1", "", 3601, deadline_ms(20000), cancel()));
  EXPECT_FALSE(jwt::assume_role_with_jwt(role_arn, t(), "eu-north-1", "", 900, deadline_ms(20000), nullptr));
  auto expired = jwt::assume_role_with_jwt(role_arn, t(), "eu-north-1", "", 900, deadline_ms(-1000), cancel());
  ASSERT_FALSE(expired);
  EXPECT_EQ(a::StsAssumeRoleFailure::DeadlineElapsed, expired.error().failure);
  EXPECT_EQ(0U, detail::fake_http_observation().sends.load());
}

// The assumed-role credentials feed the existing serverless GetCredentials owner.
TEST(JwtIamAuthPlugin, AssumedCredentialsIssueServerlessDatabaseCredentials) {
  detail::fake_http_observation().reset();
  detail::fake_http_configure(detail::FakeHttpScenario::Success, 0, nullptr);
  auto c = cancel();
  auto assumed = jwt::assume_role_with_jwt(role_arn, token(R"({"email":"a@example.com"})"), "eu-north-1",
      "", 900, deadline_ms(20000), c);
  ASSERT_TRUE(assumed);
  auto& creds = assumed.value();
  auto owner = sl::ServerlessCredentialsOwner::create({"eu-north-1", "fixture-workgroup", "fixture_db", 900},
      std::move(creds.access_key_id), std::move(creds.secret_access_key), std::move(creds.session_token),
      deadline_ms(20000), c);
  ASSERT_TRUE(owner.owner);
  auto fields = owner.owner->acquire();
  ASSERT_TRUE(fields);
  EXPECT_EQ("IAM:assumed-user", fields.value().user);
  EXPECT_FALSE(fields.value().password.empty());
  EXPECT_EQ(1U, detail::fake_http_observation().web_identity.load());
  EXPECT_EQ(1U, detail::fake_http_observation().get_credentials.load());
  EXPECT_EQ(1U, detail::fake_http_observation().signed_requests.load());
  EXPECT_TRUE(owner.owner->close());
}
