#include <gtest/gtest.h>

#include <dlfcn.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <sql.h>
#include <sqlext.h>

#include <array>
#include <iostream>
#include <string>

namespace {
class Library {
 public:
  explicit Library(const char* path) : handle_(dlopen(path, RTLD_NOW | RTLD_GLOBAL)) {}
  ~Library() { if (handle_) dlclose(handle_); }
  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;
  explicit operator bool() const { return handle_ != nullptr; }
  void* symbol(const char* name) const { return dlsym(handle_, name); }
 private:
  void* handle_{};
};

void exercise_host(const Library& crypto, const Library& ssl) {
  using Version = decltype(&OpenSSL_version);
  using Sha256 = decltype(&SHA256);
  using Method = decltype(&TLS_client_method);
  using NewContext = decltype(&SSL_CTX_new);
  using FreeContext = decltype(&SSL_CTX_free);
  const auto version = reinterpret_cast<Version>(crypto.symbol("OpenSSL_version"));
  const auto sha = reinterpret_cast<Sha256>(crypto.symbol("SHA256"));
  const auto method = reinterpret_cast<Method>(ssl.symbol("TLS_client_method"));
  const auto create = reinterpret_cast<NewContext>(ssl.symbol("SSL_CTX_new"));
  const auto destroy = reinterpret_cast<FreeContext>(ssl.symbol("SSL_CTX_free"));
  ASSERT_NE(version, nullptr);
  ASSERT_NE(sha, nullptr);
  ASSERT_NE(method, nullptr);
  ASSERT_NE(create, nullptr);
  ASSERT_NE(destroy, nullptr);
  EXPECT_EQ(dlsym(RTLD_DEFAULT, "OpenSSL_version"), crypto.symbol("OpenSSL_version"));
  EXPECT_EQ(dlsym(RTLD_DEFAULT, "SSL_CTX_new"), ssl.symbol("SSL_CTX_new"));
  const std::string prefix = "OpenSSL " ODBCPP_COHABITATION_VERSION " ";
  EXPECT_EQ(std::string(version(0)).find(prefix), 0U);
  const std::array<unsigned char, 32> expected = {
      0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
      0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  std::array<unsigned char, 32> digest{};
  ASSERT_EQ(sha(reinterpret_cast<const unsigned char*>("abc"), 3, digest.data()), digest.data());
  EXPECT_EQ(digest, expected);
  auto* context = create(method());
  ASSERT_NE(context, nullptr);
  destroy(context);
}

void exercise_driver(const Library& driver, const Library& crypto, const Library& ssl) {
  // SYSTEM_SHARED deliberately shares the configured provider with this host.
  EXPECT_EQ(driver.symbol("OpenSSL_version"), crypto.symbol("OpenSSL_version"));
  EXPECT_EQ(driver.symbol("SSL_CTX_new"), ssl.symbol("SSL_CTX_new"));
  using Alloc = SQLRETURN (*)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*);
  using Free = SQLRETURN (*)(SQLSMALLINT, SQLHANDLE);
  const auto alloc = reinterpret_cast<Alloc>(driver.symbol("SQLAllocHandle"));
  const auto free = reinterpret_cast<Free>(driver.symbol("SQLFreeHandle"));
  ASSERT_NE(alloc, nullptr);
  ASSERT_NE(free, nullptr);
  SQLHENV env = SQL_NULL_HENV;
  ASSERT_EQ(alloc(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env), SQL_SUCCESS);
  EXPECT_EQ(free(SQL_HANDLE_ENV, env), SQL_SUCCESS);
}

TEST(SharedCryptoCohabitationTest, PreservesHostOperationsAcrossDriverLifetime) {
  Library crypto(ODBCPP_COHABITATION_CRYPTO_LIBRARY);
  ASSERT_TRUE(crypto) << dlerror();
  Library ssl(ODBCPP_COHABITATION_SSL_LIBRARY);
  ASSERT_TRUE(ssl) << dlerror();
  exercise_host(crypto, ssl);
  {
    Library driver(ODBCPP_DRIVER_LIBRARY_PATH);
    ASSERT_TRUE(driver) << dlerror();
    exercise_driver(driver, crypto, ssl);
    exercise_host(crypto, ssl);
  }
  exercise_host(crypto, ssl);
  std::cout << "COHABITATION: host operations across driver lifetime passed\n";
}

TEST(SharedCryptoCohabitationTest, RepeatedDriverReferencesPreserveHostOwnership) {
  Library crypto(ODBCPP_COHABITATION_CRYPTO_LIBRARY);
  ASSERT_TRUE(crypto) << dlerror();
  Library ssl(ODBCPP_COHABITATION_SSL_LIBRARY);
  ASSERT_TRUE(ssl) << dlerror();
  for (int i = 0; i < 3; ++i) {
    Library first(ODBCPP_DRIVER_LIBRARY_PATH);
    ASSERT_TRUE(first) << dlerror();
    {
      Library second(ODBCPP_DRIVER_LIBRARY_PATH);
      ASSERT_TRUE(second) << dlerror();
      exercise_driver(second, crypto, ssl);
    }
    exercise_driver(first, crypto, ssl);
    exercise_host(crypto, ssl);
  }
  exercise_host(crypto, ssl);
  std::cout << "COHABITATION: repeated driver references passed\n";
}
}  // namespace
