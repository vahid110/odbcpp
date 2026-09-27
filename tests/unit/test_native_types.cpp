#include <gtest/gtest.h>

#include "core/database/database_factory.h"
#include "core/database/generic_database_connection.h"
#include "core/database/postgres/pg_database_connection.h"
#include "tests/mock_protocol_parser.h"

#include <limits>

using namespace rs::core::database;

TEST(NativeTypeTest, SelectedBackendNormalizesFixedScalarMetadata) {
  auto backend = DatabaseFactory::create_connection();
  struct Case { std::uint32_t id; ScalarType type; std::uint64_t size; };
  for (const auto& item : {Case{16, ScalarType::Boolean, 1},
       Case{20, ScalarType::BigInt, 19}, Case{21, ScalarType::SmallInt, 5},
       Case{23, ScalarType::Integer, 10}, Case{26, ScalarType::BigInt, 10},
       Case{700, ScalarType::Real, 7}, Case{701, ScalarType::Double, 15},
       Case{1082, ScalarType::Date, 10}, Case{2950, ScalarType::VarChar, 36}}) {
    SCOPED_TRACE(item.id);
    const auto type = backend->describe_type(item.id, -1, -1);
    EXPECT_TRUE(type.known);
    EXPECT_EQ(item.type, type.type);
    EXPECT_EQ(item.size, type.column_size);
  }
  EXPECT_FALSE(backend->is_connected());
}

TEST(NativeTypeTest, PreservesNumericAndTemporalModifiers) {
  auto backend = DatabaseFactory::create_connection();
  const auto numeric = backend->describe_type(1700, -1, (12 << 16) + 3 + 4);
  EXPECT_EQ(ScalarType::Numeric, numeric.type);
  EXPECT_EQ(12u, numeric.column_size);
  EXPECT_EQ(3, numeric.decimal_digits);
  const auto negative_scale = backend->describe_type(1700, -1, (8 << 16) + 2046 + 4);
  EXPECT_EQ(8u, negative_scale.column_size);
  EXPECT_EQ(-2, negative_scale.decimal_digits);
  EXPECT_EQ(0u, backend->describe_type(1700, -1, -1).column_size);
  for (const auto id : {1083u, 1266u, 1114u, 1184u}) {
    SCOPED_TRACE(id);
    const auto plain = backend->describe_type(id, -1, 0);
    const auto fractional = backend->describe_type(id, -1, 3);
    const auto unspecified = backend->describe_type(id, -1, -1);
    EXPECT_EQ(0, plain.decimal_digits);
    EXPECT_EQ(plain.column_size + 4, fractional.column_size);
    EXPECT_EQ(3, fractional.decimal_digits);
    EXPECT_EQ(plain.column_size + 7, unspecified.column_size);
    EXPECT_EQ(6, unspecified.decimal_digits);
    // Defensive width arithmetic must not overflow for a malformed modifier.
    const auto extreme = backend->describe_type(id, -1,
        std::numeric_limits<std::int32_t>::max());
    EXPECT_EQ(plain.column_size + 2147483648ULL, extreme.column_size);
  }
}

TEST(NativeTypeTest, PreservesCharacterBinaryAndUnknownFallbackSizes) {
  auto backend = DatabaseFactory::create_connection();
  for (const auto id : {1042u, 1043u}) {
    EXPECT_EQ(0u, backend->describe_type(id, -1, -1).column_size);
    EXPECT_EQ(0u, backend->describe_type(id, -1, 3).column_size);
    EXPECT_EQ(17u, backend->describe_type(id, -1, 21).column_size);
  }
  EXPECT_EQ(ScalarType::Char, backend->describe_type(1042, -1, 21).type);
  EXPECT_EQ(ScalarType::Binary, backend->describe_type(17, -1, -1).type);
  EXPECT_EQ(0u, backend->describe_type(17, -1, -1).column_size);
  EXPECT_EQ(32u, backend->describe_type(17, 32, -1).column_size);
  for (const auto size : {std::int16_t{-1}, std::int16_t{0}, std::int16_t{32}}) {
    const auto unknown = backend->describe_type(999999, size, -1);
    EXPECT_FALSE(unknown.known);
    EXPECT_EQ(ScalarType::VarChar, unknown.type);
    EXPECT_EQ(size > 0 ? 32u : 0u, unknown.column_size);
  }
  EXPECT_TRUE(backend->describe_type(23, 4, -1).known);
}

