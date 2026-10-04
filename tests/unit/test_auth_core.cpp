#include "core/auth/auth_core.h"
#include <gtest/gtest.h>
#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace rs::core::auth;
using rs::util::Deadline;
using rs::util::Clock;
Deadline at(long long seconds) { return Deadline{std::chrono::seconds{seconds}}; }
TargetInput target(Service service = Service::Redshift) {
  return {service, "db.example", 5439, "pilot", "exact_user", "resource-1", "db.example", "trust-1"};
}
SourceInput source(bool temporary = true) {
  return {temporary ? SourceKind::TrustedTemporaryDbIssuer : SourceKind::ExternalPassword, "source-1", "generation-1"};
}
Binding binding(bool temporary = true) {
  auto r = Binding::create(target(), source(temporary), temporary ? Method::TemporaryDatabasePassword : Method::OrdinaryPassword);
  if (!r) throw std::runtime_error("invalid test binding");
  return std::move(r).value();
}
SecretBytes secret(std::string_view value = "opaque-password") {
  auto r = SecretBytes::create(std::as_bytes(std::span{value.data(), value.size()}));
  if (!r) throw std::runtime_error("invalid test secret");
  return std::move(r).value();
}
Request request(const Binding& b, Deadline deadline = at(20), Clock::duration headroom = std::chrono::seconds{2}) {
  auto r = Request::create(b, deadline, headroom);
  if (!r) throw std::runtime_error("invalid test request");
  return std::move(r).value();
}
class FakeCancellation final : public Cancellation {
 public:
  void request_stop() noexcept { stopped_ = true; }
  const Cancellation* get_token() const noexcept { return this; }
  bool stop_requested() const noexcept override { return stopped_; }
 private: bool stopped_ = false;
};
class FakeClock final : public MonotonicClock {
 public:
  Deadline value = at(1);
  std::vector<Deadline> scripted;
  std::size_t samples = 0;
  Deadline now() noexcept override {
    if (samples < scripted.size()) value = scripted[samples];
    ++samples; return value;
  }
};
class FakeIssuer final : public TrustedIssuer {
 public:
  Binding returned = binding();
  MaterialKind kind = MaterialKind::TemporaryDatabasePassword;
  Validity validity = Validity::monotonic(at(0), at(30));
  std::string principal = "exact_user";
  int calls = 0;
  Deadline observed_deadline{};
  bool token_stop_possible = false;
  std::function<void()> on_acquire;
  std::optional<Error> fail;
  Outcome<Material> acquire(const Request& r, const Cancellation* cancel) override {
    ++calls; observed_deadline = r.deadline(); token_stop_possible = cancel != nullptr;
    if (on_acquire) on_acquire();
    if (fail) return *fail;
    return Material::create(returned, kind, principal, secret(), validity);
  }
};
std::unique_ptr<Authority> authority_for(const Binding& b, TrustedIssuer& issuer, MonotonicClock& clock) {
  auto r = Authority::create(b, issuer, clock);
  if (!r) throw std::runtime_error("test authority allocation failed");
  return std::move(r).value();
}
static_assert(!std::is_copy_constructible_v<Authority>);
static_assert(!std::is_move_constructible_v<Authority>);
static_assert(!std::is_copy_constructible_v<SecretBytes>);
static_assert(!std::is_copy_constructible_v<Material>);
static_assert(!std::is_copy_constructible_v<Receipt>);
static_assert(!std::is_default_constructible_v<Receipt>);
static_assert(!std::is_move_assignable_v<Binding>);
static_assert(!std::is_convertible_v<SecretBytes, std::string>);

