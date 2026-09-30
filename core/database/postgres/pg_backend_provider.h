#pragma once

#include "core/database/backend_provider.h"

namespace rs::core::database::postgres {

BackendCapabilities pg_backend_capabilities(std::string_view display_name) noexcept;
std::span<const TypeDefinition> pg_type_catalog(std::string_view server_version) noexcept;
TransactionCapabilities pg_transaction_capabilities() noexcept;

class PgBackendProvider final : public IBackendProvider {
 public:
  PgBackendProvider(BackendIdentity identity,
                    BackendConnectionDefaults connection_defaults);
  PgBackendProvider(const PgBackendProvider&) = delete;
  PgBackendProvider& operator=(const PgBackendProvider&) = delete;
  PgBackendProvider(PgBackendProvider&&) = delete;
  PgBackendProvider& operator=(PgBackendProvider&&) = delete;

  const BackendIdentity& identity() const noexcept override { return identity_; }
  const BackendConnectionDefaults& connection_defaults() const noexcept override {
    return connection_defaults_;
  }
  BackendCapabilities capabilities() const noexcept override { return capabilities_; }
  std::span<const TypeDefinition> type_catalog() const noexcept override;
  TransactionCapabilities transaction_capabilities() const noexcept override;
  rs::util::Result<ConnectionSettings> resolve_connection_options(
      ConnectionOptions options) const override;
  std::unique_ptr<IDatabaseConnection> create_session(
      std::unique_ptr<rs::core::transport::ITransport> transport) const override;

 private:
  BackendIdentity identity_;
  BackendConnectionDefaults connection_defaults_;
  BackendCapabilities capabilities_;
};

}  // namespace rs::core::database::postgres