TEST(NativeTypeTest, GenericConnectionUsesSelectedParserTypeSemantics) {
  GenericDatabaseConnection backend(std::make_unique<odbcpp::test::MockProtocolParser>());
  // 23 is an integer OID in PostgreSQL, but has no such meaning in this parser.
  const auto type = backend.describe_type(23, 4, -1);
  EXPECT_EQ(ScalarType::Char, type.type);
  EXPECT_EQ(7u, type.column_size);
  EXPECT_TRUE(type.known);
  EXPECT_FALSE(backend.is_connected());
}

namespace {
class MetadataLookupConnection : public postgres::PgDatabaseConnection {
public:
  QueryResult response;
  std::optional<rs::util::DbErrorCode> failure;
  std::string query;
  rs::util::Deadline observed_deadline{};
  int queries = 0;

  rs::util::Result<QueryResult> execute_query(
      std::string_view sql, rs::util::Deadline deadline) override {
    ++queries;
    query = sql;
    observed_deadline = deadline;
    if (failure) return {*failure, "injected metadata failure"};
    return response;
  }
};
} // namespace

TEST(NativeTypeLookupTest, KnownEmptyAndUnspecifiedTypesRequireNoQuery) {
  MetadataLookupConnection backend;
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  const auto empty = backend.resolve_types({}, deadline);
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
  const std::uint32_t ids[]{23, 0, 23, 1700};
  const auto result = backend.resolve_types(ids, deadline);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(3u, result->size());
  EXPECT_EQ(ScalarType::Integer, result->at(23).type);
  EXPECT_EQ(ScalarType::Numeric, result->at(1700).type);
  EXPECT_FALSE(result->at(0).known);
  EXPECT_EQ(0, backend.queries);
}

TEST(NativeTypeLookupTest, ResolvesDeduplicatedDomainsWithCallerDeadline) {
  MetadataLookupConnection backend;
  backend.response.rows = {{"90000", "1043", "21"},
                           {"90001", "1700", std::to_string((8 << 16) + 2050)}};
  const std::uint32_t ids[]{90001, 23, 90000, 90001};
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  const auto result = backend.resolve_types(ids, deadline);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(3u, result->size());
  EXPECT_EQ(ScalarType::VarChar, result->at(90000).type);
  EXPECT_EQ(17u, result->at(90000).column_size);
  EXPECT_TRUE(result->at(90000).known);
  EXPECT_EQ(ScalarType::Numeric, result->at(90001).type);
  EXPECT_EQ(-2, result->at(90001).decimal_digits);
  EXPECT_EQ(8u, result->at(90001).column_size);
  EXPECT_EQ(1, backend.queries);
  EXPECT_EQ(deadline, backend.observed_deadline);
  EXPECT_NE(std::string::npos, backend.query.find("IN (90000,90001)"));
}

TEST(NativeTypeLookupTest, MissingAndUnknownBaseTypesRetainFallback) {
  MetadataLookupConnection backend;
  backend.response.rows = {{"90000", "999999", "-1"}};
  const std::uint32_t ids[]{90000, 90001};
  const auto result = backend.resolve_types(ids, rs::util::Deadline::max());
  ASSERT_TRUE(result.has_value());
  for (const auto id : ids) {
    EXPECT_FALSE(result->at(id).known);
    EXPECT_EQ(ScalarType::VarChar, result->at(id).type);
    EXPECT_EQ(0u, result->at(id).column_size);
  }
}

TEST(NativeTypeLookupTest, RejectsMalformedRowsWithoutReturningPartialMetadata) {
  MetadataLookupConnection backend;
  const std::uint32_t ids[]{90000, 90001};
  const std::vector<ResultRow> invalid_rows{
      {}, {"90001", "23"}, {"90001", std::nullopt, "-1"},
      {"90001", "23", std::nullopt}, {"90001", "23x", "-1"},
      {"-1", "23", "-1"}, {"90001", "4294967296", "-1"},
      {"90001", "23", "2147483648"}, {"90001", "0", "-1"},
      {"90002", "23", "-1"}, {"90000", "23", "-1"}};
  for (std::size_t index = 0; index < invalid_rows.size(); ++index) {
    SCOPED_TRACE(index);
    backend.response.rows = {{"90000", "23", "-1"}, invalid_rows[index]};
    const auto bad = backend.resolve_types(ids, rs::util::Deadline::max());
    ASSERT_TRUE(bad.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), bad.error());
    EXPECT_NE(std::string::npos, bad.error_message().find("invalid parameter type metadata"));
    backend.response.rows = {{"90000", "23", "-1"}, {"90001", "1043", "21"}};
    const auto recovered = backend.resolve_types(ids, rs::util::Deadline::max());
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(17u, recovered->at(90001).column_size);
  }
}

