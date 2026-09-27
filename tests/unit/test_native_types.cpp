#include <gtest/gtest.h>

#include "core/database/database_factory.h"
#include "core/database/generic_database_connection.h"
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
