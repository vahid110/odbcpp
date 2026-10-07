#pragma once

#include "pg_catalog_profile.h"

#include "core/database/generic_database_connection.h"

#include <string>
#include <utility>

namespace rs::core::database::postgres {
namespace detail { struct StagedPreparedRefusalTestAccess; }

// PostgreSQL session with backend-specific metadata discovery.
class PgDatabaseConnection : public GenericDatabaseConnection, public ITransactionSession, public ICatalogQueries, public ISessionHealth, public ISessionReset, public ICatalogExecution {
public:
  explicit PgDatabaseConnection(
      std::unique_ptr<rs::core::transport::ITransport> transport = nullptr,
      std::optional<SessionResetProfile> reset_profile = std::nullopt,
      PgCatalogProfile catalog_profile = PgCatalogProfile::PostgreSQL);

  BackendResult<void> connect(const ConnectionSettings& settings) override;
  BackendResult<void> connect_until(const ConnectionSettings& settings, rs::util::Deadline deadline);

  ISessionReset* session_reset() noexcept override { return reset_profile_ ? this : nullptr; }
  SessionResetProfile reset_profile() const noexcept override {
    return SessionResetProfile::SameAuthenticatedServerSession;
  }
  BackendResult<void> reset_session(rs::util::Deadline deadline) override;

  ISessionHealth* session_health() noexcept override { return this; }
  BackendResult<void> check_health(rs::util::Deadline deadline) override;

  const ICatalogQueries* catalog_queries() const noexcept override { return this; }
  rs::util::Result<std::string> catalog_query(
      const CatalogRequest& request) const override;

  ICatalogExecution* catalog_execution() noexcept override {
    return catalog_profile_ == PgCatalogProfile::Redshift ? this : nullptr;
  }
  bool selects_catalog_request(const CatalogRequest& request) const noexcept override;
  BackendResult<QueryResult> execute_catalog(
      const CatalogRequest& request, rs::util::Deadline deadline) override;

  std::optional<std::string> normalize_result_value(
      ScalarType type, std::string_view value) const override;
  NativeTypeInfo describe_type(std::uint32_t id, std::int16_t size,
                              std::int32_t modifier) const override;

  ITransactionSession* transaction_session() noexcept override { return this; }
  TransactionCapabilities transaction_capabilities() const override;
  BackendResult<void> transaction(TransactionAction action,
      rs::util::Deadline deadline) override;
  BackendResult<void> set_transaction_isolation(TransactionIsolation level,
      rs::util::Deadline deadline) override;

  BackendResult<ResolvedTypeMap> resolve_types(
      std::span<const std::uint32_t> ids, rs::util::Deadline deadline) override;

protected:
  virtual RedshiftCatalogMode catalog_mode() const noexcept { return catalog_mode_; }

private:
  friend struct detail::StagedPreparedRefusalTestAccess;
  BackendResult<QueryResult> observe_and_decline_binary_prepared_for_test(
      std::string_view sql, std::span<const QueryParameter> params, rs::util::Deadline deadline);
  RedshiftCatalogMode catalog_mode_{RedshiftCatalogMode::Show};
  const std::optional<SessionResetProfile> reset_profile_;
  const PgCatalogProfile catalog_profile_;
};

} // namespace rs::core::database::postgres