TEST(NativeTypeLookupTest, PropagatesQueryFailuresAndAllowsRetry) {
  MetadataLookupConnection backend;
  const std::uint32_t ids[]{90000};
  for (const auto error : {rs::util::DbErrorCode::Timeout,
                           rs::util::DbErrorCode::NetworkError,
                           rs::util::DbErrorCode::QueryFailed}) {
    backend.failure = error;
    const auto failed = backend.resolve_types(ids, rs::util::Deadline::min());
    ASSERT_TRUE(failed.has_error());
    EXPECT_EQ(rs::util::make_error_code(error), failed.error());
    EXPECT_EQ("injected metadata failure", failed.error_message());
    EXPECT_EQ(rs::util::Deadline::min(), backend.observed_deadline);
  }
  backend.failure.reset();
  backend.response.rows = {{"90000", "23", "-1"}};
  EXPECT_TRUE(backend.resolve_types(ids, rs::util::Deadline::max()).has_value());
}

TEST(NativeTypeLookupTest, GenericBackendUsesItsOwnFallbackWithoutPostgresDiscovery) {
  GenericDatabaseConnection backend(std::make_unique<odbcpp::test::MockProtocolParser>());
  const std::uint32_t ids[]{23, 90000};
  const auto result = backend.resolve_types(ids, rs::util::Deadline::max());
  ASSERT_TRUE(result.has_value());
  for (const auto id : ids) {
    EXPECT_EQ(ScalarType::Char, result->at(id).type);
    EXPECT_EQ(7u, result->at(id).column_size);
  }
}

namespace {
class VersionedTypeCatalogConnection : public postgres::PgDatabaseConnection {
public:
  std::string version;
  std::string get_parameter(std::string_view key) const override {
    return key == "server_version" ? version : std::string{};
  }
};
}

TEST(TypeCatalogTest, AdvertisesNativeNamesAndNullablePropertiesWithoutIo) {
  auto backend = DatabaseFactory::create_connection();
  const auto catalog = backend->type_catalog();
  ASSERT_EQ(15u, catalog.size());
  for (const auto& type : catalog) {
    EXPECT_FALSE(type.name.empty());
    EXPECT_GT(type.column_size, 0u);
    if (type.type == ScalarType::VarChar) {
      EXPECT_EQ("varchar", type.name);
      EXPECT_EQ(10485760u, type.column_size);
      EXPECT_EQ(std::optional<std::string_view>("'"), type.literal_prefix);
      EXPECT_EQ(std::optional<std::string_view>("length"), type.create_params);
      EXPECT_FALSE(type.minimum_scale.has_value());
      EXPECT_FALSE(type.unsigned_attribute.has_value());
      EXPECT_TRUE(type.case_sensitive);
    }
    if (type.type == ScalarType::Integer) {
      EXPECT_EQ("integer", type.name);
      EXPECT_EQ(std::optional<bool>(false), type.unsigned_attribute);
      EXPECT_EQ(std::optional<std::int16_t>(0), type.minimum_scale);
      EXPECT_FALSE(type.literal_prefix.has_value());
      EXPECT_EQ(10, type.numeric_radix);
    }
  }
  EXPECT_FALSE(backend->is_connected());
}

TEST(TypeCatalogTest, NumericScaleTracksServerVersionWithoutInvalidatingPriorViews) {
  VersionedTypeCatalogConnection backend;
  const auto original = backend.type_catalog();
  for (const auto* version : {"", "14.18", "15.0", "17.11 (package)",
                              "invalid", "999999999999999999999"}) {
    backend.version = version;
    const bool modern = backend.version.starts_with("15.") || backend.version.starts_with("17.");
    int numerics = 0;
    for (const auto& type : backend.type_catalog()) {
      if (type.type == ScalarType::Numeric || type.type == ScalarType::Decimal) {
        ++numerics;
        EXPECT_EQ(std::optional<std::int16_t>(modern ? -1000 : 0), type.minimum_scale);
        EXPECT_EQ(std::optional<std::int16_t>(1000), type.maximum_scale);
      }
    }
    EXPECT_EQ(2, numerics);
  }
  for (const auto& type : original) {
    if (type.type == ScalarType::Numeric) {
      EXPECT_EQ(std::optional<std::int16_t>(0), type.minimum_scale);
    }
  }
}

TEST(TypeCatalogTest, GenericBackendDoesNotAdvertisePostgresTypes) {
  GenericDatabaseConnection backend(std::make_unique<odbcpp::test::MockProtocolParser>());
  EXPECT_TRUE(backend.type_catalog().empty());
  EXPECT_FALSE(backend.is_connected());
}
