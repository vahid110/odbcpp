#pragma once
#include <string>
#include <map>
#include <fstream>
#include <vector>

namespace rs::odbc {

struct ResolvedConnectionParameters {
  std::map<std::string, std::string> driver_parameters;
  std::map<std::string, std::string> dsn_parameters;
  std::map<std::string, std::string> connection_parameters;
  std::map<std::string, std::string> effective_parameters;
  std::string dsn_name;
  std::string driver_name;
};

// Connection string parser for ODBC standard formats
class ConnectionString {
public:
  // Parse connection string: "DSN=mydsn;UID=user;PWD=pass" or "DRIVER={ODBCPP};SERVER=host;..."
  static std::map<std::string, std::string> parse(const std::string& conn_str);
  
  // Load DSN from system DSN files
  static std::map<std::string, std::string> load_dsn(const std::string& dsn_name);

  // Resolve a DSN name or connection string without losing the individual
  // precedence layers: driver < DSN < connection string.
  static ResolvedConnectionParameters resolve(
      const std::string& dsn_or_connection_string,
      const std::string& default_driver_name);
  
  // Get DSN file paths for current platform
  static std::vector<std::string> get_dsn_file_paths();
  
  // Get driver configuration file paths
  static std::vector<std::string> get_driver_file_paths();

  // Utility functions (public for DSNReader)
  static std::string trim(const std::string& str);
  static std::string to_upper(const std::string& str);
};

// DSN file reader for cross-platform DSN support
class DSNReader {
public:
  // Read DSN configuration from file
  static std::map<std::string, std::string> read_dsn_file(const std::string& file_path, const std::string& dsn_name);
  
  // Check if DSN exists in system
  static bool dsn_exists(const std::string& dsn_name);
  
  // Read driver configuration from odbcinst.ini
  static std::map<std::string, std::string> read_driver_config(const std::string& driver_name);
  
  // Check if driver exists in system
  static bool driver_exists(const std::string& driver_name);

private:
  static std::map<std::string, std::string> parse_ini_section(std::ifstream& file, const std::string& section_name);
};

} // namespace rs::odbc
