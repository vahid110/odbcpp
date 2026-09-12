#pragma once

#include "odbc/odbc_types.h"

#include <cstdlib>
#include <string>

namespace odbcpp::test {

inline std::string configured_connection_string() {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, "ODBCPP_TEST_DSN") == 0 && value) {
    std::string configured(value);
    std::free(value);
    if (!configured.empty()) return configured;
  }
#else
  if (const auto* value = std::getenv("ODBCPP_TEST_DSN"); value && *value) {
    return value;
  }
#endif
  return "DSN=RedshiftProd";
}

inline SQLCHAR* configured_connection_string_data() {
  static std::string value = configured_connection_string();
  return reinterpret_cast<SQLCHAR*>(value.data());
}

}  // namespace odbcpp::test
