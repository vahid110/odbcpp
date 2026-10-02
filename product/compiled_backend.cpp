#include "core/database/backend_provider.h"
#include "core/database/database_factory.h"

#include <stdexcept>

#if defined(ODBCPP_ENABLE_POSTGRESQL) || defined(ODBCPP_ENABLE_REDSHIFT)
#include "core/database/postgres/pg_backend_provider.h"
#endif

namespace rs::core::database {

const IBackendProvider& configured_backend_provider() {
#if defined(ODBCPP_ENABLE_POSTGRESQL)
  static const postgres::PgBackendProvider provider{
      BackendIdentity{"postgresql", "PostgreSQL", "ODBCPP PostgreSQL"},
      BackendConnectionDefaults{"localhost", 5432, "postgres", true},
      SessionResetProfile::SameAuthenticatedServerSession};
  return provider;
#elif defined(ODBCPP_ENABLE_REDSHIFT)
  static const postgres::PgBackendProvider provider{
      BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      // Preserve the existing PostgreSQL-shaped defaults until a real Redshift
      // pilot establishes a supported product profile.
      BackendConnectionDefaults{"localhost", 5432, "postgres", true}, std::nullopt,
      postgres::PgCatalogProfile::Redshift};
  return provider;
#else
#error "No implemented database provider selected"
#endif
}

std::unique_ptr<IDatabaseConnection> DatabaseFactory::create_connection(
    DatabaseType type) {
  return create_connection(type, nullptr);
}

std::unique_ptr<IDatabaseConnection> DatabaseFactory::create_connection(
    DatabaseType type,
    std::unique_ptr<rs::core::transport::ITransport> transport) {
  const auto& provider = configured_backend_provider();
  if (type != get_compiled_database_type()) {
    throw std::runtime_error("Database type not supported in this build");
  }
  return provider.create_session(std::move(transport));
}

std::unique_ptr<IDatabaseConnection> DatabaseFactory::create_connection() {
  return create_connection(get_compiled_database_type(), nullptr);
}

std::unique_ptr<IDatabaseConnection> DatabaseFactory::create_connection(
    std::unique_ptr<rs::core::transport::ITransport> transport) {
  return create_connection(get_compiled_database_type(), std::move(transport));
}

DatabaseType DatabaseFactory::get_compiled_database_type() {
#if defined(ODBCPP_ENABLE_POSTGRESQL)
  return DatabaseType::PostgreSQL;
#elif defined(ODBCPP_ENABLE_REDSHIFT)
  return DatabaseType::Redshift;
#else
#error "No implemented database provider selected"
#endif
}

}  // namespace rs::core::database
