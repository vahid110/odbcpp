#include "tests/test_dm_identity.h"
#include "dm_qualified_identity.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace odbcpp::test::dm {

const QualifiedUnixManager& configured_unix_manager() {
  static const QualifiedUnixManager identity{
      ODBCPP_DM_QUALIFIED_VERSION, ODBCPP_DM_QUALIFIED_PATH,
      ODBCPP_DM_QUALIFIED_SHA256, ODBCPP_DM_QUALIFIED_PLATFORM,
      ODBCPP_DM_QUALIFIED_POINTER_BYTES, ODBCPP_DM_QUALIFIED_SQLWCHAR_BYTES,
      ODBCPP_DM_QUALIFIED_DRIVER_SQLWCHAR_BYTES, ODBCPP_DM_QUALIFIED_ODBC_VERSION};
  return identity;
}

bool same_manager_file_bytes(std::string_view file, std::string_view reference_file) {
  const std::filesystem::path path(file), snapshot(reference_file);
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error) || error ||
      !std::filesystem::is_regular_file(snapshot, error) || error) {
    return {};
  }
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > 64 * 1024 * 1024 ||
      std::filesystem::file_size(snapshot, error) != size || error) {
    return {};
  }
  std::ifstream input(path, std::ios::binary), reference(snapshot, std::ios::binary);
  if (!input || !reference) {
    return {};
  }
  std::array<char, 16384> buffer{}, expected{};
  std::size_t bytes = 0;
  while (input.read(buffer.data(), buffer.size()) || input.gcount() != 0) {
    const auto count = input.gcount();
    reference.read(expected.data(), count);
    bytes += static_cast<std::size_t>(count);
    if (bytes > size || reference.gcount() != count ||
        !std::equal(buffer.begin(), buffer.begin() + count, expected.begin())) {
      return {};
    }
  }
  if (!input.eof() || input.bad() || bytes != size || reference.peek() != std::ifstream::traits_type::eof() ||
      reference.bad()) {
    return {};
  }
  return true;
}

std::string observed_manager_library_path() {
#ifdef _WIN32
  // Read bound imports, not GetModuleHandle(basename): another loaded module
  // with that basename must not be mistaken for the executable's provider.
  // As in it_package_load, the loaded executable is trusted test input.
  const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
  if (!base) {
    return {};
  }
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
    return {};
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return {};
  }
  const auto size = nt->OptionalHeader.SizeOfImage;
  const auto entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!entry.VirtualAddress || entry.VirtualAddress >= size ||
      entry.Size > size - entry.VirtualAddress) {
    return {};
  }
  const auto* imports = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + entry.VirtualAddress);
  HMODULE provider{};
  constexpr std::array names{"SQLGetInfo", "SQLAllocHandle", "SQLGetDiagRec"};
  std::array<bool, names.size()> found{};
  for (std::size_t i = 0; i < entry.Size / sizeof(*imports); ++i) {
    const auto& item = imports[i];
    if (!item.Name) {
      break;
    }
    if (item.Name >= size || !std::memchr(base + item.Name, 0, size - item.Name)) {
      return {};
    }
    if (_stricmp(reinterpret_cast<const char*>(base + item.Name), "odbc32.dll") != 0) {
      continue;
    }
    if (!item.FirstThunk || item.FirstThunk >= size ||
        !item.OriginalFirstThunk || item.OriginalFirstThunk >= size) {
      return {};
    }
    const auto* thunks = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + item.FirstThunk);
    const auto* originals = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + item.OriginalFirstThunk);
    const auto count = std::min((size - item.FirstThunk) / sizeof(*thunks),
                               (size - item.OriginalFirstThunk) / sizeof(*originals));
    bool terminated = false;
    for (std::size_t index = 0; index < count; ++index) {
      if (!thunks[index].u1.Function) {
        terminated = originals[index].u1.AddressOfData == 0;
        break;
      }
      const auto address = originals[index].u1.AddressOfData;
      if (IMAGE_SNAP_BY_ORDINAL(address)) {
        continue;
      }
      constexpr auto prefix = offsetof(IMAGE_IMPORT_BY_NAME, Name);
      if (!address || address >= size || prefix >= size - address) {
        return {};
      }
      const auto* name = reinterpret_cast<const char*>(base + address + prefix);
      if (!std::memchr(name, 0, size - address - prefix)) {
        return {};
      }
      const auto expected = std::find_if(names.begin(), names.end(),
          [name](const char* value) { return std::strcmp(name, value) == 0; });
      if (expected == names.end()) {
        continue;
      }
      const auto name_index = static_cast<std::size_t>(expected - names.begin());
      if (found[name_index]) {
        return {};
      }
      HMODULE actual{};
      if (!GetModuleHandleExW(
              GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
              reinterpret_cast<LPCWSTR>(thunks[index].u1.Function), &actual) ||
          (provider && provider != actual)) {
        return {};
      }
      provider = actual;
      found[name_index] = true;
    }
    if (!terminated) {
      return {};
    }
  }
  if (!std::all_of(found.begin(), found.end(), [](bool value) { return value; })) {
    return {};
  }
  std::array<wchar_t, 32768> filename{};
  const auto length = provider
      ? GetModuleFileNameW(provider, filename.data(), static_cast<DWORD>(filename.size())) : 0;
  if (!length || length >= filename.size()) {
    return {};
  }
  std::error_code error;
  const auto path = std::filesystem::canonical(filename.data(), error);
  if (error) {
    return {};
  }
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
#else
  // All APIs used to identify/read manager diagnostics must have one provider.
  Dl_info provider{};
  if (!dladdr(dlsym(RTLD_DEFAULT, "SQLGetInfo"), &provider) || !provider.dli_fname) {
    return {};
  }
  for (const char* name : {"SQLAllocHandle", "SQLGetDiagRec"}) {
    Dl_info other{};
    if (!dladdr(dlsym(RTLD_DEFAULT, name), &other) ||
        other.dli_fbase != provider.dli_fbase) {
      return {};
    }
  }
  std::error_code error;
  const auto path = std::filesystem::canonical(provider.dli_fname, error);
  if (error) {
    return {};
  }
  return path.string();
#endif
}

std::string manager_configuration_provenance() {
#ifdef _WIN32
  std::string result = "default_lookup=Windows_ODBC_registry";
#else
  std::string result = "default_lookup=manager_defined_ini_locations";
#endif
  for (const char* name : {"ODBCINI", "ODBCSYSINI", "ODBCINSTINI"}) {
    result += ';';
    result += name;
#ifdef _WIN32
    std::size_t length = 0;
    if (getenv_s(&length, nullptr, 0, name) != 0) {
      return {};
    }
    result += length != 0 ? "=set" : "=unset";
#else
    result += std::getenv(name) ? "=set" : "=unset";
#endif
  }
  return result;
}

ManagerFileIdentity loaded_manager_file_identity() {
#ifdef _WIN32
  return {};
#else
  const auto path = observed_manager_library_path();
  if (path.empty() || path != configured_unix_manager().library_path) {
    return {};
  }
  // CMake validated this build-local test-data snapshot against the reviewed
  // SHA256. Exact byte comparison needs no crypto/core link or child process.
  // Like the executable, the snapshot is a trusted build input, not a defence
  // against hostile writers replacing this build while its tests run.
  if (!same_manager_file_bytes(path, ODBCPP_DM_QUALIFIED_SNAPSHOT)) {
    return {};
  }
  return {path, std::string(configured_unix_manager().library_sha256)};
#endif
}

}  // namespace odbcpp::test::dm
