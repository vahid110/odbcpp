#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>

namespace {
bool verify_module(const std::wstring& directory, const wchar_t* file) {
  const auto module = GetModuleHandleW(file);
  wchar_t path[32768]{};
  if (!module || !GetModuleFileNameW(module, path, 32768) ||
      _wcsicmp(path, (directory + L"\\" + file).c_str()) != 0) {
    std::fwprintf(stderr, L"Runtime dependency %ls did not load from the package directory.\n", file);
    return false;
  }
  std::wprintf(L"MODULE|%ls|%ls\n", file, path);
  return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 4) {
    std::fputs("Usage: it_package_load <install-dir> <ssl-dll> <crypto-dll>\n", stderr);
    return 2;
  }
  const std::wstring directory = argv[1];
  const auto driver_path = directory + L"\\odbcpp.dll";
  if (!LoadLibraryExW(driver_path.c_str(), nullptr,
                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    std::fprintf(stderr, "Packaged driver load failed: %lu\n", GetLastError());
    return 1;
  }
  if (!verify_module(directory, argv[2]) || !verify_module(directory, argv[3])) return 1;

  const auto crypto = GetModuleHandleW(argv[3]);
  using OpenSSLVersion = const char*(__cdecl*)(int);
  const auto version = reinterpret_cast<OpenSSLVersion>(GetProcAddress(crypto, "OpenSSL_version"));
  if (!version || !version(0)) {
    std::fputs("Packaged Crypto DLL does not expose OpenSSL_version.\n", stderr);
    return 1;
  }
  std::wprintf(L"VERSION|%hs\n", version(0));

  const auto setup_path = directory + L"\\odbcpp_setup.dll";
  if (!LoadLibraryExW(setup_path.c_str(), nullptr,
                      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    std::fprintf(stderr, "Packaged setup DLL load failed: %lu\n", GetLastError());
    return 1;
  }
  for (const auto file : {L"msvcp140.dll", L"vcruntime140.dll"}) {
    if (!verify_module(directory, file)) return 1;
  }
  std::wprintf(L"Packaged driver/setup and app-local runtime dependencies loaded successfully.\n");
  return 0;
}
