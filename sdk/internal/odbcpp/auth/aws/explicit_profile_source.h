#pragma once
#include "odbcpp/auth/auth_core.h"
#include <thread>

namespace rs::core::auth::aws::profile {
// Optional private, SDK-free source-policy prototype. Injected bytes/metadata
// are untrusted observations, NOT filesystem/issuer/account/expiry proof.
// No file/environment/provider/network reads; no Material/Receipt projection.
enum class Failure {
  InvalidSpec, InvalidPolicy, InvalidProjection, SourceMismatch, SourceRetired,
  ReadFailed, ClockRollback, DeadlineElapsed, Cancelled, LoadFailed,
  MissingAccessKey, MissingSecretKey, InvalidCredentials, Consumed,
  WrongThread, Reentrant, AllocationFailed
};
std::string_view safe_message(Failure) noexcept;
template<class T> using Result = std::variant<T, Failure>;
struct SourceSpec {
  std::string profile, credentials_path, config_path, source_identity,
      generation, file_identity;
  bool operator==(const SourceSpec&) const = default;
};
struct StagingProjection {
  enum class Kind { Regular, Other };
  Kind kind;
  unsigned directory_mode, file_mode, links;
  bool alias;
  std::string file_identity;
};
// Creator-thread negative retirement only, not an authority or concurrency tool.
class SourceGeneration final {
 public:
  explicit SourceGeneration(std::string generation);
  SourceGeneration(const SourceGeneration&) = delete;
  SourceGeneration& operator=(const SourceGeneration&) = delete;
  bool retire() noexcept;
 private:
  friend class ExplicitProfileSource;
  std::thread::id thread_;
  std::string generation_;
  bool retired_{false};
};
// Credentials loaded by an injected owner. SDK strings can be copied before
// this boundary; guaranteed cleansing of all copies is NOT claimed.
struct CredentialParts {
  SecretBytes access_key, secret_key, session_token;
};
enum class LoadFailure { Failed };
enum class ReadFailure { Failed };
using Reader = std::function<std::variant<rs::util::Deadline, ReadFailure>()>;
using Loader = std::function<std::variant<CredentialParts, LoadFailure>(
    const SourceSpec&, const Request&)>;
class FrozenCredentials final {
 public:
  FrozenCredentials(const FrozenCredentials&) = delete;
  FrozenCredentials& operator=(const FrozenCredentials&) = delete;
  FrozenCredentials(FrozenCredentials&&);
  FrozenCredentials& operator=(FrozenCredentials&&) = delete;
  ~FrozenCredentials();
  template<class Consumer> bool with_parts(Consumer&& consumer) const {
    if (!valid_ || consuming_ || thread_ != std::this_thread::get_id()) return false;
    consuming_ = true;
    struct Guard { bool& flag; ~Guard(){ flag=false; } } guard{consuming_};
    std::invoke(std::forward<Consumer>(consumer), parts_);
    return true;
  }
  const SourceSpec& source() const noexcept { return source_; }
  // No expiry/account fields: file provider max-expiration is not freshness.
 private:
  friend class ExplicitProfileSource;
  FrozenCredentials(SourceSpec source, CredentialParts parts);
  static SourceSpec move_source(FrozenCredentials&);
  SourceSpec source_;
  CredentialParts parts_;
  std::thread::id thread_;
  bool valid_{true};
  mutable bool consuming_{false};
};
class ExplicitProfileSource final {
 public:
  // Consumes policy bytes immediately even on refusal. Grammar is a narrow
  // bounded single credentials section, NOT the SDK's full INI grammar.
  static Result<std::unique_ptr<ExplicitProfileSource>> create(
      SourceSpec, StagingProjection, SecretBytes&& policy_bytes, Request,
      std::shared_ptr<SourceGeneration>, Reader, Loader);
  ~ExplicitProfileSource();
  ExplicitProfileSource(const ExplicitProfileSource&) = delete;
  ExplicitProfileSource& operator=(const ExplicitProfileSource&) = delete;
  Result<FrozenCredentials> load(const Cancellation* cancelled = nullptr);
  const SourceSpec& source() const noexcept { return source_; }
  std::optional<Failure> failure() const noexcept { return first_; }
 private:
  ExplicitProfileSource(SourceSpec, Request, std::shared_ptr<SourceGeneration>, Reader, Loader);
  std::optional<Failure> checkpoint(const Cancellation*) noexcept;
  Failure latch(Failure) noexcept;
  SourceSpec source_;
  Request request_;
  std::shared_ptr<SourceGeneration> generation_;
  Reader reader_;
  Loader loader_;
  std::thread::id thread_;
  std::optional<rs::util::Deadline> highwater_;
  std::optional<Failure> first_;
  bool consumed_{false}, active_{false};
};
} // namespace rs::core::auth::aws::profile
