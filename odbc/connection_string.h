#pragma once
#include <string>
#include <map>
#include <fstream>

namespace rs::odbc {

// Connection string parser for ODBC standard formats
class ConnectionString {
public:
  // Parse connection string: "DSN=mydsn;UID=user;PWD=pass" or "DRIVER={ODBCPP};SERVER=host;..."
  static std::map<std::string, std::string> parse(const std::string& conn_str);
  
  // Load DSN from system DSN files
  static std::map<std::string, std::string> load_dsn(const std::string& dsn_name);
  
  // Get DSN file paths for current platform
  static std::vector<std::string> get_dsn_file_paths();

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

private:
  static std::map<std::string, std::string> parse_ini_section(std::ifstream& file, const std::string& section_name);
};

} // namespace rs::odbc