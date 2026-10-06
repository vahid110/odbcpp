#pragma once
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include "odbcpp/auth/aws/native_body_capture.h"
#include "odbcpp/auth/checked_response_stream.h"
#include <aws/core/Aws.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/crt/io/Bootstrap.h>
#include <thread>
namespace rs::core::auth::aws::provisioned_native::detail {
struct Wire;class ProtectedNamedStage;
struct State final {
  State(Context,Request,std::shared_ptr<ResponseObservationSource>,
      std::shared_ptr<ResponseSourceGeneration>,std::shared_ptr<WorkerCancellation>,FrozenNamedSource&&,
      std::optional<FixedCase>);
  ~State();
  const Context context;const Request request;
  std::shared_ptr<ResponseObservationSource> observer;
  std::shared_ptr<ResponseSourceGeneration> generation;
  const std::shared_ptr<WorkerCancellation> cancellation;
  std::unique_ptr<ResponseOperation> operation;FrozenNamedSource source;
  std::unique_ptr<ProtectedNamedStage> protected_stage;bool source_ready{true};
  const std::optional<FixedCase> fixed;const std::thread::id creator;
  std::shared_ptr<RuntimeState> runtime;
  Aws::SDKOptions options;std::shared_ptr<Aws::Crt::Io::ClientBootstrap> bootstrap;
  Counts metrics;std::optional<TransportBoundaryLease> dispatch;
  std::shared_ptr<Wire> wire;std::optional<FrozenNativeBody> frozen;
  std::optional<ExtractedDbFields> fields;
  std::string staged_directory,staged_config,staged_credentials,expected_query;
  const Aws::Http::HttpClient* selected_client{};const Aws::Http::HttpRequest* selected_request{};
  bool token{},initialized{},attempt{},active{},creating{},consumed{},observing{},service_live{},validating{};
  Aws::Auth::AWSCredentials source_value() const;
  TransportBoundaryLease lease();bool check() noexcept;void reject() noexcept;
  void sync() noexcept;void shutdown_sdk() noexcept;void cleanup() noexcept;
};
// Runtime slot serializes only our own operations, not unrelated SDK users.
bool take_sdk_scope() noexcept;void release_sdk_scope() noexcept;
bool runtime_usable(const std::shared_ptr<RuntimeState>&) noexcept;
void release_runtime_slot(const std::shared_ptr<RuntimeState>&) noexcept;
std::shared_ptr<Aws::Crt::Io::ClientBootstrap> runtime_bootstrap(const std::shared_ptr<RuntimeState>&);
void note_runtime_idle(const std::weak_ptr<RuntimeState>&,bool request) noexcept;
class RuntimeRoute final {
 public:explicit RuntimeRoute(std::shared_ptr<State>);~RuntimeRoute();
  RuntimeRoute(const RuntimeRoute&)=delete;RuntimeRoute& operator=(const RuntimeRoute&)=delete;
 private:const std::shared_ptr<State> state_;
};
std::shared_ptr<Aws::Http::HttpClientFactory> make_runtime_factory(std::weak_ptr<RuntimeState>);
// Internal operations cannot grant issuance or expose a public delegate override.
std::shared_ptr<Aws::Http::HttpClientFactory> make_factory(State&);
void detach_reader(State&) noexcept;
void check_capture(State&) noexcept;
void late_writer(State&) noexcept;
} // namespace
