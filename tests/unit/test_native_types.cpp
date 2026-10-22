#include "odbcpp/util/hex.h"
#include <gtest/gtest.h>

#include "core/database/database_factory.h"
#include "core/database/postgres/pg_backend_provider.h"
#include "core/database/generic_database_connection.h"
#include "core/database/postgres/pg_database_connection.h"
#include "tests/mock_protocol_parser.h"

#include <limits>

using namespace rs::core::database;

TEST(NativeTypeTest, PostgresFamilyNormalizesFixedScalarMetadata) {
  auto backend = std::make_unique<postgres::PgDatabaseConnection>();
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
  auto backend = std::make_unique<postgres::PgDatabaseConnection>();
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
  auto backend = std::make_unique<postgres::PgDatabaseConnection>();
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
  MetadataLookupConnection() : PgDatabaseConnection() {}
  QueryResult response;
  std::optional<rs::util::DbErrorCode> failure;
  std::string query;
  rs::util::Deadline observed_deadline{};
  int queries = 0;
  SessionState state = SessionState::Disconnected;
  SessionState session_state() const override { return state; }

  rs::core::database::BackendResult<QueryResult> execute_query(
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
    EXPECT_EQ(bad.backend_error().operation, BackendOperation::ResolveTypes);
    EXPECT_EQ(bad.backend_error().error_class, BackendErrorClass::InvalidMetadata);
    EXPECT_EQ(bad.backend_error().session_state, SessionState::Disconnected);
    EXPECT_EQ(bad.backend_error().disposition, SessionDisposition::Retire);
    EXPECT_FALSE(bad.backend_error().native_state);
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

TEST(TypeCatalogTest, AdvertisesNativeNamesAndNullablePropertiesWithoutIo) {
  auto backend = DatabaseFactory::create_connection();
  const auto catalog = configured_backend_provider().type_catalog(backend->server_version());
  const bool redshift = DatabaseFactory::get_compiled_database_type() == DatabaseType::Redshift;
  // Changed oracle: Redshift adds its SUPER advertisement (14 -> 15 rows);
  // see docs/redshift-special-types.md.
  ASSERT_EQ(15u, catalog.size()); // Both profiles now advertise 15 rows.
  int chars = 0;
  int varbytes = 0;
  int supers = 0;
  for (const auto& type : catalog) {
    EXPECT_FALSE(type.name.empty());
    EXPECT_GT(type.column_size, 0u);
    if (type.type == ScalarType::VarChar) {
      EXPECT_EQ("varchar", type.name);
      EXPECT_EQ(redshift ? 65535u : 10485760u, type.column_size);
      EXPECT_EQ(std::optional<std::string_view>("'"), type.literal_prefix);
      EXPECT_EQ(std::optional<std::string_view>("length"), type.create_params);
      EXPECT_FALSE(type.minimum_scale.has_value());
      EXPECT_FALSE(type.unsigned_attribute.has_value());
      EXPECT_TRUE(type.case_sensitive);
    }
    if (type.type == ScalarType::Char) {
      ++chars;
      EXPECT_EQ("char", type.name);
      EXPECT_EQ(redshift ? 4096u : 10485760u, type.column_size);
      EXPECT_EQ(std::optional<std::string_view>("'"), type.literal_prefix);
      EXPECT_EQ(std::optional<std::string_view>("'"), type.literal_suffix);
      EXPECT_EQ(std::optional<std::string_view>("length"), type.create_params);
      EXPECT_TRUE(type.case_sensitive);
    }
    if (redshift) {
      EXPECT_NE(ScalarType::Binary, type.type);
    }
    if (redshift && type.type == ScalarType::LongVarChar) {
      ++supers; // PostgreSQL text remains absent; only SUPER is LONGVARCHAR.
      EXPECT_EQ("super", type.name);
      EXPECT_EQ(16777216u, type.column_size);
      EXPECT_FALSE(type.literal_prefix.has_value());
      EXPECT_FALSE(type.create_params.has_value());
      EXPECT_TRUE(type.case_sensitive);
    }
    if (type.type == ScalarType::LongVarBinary) {
      ++varbytes;
      EXPECT_TRUE(redshift);
      EXPECT_EQ("varbyte", type.name);
      EXPECT_EQ(16777216u, type.column_size);
      EXPECT_FALSE(type.literal_prefix.has_value());
      EXPECT_FALSE(type.literal_suffix.has_value());
      EXPECT_EQ(std::optional<std::string_view>("length"), type.create_params);
      EXPECT_FALSE(type.case_sensitive);
    }
    if (type.type == ScalarType::Integer) {
      EXPECT_EQ("integer", type.name);
      EXPECT_EQ(std::optional<bool>(false), type.unsigned_attribute);
      EXPECT_EQ(std::optional<std::int16_t>(0), type.minimum_scale);
      EXPECT_FALSE(type.literal_prefix.has_value());
      EXPECT_EQ(10, type.numeric_radix);
    }
  }
  EXPECT_EQ(1, chars);
  EXPECT_EQ(redshift ? 1 : 0, varbytes);
  EXPECT_EQ(redshift ? 1 : 0, supers);
  EXPECT_FALSE(backend->is_connected());
}

TEST(TypeCatalogTest, NumericScaleTracksServerVersionWithoutInvalidatingPriorViews) {
  const postgres::PgBackendProvider provider{{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      {"localhost", 5432, "postgres", true}};
  const auto original = provider.type_catalog();
  for (const auto* version : {"", "14.18", "15.0", "17.11 (package)",
                              "invalid", "999999999999999999999"}) {
    const std::string_view version_view = version;
    const bool modern = version_view.starts_with("15.") || version_view.starts_with("17.");
    int numerics = 0;
    for (const auto& type : provider.type_catalog(version_view)) {
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

TEST(BackendCapabilitiesTest, SelectedBackendProfileIsAvailableWithoutIo) {
  auto backend = DatabaseFactory::create_connection();
  const auto profile = configured_backend_provider().capabilities();
  const auto expected_name =
      DatabaseFactory::get_compiled_database_type() == DatabaseType::Redshift
          ? "Amazon Redshift"
          : "PostgreSQL";
  EXPECT_EQ(expected_name, profile.dbms_name);
  EXPECT_EQ(DatabaseFactory::get_compiled_database_type() == DatabaseType::Redshift ? 127 : 63,
      profile.max_identifier_length);
  EXPECT_EQ(IdentifierCase::Lower, profile.identifier_case);
  EXPECT_EQ(IdentifierCase::Sensitive, profile.quoted_identifier_case);
  EXPECT_EQ(NullCollation::High, profile.null_collation);
  EXPECT_EQ("\"", profile.identifier_quote);
  EXPECT_TRUE(profile.concat_null_yields_null);
  EXPECT_TRUE(profile.union_distinct);
  EXPECT_TRUE(profile.union_all);
  EXPECT_FALSE(profile.order_by_requires_select);
  EXPECT_FALSE(profile.read_only);
  backend->disconnect();
  EXPECT_EQ(expected_name, profile.dbms_name);
  EXPECT_EQ(profile.identifier_quote, configured_backend_provider().capabilities().identifier_quote);
  EXPECT_FALSE(backend->is_connected());
}

TEST(BackendCapabilitiesTest, UnspecifiedProfileDoesNotInheritPostgresClaims) {
  GenericDatabaseConnection backend(std::make_unique<odbcpp::test::MockProtocolParser>());
  const BackendCapabilities profile{};
  EXPECT_TRUE(profile.dbms_name.empty());
  EXPECT_TRUE(profile.identifier_quote.empty());
  EXPECT_EQ(0, profile.max_identifier_length);
  EXPECT_EQ(CorrelationNames::None, profile.correlation_names);
  EXPECT_EQ(GroupBySupport::None, profile.group_by);
  EXPECT_FALSE(profile.catalog_names);
  EXPECT_FALSE(profile.column_aliases);
  EXPECT_FALSE(profile.describe_parameters);
  EXPECT_FALSE(profile.procedures);
  EXPECT_FALSE(profile.create_index);
  EXPECT_FALSE(profile.insert_literals);
  EXPECT_FALSE(profile.sql92_entry);
  EXPECT_FALSE(profile.union_all);
  EXPECT_FALSE(profile.schema_in_dml);
  EXPECT_TRUE(profile.read_only);
  EXPECT_FALSE(backend.is_connected());
}

TEST(BackendErrorsTest, PostgresNormalizesNativeStatesWithoutIo) {
  auto backend = DatabaseFactory::create_connection();
  struct Case { const char* native; ErrorContext context; const char* expected; };
  for (const auto& item : {
      Case{"22P02", ErrorContext::Unknown, "22018"},
      Case{"22012", ErrorContext::Unknown, "22012"},
      Case{"23505", ErrorContext::Unknown, "23000"},
      Case{"25P02", ErrorContext::Unknown, "25P02"},
      Case{"3F000", ErrorContext::Unknown, "3F000"},
      Case{"42P07", ErrorContext::CreateTable, "42S01"},
      Case{"42P07", ErrorContext::CreateView, "42S01"},
      Case{"42P07", ErrorContext::CreateIndex, "42S11"},
      Case{"42P01", ErrorContext::Unknown, "42S02"},
      Case{"42704", ErrorContext::DropIndex, "42S12"},
      Case{"42701", ErrorContext::Unknown, "42S21"},
      Case{"42703", ErrorContext::Unknown, "42S22"}}) {
    SCOPED_TRACE(item.native);
    EXPECT_EQ(std::optional<std::string>(item.expected),
              configured_backend_provider().normalize_error_sqlstate(item.native, item.context));
  }
  EXPECT_FALSE(backend->is_connected());
}

TEST(BackendErrorsTest, InvalidUnknownAndAmbiguousStatesKeepCallerFallback) {
  const auto& backend = configured_backend_provider();
  for (const auto state : {"", "22P0", "22P020", "22p02", "22!02", "XXXXX",
                           "P0001", "42P07", "42704", "25P03", "25p02", "25!02"}) {
    SCOPED_TRACE(state);
    EXPECT_FALSE(backend.normalize_error_sqlstate(state, ErrorContext::Unknown));
  }
  EXPECT_FALSE(backend.normalize_error_sqlstate("42P07", ErrorContext::DropIndex));
  EXPECT_FALSE(backend.normalize_error_sqlstate("42704", ErrorContext::CreateIndex));
  EXPECT_FALSE(backend.normalize_error_sqlstate(std::string("22\0\0\0", 5),
                                                ErrorContext::Unknown));
}

TEST(BackendErrorsTest, NormalizedStateOwnsItsStorage) {
  std::optional<std::string> normalized;
  {
    postgres::PgBackendProvider backend{
        BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
        BackendConnectionDefaults{"localhost", 5432, "postgres", true}};
    std::string native = "22012";
    normalized = backend.normalize_error_sqlstate(native, ErrorContext::Unknown);
    native.assign("XXXXX");
  }
  EXPECT_EQ(std::optional<std::string>("22012"), normalized);
}

TEST(BackendErrorsTest, RedshiftNullViolationUsesOwningDetailAndPreservesNativeFacts) {
  postgres::PgBackendProvider redshift{
      BackendIdentity{"redshift", "Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"localhost", 5439, "dev", true}, std::nullopt,
      postgres::PgCatalogProfile::Redshift};
  postgres::PgBackendProvider postgres{
      BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"localhost", 5432, "postgres", true}};
  const IBackendProvider& provider = redshift;
  for (const auto* column : {"id", "\"quoted column\"", "列_雪"}) {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),
        std::string{"Query error: Cannot insert a NULL value into column "} + column};
    error.native_state = "XX000";
    error.session_state = SessionState::Idle;
    error.disposition = SessionDisposition::Reusable;
    const auto original = error;
    EXPECT_EQ(std::optional<std::string>{"23000"},
              provider.normalize_error_sqlstate(error, ErrorContext::Unknown));
    EXPECT_FALSE(postgres.normalize_error_sqlstate(error, ErrorContext::Unknown));
    EXPECT_EQ(original.native_state, error.native_state);
    EXPECT_EQ(original.message, error.message);
    EXPECT_EQ(original.code, error.code);
    EXPECT_EQ(original.error_class, error.error_class);
    EXPECT_EQ(original.session_state, error.session_state);
    EXPECT_EQ(original.disposition, error.disposition);
  }
}

TEST(BackendErrorsTest, RedshiftNullViolationRejectsAmbiguousMalformedAndUnobservedDetails) {
  postgres::PgBackendProvider redshift{
      BackendIdentity{"redshift", "Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"localhost", 5439, "dev", true}, std::nullopt,
      postgres::PgCatalogProfile::Redshift};
  const std::string prefix{"Query error: Cannot insert a NULL value into column "};
  for (const auto& message : std::vector<std::string>{
      "Query error: unrelated internal failure", prefix, prefix + " ",
      prefix + "id\nDETAIL", prefix + "id\t", prefix + std::string("id\0x", 4),
      prefix + "id\u0085", prefix + "id\u2028",
      prefix + std::string(1, static_cast<char>(0xff)), prefix + std::string(128, 'x'),
      "Cannot insert a NULL value into column id",
      "Query error: Cannot insert a null value into column id"}) {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), message};
    error.native_state = "XX000";
    EXPECT_FALSE(redshift.normalize_error_sqlstate(error, ErrorContext::Unknown));
  }
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), prefix + "id"};
  for (const auto* state : {"XX001", "XX000x", "xx000", "XXXXX"}) {
    error.native_state = state;
    EXPECT_FALSE(redshift.normalize_error_sqlstate(error, ErrorContext::Unknown));
  }
  error.native_state = "XX000";
  for (const auto context : {ErrorContext::CreateTable, ErrorContext::CreateView,
      ErrorContext::CreateIndex, ErrorContext::DropIndex}) {
    EXPECT_FALSE(redshift.normalize_error_sqlstate(error, context));
  }
  error.error_class = BackendErrorClass::InvalidInput;
  EXPECT_FALSE(redshift.normalize_error_sqlstate(error, ErrorContext::Unknown));
  error.error_class = BackendErrorClass::Server;
  error.code = rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError);
  EXPECT_FALSE(redshift.normalize_error_sqlstate(error, ErrorContext::Unknown));
}

