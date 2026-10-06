#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <utility>

namespace rs::core::auth::aws {
// Passive local diagnostics, never issuing identity, time quality or authority.
enum class CaptureFailure { InvalidLimit, InvalidOwner, AllocationFailed,
  CapacityExceeded, InvalidState, CallbackFailure, Cancelled, MissingQuiescence,
  InvalidRange, WrongThread };
using CaptureStatus=std::variant<std::monostate,CaptureFailure>;
template<class T> using CaptureResult=std::variant<T,CaptureFailure>;
namespace detail { struct NativeCaptureState; class NativeCaptureTestAccess; class NativeCaptureIngressOwner; class NativeCrtCaptureOwner; class ProvisionedNativeCaptureOwner; }
class NativeCaptureWriter;
class NativeCaptureHandoff;
class FrozenNativeBody;
class NativeCaptureWriteGuard final {
 public:
  NativeCaptureWriteGuard(const NativeCaptureWriteGuard&)=delete;
  NativeCaptureWriteGuard& operator=(const NativeCaptureWriteGuard&)=delete;
  NativeCaptureWriteGuard(NativeCaptureWriteGuard&&) noexcept;
  NativeCaptureWriteGuard& operator=(NativeCaptureWriteGuard&&)=delete;
  ~NativeCaptureWriteGuard();
  // All-or-nothing, including overflow. Empty chunks succeed only while healthy.
  // The caller must supply a valid span; no C++ address-validity proof is made.
  CaptureStatus append(std::span<const std::byte>) noexcept;
  CaptureStatus reject(CaptureFailure) noexcept; // Only CallbackFailure/Cancelled.
 private:
  friend class NativeCaptureWriter;
  explicit NativeCaptureWriteGuard(std::shared_ptr<detail::NativeCaptureState>) noexcept;
  std::shared_ptr<detail::NativeCaptureState> state_;
};
class NativeCaptureWriter final {
 public:
  NativeCaptureWriter(const NativeCaptureWriter&)=default;
  NativeCaptureWriter& operator=(const NativeCaptureWriter&)=default;
  NativeCaptureWriter(NativeCaptureWriter&&) noexcept=default;
  NativeCaptureWriter& operator=(NativeCaptureWriter&&) noexcept=default;
  CaptureResult<NativeCaptureWriteGuard> acquire() const noexcept;
  std::optional<CaptureFailure> failure() const noexcept;
 private:
  friend class NativeBodyCapture;
  explicit NativeCaptureWriter(std::shared_ptr<detail::NativeCaptureState> state) noexcept:state_(std::move(state)) {}
  std::shared_ptr<detail::NativeCaptureState> state_;
};
class NativeBodyCapture final {
 public:
  static constexpr std::size_t max_bytes=131072; // Local cap, not AWS limit.
  static CaptureResult<NativeBodyCapture> create(std::size_t) noexcept;
  NativeBodyCapture(const NativeBodyCapture&)=delete;
  NativeBodyCapture& operator=(const NativeBodyCapture&)=delete;
  NativeBodyCapture(NativeBodyCapture&&) noexcept;
  NativeBodyCapture& operator=(NativeBodyCapture&&)=delete;
  ~NativeBodyCapture();
  CaptureResult<NativeCaptureWriter> writer() const noexcept;
  std::optional<CaptureFailure> failure() const noexcept;
 private:
  friend class detail::NativeCaptureTestAccess;
  friend class detail::NativeCaptureIngressOwner;
  friend class detail::NativeCrtCaptureOwner;
  friend class detail::ProvisionedNativeCaptureOwner;
  explicit NativeBodyCapture(std::shared_ptr<detail::NativeCaptureState>) noexcept;
  std::shared_ptr<detail::NativeCaptureState> state_;
};
class FrozenNativeBody final {
 public:
  FrozenNativeBody(const FrozenNativeBody&)=delete;
  FrozenNativeBody& operator=(const FrozenNativeBody&)=delete;
  FrozenNativeBody(FrozenNativeBody&&) noexcept;
  FrozenNativeBody& operator=(FrozenNativeBody&&)=delete;
  ~FrozenNativeBody();
  std::optional<CaptureFailure> failure() const noexcept;
  CaptureStatus read(std::size_t offset,std::span<std::byte> destination) const noexcept;
  // Consumer runs without storage mutex. View cannot escape. This thread-confined
  // owner cannot move/destruct/reenter while its scoped view is active (fatal).
  template<class Consumer> CaptureStatus with_bytes(Consumer&& consume) const {
    auto result=begin_view();
    if(const auto* failure=std::get_if<CaptureFailure>(&result)) return *failure;
    struct End { const FrozenNativeBody& owner;~End(){owner.end_view();} } end{*this};
    std::invoke(std::forward<Consumer>(consume),std::get<std::span<const std::byte>>(result));
    return std::monostate{};
  }
 private:
  friend class NativeCaptureHandoff;
  explicit FrozenNativeBody(std::shared_ptr<detail::NativeCaptureState>) noexcept;
  CaptureResult<std::span<const std::byte>> begin_view() const noexcept;
  void end_view() const noexcept;
  std::shared_ptr<detail::NativeCaptureState> state_;
};
// Sole owning handoff. No public constructor/factory/bool-quiescence API. Current
// test-local access models a barrier only; native delegate shutdown is unproved.
class NativeCaptureHandoff final {
 public:
  NativeCaptureHandoff(const NativeCaptureHandoff&)=delete;
  NativeCaptureHandoff& operator=(const NativeCaptureHandoff&)=delete;
  NativeCaptureHandoff(NativeCaptureHandoff&&) noexcept;
  NativeCaptureHandoff& operator=(NativeCaptureHandoff&&)=delete;
  ~NativeCaptureHandoff();
  CaptureResult<FrozenNativeBody> freeze() noexcept;
 private:
  friend class detail::NativeCaptureTestAccess;
  friend class detail::NativeCaptureIngressOwner;
  friend class detail::NativeCrtCaptureOwner;
  friend class detail::ProvisionedNativeCaptureOwner;
  explicit NativeCaptureHandoff(std::shared_ptr<detail::NativeCaptureState>) noexcept;
  std::shared_ptr<detail::NativeCaptureState> state_;
};
namespace detail {
// Narrow allocation/handoff seam, NOT production SDK quiescence authority.
enum class CaptureAllocationStage { BeforeStorage, BeforeState };
class NativeCaptureTestAccess final {
 public:
  static CaptureResult<NativeBodyCapture> create_failing(std::size_t,CaptureAllocationStage) noexcept;
  static CaptureResult<NativeCaptureHandoff> handoff(NativeBodyCapture&&) noexcept;
  static std::optional<std::size_t> written_size(const NativeBodyCapture&) noexcept;
};
}
// State/guard allocation precedes callbacks. Append/acquire perform no allocation,
// callbacks or clocks under mutex. Unexpected mutex/runtime failure terminates;
// noexcept does NOT recover platform invariants. Storage APIs are thread-safe;
// this does not make an SDK iostream safe for concurrent access. Best-effort wipe
// of owned retained bytes cannot cleanse caller/transport/register/swap copies.
} // namespace rs::core::auth::aws
