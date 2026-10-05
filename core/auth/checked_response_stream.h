#pragma once
#include "core/auth/bounded_response_stream.h"
#include "core/auth/checked_response_boundary.h"
namespace rs::core::auth {
class CheckedStreamError final {
 public:
  explicit CheckedStreamError(StreamFailure local) noexcept : local_(local) {}
  explicit CheckedStreamError(BoundaryFailure boundary) noexcept : boundary_(boundary) {}
  CheckedStreamError(StreamFailure local,BoundaryFailure boundary) noexcept : local_(local),boundary_(boundary) {}
  std::optional<StreamFailure> local() const noexcept { return local_; }
  std::optional<BoundaryFailure> boundary() const noexcept { return boundary_; }
 private:
  std::optional<StreamFailure> local_;
  std::optional<BoundaryFailure> boundary_;
};
template<class T> using CheckedStreamResult = std::variant<T,CheckedStreamError>;
namespace detail {
class CheckedResponseStreamState;
// Private deterministic construction-refusal seam, not allocator/provider policy.
enum class CheckedConstructionFailure { BeforeStorage, BeforeState, AfterState };
class CheckedStreamTestAccess;
}
class CheckedStreamOwner final {
 public:
  static CheckedStreamResult<CheckedStreamOwner> create(std::size_t cap,TransportBoundaryLease&&);
  CheckedStreamOwner(const CheckedStreamOwner&) = delete;
  CheckedStreamOwner& operator=(const CheckedStreamOwner&) = delete;
  CheckedStreamOwner(CheckedStreamOwner&&) noexcept;
  CheckedStreamOwner& operator=(CheckedStreamOwner&&) = delete;
  ~CheckedStreamOwner();
  std::iostream* io() noexcept;
  std::optional<CheckedStreamError> failure() const noexcept;
 private:
  friend CheckedStreamResult<ResponseBytes> seal_checked_response(CheckedStreamOwner&&);
  friend class detail::CheckedStreamTestAccess;
  explicit CheckedStreamOwner(std::unique_ptr<detail::CheckedResponseStreamState>) noexcept;
  static CheckedStreamResult<CheckedStreamOwner> create_impl(std::size_t,TransportBoundaryLease&&,
      std::optional<detail::CheckedConstructionFailure>);
  ResponseBytes take_body();
  std::unique_ptr<detail::CheckedResponseStreamState> state_;
};
// Consume before refusal; successful storage transfer ends Transport lease BEFORE
// return. Raw io/rdbuf borrowers must have ended. Unsealed drop rejects the body.
// Thread-confined; best-effort wiping cannot erase external copies/dangling views.
// Current EOF required only for eof/fail flags. Cleared forged history unknowable.
CheckedStreamResult<ResponseBytes> seal_checked_response(CheckedStreamOwner&&);
namespace detail {
class CheckedStreamTestAccess final {
 public:
  static CheckedStreamResult<CheckedStreamOwner> refuse_construction(std::size_t,
      TransportBoundaryLease&&,CheckedConstructionFailure);
};
}
}
