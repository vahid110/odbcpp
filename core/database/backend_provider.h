#pragma once

#include "backend_capabilities.h"
#include "sql_dialect.h"
#include "i_database_connection.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace rs::core::transport {
class ITransport;
}

namespace rs::core::database {

struct BackendIdentity {
  std::string id;
  std::string display_name;
  std::string driver_name;
};

struct BackendConnectionDefaults {
  std::string host;
  std::uint16_t port;
  std::optional<std::string> database;
  bool use_ssl;
};

struct ConnectionOptions {
  std::optional<std::string> host;
  std::optional<std::uint16_t> port;
  std::optional<std::string> database;
  std::optional<std::string> user;
  std::optional<std::string> password;
  std::optional<bool> use_ssl;
  std::optional<std::string> ssl_ca_file;
  std::optional<std::string> ssl_ca_dir;
  std::chrono::milliseconds timeout{15000};
  ResponseLimits response_limits;
  ResponseLimits startup_response_limits{1024 * 1024, 10000};
  ResultLimits result_limits;
  InputLimits input_limits;
};

// Immutable product definition. A provider owns identity and static behavior;
// each create_session call returns one independent live protocol session.
class IBackendProvider {
 public:
  virtual ~IBackendProvider() = default;

  virtual const BackendIdentity& identity() const noexcept = 0;
  virtual const BackendConnectionDefaults& connection_defaults() const noexcept = 0;
  virtual const ISqlDialect& sql_dialect() const noexcept = 0;
  virtual BackendCapabilities capabilities() const noexcept = 0;
  virtual std::span<const TypeDefinition> type_catalog() const noexcept = 0;
  virtual TransactionCapabilities transaction_capabilities() const noexcept = 0;
  virtual rs::util::Result<ConnectionSettings> resolve_connection_options(
      ConnectionOptions options) const = 0;
  virtual std::unique_ptr<IDatabaseConnection> create_session(
      std::unique_ptr<rs::core::transport::ITransport> transport) const = 0;
};

// Link-time composition port. The final driver product supplies exactly one
// implementation; shared SDK and ODBC code depend only on this declaration.
const IBackendProvider& configured_backend_provider();

}  // namespace rs::core::database
