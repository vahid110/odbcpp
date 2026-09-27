#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rs::core::database {

struct TablesCatalogRequest {
  enum class Mode { Tables, Catalogs, Schemas, TableTypes };
  Mode mode{Mode::Tables};
  // Omitted names are unrestricted; present empty strings remain filters.
  // Table discovery uses LIKE patterns; key discovery uses literal names.
  std::optional<std::string> catalog, schema, table;
  // Omitted means all types; an empty list deliberately matches nothing.
  std::optional<std::vector<std::string>> types;
};
struct PrimaryKeysCatalogRequest {
  std::optional<std::string> catalog, schema;
  std::string table;
};
struct ForeignKeysCatalogRequest {
  std::optional<std::string> primary_catalog, primary_schema, primary_table;
  std::optional<std::string> foreign_catalog, foreign_schema, foreign_table;
};
using CatalogRequest = std::variant<TablesCatalogRequest,
    PrimaryKeysCatalogRequest, ForeignKeysCatalogRequest>;

} // namespace rs::core::database
