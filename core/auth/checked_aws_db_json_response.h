#pragma once
#include "core/auth/aws_db_json_response.h"
#include "core/auth/checked_response_boundary.h"
namespace rs::core::auth {
class CheckedJsonError final {
 public:
  explicit CheckedJsonError(JsonError local) noexcept : local_(local) {}
  explicit CheckedJsonError(BoundaryFailure boundary) noexcept : boundary_(boundary) {}
  CheckedJsonError(JsonError local,BoundaryFailure boundary) noexcept : local_(local),boundary_(boundary) {}
  std::optional<JsonError> local() const noexcept { return local_; }
  std::optional<BoundaryFailure> boundary() const noexcept { return boundary_; }
  std::string_view safe_message() const noexcept {
    return boundary_?"Checked JSON boundary refused":"Checked JSON representation refused";
  }
 private:
  std::optional<JsonError> local_;
  std::optional<BoundaryFailure> boundary_;
};
template<class T> using CheckedJsonResult = std::variant<T,CheckedJsonError>;
// Post-c only, consumes inactive body and phase before representation refusal.
// Active with_bytes entry move is fatal borrower misuse, never healthy return.
// Snapshot is owning/untrusted; later extraction needs its own retained phase.
CheckedJsonResult<ResponseSnapshot> parse_aws_db_json_response_checked(
    DbCredentialOperation,ResponseShape,ResponseBytes&&,ProcessingBoundaryLease&&);
namespace detail {
// Private closed allocation-refusal seam, not a provider/callback factory.
enum class CheckedJsonConstructionFailure { BeforeParse, BeforePublication };
class CheckedJsonTestAccess final {
 public:
  static CheckedJsonResult<ResponseSnapshot> refuse_processing(DbCredentialOperation,
      ResponseShape,ResponseBytes&&,ProcessingBoundaryLease&&,CheckedJsonConstructionFailure);
};
}
}
