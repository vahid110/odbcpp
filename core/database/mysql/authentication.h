#pragma once
#include "tls_negotiation.h"
#include "error_wire.h"

namespace rs::core::database::mysql {
enum class AuthenticationPath { Immediate, Cached, FullOverTls };
struct AuthenticatedGreeting {
  VerifiedGreeting verified;
  AuthenticationPath path{};
};
namespace authentication_detail {
struct SecretPacket {
  std::vector<std::byte> bytes;
  explicit SecretPacket(std::size_t size) : bytes(size) {}
  SecretPacket(const SecretPacket&) = delete;
  SecretPacket& operator=(const SecretPacket&) = delete;
  ~SecretPacket() {
    rs::core::security::secure_cleanse({reinterpret_cast<unsigned char*>(bytes.data()), bytes.size()});
  }
};
inline rs::util::Result<void> send_all(rs::core::transport::ITransport& transport,
    std::span<const std::byte> bytes, rs::util::Deadline deadline) {
  using rs::util::DbErrorCode;
  while (!bytes.empty()) {
    if (rs::util::Clock::now() >= deadline) return {DbErrorCode::Timeout};
    auto* tls = dynamic_cast<rs::core::transport::IStartTlsTransport*>(&transport);
    if (!tls || !tls->peer_identity_verified()) return {DbErrorCode::TLSError};
    auto sent = transport.send(bytes, deadline);
    if (!sent) return {sent.error()};
    if (sent->n > bytes.size()) return {DbErrorCode::ProtocolError};
    if (sent->eof || sent->n == 0) return {DbErrorCode::NetworkError};
    bytes = bytes.subspan(sent->n);
  }
  return {};
}
inline rs::util::Result<std::vector<std::byte>> receive_packet(
    rs::core::transport::ITransport& transport, std::uint8_t sequence,
    rs::util::Deadline deadline) {
  using rs::util::DbErrorCode;
  const auto read_exact = [&](std::span<std::byte> bytes) -> rs::util::Result<void> {
    while (!bytes.empty()) {
      if (rs::util::Clock::now() >= deadline) return {DbErrorCode::Timeout};
      auto read = transport.recv(bytes, deadline);
      if (!read) return {read.error()};
      if (read->n > bytes.size()) return {DbErrorCode::ProtocolError};
      if (read->eof || read->n == 0) return {DbErrorCode::NetworkError};
      bytes = bytes.subspan(read->n);
    }
    return {};
  };
  std::array<std::byte,4> header{};
  auto read = read_exact(header);
  if (!read) return {read.error()};
  ConnectionPacketDecoder decoder{sequence};
  auto initial = decoder.feed(header);
  if (!initial) return {initial.error()};
  if (initial->payload) return {DbErrorCode::ProtocolError};
  const auto length = std::to_integer<std::size_t>(header[0]) |
      (std::to_integer<std::size_t>(header[1]) << 8) |
      (std::to_integer<std::size_t>(header[2]) << 16);
  std::vector<std::byte> payload(length);
  read = read_exact(payload);
  if (!read) return {read.error()};
  auto decoded = decoder.feed(payload);
  if (!decoded || !decoded->payload) return {DbErrorCode::ProtocolError};
  return std::move(*decoded->payload);
}
inline void frame(std::span<std::byte> bytes, std::uint8_t sequence) {
  const auto length = bytes.size()-4;
  for (std::size_t i=0; i<3; ++i) bytes[i]=static_cast<std::byte>((length>>(8*i))&255);
  bytes[3]=static_cast<std::byte>(sequence);
}
// Authentication OK in the pinned profile: zero affected rows/insert ID,
// protocol-41 status and warnings. Optional EOF information is ignored, bounded.
inline bool final_ok(std::span<const std::byte> bytes) {
  return bytes.size()>=7 && bytes[0]==std::byte{0} && bytes[1]==std::byte{0} && bytes[2]==std::byte{0};
}
}

// Connect-phase proof only. No database selection, command execution or reusable
// SDK session publication yet. Credentials are borrowed; owned outgoing wire
// storage is cleansed. All credential writes require current peer verification.
inline rs::util::Result<AuthenticatedGreeting> authenticate_verified_tls(
    rs::core::transport::ITransport& transport, std::string_view host,
    std::uint16_t port, std::string_view username, std::string_view password,
    rs::util::Deadline deadline) {
  using rs::util::DbErrorCode;
  const auto reject = [](DbErrorCode code) -> rs::util::Result<AuthenticatedGreeting> {
    return {code,"MySQL authentication failed"};
  };
  struct Cleanup {
    rs::core::transport::ITransport& transport;
    bool accepted{};
    ~Cleanup() { if (!accepted) transport.close(); }
  } cleanup{transport};
  if (username.empty() || username.size()>256 || username.find('\0')!=std::string_view::npos ||
      password.size()>connection_packet_limit-1 || password.find('\0')!=std::string_view::npos)
    return reject(DbErrorCode::InvalidParameter);
  cleanup.accepted=true; // Negotiation owns cleanup, including exception unwind.
  auto verified = negotiate_verified_tls(transport,host,port,deadline);
  if (!verified) return {verified.error(),"MySQL authentication failed"};
  cleanup.accepted=false; // Own the verified transport for authentication.
  auto* tls = dynamic_cast<rs::core::transport::IStartTlsTransport*>(&transport);
  if (!tls || !tls->peer_identity_verified()) return reject(DbErrorCode::TLSError);
  auto token = make_sha2_token(password,verified->greeting.challenge,true,connection_packet_limit-1);
  if (!token) return {token.error(),"MySQL authentication failed"};
  auto request = make_ssl_request(verified->greeting);
  if (!request || request->capabilities!=verified->negotiated_capabilities)
    return reject(DbErrorCode::ProtocolError);
  constexpr std::string_view plugin="caching_sha2_password";
  {
    authentication_detail::SecretPacket response(4+32+username.size()+1+1+token->bytes().size()+plugin.size()+1);
    authentication_detail::frame(response.bytes,2);
    std::copy(request->packet.begin()+4,request->packet.end(),response.bytes.begin()+4);
    std::size_t offset=36;
    for(const auto c:username) response.bytes[offset++]=static_cast<std::byte>(c);
    ++offset; // NUL username.
    response.bytes[offset++]=static_cast<std::byte>(token->bytes().size());
    for(const auto c:token->bytes()) response.bytes[offset++]=static_cast<std::byte>(c);
    for(const auto c:plugin) response.bytes[offset++]=static_cast<std::byte>(c);
    if (!tls->peer_identity_verified()) return reject(DbErrorCode::TLSError);
    auto sent=authentication_detail::send_all(transport,response.bytes,deadline);
    if (!sent) return {sent.error(),"MySQL authentication failed"};
  }
  token->clear();
  auto reply=authentication_detail::receive_packet(transport,3,deadline);
  if (!reply) return {reply.error(),"MySQL authentication failed"};
  auto path=AuthenticationPath::Immediate;
  if (reply->size()==2 && (*reply)[0]==std::byte{1}) {
    if ((*reply)[1]==std::byte{3}) {
      path=AuthenticationPath::Cached;
      reply=authentication_detail::receive_packet(transport,4,deadline);
    } else if ((*reply)[1]==std::byte{4}) {
      path=AuthenticationPath::FullOverTls;
      if (!tls->peer_identity_verified()) return reject(DbErrorCode::TLSError);
      {
        authentication_detail::SecretPacket full(4+password.size()+1);
        authentication_detail::frame(full.bytes,4);
        for(std::size_t i=0;i<password.size();++i) full.bytes[4+i]=static_cast<std::byte>(password[i]);
        auto sent=authentication_detail::send_all(transport,full.bytes,deadline);
        if (!sent) return {sent.error(),"MySQL authentication failed"};
      }
      reply=authentication_detail::receive_packet(transport,5,deadline);
    } else return reject(DbErrorCode::UnsupportedFeature);
  }
  if (!reply) return {reply.error(),"MySQL authentication failed"};
  if (!reply->empty() && (*reply)[0]==std::byte{255}) {
    if (!valid_protocol41_error_packet(*reply)) return reject(DbErrorCode::ProtocolError);
    return reject(DbErrorCode::AuthenticationFailed);
  }
  if (!reply->empty() && (*reply)[0]==std::byte{254}) return reject(DbErrorCode::UnsupportedFeature);
  if (!authentication_detail::final_ok(*reply)) return reject(DbErrorCode::ProtocolError);
  if (((std::to_integer<unsigned>((*reply)[3]) | (std::to_integer<unsigned>((*reply)[4])<<8)) & (9|0x40|0x80|0x1000|0x4000)) ||
      (*reply)[5]!=std::byte{0} || (*reply)[6]!=std::byte{0}) return reject(DbErrorCode::UnsupportedFeature);
  if (rs::util::Clock::now()>=deadline) return reject(DbErrorCode::Timeout);
  if (!tls->peer_identity_verified()) return reject(DbErrorCode::TLSError);
  cleanup.accepted=true;
  return AuthenticatedGreeting{std::move(*verified),path};
}
}
