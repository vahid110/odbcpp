#include "tests/test_live_package_inputs.h"
#include <gtest/gtest.h>

#ifdef _WIN32
#include <sddl.h>
#endif

#include <chrono>
#include <fstream>
#include <string_view>

namespace {
namespace fs = std::filesystem;
namespace inputs = odbcpp::test::live_package_inputs;

class LivePackageInputTest : public ::testing::Test {
protected:
  fs::path directory_, file_, alias_;
  bool directory_owned_ = false;
#ifdef _WIN32
  std::wstring sid_;

  bool set_security(bool set_owner, bool read_only = false, bool everyone = false,
                    bool inherit_only_write = false) {
    const auto sddl = L"O:" + sid_ + L"D:P(A;;" +
        std::wstring(read_only ? L"FR" : L"FA") + L";;;" + sid_ +
        L")(A;;FA;;;SY)(A;;FA;;;BA)" + (everyone ? L"(A;;FR;;;WD)" : L"") +
        (inherit_only_write ? L"(A;IO;FW;;;" + sid_ + L")" : L"");
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
        SDDL_REVISION_1, &descriptor, nullptr)) {
      return false;
    }
    const auto flags = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION |
        (set_owner ? OWNER_SECURITY_INFORMATION : 0);
    const bool success = SetFileSecurityW(file_.c_str(), flags, descriptor) != FALSE;
    LocalFree(descriptor);
    return success;
  }
#endif

  bool write(std::string_view content) {
    std::ofstream output(file_, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) { return false; }
#ifdef _WIN32
    return set_security(true);
#else
    return ::chmod(file_.c_str(), 0600) == 0;
#endif
  }

  void SetUp() override {
#ifdef _WIN32
    inputs::OwnedHandle token;
    ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value));
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    ASSERT_GT(size, 0u);
    std::vector<unsigned char> data(size);
    ASSERT_TRUE(GetTokenInformation(token.value, TokenUser, data.data(), size, &size));
    LPWSTR text = nullptr;
    ASSERT_TRUE(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,
        &text));
    sid_ = text;
    LocalFree(text);
    const auto process = GetCurrentProcessId();
#else
    const auto process = ::getpid();
#endif
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = fs::temp_directory_path() / ("odbcpp-private-live-" +
        std::to_string(process) + "-" + std::to_string(nonce));
    directory_owned_ = fs::create_directory(directory_);
    ASSERT_TRUE(directory_owned_);
    directory_ = fs::canonical(directory_);
    file_ = directory_ / "connection.txt";
    alias_ = directory_ / "alias.txt";
    ASSERT_TRUE(write("DRIVER={test};SERVER=local;"));
  }

  void TearDown() override {
    if (!directory_owned_) { return; }
    std::error_code error;
    if (!file_.empty() && fs::exists(file_, error)) {
#ifdef _WIN32
      EXPECT_TRUE(set_security(false));
#else
      EXPECT_EQ(0, ::chmod(file_.c_str(), 0600));
#endif
    }
    if (!alias_.empty()) { fs::remove(alias_, error); }
    if (!file_.empty()) { fs::remove(file_, error); }
    if (!directory_.empty()) {
      EXPECT_TRUE(fs::remove(directory_, error));
    }
  }
};

TEST_F(LivePackageInputTest, ReadsExactOwnedPrivateBytesAndRejectsRelativePath) {
  const auto content = inputs::read_private_connection_file(file_);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ("DRIVER={test};SERVER=local;", *content);
  EXPECT_FALSE(inputs::read_private_connection_file(file_.filename()).has_value());
}

TEST_F(LivePackageInputTest, RejectsEmptyOversizeAndEmbeddedControlBytes) {
  for (const auto& content : std::vector<std::string>{"", std::string(4097, 'x'),
      "x\n", "x\r", std::string("x\0y", 3)}) {
    ASSERT_TRUE(write(content));
    EXPECT_FALSE(inputs::read_private_connection_file(file_).has_value());
  }
  ASSERT_TRUE(write(std::string(4096, 'x')));
  const auto content = inputs::read_private_connection_file(file_);
  ASSERT_TRUE(content.has_value());
  EXPECT_EQ(4096u, content->size());
}

TEST_F(LivePackageInputTest, RejectsExtraHardLinkAndAcceptsAfterItsRemoval) {
  std::error_code error;
  fs::create_hard_link(file_, alias_, error);
  ASSERT_FALSE(error) << error.message();
  EXPECT_FALSE(inputs::read_private_connection_file(file_).has_value());
  ASSERT_TRUE(fs::remove(alias_));
  EXPECT_TRUE(inputs::read_private_connection_file(file_).has_value());
}

