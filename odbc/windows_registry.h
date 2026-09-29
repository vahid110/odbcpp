#pragma once
#ifdef _WIN32
#include <map>
#include <optional>
#include <string>

namespace rs::odbc::windows_registry {
// nullopt means absent. Existing empty, malformed or inaccessible entries must
// not fall through to a different hive or legacy configuration source.
std::optional<std::map<std::string, std::string>> load_dsn(const std::string& name);
std::optional<std::map<std::string, std::string>> load_driver(const std::string& name);
}
#endif
