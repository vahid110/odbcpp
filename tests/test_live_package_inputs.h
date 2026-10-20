#pragma once

// Acceptance-fixture custody only. Production code does not depend on this file.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <tlhelp32.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <link.h>
#endif
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace odbcpp::test::live_package_inputs {

// ODBC fixture inputs use a signed 16-bit length, even on 64-bit Windows.
inline std::int16_t fixture_smallint_length(std::size_t size) {
  if (size > static_cast<std::size_t>(std::numeric_limits<std::int16_t>::max())) {
    throw std::length_error("Live fixture input exceeds the ODBC small length range");
  }
  return static_cast<std::int16_t>(size);
}

inline std::optional<std::string> fixture_environment(const char* name) {
#ifdef _WIN32
  char* raw = nullptr;
  std::size_t size = 0;
  const auto status = _dupenv_s(&raw, &size, name);
  const std::unique_ptr<char, decltype(&std::free)> owned(raw, &std::free);
  if (status != 0) { throw std::runtime_error("Could not read live fixture environment"); }
  if (!raw) { return std::nullopt; }
  return std::string(raw);
#else
  const char* raw = std::getenv(name);
  if (!raw) { return std::nullopt; }
  return std::string(raw);
#endif
}

inline bool canonical_file(const std::filesystem::path& path) {
  std::error_code error;
  return path.is_absolute() && std::filesystem::canonical(path, error) == path &&
      !error && std::filesystem::is_regular_file(path, error) && !error;
}

inline bool is_database_driver_image(const std::filesystem::path& path) {
  auto filename = path.filename().string();
#ifdef _WIN32
  // Windows driver basenames are ASCII and case insensitive.
  for (auto& character : filename) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
#endif
  return filename == "odbcpp.dll" || filename == "librsodbc64.dylib" ||
      filename.starts_with("libodbcpp") ||
      filename.find("redshiftodbc") != std::string::npos ||
      filename.find("psqlodbc") != std::string::npos;
}

#ifdef _WIN32
struct OwnedHandle {
  HANDLE value = INVALID_HANDLE_VALUE;
  OwnedHandle() = default;
  explicit OwnedHandle(HANDLE handle) : value(handle) {}
  OwnedHandle(const OwnedHandle&) = delete;
  OwnedHandle& operator=(const OwnedHandle&) = delete;
  ~OwnedHandle() {
    if (value != INVALID_HANDLE_VALUE && value != nullptr) { CloseHandle(value); }
  }
};

inline constexpr REGSAM native_registry_view = sizeof(void*) == 8
    ? KEY_WOW64_64KEY : KEY_WOW64_32KEY;

struct OwnedRegistryKey {
  HKEY value = nullptr;
  OwnedRegistryKey() = default;
  OwnedRegistryKey(const OwnedRegistryKey&) = delete;
  OwnedRegistryKey& operator=(const OwnedRegistryKey&) = delete;
  ~OwnedRegistryKey() { if (value) { RegCloseKey(value); } }
};

// The caller uses HKLM's native ODBC view. The hive/key arguments also permit
// isolated HKCU fixtures to test the real registry reader without elevation.
inline bool registered_driver_path_matches(HKEY hive, const std::wstring& key_name,
                                           const std::filesystem::path& expected) {
  if (!canonical_file(expected)) { return false; }
  OwnedRegistryKey key;
  if (RegOpenKeyExW(hive, key_name.c_str(), 0,
      KEY_QUERY_VALUE | native_registry_view, &key.value) != ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0, size = 0;
  if (RegQueryValueExW(key.value, L"Driver", nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
      type != REG_SZ || size < 2 * sizeof(wchar_t) || size > 32768 * sizeof(wchar_t) ||
      size % sizeof(wchar_t) != 0) {
    return false;
  }
  const DWORD expected_size = size;
  std::vector<wchar_t> text(size / sizeof(wchar_t));
  if (RegQueryValueExW(key.value, L"Driver", nullptr, &type,
      reinterpret_cast<BYTE*>(text.data()), &size) != ERROR_SUCCESS ||
      type != REG_SZ || size != expected_size || text.back() != L'\0' ||
      std::wstring_view(text.data(), text.size() - 1).find(L'\0') != std::wstring_view::npos) {
    return false;
  }
  const std::filesystem::path registered_path(text.data());
  if (!registered_path.is_absolute()) { return false; }
  std::error_code error;
  return std::filesystem::canonical(registered_path, error) == expected && !error;
}

// Windows' equivalent of a current-owner-only private fixture permits the
// account, SYSTEM and Administrators. Other allow ACEs fail closed.
inline bool private_owner_and_dacl(HANDLE file) {
  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const auto status = GetSecurityInfo(file, SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      &owner, nullptr, &dacl, nullptr, &descriptor);
  struct LocalOwner {
    PSECURITY_DESCRIPTOR value;
    ~LocalOwner() { if (value) { LocalFree(value); } }
  } security{descriptor};
  if (status != ERROR_SUCCESS || !owner || !dacl || !IsValidSid(owner)) {
    return false;
  }
  OwnedHandle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) {
    return false;
  }
  DWORD size = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
  if (size == 0) { return false; }
  std::vector<unsigned char> bytes(size);
  if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size)) {
    return false;
  }
  const auto user = reinterpret_cast<const TOKEN_USER*>(bytes.data())->User.Sid;
  if (!EqualSid(owner, user)) { return false; }
  alignas(DWORD) std::array<unsigned char, SECURITY_MAX_SID_SIZE> system{};
  alignas(DWORD) std::array<unsigned char, SECURITY_MAX_SID_SIZE> administrators{};
  DWORD system_size = static_cast<DWORD>(system.size());
  DWORD administrators_size = static_cast<DWORD>(administrators.size());
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &system_size) ||
      !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
          administrators.data(), &administrators_size)) {
    return false;
  }
  ACL_SIZE_INFORMATION information{};
  if (!GetAclInformation(dacl, &information, sizeof(information), AclSizeInformation)) {
    return false;
  }
  DWORD owner_access = 0;
  for (DWORD index = 0; index < information.AceCount; ++index) {
    void* raw = nullptr;
    if (!GetAce(dacl, index, &raw)) { return false; }
    const auto header = static_cast<const ACE_HEADER*>(raw);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) { return false; }
    const auto ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
    const auto sid = const_cast<DWORD*>(&ace->SidStart);
    if (!IsValidSid(sid) || (!EqualSid(sid, user) &&
        !EqualSid(sid, system.data()) && !EqualSid(sid, administrators.data()))) {
      return false;
    }
    if (EqualSid(sid, user) && (header->AceFlags & INHERIT_ONLY_ACE) == 0) {
      auto access = ace->Mask;
      GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE,
          FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
      MapGenericMask(&access, &mapping);
      owner_access |= access;
    }
  }
  constexpr DWORD required = FILE_READ_DATA | FILE_WRITE_DATA;
  return (owner_access & required) == required;
}

