#include "crypto.h"

#include <limits>
#include <stdexcept>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/opensslv.h>
#include <openssl/rand.h>

namespace rs::core::security {
namespace {

template <std::size_t Size>
std::array<unsigned char, Size> digest(
    const EVP_MD* algorithm, std::span<const unsigned char> input,
    std::string_view operation) {
  std::array<unsigned char, Size> output{};
  auto* context = EVP_MD_CTX_new();
  if (!context) {
    throw std::runtime_error("failed to allocate " + std::string(operation) +
                             " context");
  }

  unsigned int length = 0;
  const bool ok = EVP_DigestInit_ex(context, algorithm, nullptr) == 1 &&
      EVP_DigestUpdate(context, input.data(), input.size()) == 1 &&
      EVP_DigestFinal_ex(context, output.data(), &length) == 1;
  EVP_MD_CTX_free(context);
  if (!ok || length != output.size()) {
    throw std::runtime_error(std::string(operation) + " failed");
  }
  return output;
}

int checked_int_size(std::size_t size, std::string_view input_name) {
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error(std::string(input_name) + " is too large");
  }
  return static_cast<int>(size);
}

}  // namespace

CryptoProviderIdentity crypto_provider_identity() {
  CryptoProviderIdentity identity;
  identity.provider = "OPENSSL";
  identity.compile_version = OPENSSL_VERSION_TEXT;
  identity.runtime_version = OpenSSL_version(OPENSSL_VERSION);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
  identity.fips_enabled = EVP_default_properties_is_fips_enabled(nullptr) == 1;
#endif
  return identity;
}

Md5Digest md5(std::span<const unsigned char> input) {
  return digest<Md5Digest{}.size()>(EVP_md5(), input, "MD5");
}

Sha256Digest sha256(std::span<const unsigned char> input) {
  return digest<Sha256Digest{}.size()>(EVP_sha256(), input, "SHA-256");
}

Sha256Digest hmac_sha256(std::span<const unsigned char> key,
                         std::span<const unsigned char> input) {
  Sha256Digest output{};
  unsigned int length = 0;
  if (!HMAC(EVP_sha256(), key.data(), checked_int_size(key.size(), "HMAC key"),
            input.data(), input.size(), output.data(), &length) ||
      length != output.size()) {
    throw std::runtime_error("HMAC-SHA-256 failed");
  }
  return output;
}

Sha256Digest pbkdf2_hmac_sha256(
    std::string_view password, std::span<const unsigned char> salt,
    std::uint32_t iterations) {
  if (iterations == 0 ||
      iterations > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument("PBKDF2 iteration count is out of range");
  }

  Sha256Digest output{};
  if (PKCS5_PBKDF2_HMAC(
          password.data(), checked_int_size(password.size(), "PBKDF2 password"),
          salt.data(), checked_int_size(salt.size(), "PBKDF2 salt"),
          static_cast<int>(iterations), EVP_sha256(),
          static_cast<int>(output.size()), output.data()) != 1) {
    throw std::runtime_error("PBKDF2-HMAC-SHA-256 failed");
  }
  return output;
}

void secure_random(std::span<unsigned char> output) {
  if (output.empty()) return;
  if (RAND_bytes(output.data(), checked_int_size(output.size(), "random output")) !=
      1) {
    throw std::runtime_error("secure random generation failed");
  }
}

bool constant_time_equal(std::span<const unsigned char> left,
                         std::span<const unsigned char> right) noexcept {
  return left.size() == right.size() &&
      (left.empty() || CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0);
}

void secure_cleanse(std::span<unsigned char> value) noexcept {
  if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
}

}  // namespace rs::core::security
