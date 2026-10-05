#include <gtest/gtest.h>
#include "odbcpp/security/crypto.h"
#include <dlfcn.h>
#include <array>
#include <filesystem>
#include <memory>
#include <string>

namespace {
class LoadedLibrary {
public:
  explicit LoadedLibrary(const char* path)
      : handle(dlopen(path, RTLD_NOW | RTLD_NOLOAD)) {}
  ~LoadedLibrary() { if (handle) dlclose(handle); }
  void* handle;
};

TEST(ProviderCoexistence, KeepsSystemOpenSslAndAwsLcOperationsSeparate) {
  // NOLOAD makes missing preload a failure, not a silently weaker load order.
  LoadedLibrary host(ODBCPP_HOST_CRYPTO);
  ASSERT_NE(host.handle, nullptr) << "system OpenSSL must already be preloaded";
  void* version_symbol = dlsym(host.handle, "OpenSSL_version");
  ASSERT_NE(version_symbol, nullptr);
  EXPECT_EQ(dlsym(RTLD_DEFAULT, "OpenSSL_version"), version_symbol);
  Dl_info origin{};
  ASSERT_NE(dladdr(version_symbol, &origin), 0);
  ASSERT_NE(origin.dli_fname, nullptr);
  EXPECT_EQ(std::filesystem::canonical(origin.dli_fname),
            std::filesystem::canonical(ODBCPP_HOST_CRYPTO));
  using Version = const char* (*)(int);
  const auto host_version = reinterpret_cast<Version>(version_symbol);
  const std::string before = host_version(0);
  ASSERT_EQ(before.find("OpenSSL 3."), 0u) << before;

  using Digest = unsigned char* (*)(const unsigned char*, std::size_t, unsigned char*);
  const auto host_digest = reinterpret_cast<Digest>(dlsym(host.handle, "SHA256"));
  ASSERT_NE(host_digest, nullptr);
  const std::array<unsigned char, 3> input{'a', 'b', 'c'};
  const std::array<unsigned char, 32> expected{
      0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  std::array<unsigned char, 32> output{};
  ASSERT_EQ(host_digest(input.data(), input.size(), output.data()), output.data());
  EXPECT_EQ(output, expected);

  LoadedLibrary host_ssl(ODBCPP_HOST_SSL);
  ASSERT_NE(host_ssl.handle, nullptr) << "system OpenSSL SSL must already be preloaded";
  auto check_host_tls = [&] {
    const auto method = reinterpret_cast<const void* (*)()>(dlsym(host_ssl.handle, "TLS_client_method"));
    const auto create = reinterpret_cast<void* (*)(const void*)>(dlsym(host_ssl.handle, "SSL_CTX_new"));
    const auto destroy = reinterpret_cast<void (*)(void*)>(dlsym(host_ssl.handle, "SSL_CTX_free"));
    const auto ciphers = reinterpret_cast<int (*)(void*, const char*)>(dlsym(host_ssl.handle, "SSL_CTX_set_cipher_list"));
    ASSERT_NE(method, nullptr);
    ASSERT_NE(create, nullptr);
    ASSERT_NE(destroy, nullptr);
    ASSERT_NE(ciphers, nullptr);
    EXPECT_EQ(dlsym(RTLD_DEFAULT, "SSL_CTX_new"), dlsym(host_ssl.handle, "SSL_CTX_new"));
    Dl_info ssl_origin{};
    ASSERT_NE(dladdr(dlsym(host_ssl.handle, "SSL_CTX_new"), &ssl_origin), 0);
    ASSERT_NE(ssl_origin.dli_fname, nullptr);
    EXPECT_EQ(std::filesystem::canonical(ssl_origin.dli_fname),
              std::filesystem::canonical(ODBCPP_HOST_SSL));
    std::unique_ptr<void, decltype(destroy)> context(create(method()), destroy);
    ASSERT_NE(context, nullptr);
    EXPECT_EQ(ciphers(context.get(), "ECDHE-RSA-AES128-GCM-SHA256"), 1);
  };
  check_host_tls();

  const auto identity = rs::core::security::crypto_provider_identity();
  EXPECT_EQ(identity.provider, "AWS_LC");
  EXPECT_EQ(identity.runtime_version.find("AWS-LC"), 0u) << identity.runtime_version;
  EXPECT_FALSE(identity.fips_enabled);
  EXPECT_EQ(rs::core::security::sha256(input), expected);

  EXPECT_EQ(dlsym(RTLD_DEFAULT, "OpenSSL_version"), version_symbol);
  EXPECT_EQ(std::string(host_version(0)), before);
  output.fill(0);
  ASSERT_EQ(host_digest(input.data(), input.size(), output.data()), output.data());
  EXPECT_EQ(output, expected);
  check_host_tls();
}
} // namespace
