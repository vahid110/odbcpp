#pragma once
#include "odbcpp/auth/aws_db_response_fields.h"
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include <memory>
#include <optional>

namespace rs::core::auth::aws::serverless {
// Optional SDK-private redshift-serverless GetCredentials owner. Assumed-role
// (or static) AWS credentials become a temporary database login material via
// RedshiftServerlessClient.GetCredentials. Produces the owning typed fields
// (ExtractedDbFields); the trusted clock projection stays with the caller.
enum class OwnerFailure {
  InvalidTarget, InvalidSource, MainEntryRequired, Unavailable,
  Consumed, WrongThread, LocalFailure, DeadlineElapsed
};
struct ServerlessTarget {
  std::string region, workgroup, database;
  unsigned duration_seconds;
};
struct OwnerOutcome {
  std::unique_ptr<class ServerlessCredentialsOwner> owner;
  std::optional<OwnerFailure> failure;
};
// Test-only initialization fault points. AllocationFailure simulates the State
// allocation throwing before the process-wide SDK scope is taken; AfterEntry
// simulates InitAPI throwing after it has entered (no proven rollback).
enum class OwnerInitFixture { None, AllocationFailure, AfterEntry };
class ServerlessCredentialsOwner final {
 public:
  static OwnerOutcome create(ServerlessTarget target, SecretBytes access_key, SecretBytes secret_key,
      SecretBytes session_token, rs::util::Deadline deadline, std::shared_ptr<provisioned_native::WorkerCancellation> cancel);
  ServerlessCredentialsOwner(const ServerlessCredentialsOwner&) = delete;
  ServerlessCredentialsOwner& operator=(const ServerlessCredentialsOwner&) = delete;
  ServerlessCredentialsOwner(ServerlessCredentialsOwner&&) = delete;
  ServerlessCredentialsOwner& operator=(ServerlessCredentialsOwner&&) = delete;
  ~ServerlessCredentialsOwner();
  FieldOutcome<ExtractedDbFields> acquire();
  bool close() noexcept;
 private:
  struct State;
  friend struct ServerlessOwnerFixture;
  static OwnerOutcome create_impl(ServerlessTarget target, SecretBytes access_key, SecretBytes secret_key,
      SecretBytes session_token, rs::util::Deadline deadline, std::shared_ptr<provisioned_native::WorkerCancellation> cancel,
      OwnerInitFixture injected);
  explicit ServerlessCredentialsOwner(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
// Closed synthetic entry for offline initialization-fault tests; marker gated.
struct ServerlessOwnerFixture final {
  static OwnerOutcome create_init_failure(OwnerInitFixture);
};
} // namespace rs::core::auth::aws::serverless
