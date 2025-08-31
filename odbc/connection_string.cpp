#include "connection_string.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace rs::odbc {

std::map<std::string, std::string> ConnectionString::parse(const std::string& conn_str) {
  std::map<std::string, std::string> params;
  
  std::istringstream stream(conn_str);
  std::string pair;
  
  while (std::getline(stream, pair, ';')) {
    if (pair.empty()) continue;
    
    size_t eq_pos = pair.find('=');
    if (eq_pos == std::string::npos) continue;
    
    std::string key = trim(pair.substr(0, eq_pos));
    std::string value = trim(pair.substr(eq_pos + 1));
    
    // Remove braces from values like {ODBCPP Driver}
    if (value.front() == '{' && value.back() == '}') {
      value = value.substr(1, value.length() - 2);
    }
    
    params[to_upper(key)] = value;
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

std::vector<std::string> ConnectionString::get_dsn_file_paths() {
  std::vector<std::string> paths;
  
#ifdef _WIN32
  // Windows: Registry-based DSNs (simplified file-based approach for now)
  paths.push_back("C:\\Windows\\odbc.ini");
  paths.push_back("odbcpp.dsn"); // Local DSN file
#else
  // Unix/Linux/macOS: unixODBC standard locations
  paths.push_back("/etc/odbc.ini");           // System DSNs
  paths.push_back("/usr/local/etc/odbc.ini"); // Homebrew location
  
  // User DSNs
  const char* home = getenv("HOME");
  if (home) {
    paths.push_back(std::string(home) + "/.odbc.ini");
  }
  
  // Local DSN file
  paths.push_back("odbcpp.dsn");
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
  std::transform(result.begin(), result.end(), result.begin(), ::toupper);
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