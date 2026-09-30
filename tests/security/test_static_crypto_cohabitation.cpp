#include <gtest/gtest.h>

#include <dlfcn.h>
#include <sql.h>
#include <sqlext.h>

namespace {

class DynamicLibrary {
 public:
  DynamicLibrary(const char* path, int flags) : handle_(dlopen(path, flags)) {}
  ~DynamicLibrary() {
    if (handle_) dlclose(handle_);
  }

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;

  explicit operator bool() const { return handle_ != nullptr; }
  void* symbol(const char* name) const { return dlsym(handle_, name); }

 private:
  void* handle_{};
};

TEST(StaticCryptoCohabitationTest,
     PreservesPreloadedProviderAndLoadsOnlyOdbcSurface) {
  DynamicLibrary preloaded_crypto(ODBCPP_COHABITATION_CRYPTO_LIBRARY,
                                  RTLD_NOW | RTLD_GLOBAL);
  ASSERT_TRUE(preloaded_crypto) << dlerror();
  DynamicLibrary preloaded_ssl(ODBCPP_COHABITATION_SSL_LIBRARY,
                               RTLD_NOW | RTLD_GLOBAL);
  ASSERT_TRUE(preloaded_ssl) << dlerror();

  auto* const provider_before = dlsym(RTLD_DEFAULT, "OpenSSL_version");
  ASSERT_NE(provider_before, nullptr);
  EXPECT_EQ(provider_before, preloaded_crypto.symbol("OpenSSL_version"));

  DynamicLibrary driver(ODBCPP_DRIVER_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
  ASSERT_TRUE(driver) << dlerror();

  EXPECT_EQ(driver.symbol("OpenSSL_version"), nullptr);
  EXPECT_EQ(dlsym(RTLD_DEFAULT, "OpenSSL_version"), provider_before);

  using AllocHandle = SQLRETURN (*)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*);
  using SetEnvAttr = SQLRETURN (*)(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER);
  using FreeHandle = SQLRETURN (*)(SQLSMALLINT, SQLHANDLE);
  const auto alloc_handle = reinterpret_cast<AllocHandle>(
      driver.symbol("SQLAllocHandle"));
  const auto set_env_attr = reinterpret_cast<SetEnvAttr>(
      driver.symbol("SQLSetEnvAttr"));
  const auto free_handle = reinterpret_cast<FreeHandle>(
      driver.symbol("SQLFreeHandle"));
  ASSERT_NE(alloc_handle, nullptr);
  ASSERT_NE(set_env_attr, nullptr);
  ASSERT_NE(free_handle, nullptr);

  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment),
            SQL_SUCCESS);
  ASSERT_EQ(set_env_attr(environment, SQL_ATTR_ODBC_VERSION,
                         reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0),
            SQL_SUCCESS);
  ASSERT_EQ(alloc_handle(SQL_HANDLE_DBC, environment, &connection), SQL_SUCCESS);
  EXPECT_EQ(free_handle(SQL_HANDLE_DBC, connection), SQL_SUCCESS);
  EXPECT_EQ(free_handle(SQL_HANDLE_ENV, environment), SQL_SUCCESS);
}

}  // namespace
