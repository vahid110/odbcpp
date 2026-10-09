#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace rs::core::database {

// Normalized catalog matching intent; lexical API rules and native SQL stay
// with their respective adapters/backends. Existing preserves old requests.
enum class CatalogNameMatch { Existing, IdentifierUnquoted, IdentifierQuoted };
struct CatalogNameMatches {
  CatalogNameMatch catalog{CatalogNameMatch::Existing};
  CatalogNameMatch schema{CatalogNameMatch::Existing};
  CatalogNameMatch object{CatalogNameMatch::Existing};
  CatalogNameMatch member{CatalogNameMatch::Existing};
  // ForeignKeys maps the first three fields to the primary side.
  CatalogNameMatch foreign_catalog{CatalogNameMatch::Existing};
  CatalogNameMatch foreign_schema{CatalogNameMatch::Existing};
  CatalogNameMatch foreign_object{CatalogNameMatch::Existing};
  bool existing() const noexcept {
    return catalog == CatalogNameMatch::Existing && schema == CatalogNameMatch::Existing &&
        object == CatalogNameMatch::Existing && member == CatalogNameMatch::Existing &&
        foreign_catalog == CatalogNameMatch::Existing && foreign_schema == CatalogNameMatch::Existing &&
        foreign_object == CatalogNameMatch::Existing;
  }
};

struct TablesCatalogRequest {
  enum class Mode { Tables, Catalogs, Schemas, TableTypes };
  Mode mode{Mode::Tables};
  // Omitted names are unrestricted; present empty strings remain filters.
  // Table discovery uses LIKE patterns; key discovery uses literal names.
  std::optional<std::string> catalog, schema, table;
  // Omitted means all types; an empty list deliberately matches nothing.
  std::optional<std::vector<std::string>> types;
  CatalogNameMatches name_matches{};
};
struct PrimaryKeysCatalogRequest {
  std::optional<std::string> catalog, schema;
  std::string table;
  CatalogNameMatches name_matches{};
};
struct ForeignKeysCatalogRequest {
  std::optional<std::string> primary_catalog, primary_schema, primary_table;
  std::optional<std::string> foreign_catalog, foreign_schema, foreign_table;
  CatalogNameMatches name_matches{};
};
struct ColumnsCatalogRequest {
  // Catalog is literal; schema, table and column are LIKE patterns.
  std::optional<std::string> catalog, schema, table, column;
  CatalogNameMatches name_matches{};
};
struct StatisticsCatalogRequest {
  std::optional<std::string> catalog, schema;
  std::string table;
  bool unique_only{false};
  CatalogNameMatches name_matches{};
};
struct ProceduresCatalogRequest {
  // Catalog is literal; schema and procedure are LIKE patterns.
  std::optional<std::string> catalog, schema, procedure;
  CatalogNameMatches name_matches{};
};
struct ProcedureColumnsCatalogRequest {
  std::optional<std::string> catalog, schema, procedure, column;
  CatalogNameMatches name_matches{};
};
struct SpecialColumnsCatalogRequest {
  enum class Identifier { RowIdentity, RowVersion };
  enum class Scope { CurrentRow, Transaction, Session };
  Identifier identifier{Identifier::RowIdentity};
  Scope scope{Scope::CurrentRow};
  std::optional<std::string> catalog, schema;
  std::string table;
  bool require_non_nullable{false};
  CatalogNameMatches name_matches{};
};
using CatalogRequest = std::variant<TablesCatalogRequest,
    PrimaryKeysCatalogRequest, ForeignKeysCatalogRequest, ColumnsCatalogRequest,
    StatisticsCatalogRequest, ProceduresCatalogRequest,
    ProcedureColumnsCatalogRequest, SpecialColumnsCatalogRequest>;

} // namespace rs::core::database
