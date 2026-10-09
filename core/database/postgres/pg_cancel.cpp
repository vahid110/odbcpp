#include "pg_database_connection.h"
#include "odbcpp/security/crypto.h"
#include "odbcpp/transport/start_tls_transport.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace rs::core::database::postgres {
namespace {
using rs::util::Deadline;
using rs::core::transport::ITransport;
using rs::core::transport::IServerCancelTransport;
struct ProtectedKey {
  std::array<std::byte, 8> bytes{};
  explicit ProtectedKey(const std::array<std::byte, 8>& source) : bytes(source) {}
  ~ProtectedKey() { rs::core::security::secure_cleanse(std::span<unsigned char>(
      reinterpret_cast<unsigned char*>(bytes.data()), bytes.size())); }
  ProtectedKey(const ProtectedKey&) = delete;
  ProtectedKey& operator=(const ProtectedKey&) = delete;
};

// Narrow cancellation eligibility, not a SQL translator: one nonempty command,
// optional trailing delimiter/comments. Mirrors the existing PostgreSQL marker
// scanner's quoted strings/identifiers, dollar tags and nested comments. Unknown
// or unterminated forms refuse cancellation without refusing ordinary execution.
static bool cancellation_single_statement(std::string_view sql) noexcept {
  const auto identifier = [](char ch) { const auto c=static_cast<unsigned char>(ch);
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='$'||c>=128; };
  bool content=false, terminated=false;
  for (std::size_t i=0;i<sql.size();) {
    const char ch=sql[i], next=i+1<sql.size()?sql[i+1]:'\0';
    if (ch==' '||ch=='\t'||ch=='\n'||ch=='\r'||ch=='\f'||ch=='\v') {++i;continue;}
    if(ch=='-'&&next=='-') {i+=2;while(i<sql.size()&&sql[i]!='\r'&&sql[i]!='\n')++i;continue;}
    if(ch=='/'&&next=='*') {
      std::size_t depth=1;i+=2;
      while(i<sql.size()&&depth) {
        if(i+1<sql.size()&&sql[i]=='/'&&sql[i+1]=='*'){++depth;i+=2;}
        else if(i+1<sql.size()&&sql[i]=='*'&&sql[i+1]=='/'){--depth;i+=2;}
        else ++i;
      }
      if (depth) return false;
      continue;
    }
    if(terminated)return false;
    if(ch==';'){if(!content)return false;terminated=true;++i;continue;}
    content=true;
    if(ch=='\''||ch=='"') {
      const bool escape=ch=='\''&&i>0&&(sql[i-1]=='e'||sql[i-1]=='E')&&(i==1||!identifier(sql[i-2]));
      const char quote=ch; ++i;bool closed=false;
      while(i<sql.size()) {
        if(sql[i]==quote){++i;if(i<sql.size()&&sql[i]==quote){++i;continue;}closed=true;break;}
        if(sql[i]=='\\'&&quote=='\'') {if(!escape)return false;if(++i==sql.size())return false;}
        ++i;
      }
      if (!closed) return false;
      continue;
    }
    if(ch=='$'&&(i==0||!identifier(sql[i-1]))) {
      std::size_t end=i+1;
      if(end<sql.size()&&((sql[end]>='a'&&sql[end]<='z')||(sql[end]>='A'&&sql[end]<='Z')||sql[end]=='_'))
        while(end<sql.size()&&identifier(sql[end])&&sql[end]!='$')++end;
      if(end<sql.size()&&sql[end]=='$') {
        const auto delimiter=sql.substr(i,end-i+1);const auto close=sql.find(delimiter,end+1);
        if (close == std::string_view::npos) return false;
        i=close+delimiter.size();
        continue;
      }
    }
    ++i;
  }
  return content;
}

// This endpoint owns every object accessed by the cancelling thread. In
// particular it never retains a raw session, main transport or OpenSSL context.
class PgCancellation final : public SessionCancellationWire {
 public:
  PgCancellation(std::unique_ptr<ITransport> peer, const std::array<std::byte, 8>& key,
      std::shared_ptr<rs::core::transport::CancellationWait> wait,
      Deadline original, std::uint64_t generation, bool tls, std::string hostname)
      : peer_(std::move(peer)), key_(key), wait_(std::move(wait)), original_(original), generation_(generation),
        tls_(tls), hostname_(std::move(hostname)) {}

