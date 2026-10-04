#include "core/auth/aws_db_response_fields.h"
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>

namespace {
using namespace rs::core::auth;
using Op = DbCredentialOperation;
using Shape = ResponseShape;
constexpr Op operations[] = {Op::ServerlessGetCredentials, Op::ClusterGetCredentials, Op::ClusterGetCredentialsWithIam};
Shape shape(Op op) {
  if (op == Op::ServerlessGetCredentials) return Shape::ServerlessObject;
  return op == Op::ClusterGetCredentials ? Shape::ClusterResult : Shape::ClusterWithIamResult;
}
std::string key(Op op, int role) {
  if (op == Op::ServerlessGetCredentials) return std::array<std::string,4>{"dbUser", "dbPassword", "expiration", "nextRefreshTime"}[role];
  return std::array<std::string,4>{"DbUser", "DbPassword", "Expiration", "NextRefreshTime"}[role];
}
FieldAtom text(std::string value) {
  auto bytes = SecretBytes::create(std::as_bytes(std::span{value.data(), value.size()}));
  if (!bytes) throw std::runtime_error("test secret bounds");
  return TextBytes{std::move(bytes).value()};
}
FieldAtom timestamp(Op op, bool later = false) {
  if (op == Op::ServerlessGetCredentials) return NumberLexeme{later ? "2000.000001" : "1060.000001"};
  return text(later ? "1970-01-01T00:33:20.000001Z" : "1970-01-01T00:17:40.000001Z");
}
FieldOccurrence field(std::string name, FieldAtom atom) {
  auto r = FieldOccurrence::create(std::move(name), std::move(atom));
  if (!r) throw std::runtime_error("test field invalid");
  return std::move(r).value();
}
std::vector<FieldOccurrence> fields(Op op, int missing = -1, int wrong = -1) {
  std::vector<FieldOccurrence> result;
  for (int role = 0; role < 3; ++role) if (role != missing) {
    FieldAtom atom = role == 2 ? timestamp(op) : text(role == 0 ? "IAM:alice" : "SECRET_SENTINEL");
    if (role == wrong) atom = NullAtom{};
    result.push_back(field(key(op, role), std::move(atom)));
  }
  return result;
}
ResponseSnapshot snapshot(Op op, std::vector<FieldOccurrence> occurrences) {
  auto r = ResponseSnapshot::create(shape(op), std::move(occurrences));
  if (!r) throw std::runtime_error("test snapshot invalid");
  return std::move(r).value();
}
void failure(const FieldOutcome<ExtractedDbFields>& result, FieldFailure expected) {
  ASSERT_FALSE(result); EXPECT_EQ(expected, result.error().failure);
  EXPECT_EQ(std::string_view::npos, result.error().safe_message().find("SECRET_SENTINEL"));
  EXPECT_EQ(std::string_view::npos, result.error().safe_message().find("IAM:alice"));
}
TEST(AwsDbResponseFieldsTest, ThreeSchemasNormalizeReverseOrderAndOwnExactMicroseconds) {
  for (auto op : operations) {
    auto original = fields(op); std::vector<FieldOccurrence> reversed;
    for (auto i = original.size(); i > 0; --i) reversed.push_back(std::move(original[i - 1]));
    auto input = snapshot(op, std::move(reversed));
    auto r = extract_db_fields(op, std::move(input)); ASSERT_TRUE(r);
    EXPECT_EQ("IAM:alice", r.value().user); EXPECT_EQ(1060000001, r.value().expiry.microseconds_since_epoch);
    EXPECT_TRUE(input.invariant_error());
    r.value().password.with_bytes([](auto bytes) {
      EXPECT_EQ("SECRET_SENTINEL", std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    });
  }
}
TEST(AwsDbResponseFieldsTest, RequiredMissingAndNullNeverDefaultToEmptyOrZero) {
  for (auto op : operations) for (int role = 0; role < 3; ++role) {
    failure(extract_db_fields(op, snapshot(op, fields(op, role))), FieldFailure::MissingField);
    failure(extract_db_fields(op, snapshot(op, fields(op, -1, role))), FieldFailure::WrongType);
  }
}
TEST(AwsDbResponseFieldsTest, EqualAndConflictingDuplicatesAreBothRefused) {
  for (auto op : operations) for (int role = 0; role < 3; ++role) for (bool conflict : {false, true}) {
    auto f = fields(op); auto atom = role == 2 ? timestamp(op, conflict) : text(conflict ? "foreign" : role == 0 ? "IAM:alice" : "SECRET_SENTINEL");
    f.push_back(field(key(op, role), std::move(atom)));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::DuplicateField);
  }
}
TEST(AwsDbResponseFieldsTest, WrongTypesAreNotCoercedToTextOrNumbers) {
  for (auto op : operations) for (int role = 0; role < 3; ++role) for (int type = 0; type < 4; ++type) {
    auto f = fields(op, role); FieldAtom atom = BooleanAtom{};
    if (type == 1) atom = ObjectAtom{};
    if (type == 2) atom = ArrayAtom{};
    if (type == 3) atom = role == 2 && op == Op::ServerlessGetCredentials ? text("1060") : FieldAtom{NumberLexeme{"1060"}};
    f.push_back(field(key(op, role), std::move(atom)));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::WrongType);
  }
}
TEST(AwsDbResponseFieldsTest, UnknownCaseNestedAndOperationMixingFailClosed) {
  for (auto op : operations) {
    auto f = fields(op); f.push_back(field("unknown", text("SECRET_SENTINEL")));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::UnknownField);
    f = fields(op, 0); f.push_back(field("DBUSER", text("IAM:alice")));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::UnknownField);
    for (auto other : operations) if (op != other)
      failure(extract_db_fields(other, snapshot(op, fields(op))), FieldFailure::InvalidShape);
  }
  auto input = snapshot(Op::ServerlessGetCredentials, fields(Op::ServerlessGetCredentials));
  failure(extract_db_fields(static_cast<Op>(99), std::move(input)), FieldFailure::UnsupportedOperation);
  EXPECT_TRUE(input.invariant_error());
}
TEST(AwsDbResponseFieldsTest, RefreshNeverSubstitutesOrExtendsExpiry) {
  for (auto op : {Op::ServerlessGetCredentials, Op::ClusterGetCredentialsWithIam}) {
    auto f = fields(op); f.push_back(field(key(op, 3), timestamp(op, true)));
    auto r = extract_db_fields(op, snapshot(op, std::move(f))); ASSERT_TRUE(r);
    EXPECT_EQ(1060000001, r.value().expiry.microseconds_since_epoch);
    f = fields(op, 2); f.push_back(field(key(op, 3), timestamp(op, true)));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::MissingField);
    f = fields(op); f.push_back(field(key(op, 3), timestamp(op))); f.push_back(field(key(op, 3), timestamp(op)));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::DuplicateField);
    f = fields(op); f.push_back(field(key(op, 3), NullAtom{}));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::WrongType);
    f = fields(op); f.push_back(field(key(op, 3), op == Op::ServerlessGetCredentials ? FieldAtom{NumberLexeme{"NaN"}} : text("bad")));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::InvalidTimestamp);
  }
  auto op = Op::ClusterGetCredentials; auto f = fields(op); f.push_back(field(key(op, 3), timestamp(op)));
  failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::UnknownField);
}
TEST(AwsDbResponseFieldsTest, TimestampTagAndPrecisionRefusalsPropagateClosedErrors) {
  for (auto op : operations) {
    auto f = fields(op, 2);
    f.push_back(field(key(op, 2), op == Op::ServerlessGetCredentials ? FieldAtom{NumberLexeme{"1060.0000001"}} : text("1970-01-01T00:17:40.0000001Z")));
    auto r = extract_db_fields(op, snapshot(op, std::move(f)));
    failure(r, FieldFailure::InvalidTimestamp); EXPECT_EQ(TimestampFailure::PrecisionLoss, r.error().timestamp_failure);
    f = fields(op, 2); f.push_back(field(key(op, 2), op == Op::ServerlessGetCredentials ? FieldAtom{NumberLexeme{"1970-01-01T00:17:40Z"}} : text("1060")));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::InvalidTimestamp);
  }
}
TEST(AwsDbResponseFieldsTest, PrincipalValidationPreservesCurrentUtf8AndAsciiControlPolicy) {
  auto op = Op::ServerlessGetCredentials;
  for (std::string user : std::vector<std::string>{"", "a\n", std::string("a\0b", 3), std::string("\x7f", 1),
      std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2)}) {
    auto f = fields(op, 0); f.push_back(field(key(op, 0), text(user)));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::InvalidUser);
  }
  for (std::string user : {std::string(1024, 'a'), std::string("\xc2\x85", 2), std::string("\xe2\x82\xac", 3)}) {
    auto f = fields(op, 0); f.push_back(field(key(op, 0), text(user)));
    auto r = extract_db_fields(op, snapshot(op, std::move(f))); ASSERT_TRUE(r); EXPECT_EQ(user, r.value().user);
  }
  auto f = fields(op, 0); f.push_back(field(key(op, 0), text(std::string(1025, 'a'))));
  failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::ResourceLimit);
}
TEST(AwsDbResponseFieldsTest, PasswordEmptyNulAndMaximumAreOpaqueAndBounded) {
  auto op = Op::ServerlessGetCredentials;
  for (std::string password : {std::string{}, std::string("a\0b", 3)}) {
    auto f = fields(op, 1); f.push_back(field(key(op, 1), text(password)));
    failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::InvalidPassword);
  }
  auto f = fields(op, 1); f.push_back(field(key(op, 1), text(std::string(SecretBytes::max_bytes, '\xff'))));
  auto r = extract_db_fields(op, snapshot(op, std::move(f))); ASSERT_TRUE(r);
  EXPECT_EQ(SecretBytes::max_bytes, r.value().password.size()); // Password is not UTF8-normalized.
}
TEST(AwsDbResponseFieldsTest, FactoryBoundsInvalidKeysAndAggregateConsumeOwnership) {
  for (std::string bad : {std::string{}, std::string("bad key"), std::string("key\0x", 5), std::string(65, 'a')}) {
    auto atom = text("SECRET_SENTINEL"); auto r = FieldOccurrence::create(bad, std::move(atom));
    EXPECT_FALSE(r); EXPECT_TRUE(std::get<TextBytes>(atom).bytes.empty());
  }
  auto long_number = FieldAtom{NumberLexeme{std::string(65, '1')}};
  EXPECT_FALSE(FieldOccurrence::create("expiration", std::move(long_number)));
  std::vector<FieldOccurrence> many;
  for (int i = 0; i < 9; ++i) many.push_back(field("dbUser", text("SECRET_SENTINEL")));
  auto r = ResponseSnapshot::create(Shape::ServerlessObject, std::move(many)); ASSERT_FALSE(r);
  EXPECT_EQ(FieldFailure::ResourceLimit, r.error().failure);
  std::vector<FieldOccurrence> large;
  large.push_back(field("dbPassword", text(std::string(65536, 'a'))));
  large.push_back(field("dbUser", text(std::string(65536, 'b'))));
  auto aggregate = ResponseSnapshot::create(Shape::ServerlessObject, std::move(large)); ASSERT_FALSE(aggregate);
  EXPECT_EQ(FieldFailure::ResourceLimit, aggregate.error().failure);
  auto invalid_shape = fields(Op::ServerlessGetCredentials);
  EXPECT_FALSE(ResponseSnapshot::create(static_cast<Shape>(99), std::move(invalid_shape)));
}
TEST(AwsDbResponseFieldsTest, TimestampRoleBoundsAndRefreshValueCannotChangeRequiredExpiry) {
  auto op = Op::ClusterGetCredentialsWithIam;
  auto f = fields(op, 2); f.push_back(field(key(op, 2), text(std::string(65, 'a'))));
  failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::ResourceLimit);
  f = fields(op); f.push_back(field(key(op, 3), text("1970-01-01T00:00:00.000001Z")));
  auto r = extract_db_fields(op, snapshot(op, std::move(f))); ASSERT_TRUE(r);
  EXPECT_EQ(1060000001, r.value().expiry.microseconds_since_epoch); // Earlier refresh also not authority.
}
TEST(AwsDbResponseFieldsTest, MovedFromOccurrenceSnapshotAndEarlyErrorCannotPublish) {
  auto atom = text("SECRET_SENTINEL"); auto original = field("dbPassword", std::move(atom)); auto kept = std::move(original);
  EXPECT_TRUE(original.invariant_error()); std::vector<FieldOccurrence> invalid; invalid.push_back(std::move(original));
  EXPECT_FALSE(ResponseSnapshot::create(Shape::ServerlessObject, std::move(invalid)));
  EXPECT_FALSE(kept.invariant_error());
  auto input = snapshot(Op::ServerlessGetCredentials, fields(Op::ServerlessGetCredentials)); auto retained = std::move(input);
  failure(extract_db_fields(Op::ServerlessGetCredentials, std::move(input)), FieldFailure::InvalidShape);
  EXPECT_TRUE(extract_db_fields(Op::ServerlessGetCredentials, std::move(retained)));
}
struct Cancels : Cancellation {
  mutable unsigned calls = 0; unsigned stop_at = 1;
  bool stop_requested() const noexcept override { return ++calls >= stop_at; }
};
TEST(AwsDbResponseFieldsTest, CancellationConsumesAtEntryAndBeforeOwningPublication) {
  for (unsigned stop_at : {1u, 2u}) {
    Cancels cancel; cancel.stop_at = stop_at;
    auto input = snapshot(Op::ServerlessGetCredentials, fields(Op::ServerlessGetCredentials));
    failure(extract_db_fields(Op::ServerlessGetCredentials, std::move(input), &cancel), FieldFailure::Cancelled);
    EXPECT_TRUE(input.invariant_error()); EXPECT_EQ(stop_at, cancel.calls);
  }
}
TEST(AwsDbResponseFieldsTest, DeterministicPrecedenceAndSafeErrors) {
  auto op = Op::ServerlessGetCredentials; auto f = fields(op, 2); f.push_back(field("unknown", text("SECRET_SENTINEL")));
  failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::UnknownField);
  f = fields(op, 2); f.push_back(field(key(op, 0), NullAtom{}));
  failure(extract_db_fields(op, snapshot(op, std::move(f))), FieldFailure::DuplicateField);
  for (auto error : {FieldFailure::UnsupportedOperation, FieldFailure::InvalidShape, FieldFailure::ResourceLimit,
      FieldFailure::InvalidFieldName, FieldFailure::UnknownField, FieldFailure::MissingField, FieldFailure::DuplicateField,
      FieldFailure::WrongType, FieldFailure::InvalidUser, FieldFailure::InvalidPassword, FieldFailure::InvalidTimestamp,
      FieldFailure::Cancelled, FieldFailure::AllocationFailed}) EXPECT_FALSE(FieldError{error}.safe_message().empty());
}
TEST(AwsDbResponseFieldsTest, CandidateMicrosecondsReachConverterWithoutMsRoundtripOrAuthority) {
  using namespace std::chrono_literals; using rs::util::Deadline;
  auto at = [](long long seconds) { return Deadline{std::chrono::seconds{seconds}}; };
  auto data = extract_db_fields(Op::ServerlessGetCredentials, snapshot(Op::ServerlessGetCredentials, fields(Op::ServerlessGetCredentials)));
  ASSERT_TRUE(data); EXPECT_EQ(1060000001, data.value().expiry.microseconds_since_epoch);
  EXPECT_EQ(1, data.value().expiry.microseconds_since_epoch % 1000); // Existing msadapters cannot silently take this.
  auto binding = Binding::create({Service::Redshift, "db.example", 5439, "pilot", "IAM:alice", "resource", "db.example", "trust"},
      {SourceKind::TrustedTemporaryDbIssuer, "source", "generation"}, Method::TemporaryDatabasePassword);
  ASSERT_TRUE(binding); auto req = Request::create(std::move(binding).value(), at(150), 6s); ASSERT_TRUE(req);
  TemporaryClockSample sample{{1000000000}, at(100), at(102), at(103), at(103), 3s, 0us, 0us, at(99)};
  auto result = convert_temporary_db_validity(req.value(), data.value().expiry, sample, {10s, 2s, 10s, 3600s});
  ASSERT_TRUE(result); EXPECT_EQ(at(157) + 1us, result.value().expires_at);
  // No issuer or Authority was selected: candidate data is not a receipt.
}
} // namespace