template<class T> void denied(const Outcome<T>& r, Reason reason) {
  ASSERT_FALSE(r);
  EXPECT_EQ(reason, r.error().reason());
  EXPECT_FALSE(r.error().safe_message().empty());
}
TEST(AuthCoreSecretTest, OwnershipScopedAccessMoveReplacementAndClear) {
  std::string input = "source-value";
  auto first = secret(input);
  input.assign(input.size(), 'x');
  bool owned = false;
  first.with_bytes([&](auto bytes) { owned = std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()} == "source-value"; });
  EXPECT_TRUE(owned);
  auto moved = std::move(first);
  EXPECT_TRUE(first.empty());
  auto replaced = secret("replacement");
  replaced = std::move(moved);
  EXPECT_TRUE(moved.empty());
  bool intact = false;
  replaced.with_bytes([&](auto bytes) {
    intact = bytes.size() == 12 && bytes.front() == std::byte{'s'};
    EXPECT_FALSE(replaced.clear()); // Borrow remains valid on reentrant refusal.
    EXPECT_THROW(replaced.with_bytes([](auto) {}), std::logic_error);
  });
  EXPECT_TRUE(intact);
  EXPECT_TRUE(replaced.clear()); EXPECT_TRUE(replaced.empty());
  EXPECT_TRUE(replaced.clear());
}
TEST(AuthCoreSecretTest, ExactBoundsBinaryOwnershipAndExceptionRelease) {
  std::vector<std::byte> bytes(SecretBytes::max_bytes, std::byte{0xff});
  auto exact = SecretBytes::create(bytes); ASSERT_TRUE(exact);
  bytes.push_back(std::byte{0});
  denied(SecretBytes::create(bytes), Reason::SecretLimit);
  auto value = std::move(exact).value();
  EXPECT_THROW(value.with_bytes([](auto) { throw std::runtime_error("consumer failure"); }), std::runtime_error);
  bool usable = false; value.with_bytes([&](auto view) { usable = view.size() == SecretBytes::max_bytes; });
  EXPECT_TRUE(usable); EXPECT_TRUE(value.clear());
  const std::array binary{std::byte{0}, std::byte{0xff}, std::byte{7}};
  auto b = SecretBytes::create(binary); ASSERT_TRUE(b);
  bool exact_binary = false; b.value().with_bytes([&](auto view) { exact_binary = std::equal(view.begin(), view.end(), binary.begin(), binary.end()); });
  EXPECT_TRUE(exact_binary);
}
TEST(AuthCoreBindingTest, BoundedExactPolicyRejectsMissingMalformedAndUnsupported) {
  auto bad = target(); bad.endpoint = std::string("secret\0endpoint", 15);
  denied(Binding::create(bad, source(), Method::TemporaryDatabasePassword), Reason::InvalidBinding);
  bad = target(); bad.principal = "\xc0\x80";
  denied(Binding::create(bad, source(), Method::TemporaryDatabasePassword), Reason::InvalidBinding);
  bad = target(); bad.port = 65536;
  denied(Binding::create(bad, source(), Method::TemporaryDatabasePassword), Reason::InvalidBinding);
  bad = target(); bad.trust_policy.clear();
  denied(Binding::create(bad, source(), Method::TemporaryDatabasePassword), Reason::InvalidBinding);
  auto unknown = source(); unknown.generation.clear();
  denied(Binding::create(target(), unknown, Method::TemporaryDatabasePassword), Reason::InvalidBinding);
  unknown = source(); unknown.kind = static_cast<SourceKind>(99);
  denied(Binding::create(target(), unknown, Method::TemporaryDatabasePassword), Reason::UnsupportedMethod);
  denied(Binding::create(target(Service::Rds), source(), Method::TemporaryDatabasePassword), Reason::UnsupportedMethod);
  denied(Binding::create(target(), source(), static_cast<Method>(99)), Reason::UnsupportedMethod);
}
TEST(AuthCoreBindingTest, MovedFromBindingIsRejectedByEveryOwningFactory) {
  auto original = binding();
  auto retained = std::move(original);
  FakeClock clock; FakeIssuer issuer;
  denied(Request::create(original, at(20), std::chrono::seconds{2}), Reason::InvalidBinding);
  denied(Authority::create(original, issuer, clock), Reason::InvalidBinding);
  denied(Material::create(original, MaterialKind::TemporaryDatabasePassword, "exact_user",
      secret(), Validity::monotonic(at(0), at(30))), Reason::InvalidBinding);
  auto copied_invalid = original;
  denied(Request::create(copied_invalid, at(20), Clock::duration::zero()), Reason::InvalidBinding);
  // Moving does not invalidate the destination or alter its original policy.
  auto authority = authority_for(retained, issuer, clock);
  auto admitted = authority->acquire(request(retained)); ASSERT_TRUE(admitted);
  EXPECT_EQ(1, issuer.calls);
}
TEST(AuthCoreAdmissionTest, MovedFromRequestCannotReachTrustedIssuer) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer;
  auto original = request(b);
  auto retained = std::move(original);
  auto authority = authority_for(b, issuer, clock);
  denied(authority->acquire(original), Reason::InvalidBinding);
  EXPECT_EQ(0, issuer.calls);
  ASSERT_TRUE(authority->acquire(retained)); EXPECT_EQ(1, issuer.calls);
}
TEST(AuthCoreAdmissionTest, IssuerReturningMovedFromMaterialCannotPublishCredentials) {
  class MovedFromIssuer final : public TrustedIssuer {
   public:
    std::optional<Material> retained;
    bool source_empty = false;
    Outcome<Material> acquire(const Request& r, const Cancellation*) override {
      const bool ordinary = r.binding().method() == Method::OrdinaryPassword;
      auto candidate = Material::create(r.binding(), ordinary ? MaterialKind::OrdinaryPassword : MaterialKind::TemporaryDatabasePassword,
          "exact_user", secret(), ordinary ? Validity::ordinary() : Validity::monotonic(at(0), at(30)));
      if (!candidate) return candidate.error();
      retained.emplace(std::move(candidate).value());
      candidate.value().with_secret([&](auto bytes) { source_empty = bytes.empty(); });
      // This bypasses the material factory through its legitimate move API.
      // Admission must independently validate the now-empty issuer result.
      return std::move(candidate).value();
    }
  };
  for (bool temporary : {false, true}) {
    auto b = binding(temporary); FakeClock clock; MovedFromIssuer issuer;
    auto authority = authority_for(b, issuer, clock);
    denied(authority->acquire(request(b)), Reason::InvalidMaterial);
    EXPECT_TRUE(issuer.source_empty);
    ASSERT_TRUE(issuer.retained);
    bool retained_secret = false;
    issuer.retained->with_secret([&](auto bytes) { retained_secret = bytes.size() == 15; });
    EXPECT_TRUE(retained_secret); // Rejection does not damage the real owner.
  }
}
TEST(AuthCoreAdmissionTest, RequestAndCandidateTargetOwnershipAndSingleUseHandoff) {
  auto input = target(); auto src = source();
  auto br = Binding::create(input, src, Method::TemporaryDatabasePassword); ASSERT_TRUE(br);
  auto b = std::move(br).value(); input.database = "foreign"; src.generation = "rotated";
  FakeClock clock; FakeIssuer issuer; auto authority = authority_for(b, issuer, clock);
  auto r = request(b); auto admitted = authority->acquire(r); ASSERT_TRUE(admitted);
  EXPECT_EQ(1, issuer.calls); EXPECT_EQ(r.deadline(), issuer.observed_deadline);
  auto receipt = std::move(admitted).value();
  auto taken = authority->take(std::move(receipt)); ASSERT_TRUE(taken);
  bool bytes_ok = false; taken.value().with_secret([&](auto bytes) { bytes_ok = bytes.size() == 15; });
  EXPECT_TRUE(bytes_ok);
  denied(authority->take(std::move(receipt)), Reason::ConsumedReceipt);
}
TEST(AuthCoreAdmissionTest, RequestMismatchRefusesBeforeIssuer) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer; auto authority = authority_for(b, issuer, clock);
  auto foreign = target(); foreign.database = "foreign";
  auto fb = Binding::create(foreign, source(), Method::TemporaryDatabasePassword); ASSERT_TRUE(fb);
  denied(authority->acquire(request(fb.value())), Reason::TargetMismatch);
  auto src = source(); src.generation = "foreign";
  auto sb = Binding::create(target(), src, Method::TemporaryDatabasePassword); ASSERT_TRUE(sb);
  denied(authority->acquire(request(sb.value())), Reason::SourceMismatch);
  EXPECT_EQ(0, issuer.calls);
}
TEST(AuthCoreAdmissionTest, ReturnedTargetSourceAndPrincipalMismatchNeverPublish) {
  auto b = binding(); FakeClock clock;
  for (int mismatch = 0; mismatch != 5; ++mismatch) {
    auto t = target(); auto src = source();
    if (mismatch == 0) t.tls_identity = "foreign-peer";
    if (mismatch == 1) t.resource_id = "foreign-resource";
    if (mismatch == 2) src.identity = "foreign-source";
    if (mismatch == 3) src.generation = "rotated";
    auto rb = Binding::create(t, src, Method::TemporaryDatabasePassword); ASSERT_TRUE(rb);
    // Binding assignment is deliberately unavailable; construct a separate issuer.
    class FixedIssuer final : public TrustedIssuer {
     public:
      FixedIssuer(Binding b, std::string p) : binding_(std::move(b)), principal_(std::move(p)) {}
      Outcome<Material> acquire(const Request&, const Cancellation*) override {
        return Material::create(binding_, MaterialKind::TemporaryDatabasePassword, principal_, secret(), Validity::monotonic(at(0), at(30)));
      }
     private: Binding binding_; std::string principal_;
    } fixed(rb.value(), mismatch == 4 ? "IAM:unexpected_user" : "exact_user");
    auto authority = authority_for(b, fixed, clock);
    denied(authority->acquire(request(b)), mismatch < 2 ? Reason::TargetMismatch : mismatch < 4 ? Reason::SourceMismatch : Reason::PrincipalMismatch);
  }
}
TEST(AuthCoreAdmissionTest, EqualBindingsDoNotForgeIssuerAuthority) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer;
  auto one = authority_for(b, issuer, clock); auto two = authority_for(b, issuer, clock);
  auto result = one->acquire(request(b)); ASSERT_TRUE(result);
  auto receipt = std::move(result).value();
  denied(two->take(std::move(receipt)), Reason::UntrustedReceipt);
  denied(one->take(std::move(receipt)), Reason::ConsumedReceipt);
}
TEST(AuthCoreAdmissionTest, DestroyedAuthorityCannotBeReplacedByMatchingIdentityStrings) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer;
  std::optional<Receipt> retained;
  {
    auto authority = authority_for(b, issuer, clock);
    auto admitted = authority->acquire(request(b)); ASSERT_TRUE(admitted);
    retained.emplace(std::move(admitted).value());
  }
  auto replacement = authority_for(b, issuer, clock);
  denied(replacement->take(std::move(*retained)), Reason::UntrustedReceipt);
}
TEST(AuthCoreMaterialTest, OrdinaryPasswordWorksOnlyThroughExplicitTargetAuthority) {
  class PasswordIssuer final : public TrustedIssuer {
   public: Outcome<Material> acquire(const Request& r, const Cancellation*) override {
      return Material::create(r.binding(), MaterialKind::OrdinaryPassword, "exact_user", secret(), Validity::ordinary());
    }
  } issuer;
  FakeClock clock;
  for (auto service : {Service::PostgreSql, Service::Redshift, Service::Rds}) {
    auto b = Binding::create(target(service), source(false), Method::OrdinaryPassword); ASSERT_TRUE(b);
    auto authority = authority_for(b.value(), issuer, clock);
    auto admitted = authority->acquire(request(b.value())); ASSERT_TRUE(admitted);
    auto receipt = std::move(admitted).value();
    ASSERT_TRUE(authority->take(std::move(receipt)));
  }
}
TEST(AuthCoreMaterialTest, OrdinaryPasswordIsNotTemporaryMaterialOrIssuerProof) {
  auto b = binding(false);
  auto candidate = Material::create(b, MaterialKind::OrdinaryPassword, "exact_user", secret(), Validity::ordinary());
  ASSERT_TRUE(candidate); // Candidate construction grants no Receipt constructor.
  denied(Material::create(binding(), MaterialKind::TemporaryDatabasePassword, "exact_user", secret(), Validity::ordinary()), Reason::InvalidMaterial);
  denied(Material::create(b, MaterialKind::TemporaryDatabasePassword, "exact_user", secret(), Validity::monotonic(at(0), at(30))), Reason::MethodMismatch);
  denied(Material::create(b, MaterialKind::OrdinaryPassword, "exact_user", secret(std::string_view{"a\0b", 3}), Validity::ordinary()), Reason::InvalidMaterial);
  class PasswordIssuer final : public TrustedIssuer {
   public: Outcome<Material> acquire(const Request& r, const Cancellation*) override {
      return Material::create(r.binding(), MaterialKind::OrdinaryPassword, "exact_user", secret(), Validity::ordinary());
    }
  } issuer;
  FakeClock clock; auto authority = authority_for(b, issuer, clock);
  auto admitted = authority->acquire(request(b)); ASSERT_TRUE(admitted);
  auto receipt = std::move(admitted).value();
  ASSERT_TRUE(authority->take(std::move(receipt)));
}
TEST(AuthCoreTimeTest, ExclusiveHeadroomAndCredentialExpiry) {
  auto b = binding(); FakeClock clock;
  for (long long expiry : {21, 22, 23}) {
    FakeIssuer issuer; issuer.validity = Validity::monotonic(at(0), at(expiry));
    auto authority = authority_for(b, issuer, clock);
    auto r = authority->acquire(request(b));
    if (expiry > 22) {
      ASSERT_TRUE(r);
    } else {
      denied(r, Reason::CredentialExpired);
    }
  }
  FakeIssuer issuer; auto authority = authority_for(b, issuer, clock);
  clock.value = at(20);
  denied(authority->acquire(request(b)), Reason::DeadlineElapsed); EXPECT_EQ(0, issuer.calls);
}
TEST(AuthCoreTimeTest, RejectsOverflowExtremaNegativeAndExcessiveHeadroom) {
  auto b = binding();
  denied(Request::create(b, Deadline::max(), Clock::duration::zero()), Reason::InvalidRequest);
  denied(Request::create(b, Deadline::min(), Clock::duration::zero()), Reason::InvalidRequest);
  denied(Request::create(b, at(20), Clock::duration{-1}), Reason::InvalidRequest);
  denied(Request::create(b, at(20), std::chrono::hours{25}), Reason::InvalidRequest);
  denied(Request::create(b, Deadline::max() - Clock::duration{1}, Clock::duration{2}), Reason::InvalidRequest);
  denied(Material::create(b, MaterialKind::TemporaryDatabasePassword, "exact_user", secret(), Validity::monotonic(at(30), at(30))), Reason::InvalidMaterial);
  denied(Material::create(b, MaterialKind::TemporaryDatabasePassword, "exact_user", secret(), Validity::monotonic(at(0), Deadline::max())), Reason::InvalidMaterial);
}
TEST(AuthCoreTimeTest, RollbackCannotExtendReceiptOrLaterAdmission) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer; auto authority = authority_for(b, issuer, clock);
  clock.value = at(10); auto admitted = authority->acquire(request(b)); ASSERT_TRUE(admitted);
  auto receipt = std::move(admitted).value(); clock.value = at(9);
  denied(authority->take(std::move(receipt)), Reason::ClockRollback);
  denied(authority->acquire(request(b)), Reason::ClockRollback);
  EXPECT_EQ(1, issuer.calls);
}
TEST(AuthCoreTimeTest, LateIssuerAndLateLocalConstructionCannotPublish) {
  auto b = binding();
  for (auto samples : {std::vector<Deadline>{at(1), at(20)}, std::vector<Deadline>{at(1), at(1), at(20)},
                       std::vector<Deadline>{at(2), at(1)}}) {
    FakeClock clock; clock.scripted = std::move(samples); FakeIssuer issuer;
    auto authority = authority_for(b, issuer, clock); auto result = authority->acquire(request(b));
    ASSERT_FALSE(result); EXPECT_EQ(1, issuer.calls);
    EXPECT_TRUE(result.error().reason() == Reason::DeadlineElapsed || result.error().reason() == Reason::ClockRollback);
  }
}
TEST(AuthCoreTimeTest, ExpiredHandoffConsumesRatherThanExtendsReceipt) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer; auto authority = authority_for(b, issuer, clock);
  auto admitted = authority->acquire(request(b)); ASSERT_TRUE(admitted);
  auto receipt = std::move(admitted).value(); clock.value = at(20);
  denied(authority->take(std::move(receipt)), Reason::DeadlineElapsed);
  clock.value = at(21); denied(authority->take(std::move(receipt)), Reason::ConsumedReceipt);
}
TEST(AuthCoreTimeTest, FutureIssuerSampleIsMalformedNotVerified) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer;
  issuer.validity = Validity::monotonic(at(2), at(30)); auto authority = authority_for(b, issuer, clock);
  denied(authority->acquire(request(b)), Reason::InvalidMaterial);
}
TEST(AuthCoreCancellationTest, CancelBeforeAndDuringIssuerAndBeforeTake) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer; auto authority = authority_for(b, issuer, clock);
  FakeCancellation before; before.request_stop();
  denied(authority->acquire(request(b), before.get_token()), Reason::Cancelled); EXPECT_EQ(0, issuer.calls);
  FakeCancellation during; issuer.on_acquire = [&] { during.request_stop(); };
  denied(authority->acquire(request(b), during.get_token()), Reason::Cancelled);
  EXPECT_EQ(1, issuer.calls); EXPECT_TRUE(issuer.token_stop_possible);
  issuer.on_acquire = {};
  auto admitted = authority->acquire(request(b)); ASSERT_TRUE(admitted);
  auto receipt = std::move(admitted).value();
  denied(authority->take(std::move(receipt), before.get_token()), Reason::Cancelled);
  denied(authority->take(std::move(receipt)), Reason::ConsumedReceipt);
}
TEST(AuthCoreErrorsTest, IssuerFailuresAndSecretBearingExceptionsAreSanitized) {
  auto b = binding(); FakeClock clock; FakeIssuer issuer; auto authority = authority_for(b, issuer, clock);
  issuer.on_acquire = [] { throw std::runtime_error("secret-token-should-not-escape"); };
  auto failure = authority->acquire(request(b)); denied(failure, Reason::IssuerFailed);
  EXPECT_TRUE(failure.error().safe_message().find("secret-token") == std::string_view::npos);
  issuer.on_acquire = {}; issuer.fail = Error{Reason::PrincipalMismatch};
  auto denied_result = authority->acquire(request(b)); denied(denied_result, Reason::PrincipalMismatch);
  EXPECT_EQ(Category::Denied, denied_result.error().category());
  Error unknown{static_cast<Reason>(999)}; EXPECT_EQ(Reason::IssuerFailed, unknown.reason());
}
} // namespace
