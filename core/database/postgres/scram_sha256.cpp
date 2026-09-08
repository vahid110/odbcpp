#include "scram_sha256.h"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/rand.h>

namespace rs::core::database::postgres {
namespace {

constexpr std::size_t kDigestSize = 32;
constexpr std::uint32_t kMaximumIterations = 10'000'000;

std::string base64_encode(std::span<const unsigned char> input) {
  if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("SCRAM value is too large");
  }
  std::string output(4 * ((input.size() + 2) / 3), '\0');
  const auto length = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(output.data()), input.data(),
      static_cast<int>(input.size()));
  if (length < 0) throw std::runtime_error("SCRAM base64 encoding failed");
  output.resize(static_cast<std::size_t>(length));
  return output;
}

std::vector<unsigned char> base64_decode(std::string_view input) {
  if (input.empty() || input.size() % 4 != 0 ||
      input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("invalid SCRAM base64 value");
  }
  std::vector<unsigned char> output(3 * (input.size() / 4));
  const auto length = EVP_DecodeBlock(
      output.data(), reinterpret_cast<const unsigned char*>(input.data()),
      static_cast<int>(input.size()));
  if (length < 0) throw std::runtime_error("invalid SCRAM base64 value");
  std::size_t padding = 0;
  if (!input.empty() && input.back() == '=') ++padding;
  if (input.size() > 1 && input[input.size() - 2] == '=') ++padding;
  if (static_cast<std::size_t>(length) < padding) {
    throw std::runtime_error("invalid SCRAM base64 padding");
  }
  output.resize(static_cast<std::size_t>(length) - padding);
  return output;
}

std::array<unsigned char, kDigestSize> sha256(
    std::span<const unsigned char> input) {
  std::array<unsigned char, kDigestSize> output{};
  auto* context = EVP_MD_CTX_new();
  if (!context) throw std::runtime_error("failed to allocate SHA-256 context");
  unsigned int length = 0;
  const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
      EVP_DigestUpdate(context, input.data(), input.size()) == 1 &&
      EVP_DigestFinal_ex(context, output.data(), &length) == 1;
  EVP_MD_CTX_free(context);
  if (!ok || length != output.size()) {
    throw std::runtime_error("SCRAM SHA-256 failed");
  }
  return output;
}

std::array<unsigned char, kDigestSize> hmac_sha256(
    std::span<const unsigned char> key, std::string_view input) {
  std::array<unsigned char, kDigestSize> output{};
  auto* algorithm = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
  if (!algorithm) throw std::runtime_error("failed to load HMAC");
  auto* context = EVP_MAC_CTX_new(algorithm);
  EVP_MAC_free(algorithm);
  if (!context) throw std::runtime_error("failed to allocate HMAC context");

  char digest_name[] = "SHA256";
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(
          OSSL_MAC_PARAM_DIGEST, digest_name, 0),
      OSSL_PARAM_construct_end()};
  std::size_t length = 0;
  const bool ok = EVP_MAC_init(context, key.data(), key.size(), parameters) == 1 &&
      EVP_MAC_update(
          context, reinterpret_cast<const unsigned char*>(input.data()),
          input.size()) == 1 &&
      EVP_MAC_final(context, output.data(), &length, output.size()) == 1;
  EVP_MAC_CTX_free(context);
  if (!ok || length != output.size()) {
    throw std::runtime_error("SCRAM HMAC-SHA-256 failed");
  }
  return output;
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
  if (password.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      salt.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      iterations > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("SCRAM credential input is too large");
  }
  std::array<unsigned char, kDigestSize> output{};
  if (PKCS5_PBKDF2_HMAC(
          password.data(), static_cast<int>(password.size()), salt.data(),
          static_cast<int>(salt.size()), static_cast<int>(iterations),
          EVP_sha256(), static_cast<int>(output.size()), output.data()) != 1) {
    throw std::runtime_error("SCRAM PBKDF2 failed");
  }
  return output;
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
  if (!password_.empty()) OPENSSL_cleanse(password_.data(), password_.size());
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
  auto salted = salted_password(password_, salt, iterations);
  auto client_key = hmac_sha256(salted, "Client Key");
  const auto stored_key = sha256(client_key);

  server_first_ = message;
  client_final_without_proof_ = "c=biws,r=" + nonce->second;
  const auto authentication_message = client_first_bare_ + "," +
      server_first_ + "," + client_final_without_proof_;
  const auto client_signature = hmac_sha256(stored_key, authentication_message);
  std::array<unsigned char, kDigestSize> client_proof{};
  for (std::size_t i = 0; i < client_proof.size(); ++i) {
    client_proof[i] = client_key[i] ^ client_signature[i];
  }
  auto server_key = hmac_sha256(salted, "Server Key");
  expected_server_signature_ = hmac_sha256(server_key, authentication_message);
  OPENSSL_cleanse(salted.data(), salted.size());
  OPENSSL_cleanse(client_key.data(), client_key.size());
  OPENSSL_cleanse(server_key.data(), server_key.size());
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
  if (decoded.size() != expected_server_signature_.size() ||
      CRYPTO_memcmp(decoded.data(), expected_server_signature_.data(),
                    expected_server_signature_.size()) != 0) {
    throw std::runtime_error("SCRAM server signature verification failed");
  }
}

std::string generate_scram_nonce() {
  std::array<unsigned char, 18> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
    throw std::runtime_error("failed to generate SCRAM nonce");
  }
  return base64_encode(random);
}

} // namespace rs::core::database::postgres