TEST(BackendErrorsTest, OwningDiagnosticDefaultDelegatesExistingNativeStatePolicy) {
  postgres::PgBackendProvider redshift{
      BackendIdentity{"redshift", "Redshift", "ODBCPP Redshift"},
      BackendConnectionDefaults{"localhost", 5439, "dev", true}, std::nullopt,
      postgres::PgCatalogProfile::Redshift};
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),
      "Query error: Cannot insert a NULL value into column id"};
  error.native_state = "XX000";
  EXPECT_FALSE(redshift.normalize_error_sqlstate("XX000", ErrorContext::Unknown));
  EXPECT_FALSE(redshift.IBackendProvider::normalize_error_sqlstate(error, ErrorContext::Unknown));
  for (const auto* state : {"23502", "23505"}) {
    error.native_state = state;
    EXPECT_EQ(std::optional<std::string>{"23000"},
              redshift.normalize_error_sqlstate(error, ErrorContext::Unknown));
    EXPECT_EQ(std::optional<std::string>{"23000"},
              redshift.IBackendProvider::normalize_error_sqlstate(error, ErrorContext::Unknown));
  }
  error.native_state = "42P07";
  EXPECT_EQ(std::optional<std::string>{"42S11"},
            redshift.IBackendProvider::normalize_error_sqlstate(error, ErrorContext::CreateIndex));
  EXPECT_FALSE(redshift.normalize_error_sqlstate(error, ErrorContext::Unknown));
}

