#pragma once
#include "i_database_connection.h"
#include <memory>
#include <optional>

namespace rs::core::database {
namespace detail { struct SessionOwnershipState; }

// Internal ownership primitive, not an installed SDK or pool API. A borrowed
// session/facet pointer is valid only while its lease remains active. Callers
// must serialize operations on one lease and must not retain escaped pointers.
class SessionLease final {
 public:
  SessionLease(const SessionLease&) = delete;
  SessionLease& operator=(const SessionLease&) = delete;
  SessionLease(SessionLease&&) noexcept;
  SessionLease& operator=(SessionLease&&) noexcept;
  ~SessionLease();

  IDatabaseConnection* session() const noexcept;
  explicit operator bool() const noexcept { return session() != nullptr; }
  // No I/O cleanup/reset/reconnect is attempted. Returning or abandoning the
  // lease retires and destroys the physical session, even after reset success.
  void retire() noexcept;

 private:
  friend class SessionOwner;
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
  SessionOwner(const SessionOwner&) = delete;
  SessionOwner& operator=(const SessionOwner&) = delete;
  SessionOwner(SessionOwner&&) noexcept;
  SessionOwner& operator=(SessionOwner&&) noexcept;
  ~SessionOwner();

  std::optional<SessionLease> try_acquire();

 private:
  void close() noexcept;
  std::shared_ptr<detail::SessionOwnershipState> state_;
};
} // namespace rs::core::database
