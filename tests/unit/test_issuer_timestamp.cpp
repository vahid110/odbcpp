#include "odbcpp/auth/issuer_timestamp.h"
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace rs::core::auth;
using F = IssuerTimestampFormat;
using E = TimestampFailure;
void value(F format, std::string_view text, std::int64_t expected) {
  auto result = parse_issuer_timestamp(format, text); ASSERT_TRUE(result);
  EXPECT_EQ(expected, result.value().microseconds_since_epoch);
}
void failure(F format, std::string_view text, E expected) {
  auto result = parse_issuer_timestamp(format, text); ASSERT_FALSE(result);
  EXPECT_EQ(expected, result.failure());
}
TEST(IssuerTimestampTest, EquivalentNumericSpellingsAreExactIntegerMicroseconds) {
  for (auto text : {"1060.000001", "1060000001e-6", "1.060000001e3", "1060.0000010"})
    value(F::JsonSeconds, text, 1060000001);
  for (auto text : {"1", "1.0", "1e0", "1E+00", "0.1e1", "1000000e-6"}) value(F::JsonSeconds, text, 1000000);
  value(F::JsonSeconds, "1e-6", 1); value(F::JsonSeconds, "0.000001", 1);
  value(F::JsonSeconds, "10e-7", 1); // Exact division, no discarded nonzero digit.
  value(F::JsonSeconds, "0.000000000001e6", 1);
}
TEST(IssuerTimestampTest, Int64LimitIsExactWithoutFloatOrSaturation) {
  constexpr auto maximum = (std::numeric_limits<std::int64_t>::max)();
  value(F::JsonSeconds, "9223372036854.775807", maximum);
  value(F::JsonSeconds, "9223372036854775807e-6", maximum);
  value(F::JsonSeconds, "9223372036854.775806", maximum - 1);
  failure(F::JsonSeconds, "9223372036854.775808", E::Overflow);
  failure(F::JsonSeconds, "9223372036855", E::Overflow);
  failure(F::JsonSeconds, "9223372036854775808e-6", E::Overflow);
  failure(F::JsonSeconds, "1e18", E::Overflow);
}
TEST(IssuerTimestampTest, SubmicrosecondPrecisionNeverRoundsOrTruncates) {
  for (auto text : {"1e-7", "0.0000001", "1060.0000019", "9e-18", "1.000000000000000001"})
    failure(F::JsonSeconds, text, E::PrecisionLoss);
  value(F::JsonSeconds, "1.000000000000000000", 1000000);
  value(F::JsonSeconds, "0.000000000000000001e12", 1);
}
TEST(IssuerTimestampTest, NumericGrammarIsCompleteAsciiAndFiniteOnly) {
  for (auto text : {"", "-", "+1", "01", "00.1", ".1", "1.", "1e", "1e+", "1e-", "1 0",
       " 1", "1\n", "true", "null", "NaN", "Infinity", "-Infinity", "1.2.3", "1e1x", "0x1", "\"1\""})
    failure(F::JsonSeconds, text, E::InvalidSyntax);
  failure(F::JsonSeconds, std::string("1\0x", 3), E::InvalidSyntax);
  failure(F::JsonSeconds, std::string("\xff", 1), E::InvalidSyntax);
  for (auto text : {"0", "0.0", "-0", "-0.0", "-1", "-1e-6"}) failure(F::JsonSeconds, text, E::Nonpositive);
}
TEST(IssuerTimestampTest, ResourceLimitsBoundDigitsAndExponentBeforeScaling) {
  for (auto text : {"1e19", "1e-19", "1e000", "1e9999999999999999999999",
       "10000000000000000000", "0.0000000000000000001"}) failure(F::JsonSeconds, text, E::ResourceLimit);
  failure(F::JsonSeconds, std::string(65, '1'), E::ResourceLimit);
  value(F::JsonSeconds, "1e+06", 1000000000000);
  failure(F::JsonSeconds, "1e-18", E::PrecisionLoss); // Local exponent bound inclusive.
}
TEST(IssuerTimestampTest, CheckedScalingAndFailurePrecedenceRemainBounded) {
  failure(F::JsonSeconds, "0.1e18", E::Overflow); // scale23: incremental checked multiply.
  failure(F::JsonSeconds, "1e-18", E::PrecisionLoss); // scale-12: no overflowing divisor.
  failure(F::JsonSeconds, "0.000000000000000001e-18", E::PrecisionLoss); // scale-30.
  failure(F::JsonSeconds, "1e99x", E::ResourceLimit); // Parsed exponent policy before trailing junk.
  failure(F::JsonSeconds, "9223372036854775808x", E::Overflow); // Checked coefficient before trailing junk.
  failure(static_cast<F>(99), std::string(65, '1'), E::ResourceLimit); // Entry bound before dispatch.
  failure(static_cast<F>(99), "", E::InvalidSyntax);
}
TEST(IssuerTimestampTest, CanonicalUtcFractionAndNumericFormatAgree) {
  value(F::UtcIso8601, "1970-01-01T00:17:40.000001Z", 1060000001);
  value(F::UtcIso8601, "1970-01-01T00:00:00.000001Z", 1);
  value(F::UtcIso8601, "1970-01-01T00:00:00.1Z", 100000);
  value(F::UtcIso8601, "1970-01-01T00:00:01Z", 1000000);
  value(F::UtcIso8601, "1970-01-01T23:59:59.999999Z", 86399999999);
  failure(F::UtcIso8601, "1970-01-01T00:00:00Z", E::Nonpositive);
}
TEST(IssuerTimestampTest, GregorianCalendarValidatesLeapCenturiesAndMonthBoundaries) {
  value(F::UtcIso8601, "2000-02-29T00:00:00Z", 951782400000000);
  value(F::UtcIso8601, "2024-02-29T00:00:00Z", 1709164800000000);
  value(F::UtcIso8601, "2024-03-01T00:00:00Z", 1709251200000000);
  value(F::UtcIso8601, "9999-12-31T23:59:59.999999Z", 253402300799999999);
  for (auto text : {"2100-02-29T00:00:00Z", "2023-02-29T00:00:00Z", "2024-04-31T00:00:00Z",
       "2024-00-01T00:00:00Z", "2024-13-01T00:00:00Z", "2024-01-00T00:00:00Z", "2024-01-32T00:00:00Z",
       "1969-12-31T23:59:59Z"}) failure(F::UtcIso8601, text, E::InvalidSyntax);
}
TEST(IssuerTimestampTest, IsoSubsetRefusesNormalizationOffsetsAndExtraPrecision) {
  for (auto text : {"2024-01-01T24:00:00Z", "2024-01-01T00:60:00Z", "2024-01-01T00:00:60Z",
       "2024-01-01t00:00:00Z", "2024-01-01T00:00:00z", "2024-01-01T00:00:00", "2024-01-01T00:00:00+00:00",
       "2024-01-01 00:00:00Z", "2024-01-01T00:00:00.Z", "2024-01-01T00:00:00.1xZ", "2024-01-01T00:00:00Zx",
       "2024-1-01T00:00:00Z", " 2024-01-01T00:00:00Z"}) failure(F::UtcIso8601, text, E::InvalidSyntax);
  failure(F::UtcIso8601, "2024-01-01T00:00:00.0000000Z", E::PrecisionLoss);
  failure(F::UtcIso8601, "2024-01-01T00:00:00.0000001Z", E::PrecisionLoss);
  failure(F::UtcIso8601, std::string("2024-01-01T00:00:00.1\0Z", 23), E::InvalidSyntax);
}
TEST(IssuerTimestampTest, FormatTagIsolationAndSafeErrorsNeverEchoInput) {
  failure(F::JsonSeconds, "2024-01-01T00:00:00Z", E::InvalidSyntax);
  failure(F::UtcIso8601, "1704067200", E::InvalidSyntax);
  failure(static_cast<F>(99), "1", E::UnsupportedFormat);
  for (auto e : {E::UnsupportedFormat, E::InvalidSyntax, E::ResourceLimit, E::Nonpositive, E::PrecisionLoss, E::Overflow}) {
    auto safe = TimestampResult::safe_message(e); EXPECT_FALSE(safe.empty());
    EXPECT_EQ(std::string_view::npos, safe.find("SECRET_SENTINEL"));
  }
  failure(F::JsonSeconds, "SECRET_SENTINEL", E::InvalidSyntax);
}
TEST(IssuerTimestampTest, ExactParsedExpiryComposesWithOriginalDeadlineAndQuality) {
  using namespace std::chrono_literals;
  using rs::util::Deadline;
  const auto at = [](long long seconds) { return Deadline{std::chrono::seconds{seconds}}; };
  auto binding = Binding::create({Service::Redshift, "db.example", 5439, "pilot", "user", "resource", "db.example", "trust"},
      {SourceKind::TrustedTemporaryDbIssuer, "source", "generation"}, Method::TemporaryDatabasePassword);
  ASSERT_TRUE(binding);
  auto req = Request::create(std::move(binding).value(), at(150), 6s); ASSERT_TRUE(req);
  auto expiry = parse_issuer_timestamp(F::JsonSeconds, "1060.000001"); ASSERT_TRUE(expiry);
  TemporaryClockSample sample{{1000000000}, at(100), at(102), at(103), at(103), 3s, 0us, 0us, at(99)};
  TemporaryTimePolicy policy{10s, 2s, 10s, 3600s};
  auto converted = convert_temporary_db_validity(req.value(), expiry.value(), sample, policy); ASSERT_TRUE(converted);
  EXPECT_EQ(at(157) + 1us, converted.value().expires_at); EXPECT_EQ(at(103), converted.value().issued_at);
  auto boundary = Request::create(req.value().binding(), at(150), 7s + 1us); ASSERT_TRUE(boundary);
  auto refused = convert_temporary_db_validity(boundary.value(), expiry.value(), sample, policy);
  ASSERT_FALSE(refused); EXPECT_EQ(TimeFailure::Expired, refused.failure());
}
} // namespace
