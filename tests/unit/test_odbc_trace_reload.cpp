#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#include <sql.h>

#include <cstdlib>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#ifdef ODBCPP_TRACE_RELOAD_EXIT_POLICY_CONTROL
// Compile-time isolated controls exercise the same main/CTest policy below.
// These names can never satisfy the real loader evidence inventory.
TEST(TraceReloadExitPolicyControl, Pass) { SUCCEED(); }
TEST(TraceReloadExitPolicyControl, SkipA) { GTEST_SKIP() << "synthetic skip A"; }
TEST(TraceReloadExitPolicyControl, SkipB) { GTEST_SKIP() << "synthetic skip B"; }
TEST(TraceReloadExitPolicyControl, Failure) { ADD_FAILURE() << "synthetic failure"; }
TEST(TraceReloadExitPolicyControl, FailureThenSkip) {
  ADD_FAILURE() << "synthetic failure before skip";
  GTEST_SKIP() << "must not hide the failure";
}
TEST(TraceReloadExitPolicyControl, AdHocFailureAndSkip) {
  GTEST_SKIP() << "must not hide an out-of-test failure";
}
TEST(TraceReloadExitPolicyControl, SkipTextFailure) {
  ADD_FAILURE() << "[  SKIPPED ] this text must not override failure";
}
#else
// Loads the real driver library the way a Driver Manager does, without linking
// the driver's static core, so unload runs the library's own shutdown path.
namespace {

using AllocHandle = SQLRETURN (SQL_API*)(SQLSMALLINT, SQLHANDLE, SQLHANDLE*);
using FreeHandle = SQLRETURN (SQL_API*)(SQLSMALLINT, SQLHANDLE);

#ifdef _WIN32
// LOAD_WITH_ALTERED_SEARCH_PATH and full-path GetModuleHandle both require a
// native backslash path; CMake's generator expression uses forward slashes.
std::wstring native_driver_path() {
  return std::filesystem::path(ODBCPP_DRIVER_LIBRARY_PATH).make_preferred().wstring();
}
#endif

class DriverLibrary {
public:
  bool load() {
#ifdef _WIN32
    handle_ = LoadLibraryExW(native_driver_path().c_str(), nullptr,
                             LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    handle_ = dlopen(ODBCPP_DRIVER_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
#endif
    return handle_ != nullptr;
  }

  template <typename Function>
  Function symbol(const char* name) const {
#ifdef _WIN32
    return reinterpret_cast<Function>(
        GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return reinterpret_cast<Function>(dlsym(handle_, name));
#endif
  }

  bool unload() {
    if (!handle_) return true;
#ifdef _WIN32
    const bool unloaded = FreeLibrary(static_cast<HMODULE>(handle_)) != FALSE;
#else
    const bool unloaded = dlclose(handle_) == 0;
#endif
    handle_ = nullptr;
    return unloaded;
  }

  // True when the loader actually removed the image after unload().
  static bool resident() {
#ifdef _WIN32
    return GetModuleHandleW(native_driver_path().c_str()) != nullptr;
#else
    void* probe = dlopen(ODBCPP_DRIVER_LIBRARY_PATH, RTLD_NOW | RTLD_NOLOAD);
    if (probe) dlclose(probe);
    return probe != nullptr;
#endif
  }

  ~DriverLibrary() { unload(); }

private:
  void* handle_ = nullptr;
};

void set_environment(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty()) {
    unsetenv(name);
  } else {
    setenv(name, value.c_str(), 1);
  }
#endif
}

// Restores the process environment and removes the directory on every exit
// path, including a failed ASSERT.
struct TraceEnvironment {
  explicit TraceEnvironment(const std::filesystem::path& directory_path)
      : directory(directory_path) {
    std::filesystem::create_directories(directory);
    set_environment("ODBCPP_TRACE", "1");
    set_environment("ODBCPP_TRACE_FILE", (directory / "trace.log").string());
    // Overrides any registered driver-section format so text matching holds.
    set_environment("ODBCPP_TRACE_FORMAT", "Text");
  }
  ~TraceEnvironment() {
    for (const char* name : {"ODBCPP_TRACE", "ODBCPP_TRACE_FILE", "ODBCPP_TRACE_FORMAT"}) {
      set_environment(name, "");
    }
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  TraceEnvironment(const TraceEnvironment&) = delete;
  TraceEnvironment& operator=(const TraceEnvironment&) = delete;
  std::filesystem::path directory;
};

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::size_t count(const std::string& text, std::string_view needle) {
  std::size_t found = 0;
  for (auto at = text.find(needle); at != std::string::npos;
       at = text.find(needle, at + needle.size())) {
    ++found;
  }
  return found;
}

TEST(OdbcTraceReload, UnloadClosesTheTraceFileAndReloadStartsFresh) {
#ifdef _WIN32
  const auto process = _getpid();
#else
  const auto process = getpid();
#endif
  const TraceEnvironment environment(std::filesystem::temp_directory_path() /
      ("odbcpp-trace-reload-" + std::to_string(process)));
  const auto& directory = environment.directory;
  const auto path = directory / "trace.log";

  bool stayed_resident = false;
  for (int cycle = 0; cycle < 2 && !stayed_resident; ++cycle) {
    SCOPED_TRACE(cycle);
    DriverLibrary driver;
    ASSERT_TRUE(driver.load()) << ODBCPP_DRIVER_LIBRARY_PATH;
    const auto alloc = driver.symbol<AllocHandle>("SQLAllocHandle");
    const auto release = driver.symbol<FreeHandle>("SQLFreeHandle");
    ASSERT_NE(nullptr, alloc);
    ASSERT_NE(nullptr, release);
    SQLHANDLE odbc_environment = SQL_NULL_HANDLE;
    ASSERT_EQ(SQL_SUCCESS, alloc(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &odbc_environment));
    EXPECT_EQ(SQL_SUCCESS, release(SQL_HANDLE_ENV, odbc_environment));
    ASSERT_TRUE(driver.unload());
    if (DriverLibrary::resident()) {
      stayed_resident = true;
      break;
    }

    const auto contents = read_file(path);
    EXPECT_EQ(2u, count(contents, "event=odbc_trace_enter"));
    EXPECT_EQ(2u, count(contents, "event=odbc_trace_exit"));
    // A fresh image resolves its own tracer: call identifiers restart.
    EXPECT_NE(std::string::npos, contents.find("call=\"1\""));
    EXPECT_EQ(std::string::npos, contents.find("call=\"3\""));
    // Windows refuses to rename an open file; unload must have closed it.
    std::error_code renamed;
    std::filesystem::rename(path, directory / ("cycle-" + std::to_string(cycle) + ".log"),
                            renamed);
    EXPECT_FALSE(renamed) << renamed.message();
  }

  if (stayed_resident) {
    RecordProperty("odbcpp_trace_reload_boundary", "driver_image_retained");
    GTEST_SKIP() << "The platform loader kept the driver image resident after "
                    "unload; unload-time sink closure was not observable.";
  }
}

TEST(OdbcTraceReload, DisabledTracingAlsoRetiresStorageAcrossImageReloads) {
#ifdef _WIN32
  const auto process = _getpid();
#else
  const auto process = getpid();
#endif
  const TraceEnvironment environment(std::filesystem::temp_directory_path() /
      ("odbcpp-trace-disabled-reload-" + std::to_string(process)));
  set_environment("ODBCPP_TRACE", "0");
  for (int cycle = 0; cycle < 2; ++cycle) {
    SCOPED_TRACE(cycle);
    DriverLibrary driver;
    ASSERT_TRUE(driver.load()) << ODBCPP_DRIVER_LIBRARY_PATH;
    const auto alloc = driver.symbol<AllocHandle>("SQLAllocHandle");
    const auto release = driver.symbol<FreeHandle>("SQLFreeHandle");
    ASSERT_NE(nullptr, alloc);
    ASSERT_NE(nullptr, release);
    SQLHANDLE odbc_environment = SQL_NULL_HANDLE;
    ASSERT_EQ(SQL_SUCCESS, alloc(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &odbc_environment));
    EXPECT_EQ(SQL_SUCCESS, release(SQL_HANDLE_ENV, odbc_environment));
    ASSERT_TRUE(driver.unload());
    if (DriverLibrary::resident()) {
      RecordProperty("odbcpp_trace_reload_boundary", "driver_image_retained");
      GTEST_SKIP() << "The loader retained the image; disabled unload cleanup was not observable.";
    }
    EXPECT_FALSE(std::filesystem::exists(environment.directory / "trace.log"));
  }
}

}  // namespace
#endif

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // Listing is inventory only. It emits no result marker and cannot qualify a
  // run; preserve the standard interface used by native inventory collectors.
  if (GTEST_FLAG_GET(list_tests)) {
    return RUN_ALL_TESTS();
  }
  if (GTEST_FLAG_GET(output).empty()) {
    GTEST_FLAG_SET(output, "xml:odbcpp-trace-reload-gtest.xml");
  }
#ifdef _WIN32
  const auto process = _getpid();
#else
  const auto process = getpid();
#endif
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto run = std::to_string(process) + "-" + std::to_string(ticks);
  // Standard GoogleTest XML records this root property. Match it to the one
  // post-run marker in CTest's hashed stdout, refusing a stale separate XML.
  ::testing::Test::RecordProperty("odbcpp_trace_reload_run", run);
#ifdef ODBCPP_TRACE_RELOAD_EXIT_POLICY_CONTROL
  if (GTEST_FLAG_GET(filter) == "TraceReloadExitPolicyControl.AdHocFailureAndSkip") {
    ADD_FAILURE() << "synthetic out-of-test failure before RUN_ALL_TESTS";
  }
#endif
  // Do not let a repeated or zero-iteration invocation advertise the same
  // complete inventory as one real selected execution.
  const int native_result = GTEST_FLAG_GET(repeat) == 1 ? RUN_ALL_TESTS() : 1;
  const auto& unit = *::testing::UnitTest::GetInstance();
  const auto selected = unit.test_to_run_count();
  const auto passed = unit.successful_test_count();
  const auto skipped = unit.skipped_test_count();
  const auto failed = unit.failed_test_count();
  const auto disabled = unit.disabled_test_count();
  int result = native_result;
  if (result == 0) {
    if (selected == 0 || disabled != 0 || passed + skipped + failed != selected) {
      result = 1;
    } else if (skipped == selected && failed == 0) {
      result = 77;
    } else if (skipped != 0 || failed != 0) {
      result = 1; // A partial skip is neither a complete pass nor all-skipped.
    }
  }
  std::printf("ODBCPP_TRACE_RELOAD_RESULT_V1 run=%s selected=%d passed=%d skipped=%d "
              "failed=%d disabled=%d result=%d exit=%d\n",
              run.c_str(), selected, passed, skipped, failed, disabled, native_result, result);
  return result;
}