  std::shared_ptr<rs::core::transport::CancellationWait> wait_control() const noexcept override { return wait_; }
  bool enter_resolver() noexcept override {
    std::lock_guard lock(mutex_);
    if (sealed_ || claimed_ || error_before_claim_ || phase_ != Phase::Finished) return false;
    phase_ = Phase::Resolver;
    return true;
  }
  void leave_resolver() noexcept override {
    std::lock_guard lock(mutex_);
    if (!sealed_ && phase_ == Phase::Resolver) phase_ = Phase::Finished;
  }
  bool next_phase() noexcept override {
    std::lock_guard lock(mutex_);
    if (claimed_ && local_after_ready_ && !error_before_claim_) confirmed_ = true;
    if (sealed_ || claimed_ || error_before_claim_) return false;
    phase_ = Phase::BeforeWire;
    return true;
  }
  bool before_wire() noexcept override {
    std::lock_guard lock(mutex_);
    if (sealed_ || claimed_) return false;
    phase_ = Phase::Writing;
    return true;
  }
  void sent() noexcept override {
    std::lock_guard lock(mutex_);
    phase_ = Phase::Running;
    changed_.notify_all();
  }
  void error_observed(std::string_view native_state) noexcept override {
    std::lock_guard lock(mutex_);
    if (!error_seen_) error_before_claim_ = !claimed_;
    error_seen_ = true;
    cancelled_error_ = claimed_ && !error_before_claim_ && native_state == "57014";
  }
  CancellationOutcome drained(bool legal_ready) noexcept override {
    std::unique_lock lock(mutex_);
    phase_ = Phase::Finished;
    changed_.notify_all();
    if (claimed_ && !secondary_done_) {
      if (!changed_.wait_until(lock, wait_->effective(original_),
          [&] { return secondary_done_; })) uncertain_ = true;
    }
    confirmed_ = claimed_ && legal_ready && cancelled_error_ && secondary_ok_ &&
        Deadline::clock::now() < wait_->effective(original_);
    // A normal completion does not prove an escaped CancelRequest harmless to
    // the next operation. Quarantine and retire, even with owning normal rows.
    retire_ = claimed_ && (uncertain_ || (escaped_ && !confirmed_));
    return outcome();
  }
  CancellationOutcome seal() noexcept override {
    std::unique_lock lock(mutex_);
    sealed_ = true;
    phase_ = Phase::Finished;
    changed_.notify_all();
    if (claimed_ && !secondary_done_ &&
        !changed_.wait_until(lock, wait_->effective(original_), [&] { return secondary_done_; })) {
      uncertain_ = true;
      retire_ = true;
    }
    return outcome();
  }

  bool request() noexcept override {
    try {
      std::unique_lock lock(mutex_);
      if (!generation_) return false;
      if (sealed_) return true;
      // Refuse before freezing the cutoff or touching claim/secondary state.
      // This query belongs to the main owner's unsupported type discovery.
      if (phase_ == Phase::Resolver) return false;
      if (error_before_claim_) return false;
      if (claimed_) return secondary_done_ && secondary_ok_;
      if (!wait_->freeze(original_, Deadline::clock::now())) return false;
      claimed_ = true;
      const auto cutoff = wait_->effective(original_);
      if (phase_ == Phase::Finished) {
        // READY does not end the owning ODBC operation. Retain this local
        // claim through decoding/publication, without sending a late packet.
        // Retain an active local claim without inventing a server cancellation:
        // final normal completion keeps its status but suppresses its cursor.
        // A subsequent BEGIN->user transition promotes this to local HY008.
        local_after_ready_ = true;
        secondary_done_ = secondary_ok_ = true;
        changed_.notify_all();
        return true;
      }
      if (phase_ == Phase::BeforeWire) {
        secondary_done_ = secondary_ok_ = confirmed_ = true;
        changed_.notify_all();
        return true;
      }
      if (phase_ == Phase::Writing &&
          !changed_.wait_until(lock, cutoff, [&] { return phase_ != Phase::Writing; })) {
        secondary_done_ = true;
        uncertain_ = retire_ = true;
        changed_.notify_all();
        return false;
      }
      if (phase_ != Phase::Running) {
        secondary_done_ = true;
        changed_.notify_all();
        return false;
      }
      lock.unlock();
      const bool ok = exchange(cutoff);
      peer_->close();
      lock.lock();
      secondary_done_ = true;
      secondary_ok_ = ok;
      uncertain_ = !ok;
      if (!ok) retire_ = true;
      changed_.notify_all();
      return ok;
    } catch (...) {
      peer_->close();
      std::lock_guard lock(mutex_);
      secondary_done_ = true;
      uncertain_ = retire_ = true;
      changed_.notify_all();
      return false;
    }
  }
 private:
  enum class Phase { BeforeWire, Writing, Running, Finished, Resolver };
  CancellationOutcome outcome() const noexcept { return {claimed_, confirmed_, retire_}; }
  bool send_all(std::span<const std::byte> bytes, Deadline cutoff) {
    while (!bytes.empty()) {
      if (Deadline::clock::now() >= cutoff) return false;
      const auto result = peer_->send(bytes, cutoff);
      if (result.has_error() || result->eof || result->n == 0 || result->n > bytes.size()) return false;
      bytes = bytes.subspan(result->n);
    }
    return Deadline::clock::now() < cutoff;
  }
  bool exchange(Deadline cutoff) {
    auto* carrier = dynamic_cast<IServerCancelTransport*>(peer_.get());
    if (!carrier || carrier->connect_cancellation_peer(cutoff).has_error()) return false;
    if (tls_) {
      // The clone contains the main owner's frozen trust snapshot. The server
      // must agree to TLS before the protected cancellation bytes are sent.
      constexpr std::array<std::byte, 8> ssl_request{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{8},
          std::byte{4}, std::byte{210}, std::byte{22}, std::byte{47}};
      if (!send_all(ssl_request, cutoff)) return false;
      std::array<std::byte, 1> response{};
      auto read = peer_->recv(response, cutoff);
      auto* start_tls = dynamic_cast<rs::core::transport::IStartTlsTransport*>(peer_.get());
      if (read.has_error() || read->n != 1 || read->eof || response[0] != std::byte{'S'} ||
          !start_tls || start_tls->upgrade_to_tls(hostname_, cutoff).has_error() ||
          !start_tls->peer_identity_verified()) return false;
    }
    std::array<std::byte, 16> packet{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{16},
        std::byte{4}, std::byte{210}, std::byte{22}, std::byte{46}};
    struct CleanPacket {
      std::array<std::byte, 16>& bytes;
      ~CleanPacket() { rs::core::security::secure_cleanse(std::span<unsigned char>(
          reinterpret_cast<unsigned char*>(bytes.data()), bytes.size())); }
    } clean{packet};
    std::copy(key_.bytes.begin(), key_.bytes.end(), packet.begin() + 8);
    {
      std::lock_guard lock(mutex_);
      // Conservatively record escape before a send that can partly succeed.
      escaped_ = true;
    }
    const bool sent = send_all(packet, cutoff);
    if (!sent) return false;
    std::array<std::byte, 1> unexpected{};
    auto read = peer_->recv(unexpected, cutoff);
    return !read.has_error() && read->eof && read->n == 0 && Deadline::clock::now() < cutoff;
  }
  std::unique_ptr<ITransport> peer_;
  ProtectedKey key_;
  std::shared_ptr<rs::core::transport::CancellationWait> wait_;
  Deadline original_;
  const std::uint64_t generation_;
  bool tls_;
  std::string hostname_;
  std::mutex mutex_;
  std::condition_variable changed_;
  Phase phase_{Phase::BeforeWire};
  bool sealed_{false}, claimed_{false}, secondary_done_{false}, secondary_ok_{false}, local_after_ready_{false};
  bool error_seen_{false}, error_before_claim_{false}, cancelled_error_{false}, escaped_{false};
  bool uncertain_{false}, confirmed_{false}, retire_{false};
};
} // namespace

