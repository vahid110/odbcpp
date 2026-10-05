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
struct ColumnsCatalogRequest {
  // Catalog is literal; schema, table and column are LIKE patterns.
  std::optional<std::string> catalog, schema, table, column;
};
struct StatisticsCatalogRequest {
  std::optional<std::string> catalog, schema;
  std::string table;
  bool unique_only{false};
};
struct ProceduresCatalogRequest {
  // Catalog is literal; schema and procedure are LIKE patterns.
  std::optional<std::string> catalog, schema, procedure;
};
struct ProcedureColumnsCatalogRequest {
  std::optional<std::string> catalog, schema, procedure, column;
};
struct SpecialColumnsCatalogRequest {
  enum class Identifier { RowIdentity, RowVersion };
  enum class Scope { CurrentRow, Transaction, Session };
  Identifier identifier{Identifier::RowIdentity};
  Scope scope{Scope::CurrentRow};
  std::optional<std::string> catalog, schema;
  std::string table;
  bool require_non_nullable{false};
};
using CatalogRequest = std::variant<TablesCatalogRequest,
    PrimaryKeysCatalogRequest, ForeignKeysCatalogRequest, ColumnsCatalogRequest,
    StatisticsCatalogRequest, ProceduresCatalogRequest,
    ProcedureColumnsCatalogRequest, SpecialColumnsCatalogRequest>;

} // namespace rs::core::database