TEST(BinaryParameterContractTest, PlainHexRoundTripsAllOctetsAndRejectsNativeEscapes) {
  std::string raw;
  for (int i = 0; i < 256; ++i) raw.push_back(static_cast<char>(i));
  EXPECT_EQ(std::optional<std::string>(raw), rs::util::decode_hex(rs::util::encode_hex(raw)));
  EXPECT_EQ(std::optional<std::string>(""), rs::util::decode_hex(""));
  EXPECT_EQ(std::optional<std::string>(std::string("\0\xab\xff", 3)), rs::util::decode_hex("00aBFF"));
  for (const auto invalid : {"0", "gg", "\\x00", "\\000", " 00", "0 "}) {
    EXPECT_FALSE(rs::util::decode_hex(invalid));
  }
}

TEST(BackendValueTest, PostgresNormalizesHexLegacyAndBooleanWithoutIo) {
  postgres::PgDatabaseConnection backend{};
  const auto binary = [&](std::string_view text) {
    return backend.normalize_result_value(ScalarType::Binary, text);
  };
  EXPECT_EQ(std::optional<std::string>(std::string("\0\1\x7f\xff", 4)), binary("\\x00017fFF"));
  EXPECT_EQ(std::optional<std::string>(std::string("A\\B\0", 4)), binary("A\\\\B\\000"));
  EXPECT_EQ(std::optional<std::string>(""), binary("\\x"));
  EXPECT_EQ(std::optional<std::string>(""), binary(""));
  for (const auto invalid : {"\\x123", "\\xzz", "\\", "\\12", "\\400", "\\08a"}) {
    EXPECT_FALSE(binary(invalid));
  }
  for (const auto text : {"t", "true", "1"})
    EXPECT_EQ(std::optional<std::string>("1"), backend.normalize_result_value(ScalarType::Boolean, text));
  for (const auto text : {"f", "false", "0"})
    EXPECT_EQ(std::optional<std::string>("0"), backend.normalize_result_value(ScalarType::Boolean, text));
  for (const auto text : {"", "yes", "2", "TRUE", " t"})
    EXPECT_FALSE(backend.normalize_result_value(ScalarType::Boolean, text));
  EXPECT_EQ(std::optional<std::string>("\\x00"), backend.normalize_result_value(ScalarType::VarChar, "\\x00"));
  std::string all_bytes, legacy;
  for (int i = 0; i < 256; ++i) {
    all_bytes.push_back(static_cast<char>(i));
    legacy.push_back('\\');
    legacy.push_back(static_cast<char>('0' + i / 64));
    legacy.push_back(static_cast<char>('0' + (i / 8) % 8));
    legacy.push_back(static_cast<char>('0' + i % 8));
  }
  EXPECT_EQ(std::optional<std::string>(all_bytes), binary(legacy));
  EXPECT_FALSE(backend.is_connected());
}

