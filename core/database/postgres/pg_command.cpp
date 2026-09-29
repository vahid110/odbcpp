#include "pg_command.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace rs::core::database::postgres {
StatementKind classify_command_tag(std::string_view tag) {
  const auto first = tag.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return StatementKind::Unknown;
  std::string normalized(tag.substr(first));
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  struct Mapping { std::string_view prefix; StatementKind kind; };
  static constexpr std::array<Mapping, 31> mappings{{
      {"SELECT", StatementKind::SelectCursor},
      {"INSERT", StatementKind::Insert},
      {"UPDATE", StatementKind::UpdateWhere},
      {"DELETE", StatementKind::DeleteWhere},
      {"CALL", StatementKind::Call},
      {"GRANT", StatementKind::Grant},
      {"REVOKE", StatementKind::Revoke},
      {"ALTER DOMAIN", StatementKind::AlterDomain},
      {"ALTER TABLE", StatementKind::AlterTable},
      {"CREATE ASSERTION", StatementKind::CreateAssertion},
      {"CREATE CHARACTER SET", StatementKind::CreateCharacterSet},
      {"CREATE COLLATION", StatementKind::CreateCollation},
      {"CREATE DOMAIN", StatementKind::CreateDomain},
      {"CREATE INDEX", StatementKind::CreateIndex},
      {"CREATE UNIQUE INDEX", StatementKind::CreateIndex},
      {"CREATE SCHEMA", StatementKind::CreateSchema},
      {"CREATE TABLE", StatementKind::CreateTable},
      {"CREATE TEMP TABLE", StatementKind::CreateTable},
      {"CREATE TEMPORARY TABLE", StatementKind::CreateTable},
      {"CREATE UNLOGGED TABLE", StatementKind::CreateTable},
      {"CREATE TRANSLATION", StatementKind::CreateTranslation},
      {"CREATE VIEW", StatementKind::CreateView},
      {"DROP ASSERTION", StatementKind::DropAssertion},
      {"DROP CHARACTER SET", StatementKind::DropCharacterSet},
      {"DROP COLLATION", StatementKind::DropCollation},
      {"DROP DOMAIN", StatementKind::DropDomain},
      {"DROP INDEX", StatementKind::DropIndex},
      {"DROP SCHEMA", StatementKind::DropSchema},
      {"DROP TABLE", StatementKind::DropTable},
      {"DROP TRANSLATION", StatementKind::DropTranslation},
      {"DROP VIEW", StatementKind::DropView},
  }};
  for (const auto& item : mappings) {
    if (normalized.starts_with(item.prefix) &&
        (normalized.size() == item.prefix.size() ||
         std::isspace(static_cast<unsigned char>(normalized[item.prefix.size()])))) {
      return item.kind;
    }
  }
  return StatementKind::Unknown;
}
} // namespace rs::core::database::postgres
