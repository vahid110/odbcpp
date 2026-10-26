#include "tests/test_dm_identity.h"
#include "dm_qualified_identity.h"

#ifndef _WIN32
#include <dlfcn.h>
#endif

#include <algorithm>
#include <array>
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

ManagerFileIdentity loaded_manager_file_identity() {
#ifdef _WIN32
  return {};
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
  if (error || path.string() != configured_unix_manager().library_path) {
    return {};
  }
  // CMake validated this build-local test-data snapshot against the reviewed
  // SHA256. Exact byte comparison needs no crypto/core link or child process.
  // Like the executable, the snapshot is a trusted build input, not a defence
  // against hostile writers replacing this build while its tests run.
  if (!same_manager_file_bytes(path.string(), ODBCPP_DM_QUALIFIED_SNAPSHOT)) {
    return {};
  }
  return {path.string(), std::string(configured_unix_manager().library_sha256)};
#endif
}

}  // namespace odbcpp::test::dm