TEST(BackendValueTest, GenericBackendUsesNormalizedBytesAndStrictBoolean) {
  GenericDatabaseConnection backend(std::make_unique<odbcpp::test::MockProtocolParser>());
  EXPECT_EQ(std::optional<std::string>("\\x00"), backend.normalize_result_value(ScalarType::Binary, "\\x00"));
  EXPECT_EQ(std::optional<std::string>(std::string("\0\xff", 2)),
            backend.normalize_result_value(ScalarType::Binary, std::string("\0\xff", 2)));
  EXPECT_EQ(std::optional<std::string>("0"), backend.normalize_result_value(ScalarType::Boolean, "0"));
  EXPECT_FALSE(backend.normalize_result_value(ScalarType::Boolean, "f"));
}

TEST(NativeTypeLookupTest, InvalidDrainedMetadataPreservesPassiveStateAndOwnsError) {
  const std::uint32_t ids[]{90000};
  for (const auto state : {SessionState::Disconnected, SessionState::Idle, SessionState::Transaction,
                           SessionState::FailedTransaction, SessionState::Unknown}) {
    SCOPED_TRACE(static_cast<int>(state));
    std::optional<BackendError> saved;
    {
      MetadataLookupConnection backend;
      backend.state = state;
      backend.response.rows = {{"90000", "0", "-1"}};
      auto failed = backend.resolve_types(ids, rs::util::Deadline::max());
      ASSERT_TRUE(failed.has_error());
      saved = failed.backend_error();
      EXPECT_EQ(saved->session_state, state);
      EXPECT_EQ(saved->disposition, state == SessionState::Disconnected ? SessionDisposition::Retire :
          state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired);
      EXPECT_FALSE(saved->native_code);
      EXPECT_FALSE(saved->native_state);
      EXPECT_FALSE(saved->retry_safe);
      auto moved = std::move(failed);
      EXPECT_EQ(moved.error_message(), saved->message);
      backend.response.rows = {{"90000", "23", "-1"}};
      auto recovered = backend.resolve_types(ids, rs::util::Deadline::max());
      ASSERT_TRUE(recovered);
      EXPECT_EQ(recovered->at(90000).type, ScalarType::Integer);
      EXPECT_EQ(backend.state, state);
    }
    EXPECT_EQ(saved->operation, BackendOperation::ResolveTypes);
    EXPECT_EQ(saved->error_class, BackendErrorClass::InvalidMetadata);
    EXPECT_EQ(saved->message, "Data source returned invalid parameter type metadata");
  }
}

