#include "pg_backend_provider.h"

#include "pg_database_connection.h"
#include "pg_protocol_parser.h"
#include "pg_sql_dialect.h"

#include <string_view>
#include <utility>

namespace rs::core::database::postgres {
namespace {
class PgSqlDialect final : public ISqlDialect {
 public:
  std::size_t count_parameter_markers(std::string_view sql) const override {
    return PgProtocolParser::parameter_marker_count(sql);
  }
  SqlTranslationResult translate_sql(std::string_view sql) const override {
    return translate_odbc_sql(sql);
  }
};
} // namespace

const ISqlDialect& PgBackendProvider::sql_dialect() const noexcept {
  static const PgSqlDialect dialect;
  return dialect;
}


PgBackendProvider::PgBackendProvider(
    BackendIdentity identity, BackendConnectionDefaults connection_defaults,
    std::optional<SessionResetProfile> reset_profile)
    : identity_(std::move(identity)),
      connection_defaults_(std::move(connection_defaults)),
      capabilities_(pg_backend_capabilities(identity_.display_name)), reset_profile_(reset_profile) {}

std::span<const TypeDefinition> PgBackendProvider::type_catalog(std::string_view server_version) const noexcept {
  return pg_type_catalog(server_version);
}

TransactionCapabilities PgBackendProvider::transaction_capabilities() const noexcept {
  return pg_transaction_capabilities();
}

rs::util::Result<ConnectionSettings>
PgBackendProvider::resolve_connection_options(ConnectionOptions options) const {
  const auto no_nul = [](const std::optional<std::string>& value) {
    return !value || value->find('\0') == std::string::npos;
  };
  if (!no_nul(options.host) || !no_nul(options.database) ||
      !no_nul(options.user) || !no_nul(options.password) ||
      !no_nul(options.ssl_ca_file) || !no_nul(options.ssl_ca_dir)) {
    return {rs::util::DbErrorCode::InvalidParameter,
            "Connection option contains an embedded NUL byte"};
  }

  ConnectionSettings settings;
  settings.host = options.host.value_or(connection_defaults_.host);
  settings.port = options.port.value_or(connection_defaults_.port);
  settings.database = options.database.value_or(
      connection_defaults_.database.value_or(std::string{}));
  settings.user = options.user.value_or(std::string{});
  settings.password = options.password.value_or(std::string{});
  settings.use_ssl = options.use_ssl.value_or(connection_defaults_.use_ssl);
  settings.ssl_ca_file = options.ssl_ca_file.value_or(std::string{});
  settings.ssl_ca_dir = options.ssl_ca_dir.value_or(std::string{});
  settings.timeout = options.timeout;
  settings.response_limits = options.response_limits;
  settings.startup_response_limits = options.startup_response_limits;
  settings.result_limits = options.result_limits;
  settings.input_limits = options.input_limits;
  if (!valid_resource_limits(settings)) {
    return {rs::util::DbErrorCode::InvalidParameter, "Resource limits are outside supported bounds"};
  }

  if (!settings.ssl_ca_file.empty() && !settings.ssl_ca_dir.empty()) {
    return {rs::util::DbErrorCode::InvalidParameter,
            "SSLCAFILE and SSLCADIR cannot both be specified"};
  }
  if (!settings.use_ssl &&
      (!settings.ssl_ca_file.empty() || !settings.ssl_ca_dir.empty())) {
    return {rs::util::DbErrorCode::InvalidParameter,
            "SSLCAFILE and SSLCADIR require SSL=true"};
  }
  return settings;
}

std::unique_ptr<IDatabaseConnection> PgBackendProvider::create_session(
    std::unique_ptr<rs::core::transport::ITransport> transport) const {
  return std::make_unique<PgDatabaseConnection>(std::move(transport), reset_profile_);
}

}  // namespace rs::core::database::postgres
