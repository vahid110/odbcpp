#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace odbcpp::test {
// Writes a temporary odbcinst.ini with one driver section and points
// ODBCINSTINI at it while alive. Tests use it to configure administrator-only
// driver keys (for example LogFile) the way a deployment does, then select the
// section with the returned DRIVER= connection-string prefix.
class ScopedDriverConfig {
 public:
  ScopedDriverConfig(const std::filesystem::path& directory, std::string driver,
                     const std::map<std::string, std::string>& keys)
      : driver_(std::move(driver)), path_(directory / "odbcinst.ini") {
    if (const char* previous = std::getenv("ODBCINSTINI")) previous_ = previous;
    std::ofstream file(path_);
    file << "[" << driver_ << "]\n";
    for (const auto& [key, value] : keys) file << key << "=" << value << "\n";
    if (!file) throw std::runtime_error("Could not write the test driver configuration");
    file.close();
    set(path_.string());
  }
  ScopedDriverConfig(const ScopedDriverConfig&) = delete;
  ScopedDriverConfig& operator=(const ScopedDriverConfig&) = delete;
  ~ScopedDriverConfig() {
    if (previous_) {
      set(*previous_);
    } else {
      unset();
    }
  }
  std::string connection_prefix() const { return "DRIVER=" + driver_ + ";"; }

 private:
  static void set(const std::string& value) {
#ifdef _WIN32
    _putenv_s("ODBCINSTINI", value.c_str());
#else
    setenv("ODBCINSTINI", value.c_str(), 1);
#endif
  }
  static void unset() {
#ifdef _WIN32
    _putenv_s("ODBCINSTINI", "");
#else
    unsetenv("ODBCINSTINI");
#endif
  }
  std::string driver_;
  std::filesystem::path path_;
  std::optional<std::string> previous_;
};
}  // namespace odbcpp::test
