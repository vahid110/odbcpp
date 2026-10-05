#include "odbcpp/auth/aws_db_json_response.h"
#include "odbcpp/auth/aws_db_xml_response.h"
#include "odbcpp/auth/redshift_serverless_response.h"
#include "odbcpp/auth/redshift_provisioned_response.h"
#include "odbcpp/auth/redshift_withiam_response.h"
#include <gtest/gtest.h>
#include <functional>
#include <stdexcept>

namespace {
using namespace rs::core::auth;
using namespace std::chrono_literals;
using rs::util::Deadline;
Deadline at(long long seconds) { return Deadline{std::chrono::seconds{seconds}}; }
enum class Api { Serverless, Provisioned, WithIam };
constexpr Api apis[]{Api::Serverless, Api::Provisioned, Api::WithIam};
struct Clock final : MonotonicClock {
  Deadline value{at(103)};
  unsigned calls{0}, change_at{0};
  Deadline changed{at(103)};
  Deadline now() noexcept override {
    if (++calls == change_at) { value = changed; }
    return value;
  }
};
// Test-local observer records REAL reads; it does not manufacture clock quality
// or AWS provenance. Its stage checkpoint is distinct from the UTC predecessor.
struct Observer final : MonotonicClock {
  Clock& source;
  Deadline highwater{at(103)};
  bool rolled_back{false};
  explicit Observer(Clock& clock) : source(clock) {}
  Deadline now() noexcept override {
    const auto current = source.now();
    if (current < highwater) { rolled_back = true; }
    else { highwater = current; }
    return current;
  }
};
struct Stop final : Cancellation {
  bool stopped{false};
  bool stop_requested() const noexcept override { return stopped; }
};
enum class Boundary { Cancelled, Rollback, Late };
using Result = std::variant<Material, JsonError, XmlError, FieldError, ResponseError,
                            ProvisionedError, WithIamError, Boundary>;
enum class Stage { Parsed, Extracted, Projected };
using Hook = std::function<void(Stage)>;
Request request(Api api, std::string generation = "generation", Deadline deadline = at(154)) {
  const auto resource = api == Api::Serverless
      ? "arn:aws:redshift-serverless:eu-north-1:123456789012:workgroup/exact-id"
      : "arn:aws:redshift:eu-north-1:123456789012:cluster:exact-cluster";
  auto b = Binding::create({Service::Redshift, "db.example", 5439, "pilot", "IAM:user", resource,
                           "db.example", "trust"},
                          {SourceKind::TrustedTemporaryDbIssuer, "source", std::move(generation)},
                          Method::TemporaryDatabasePassword);
  if (!b) { throw std::runtime_error("invalid test binding"); }
  auto r = Request::create(std::move(b).value(), deadline, 6s);
  if (!r) { throw std::runtime_error("invalid test request"); }
  return std::move(r).value();
}
TemporaryClockSample sample() {
  return {{1000000000}, at(100), at(102), at(103), at(103), 0us, 0us, 0us, at(99)};
}
TemporaryTimePolicy policy() { return {10s, 2s, 10s, 3600s}; }
std::string body(Api api, std::string user = "IAM:user", std::string extra = "",
                 bool exact_fraction = true) {
  if (api == Api::Serverless) {
    return "{\"dbUser\":\"" + user + "\",\"dbPassword\":\"secret-sentinel\",\"expiration\":" +
           (exact_fraction ? "1060.000001" : "1060") + extra + "}";
  }
  const std::string op = api == Api::Provisioned ? "GetClusterCredentials" : "GetClusterCredentialsWithIAM";
  return "<" + op + "Response xmlns=\"http://redshift.amazonaws.com/doc/2012-12-01/\"><" + op +
      "Result><DbUser>" + user + "</DbUser><DbPassword>secret-sentinel</DbPassword><Expiration>" +
      (exact_fraction ? "1970-01-01T00:17:40.000001Z" : "1970-01-01T00:17:40Z") +
      "</Expiration>" + extra + "</" + op + "Result></" + op + "Response>";
}
ResponseBytes raw(std::string_view text) {
  Clock clock;
  auto created = StreamOwner::create(StreamOwner::max_bytes, at(154), clock, nullptr);
  if (!std::holds_alternative<StreamOwner>(created)) { throw std::runtime_error("test stream creation failed"); }
  auto owner = std::move(std::get<StreamOwner>(created));
  owner.io()->write(text.data(), static_cast<std::streamsize>(text.size()));
  auto sealed = seal_response(std::move(owner));
  if (!std::holds_alternative<ResponseBytes>(sealed)) { throw std::runtime_error("test stream sealing failed"); }
  return std::move(std::get<ResponseBytes>(sealed));
}
std::optional<Boundary> gate(Observer& observer, Deadline deadline, const Cancellation* stop) {
  if (stop && stop->stop_requested()) { return Boundary::Cancelled; }
  const auto now = observer.now();
  if (observer.rolled_back) { return Boundary::Rollback; }
  if (now >= deadline) { return Boundary::Late; }
  return {};
}
// Test-local composition only: no production helper, no issuance authority.
Result compose(Api api, const Request& original, ResponseBytes&& bytes, Observer& clock,
               const Cancellation* stop = nullptr, Hook hook = {}) {
  const auto op = api == Api::Serverless ? DbCredentialOperation::ServerlessGetCredentials :
      api == Api::Provisioned ? DbCredentialOperation::ClusterGetCredentials :
                               DbCredentialOperation::ClusterGetCredentialsWithIam;
  const auto shape = api == Api::Serverless ? ResponseShape::ServerlessObject :
      api == Api::Provisioned ? ResponseShape::ClusterResult : ResponseShape::ClusterWithIamResult;
  auto parsed = [&]() -> std::variant<ResponseSnapshot, JsonError, XmlError> {
    if (api == Api::Serverless) {
      auto result = parse_aws_db_json_response(op, shape, std::move(bytes), original.deadline(), clock.highwater, clock, stop);
      if (auto* error = std::get_if<JsonError>(&result)) { return *error; }
      return std::move(std::get<ResponseSnapshot>(result));
    }
    auto result = parse_aws_db_xml_response(op, shape, std::move(bytes), original.deadline(), clock.highwater, clock, stop);
    if (auto* error = std::get_if<XmlError>(&result)) { return *error; }
    return std::move(std::get<ResponseSnapshot>(result));
  }();
  if (auto* error = std::get_if<JsonError>(&parsed)) { return *error; }
  if (auto* error = std::get_if<XmlError>(&parsed)) { return *error; }
  if (hook) { hook(Stage::Parsed); }
  if (auto error = gate(clock, original.deadline(), stop)) { return *error; }
  auto extracted = extract_db_fields(op, std::move(std::get<ResponseSnapshot>(parsed)), stop);
  if (!extracted) { return extracted.error(); }
  if (hook) { hook(Stage::Extracted); }
  if (auto error = gate(clock, original.deadline(), stop)) { return *error; }
  auto current = sample();
  current.now = clock.highwater; // Do NOT replace current.previous_monotonic_sample.
  auto fields = std::move(extracted).value();
  Result projected = [&]() -> Result {
    if (api == Api::Serverless) {
      auto context = ServerlessAcquisition::create(original,
          {AwsPartition::Aws, "123456789012", "eu-north-1", "exact-id", "exact-name", "pilot", 900});
      if (!context) { return context.error(); }
      ServerlessResponseUtc response{std::move(fields.user), std::move(fields.password), fields.expiry};
      auto result = project_serverless_response_utc(context.value(), std::move(response), current, policy(), stop);
      if (!result) { return result.error(); }
      return std::move(result).value();
    }
    if (api == Api::Provisioned) {
      auto context = ProvisionedAcquisition::create(original,
          {ProvisionedPartition::Aws, "123456789012", "eu-north-1", "exact-cluster", "pilot", "user", false, std::nullopt, 900});
      if (!context) { return context.error(); }
      ProvisionedResponseUtc response{std::move(fields.user), std::move(fields.password), fields.expiry};
      auto result = project_provisioned_response_utc(context.value(), std::move(response), current, policy(), stop);
      if (!result) { return result.error(); }
      return std::move(result).value();
    }
    auto context = WithIamAcquisition::create(original,
        {WithIamPartition::Aws, "123456789012", "eu-north-1", "exact-cluster", "pilot", "IAM:user", 900});
    if (!context) { return context.error(); }
    WithIamResponseUtc response{std::move(fields.user), std::move(fields.password), fields.expiry};
    auto result = project_withiam_response_utc(context.value(), std::move(response), current, policy(), stop);
    if (!result) { return result.error(); }
    return std::move(result).value();
  }();
  if (!std::holds_alternative<Material>(projected)) { return projected; }
  EXPECT_TRUE(fields.password.empty());
  if (hook) { hook(Stage::Projected); }
  if (auto error = gate(clock, original.deadline(), stop)) { return *error; }
  return projected;
}
TEST(AuthRawResponseProjection, ThreeMethodsOwnSecretsAndPreserveExclusiveOneMicrosecond) {
  for (const auto api : apis) {
    Clock source; Observer clock(source); auto input = raw(body(api));
    auto result = compose(api, request(api), std::move(input), clock);
    ASSERT_TRUE(std::holds_alternative<Material>(result)); EXPECT_EQ(input.size(), 0u);
    auto material = std::move(std::get<Material>(result));
    EXPECT_EQ(material.validity().issued_at, at(103));
    EXPECT_EQ(material.validity().expires_at, at(160) + 1us);
    EXPECT_EQ(material.principal(), "IAM:user");
    material.with_secret([](auto bytes) {
      EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()) == "secret-sentinel");
    });
    auto truncated = raw(body(api, "IAM:user", "", false));
    auto denied = compose(api, request(api), std::move(truncated), clock);
    EXPECT_FALSE(std::holds_alternative<Material>(denied));
  }
}
TEST(AuthRawResponseProjection, WrongMethodEnvelopeNeverReachesProjection) {
  for (const auto api : apis) {
    Clock source; Observer clock(source); auto input = raw(body(api == Api::Provisioned ? Api::WithIam : Api::Provisioned));
    auto result = compose(api, request(api), std::move(input), clock);
    EXPECT_FALSE(std::holds_alternative<Material>(result));
    EXPECT_TRUE(std::holds_alternative<JsonError>(result) || std::holds_alternative<XmlError>(result));
    EXPECT_EQ(input.size(), 0u);
  }
}
TEST(AuthRawResponseProjection, DuplicateFieldsBeatWrongPrincipalAtExtractionStage) {
  for (const auto api : apis) {
    Clock source; Observer clock(source);
    auto input = raw(body(api, "wrong", api == Api::Serverless ? ",\"dbUser\":\"again\"" : "<DbUser>again</DbUser>"));
    auto result = compose(api, request(api), std::move(input), clock);
    ASSERT_TRUE(std::holds_alternative<FieldError>(result));
    EXPECT_EQ(std::get<FieldError>(result).failure, FieldFailure::DuplicateField);
    EXPECT_EQ(std::get<FieldError>(result).safe_message().find("secret-sentinel"), std::string_view::npos);
  }
}
TEST(AuthRawResponseProjection, ValidFieldsStillRequireExactPrincipal) {
  for (const auto api : apis) {
    Clock source; Observer clock(source); auto input = raw(body(api, "wrong"));
    auto result = compose(api, request(api), std::move(input), clock);
    if (api == Api::Serverless) {
      ASSERT_TRUE(std::holds_alternative<ResponseError>(result));
      EXPECT_EQ(std::get<ResponseError>(result).failure, ResponseFailure::PrincipalMismatch);
    } else if (api == Api::Provisioned) {
      ASSERT_TRUE(std::holds_alternative<ProvisionedError>(result));
      EXPECT_EQ(std::get<ProvisionedError>(result).failure, ProvisionedFailure::PrincipalMismatch);
    } else {
      ASSERT_TRUE(std::holds_alternative<WithIamError>(result));
      EXPECT_EQ(std::get<WithIamError>(result).failure, WithIamFailure::PrincipalMismatch);
    }
  }
}
TEST(AuthRawResponseProjection, RefreshNeverExtendsExpiryAndProvisionedRejectsIt) {
  for (const auto api : apis) {
    Clock source; Observer clock(source);
    const auto extra = api == Api::Serverless ? ",\"nextRefreshTime\":1200" : "<NextRefreshTime>1970-01-01T00:20:00Z</NextRefreshTime>";
    auto input = raw(body(api, "IAM:user", extra));
    auto result = compose(api, request(api), std::move(input), clock);
    if (api == Api::Provisioned) {
      ASSERT_TRUE(std::holds_alternative<FieldError>(result));
      EXPECT_EQ(std::get<FieldError>(result).failure, FieldFailure::UnknownField);
    } else {
      ASSERT_TRUE(std::holds_alternative<Material>(result));
      EXPECT_EQ(std::get<Material>(result).validity().expires_at, at(160) + 1us);
    }
  }
}
TEST(AuthRawResponseProjection, ProcessingDelayRetainsBracketPredecessorAndExpiryBound) {
  for (const auto api : apis) {
    Clock source; Observer clock(source); auto input = raw(body(api));
    auto result = compose(api, request(api), std::move(input), clock, nullptr,
        [&](Stage stage) { if (stage == Stage::Parsed) { source.value = at(105); } });
    ASSERT_TRUE(std::holds_alternative<Material>(result));
    EXPECT_EQ(clock.highwater, at(105));
    EXPECT_EQ(std::get<Material>(result).validity().expires_at, at(160) + 1us);
    EXPECT_EQ(std::get<Material>(result).validity().issued_at, at(103));
  }
}
TEST(AuthRawResponseProjection, StaleBracketCannotBeRefreshedByFinishingParse) {
  Clock source; Observer clock(source); auto input = raw(body(Api::Serverless));
  auto result = compose(Api::Serverless, request(Api::Serverless), std::move(input), clock, nullptr,
      [&](Stage stage) { if (stage == Stage::Extracted) { source.value = at(111); } });
  ASSERT_TRUE(std::holds_alternative<ResponseError>(result));
  EXPECT_EQ(std::get<ResponseError>(result).time_failure, TimeFailure::StaleSample);
}
TEST(AuthRawResponseProjection, RetainedClockRollbackAndOriginalDeadlineBlockPublication) {
  for (const auto api : apis) {
    for (const auto stage : {Stage::Parsed, Stage::Extracted, Stage::Projected}) {
      for (const auto late : {false, true}) {
        Clock source; Observer clock(source); auto input = raw(body(api));
        auto result = compose(api, request(api), std::move(input), clock, nullptr,
            [&](Stage current) { if (current == stage) { source.value = late ? at(154) : at(102); } });
        ASSERT_TRUE(std::holds_alternative<Boundary>(result));
        EXPECT_EQ(std::get<Boundary>(result), late ? Boundary::Late : Boundary::Rollback);
      }
    }
  }
}
TEST(AuthRawResponseProjection, ClockRecoveryCannotClearPreviouslyObservedRollback) {
  for (const auto api : apis) {
    Clock source; Observer clock(source);
    auto first = raw(body(api));
    auto refused = compose(api, request(api), std::move(first), clock, nullptr,
        [&](Stage stage) { if (stage == Stage::Extracted) { source.value = at(102); } });
    ASSERT_TRUE(std::holds_alternative<Boundary>(refused));
    EXPECT_EQ(std::get<Boundary>(refused), Boundary::Rollback);
    ASSERT_TRUE(clock.rolled_back);
    EXPECT_EQ(clock.highwater, at(103));
    EXPECT_EQ(first.size(), 0u);

    // A later real clock observation recovers, but the request-local fault is
    // latched. Successful syntax parsing cannot erase that observed history.
    source.value = at(105);
    auto second = raw(body(api));
    bool parsed = false, extracted = false, projected = false;
    auto still_refused = compose(api, request(api), std::move(second), clock, nullptr,
        [&](Stage stage) {
          if (stage == Stage::Parsed) { parsed = true; }
          if (stage == Stage::Extracted) { extracted = true; }
          if (stage == Stage::Projected) { projected = true; }
        });
    ASSERT_TRUE(std::holds_alternative<Boundary>(still_refused));
    EXPECT_EQ(std::get<Boundary>(still_refused), Boundary::Rollback);
    EXPECT_TRUE(parsed);
    EXPECT_FALSE(extracted);
    EXPECT_FALSE(projected);
    EXPECT_TRUE(clock.rolled_back);
    EXPECT_EQ(clock.highwater, at(105));
    EXPECT_EQ(second.size(), 0u);
  }
}
TEST(AuthRawResponseProjection, CancellationAtEachStageConsumesOwnersAndPublishesNothing) {
  for (const auto api : apis) {
    for (const auto stage : {Stage::Parsed, Stage::Extracted, Stage::Projected}) {
      Clock source; Observer clock(source); Stop stop; auto input = raw(body(api));
      auto result = compose(api, request(api), std::move(input), clock, &stop,
          [&](Stage current) { if (current == stage) { stop.stopped = true; } });
      ASSERT_TRUE(std::holds_alternative<Boundary>(result));
      EXPECT_EQ(std::get<Boundary>(result), Boundary::Cancelled); EXPECT_EQ(input.size(), 0u);
    }
  }
}
TEST(AuthRawResponseProjection, ActualReadsDuringParsingRetainCheckpointAndRefuseClockFaults) {
  for (const auto api : apis) {
    for (const auto late : {false, true}) {
      Clock source; source.change_at = 2; source.changed = late ? at(154) : at(102);
      Observer clock(source); auto input = raw(body(api));
      auto result = compose(api, request(api), std::move(input), clock);
      if (api == Api::Serverless) {
        ASSERT_TRUE(std::holds_alternative<JsonError>(result));
        EXPECT_EQ(std::get<JsonError>(result).failure,
                  late ? JsonFailure::DeadlineElapsed : JsonFailure::ClockRollback);
      } else {
        ASSERT_TRUE(std::holds_alternative<XmlError>(result));
        EXPECT_EQ(std::get<XmlError>(result).failure,
                  late ? XmlFailure::DeadlineElapsed : XmlFailure::ClockRollback);
      }
      EXPECT_GE(source.calls, 2u);
      EXPECT_EQ(clock.highwater, late ? at(154) : at(103));
      EXPECT_EQ(input.size(), 0u);
    }
  }
}
TEST(AuthRawResponseProjection, EarlyCancellationPreservesParserErrorStage) {
  Clock source; Observer clock(source); Stop stop; stop.stopped = true; auto input = raw("malformed");
  auto result = compose(Api::Serverless, request(Api::Serverless), std::move(input), clock, &stop);
  ASSERT_TRUE(std::holds_alternative<JsonError>(result));
  EXPECT_EQ(std::get<JsonError>(result).failure, JsonFailure::Cancelled); EXPECT_EQ(input.size(), 0u);
}
// Synthetic issuer is explicitly injected; this is NOT authenticated AWS ingress.
struct SyntheticIssuer final : TrustedIssuer {
  const ServerlessAcquisition& context;
  Observer& clock;
  std::string wire;
  SyntheticIssuer(const ServerlessAcquisition& acquisition, Observer& observer, std::string body_text)
      : context(acquisition), clock(observer), wire(std::move(body_text)) {}
  Outcome<Material> acquire(const Request& original, const Cancellation* stop) override {
    if (!context.matches(original)) { return Error{Reason::InvalidRequest}; }
    auto input = raw(wire);
    auto result = compose(Api::Serverless, original, std::move(input), clock, stop);
    if (!std::holds_alternative<Material>(result)) { return Error{Reason::IssuerFailed}; }
    return std::move(std::get<Material>(result));
  }
};
TEST(AuthRawResponseProjection, ExplicitSyntheticAuthorityBridgeIsSingleUseAndChecksGeneration) {
  const auto original = request(Api::Serverless);
  auto made = ServerlessAcquisition::create(original,
      {AwsPartition::Aws, "123456789012", "eu-north-1", "exact-id", "exact-name", "pilot", 900});
  ASSERT_TRUE(made); Clock source; Observer clock(source);
  SyntheticIssuer issuer(made.value(), clock, body(Api::Serverless));
  auto authority = Authority::create(original.binding(), issuer, clock); ASSERT_TRUE(authority);
  auto receipt = authority.value()->acquire(original); ASSERT_TRUE(receipt);
  auto material = authority.value()->take(std::move(receipt).value()); ASSERT_TRUE(material);
  auto reused = authority.value()->take(std::move(receipt).value()); ASSERT_FALSE(reused);
  EXPECT_EQ(reused.error().reason(), Reason::ConsumedReceipt);
  auto foreign = authority.value()->acquire(request(Api::Serverless, "foreign"));
  ASSERT_FALSE(foreign); EXPECT_EQ(foreign.error().reason(), Reason::SourceMismatch);
}
TEST(AuthRawResponseProjection, SyntheticAuthorityCannotTurnWrongEnvelopeIntoReceipt) {
  const auto original = request(Api::Serverless);
  auto made = ServerlessAcquisition::create(original,
      {AwsPartition::Aws, "123456789012", "eu-north-1", "exact-id", "exact-name", "pilot", 900});
  ASSERT_TRUE(made); Clock source; Observer clock(source);
  SyntheticIssuer issuer(made.value(), clock, body(Api::WithIam));
  auto authority = Authority::create(original.binding(), issuer, clock); ASSERT_TRUE(authority);
  auto receipt = authority.value()->acquire(original); ASSERT_FALSE(receipt);
  EXPECT_EQ(receipt.error().reason(), Reason::IssuerFailed);
}
} // namespace
