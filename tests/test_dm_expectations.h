#pragma once

// Expected Driver Manager results where they differ from the direct driver
// (docs/plans/driver-manager-test-layer.md, principles 3, 4 and 6). The
// default is "same as direct": a profile without an entry must behave exactly
// like the in-process driver, so a manager that changes behaviour fails the
// test and the table is updated with evidence. Evidence for each entry is in
// docs/plans/driver-manager-phase1-wave1.md.
//
// Direct builds compile none of this: diag_rec is ::SQLGetDiagRec itself.
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include "tests/test_dm_odbc.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <string_view>
#else
#include "odbc/odbc_api.h"
#endif

namespace odbcpp::test {

#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
namespace dm {

enum class Origin { Driver, Manager };

// Which component raised a diagnostic record. Manager messages carry the
// manager's vendor and component prefix; this driver's messages carry none.
inline Origin record_origin(std::string_view message) {
  return message.starts_with("[iODBC][Driver Manager]") ||
                 message.starts_with("[unixODBC][Driver Manager]") ||
                 message.starts_with("[Microsoft][ODBC Driver Manager]")
             ? Origin::Manager
             : Origin::Driver;
}

// The manager's own version (SQL_DM_VER), read once on an unconnected handle.
inline const std::string& manager_version() {
  static const std::string version = [] {
    SQLHENV environment = SQL_NULL_HENV;
    SQLHDBC connection = SQL_NULL_HDBC;
    SQLCHAR value[64]{};
    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment) == SQL_SUCCESS) {
      SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                    reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
      if (SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection) == SQL_SUCCESS) {
        SQLGetInfo(connection, SQL_DM_VER, value, sizeof(value), nullptr);
        SQLFreeHandle(SQL_HANDLE_DBC, connection);
      }
      SQLFreeHandle(SQL_HANDLE_ENV, environment);
    }
    return std::string(reinterpret_cast<const char*>(value));
  }();
  return version;
}

// Records posted by SQLConnect or SQLDriverConnect, per connection handle, as
// SQLSTATE plus message. unixODBC copies those records into its own store
// during the connect call instead of reading them from the driver later.
// Suites capture them through their fixture connect forwards (note_connect
// below); a later record on the same handle matches only if it is the same.
inline std::map<SQLHANDLE, std::set<std::string>>& connect_posted() {
  static std::map<SQLHANDLE, std::set<std::string>> records;
  return records;
}

inline std::string record_key(const SQLCHAR* sqlstate, const SQLCHAR* message) {
  return std::string(reinterpret_cast<const char*>(sqlstate)) + '\n' +
         reinterpret_cast<const char*>(message);
}

inline bool posted_by_connect(SQLSMALLINT handle_type, SQLHANDLE handle,
                              const std::string& key) {
  if (handle_type != SQL_HANDLE_DBC) return false;
  const auto found = connect_posted().find(handle);
  return found != connect_posted().end() && found->second.contains(key);
}

// ANSI SQLGetDiagRec with MessageText NULL and BufferLength 0 on an existing
// record. Direct: SQL_SUCCESS with the SQLSTATE.
struct NullBufferDiagExpectation {
  SQLRETURN result;
  bool writes_sqlstate;
  const char* evidence;
};

inline NullBufferDiagExpectation null_buffer_diag_expectation(Origin origin,
                                                              bool from_connect) {
  const auto& version = manager_version();
  if (version == "03.52.1623.0502" && origin == Origin::Driver) {
    // iODBC 3.52.16 passes the driver a non-null 0-length scratch buffer, so
    // the driver reports truncation, and iODBC does not copy the SQLSTATE.
    return {SQL_SUCCESS_WITH_INFO, false, "wave 1 bucket A (iODBC 3.52.16)"};
  }
  if (version == "03.52.0002.0003" && (origin == Origin::Manager || from_connect)) {
    // unixODBC 2.3.14 reports truncation for records it holds itself: the
    // ones it raised, and the driver's connect records it copied.
    return {SQL_SUCCESS_WITH_INFO, true, "wave 1 bucket A2 (unixODBC 2.3.14)"};
  }
  return {SQL_SUCCESS, true, "same as direct"};
}

}  // namespace dm
#endif

