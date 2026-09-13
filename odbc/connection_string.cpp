#include "connection_string.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace rs::odbc {
namespace {

void overlay(std::map<std::string, std::string>& target,
             const std::map<std::string, std::string>& source) {
  for (const auto& [key, value] : source) target[key] = value;
}

std::string environment_value(const char* name) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

} // namespace

std::map<std::string, std::string> ConnectionString::parse(const std::string& conn_str) {
  if (conn_str.find('\0') != std::string::npos) {
    throw std::invalid_argument("Connection string contains an embedded NUL byte");
  }
  std::map<std::string, std::string> params;

  std::size_t position = 0;
  while (position < conn_str.size()) {
    const auto separator = conn_str.find(';', position);
    const auto equals = conn_str.find('=', position);
    if (equals == std::string::npos ||
        (separator != std::string::npos && separator < equals)) {
      if (separator == std::string::npos) break;
      position = separator + 1;
      continue;
    }

    const auto key = trim(conn_str.substr(position, equals - position));
    position = equals + 1;
    while (position < conn_str.size() &&
           std::isspace(static_cast<unsigned char>(conn_str[position]))) {
      ++position;
    }

    std::string value;
    if (position < conn_str.size() && conn_str[position] == '{') {
      ++position;
      bool closed = false;
      while (position < conn_str.size()) {
        const char current = conn_str[position++];
        if (current != '}') {
          value.push_back(current);
        } else if (position < conn_str.size() && conn_str[position] == '}') {
          value.push_back('}');
          ++position;
        } else {
          closed = true;
          break;
        }
      }
      if (!closed) {
        throw std::invalid_argument("Unterminated braced connection string value");
      }
      while (position < conn_str.size() &&
             std::isspace(static_cast<unsigned char>(conn_str[position]))) {
        ++position;
      }
      if (position < conn_str.size() && conn_str[position] != ';') {
        throw std::invalid_argument("Unexpected text after braced connection string value");
      }
    } else {
      const auto end = conn_str.find(';', position);
      value = trim(conn_str.substr(
          position, end == std::string::npos ? end : end - position));
      position = end == std::string::npos ? conn_str.size() : end;
    }

    if (!key.empty()) params.try_emplace(to_upper(key), std::move(value));
    if (position < conn_str.size()) ++position;
  }

  return params;
}

std::map<std::string, std::string> ConnectionString::load_dsn(const std::string& dsn_name) {
  auto dsn_paths = get_dsn_file_paths();
  
  for (const auto& path : dsn_paths) {
    auto params = DSNReader::read_dsn_file(path, dsn_name);
    if (!params.empty()) {
      return params;
    }
  }
  
  return {}; // DSN not found
}

ResolvedConnectionParameters ConnectionString::resolve(
    const std::string& dsn_or_connection_string,
    const std::string& default_driver_name) {
  ResolvedConnectionParameters resolved;

  if (dsn_or_connection_string.find('=') != std::string::npos) {
    resolved.connection_parameters = parse(dsn_or_connection_string);
    const auto dsn = resolved.connection_parameters.find("DSN");
    if (dsn != resolved.connection_parameters.end()) {
      resolved.dsn_name = dsn->second;
      resolved.dsn_parameters = load_dsn(resolved.dsn_name);
    }
  } else {
    resolved.dsn_name = dsn_or_connection_string;
    resolved.dsn_parameters = load_dsn(resolved.dsn_name);
  }

  overlay(resolved.effective_parameters, resolved.dsn_parameters);
  overlay(resolved.effective_parameters, resolved.connection_parameters);

  const auto driver = resolved.effective_parameters.find("DRIVER");
  resolved.driver_name = driver == resolved.effective_parameters.end()
      ? default_driver_name
      : driver->second;
  if (!resolved.driver_name.empty()) {
    resolved.driver_parameters =
        DSNReader::read_driver_config(resolved.driver_name);
  }
  // Keep the generic legacy registration useful for direct connections.
  if (resolved.driver_parameters.empty() && resolved.driver_name != "ODBCPP") {
    resolved.driver_parameters = DSNReader::read_driver_config("ODBCPP");
  }

  return resolved;
}

