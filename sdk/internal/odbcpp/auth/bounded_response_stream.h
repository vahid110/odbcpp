#pragma once
#include "odbcpp/auth/auth_core.h"
#include <iostream>
#include <memory>
#include <span>
#include <variant>

namespace rs::core::auth {
enum class StreamFailure {
  InvalidLimit, InvalidDeadline, InvalidOwner, LimitExceeded, InvalidSeek,
  ClockRollback, DeadlineElapsed, Cancelled, AllocationFailed, StreamFailed,
  AlreadyConsumed
};
std::string_view stream_safe_message(StreamFailure) noexcept;
template<class T> using StreamResult = std::variant<T, StreamFailure>;
namespace detail { class ResponseStreamState; }

// Separate body owner; does not widen SecretBytes or establish credential validity.
// Thread-confined, scoped views must not escape. Wiping is best effort and cannot
// cleanse caller/transport/consumer copies, registers, swap or crash dumps.
class ResponseBytes final {
 public:
  ResponseBytes(const ResponseBytes&) = delete;
  ResponseBytes& operator=(const ResponseBytes&) = delete;
  ResponseBytes(ResponseBytes&&);
  ResponseBytes& operator=(ResponseBytes&&) = delete;
  ~ResponseBytes();
  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  bool clear() noexcept;
  template<class Consumer> void with_bytes(Consumer&& consume) const {
    if (!owns_ || consuming_) throw_invalid();
    consuming_ = true;
    struct Guard { bool& flag; ~Guard() { flag = false; } } guard{consuming_};
    std::invoke(std::forward<Consumer>(consume),
                std::span<const std::byte>{bytes_.get(), size_});
  }
 private:
  friend class StreamOwner;
  friend class CheckedStreamOwner;
  friend StreamResult<ResponseBytes> seal_response(class StreamOwner&&);
  ResponseBytes(std::unique_ptr<std::byte[]> bytes, std::size_t cap, std::size_t size)
      : bytes_(std::move(bytes)), cap_(cap), size_(size) {}
  static void throw_invalid();
  void wipe() noexcept;
  std::unique_ptr<std::byte[]> bytes_;
  std::size_t cap_{0}, size_{0};
  bool owns_{true};
  mutable bool consuming_{false};
};
// Pure append-only request-local sink, no SDK retained-ownership support.
// Borrowed io()/rdbuf pointers expire on seal/destruction; owner move preserves
// their live address. Escaped dangling borrows cannot be revoked by C++.
// Observers must outlive the owner; all operations are thread-confined.
class StreamOwner final {
 public:
  static constexpr std::size_t max_bytes = 131072; // Local policy, not AWS limit.
  static StreamResult<StreamOwner> create(std::size_t cap,
      rs::util::Deadline original_deadline, MonotonicClock&, const Cancellation*);
  StreamOwner(const StreamOwner&) = delete;
  StreamOwner& operator=(const StreamOwner&) = delete;
  StreamOwner(StreamOwner&&) noexcept;
  StreamOwner& operator=(StreamOwner&&) = delete;
  ~StreamOwner();
  std::iostream* io() noexcept;
  std::optional<StreamFailure> failure() const noexcept;
 private:
  friend StreamResult<ResponseBytes> seal_response(StreamOwner&&);
  explicit StreamOwner(std::unique_ptr<detail::ResponseStreamState> state);
  std::unique_ptr<detail::ResponseStreamState> state_;
};
// Seal accepts only currently observed end-of-input, not stale EOF history.
// Caller-forged flags cleared before seal cannot be historically detected.
// Moves into local ownership before refusal. Empty success is valid here only;
// it is not a valid service envelope. No reset or retry reuse is supported.
StreamResult<ResponseBytes> seal_response(StreamOwner&&);
} // namespace rs::core::auth