inline bool same_metadata(const BY_HANDLE_FILE_INFORMATION& a,
                          const BY_HANDLE_FILE_INFORMATION& b) {
  return a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
      a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow &&
      a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
      a.dwFileAttributes == b.dwFileAttributes && a.nNumberOfLinks == 1 &&
      b.nNumberOfLinks == 1 &&
      CompareFileTime(&a.ftCreationTime, &b.ftCreationTime) == 0 &&
      CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0;
}
#endif

inline bool connection_driver_matches(const std::string& selection,
                                      const std::filesystem::path& expected) {
#ifdef _WIN32
  // Windows DRIVER names a registered description, not a library path. Refuse
  // key traversal/normalization before checking the exact native HKLM image.
  if (selection.empty() || selection.size() > 128 || selection.front() == ' ' ||
      selection.back() == ' ') {
    return false;
  }
  for (const auto ch : selection) {
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        (ch >= '0' && ch <= '9') || ch == ' ' || ch == '_' || ch == '-' || ch == '.')) {
      return false;
    }
  }
  return registered_driver_path_matches(HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\ODBC\\ODBCINST.INI\\" + std::wstring(selection.begin(), selection.end()), expected);
#else
  return std::filesystem::path(selection) == expected;
#endif
}

inline std::optional<std::string> read_private_connection_file(
    const std::filesystem::path& path) {
  if (!canonical_file(path)) { return std::nullopt; }
  std::string bytes;
#ifdef _WIN32
  constexpr DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN;
  // Deny concurrent write/delete opens while inspecting and reading this file.
  OwnedHandle file{CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr)};
  BY_HANDLE_FILE_INFORMATION before{}, after{}, named{};
  FILE_BASIC_INFO basic_before{}, basic_after{};
  if (file.value == INVALID_HANDLE_VALUE || GetFileType(file.value) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(file.value, &before) ||
      !GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic_before,
          sizeof(basic_before)) || before.nNumberOfLinks != 1 ||
      (before.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      before.nFileSizeHigh != 0 || before.nFileSizeLow == 0 || before.nFileSizeLow > 4096 ||
      !private_owner_and_dacl(file.value)) {
    return std::nullopt;
  }
  bytes.resize(before.nFileSizeLow);
  DWORD offset = 0;
  while (offset < before.nFileSizeLow) {
    DWORD count = 0;
    if (!ReadFile(file.value, bytes.data() + offset, before.nFileSizeLow - offset,
        &count, nullptr) || count == 0) {
      return std::nullopt;
    }
    offset += count;
  }
  char extra = 0;
  DWORD extra_count = 0;
  if (!ReadFile(file.value, &extra, 1, &extra_count, nullptr) || extra_count != 0 ||
      !GetFileInformationByHandle(file.value, &after) || !same_metadata(before, after) ||
      !GetFileInformationByHandleEx(file.value, FileBasicInfo, &basic_after,
          sizeof(basic_after)) ||
      basic_before.ChangeTime.QuadPart != basic_after.ChangeTime.QuadPart ||
      !private_owner_and_dacl(file.value)) {
    return std::nullopt;
  }
  OwnedHandle named_file{CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL,
      FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr)};
  if (named_file.value == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandle(named_file.value, &named) || !same_metadata(before, named)) {
    return std::nullopt;
  }