TEST(TypeCatalogTest, ProviderPolicyDoesNotRetainVersionInputOrInvalidateProfiles) {
  using namespace rs::core::database;
  postgres::PgBackendProvider provider{
      BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"localhost", 5432, "postgres", true}};
  std::string version = "17.11";
  const auto modern = provider.type_catalog(version);
  version.assign("14.18");
  const auto legacy = provider.type_catalog(version);
  const auto fallback = provider.type_catalog("999999999999999999999999");
  ASSERT_EQ(legacy.size(), modern.size()); ASSERT_EQ(legacy.size(), fallback.size());
  for (std::size_t index = 0; index < legacy.size(); ++index) {
    EXPECT_EQ(legacy[index].name, modern[index].name);
    EXPECT_EQ(legacy[index].name, fallback[index].name);
    if (legacy[index].type == ScalarType::Numeric || legacy[index].type == ScalarType::Decimal) {
      EXPECT_EQ(std::optional<std::int16_t>(-1000), modern[index].minimum_scale);
      EXPECT_EQ(std::optional<std::int16_t>(0), legacy[index].minimum_scale);
      EXPECT_EQ(legacy[index].minimum_scale, fallback[index].minimum_scale);
    }
  }
  EXPECT_EQ(legacy.data(), provider.type_catalog().data());
  EXPECT_EQ(modern.data(), provider.type_catalog("15.0").data());
}

