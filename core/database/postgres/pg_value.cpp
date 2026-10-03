#include "pg_database_connection.h"
#include "pg_value.h"

namespace rs::core::database::postgres {
NativeTypeInfo PgDatabaseConnection::describe_type(
    std::uint32_t id, std::int16_t size, std::int32_t modifier) const {
  if (catalog_profile_ == PgCatalogProfile::Redshift && id == 6551) {
    // The result family is evidenced; wire size/modifier semantics are not.
    return {ScalarType::LongVarBinary, 0, 0, true};
  }
  return GenericDatabaseConnection::describe_type(id, size, modifier);
}

std::optional<std::string> PgDatabaseConnection::normalize_result_value(
    ScalarType type, std::string_view value) const {
  if (type == ScalarType::LongVarBinary) {
    if (catalog_profile_ != PgCatalogProfile::Redshift) return std::nullopt;
    return rs::util::decode_hex(value);
  }
  return normalize_pg_result_value(type, value);
}

} // namespace rs::core::database::postgres
