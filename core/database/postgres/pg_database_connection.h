#pragma once

#include "core/database/generic_database_connection.h"

namespace rs::core::database::postgres {

// PostgreSQL session with backend-specific metadata discovery.
class PgDatabaseConnection : public GenericDatabaseConnection {
public:
  explicit PgDatabaseConnection(
      std::unique_ptr<rs::core::transport::ITransport> transport = nullptr);

  rs::util::Result<std::string> catalog_query(
      const CatalogRequest& request) const override;

  std::span<const TypeDefinition> type_catalog() const override;

  TransactionCapabilities transaction_capabilities() const override;
  rs::util::Result<void> transaction(TransactionAction action,
      rs::util::Deadline deadline) override;
  rs::util::Result<void> set_transaction_isolation(TransactionIsolation level,
      rs::util::Deadline deadline) override;

  rs::util::Result<ResolvedTypeMap> resolve_types(
      std::span<const std::uint32_t> ids, rs::util::Deadline deadline) override;
};

} // namespace rs::core::database::postgres
