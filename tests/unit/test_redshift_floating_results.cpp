// Redshift REAL/DOUBLE result precision (audit N10, docs/redshift-conversion-coverage.md).
// Redshift text output defaults to DBL_DIG/FLT_DIG significant digits, which
// does not identify the stored value. The Redshift profile requests
// extra_float_digits=2 at startup (AWS JDBC 773fa16 ConnectionFactoryImpl.java:425-428,
// its default for Redshift's 8.0.2 server version) and re-emits the shortest
// exact decimal. Server spellings here follow PostgreSQL 8.0 float8out/float4out
// (%.17g / %.8g); actual Redshift output remains a live observation.
#include "tests/support/redshift_scripted_wire.h"

#include "core/database/postgres/pg_protocol_parser.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace rs::core::database;
using namespace rs::tests::redshift_wire;

// Key/value fields of a StartupMessage after its length and protocol words.
std::map<std::string, std::string> startup_fields(const std::string& message) {
  std::map<std::string, std::string> fields;
  std::size_t position = 8;
  while (position < message.size() && message[position] != '\0') {
    const auto key_end = message.find('\0', position);
    const auto value_end = message.find('\0', key_end + 1);
    fields[message.substr(position, key_end - position)] =
        message.substr(key_end + 1, value_end - key_end - 1);
    position = value_end + 1;
  }
  return fields;
}