TEST_F(LivePackageInputTest, RejectsBroaderReadPermission) {
#ifdef _WIN32
  ASSERT_TRUE(set_security(false, false, true));
#else
  ASSERT_EQ(0, ::chmod(file_.c_str(), 0644));
#endif
  EXPECT_FALSE(inputs::read_private_connection_file(file_).has_value());
}

TEST_F(LivePackageInputTest, RequiresOwnerWritePermission) {
#ifdef _WIN32
  ASSERT_TRUE(set_security(false, true));
#else
  ASSERT_EQ(0, ::chmod(file_.c_str(), 0400));
#endif
  EXPECT_FALSE(inputs::read_private_connection_file(file_).has_value());
}

#ifndef _WIN32
TEST_F(LivePackageInputTest, RejectsSymbolicLink) {
  fs::create_symlink(file_, alias_);
  EXPECT_FALSE(inputs::read_private_connection_file(alias_).has_value());
}
#else
TEST_F(LivePackageInputTest, InheritOnlyWriteDoesNotSupplyEffectiveOwnerWrite) {
  ASSERT_TRUE(set_security(false, true, false, true));
  EXPECT_FALSE(inputs::read_private_connection_file(file_).has_value());
}

TEST_F(LivePackageInputTest, RegistrySelectionRequiresExactRegularImageAndStringValue) {
  const auto key_name = L"Software\\ODBCPP_Live_Input_" + directory_.filename().native();
  struct Registration {
    std::wstring name;
    inputs::OwnedRegistryKey key;
    bool created = false;
    ~Registration() {
      if (key.value) { RegCloseKey(key.value); key.value = nullptr; }
      if (created) {
        EXPECT_EQ(ERROR_SUCCESS, RegDeleteKeyExW(HKEY_CURRENT_USER, name.c_str(),
            inputs::native_registry_view, 0));
      }
    }
  } registration{key_name};
  DWORD disposition = 0;
  ASSERT_EQ(ERROR_SUCCESS, RegCreateKeyExW(HKEY_CURRENT_USER, key_name.c_str(), 0,
      nullptr, REG_OPTION_VOLATILE, KEY_SET_VALUE | KEY_QUERY_VALUE | inputs::native_registry_view,
      nullptr, &registration.key.value, &disposition));
  registration.created = disposition == REG_CREATED_NEW_KEY;
  ASSERT_TRUE(registration.created);
  EXPECT_FALSE(inputs::registered_driver_path_matches(HKEY_CURRENT_USER, key_name, file_));
  const auto set_value = [&](const std::wstring& value, DWORD type = REG_SZ) {
    return RegSetValueExW(registration.key.value, L"Driver", 0, type,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
  };
  ASSERT_TRUE(set_value(file_.native()));
  EXPECT_TRUE(inputs::registered_driver_path_matches(HKEY_CURRENT_USER, key_name, file_));
  EXPECT_FALSE(inputs::registered_driver_path_matches(HKEY_CURRENT_USER, key_name, file_.filename()));
  // The temporary fixture and a Windows CI checkout may be on different drives.
  // Use the fixture's directory so the relative value still resolves to the
  // exact existing file, preserving the rejection assertion below.
  struct RestoreCurrentDirectory {
    fs::path previous = fs::current_path();
    ~RestoreCurrentDirectory() {
      std::error_code error;
      fs::current_path(previous, error);
      EXPECT_FALSE(error) << error.message();
    }
  } restore_current_directory;
  fs::current_path(directory_);
  const auto relative = fs::relative(file_, fs::current_path());
  ASSERT_FALSE(relative.empty());
  ASSERT_TRUE(relative.is_relative());
  ASSERT_EQ(file_, fs::canonical(relative));
  ASSERT_TRUE(set_value(relative.native()));
  EXPECT_FALSE(inputs::registered_driver_path_matches(HKEY_CURRENT_USER, key_name, file_));
  ASSERT_TRUE(set_value((directory_ / "missing.dll").native()));
  EXPECT_FALSE(inputs::registered_driver_path_matches(HKEY_CURRENT_USER, key_name, file_));
  ASSERT_TRUE(set_value(file_.native(), REG_EXPAND_SZ));
  EXPECT_FALSE(inputs::registered_driver_path_matches(HKEY_CURRENT_USER, key_name, file_));
  ASSERT_TRUE(set_value(file_.native() + std::wstring(1, L'\0') + L"extra"));
  EXPECT_FALSE(inputs::registered_driver_path_matches(HKEY_CURRENT_USER, key_name, file_));
  ASSERT_TRUE(set_value(L""));
  EXPECT_FALSE(inputs::registered_driver_path_matches(HKEY_CURRENT_USER, key_name, file_));
  for (const auto* selection : {"", "ODBCPP\\escape", "ODBCPP/escape", " name", "name "}) {
    EXPECT_FALSE(inputs::connection_driver_matches(selection, file_));
  }
}
#endif

TEST(LivePackageLengthsTest, PreservesRepresentableLengthsAndRefusesNarrowing) {
  EXPECT_EQ(0, inputs::fixture_smallint_length(0));
  EXPECT_EQ(32767, inputs::fixture_smallint_length(32767));
  EXPECT_THROW(inputs::fixture_smallint_length(32768), std::length_error);
  EXPECT_THROW(inputs::fixture_smallint_length(std::numeric_limits<std::size_t>::max()),
      std::length_error);
}

TEST(LivePackageImagesTest, ObservesActualExecutableExactlyOnce) {
  fs::path expected;
#ifdef _WIN32
  std::array<wchar_t, 32768> executable_path{};
  const auto length = GetModuleFileNameW(nullptr, executable_path.data(), static_cast<DWORD>(executable_path.size()));
  ASSERT_GT(length, 0u);
  ASSERT_LT(length, executable_path.size());
  expected = fs::path(std::wstring(executable_path.data(), length));
#elif defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  ASSERT_GT(size, 0u);
  std::vector<char> executable_path(size);
  ASSERT_EQ(0, _NSGetExecutablePath(executable_path.data(), &size));
  expected = fs::path(executable_path.data());
#else
  expected = fs::read_symlink("/proc/self/exe");
#endif
  const auto images = inputs::loaded_image_paths();
  ASSERT_TRUE(images.has_value());
  std::size_t matches = 0;
  for (const auto& path : *images) {
    std::error_code error;
    if (fs::equivalent(expected, path, error) && !error) { ++matches; }
  }
  EXPECT_EQ(1u, matches);
}

TEST(LivePackageImagesTest, ClassifiesDatabaseDriversWithoutOtherLibraries) {
  EXPECT_TRUE(inputs::is_database_driver_image("libodbcpp.so"));
  EXPECT_TRUE(inputs::is_database_driver_image("libodbcpp.dylib"));
  EXPECT_TRUE(inputs::is_database_driver_image("libredshiftodbc.dylib"));
  EXPECT_TRUE(inputs::is_database_driver_image("librsodbc64.dylib"));
  EXPECT_TRUE(inputs::is_database_driver_image("psqlodbc.dll"));
  EXPECT_FALSE(inputs::is_database_driver_image("odbc32.dll"));
  EXPECT_FALSE(inputs::is_database_driver_image("libiodbc.so.2"));
  EXPECT_FALSE(inputs::is_database_driver_image("libcrypto-3-x64.dll"));
#ifdef _WIN32
  EXPECT_TRUE(inputs::is_database_driver_image("ODBCPP.DLL"));
  EXPECT_TRUE(inputs::is_database_driver_image("AmazonRedshiftODBC64.dll"));
  EXPECT_TRUE(inputs::is_database_driver_image("PSQLODBC35W.DLL"));
#endif
}

TEST(LivePackageEnvironmentTest, OwnsValuesAcrossEnvironmentChangesAndPreservesAbsence) {
#ifdef _WIN32
  const auto process = GetCurrentProcessId();
#else
  const auto process = ::getpid();
#endif
  const auto name = "ODBCPP_LIVE_INPUT_TEST_" + std::to_string(process);
  ASSERT_FALSE(inputs::fixture_environment(name.c_str()).has_value());
  struct Restore {
    std::string name;
    ~Restore() {
#ifdef _WIN32
      _putenv_s(name.c_str(), "");
#else
      ::unsetenv(name.c_str());
#endif
    }
  } restore{name};
  const auto set = [&](const char* value) {
#ifdef _WIN32
    return _putenv_s(name.c_str(), value) == 0;
#else
    return ::setenv(name.c_str(), value, 1) == 0;
#endif
  };
  ASSERT_TRUE(set("first"));
  const auto owned = inputs::fixture_environment(name.c_str());
  ASSERT_TRUE(owned.has_value());
  ASSERT_TRUE(set("second"));
  EXPECT_EQ("first", *owned);
  EXPECT_EQ("second", inputs::fixture_environment(name.c_str()));
#ifndef _WIN32
  ASSERT_TRUE(set(""));
  const auto empty = inputs::fixture_environment(name.c_str());
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
#endif
}

}  // namespace
