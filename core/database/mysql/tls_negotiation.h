#pragma once
#include "connection_security.h"
#include "odbcpp/transport/start_tls_transport.h"

namespace rs::core::database::mysql {
struct VerifiedGreeting {
  ServerGreeting greeting;
  std::uint32_t negotiated_capabilities{};
};

// Private connection-phase composition, not an authenticated SDK session.
// The same transport and original deadline cover plaintext greeting, SSLRequest
// and TLS upgrade. On every failure (including exceptions), retire the socket.
inline rs::util::Result<VerifiedGreeting> negotiate_verified_tls(
    rs::core::transport::ITransport& transport, std::string_view host,
    std::uint16_t port, rs::util::Deadline deadline) {
  using rs::util::DbErrorCode;
  struct Cleanup {
    rs::core::transport::ITransport& transport;
    bool accepted{};
    ~Cleanup() { if (!accepted) transport.close(); }
  } cleanup{transport};
  const auto reject = [](DbErrorCode code) -> rs::util::Result<VerifiedGreeting> {
    return {code, "MySQL verified TLS negotiation failed"};
  };
  const auto expired = [&] { return rs::util::Clock::now() >= deadline; };
  auto* tls = dynamic_cast<rs::core::transport::IStartTlsTransport*>(&transport);
  if (!tls) return reject(DbErrorCode::UnsupportedFeature);
  if (host.empty() || host.find('\0') != std::string_view::npos || port == 0)
    return reject(DbErrorCode::InvalidParameter);
  if (expired()) return reject(DbErrorCode::Timeout);
  auto connected = tls->connect_plain(host, port, deadline);
  if (!connected) return {connected.error(), "MySQL verified TLS negotiation failed"};
  const auto read_exact = [&](std::span<std::byte> bytes) -> rs::util::Result<void> {
    while (!bytes.empty()) {
      if (expired()) return {DbErrorCode::Timeout};
      auto read = transport.recv(bytes, deadline);
      if (!read) return {read.error()};
      if (read->n > bytes.size()) return {DbErrorCode::ProtocolError};
      if (read->eof || read->n == 0) return {DbErrorCode::NetworkError};
      bytes = bytes.subspan(read->n);
    }
    return {};
  };
  std::array<std::byte, 4> header{};
  auto read = read_exact(header);
  if (!read) return {read.error(), "MySQL verified TLS negotiation failed"};
  ConnectionPacketDecoder decoder{0};
  auto initial = decoder.feed(header);
  if (!initial) return {initial.error(), "MySQL verified TLS negotiation failed"};
  if (initial->payload) return reject(DbErrorCode::ProtocolError);
  const auto length = std::to_integer<std::size_t>(header[0]) |
      (std::to_integer<std::size_t>(header[1]) << 8) |
      (std::to_integer<std::size_t>(header[2]) << 16);
  std::vector<std::byte> payload(length);
  read = read_exact(payload);
  if (!read) return {read.error(), "MySQL verified TLS negotiation failed"};
  auto packet = decoder.feed(payload);
  if (!packet || !packet->payload) return reject(DbErrorCode::ProtocolError);
  auto greeting = decode_server_greeting(*packet->payload);
  if (!greeting) return {greeting.error(), "MySQL verified TLS negotiation failed"};
  // Deliberately pinned proof profile. Broader version admission needs evidence.
  if (greeting->server_version != "8.4.11") return reject(DbErrorCode::UnsupportedFeature);
  auto request = make_ssl_request(*greeting);
  if (!request) return {request.error(), "MySQL verified TLS negotiation failed"};
  std::span<const std::byte> output{request->packet};
  while (!output.empty()) {
    if (expired()) return reject(DbErrorCode::Timeout);
    auto sent = transport.send(output, deadline);
    if (!sent) return {sent.error(), "MySQL verified TLS negotiation failed"};
    if (sent->n > output.size()) return reject(DbErrorCode::ProtocolError);
    if (sent->eof || sent->n == 0) return reject(DbErrorCode::NetworkError);
    output = output.subspan(sent->n);
  }
  if (expired()) return reject(DbErrorCode::Timeout);
  auto upgraded = tls->upgrade_to_tls(host, deadline);
  if (!upgraded) return {upgraded.error(), "MySQL verified TLS negotiation failed"};
  if (!tls->peer_identity_verified()) return reject(DbErrorCode::TLSError);
  if (expired()) return reject(DbErrorCode::Timeout);
  cleanup.accepted = true;
  return VerifiedGreeting{std::move(*greeting), request->capabilities};
}
}
