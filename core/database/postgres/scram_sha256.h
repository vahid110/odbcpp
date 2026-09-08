#pragma once

#include <array>
#include <string>
#include <string_view>

namespace rs::core::database::postgres {

class ScramSha256Client {
public:
  ScramSha256Client(std::string user, std::string password,
                   std::string client_nonce);
  ~ScramSha256Client();

  ScramSha256Client(const ScramSha256Client&) = delete;
  ScramSha256Client& operator=(const ScramSha256Client&) = delete;

  std::string client_first_message() const;
  std::string receive_server_first(std::string_view message);
  void verify_server_final(std::string_view message) const;

private:
  std::string password_;
  std::string client_nonce_;
  std::string client_first_bare_;
  std::string server_first_;
  std::string client_final_without_proof_;
  std::array<unsigned char, 32> expected_server_signature_{};
  bool server_first_received_{false};
};

std::string generate_scram_nonce();

} // namespace rs::core::database::postgres
