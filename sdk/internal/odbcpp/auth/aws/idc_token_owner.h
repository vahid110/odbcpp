#pragma once
#include "odbcpp/auth/aws_db_response_fields.h"
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include <memory>
#include <optional>

namespace rs::core::auth::aws::idc {
// Optional SDK-private redshift-serverless GetIdentityCenterAuthToken owner.
// IdC-issued AWS credentials become a native Identity Center subject token for
// the native-IdP login path. Produces the owning token plus trusted expiry; the
// native-IdP attributes (idp_type/provider_name/token_type/identity_namespace)
// stay with the caller, which owns option resolution and the login material.
enum class OwnerFailure {
  InvalidTarget, InvalidSource, MainEntryRequired, Unavailable,
  Consumed, WrongThread, LocalFailure, DeadlineElapsed
};
struct IdcTokenTarget {
  std::string region, workgroup;
};
struct IdcTokenFields {
  SecretBytes token;
  UtcInstant expires_at;
};
struct OwnerOutcome {
  std::unique_ptr<class IdcTokenOwner> owner;
  std::optional<OwnerFailure> failure;
};
// Test-only initialization fault points. AllocationFailure simulates the State
// allocation throwing before the process-wide SDK scope is taken; AfterEntry
// simulates InitAPI throwing after it has entered (no proven rollback).
enum class OwnerInitFixture { None, AllocationFailure, AfterEntry };
class IdcTokenOwner final {
 public:
  static OwnerOutcome create(IdcTokenTarget target, SecretBytes access_key, SecretBytes secret_key,
      SecretBytes session_token, rs::util::Deadline deadline,
      std::shared_ptr<provisioned_native::WorkerCancellation> cancel);
  IdcTokenOwner(const IdcTokenOwner&) = delete;
  IdcTokenOwner& operator=(const IdcTokenOwner&) = delete;
  IdcTokenOwner(IdcTokenOwner&&) = delete;
  IdcTokenOwner& operator=(IdcTokenOwner&&) = delete;
  ~IdcTokenOwner();
  FieldOutcome<IdcTokenFields> acquire();
  bool close() noexcept;
 private:
  struct State;
  friend struct IdcTokenOwnerFixture;
  static OwnerOutcome create_impl(IdcTokenTarget target, SecretBytes access_key, SecretBytes secret_key,
      SecretBytes session_token, rs::util::Deadline deadline,
      std::shared_ptr<provisioned_native::WorkerCancellation> cancel, OwnerInitFixture injected);
  explicit IdcTokenOwner(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
// Closed synthetic entry for offline initialization-fault tests; marker gated.
struct IdcTokenOwnerFixture final {
  static OwnerOutcome create_init_failure(OwnerInitFixture);
};
} // namespace rs::core::auth::aws::idc