// Fixture connect forwards pass their result through this so manager builds
// know which connection records came from a connect call.
inline SQLRETURN note_connect(SQLHDBC connection, SQLRETURN result) {
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
  // Reading the records is nondestructive; the case still sees all of them.
  auto& records = dm::connect_posted()[connection];
  records.clear();
  for (SQLSMALLINT record = 1;; ++record) {
    SQLCHAR state[6]{};
    SQLCHAR message[4096]{};
    if (::SQLGetDiagRec(SQL_HANDLE_DBC, connection, record, state, nullptr, message,
                        static_cast<SQLSMALLINT>(sizeof(message)), nullptr) != SQL_SUCCESS) {
      break;
    }
    records.insert(dm::record_key(state, message));
  }
#else
  (void)connection;
#endif
  return result;
}

// SQLGetDiagRec for suites built both ways. Direct builds call the driver
// unchanged. Manager builds handle the NULL-message, zero-length call: they
// assert its result against the table above, then read the record again with
// a real buffer and return that, so the case goes on to check the record.
inline SQLRETURN diag_rec(SQLSMALLINT handle_type, SQLHANDLE handle,
                          SQLSMALLINT record, SQLCHAR* sqlstate,
                          SQLINTEGER* native_error, SQLCHAR* message,
                          SQLSMALLINT buffer_length, SQLSMALLINT* text_length) {
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
  if (message != nullptr || buffer_length != 0) {
    return ::SQLGetDiagRec(handle_type, handle, record, sqlstate, native_error,
                           message, buffer_length, text_length);
  }
  SQLCHAR null_state[6] = {'?', '?', '?', '?', '?', '\0'};
  const SQLRETURN null_result = ::SQLGetDiagRec(
      handle_type, handle, record, null_state, nullptr, nullptr, 0, nullptr);
  SQLCHAR full_state[6]{};
  SQLINTEGER full_native = 0;
  SQLCHAR full_message[4096]{};
  SQLSMALLINT full_length = 0;
  const SQLRETURN result = ::SQLGetDiagRec(
      handle_type, handle, record, full_state, &full_native, full_message,
      static_cast<SQLSMALLINT>(sizeof(full_message)), &full_length);
  if (sqlstate) std::memcpy(sqlstate, full_state, sizeof(full_state));
  if (native_error) *native_error = full_native;
  if (text_length) *text_length = full_length;
  if (result != SQL_SUCCESS) {
    // No record (or an unreadable one): the NULL-buffer call must agree.
    EXPECT_EQ(result, null_result) << "NULL-buffer SQLGetDiagRec, record " << record;
    return result;
  }
  const auto origin = dm::record_origin(reinterpret_cast<const char*>(full_message));
  const auto expected = dm::null_buffer_diag_expectation(
      origin, dm::posted_by_connect(handle_type, handle,
                                    dm::record_key(full_state, full_message)));
  EXPECT_EQ(expected.result, null_result)
      << "NULL-buffer SQLGetDiagRec on " << reinterpret_cast<const char*>(full_state)
      << " (" << (origin == dm::Origin::Manager ? "manager" : "driver")
      << "-raised); manager " << dm::manager_version() << "; " << expected.evidence;
  EXPECT_STREQ(expected.writes_sqlstate ? reinterpret_cast<const char*>(full_state) : "?????",
               reinterpret_cast<const char*>(null_state))
      << "NULL-buffer SQLGetDiagRec SQLSTATE; " << expected.evidence;
  return result;
#else
  return ::SQLGetDiagRec(handle_type, handle, record, sqlstate, native_error,
                         message, buffer_length, text_length);
#endif
}

}  // namespace odbcpp::test
