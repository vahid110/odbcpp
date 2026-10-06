#pragma once
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include "odbcpp/database/backend_result.h"
namespace rs::core::database { struct ConnectionSettings; }
namespace rs::core::database::postgres { class PgDatabaseConnection; }
namespace rs::core::auth::aws::provisioned_native {
// Optional AWS-to-PG composition; never part of provider-free AuthCore.
// Uses the owner's supported standalone creator/main-entry SDK scope only.
enum class ConnectorFailure { InvalidObservation, ConnectionRejected,
  ObservationRejected, LocalFailure };
struct ConnectorOutcome {
  std::optional<rs::core::database::BackendResult<void>> connection;
  std::optional<ConnectorFailure> failure;
  Counts counts;
  bool entered{},wire_identity{},password_present{},raw_expiry_positive{},
      borrow_passed{},observation_closed{};
  bool succeeded() const noexcept {
    return !failure && connection && connection->has_value() && borrow_passed &&
        observation_closed && !counts.boundary && counts.n_samples==1;
  }
};
// Consumes observation by closing it, retaining any owning backend error.
// Success leaves the new PG session caller-owned. Enclosing failure after a
// successful connect disconnects it. No reacquisition/retry/receipt/UTC claim.
// NativeOwner remains caller-owned and must be closed before general reuse.
ConnectorOutcome connect_provisioned_observation_until(
    rs::core::database::postgres::PgDatabaseConnection&,
    const rs::core::database::ConnectionSettings&, Observation&);
} // namespace rs::core::auth::aws::provisioned_native
