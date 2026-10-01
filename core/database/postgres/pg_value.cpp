#include "pg_database_connection.h"
#include "pg_value.h"

namespace rs::core::database::postgres {
std::optional<std::string> PgDatabaseConnection::normalize_result_value(
    ScalarType type, std::string_view value) const {
  return normalize_pg_result_value(type, value);
}

} // namespace rs::core::database::postgres
