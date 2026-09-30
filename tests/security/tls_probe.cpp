// POSIX probe shared by the production OpenSSL and isolated AWS-LC proofs.
#include "core/security/tls_client.h"
#include "core/security/crypto.h"
#include "core/util/platform.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace rs::core::security;

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--identity") {
    const auto identity = crypto_provider_identity();
    const TlsClientConfig config;
    std::cout << identity.provider << '\n' << identity.compile_version << '\n'
              << identity.runtime_version << '\n' << identity.fips_enabled << '\n'
              << config.minimum_version << '\n' << config.verify_peer << '\n'
              << config.verify_hostname << '\n';
    return 0;
  }
  if (argc != 5) return 2;
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 2;
  struct SocketGuard { int fd; ~SocketGuard() { ::close(fd); } } guard{fd};
  try {
    timeval timeout{5, 0};
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)))
      throw std::runtime_error("socket timeout setup failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<unsigned short>(std::stoi(argv[1])));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
      throw std::runtime_error("loopback connect failed");
    TlsClient client;
    TlsClientConfig config;
    config.ca_file = argv[3];
    client.configure(config);
    auto step = client.begin_socket(fd, argv[2]);
    if (step.state != TlsStepState::Complete)
      throw std::runtime_error("TLS initialization failed: " + step.message);
    step = client.handshake();
    const std::string expected = argv[4];
    if (expected != "accept") {
      if (step.state != TlsStepState::TlsError || client.peer_identity_verified())
        throw std::runtime_error("expected unverified TLS rejection: " + step.message);
      if (expected == "hostname" &&
          step.message.find("hostname verification failed") == std::string::npos)
        throw std::runtime_error("wrong hostname rejection reason: " + step.message);
      if (expected == "protocol" &&
          step.message.find("TLSV1_ALERT_PROTOCOL_VERSION") == std::string::npos &&
          step.message.find("tlsv1 alert protocol version") == std::string::npos)
        throw std::runtime_error("wrong protocol rejection reason: " + step.message);
      if (expected == "trust" &&
          step.message.find("CERTIFICATE_VERIFY_FAILED") == std::string::npos &&
          step.message.find("certificate verify failed") == std::string::npos)
        throw std::runtime_error("wrong trust rejection reason: " + step.message);
    } else {
      if (step.state != TlsStepState::Complete || !client.peer_identity_verified())
        throw std::runtime_error("verified handshake failed: " + step.message);
      const std::array<std::byte, 4> sent{std::byte{'p'}, std::byte{0}, std::byte{0xff}, std::byte{'g'}};
      std::size_t count = 0;
      while (count < sent.size()) {
        step = client.write(std::span(sent).subspan(count));
        if (step.state != TlsStepState::Complete || !step.processed)
          throw std::runtime_error("TLS write failed: " + step.message);
        count += step.processed;
      }
      std::array<std::byte, 4> received{};
      count = 0;
      while (count < received.size()) {
        step = client.read(std::span(received).subspan(count));
        if (step.state != TlsStepState::Complete || !step.processed)
          throw std::runtime_error("TLS read failed: " + step.message);
        count += step.processed;
      }
      if (received != sent) throw std::runtime_error("TLS payload mismatch");
    }
    client.reset_session();
    if (client.active() || client.peer_identity_verified())
      throw std::runtime_error("reset retained session verification state");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
