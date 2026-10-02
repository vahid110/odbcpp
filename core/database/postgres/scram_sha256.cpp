#include "scram_sha256.h"
#include "core/security/crypto.h"
#include "core/util/base64.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

namespace rs::core::database::postgres {
namespace {

constexpr std::size_t kDigestSize =
    rs::core::security::Sha256Digest{}.size();
constexpr std::uint32_t kMaximumIterations = 10'000'000;

struct ScramSecrets {
  rs::core::security::Sha256Digest salted{};
  rs::core::security::Sha256Digest client_key{};
  rs::core::security::Sha256Digest server_key{};

  ~ScramSecrets() {
    rs::core::security::secure_cleanse(salted);
    rs::core::security::secure_cleanse(client_key);
    rs::core::security::secure_cleanse(server_key);
  }
};

std::string base64_encode(std::span<const unsigned char> input) {
  return rs::util::base64_encode(input);
}

std::vector<unsigned char> base64_decode(std::string_view input) {
  if (input.empty()) {
    throw std::runtime_error("invalid SCRAM base64 value");
  }
  return rs::util::base64_decode(input);
}

std::array<unsigned char, kDigestSize> sha256(
    std::span<const unsigned char> input) {
  return rs::core::security::sha256(input);
}

std::array<unsigned char, kDigestSize> hmac_sha256(
    std::span<const unsigned char> key, std::string_view input) {
  return rs::core::security::hmac_sha256(
      key, {reinterpret_cast<const unsigned char*>(input.data()), input.size()});
}

std::map<char, std::string> parse_attributes(std::string_view message) {
  std::map<char, std::string> attributes;
  std::size_t offset = 0;
  while (offset <= message.size()) {
    const auto separator = message.find(',', offset);
    const auto end = separator == std::string_view::npos
        ? message.size() : separator;
    const auto attribute = message.substr(offset, end - offset);
    if (attribute.size() < 3 || attribute[1] != '=' ||
        !attributes.emplace(attribute[0], std::string(attribute.substr(2))).second) {
      throw std::runtime_error("invalid SCRAM attribute list");
    }
    if (separator == std::string_view::npos) break;
    offset = separator + 1;
  }
  return attributes;
}

std::string escape_username(std::string_view user) {
  std::string escaped;
  escaped.reserve(user.size());
  for (const char ch : user) {
    if (ch == '=') {
      escaped += "=3D";
    } else if (ch == ',') {
      escaped += "=2C";
    } else {
      escaped.push_back(ch);
    }
  }
  return escaped;
}

std::array<unsigned char, kDigestSize> salted_password(
    const std::string& password, std::span<const unsigned char> salt,
    std::uint32_t iterations) {
  return rs::core::security::pbkdf2_hmac_sha256(password, salt, iterations);
}

} // namespace

ScramSha256Client::ScramSha256Client(
    std::string user, std::string password, std::string client_nonce)
    : password_(std::move(password)), client_nonce_(std::move(client_nonce)) {
  if (client_nonce_.empty() || client_nonce_.find(',') != std::string::npos) {
    throw std::invalid_argument("invalid SCRAM client nonce");
  }
  client_first_bare_ =
      "n=" + escape_username(user) + ",r=" + client_nonce_;
}

ScramSha256Client::~ScramSha256Client() {
  rs::core::security::secure_cleanse({
      reinterpret_cast<unsigned char*>(password_.data()), password_.size()});
  rs::core::security::secure_cleanse(expected_server_signature_);
}

std::string ScramSha256Client::client_first_message() const {
  return "n,," + client_first_bare_;
}

std::string ScramSha256Client::receive_server_first(std::string_view message) {
  if (server_first_received_) {
    throw std::runtime_error("duplicate SCRAM server-first message");
  }
  const auto attributes = parse_attributes(message);
  if (attributes.contains('m')) {
    throw std::runtime_error("unsupported mandatory SCRAM extension");
  }
  const auto nonce = attributes.find('r');
  const auto salt_value = attributes.find('s');
  const auto iteration_value = attributes.find('i');
  if (nonce == attributes.end() || salt_value == attributes.end() ||
      iteration_value == attributes.end() ||
      !nonce->second.starts_with(client_nonce_) ||
      nonce->second.size() <= client_nonce_.size()) {
    throw std::runtime_error("invalid SCRAM server-first message");
  }

  std::uint32_t iterations = 0;
  const auto [end, error] = std::from_chars(
      iteration_value->second.data(),
      iteration_value->second.data() + iteration_value->second.size(),
      iterations);
  if (error != std::errc{} ||
      end != iteration_value->second.data() + iteration_value->second.size() ||
      iterations == 0 || iterations > kMaximumIterations) {
    throw std::runtime_error("invalid SCRAM iteration count");
  }

  auto salt = base64_decode(salt_value->second);
  ScramSecrets secrets;
  secrets.salted = salted_password(password_, salt, iterations);
  secrets.client_key = hmac_sha256(secrets.salted, "Client Key");
  const auto stored_key = sha256(secrets.client_key);

  server_first_ = message;
  client_final_without_proof_ = "c=biws,r=" + nonce->second;
  const auto authentication_message = client_first_bare_ + "," +
      server_first_ + "," + client_final_without_proof_;
  const auto client_signature = hmac_sha256(stored_key, authentication_message);
  std::array<unsigned char, kDigestSize> client_proof{};
  for (std::size_t i = 0; i < client_proof.size(); ++i) {
    client_proof[i] = secrets.client_key[i] ^ client_signature[i];
  }
  secrets.server_key = hmac_sha256(secrets.salted, "Server Key");
  expected_server_signature_ =
      hmac_sha256(secrets.server_key, authentication_message);
  server_first_received_ = true;
  return client_final_without_proof_ + ",p=" + base64_encode(client_proof);
}

void ScramSha256Client::verify_server_final(std::string_view message) const {
  if (!server_first_received_) {
    throw std::runtime_error("SCRAM server-final message arrived out of sequence");
  }
  const auto attributes = parse_attributes(message);
  if (const auto server_error = attributes.find('e');
      server_error != attributes.end()) {
    throw std::runtime_error("SCRAM authentication failed: " +
                             server_error->second);
  }
  const auto verifier = attributes.find('v');
  if (verifier == attributes.end()) {
    throw std::runtime_error("SCRAM server signature is missing");
  }
  const auto decoded = base64_decode(verifier->second);
  if (!rs::core::security::constant_time_equal(
          decoded, expected_server_signature_)) {
    throw std::runtime_error("SCRAM server signature verification failed");
  }
}

std::string generate_scram_nonce() {
  std::array<unsigned char, 18> random{};
  rs::core::security::secure_random(random);
  return base64_encode(random);
}

} // namespace rs::core::database::postgres
