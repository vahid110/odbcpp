#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <cstring>

namespace {
// GetModuleHandle(basename) is ambiguous when the host loaded another copy.
// Resolve a bound import address to the module actually serving the driver.
HMODULE imported_module(HMODULE driver, const wchar_t* file) {
  char name[512]{};
  if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, name, sizeof(name), nullptr, nullptr)) return nullptr;
  const auto* base = reinterpret_cast<const unsigned char*>(driver);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
  const auto size = nt->OptionalHeader.SizeOfImage;
  const auto entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!entry.VirtualAddress || entry.VirtualAddress >= size || entry.Size > size - entry.VirtualAddress) return nullptr;
  const auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + entry.VirtualAddress);
  for (size_t i = 0; i < entry.Size / sizeof(*imports); ++i) {
    const auto& item = imports[i];
    if (!item.Name) break;
    if (item.Name >= size || !std::memchr(base + item.Name, 0, size - item.Name)) return nullptr;
    if (_stricmp(reinterpret_cast<const char*>(base + item.Name), name) != 0) continue;
    if (!item.FirstThunk || item.FirstThunk > size || sizeof(IMAGE_THUNK_DATA) > size - item.FirstThunk) return nullptr;
    const auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + item.FirstThunk);
    HMODULE module{};
    if (!thunk->u1.Function || !GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(thunk->u1.Function), &module)) return nullptr;
    return module;
  }
  return nullptr;
}

bool verify_module(const std::wstring& directory, const wchar_t* file, HMODULE module) {
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
    std::fwprintf(stderr, L"Runtime dependency %ls loaded %ls; expected %ls.\n",
                  file, path, expected_path.c_str());
    return false;
  }
  std::wprintf(L"MODULE|%ls|%ls\n", file, path);
  return true;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 4 && argc != 5) {
    std::fwprintf(stderr, L"Usage: it_package_load <install-dir> <ssl-dll> <crypto-dll> [preload-dir]\n");
    return 2;
  }
  const std::wstring directory = argv[1];
  if (argc == 5) {
    const std::wstring preload = argv[4];
    // Crypto first, so the SSL preload binds against the same host directory.
    for (const auto file : {argv[3], argv[2]}) {
      const auto path = preload + L"\\" + file;
      const auto module = LoadLibraryExW(path.c_str(), nullptr,
          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
      if (!module || !verify_module(preload, file, module)) {
        std::fwprintf(stderr, L"Host preload failed.\n");
        return 2;
      }
    }
    std::wprintf(L"PRELOAD_READY\n");
  }
  const auto driver_path = directory + L"\\odbcpp.dll";
  const auto driver = LoadLibraryExW(driver_path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!driver) {
    std::fwprintf(stderr, L"Packaged driver load failed: %lu\n", GetLastError());
    return 1;
  }
  const auto ssl = imported_module(driver, argv[2]);
  const auto crypto = imported_module(driver, argv[3]);
  if (!ssl || !crypto) {
    std::fwprintf(stderr, L"Driver crypto imports could not be resolved.\n");
    return 1;
  }
  if (!verify_module(directory, argv[2], ssl) || !verify_module(directory, argv[3], crypto)) {
    if (argc == 5) {
      std::wprintf(L"FOREIGN_PRELOAD_BOUND_TO_DRIVER\n");
      return 3;
    }
    return 1;
  }

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
    if (!verify_module(directory, file, GetModuleHandleW(file))) return 1;
  }
  std::wprintf(L"Packaged driver/setup and app-local runtime dependencies loaded successfully.\n");
  return 0;
}