TEST(NativeTypeTest, RedshiftVarbyteResultFamilyKeepsPostgresOidSemanticsIsolated) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection pg;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  EXPECT_FALSE(pg.describe_type(6551, -1, -1).known);
  EXPECT_EQ(ScalarType::VarChar, pg.describe_type(6551, -1, -1).type);
  for (const auto modifier : {-1, 0, 1028}) {
    const auto native = redshift.describe_type(6551, -1, modifier);
    EXPECT_TRUE(native.known);
    EXPECT_EQ(ScalarType::LongVarBinary, native.type);
    EXPECT_EQ(0u, native.column_size); // Unknown, not an invented maximum.
  }
  EXPECT_EQ(ScalarType::Binary, redshift.describe_type(17, -1, -1).type);
  EXPECT_FALSE(redshift.describe_type(999999, -1, -1).known);
  EXPECT_FALSE(pg.is_connected()); EXPECT_FALSE(redshift.is_connected());
}

TEST(BackendValueTest, RedshiftVarbyteDecodesOnlyStrictPlainHexAndPreservesBytea) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection pg;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  EXPECT_EQ(std::optional<std::string>(std::string("\0\1\x7f\xff", 4)),
      redshift.normalize_result_value(ScalarType::LongVarBinary, "00017fFF"));
  EXPECT_EQ(std::optional<std::string>(""),
      redshift.normalize_result_value(ScalarType::LongVarBinary, ""));
  for (const auto* invalid : {"a", "xyz", "ab ", " ab", "\\xab", "\\377"})
    EXPECT_FALSE(redshift.normalize_result_value(ScalarType::LongVarBinary, invalid));
  EXPECT_EQ(std::optional<std::string>("ab"), pg.normalize_result_value(ScalarType::Binary, "ab"));
  EXPECT_EQ(std::optional<std::string>(std::string("\xab", 1)),
      redshift.normalize_result_value(ScalarType::LongVarBinary, "ab"));
  EXPECT_FALSE(pg.normalize_result_value(ScalarType::LongVarBinary, "ab"));
  EXPECT_EQ(pg.normalize_result_value(ScalarType::Binary, "\\x00ff"),
      redshift.normalize_result_value(ScalarType::Binary, "\\x00ff"));
}

