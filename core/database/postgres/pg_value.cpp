#include "pg_database_connection.h"
#include "pg_value.h"

namespace rs::core::database::postgres {
NativeTypeInfo PgDatabaseConnection::describe_type(
    std::uint32_t id, std::int16_t size, std::int32_t modifier) const {
  if (catalog_profile_ == PgCatalogProfile::Redshift && id == 6551) {
    // The result family is evidenced; wire size/modifier semantics are not.
    return {ScalarType::LongVarBinary, 0, 0, true};
  }
  if (catalog_profile_ == PgCatalogProfile::Redshift && id == 4000) {
    // SUPER: server-serialized JSON text. The 16 MB object limit is not a
    // per-column character length, so the dimension stays unknown.
    NativeTypeInfo super{ScalarType::LongVarChar, 0, 0, true};
    super.type_name = "super";
    return super;
  }
  if (catalog_profile_ == PgCatalogProfile::Redshift &&
      (id == 3000 || id == 3001 || id == 3999)) {
    // GEOMETRY (3000 raw, 3999 hex) and GEOGRAPHY (3001 hex) are EWKB values.
    // The documented 1,048,447-byte object limit is not a column dimension.
    NativeTypeInfo spatial{ScalarType::LongVarBinary, 0, 0, true};
    spatial.type_name = id == 3001 ? "geography" : "geometry";
    return spatial;
  }
  return GenericDatabaseConnection::describe_type(id, size, modifier);
}

std::optional<std::string> PgDatabaseConnection::normalize_native_result_value(
    std::uint32_t id, ScalarType type, std::string_view value) const {
  if (catalog_profile_ == PgCatalogProfile::Redshift && id == 3000 &&
      type == ScalarType::LongVarBinary) {
    // Raw EWKB in a text-format cell (AWS ODBC rsutil.c, AWS JDBC getBytes).
    // A WKB value starts with its byte-order flag; anything else, including
    // hexadecimal text, is a cell error rather than silently returned bytes.
    if (!value.empty() && value.front() != '\0' && value.front() != '\1') return std::nullopt;
    return std::string(value);
  }
  return normalize_result_value(type, value);
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
