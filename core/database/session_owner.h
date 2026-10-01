#pragma once
#include "i_database_connection.h"
#include "credential_context.h"
#include <memory>
#include <optional>

namespace rs::core::database {
namespace detail {
struct SessionOwnershipState;
struct SessionCacheGeneration;
struct SessionOwnershipTestAccess;
}

// Private local scope identity, not cached data or a freshness guarantee for
// external schema changes. Validation is point-in-time, not a reservation.
// Consumers must also use the requesting lease's accepts_cache() affinity check.
class SessionCacheToken final {
 public:
  SessionCacheToken(const SessionCacheToken&) = default;
  SessionCacheToken& operator=(const SessionCacheToken&) = default;
  SessionCacheToken(SessionCacheToken&&) noexcept = default;
  SessionCacheToken& operator=(SessionCacheToken&&) noexcept = default;
  bool is_current() const noexcept;
 private:
  friend class SessionLease;
  SessionCacheToken(std::weak_ptr<detail::SessionOwnershipState>,
      std::weak_ptr<const detail::SessionCacheGeneration>) noexcept;
  std::weak_ptr<detail::SessionOwnershipState> state_;
  std::weak_ptr<const detail::SessionCacheGeneration> generation_;
};

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
  // Bound authenticated Idle leases only, established by passive checks without
  // backend calls under ownership locks. Every execution/reset attempt invalidates
  // all copies before backend access; closure/retirement/credential staleness
  // also invalidate. No payload or cache policy is provided.
  std::optional<SessionCacheToken> cache_token();
  bool accepts_cache(const SessionCacheToken&) const noexcept;
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
  // cannot access the raw reset facet; cache storage is not enabled.
  BackendResult<void> reset_session(rs::util::Deadline deadline);
  // Explicit opt-in return to this same credential-bound owner only. Always
  // performs reset with the original deadline, then rechecks admission and
  // credential freshness. Success consumes the lease and invalidates scopes;
  // every failure retires. Destruction/retire remain terminal. No pool/reconnect.
  BackendResult<void> return_reusable(rs::util::Deadline deadline);

 private:
  friend class SessionOwner;
  IDatabaseConnection* physical_session() const noexcept;
  void invalidate_cache() noexcept;
  explicit SessionLease(std::shared_ptr<detail::SessionOwnershipState>) noexcept;
  std::shared_ptr<detail::SessionOwnershipState> state_;
};

// At most one borrower for a uniquely adopted session. Concurrent checkout is
// supported; moving/destroying the same owner/lease requires external ordering.
// Owner destruction prevents checkout but cannot interrupt an active borrower.
// The lease keeps the physical session alive until retirement. Retirement is
// terminal unless explicit return_reusable succeeds; no raw ownership extraction.
// Opt-in private policy: finite absolute monotonic lifetime and positive idle
// duration. Idle starts at adoption and successful return only; no eviction timer.
struct SessionReusePolicy {
  rs::util::Deadline retire_at;
  rs::util::Clock::duration max_idle;
};

class SessionOwner final {
 public:
  explicit SessionOwner(std::unique_ptr<IDatabaseConnection>);
  // Trusted composition must bind this token to the adopted authenticated
  // session. Matching stale credentials close admission; active leases survive.
  SessionOwner(std::unique_ptr<IDatabaseConnection>, CredentialToken);
  SessionOwner(std::unique_ptr<IDatabaseConnection>, CredentialToken, SessionReusePolicy);
  SessionOwner(const SessionOwner&) = delete;
  SessionOwner& operator=(const SessionOwner&) = delete;
  SessionOwner(SessionOwner&&) noexcept;
  SessionOwner& operator=(SessionOwner&&) noexcept;
  ~SessionOwner();

  std::optional<SessionLease> try_acquire();
  // Missing, foreign and wrong-generation tokens cannot disrupt a valid owner.
  std::optional<SessionLease> try_acquire(const CredentialToken&);

 private:
  friend struct detail::SessionOwnershipTestAccess;
  using NowFactory = rs::util::Deadline(*)() noexcept;
  using CacheGenerationFactory = std::shared_ptr<const detail::SessionCacheGeneration>(*)();
  static std::shared_ptr<const detail::SessionCacheGeneration> make_cache_generation();
  SessionOwner(std::unique_ptr<IDatabaseConnection>, std::optional<CredentialToken>,
      CacheGenerationFactory = &make_cache_generation, std::optional<SessionReusePolicy> = std::nullopt, NowFactory = &rs::util::Clock::now);
  std::optional<SessionLease> try_acquire_impl(const CredentialToken*);
  void close() noexcept;
  std::shared_ptr<detail::SessionOwnershipState> state_;
};
} // namespace rs::core::database
