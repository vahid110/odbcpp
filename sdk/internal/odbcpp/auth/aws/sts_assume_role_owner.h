#pragma once
#include "odbcpp/auth/sts_assume_role_response.h"
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include <memory>
#include <optional>

namespace rs::core::auth::aws::sts {
// Optional SDK-private STS AssumeRole observational owner. Produces the owning
// typed response (StsAssumeRoleResponseUtc); the trusted clock projection stays
// with the caller that owns a monotonic/UTC sampler, mirroring the split between
// extract_db_fields and project_provisioned_response.
enum class OwnerFailure {
  InvalidTarget, InvalidSource, MainEntryRequired, Unavailable,
  Consumed, WrongThread, LocalFailure, ProviderDenied
};
struct OwnerOutcome {
  std::unique_ptr<class StsAssumeRoleOwner> owner;
  std::optional<OwnerFailure> failure;
};
class StsAssumeRoleOwner final {
 public:
  static OwnerOutcome create(StsAssumeRoleTarget target, SecretBytes access_key, SecretBytes secret_key,
      SecretBytes session_token, rs::util::Deadline deadline, std::shared_ptr<provisioned_native::WorkerCancellation> cancel);
  StsAssumeRoleOwner(const StsAssumeRoleOwner&) = delete;
  StsAssumeRoleOwner& operator=(const StsAssumeRoleOwner&) = delete;
  StsAssumeRoleOwner(StsAssumeRoleOwner&&) = delete;
  StsAssumeRoleOwner& operator=(StsAssumeRoleOwner&&) = delete;
  ~StsAssumeRoleOwner();
  StsOutcome<StsAssumeRoleResponseUtc> acquire();
  bool close() noexcept;
 private:
  struct State;
  explicit StsAssumeRoleOwner(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
} // namespace rs::core::auth::aws::sts
