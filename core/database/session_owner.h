#pragma once
#include "i_database_connection.h"
#include "credential_context.h"
#include <memory>
#include <optional>

namespace rs::core::database {
namespace detail { struct SessionOwnershipState; }

// Internal ownership primitive, not an installed SDK or pool API. A borrowed
// session is accessed only through lease operations: no raw session/facet escape.
// Callers must serialize operations and moves/retirement on one lease.
class SessionLease final {
 public:
  SessionLease(const SessionLease&) = delete;
  SessionLease& operator=(const SessionLease&) = delete;
  SessionLease(SessionLease&&) noexcept;
  SessionLease& operator=(SessionLease&&) noexcept;
  ~SessionLease();

  explicit operator bool() const noexcept { return physical_session() != nullptr; }
  // Execution preserves owning results/errors. A Retire snapshot or exception
  // destroys the physical session; other outcomes remain same-borrower only.
  BackendResult<QueryResult> execute_query(std::string_view sql, rs::util::Deadline deadline);
  BackendResult<QueryResult> execute_prepared(std::string_view sql,
      std::span<const QueryParameter> params, rs::util::Deadline deadline);
  // No I/O cleanup/reset/reconnect is attempted. Returning or abandoning the
  // lease retires and destroys the physical session, even after reset success.
  void retire() noexcept;
  // Explicit coordinator cleanup of this active borrow, using the original
  // deadline. Any failed/unsupported/ambiguous cleanup retires the lease.
  // Success keeps exclusive ownership, never grants return/requeue. Borrowers
  // cannot access the raw reset facet; cache tokens/reuse are not enabled.
  BackendResult<void> reset_session(rs::util::Deadline deadline);

 private:
  friend class SessionOwner;
  IDatabaseConnection* physical_session() const noexcept;
  explicit SessionLease(std::shared_ptr<detail::SessionOwnershipState>) noexcept;
  std::shared_ptr<detail::SessionOwnershipState> state_;
};

// At most one borrower for a uniquely adopted session. Concurrent checkout is
// supported; moving/destroying the same owner/lease requires external ordering.
// Owner destruction prevents checkout but cannot interrupt an active borrower.
// The lease keeps the physical session alive until retirement. Retirement is
// terminal: there is no adoption/requeue/reuse API or raw ownership extraction.
class SessionOwner final {
 public:
  explicit SessionOwner(std::unique_ptr<IDatabaseConnection>);
  // Trusted composition must bind this token to the adopted authenticated
  // session. Matching stale credentials close admission; active leases survive.
  SessionOwner(std::unique_ptr<IDatabaseConnection>, CredentialToken);
  SessionOwner(const SessionOwner&) = delete;
  SessionOwner& operator=(const SessionOwner&) = delete;
  SessionOwner(SessionOwner&&) noexcept;
  SessionOwner& operator=(SessionOwner&&) noexcept;
  ~SessionOwner();

  std::optional<SessionLease> try_acquire();
  // Missing, foreign and wrong-generation tokens cannot disrupt a valid owner.
  std::optional<SessionLease> try_acquire(const CredentialToken&);

 private:
  SessionOwner(std::unique_ptr<IDatabaseConnection>, std::optional<CredentialToken>);
  std::optional<SessionLease> try_acquire_impl(const CredentialToken*);
  void close() noexcept;
  std::shared_ptr<detail::SessionOwnershipState> state_;
};
} // namespace rs::core::database