std::string to_string(const std::vector<std::byte>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

TEST(RedshiftFloatingStartupTest, OnlyRedshiftRequestsExtraFloatDigitsWithinExactBudget) {
  const std::map<std::string, std::string> params{{"client_encoding", "UTF8"}};
  postgres::PgProtocolParser postgres_parser;
  postgres::PgProtocolParser redshift_parser(postgres::PgCatalogProfile::Redshift);
  const auto postgres_message = postgres_parser.create_startup_message("u", "d", params);
  const auto redshift_message = redshift_parser.create_startup_message("u", "d", params);
  EXPECT_EQ(0u, startup_fields(to_string(postgres_message)).count("extra_float_digits"));
  const auto fields = startup_fields(to_string(redshift_message));
  ASSERT_EQ(1u, fields.count("extra_float_digits"));
  EXPECT_EQ("2", fields.at("extra_float_digits"));
  // "extra_float_digits\0" + "2\0" is counted in the exact wire budget.
  ASSERT_EQ(postgres_message.size() + 21, redshift_message.size());
  EXPECT_NO_THROW(redshift_parser.create_startup_message("u", "d", params, redshift_message.size()));
  EXPECT_THROW(redshift_parser.create_startup_message("u", "d", params, redshift_message.size() - 1),
               RequestWireLimitExceeded);
  // An explicit caller value is not duplicated or overridden.
  const std::map<std::string, std::string> explicit_params{{"extra_float_digits", "1"}};
  const auto explicit_fields = startup_fields(to_string(
      redshift_parser.create_startup_message("u", "d", explicit_params)));
  EXPECT_EQ("1", explicit_fields.at("extra_float_digits"));
}

TEST(RedshiftFloatingCodecTest, ShortestExactTextKeepsDefaultNotationAndSpecials) {
  postgres::PgDatabaseConnection redshift(nullptr, std::nullopt, postgres::PgCatalogProfile::Redshift);
  postgres::PgDatabaseConnection postgres_connection;
  const std::pair<const char*, const char*> doubles[]{
      {"0.10000000000000001", "0.1"},
      {"0.30000000000000004", "0.30000000000000004"},
      {"1.7976931348623157e+308", "1.7976931348623157e+308"},
      {"4.9406564584124654e-324", "5e-324"},
      {"2.2250738585072014e-308", "2.2250738585072014e-308"},
      {"-0", "-0"},
      {"0", "0"},
      {"100000", "100000"},
      {"10000000000000000", "1e+16"}, // %.17g keeps fixed notation at exponent 16.
      {"123456789012345", "123456789012345"},
      {"1.2345678901234568e+17", "1.2345678901234568e+17"},
      {"0.0001", "0.0001"},
      {"1.0000000000000001e-05", "1e-05"},
      {"-1234.5", "-1234.5"},
      {"NaN", "NaN"},
      {"Infinity", "Infinity"},
      {"-Infinity", "-Infinity"},
      // Unrecognised or non-identifying spellings are kept, never errors.
      {"1e-400", "1e-400"},
      {"1e+400", "1e+400"},
      {"abc", "abc"},
      {" 1", " 1"},
  };
  for (const auto& [server, expected] : doubles) {
    SCOPED_TRACE(server);
    EXPECT_EQ(std::optional<std::string>(expected),
              redshift.normalize_native_result_value(701, ScalarType::Double, server));
    // PostgreSQL keeps its own server text unchanged.
    EXPECT_EQ(std::optional<std::string>(server),
              postgres_connection.normalize_native_result_value(701, ScalarType::Double, server));
  }
  // Genuine %.8g float4out spellings (extra_float_digits=2).
  const std::pair<const char*, const char*> reals[]{
      {"0.1", "0.1"},
      {"3.4028235e+38", "3.4028235e+38"},
      {"1.4012985e-45", "1e-45"},
      {"100000", "100000"},
      {"1000000", "1e+06"},
      {"-0", "-0"},
      {"NaN", "NaN"},
      // Documented residual: 8 digits name a neighbour of the stored 0x625d754f.
      {"1.0212959e+21", "1.0212959e+21"},
  };
  for (const auto& [server, expected] : reals) {
    SCOPED_TRACE(server);
    EXPECT_EQ(std::optional<std::string>(expected),
              redshift.normalize_native_result_value(700, ScalarType::Real, server));
  }
}

TEST(RedshiftFloatingCodecTest, EightDigitRealResidualSelectsTheNamedNeighbour) {
  // Not a fix claim: %.8g cannot identify every float. The stored value
  // 0x625d754f prints as 1.0212959e+21, which is the float 0x625d7550.
  postgres::PgDatabaseConnection redshift(nullptr, std::nullopt, postgres::PgCatalogProfile::Redshift);
  const auto text = redshift.normalize_native_result_value(700, ScalarType::Real, "1.0212959e+21");
  ASSERT_TRUE(text);
  const float parsed = std::strtof(text->c_str(), nullptr);
  EXPECT_EQ(0x625d7550u, std::bit_cast<std::uint32_t>(parsed));
  EXPECT_NE(0x625d754fu, std::bit_cast<std::uint32_t>(parsed));
}

class RedshiftFloatingResultsTest : public RedshiftScriptedSessionTest {};

TEST_F(RedshiftFloatingResultsTest, SessionStartupRequestsExtraFloatDigits) {
  const auto fields = startup_fields(script->startup);
  ASSERT_EQ(1u, fields.count("extra_float_digits"));
  EXPECT_EQ("2", fields.at("extra_float_digits"));
  EXPECT_EQ("UTF8", fields.at("client_encoding")); // Existing parameters retained.
}

TEST_F(RedshiftFloatingResultsTest, DoubleAndRealTargetsReceiveTheStoredValue) {
  script->responses["SELECT floats"] =
      row_description({{"d", 701}, {"r", 700}}) +
      data_row({std::string("0.30000000000000004"), std::string("0.1")}) +
      data_row({std::string("1.7976931348623157e+308"), std::string("3.4028235e+38")}) +
      data_row({std::string("4.9406564584124654e-324"), std::string("1.4012985e-45")}) +
      data_row({std::string("-0"), std::string("-0")}) +
      data_row({std::string("0.10000000000000001"), std::nullopt}) +
      complete("SELECT 5");
  const double doubles[]{0.1 + 0.2, std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::denorm_min(), -0.0, 0.1};
  const char* double_text[]{"0.30000000000000004", "1.7976931348623157e+308", "5e-324", "-0", "0.1"};
  const float reals[]{0.1f, std::numeric_limits<float>::max(), std::numeric_limits<float>::denorm_min(), -0.0f};
  const char* real_text[]{"0.1", "3.4028235e+38", "1e-45", "-0"};
  // One target per column and fetch: a repeated fixed-length SQLGetData is SQL_NO_DATA.
  for (const bool character : {false, true}) {
    SCOPED_TRACE(character);
    ASSERT_EQ(SQL_SUCCESS, execute("SELECT floats"));
    for (std::size_t row = 0; row < 5; ++row) {
      SCOPED_TRACE(row);
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      SQLLEN length = -7;
      char text[64]{};
      if (character) {
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, text, sizeof(text), &length));
        EXPECT_STREQ(double_text[row], text);
      } else {
        double d = 1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_DOUBLE, &d, sizeof(d), &length));
        EXPECT_EQ(std::bit_cast<std::uint64_t>(doubles[row]), std::bit_cast<std::uint64_t>(d));
      }
      if (row == 4) {
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_CHAR, text, sizeof(text), &length));
        EXPECT_EQ(SQL_NULL_DATA, length);
        continue;
      }
      if (character) {
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_CHAR, text, sizeof(text), &length));
        EXPECT_STREQ(real_text[row], text);
      } else {
        float r = 1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_FLOAT, &r, sizeof(r), &length));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(reals[row]), std::bit_cast<std::uint32_t>(r));
      }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(RedshiftFloatingResultsTest, ExactDoubleTextChangesNumericTruncationOnlyWhereValueDiffers) {
  // 0.1+0.2 is not 0.3: an exact NUMERIC(38,2) target now reports fractional
  // truncation, while a value whose decimal is exact converts without warning.
  script->responses["SELECT sums"] =
      row_description({{"d", 701}}) + data_row({std::string("0.30000000000000004")}) +
      data_row({std::string("0.25")}) + complete("SELECT 2");
  ASSERT_EQ(SQL_SUCCESS, execute("SELECT sums"));
  SQLHDESC ard = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr));
  SQL_NUMERIC_STRUCT numeric{};
  SQLLEN length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_NUMERIC, &numeric, sizeof(numeric), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION, reinterpret_cast<SQLPOINTER>(38), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE, reinterpret_cast<SQLPOINTER>(2), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_DATA_PTR, &numeric, 0));
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt));
  EXPECT_EQ("01S07", state());
  EXPECT_EQ(30, numeric.val[0]);
  EXPECT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(25, numeric.val[0]);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
}

} // namespace