TEST(NativeTypeTest, RedshiftVarbyteParameterMetadataFailsLocallyWithoutDiscoverySql) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  std::uint32_t ids[]{23, 6551, 17, 6551};
  // Known metadata is pure: neither a transport nor an unexpired deadline is needed.
  const auto result = redshift.resolve_types(ids, rs::util::Deadline{});
  ASSERT_TRUE(result);
  ASSERT_EQ(3u, result->size());
  EXPECT_EQ(ScalarType::Integer, result->at(23).type);
  EXPECT_EQ(10u, result->at(23).column_size);
  EXPECT_EQ(ScalarType::Binary, result->at(17).type);
  const auto& native = result->at(6551);
  EXPECT_TRUE(native.known);
  EXPECT_EQ(ScalarType::LongVarBinary, native.type);
  EXPECT_EQ(0u, native.column_size); // Unknown dimension, not an invented maximum.
  EXPECT_EQ(0, native.decimal_digits);
  EXPECT_FALSE(native.declared_type);
  EXPECT_FALSE(native.declared_column_size);
  ids[1] = 999999; // The returned map owns its keys and normalized descriptions.
  EXPECT_EQ(1u, result->count(6551));
  EXPECT_EQ(0u, result->count(999999));
  const std::uint32_t retry_ids[]{6551};
  const auto retry = redshift.resolve_types(retry_ids, rs::util::Deadline{});
  ASSERT_TRUE(retry);
  EXPECT_EQ(ScalarType::LongVarBinary, retry->at(6551).type);
  EXPECT_FALSE(redshift.is_connected());
}

TEST(NativeTypeTest, RedshiftSuperIsNamedLongVarcharOnlyInRedshiftProfile) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection pg;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  EXPECT_FALSE(pg.describe_type(4000, -1, -1).known);
  EXPECT_EQ(ScalarType::VarChar, pg.describe_type(4000, -1, -1).type);
  EXPECT_FALSE(pg.describe_type(4000, -1, -1).type_name);
  for (const auto modifier : {-1, 0, 1028}) {
    const auto native = redshift.describe_type(4000, -1, modifier);
    EXPECT_TRUE(native.known);
    EXPECT_EQ(ScalarType::LongVarChar, native.type);
    EXPECT_EQ(0u, native.column_size); // Unknown, not the 16 MB object bound.
    EXPECT_EQ(0, native.decimal_digits);
    EXPECT_EQ(std::optional<std::string>("super"), native.type_name);
    EXPECT_FALSE(native.declared_type);
  }
  // SUPER text is preserved byte for byte; no JSON reserialization.
  EXPECT_EQ(std::optional<std::string>(R"({"a" : 1})"),
      redshift.normalize_result_value(ScalarType::LongVarChar, R"({"a" : 1})"));
  const std::uint32_t ids[]{4000};
  const auto resolved = redshift.resolve_types(ids, rs::util::Deadline{});
  ASSERT_TRUE(resolved); // Known metadata needs no transport or discovery SQL.
  EXPECT_EQ(ScalarType::LongVarChar, resolved->at(4000).type);
  EXPECT_EQ(std::optional<std::string>("super"), resolved->at(4000).type_name);
  EXPECT_FALSE(redshift.is_connected());
}

TEST(NativeTypeTest, CharacterPolicyLeavesSuperPresentationUntouched) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  const auto native = apply_character_metadata_policy(
      redshift.describe_type(4000, -1, -1), CharacterMetadataPolicy{255, 65535, true});
  EXPECT_EQ(ScalarType::LongVarChar, native.type);
  EXPECT_EQ(0u, native.column_size);
  EXPECT_FALSE(native.declared_type);
  EXPECT_EQ(std::optional<std::string>("super"), native.type_name);
}
