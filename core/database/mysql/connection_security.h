#pragma once
#include "handshake_wire.h"
#include "core/security/crypto.h"

namespace rs::core::database::mysql {
struct SslRequest {
  std::uint32_t capabilities{};
  std::array<std::byte, 36> packet{};
};
// Protocol-41, UTF8MB4 general collation, no optional wire extensions yet.
// Contains no username/password/database. Does not establish or verify TLS.
inline rs::util::Result<SslRequest> make_ssl_request(const ServerGreeting& greeting,
    std::uint32_t max_packet_size = static_cast<std::uint32_t>(connection_packet_limit)) {
  constexpr auto required = client_protocol_41 | client_ssl | client_secure_connection | client_plugin_auth;
  if ((greeting.capabilities & required) != required)
    return {rs::util::DbErrorCode::UnsupportedFeature, "MySQL TLS capability is unavailable"};
  if (max_packet_size == 0 || max_packet_size > connection_packet_limit)
    return {rs::util::DbErrorCode::InvalidParameter, "MySQL connection packet budget is invalid"};
  SslRequest result;
  result.capabilities = required;
  result.packet[0] = std::byte{32}; result.packet[3] = std::byte{1};
  for (std::size_t i = 0; i < 4; ++i) {
    result.packet[4 + i] = static_cast<std::byte>((required >> (8 * i)) & 255);
    result.packet[8 + i] = static_cast<std::byte>((max_packet_size >> (8 * i)) & 255);
  }
  result.packet[12] = std::byte{45};
  return result;
}

// Move-only retained derived credential. Consumers borrow bytes synchronously;
// moving cleanses the source and destruction cleanses all retained storage.
class Sha2Token {
 public:
  Sha2Token() = default;
  Sha2Token(const Sha2Token&) = delete;
  Sha2Token& operator=(const Sha2Token&) = delete;
  Sha2Token(Sha2Token&& other) noexcept : bytes_(other.bytes_), size_(other.size_) { other.clear(); }
  Sha2Token& operator=(Sha2Token&& other) noexcept {
    if (this != &other) { clear(); bytes_ = other.bytes_; size_ = other.size_; other.clear(); }
    return *this;
  }
  ~Sha2Token() { clear(); }
  std::span<const unsigned char> bytes() const noexcept { return {bytes_.data(), size_}; }
  void clear() noexcept { rs::core::security::secure_cleanse(bytes_); size_ = 0; }
 private:
  friend rs::util::Result<Sha2Token> make_sha2_token(std::string_view,
      std::span<const std::byte>, bool, std::size_t);
  std::array<unsigned char, 32> bytes_{};
  std::size_t size_{};
};

// Caller must obtain verification from the live transport, not an SSL flag.
// No RSA/plaintext fallback, password copy, or hashing before verified TLS.
inline rs::util::Result<Sha2Token> make_sha2_token(std::string_view password,
    std::span<const std::byte> challenge, bool peer_identity_verified,
    std::size_t password_limit = 65536) {
  if (!peer_identity_verified)
    return {rs::util::DbErrorCode::TLSError, "MySQL authentication requires verified TLS"};
  if (password_limit > 1024 * 1024 || password.size() > password_limit)
    return {rs::util::DbErrorCode::ResourceLimit, "MySQL credential input limit exceeded"};
  if (password.find('\0') != std::string_view::npos || challenge.size() != 20)
    return {rs::util::DbErrorCode::InvalidParameter, "MySQL authentication input is invalid"};
  Sha2Token result;
  if (password.empty()) return rs::util::Result<Sha2Token>{std::move(result)};
  struct Work {
    rs::core::security::Sha256Digest first{}, second{}, third{};
    std::array<unsigned char, 52> combined{};
    ~Work() {
      rs::core::security::secure_cleanse(first); rs::core::security::secure_cleanse(second);
      rs::core::security::secure_cleanse(third); rs::core::security::secure_cleanse(combined);
    }
  } work;
  work.first = rs::core::security::sha256({reinterpret_cast<const unsigned char*>(password.data()), password.size()});
  work.second = rs::core::security::sha256(work.first);
  std::copy(work.second.begin(), work.second.end(), work.combined.begin());
  for (std::size_t i = 0; i < challenge.size(); ++i)
    work.combined[32 + i] = std::to_integer<unsigned char>(challenge[i]);
  work.third = rs::core::security::sha256(work.combined);
  for (std::size_t i = 0; i < result.bytes_.size(); ++i)
    result.bytes_[i] = static_cast<unsigned char>(work.first[i] ^ work.third[i]);
  result.size_ = result.bytes_.size();
  return rs::util::Result<Sha2Token>{std::move(result)};
}
}
