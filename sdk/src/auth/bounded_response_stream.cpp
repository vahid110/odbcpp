#include "odbcpp/auth/bounded_response_stream.h"
#include <cstring>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <streambuf>

namespace rs::core::auth {
namespace {
void wipe_bytes(std::byte* bytes, std::size_t n) noexcept {
  volatile std::byte* p = bytes;
  for (std::size_t i = 0; i < n; ++i) p[i] = std::byte{0};
}
}
std::string_view stream_safe_message(StreamFailure reason) noexcept {
  switch (reason) {
    case StreamFailure::InvalidLimit: return "Invalid response limit";
    case StreamFailure::InvalidDeadline: return "Invalid response deadline";
    case StreamFailure::InvalidOwner: return "Invalid response owner";
    case StreamFailure::LimitExceeded: return "Response limit exceeded";
    case StreamFailure::InvalidSeek: return "Invalid response seek";
    case StreamFailure::ClockRollback: return "Response clock rollback";
    case StreamFailure::DeadlineElapsed: return "Response deadline elapsed";
    case StreamFailure::Cancelled: return "Response cancelled";
    case StreamFailure::AllocationFailed: return "Response allocation failed";
    case StreamFailure::StreamFailed: return "Response stream failed";
    case StreamFailure::AlreadyConsumed: return "Response already consumed";
  }
  return "Invalid response failure";
}
void ResponseBytes::throw_invalid() { throw std::logic_error("Invalid response consumption"); }
ResponseBytes::ResponseBytes(ResponseBytes&& other) {
  if (other.consuming_) throw_invalid();
  bytes_ = std::move(other.bytes_); cap_ = other.cap_; size_ = other.size_;
  owns_ = other.owns_; other.cap_ = 0; other.size_ = 0; other.owns_ = false;
}
void ResponseBytes::wipe() noexcept {
  if (bytes_) wipe_bytes(bytes_.get(), cap_);
}
ResponseBytes::~ResponseBytes() { wipe(); }
bool ResponseBytes::clear() noexcept {
  if (consuming_) return false;
  wipe(); bytes_.reset(); cap_ = 0; size_ = 0; owns_ = false; return true;
}
namespace detail {
class ResponseBuffer final : public std::streambuf {
 public:
  ResponseBuffer(std::size_t limit, rs::util::Deadline deadline,
      MonotonicClock& clock, const Cancellation* cancel, rs::util::Deadline now)
      : bytes(std::make_unique<std::byte[]>(limit)), cap(limit), deadline_(deadline),
        highwater_(now), clock_(clock), cancel_(cancel) {}
  ~ResponseBuffer() override { if (bytes) wipe_bytes(bytes.get(), cap); }
  bool gate() noexcept {
    if (fault) return false;
    if (cancel_ && cancel_->stop_requested()) return fail(StreamFailure::Cancelled);
    const auto now = clock_.now();
    if (now < highwater_) return fail(StreamFailure::ClockRollback);
    highwater_ = now;
    if (now >= deadline_) return fail(StreamFailure::DeadlineElapsed);
    return true;
  }
  bool fail(StreamFailure reason) noexcept {
    if (!fault) { fault = reason; if (bytes) wipe_bytes(bytes.get(), cap); }
    return false;
  }
  std::unique_ptr<std::byte[]> bytes;
  const std::size_t cap;
  std::size_t written{0};
  bool eof_observed{false};
  bool current_eof() const noexcept { return eof_observed && read_ == written; }
  std::optional<StreamFailure> fault;
 protected:
  std::streamsize xsputn(const char* p, std::streamsize n) override {
    if (!gate()) return 0;
    if (n < 0 || static_cast<std::uintmax_t>(n) > cap - written) {
      fail(StreamFailure::LimitExceeded); return 0;
    }
    if (n > 0) {
      eof_observed = false;
      std::memcpy(bytes.get() + written, p, static_cast<std::size_t>(n));
    }
    written += static_cast<std::size_t>(n); return n;
  }
  int_type overflow(int_type c) override {
    if (!gate()) return traits_type::eof();
    if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
    const char value = traits_type::to_char_type(c);
    return xsputn(&value, 1) == 1 ? c : traits_type::eof();
  }
  int sync() override { return gate() ? 0 : -1; }
  int_type underflow() override {
    if (!gate()) return traits_type::eof();
    if (read_ == written) { eof_observed = true; return traits_type::eof(); }
    eof_observed = false;
    return traits_type::to_int_type(static_cast<char>(bytes[read_]));
  }
  int_type uflow() override {
    const auto c = underflow();
    if (!traits_type::eq_int_type(c, traits_type::eof())) ++read_;
    return c;
  }
  std::streamsize xsgetn(char* p, std::streamsize n) override {
    if (!gate()) return 0;
    if (n < 0) { fail(StreamFailure::StreamFailed); return 0; }
    eof_observed = false;
    const auto available = written - read_;
    const auto amount = static_cast<std::uintmax_t>(n) > available ? available : static_cast<std::size_t>(n);
    if (amount) std::memcpy(p, bytes.get() + read_, amount);
    read_ += amount;
    if (static_cast<std::uintmax_t>(n) > amount) eof_observed = true;
    return static_cast<std::streamsize>(amount);
  }
  pos_type seekoff(off_type off, std::ios_base::seekdir way,
                   std::ios_base::openmode mode) override {
    if (!gate()) return pos_type(off_type(-1));
    if (mode == std::ios_base::out && way == std::ios_base::cur && off == 0)
      return pos_type(static_cast<off_type>(written));
    if (mode != std::ios_base::in) return bad_seek();
    off_type base;
    if (way == std::ios_base::beg) base = 0;
    else if (way == std::ios_base::cur) base = static_cast<off_type>(read_);
    else if (way == std::ios_base::end) base = static_cast<off_type>(written);
    else return bad_seek();
    if ((off > 0 && base > std::numeric_limits<off_type>::max() - off) ||
        (off < 0 && base < std::numeric_limits<off_type>::min() - off)) return bad_seek();
    const off_type next = base + off;
    if (next < 0 || static_cast<std::uintmax_t>(next) > written) return bad_seek();
    read_ = static_cast<std::size_t>(next); eof_observed = false; return pos_type(next);
  }
  pos_type seekpos(pos_type pos, std::ios_base::openmode mode) override {
    return seekoff(static_cast<off_type>(pos), std::ios_base::beg, mode);
  }
 private:
  pos_type bad_seek() noexcept { fail(StreamFailure::InvalidSeek); return pos_type(off_type(-1)); }
  std::size_t read_{0};
  rs::util::Deadline deadline_, highwater_;
  MonotonicClock& clock_;
  const Cancellation* cancel_;
};
class ResponseStreamState final : public std::iostream {
 public:
  ResponseStreamState(std::size_t cap, rs::util::Deadline deadline,
      MonotonicClock& clock, const Cancellation* cancel, rs::util::Deadline now)
      : std::iostream(nullptr), buffer(cap, deadline, clock, cancel, now) { rdbuf(&buffer); }
  ResponseBuffer buffer;
};
}
StreamOwner::StreamOwner(std::unique_ptr<detail::ResponseStreamState> state) : state_(std::move(state)) {}
StreamOwner::StreamOwner(StreamOwner&&) noexcept = default;
StreamOwner::~StreamOwner() = default;
std::iostream* StreamOwner::io() noexcept { return state_.get(); }
std::optional<StreamFailure> StreamOwner::failure() const noexcept {
  return state_ ? state_->buffer.fault : std::optional{StreamFailure::InvalidOwner};
}
StreamResult<StreamOwner> StreamOwner::create(std::size_t cap,
    rs::util::Deadline deadline, MonotonicClock& clock, const Cancellation* cancel) {
  if (cap == 0 || cap > max_bytes || cap > static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max()))
    return StreamFailure::InvalidLimit;
  if (deadline == rs::util::Deadline::max()) return StreamFailure::InvalidDeadline;
  if (cancel && cancel->stop_requested()) return StreamFailure::Cancelled;
  const auto now = clock.now();
  if (now >= deadline) return StreamFailure::DeadlineElapsed;
  try {
    auto state = std::make_unique<detail::ResponseStreamState>(cap, deadline, clock, cancel, now);
    if (!state->buffer.gate()) return *state->buffer.fault;
    return StreamOwner(std::move(state));
  } catch (...) { return StreamFailure::AllocationFailed; }
}
StreamResult<ResponseBytes> seal_response(StreamOwner&& supplied) {
  StreamOwner owner(std::move(supplied));
  if (!owner.state_) return StreamFailure::AlreadyConsumed;
  auto& state = *owner.state_;
  if (!state.buffer.gate()) return *state.buffer.fault;
  if (state.bad() || (state.fail() && !(state.eof() && state.buffer.current_eof()))) {
    state.buffer.fail(StreamFailure::StreamFailed); return StreamFailure::StreamFailed;
  }
  return ResponseBytes(std::move(state.buffer.bytes), state.buffer.cap, state.buffer.written);
}
} // namespace rs::core::auth
