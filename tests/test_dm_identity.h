#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace odbcpp::test::dm {

// Test qualification input, not a discovered version label. A qualified hash
// identifies the reviewed manager build whose SQL_DM_VER aliases other builds.
struct QualifiedUnixManager {
  std::string_view version;
  std::string_view library_path;
  std::string_view library_sha256;
  std::string_view platform;
  std::size_t pointer_bytes{};
  std::size_t sqlwchar_bytes{};
  std::size_t driver_sqlwchar_bytes{};
  unsigned odbc_version{};
};

struct ObservedManager {
  std::string_view sql_dm_version;
  std::string_view library_path;
  std::string_view library_sha256;
  std::string_view platform;
  std::size_t pointer_bytes{};
  std::size_t sqlwchar_bytes{};
  std::size_t driver_sqlwchar_bytes{};
  unsigned odbc_version{};
};

inline bool qualified_unixodbc2314(const QualifiedUnixManager& qualified,
                                  const ObservedManager& observed) {
  return qualified.version == "2.3.14" &&
      observed.sql_dm_version == "03.52.0002.0003" &&
      !qualified.library_path.empty() &&
      qualified.library_path == observed.library_path &&
      qualified.library_sha256.size() == 64 &&
      qualified.library_sha256 == observed.library_sha256 &&
      (qualified.platform == "Linux" || qualified.platform == "Darwin") &&
      qualified.platform == observed.platform &&
      qualified.pointer_bytes == 8 && observed.pointer_bytes == 8 &&
      qualified.sqlwchar_bytes == 2 && observed.sqlwchar_bytes == 2 &&
      qualified.driver_sqlwchar_bytes == 2 && observed.driver_sqlwchar_bytes == 2 &&
      qualified.odbc_version == 3 && observed.odbc_version == 3;
}

struct ManagerFileIdentity {
  std::string path;
  std::string sha256;
};

// Observation only: available even when no known-deviation receipt applies.
// An observed path must never grant a manager-specific expectation by itself.
std::string observed_manager_library_path();
// Fixed provenance labels and set/unset facts only; never environment values.
std::string manager_configuration_provenance();

// Resolves the loaded manager's API provider, rather than trusting a linker
// search path, executable version label or SQL_DM_VER alone.
ManagerFileIdentity loaded_manager_file_identity();
const QualifiedUnixManager& configured_unix_manager();
bool same_manager_file_bytes(std::string_view file, std::string_view snapshot);

}  // namespace odbcpp::test::dm