void PgDatabaseConnection::authenticated_backend_key(std::span<const std::byte> payload) {
  if (payload.size() != cancellation_key_.size() || cancellation_key_present_)
    throw std::runtime_error("Invalid PostgreSQL backend cancellation key");
  std::copy(payload.begin(), payload.end(), cancellation_key_.begin());
  cancellation_key_present_ = true;
}
void PgDatabaseConnection::forget_backend_key() noexcept {
  rs::core::security::secure_cleanse(std::span<unsigned char>(
      reinterpret_cast<unsigned char*>(cancellation_key_.data()), cancellation_key_.size()));
  cancellation_key_present_ = false;
}
bool PgDatabaseConnection::cancellation_request_eligible(std::string_view sql) const noexcept {
  return supports_server_cancellation() && cancellation_single_statement(sql);
}
bool PgDatabaseConnection::supports_server_cancellation() const noexcept {
  const auto* carrier = dynamic_cast<const IServerCancelTransport*>(cancellation_transport());
  return cancellation_key_present_ && is_connected() && carrier && carrier->supports_server_cancel();
}
std::shared_ptr<SessionCancellation> PgDatabaseConnection::arm_cancellation(
    std::uint64_t generation, Deadline original) {
  if (!generation || has_cancellation_operation() || Deadline::clock::now() >= original ||
      !supports_server_cancellation()) return {};
  auto* carrier = dynamic_cast<IServerCancelTransport*>(cancellation_transport());
  // Entire peer/trust acquisition is serialized on the executing session owner,
  // before the endpoint is published. No store traversal happens on a canceller.
  auto peer = carrier->cancellation_peer();
  if (!peer) return {};
  auto wait = std::make_shared<rs::core::transport::CancellationWait>();
  auto operation = std::make_shared<PgCancellation>(std::move(peer), cancellation_key_, wait,
      original, generation, cancellation_settings().use_ssl, cancellation_settings().host);
  carrier->cancellation_wait(wait);
  install_cancellation(operation);
  return operation;
}
} // namespace rs::core::database::postgres

namespace rs::core::database::postgres {
CancellationOutcome PgDatabaseConnection::finish_cancellation(
    const std::shared_ptr<SessionCancellation>& endpoint) noexcept {
  return finish_cancellation_operation(endpoint);
}
} // namespace rs::core::database::postgres