#elif defined(__APPLE__) || defined(__linux__)
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) { return std::nullopt; }
  struct FileOwner {
    int value;
    ~FileOwner() { if (value >= 0) { ::close(value); } }
  } file{fd};
  struct stat before{}, after{}, named{};
  if (::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_uid != ::geteuid() || (before.st_mode & 07777) != 0600 ||
      before.st_nlink != 1 || before.st_size <= 0 || before.st_size > 4096) {
    return std::nullopt;
  }
  bytes.resize(static_cast<std::size_t>(before.st_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (count <= 0) { return std::nullopt; }
    offset += static_cast<std::size_t>(count);
  }
  char extra = 0;
  if (::read(fd, &extra, 1) != 0 || ::fstat(fd, &after) != 0 ||
      before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
      before.st_size != after.st_size || before.st_uid != after.st_uid ||
      before.st_mode != after.st_mode || after.st_nlink != 1) {
    return std::nullopt;
  }
#ifdef __APPLE__
  if (before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
      before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
      before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
      before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec) {
    return std::nullopt;
  }
#else
  if (before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
      before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
    return std::nullopt;
  }
#endif
  if (::lstat(path.c_str(), &named) != 0 || !S_ISREG(named.st_mode) ||
      named.st_dev != before.st_dev || named.st_ino != before.st_ino) {
    return std::nullopt;
  }
  if (::close(fd) != 0) { file.value = -1; return std::nullopt; }
  file.value = -1;
#else
  return std::nullopt;
#endif
  if (bytes.find('\0') != std::string::npos || bytes.find('\n') != std::string::npos ||
      bytes.find('\r') != std::string::npos) {
    return std::nullopt;
  }
  return bytes;
}

inline std::optional<std::vector<std::filesystem::path>> loaded_image_paths() {
  std::vector<std::filesystem::path> images;
#ifdef _WIN32
  OwnedHandle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
      GetCurrentProcessId())};
  if (snapshot.value == INVALID_HANDLE_VALUE) { return std::nullopt; }
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Module32FirstW(snapshot.value, &entry)) { return std::nullopt; }
  do { images.emplace_back(entry.szExePath); }
  while (Module32NextW(snapshot.value, &entry));
  if (GetLastError() != ERROR_NO_MORE_FILES) { return std::nullopt; }
#elif defined(__APPLE__)
  for (std::uint32_t index = 0; index < _dyld_image_count(); ++index) {
    if (const char* image = _dyld_get_image_name(index)) { images.emplace_back(image); }
  }
#elif defined(__linux__)
  struct Context { std::vector<std::filesystem::path>* images; bool failed; } context{&images, false};
  const auto result = dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* raw) {
    auto& target = *static_cast<Context*>(raw);
    try {
      if (info->dlpi_name && info->dlpi_name[0] != '\0') {
        target.images->emplace_back(info->dlpi_name);
      } else {
        target.images->push_back(std::filesystem::read_symlink("/proc/self/exe"));
      }
      return 0;
    } catch (...) { target.failed = true; return 1; }
  }, &context);
  if (result != 0 || context.failed) { return std::nullopt; }
#else
  return std::nullopt;
#endif
  return images;
}

}  // namespace odbcpp::test::live_package_inputs
