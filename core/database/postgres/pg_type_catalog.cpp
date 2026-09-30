#include "pg_database_connection.h"
#include "pg_backend_provider.h"

#include <array>
#include <charconv>

namespace rs::core::database::postgres {
namespace {
using enum ScalarType;
constexpr auto definitions = std::to_array<TypeDefinition>({
    {Boolean, "boolean", 1, {}, {}, {}, false, {}, {}, {}, 0},
    {BigInt, "bigint", 19, {}, {}, {}, false, false, 0, 0, 10},
    {Binary, "bytea", 1073741824, "'", "'", {}, false, {}, {}, {}, 0},
    {LongVarChar, "text", 1073741824, "'", "'", {}, true, {}, {}, {}, 0},
    {Char, "char", 10485760, "'", "'", "length", true, {}, {}, {}, 0},
    {Numeric, "numeric", 1000, {}, {}, "precision,scale", false, false, 0, 1000, 10},
    {Decimal, "decimal", 1000, {}, {}, "precision,scale", false, false, 0, 1000, 10},
    {Integer, "integer", 10, {}, {}, {}, false, false, 0, 0, 10},
    {SmallInt, "smallint", 5, {}, {}, {}, false, false, 0, 0, 10},
    {Real, "real", 7, {}, {}, {}, false, false, {}, {}, 2},
    {Double, "double precision", 15, {}, {}, {}, false, false, {}, {}, 2},
    {Date, "date", 10, "'", "'", {}, false, {}, {}, {}, 0},
    {Time, "time", 15, "'", "'", "precision", false, {}, 0, 6, 0},
    {Timestamp, "timestamp", 26, "'", "'", "precision", false, {}, 0, 6, 0},
    {VarChar, "varchar", 10485760, "'", "'", "length", true, {}, {}, {}, 0},
});
constexpr auto modern_definitions = [] {
  auto result = definitions;
  for (auto& type : result) {
    if (type.type == Numeric || type.type == Decimal) type.minimum_scale = -1000;
  }
  return result;
}();
} // namespace

std::span<const TypeDefinition> pg_type_catalog(
    std::string_view version) noexcept {
  if (version.empty()) return definitions;
  int major = 0;
  const auto parsed = std::from_chars(version.data(), version.data() + version.size(), major);
  return parsed.ec == std::errc{} && major >= 15
      ? std::span<const TypeDefinition>(modern_definitions)
      : std::span<const TypeDefinition>(definitions);
}

std::span<const TypeDefinition> PgDatabaseConnection::type_catalog() const {
  return pg_type_catalog(get_parameter("server_version"));
}

} // namespace rs::core::database::postgres
