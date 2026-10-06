#include "odbcpp/auth/aws/native_body_capture.h"
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

namespace rs::core::auth::aws {
namespace detail {
struct NativeCaptureState final {
  NativeCaptureState(std::unique_ptr<std::byte[]> data,std::size_t limit) noexcept
      :bytes(std::move(data)),cap(limit),creator(std::this_thread::get_id()) {}
  ~NativeCaptureState() { wipe(); }
  void wipe() noexcept {
    volatile std::byte* p=bytes.get();for(std::size_t i=0;i<cap;++i) p[i]=std::byte{0};size=0;
  }
  CaptureFailure latch(CaptureFailure f) noexcept { if(!first) { first=f; }return *first; }
  std::mutex mutex;std::unique_ptr<std::byte[]> bytes;const std::size_t cap;
  const std::thread::id creator;std::size_t size{},writers{};
  bool closed{},frozen{},viewing{};std::optional<CaptureFailure> first;
};
}
namespace {
using State=detail::NativeCaptureState;
void require_owner(const std::shared_ptr<State>& s) noexcept {
  if(s && (s->creator!=std::this_thread::get_id() || s->viewing)) std::terminate();
}
void abandon(std::shared_ptr<State>& s) noexcept {
  if(!s) return;
  require_owner(s);
  { std::lock_guard lock(s->mutex);s->closed=true;if(!s->frozen) s->latch(CaptureFailure::InvalidState);s->wipe(); }
  s.reset();
}
std::optional<CaptureFailure> failure(const std::shared_ptr<State>& s) noexcept {
  if(!s) return CaptureFailure::InvalidOwner;
  std::lock_guard lock(s->mutex);return s->first;
}
CaptureResult<std::shared_ptr<State>> make_state(std::size_t cap,
    std::optional<detail::CaptureAllocationStage> inject) noexcept {
  if(cap==0 || cap>NativeBodyCapture::max_bytes) return CaptureFailure::InvalidLimit;
  try {
    if(inject==detail::CaptureAllocationStage::BeforeStorage) throw std::bad_alloc{};
    auto data=std::make_unique<std::byte[]>(cap);
    if(inject==detail::CaptureAllocationStage::BeforeState) throw std::bad_alloc{};
    return std::make_shared<State>(std::move(data),cap);
  } catch(const std::bad_alloc&) { return CaptureFailure::AllocationFailed; }
}
}
NativeCaptureWriteGuard::NativeCaptureWriteGuard(std::shared_ptr<State> s) noexcept:state_(std::move(s)) {}
NativeCaptureWriteGuard::NativeCaptureWriteGuard(NativeCaptureWriteGuard&& other) noexcept:state_(std::move(other.state_)) {}
NativeCaptureWriteGuard::~NativeCaptureWriteGuard() {
  if(state_) {std::lock_guard lock(state_->mutex);if(!state_->writers) std::terminate();--state_->writers;}
}
CaptureStatus NativeCaptureWriteGuard::append(std::span<const std::byte> bytes) noexcept {
  if(!state_) return CaptureFailure::InvalidOwner;
  std::lock_guard lock(state_->mutex);
  if(state_->first) return *state_->first;
  if(state_->closed) return state_->latch(CaptureFailure::InvalidState);
  if(bytes.size()>state_->cap-state_->size) return state_->latch(CaptureFailure::CapacityExceeded);
  if(!bytes.empty()) std::memcpy(state_->bytes.get()+state_->size,bytes.data(),bytes.size());
  state_->size+=bytes.size();return std::monostate{};
}
CaptureStatus NativeCaptureWriteGuard::reject(CaptureFailure reason) noexcept {
  if(!state_) return CaptureFailure::InvalidOwner;
  std::lock_guard lock(state_->mutex);
  if(state_->first) return *state_->first;
  if(state_->closed || (reason!=CaptureFailure::CallbackFailure && reason!=CaptureFailure::Cancelled))
    return state_->latch(CaptureFailure::InvalidState);
  return state_->latch(reason);
}
CaptureResult<NativeCaptureWriteGuard> NativeCaptureWriter::acquire() const noexcept {
  if(!state_) return CaptureFailure::InvalidOwner;
  std::lock_guard lock(state_->mutex);
  if(state_->first) return *state_->first;
  if(state_->closed || state_->writers==std::numeric_limits<std::size_t>::max()) return state_->latch(CaptureFailure::InvalidState);
  ++state_->writers;return NativeCaptureWriteGuard{state_};
}
std::optional<CaptureFailure> NativeCaptureWriter::failure() const noexcept { return aws::failure(state_); }
NativeBodyCapture::NativeBodyCapture(std::shared_ptr<State> s) noexcept:state_(std::move(s)) {}
NativeBodyCapture::NativeBodyCapture(NativeBodyCapture&& other) noexcept {
  require_owner(other.state_);state_=std::move(other.state_);
}
NativeBodyCapture::~NativeBodyCapture() { abandon(state_); }
CaptureResult<NativeBodyCapture> NativeBodyCapture::create(std::size_t cap) noexcept {
  auto s=make_state(cap,std::nullopt);if(auto* f=std::get_if<CaptureFailure>(&s)) return *f;
  return NativeBodyCapture{std::get<std::shared_ptr<State>>(std::move(s))};
}
CaptureResult<NativeCaptureWriter> NativeBodyCapture::writer() const noexcept {
  if(!state_) return CaptureFailure::InvalidOwner;
  if(state_->creator!=std::this_thread::get_id()) return CaptureFailure::WrongThread;
  std::lock_guard lock(state_->mutex);
  if(state_->first) return *state_->first;
  if(state_->closed) return state_->latch(CaptureFailure::InvalidState);
  return NativeCaptureWriter{state_};
}
std::optional<CaptureFailure> NativeBodyCapture::failure() const noexcept { return aws::failure(state_); }
FrozenNativeBody::FrozenNativeBody(std::shared_ptr<State> s) noexcept:state_(std::move(s)) {}
FrozenNativeBody::FrozenNativeBody(FrozenNativeBody&& other) noexcept { require_owner(other.state_);state_=std::move(other.state_); }
FrozenNativeBody::~FrozenNativeBody() { abandon(state_); }
std::optional<CaptureFailure> FrozenNativeBody::failure() const noexcept { return aws::failure(state_); }
CaptureResult<std::span<const std::byte>> FrozenNativeBody::begin_view() const noexcept {
  if(!state_) return CaptureFailure::InvalidOwner;
  if(state_->creator!=std::this_thread::get_id()) return CaptureFailure::WrongThread;
  std::lock_guard lock(state_->mutex);if(state_->viewing) std::terminate();
  if(!state_->frozen) return CaptureFailure::InvalidState;
  state_->viewing=true;return std::span<const std::byte>{state_->bytes.get(),state_->size};
}
void FrozenNativeBody::end_view() const noexcept {
  if(!state_ || state_->creator!=std::this_thread::get_id()) std::terminate();
  std::lock_guard lock(state_->mutex);if(!state_->viewing) std::terminate();state_->viewing=false;
}
CaptureStatus FrozenNativeBody::read(std::size_t offset,std::span<std::byte> output) const noexcept {
  auto view=begin_view();if(auto* f=std::get_if<CaptureFailure>(&view)) return *f;
  const auto bytes=std::get<std::span<const std::byte>>(view);
  struct End { const FrozenNativeBody& owner;~End(){owner.end_view();} } end{*this};
  if(offset>bytes.size() || output.size()>bytes.size()-offset) return CaptureFailure::InvalidRange;
  if(!output.empty()) { std::memcpy(output.data(),bytes.data()+offset,output.size()); }
  return std::monostate{};
}
NativeCaptureHandoff::NativeCaptureHandoff(std::shared_ptr<State> s) noexcept:state_(std::move(s)) {}
NativeCaptureHandoff::NativeCaptureHandoff(NativeCaptureHandoff&& other) noexcept {require_owner(other.state_);state_=std::move(other.state_);}
NativeCaptureHandoff::~NativeCaptureHandoff() {abandon(state_);}
CaptureResult<FrozenNativeBody> NativeCaptureHandoff::freeze() noexcept {
  if(!state_) return CaptureFailure::InvalidOwner;
  if(state_->creator!=std::this_thread::get_id()) return CaptureFailure::WrongThread;
  auto local=std::move(state_);std::lock_guard lock(local->mutex);
  local->closed=true;
  if(local->writers) local->latch(CaptureFailure::MissingQuiescence);
  if(local->first) {local->wipe();return *local->first;}
  local->frozen=true;return FrozenNativeBody{std::move(local)};
}
CaptureResult<NativeBodyCapture> detail::NativeCaptureTestAccess::create_failing(std::size_t cap,CaptureAllocationStage stage) noexcept {
  if(stage!=CaptureAllocationStage::BeforeStorage && stage!=CaptureAllocationStage::BeforeState) return CaptureFailure::InvalidState;
  auto s=make_state(cap,stage);if(auto* f=std::get_if<CaptureFailure>(&s)) return *f;
  return NativeBodyCapture{std::get<std::shared_ptr<State>>(std::move(s))};
}
CaptureResult<NativeCaptureHandoff> detail::NativeCaptureTestAccess::handoff(NativeBodyCapture&& owner) noexcept {
  if(!owner.state_) return CaptureFailure::InvalidOwner;
  if(owner.state_->creator!=std::this_thread::get_id()) return CaptureFailure::WrongThread;
  require_owner(owner.state_);return NativeCaptureHandoff{std::move(owner.state_)};
}
std::optional<std::size_t> detail::NativeCaptureTestAccess::written_size(const NativeBodyCapture& owner) noexcept {
  if(!owner.state_ || owner.state_->creator!=std::this_thread::get_id()) return std::nullopt;
  std::lock_guard lock(owner.state_->mutex);return owner.state_->size;
}
} // namespace rs::core::auth::aws
