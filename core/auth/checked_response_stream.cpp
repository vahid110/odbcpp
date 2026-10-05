#include "core/auth/checked_response_stream.h"
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <streambuf>
#include <type_traits>
namespace rs::core::auth {
namespace {
void wipe(std::byte* p,std::size_t size) noexcept {
  volatile std::byte* bytes=p;for(std::size_t n=0;n<size;++n) { bytes[n]=std::byte{0}; }
}
bool terminal(BoundaryFailure f) noexcept {
  switch(f) {
    case BoundaryFailure::ReadFailed: case BoundaryFailure::ClockRollback:
    case BoundaryFailure::DeadlineElapsed: case BoundaryFailure::Cancelled:
    case BoundaryFailure::Overflow: case BoundaryFailure::StaleObservation:
    case BoundaryFailure::SourceClosed: case BoundaryFailure::SourceChanged:
    case BoundaryFailure::UnsafeDiagnostic: case BoundaryFailure::InvalidDiagnostic:
    case BoundaryFailure::StaleDiagnostic: case BoundaryFailure::ForeignDispatch:
    case BoundaryFailure::BodyRejected: return true;
    case BoundaryFailure::InvalidBoundary: case BoundaryFailure::InvalidInput:
    case BoundaryFailure::InvalidState: case BoundaryFailure::WrongOperation:
    case BoundaryFailure::WrongThread: case BoundaryFailure::UnknownQuality:
    case BoundaryFailure::Borrowed: case BoundaryFailure::AllocationFailed: return false;
  }
  std::terminate();
}
BoundaryFailure reject(TransportBoundaryLease& lease) noexcept {
  const auto refused=lease.reject_body();const auto* f=std::get_if<BoundaryFailure>(&refused);
  if(!f || !terminal(*f)) { std::terminate(); }
  const auto confirmed=lease.check();const auto* again=std::get_if<BoundaryFailure>(&confirmed);
  if(!again || *again!=*f) { std::terminate(); }
  return *f;
}
// Remains outside potentially throwing construction and return movements.
// On exceptional return, success cannot suppress rejection. No clock callback.
class RejectionGuard final {
 public:
  explicit RejectionGuard(TransportBoundaryLease&& supplied) noexcept
      : lease(std::move(supplied)),exceptions(std::uncaught_exceptions()) {}
  ~RejectionGuard() {
    if(lease.request() && (!success || std::uncaught_exceptions()>exceptions)) { (void)reject(lease); }
  }
  TransportBoundaryLease lease;bool success{};
 private:
  const int exceptions;
};
}
namespace detail {
class CheckedResponseBuffer final : public std::streambuf {
 public:
  CheckedResponseBuffer(std::unique_ptr<std::byte[]> supplied,std::size_t size) noexcept
      : bytes(std::move(supplied)),cap(size) {}
  ~CheckedResponseBuffer() override { if(bytes) { wipe(bytes.get(),cap); } }
  void attach(TransportBoundaryLease&& supplied) noexcept { lease.emplace(std::move(supplied)); }
  bool gate() noexcept {
    if(fault) { return false; }
    if(!lease) { std::terminate(); }
    const auto checked=lease->check();
    if(const auto* f=std::get_if<BoundaryFailure>(&checked)) {
      fault.emplace(*f);if(bytes) { wipe(bytes.get(),cap); }return false;
    }
    return true;
  }
  bool fail(StreamFailure local) noexcept {
    if(!fault) {
      if(!lease || !lease->request()) { std::terminate(); }
      fault.emplace(local,reject(*lease));if(bytes) { wipe(bytes.get(),cap); }
    }
    return false;
  }
  bool current_eof() const noexcept { return eof_observed && read==written; }
  std::unique_ptr<std::byte[]> bytes;
  const std::size_t cap;std::size_t written{};
  std::optional<TransportBoundaryLease> lease;
  std::optional<CheckedStreamError> fault;
 protected:
  std::streamsize xsputn(const char* p,std::streamsize n) override {
    if(!gate()) { return 0; }
    if(n<0 || static_cast<std::uintmax_t>(n)>cap-written) { fail(StreamFailure::LimitExceeded);return 0; }
    if(n>0) { eof_observed=false;std::memcpy(bytes.get()+written,p,static_cast<std::size_t>(n)); }
    written+=static_cast<std::size_t>(n);return n;
  }
  int_type overflow(int_type c) override {
    if(!gate()) { return traits_type::eof(); }
    if(traits_type::eq_int_type(c,traits_type::eof())) { return traits_type::not_eof(c); }
    const char value=traits_type::to_char_type(c);return xsputn(&value,1)==1?c:traits_type::eof();
  }
  int sync() override { return gate()?0:-1; }
  int_type underflow() override {
    if(!gate()) { return traits_type::eof(); }
    if(read==written) { eof_observed=true;return traits_type::eof(); }
    eof_observed=false;return traits_type::to_int_type(static_cast<char>(bytes[read]));
  }
  int_type uflow() override {
    const auto c=underflow();if(!traits_type::eq_int_type(c,traits_type::eof())) { ++read; }return c;
  }
  std::streamsize xsgetn(char* p,std::streamsize n) override {
    if(!gate()) { return 0; }
    if(n<0) { fail(StreamFailure::StreamFailed);return 0; }
    eof_observed=false;const auto available=written-read;
    const auto amount=static_cast<std::uintmax_t>(n)>available?available:static_cast<std::size_t>(n);
    if(amount) { std::memcpy(p,bytes.get()+read,amount); }read+=amount;
    if(static_cast<std::uintmax_t>(n)>amount) { eof_observed=true; }
    return static_cast<std::streamsize>(amount);
  }
  pos_type seekoff(off_type off,std::ios_base::seekdir way,std::ios_base::openmode mode) override {
    if(!gate()) { return pos_type(off_type(-1)); }
    if(mode==std::ios_base::out && way==std::ios_base::cur && off==0) { return pos_type(static_cast<off_type>(written)); }
    if(mode!=std::ios_base::in) { return bad_seek(); }
    off_type base;
    if(way==std::ios_base::beg) { base=0; }
    else if(way==std::ios_base::cur) { base=static_cast<off_type>(read); }
    else if(way==std::ios_base::end) { base=static_cast<off_type>(written); }
    else { return bad_seek(); }
    if((off>0 && base>std::numeric_limits<off_type>::max()-off) ||
       (off<0 && base<std::numeric_limits<off_type>::min()-off)) { return bad_seek(); }
    const off_type next=base+off;
    if(next<0 || static_cast<std::uintmax_t>(next)>written) { return bad_seek(); }
    read=static_cast<std::size_t>(next);eof_observed=false;return pos_type(next);
  }
  pos_type seekpos(pos_type pos,std::ios_base::openmode mode) override {
    return seekoff(static_cast<off_type>(pos),std::ios_base::beg,mode);
  }
 private:
  pos_type bad_seek() noexcept { fail(StreamFailure::InvalidSeek);return pos_type(off_type(-1)); }
  std::size_t read{};bool eof_observed{};
};
class CheckedResponseStreamState final : public std::iostream {
 public:
  CheckedResponseStreamState(std::unique_ptr<std::byte[]> storage,std::size_t cap)
      : std::iostream(nullptr),buffer(std::move(storage),cap) { rdbuf(&buffer); }
  ~CheckedResponseStreamState() override {
    if(!consumed && buffer.lease && buffer.lease->request()) { (void)reject(*buffer.lease); }
  }
  CheckedResponseBuffer buffer;bool consumed{};
};
}
CheckedStreamOwner::CheckedStreamOwner(std::unique_ptr<detail::CheckedResponseStreamState> state) noexcept : state_(std::move(state)) {}
CheckedStreamOwner::CheckedStreamOwner(CheckedStreamOwner&&) noexcept = default;
CheckedStreamOwner::~CheckedStreamOwner() = default;
std::iostream* CheckedStreamOwner::io() noexcept { return state_.get(); }
std::optional<CheckedStreamError> CheckedStreamOwner::failure() const noexcept {
  return state_?state_->buffer.fault:std::optional{CheckedStreamError{StreamFailure::InvalidOwner}};
}
CheckedStreamResult<CheckedStreamOwner> CheckedStreamOwner::create(std::size_t cap,TransportBoundaryLease&& lease) {
  return create_impl(cap,std::move(lease),{});
}
CheckedStreamResult<CheckedStreamOwner> CheckedStreamOwner::create_impl(std::size_t cap,
    TransportBoundaryLease&& supplied,std::optional<detail::CheckedConstructionFailure> injected) {
  RejectionGuard guard(std::move(supplied));
  if(!guard.lease.request()) { return CheckedStreamError{BoundaryFailure::InvalidBoundary}; }
  const auto status=guard.lease.check();
  if(const auto* f=std::get_if<BoundaryFailure>(&status)) { return CheckedStreamError{*f}; }
  if(cap==0 || cap>StreamOwner::max_bytes || cap>static_cast<std::uintmax_t>(std::numeric_limits<std::streamoff>::max())) {
    return CheckedStreamError{StreamFailure::InvalidLimit,reject(guard.lease)};
  }
  try {
    if(injected==detail::CheckedConstructionFailure::BeforeStorage) { throw std::bad_alloc{}; }
    auto storage=std::make_unique<std::byte[]>(cap);
    if(injected==detail::CheckedConstructionFailure::BeforeState) { throw std::bad_alloc{}; }
    auto state=std::make_unique<detail::CheckedResponseStreamState>(std::move(storage),cap);
    if(injected==detail::CheckedConstructionFailure::AfterState) { throw std::bad_alloc{}; }
    state->buffer.attach(std::move(guard.lease));
    return CheckedStreamOwner(std::move(state));
  } catch(...) { return CheckedStreamError{StreamFailure::AllocationFailed,reject(guard.lease)}; }
}
CheckedStreamResult<CheckedStreamOwner> detail::CheckedStreamTestAccess::refuse_construction(
    std::size_t cap,TransportBoundaryLease&& lease,CheckedConstructionFailure step) {
  return CheckedStreamOwner::create_impl(cap,std::move(lease),step);
}
ResponseBytes CheckedStreamOwner::take_body() {
  auto& b=state_->buffer;return ResponseBytes(std::move(b.bytes),b.cap,b.written);
}
CheckedStreamResult<ResponseBytes> seal_checked_response(CheckedStreamOwner&& supplied) {
  CheckedStreamOwner owner(std::move(supplied));
  if(!owner.state_) { return CheckedStreamError{StreamFailure::AlreadyConsumed}; }
  auto& state=*owner.state_;
  if(!state.buffer.gate()) { return *state.buffer.fault; }
  if(state.bad() || (state.fail() && !(state.eof() && state.buffer.current_eof()))) {
    state.buffer.fail(StreamFailure::StreamFailed);return *state.buffer.fault;
  }
  RejectionGuard guard(std::move(*state.buffer.lease));
  // Guard remains usable through ResponseBytes construction/variant moves.
  // Newly constructed bytes have no active with_bytes borrower. Their existing
  // move is NOT blanket noexcept; an exceptional result return poisons via guard.
  try {
    CheckedStreamResult<ResponseBytes> result(owner.take_body());
    state.consumed=true;owner.state_.reset();guard.success=true;return result;
  } catch(...) { return CheckedStreamError{StreamFailure::AllocationFailed,reject(guard.lease)}; }
}
static_assert(std::is_nothrow_move_constructible_v<TransportBoundaryLease>);
static_assert(std::is_nothrow_move_constructible_v<CheckedStreamOwner>);
static_assert(std::is_nothrow_move_constructible_v<CheckedStreamResult<CheckedStreamOwner>>);
static_assert(std::is_nothrow_constructible_v<CheckedStreamResult<CheckedStreamOwner>,CheckedStreamOwner&&>);
}
