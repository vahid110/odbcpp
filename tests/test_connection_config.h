#pragma once

#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include "tests/test_dm_odbc.h"
#else
#include "odbc/odbc_types.h"
#endif

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

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

// SQLConnect ServerName for the configured fixture. The in-process driver
// accepts the configured connection string itself. A Driver Manager treats
// ServerName as a DSN name, so a manager build passes only NAME from a
// configured "DSN=NAME" or "DSN=NAME;"; any other form is passed unchanged
// and the manager's own connect diagnostic reports it.
inline std::string configured_server_name() {
  auto configured = configured_connection_string();
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
  std::string_view value(configured);
  if (!value.empty() && value.back() == ';') value.remove_suffix(1);
  if (value.size() > 4 && value.find(';') == std::string_view::npos &&
      std::toupper(static_cast<unsigned char>(value[0])) == 'D' &&
      std::toupper(static_cast<unsigned char>(value[1])) == 'S' &&
      std::toupper(static_cast<unsigned char>(value[2])) == 'N' && value[3] == '=') {
    return std::string(value.substr(4));
  }
#endif
  return configured;
}

inline SQLCHAR* configured_server_name_data() {
  static std::string value = configured_server_name();
  return reinterpret_cast<SQLCHAR*>(value.data());
}

// SQL_ATTR_LOGIN_TIMEOUT is a 32-bit SQLUINTEGER, but iODBC 3.52.16 writes
// 8 bytes for it before connect (asserted by
// ConnectionAttributeIntegrationTest.LoginTimeoutOutputWidth). Fixture budget
// helpers read it through zeroed wider storage so a manager cannot overwrite
// the caller's neighbouring stack slot; the declared 4 bytes are the value.
inline SQLRETURN get_login_timeout(SQLHDBC connection, SQLUINTEGER* value) {
  alignas(SQLULEN) unsigned char storage[sizeof(SQLULEN)]{};
  const SQLRETURN result = SQLGetConnectAttr(
      connection, SQL_ATTR_LOGIN_TIMEOUT, storage,
      static_cast<SQLINTEGER>(sizeof(SQLUINTEGER)), nullptr);
  std::memcpy(value, storage, sizeof(SQLUINTEGER));
  return result;
}

}  // namespace odbcpp::test