std::vector<std::string> ConnectionString::get_dsn_file_paths() {
  std::vector<std::string> paths;

  const auto odbcini = environment_value("ODBCINI");
  if (!odbcini.empty()) paths.push_back(odbcini);

  const auto odbcsysini = environment_value("ODBCSYSINI");
  if (!odbcsysini.empty()) paths.push_back(odbcsysini + "/odbc.ini");

#ifdef _WIN32
  // Windows: Registry-based DSNs (simplified file-based approach for now)
  paths.push_back("C:\\Windows\\odbc.ini");
  paths.push_back("odbcpp.dsn"); // Local DSN file
#else
  // Unix/Linux/macOS: unixODBC standard locations
  // Standard unixODBC locations
  paths.push_back("/etc/odbc.ini");           // System DSNs
  paths.push_back("/usr/local/etc/odbc.ini"); // Homebrew location
  
  // User DSNs
  const auto home = environment_value("HOME");
  if (!home.empty()) paths.push_back(home + "/.odbc.ini");
  
  // Local project files (for testing)
  paths.push_back("odbc.ini");        // Current directory
  paths.push_back("../odbc.ini");     // From build directory
  paths.push_back("odbcpp.dsn");      // Legacy file
  paths.push_back("../odbcpp.dsn");   // Legacy from build directory
#endif
  
  return paths;
}

std::vector<std::string> ConnectionString::get_driver_file_paths() {
  std::vector<std::string> paths;

  const auto odbcsysini = environment_value("ODBCSYSINI");
  const auto odbcinstini = environment_value("ODBCINSTINI");
  if (!odbcinstini.empty()) {
    const std::filesystem::path configured(odbcinstini);
    if (configured.is_absolute() || odbcsysini.empty()) {
      paths.push_back(configured.string());
    } else {
      paths.push_back(
          (std::filesystem::path(odbcsysini) / configured).string());
    }
  }
  if (!odbcsysini.empty()) paths.push_back(odbcsysini + "/odbcinst.ini");

#ifdef _WIN32
  // Windows: Registry-based drivers (simplified file-based approach for now)
  paths.push_back("C:\\Windows\\odbcinst.ini");
#else
  // Unix/Linux/macOS: unixODBC standard locations
  // Standard unixODBC locations
  paths.push_back("/etc/odbcinst.ini");           // System drivers
  paths.push_back("/usr/local/etc/odbcinst.ini"); // Homebrew location
  
  // Local project files (for testing)
  paths.push_back("odbcinst.ini");     // Current directory
  paths.push_back("../odbcinst.ini");  // From build directory
#endif
  
  return paths;
}

std::string ConnectionString::trim(const std::string& str) {
  size_t start = str.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  
  size_t end = str.find_last_not_of(" \t\r\n");
  return str.substr(start, end - start + 1);
}

std::string ConnectionString::to_upper(const std::string& str) {
  std::string result = str;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char character) { return std::toupper(character); });
  return result;
}

// DSNReader implementation
std::map<std::string, std::string> DSNReader::read_dsn_file(const std::string& file_path, const std::string& dsn_name) {
  std::ifstream file(file_path);
  if (!file.is_open()) {
    return {};
  }
  
  return parse_ini_section(file, dsn_name);
}

bool DSNReader::dsn_exists(const std::string& dsn_name) {
  auto paths = ConnectionString::get_dsn_file_paths();
  
  for (const auto& path : paths) {
    auto params = read_dsn_file(path, dsn_name);
    if (!params.empty()) {
      return true;
    }
  }
  
  return false;
}

std::map<std::string, std::string> DSNReader::read_driver_config(const std::string& driver_name) {
  auto paths = ConnectionString::get_driver_file_paths();
  
  for (const auto& path : paths) {
    std::ifstream file(path);
    if (!file.is_open()) {
      continue;
    }
    
    auto params = parse_ini_section(file, driver_name);
    if (!params.empty()) {
      return params;
    }
  }
  
  return {}; // Driver not found
}

bool DSNReader::driver_exists(const std::string& driver_name) {
  auto config = read_driver_config(driver_name);
  return !config.empty();
}

std::map<std::string, std::string> DSNReader::parse_ini_section(std::ifstream& file, const std::string& section_name) {
  std::map<std::string, std::string> params;
  std::string line;
  bool in_section = false;
  
  std::string target_section = "[" + section_name + "]";
  
  while (std::getline(file, line)) {
    line = ConnectionString::trim(line);
    
    if (line.empty() || line[0] == '#' || line[0] == ';') {
      continue; // Skip comments and empty lines
    }
    
    if (line[0] == '[') {
      // Section header
      if (ConnectionString::to_upper(line) == ConnectionString::to_upper(target_section)) {
        in_section = true;
      } else {
        in_section = false;
      }
      continue;
    }
    
    if (in_section) {
      size_t eq_pos = line.find('=');
      if (eq_pos != std::string::npos) {
        std::string key = ConnectionString::trim(line.substr(0, eq_pos));
        std::string value = ConnectionString::trim(line.substr(eq_pos + 1));
        params[ConnectionString::to_upper(key)] = value;
      }
    }
  }
  
  return params;
}

} // namespace rs::odbc
