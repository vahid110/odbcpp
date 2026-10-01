#pragma once

#include "core/database/generic_database_connection.h"

#include <string>
#include <utility>

namespace rs::core::database::postgres {

// PostgreSQL session with backend-specific metadata discovery.
class PgDatabaseConnection : public GenericDatabaseConnection, public ITransactionSession, public ICatalogQueries, public ISessionHealth {
public:
  explicit PgDatabaseConnection(
      std::unique_ptr<rs::core::transport::ITransport> transport = nullptr);

  ISessionHealth* session_health() noexcept override { return this; }
  BackendResult<void> check_health(rs::util::Deadline deadline) override;

  const ICatalogQueries* catalog_queries() const noexcept override { return this; }
  rs::util::Result<std::string> catalog_query(
      const CatalogRequest& request) const override;

  std::optional<std::string> normalize_result_value(
      ScalarType type, std::string_view value) const override;

  ITransactionSession* transaction_session() noexcept override { return this; }
  TransactionCapabilities transaction_capabilities() const override;
  BackendResult<void> transaction(TransactionAction action,
      rs::util::Deadline deadline) override;
  BackendResult<void> set_transaction_isolation(TransactionIsolation level,
      rs::util::Deadline deadline) override;

  BackendResult<ResolvedTypeMap> resolve_types(
      std::span<const std::uint32_t> ids, rs::util::Deadline deadline) override;

};

} // namespace rs::core::database::postgres
