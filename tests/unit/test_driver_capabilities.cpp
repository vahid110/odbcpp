#include "odbc/odbc_types.h"
#include "tests/test_driver_exports.h"

#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

class DynamicLibrary {
 public:
#ifdef _WIN32
  using Symbol = FARPROC;
#else
  using Symbol = void*;
#endif

  explicit DynamicLibrary(const char* path) {
#ifdef _WIN32
    handle_ = LoadLibraryA(path);
#else
    handle_ = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
  }

  ~DynamicLibrary() {
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(handle_);
#else
    dlclose(handle_);
#endif
  }

  bool valid() const { return handle_ != nullptr; }

  Symbol symbol(const char* name) const {
#ifdef _WIN32
    return GetProcAddress(handle_, name);
#else
    return dlsym(handle_, name);
#endif
  }

 private:
#ifdef _WIN32
  HMODULE handle_{};
#else
  void* handle_{};
#endif
};

TEST(DriverCapabilitiesTest, AdvertisesExactlyItsBaseExports) {
  DynamicLibrary driver(ODBCPP_DRIVER_LIBRARY_PATH);
  ASSERT_TRUE(driver.valid()) << ODBCPP_DRIVER_LIBRARY_PATH;

  for (const auto& function : odbcpp::test::advertised_functions) {
    EXPECT_NE(nullptr, driver.symbol(function.name)) << function.name;
  }
  for (const auto* name : odbcpp::test::wide_exports) {
    EXPECT_NE(nullptr, driver.symbol(name)) << name;
  }

  using GetFunctions = SQLRETURN(SQL_API*)(SQLHDBC, SQLUSMALLINT,
                                          SQLUSMALLINT*);
  const auto get_functions = reinterpret_cast<GetFunctions>(
      driver.symbol("SQLGetFunctions"));
  ASSERT_NE(nullptr, get_functions);

  SQLUSMALLINT supported = SQL_TRUE;
  EXPECT_EQ(SQL_INVALID_HANDLE,
            get_functions(SQL_NULL_HDBC, SQL_API_SQLCONNECT, &supported));
}

}  // namespace
