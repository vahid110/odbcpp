#pragma once
#include "odbcpp/auth/aws/provisioned_native_owner.h"
namespace rs::core::auth::aws::provisioned_native {
namespace detail { struct RuntimeState; }
// Owning explicit signing input, not account/IAM provenance. No serializer.
// Each field is locally bounded; SDK/transport copies are not guaranteed wiped.
class FrozenSigningSource final {
 public:
  static constexpr std::size_t max_field_bytes=4096;
  static std::variant<FrozenSigningSource,Failure> create(std::string identity,std::string generation,
      SecretBytes&& access_key,SecretBytes&& secret_key,SecretBytes&& session_token);
  FrozenSigningSource(const FrozenSigningSource&)=delete;
  FrozenSigningSource& operator=(const FrozenSigningSource&)=delete;
  FrozenSigningSource(FrozenSigningSource&&)=default;
  FrozenSigningSource& operator=(FrozenSigningSource&&)=delete;
 private:
  friend class ProvisionedSdkRuntime;
  FrozenSigningSource(std::string,std::string,SecretBytes&&,SecretBytes&&,SecretBytes&&);
  std::string identity_,generation_;SecretBytes access_,secret_,token_;
};
struct RuntimeCounts { unsigned initializations{},shutdowns{},idle_clients{},idle_requests{};bool occupied{},closed{}; };
struct RuntimeCreateOutcome;
enum class RuntimeInitFixture { BeforeEntry, AfterEntry };
// Optional standalone application lifetime; never adopts an unrelated SDK init.
// Startup/shutdown belong to Darwin main entry. Operations are creator-thread
// confined and may run sequentially on workers. Caller keeps this object alive
// during calls and freezes empty config/environment/no-exec before startup.
class ProvisionedSdkRuntime final {
 public:
  static RuntimeCreateOutcome create();
  ProvisionedSdkRuntime(const ProvisionedSdkRuntime&)=delete;
  ProvisionedSdkRuntime& operator=(const ProvisionedSdkRuntime&)=delete;
  ~ProvisionedSdkRuntime();
  CreateOutcome create_operation(Context,Request,std::shared_ptr<ResponseObservationSource>,
      std::shared_ptr<ResponseSourceGeneration>,std::shared_ptr<WorkerCancellation>,FrozenSigningSource&&);
  bool close() noexcept;RuntimeCounts counts() const noexcept;
 private:
  friend struct FixedRuntimeFixture;
  explicit ProvisionedSdkRuntime(std::shared_ptr<detail::RuntimeState>);
  static RuntimeCreateOutcome create_impl(std::optional<RuntimeInitFixture>);
  CreateOutcome make(Context&&,Request&&,std::shared_ptr<ResponseObservationSource>,
      std::shared_ptr<ResponseSourceGeneration>,std::shared_ptr<WorkerCancellation>,FrozenSigningSource&&,
      std::optional<FixedCase>);
  const std::shared_ptr<detail::RuntimeState> state_;
};
struct RuntimeCreateOutcome {std::unique_ptr<ProvisionedSdkRuntime> runtime;std::optional<Failure> failure;};
// Closed synthetic route only; same existing fixed bodies/context, marker gated.
enum class RuntimeFixtureSource { First, Second };
struct FixedRuntimeFixture final {
  static RuntimeCreateOutcome create_init_failure(RuntimeInitFixture);
  static Context context(RuntimeFixtureSource=RuntimeFixtureSource::First);
  static CreateOutcome create(ProvisionedSdkRuntime&,FixedCase,std::shared_ptr<ResponseObservationSource>,
      std::shared_ptr<ResponseSourceGeneration>,std::shared_ptr<WorkerCancellation>,RuntimeFixtureSource=RuntimeFixtureSource::First);
};
} // namespace
