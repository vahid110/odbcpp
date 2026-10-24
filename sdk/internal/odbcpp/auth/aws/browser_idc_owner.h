#pragma once
#include "odbcpp/auth/aws_db_response_fields.h"
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include <chrono>
#include <memory>
#include <optional>

namespace rs::core::auth::aws::browser_idc {
// Optional SDK-private Identity Center OAuth/PKCE owner. The IdC user signs in
// in a browser (authorization-code + PKCE against the Redshift IdC application)
// and the driver exchanges the code for a native subject token. Produces the
// owning token plus trusted expiry; the native-IdP attributes stay with the
// caller. The loopback redirect is bound to 127.0.0.1 only.
enum class OwnerFailure {
  InvalidTarget, MainEntryRequired, Unavailable, Consumed, WrongThread,
  LocalFailure, DeadlineElapsed, Cancelled, AuthenticationFailed
};
struct BrowserIdcTarget {
  std::string issuer_url;   // https://identitycenter.amazonaws.com/ssoins-<id>
  std::string region;       // IdC region (e.g. us-east-1)
  unsigned short listen_port = 7890;  // loopback redirect port
  std::chrono::seconds response_timeout{120};
};
struct BrowserIdcToken {
  SecretBytes token;
  UtcInstant expires_at;
};
struct OwnerOutcome {
  std::unique_ptr<class BrowserIdcOwner> owner;
  std::optional<OwnerFailure> failure;
};
class BrowserIdcOwner final {
 public:
  static OwnerOutcome create(BrowserIdcTarget target, rs::util::Deadline deadline,
      std::shared_ptr<provisioned_native::WorkerCancellation> cancel);
  BrowserIdcOwner(const BrowserIdcOwner&) = delete;
  BrowserIdcOwner& operator=(const BrowserIdcOwner&) = delete;
  BrowserIdcOwner(BrowserIdcOwner&&) = delete;
  BrowserIdcOwner& operator=(BrowserIdcOwner&&) = delete;
  ~BrowserIdcOwner();
  FieldOutcome<BrowserIdcToken> acquire();
  bool close() noexcept;
 private:
  struct State;
  explicit BrowserIdcOwner(std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
} // namespace rs::core::auth::aws::browser_idc
