#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>

namespace {
bool verify_module(const std::wstring& directory, const wchar_t* file) {
  const auto module = GetModuleHandleW(file);
  wchar_t path[32768]{};
  const auto length = module ? GetModuleFileNameW(module, path, 32768) : 0;
  const auto expected_path = directory + L"\\" + file;
  if (!length || length >= 32768) {
    std::fwprintf(stderr, L"Runtime dependency %ls did not load from the package directory.\n", file);
    return false;
  }
  const auto loaded_file = CreateFileW(path, FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  const auto expected_file = CreateFileW(expected_path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  BY_HANDLE_FILE_INFORMATION loaded_info{}, expected_info{};
  const bool same_file = loaded_file != INVALID_HANDLE_VALUE &&
      expected_file != INVALID_HANDLE_VALUE &&
      GetFileInformationByHandle(loaded_file, &loaded_info) &&
      GetFileInformationByHandle(expected_file, &expected_info) &&
      loaded_info.dwVolumeSerialNumber == expected_info.dwVolumeSerialNumber &&
      loaded_info.nFileIndexHigh == expected_info.nFileIndexHigh &&
      loaded_info.nFileIndexLow == expected_info.nFileIndexLow;
  if (loaded_file != INVALID_HANDLE_VALUE) CloseHandle(loaded_file);
  if (expected_file != INVALID_HANDLE_VALUE) CloseHandle(expected_file);
  if (!same_file) {
    std::fwprintf(stderr, L"Runtime dependency %ls is not the packaged file.\n", file);
    return false;
  }
  std::wprintf(L"MODULE|%ls|%ls\n", file, path);
  return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 4) {
    std::fwprintf(stderr, L"Usage: it_package_load <install-dir> <ssl-dll> <crypto-dll>\n");
    return 2;
  }
  const std::wstring directory = argv[1];
  const auto driver_path = directory + L"\\odbcpp.dll";
  if (!LoadLibraryExW(driver_path.c_str(), nullptr,
                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    std::fwprintf(stderr, L"Packaged driver load failed: %lu\n", GetLastError());
    return 1;
  }
  if (!verify_module(directory, argv[2]) || !verify_module(directory, argv[3])) return 1;

  const auto crypto = GetModuleHandleW(argv[3]);
  using OpenSSLVersion = const char*(__cdecl*)(int);
  const auto version = reinterpret_cast<OpenSSLVersion>(GetProcAddress(crypto, "OpenSSL_version"));
  if (!version || !version(0)) {
    std::fwprintf(stderr, L"Packaged Crypto DLL does not expose OpenSSL_version.\n");
    return 1;
  }
  std::wprintf(L"VERSION|%hs\n", version(0));

  const auto setup_path = directory + L"\\odbcpp_setup.dll";
  if (!LoadLibraryExW(setup_path.c_str(), nullptr,
                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    std::fwprintf(stderr, L"Packaged setup DLL load failed: %lu\n", GetLastError());
    return 1;
  }
  for (const auto file : {L"msvcp140.dll", L"vcruntime140.dll"}) {
    if (!verify_module(directory, file)) return 1;
  }
  std::wprintf(L"Packaged driver/setup and app-local runtime dependencies loaded successfully.\n");
  return 0;
}
