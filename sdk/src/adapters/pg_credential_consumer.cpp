#include "odbcpp/auth/pg_credential_consumer.h"
#include "core/database/postgres/pg_database_connection.h"
#include <algorithm>
#include <new>
namespace rs::core::auth {
namespace {
using namespace rs::core::database;
struct PasswordCleanup {
  std::string& value;
  ~PasswordCleanup() {
    volatile char* bytes = value.data();
    for (std::size_t i = 0; i < value.size(); ++i) bytes[i] = 0;
    value.clear(); // Best effort; allocator/transport copies are not guaranteed erased.
  }
};
}
rs::core::database::BackendResult<void> connect_bound_temporary_db_until(
    rs::core::database::postgres::PgDatabaseConnection& connection,
    const rs::core::database::ConnectionSettings& selected, const Request& request,
    const ExtractedDbFields& fields) {
  using namespace rs::core::database;
  const auto invalid = [&]() -> BackendResult<void> {
    return local_backend_error(LocalFailure::InvalidInput,
        "Invalid bound database credential handoff", BackendOperation::Connect,
        connection.session_state());
  };
  const auto& binding = request.binding();
  const auto& target = binding.target();
  if (binding.invariant_error() ||
      binding.method() != Method::TemporaryDatabasePassword ||
      binding.source().kind != SourceKind::TrustedTemporaryDbIssuer ||
      (target.service != Service::PostgreSql && target.service != Service::Redshift &&
       target.service != Service::Rds) ||
      target.endpoint != selected.host || target.port != selected.port ||
      target.database != selected.database || target.principal != selected.user ||
      fields.user != target.principal || target.tls_identity != selected.host ||
      target.trust_policy != "verify-full" || !selected.use_ssl ||
      selected.ssl_ca_file.empty() || selected.ssl_ca_file.find('\0') != std::string::npos ||
      !selected.ssl_ca_dir.empty() ||
      !selected.password.empty() || connection.is_connected() ||
      fields.password.empty() || fields.password.size() > SecretBytes::max_bytes ||
      request.deadline() == rs::util::Deadline::min() ||
      request.deadline() == rs::util::Deadline::max()) return invalid();
  if (rs::util::Clock::now() >= request.deadline()) {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),
                       "Database credential handoff deadline elapsed"};
    error.operation = BackendOperation::Connect;
    error.session_state = connection.session_state();
    error.disposition = SessionDisposition::Retire;
    return error;
  }
  try {
    ConnectionSettings settings = selected;
    PasswordCleanup cleanup{settings.password};
    bool contains_nul = false;
    fields.password.with_bytes([&](std::span<const std::byte> bytes) {
      contains_nul = std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end();
      if (!contains_nul) settings.password.assign(
          reinterpret_cast<const char*>(bytes.data()), bytes.size());
    });
    if (contains_nul) return invalid();
    return connection.connect_until(settings, request.deadline());
  } catch (const std::bad_alloc&) {
    connection.disconnect();
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::AllocationFailure),
                       "Database credential handoff allocation failed"};
    error.operation = BackendOperation::Connect;
    error.session_state = SessionState::Disconnected;
    error.disposition = SessionDisposition::Retire;
    return error;
  } catch (...) {
    connection.disconnect();
    return local_backend_error(LocalFailure::InvalidInput,
        "Database credential handoff failed", BackendOperation::Connect,
        SessionState::Disconnected);
  }
}
} // namespace rs::core::auth

namespace rs::core::auth {
rs::core::database::BackendResult<void> connect_bound_ordinary_password_until(
    rs::core::database::postgres::PgDatabaseConnection& connection,
    const rs::core::database::ConnectionSettings& selected, const Request& request,
    const Material& material) {
  using namespace rs::core::database;
  const auto invalid = [&]() -> BackendResult<void> {
    return local_backend_error(LocalFailure::InvalidInput,
        "Invalid bound ordinary password handoff", BackendOperation::Connect,
        connection.session_state());
  };
  const auto& binding = request.binding();
  const auto& target = binding.target();
  const auto& supplied = material.binding();
  const auto validity = material.validity();
  if (binding.invariant_error() || supplied.invariant_error() ||
      binding.method() != Method::OrdinaryPassword ||
      supplied.method() != Method::OrdinaryPassword ||
      material.kind() != MaterialKind::OrdinaryPassword ||
      binding.source().kind != SourceKind::ExternalPassword ||
      target != supplied.target() || binding.source() != supplied.source() ||
      material.principal() != target.principal ||
      validity.kind != Validity::Kind::NoKnownAcquisitionExpiry ||
      validity.issued_at != rs::util::Deadline{} ||
      validity.expires_at != rs::util::Deadline{} ||
      target.endpoint != selected.host || target.port != selected.port ||
      target.database != selected.database || target.principal != selected.user ||
      target.tls_identity != selected.host || target.trust_policy != "verify-full" ||
      !selected.use_ssl || selected.ssl_ca_file.empty() ||
      selected.ssl_ca_file.find('\0') != std::string::npos || !selected.ssl_ca_dir.empty() ||
      !selected.password.empty() || connection.is_connected() ||
      request.deadline() == rs::util::Deadline::min() ||
      request.deadline() == rs::util::Deadline::max()) return invalid();
  if (rs::util::Clock::now() >= request.deadline()) {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),
                       "Ordinary password handoff deadline elapsed"};
    error.operation = BackendOperation::Connect;
    error.session_state = connection.session_state();
    error.disposition = SessionDisposition::Retire;
    return error;
  }
  try {
    ConnectionSettings settings = selected;
    PasswordCleanup cleanup{settings.password};
    bool invalid_secret = false;
    material.with_secret([&](std::span<const std::byte> bytes) {
      invalid_secret = bytes.empty() || bytes.size() > SecretBytes::max_bytes ||
          std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end();
      if (!invalid_secret) settings.password.assign(
          reinterpret_cast<const char*>(bytes.data()), bytes.size());
    });
    if (invalid_secret) return invalid();
    return connection.connect_until(settings, request.deadline());
  } catch (const std::bad_alloc&) {
    connection.disconnect();
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::AllocationFailure),
                       "Ordinary password handoff allocation failed"};
    error.operation = BackendOperation::Connect;
    error.session_state = SessionState::Disconnected;
    error.disposition = SessionDisposition::Retire;
    return error;
  } catch (...) {
    connection.disconnect();
    return local_backend_error(LocalFailure::InvalidInput,
        "Ordinary password handoff failed", BackendOperation::Connect,
        SessionState::Disconnected);
  }
}
} // namespace rs::core::auth
