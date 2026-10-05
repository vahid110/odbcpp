#pragma once

namespace rs::core::database {

// Backend-normalized completed operation; no native tags or ODBC constants.
enum class StatementKind {
  Unknown,
  SelectCursor,
  Insert,
  UpdateWhere,
  DeleteWhere,
  Call,
  Grant,
  Revoke,
  AlterDomain,
  AlterTable,
  CreateAssertion,
  CreateCharacterSet,
  CreateCollation,
  CreateDomain,
  CreateIndex,
  CreateSchema,
  CreateTable,
  CreateTranslation,
  CreateView,
  DropAssertion,
  DropCharacterSet,
  DropCollation,
  DropDomain,
  DropIndex,
  DropSchema,
  DropTable,
  DropTranslation,
  DropView,
};

} // namespace rs::core::database
