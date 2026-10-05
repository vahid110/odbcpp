#include "tests/support/pg_staged_refusal_observer.h"
#include <utility>

namespace rs::tests::support {
using core::transport::IOResult;
using util::DbErrorCode;
util::Result<std::unique_ptr<PgStagedRefusalObserver>> PgStagedRefusalObserver::create(
    std::unique_ptr<core::transport::ITransport> delegate) {
  if (!delegate || !dynamic_cast<core::transport::IStartTlsTransport*>(delegate.get()) ||
      !dynamic_cast<core::transport::ITlsConfigurableTransport*>(delegate.get())) {
    return {DbErrorCode::TLSError, "Required synchronous TLS transport extensions missing"};
  }
  return std::unique_ptr<PgStagedRefusalObserver>(new PgStagedRefusalObserver(std::move(delegate)));
}
PgStagedRefusalObserver::PgStagedRefusalObserver(std::unique_ptr<core::transport::ITransport> d)
    : delegate_(std::move(d)),
      tls_(dynamic_cast<core::transport::IStartTlsTransport*>(delegate_.get())),
      configuration_(dynamic_cast<core::transport::ITlsConfigurableTransport*>(delegate_.get())) {}
void PgStagedRefusalObserver::fail(Fault fault) noexcept {
  if (state_ == State::Fault) return;
  state_ = State::Fault; summary_.verified = false; summary_.fault = fault;
  outgoing_.clear(); incoming_.clear();
}
void PgStagedRefusalObserver::phase_call() noexcept {
  if (state_ == State::Armed) fail(Fault::Lifecycle);
}
void PgStagedRefusalObserver::check_deadline(util::Deadline d) noexcept {
  if (state_ == State::Armed && (d != deadline_ || util::Clock::now() >= deadline_))
    fail(Fault::Deadline);
}
bool PgStagedRefusalObserver::arm(util::Deadline d) noexcept {
  if (state_ != State::FreshUnarmed) { fail(Fault::Lifecycle); return false; }
  if (!tls_->peer_identity_verified()) { fail(Fault::Peer); return false; }
  if (util::Clock::now() >= d) { fail(Fault::Deadline); return false; }
  deadline_ = d; state_ = State::Armed; return true;
}
PgStagedRefusalObserver::Summary PgStagedRefusalObserver::seal() noexcept {
  if (state_ == State::Sealed || state_ == State::Fault) return summary_;
  if (state_ != State::Armed) { fail(Fault::Lifecycle); return summary_; }
  check_deadline(deadline_);
  if (state_ == State::Fault) return summary_;
  if (!outgoing_.empty() || !incoming_.empty() || out_phase_ != 3 || in_phase_ != 5) {
    fail(Fault::Incomplete); return summary_;
  }
  state_ = State::Sealed; summary_.verified = true;
  outgoing_.clear(); incoming_.clear(); return summary_;
}
util::Result<void> PgStagedRefusalObserver::connect(std::string_view h,std::uint16_t p,util::Deadline d) {
  phase_call(); return delegate_->connect(h,p,d);
}
util::Result<void> PgStagedRefusalObserver::connect_plain(std::string_view h,std::uint16_t p,util::Deadline d) {
  phase_call(); return tls_->connect_plain(h,p,d);
}
util::Result<void> PgStagedRefusalObserver::upgrade_to_tls(std::string_view h,util::Deadline d) {
  phase_call(); return tls_->upgrade_to_tls(h,d);
}
void PgStagedRefusalObserver::set_ca_locations(const std::string& f,const std::string& d) {
  phase_call(); configuration_->set_ca_locations(f,d);
}
bool PgStagedRefusalObserver::peer_identity_verified() noexcept {
  const bool verified = tls_->peer_identity_verified();
  if (state_ == State::Armed && !verified) fail(Fault::Peer);
  return verified;
}
void PgStagedRefusalObserver::close() noexcept { phase_call(); delegate_->close(); }
util::Result<IOResult> PgStagedRefusalObserver::send(std::span<const std::byte> b,util::Deadline d) {
  check_deadline(d);
  try {
    auto result = delegate_->send(b,d);
    check_deadline(d);
    if (state_ == State::Armed) {
      if (result.has_error()) fail(Fault::Transport);
      else if (!result->n || result->n > b.size() || result->eof) fail(Fault::Progress);
      else observe(b.first(result->n),true);
    }
    return result;
  } catch (...) { if (state_ == State::Armed) fail(Fault::Transport); throw; }
}
util::Result<IOResult> PgStagedRefusalObserver::recv(std::span<std::byte> b,util::Deadline d) {
  check_deadline(d);
  try {
    auto result = delegate_->recv(b,d);
    check_deadline(d);
    if (state_ == State::Armed) {
      if (result.has_error()) fail(Fault::Transport);
      else if (!result->n || result->n > b.size() || result->eof) fail(Fault::Progress);
      else observe(b.first(result->n),false);
    }
    return result;
  } catch (...) { if (state_ == State::Armed) fail(Fault::Transport); throw; }
}
void PgStagedRefusalObserver::observe(std::span<const std::byte> b,bool outgoing) noexcept {
  const auto used = summary_.sent_bytes + summary_.received_bytes;
  if (b.size() > max_bytes - used) { fail(Fault::Bounds); return; }
  (outgoing ? summary_.sent_bytes : summary_.received_bytes) += b.size();
  for (const auto byte : b) {
    if (state_ != State::Armed) break;
    ++processed_; feed(byte,outgoing);
  }
}
void PgStagedRefusalObserver::feed(std::byte byte,bool outgoing) noexcept {
  auto& f = outgoing ? outgoing_ : incoming_;
  if (f.header_used < 5) {
    f.header[f.header_used++] = byte;
    if (f.header_used == 5) { header_ready(f,outgoing); if (state_ == State::Armed && !f.body_left) complete(f,outgoing); }
    return;
  }
  // Only the bounded parameter-count and Ready byte are retained, never IDs/text.
  if (!outgoing && ((f.tag == 't' && f.body_seen < 2) || (f.tag == 'Z' && f.body_seen == 0)))
    f.prefix[f.body_seen] = byte;
  ++f.body_seen; --f.body_left;
  if (!f.body_left) complete(f,outgoing);
}
void PgStagedRefusalObserver::header_ready(Frame& f,bool outgoing) noexcept {
  f.tag = static_cast<char>(std::to_integer<unsigned char>(f.header[0]));
  for (std::size_t i=1;i<5;++i) f.length=(f.length<<8)|std::to_integer<unsigned char>(f.header[i]);
  if (f.length < 4 || f.length > max_bytes || f.length-4 > max_bytes-processed_ ||
      summary_.sent_frames+summary_.received_frames >= max_frames) { fail(Fault::Bounds); return; }
  f.body_left=f.length-4;
  if (outgoing) {
    const bool valid = (out_phase_==0 && f.tag=='P' && f.length>=8) ||
        (out_phase_==1 && f.tag=='D' && f.length==6) ||
        (out_phase_==2 && f.tag=='S' && f.length==4);
    if (!valid) fail(Fault::Phase);
    return;
  }
  if (in_phase_==5) { fail(Fault::Phase); return; }
  if (f.tag=='N' || f.tag=='S' || f.tag=='A') return;
  if (f.tag=='E') { if (in_phase_==4) fail(Fault::Phase); return; }
  const bool valid = (in_phase_==0 && f.tag=='1' && f.length==4) ||
      (in_phase_==1 && f.tag=='t' && f.length>=6) ||
      (in_phase_==2 && f.tag=='T' && f.length>=6) ||
      (in_phase_==2 && f.tag=='n' && f.length==4) ||
      ((in_phase_==3 || in_phase_==4) && f.tag=='Z' && f.length==5);
  if (!valid) fail(Fault::Phase);
}
void PgStagedRefusalObserver::complete(Frame& f,bool outgoing) noexcept {
  if (outgoing) {
    ++summary_.sent_frames; ++out_phase_;
    if (f.tag=='P') ++summary_.parses;
    else if (f.tag=='D') ++summary_.describes;
    else ++summary_.syncs;
  } else {
    ++summary_.received_frames;
    if (f.tag=='1') in_phase_=1;
    else if (f.tag=='t') {
      const auto count=(std::to_integer<unsigned>(f.prefix[0])<<8)|std::to_integer<unsigned>(f.prefix[1]);
      if (count>8 || count!=1 || f.length!=6+4*count) { fail(Fault::Frame); return; }
      summary_.parameter_count=count; in_phase_=2;
    } else if (f.tag=='T' || f.tag=='n') in_phase_=3;
    else if (f.tag=='E') in_phase_=4;
    else if (f.tag=='Z') {
      if (f.prefix[0]!=std::byte{'I'}) { fail(Fault::Phase); return; }
      summary_.ready_idle=true;
      summary_.completion=in_phase_==4 ? Completion::ErrorIdle : Completion::DescriptionIdle;
      in_phase_=5;
    }
  }
  f.clear();
}
} // namespace rs::tests::support
