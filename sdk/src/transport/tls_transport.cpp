#include "odbcpp/transport/tls_transport.h"

#include "odbcpp/security/tls_client.h"
#include "odbcpp/transport/socket_wait.h"
#include "odbcpp/util/exception_adapter.h"
#include "odbcpp/util/platform.h"

#include <chrono>
#include <memory>
#include <stdexcept>

using rs::util::Deadline;
using rs::util::IOError;
using rs::util::TLSError;
using rs::util::TimeoutError;

namespace rs::core::transport {

class TLSTransport::ProviderState {
public:
  std::unique_ptr<rs::core::security::TlsClient> client = std::make_unique<rs::core::security::TlsClient>();
};

namespace {

using rs::core::security::TlsStep;
using rs::core::security::TlsStepState;

bool deadline_expired(Deadline deadline) {
  return rs::util::remaining(deadline) <= std::chrono::milliseconds::zero();
}

void wait_for_tls(SocketTransport::socket_t socket, TlsStepState state,
                  Deadline deadline, std::string_view operation,
                  const std::shared_ptr<CancellationWait>& control = {}) {
  for (;;) {
  const auto waited = wait_for_socket(
      socket, state == TlsStepState::WantRead,
      state == TlsStepState::WantWrite, control ? control->slice(deadline) : deadline);
  if (waited == SocketWaitResult::Timeout && control && rs::util::Clock::now() < control->effective(deadline)) continue;
  if (waited == SocketWaitResult::Timeout) {
    throw TimeoutError("TLS " + std::string(operation) + " timeout");
  }
  if (waited == SocketWaitResult::Failed) {
    throw IOError("TLS " + std::string(operation) + " socket wait failed");
  }
  return;
  }
}

[[noreturn]] void throw_tls_step(
    const TlsStep& step, std::string_view operation) {
  if (step.state == TlsStepState::SystemError) {
    if (socket_error_is_timeout(step.system_error)) {
      throw TimeoutError("TLS " + std::string(operation) + " timeout");
    }
    if (operation != "handshake") {
      throw IOError(step.message.empty()
          ? "TLS " + std::string(operation) + " I/O error"
          : step.message);
    }
  }
  throw TLSError(step.message.empty()
      ? "TLS " + std::string(operation) + " failed"
      : step.message);
}

}  // namespace

TLSTransport::TLSTransport(DeadlineModel deadline_model)
    : tcp_(deadline_model), provider_(std::make_unique<ProviderState>()) {}

TLSTransport::~TLSTransport() { close(); }

rs::util::Result<void> TLSTransport::connect(
    std::string_view host, uint16_t port, Deadline deadline) {
  auto result = connect_plain(host, port, deadline);
  if (result.has_error()) return result;
  return upgrade_to_tls(host, deadline);
}

rs::util::Result<void> TLSTransport::connect_plain(
    std::string_view host, uint16_t port, Deadline deadline) {
  close();
  return tcp_.connect(host, port, deadline);
}

rs::util::Result<void> TLSTransport::upgrade_to_tls(
    std::string_view host, Deadline deadline) {
  if (host.find('\0') != std::string_view::npos) {
    close();
    return {rs::util::DbErrorCode::InvalidParameter,
            "TLS host contains an embedded NUL byte"};
  }
  auto result = rs::util::try_catch([&] { upgrade_impl(host, deadline); });
  if (result.has_error()) close();
  return result;
}

void TLSTransport::upgrade_from(
    socket_t socket, std::string_view host, Deadline deadline) {
  close();
  tcp_.adopt(socket);
  try {
    upgrade_impl(host, deadline);
  } catch (...) {
    close();
    throw;
  }
}

void TLSTransport::upgrade_impl(std::string_view host, Deadline deadline) {
  peer_identity_verified_ = false;
  if (host.find('\0') != std::string_view::npos) {
    throw TLSError("TLS host contains an embedded NUL byte");
  }
  if (provider_->client->active()) {
    throw TLSError("TLS session is already active");
  }
  tcp_.prepare_for_io(deadline);
  if (!provider_->client->frozen_context()) {
    provider_->client->configure({min_version_, verify_, verify_host_, ca_file_, ca_dir_});
  } else if (host != verified_hostname_) {
    throw TLSError("Cancellation TLS identity mismatch");
  }
  const auto started = provider_->client->begin_socket(
      static_cast<std::intptr_t>(tcp_.native()), host);
  if (started.state != TlsStepState::Complete) {
    throw_tls_step(started, "setup");
  }

  for (;;) {
    if (deadline_expired(deadline)) {
      throw TimeoutError("TLS handshake timeout");
    }
    const auto step = provider_->client->handshake();
    if (step.state == TlsStepState::Complete) break;
    if (step.state == TlsStepState::WantRead ||
        step.state == TlsStepState::WantWrite) {
      wait_for_tls(tcp_.native(), step.state, deadline, "handshake");
      continue;
    }
    throw_tls_step(step, "handshake");
  }
  peer_identity_verified_ = provider_->client->peer_identity_verified();
  if (peer_identity_verified_) verified_hostname_ = std::string(host);
}

std::unique_ptr<ITransport> TLSTransport::cancellation_peer() const {
  if (!supports_server_cancel()) return nullptr;
  auto destination = tcp_.cancellation_peer();
  const auto* plain = dynamic_cast<SocketTransport*>(destination.get());
  auto trust = provider_->client->verified_sibling();
  if (!plain || !trust) return nullptr;
  auto peer = std::make_unique<TLSTransport>(DeadlineModel::Strict);
  if (!peer->tcp_.copy_cancellation_peer(*plain)) return nullptr;
  peer->provider_->client = std::move(trust);
  peer->verified_hostname_ = verified_hostname_;
  return peer;
}

rs::util::Result<void> TLSTransport::connect_cancellation_peer(Deadline deadline) {
  if (!provider_->client->frozen_context() || verified_hostname_.empty()) {
    return {rs::util::DbErrorCode::InvalidParameter, "Cancellation TLS trust unavailable"};
  }
  return tcp_.connect_cancellation_peer(deadline);
}

void TLSTransport::close() noexcept {
  peer_identity_verified_ = false;
  provider_->client->reset_session();
  tcp_.close();
}

rs::util::Result<IOResult> TLSTransport::send(
    std::span<const std::byte> buffer, Deadline deadline) {
  if (!provider_->client->active()) return tcp_.send(buffer, deadline);
  return rs::util::try_catch([&]() {
    tcp_.prepare_for_io(cancellation_wait_ ? cancellation_wait_->effective(deadline) : deadline);
    if (buffer.empty()) return IOResult{0, false};

    std::size_t written = 0;
    while (written < buffer.size()) {
      if (deadline_expired(cancellation_wait_ ? cancellation_wait_->effective(deadline) : deadline)) throw TimeoutError("TLS send timeout");
      const auto step = provider_->client->write(buffer.subspan(written));
      written += step.processed;
      if (step.state == TlsStepState::Complete) {
        if (step.processed == 0 && written < buffer.size()) {
          throw IOError("TLS send made no progress");
        }
        continue;
      }
      if (step.state == TlsStepState::WantRead ||
          step.state == TlsStepState::WantWrite) {
        wait_for_tls(tcp_.native(), step.state, deadline, "send", cancellation_wait_);
        continue;
      }
      throw_tls_step(step, "send");
    }
    return IOResult{written, false};
  });
}

rs::util::Result<IOResult> TLSTransport::recv(
    std::span<std::byte> buffer, Deadline deadline) {
  if (!provider_->client->active()) return tcp_.recv(buffer, deadline);
  return rs::util::try_catch([&]() {
    tcp_.prepare_for_io(cancellation_wait_ ? cancellation_wait_->effective(deadline) : deadline);
    if (buffer.empty()) return IOResult{0, false};

    for (;;) {
      if (deadline_expired(cancellation_wait_ ? cancellation_wait_->effective(deadline) : deadline)) throw TimeoutError("TLS recv timeout");
      const auto step = provider_->client->read(buffer);
      if (step.state == TlsStepState::Complete) {
        return IOResult{step.processed, false};
      }
      if (step.state == TlsStepState::Closed) return IOResult{0, true};
      if (step.state == TlsStepState::WantRead ||
          step.state == TlsStepState::WantWrite) {
        wait_for_tls(tcp_.native(), step.state, deadline, "recv", cancellation_wait_);
        continue;
      }
      throw_tls_step(step, "recv");
    }
  });
}

void TLSTransport::set_min_tls_version(long version) {
  min_version_ = version;
  provider_->client->reset_context_if_inactive();
}

}  // namespace rs::core::transport
