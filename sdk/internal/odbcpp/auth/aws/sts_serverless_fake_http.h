#pragma once
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include <aws/core/http/HttpClientFactory.h>
#include <atomic>
#include <memory>
#include <string>

namespace rs::core::auth::aws::detail {
// Test-only synthetic HTTP for the standalone STS/serverless owners. Never
// active in production: the owners install it only under the reviewed offline
// marker (ODBCPP_AUTH_SDK_OFFLINE_FIXTURE == "synthetic-fake-http-only"). It
// records endpoint/scheme/signing observations and refuses any non-approved
// target, mirroring the pinned-endpoint guarantee that production enforces via
// endpointOverride + a verified TLS connection.
enum class FakeHttpScenario : unsigned { Success, Denied, LateCompletion, CancelDuringTransport };

struct FakeHttpObservation {
  std::atomic<unsigned> sends{0};
  std::atomic<unsigned> approved{0};
  std::atomic<unsigned> foreign_scheme{0};
  std::atomic<unsigned> foreign_authority{0};
  std::atomic<unsigned> denied{0};
  std::atomic<unsigned> assume_role{0};
  std::atomic<unsigned> get_credentials{0};
  std::atomic<unsigned> late_completion{0};
  void reset() noexcept {
    sends = 0; approved = 0; foreign_scheme = 0; foreign_authority = 0;
    denied = 0; assume_role = 0; get_credentials = 0; late_completion = 0;
  }
};

FakeHttpObservation& fake_http_observation() noexcept;
bool fake_http_enabled() noexcept;
void fake_http_configure(FakeHttpScenario, unsigned delay_ms,
    std::shared_ptr<provisioned_native::WorkerCancellation>);
std::shared_ptr<Aws::Http::HttpClientFactory> make_fake_http_factory(
    std::string expected_authority, bool sts);
} // namespace rs::core::auth::aws::detail
