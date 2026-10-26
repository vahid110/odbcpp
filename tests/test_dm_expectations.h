#pragma once

// Expected Driver Manager results where they differ from the direct driver
// (docs/plans/driver-manager-test-layer.md, principles 3, 4 and 6). The
// default is "same as direct": a profile without an entry must behave exactly
// like the in-process driver, so a manager that changes behaviour fails the
// test and the table is updated with evidence. Evidence for each entry is in
// docs/plans/driver-manager-phase1-wave1.md, and for the live buckets (R*) in
// docs/plans/driver-manager-live-macos.md.
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

#include <string>
#include <string_view>
#endif

namespace odbcpp::test {

namespace dm {

// Known manager deviations from the direct driver. Each has evidence in
// docs/plans/driver-manager-phase1-wave1.md (bucket letters in kEntries).
enum class Deviation {
  ManagerRaisesOdbc2States,
  ManagerOdbcVersion,
  OdbcVersionWideLengthInCharacters,
  GetDataArdTypeRejected,
  BoundWideIndicatorIsCopiedBytes,
  AnsiTextWidenedPerByte,
  AnsiGetInfoStringLengths,
  StatementDiagHeaderWrongHandleType,
  FreeConnectedConnectionInvalidatesHandle,
  GetFunctionsAcceptsNullOutput,
  ExplicitTextLengthReplacedByNts,
  NegativeBufferLengthRejectedWithNullOutput,
  NullOutputBufferSubstituted,
  DriverConnectOutputs,
  ConnectRecordsReversed,
  StateTableAfterFailedCall,
  GetDataBeforeExecuteIsSequenceError,
  Odbc2FunctionsReported,
  LoginTimeoutWritesEightBytes,
  ColumnCountWrittenOnError,
  FailedCallForgetsOpenCursor,
  ManagerWideXopenCliYear,
  MoreResultsErrorKeepsCursorOpen,
  RowCountWrittenOnError,
  RowDescriptorUnavailableAfterClose,
  AnsiTruncatedLengthIsCopied,
  SpecialColumnsNullableDropped,
  ColumnCountNullOutputAccepted,
  ReconnectAfterInternalCloseRefused,
  ManagerAnswersLoginTimeoutWhileDisconnected,
  ManagerAnswersLoginTimeoutBeforeDriverLoad,
  ServerNameIsDataSourceName,
  WideDiagLengthInUtf8Bytes,
  LegacyErrorNullBufferConsumesRecord,
  BracedDriverValueLoadedVerbatim,
  FailedDriverConnectWritesLength,
  WideConnectStringStopsAtSupplementary,
  GetFunctionsRefusesOdbc38Id,
  PutDataNullEmptyPieceRejected,
  BookmarkColumnDescribeRejected,
  RowNumberOffRowRefused,
  CatalogRefetchAfterNoDataRefused,
  FetchOrientationState,
  ConnectReappliesPreloadLoginTimeout,
  RefusedLoginTimeoutReplayedOnConnect,
  CancelClosesIdleCursor,
  AnsiNameLengthInCharacters,
  AnsiGetInfoLengthInWideBytes,
  AnsiColAttributeLengthInWideBytes,
  NumericAttributeNegativeBufferRejected,
  DriverConnectAppendsSemicolon,
  DisconnectInvalidatesManagerChildren,
  WindowsFunctionsReported,
  Odbc2FunctionBitmapOmitsBulkAndAllocHandleStd,
  BookmarkBindTypeRejected,
};

}  // namespace dm

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

inline constexpr const char* kIodbc35216 = "03.52.1623.0502";
inline constexpr const char* kUnixodbc2314 = "03.52.0002.0003";
inline constexpr const char* kWindows38126100 = "03.81.26100.0000";

struct Entry {
  Deviation deviation;
  const char* manager_version;
  const char* value;  // when the expected value is the manager's own
  const char* evidence;
};

inline constexpr Entry kEntries[] = {
    // Windows x64, SQLWCHAR 2, ODBC 3: trace/probe evidence in
    // docs/plans/driver-manager-windows-results.md. These entries describe
    // the measured manager version, not all Windows managers.
    {Deviation::ManagerOdbcVersion, kWindows38126100, "03.80.0000", "Windows W1"},
    {Deviation::AnsiGetInfoLengthInWideBytes, kWindows38126100, nullptr, "Windows W2"},
    {Deviation::AnsiColAttributeLengthInWideBytes, kWindows38126100, nullptr, "Windows W2"},
    {Deviation::WindowsFunctionsReported, kWindows38126100, nullptr, "Windows W3"},
    {Deviation::Odbc2FunctionBitmapOmitsBulkAndAllocHandleStd, kWindows38126100, nullptr, "Windows W3"},
    {Deviation::GetFunctionsAcceptsNullOutput, kWindows38126100, "", "Windows W3"},
    {Deviation::DriverConnectAppendsSemicolon, kWindows38126100, nullptr, "Windows W4"},
    {Deviation::DisconnectInvalidatesManagerChildren, kWindows38126100, nullptr, "Windows W5"},
    {Deviation::BookmarkBindTypeRejected, kWindows38126100, nullptr, "Windows W6"},
    {Deviation::GetDataBeforeExecuteIsSequenceError, kWindows38126100, nullptr, "Windows W7"},
    {Deviation::StateTableAfterFailedCall, kWindows38126100, nullptr, "Windows W7"},
    {Deviation::MoreResultsErrorKeepsCursorOpen, kWindows38126100, nullptr, "Windows W8"},
    {Deviation::ReconnectAfterInternalCloseRefused, kWindows38126100, nullptr, "Windows W9"},
    {Deviation::NumericAttributeNegativeBufferRejected, kWindows38126100, "HY090", "Windows W10"},
    {Deviation::ManagerRaisesOdbc2States, kIodbc35216, nullptr, "bucket C"},
    {Deviation::ManagerOdbcVersion, kIodbc35216, "03.52.0000", "bucket F"},
    {Deviation::ManagerOdbcVersion, kUnixodbc2314, "03.52", "bucket F"},
    {Deviation::OdbcVersionWideLengthInCharacters, kIodbc35216, nullptr, "bucket F"},
    {Deviation::GetDataArdTypeRejected, kIodbc35216, nullptr, "bucket D"},
    {Deviation::BoundWideIndicatorIsCopiedBytes, kIodbc35216, nullptr, "bucket E"},
    {Deviation::AnsiTextWidenedPerByte, kIodbc35216, nullptr, "bucket L"},
    {Deviation::AnsiGetInfoStringLengths, kIodbc35216, nullptr, "bucket P"},
    {Deviation::StatementDiagHeaderWrongHandleType, kIodbc35216, nullptr, "bucket H"},
    {Deviation::FreeConnectedConnectionInvalidatesHandle, kIodbc35216, nullptr, "bucket G"},
    // value: the diagnostic left on the connection ("" for none).
    {Deviation::GetFunctionsAcceptsNullOutput, kIodbc35216, "HY095", "bucket R"},
    {Deviation::GetFunctionsAcceptsNullOutput, kUnixodbc2314, "", "bucket R"},
    {Deviation::ExplicitTextLengthReplacedByNts, kIodbc35216, nullptr, "bucket B"},
    // value: the SQLSTATE the manager raises (ODBC 2.x S1001 mapped).
    {Deviation::NegativeBufferLengthRejectedWithNullOutput, kIodbc35216, "HY001", "bucket K"},
    {Deviation::NullOutputBufferSubstituted, kIodbc35216, nullptr, "bucket A"},
    {Deviation::DriverConnectOutputs, kIodbc35216, nullptr, "bucket I"},
    {Deviation::ConnectRecordsReversed, kUnixodbc2314, nullptr, "bucket Q"},
    {Deviation::StateTableAfterFailedCall, kIodbc35216, nullptr, "bucket O"},
    {Deviation::StateTableAfterFailedCall, kUnixodbc2314, nullptr, "bucket O"},
    {Deviation::GetDataBeforeExecuteIsSequenceError, kIodbc35216, nullptr, "bucket M"},
    {Deviation::GetDataBeforeExecuteIsSequenceError, kUnixodbc2314, nullptr, "bucket M"},
    {Deviation::Odbc2FunctionsReported, kUnixodbc2314, nullptr, "bucket N"},
    {Deviation::LoginTimeoutWritesEightBytes, kIodbc35216, nullptr, "bucket J"},
    {Deviation::ColumnCountWrittenOnError, kIodbc35216, nullptr, "bucket S"},
    {Deviation::FailedCallForgetsOpenCursor, kUnixodbc2314, nullptr, "bucket O"},
    {Deviation::ManagerWideXopenCliYear, kUnixodbc2314, "1994", "bucket T"},
    {Deviation::MoreResultsErrorKeepsCursorOpen, kIodbc35216, nullptr, "bucket V"},
    {Deviation::MoreResultsErrorKeepsCursorOpen, kUnixodbc2314, nullptr, "bucket V"},
    {Deviation::RowCountWrittenOnError, kUnixodbc2314, nullptr, "bucket W"},
    // value: the SQLSTATE the manager raises.
    {Deviation::RowDescriptorUnavailableAfterClose, kUnixodbc2314, "HY007", "bucket X"},
    {Deviation::AnsiTruncatedLengthIsCopied, kIodbc35216, nullptr, "bucket P"},
    {Deviation::SpecialColumnsNullableDropped, kIodbc35216, nullptr, "bucket U"},
    {Deviation::ColumnCountNullOutputAccepted, kIodbc35216, nullptr, "bucket S"},
    {Deviation::ReconnectAfterInternalCloseRefused, kIodbc35216, nullptr, "bucket Y"},
    {Deviation::ReconnectAfterInternalCloseRefused, kUnixodbc2314, nullptr, "bucket Y"},
    // Live Redshift buckets (docs/plans/driver-manager-live-macos.md).
    // value: the manager's own default while no application value is set.
    // iODBC answers until the first connect attempt loads the driver;
    // unixODBC whenever the handle is not connected.
    {Deviation::ManagerAnswersLoginTimeoutBeforeDriverLoad, kIodbc35216, "0", "live bucket R1"},
    {Deviation::ManagerAnswersLoginTimeoutWhileDisconnected, kUnixodbc2314, "15", "live bucket R1"},
    // value: the SQLSTATE the manager raises for a connection string passed
    // as SQLConnect ServerName.
    {Deviation::ServerNameIsDataSourceName, kIodbc35216, "IM002", "live bucket R2"},
    {Deviation::ServerNameIsDataSourceName, kUnixodbc2314, "HY090", "live bucket R2"},
    {Deviation::WideDiagLengthInUtf8Bytes, kIodbc35216, nullptr, "live bucket R3"},
    {Deviation::WideDiagLengthInUtf8Bytes, kUnixodbc2314, nullptr, "live bucket R3"},
    // value: what the manager returns for the NULL-buffer, negative-length call.
    {Deviation::LegacyErrorNullBufferConsumesRecord, kIodbc35216, "-1", "live bucket R4"},
    {Deviation::LegacyErrorNullBufferConsumesRecord, kUnixodbc2314, "0", "live bucket R4"},
    {Deviation::BracedDriverValueLoadedVerbatim, kIodbc35216, nullptr, "live bucket R5"},
    // value: the StringLength2 the manager writes after a failed connect.
    {Deviation::FailedDriverConnectWritesLength, kIodbc35216, "1", "live bucket R6"},
    {Deviation::WideConnectStringStopsAtSupplementary, kUnixodbc2314, nullptr, "live bucket R7"},
    // value: the SQLSTATE the manager raises.
    {Deviation::GetFunctionsRefusesOdbc38Id, kUnixodbc2314, "HY095", "live bucket R8"},
    {Deviation::PutDataNullEmptyPieceRejected, kIodbc35216, nullptr, "live bucket R9"},
    {Deviation::BookmarkColumnDescribeRejected, kIodbc35216, nullptr, "live bucket R10"},
    {Deviation::RowNumberOffRowRefused, kUnixodbc2314, nullptr, "live bucket R11"},
    {Deviation::CatalogRefetchAfterNoDataRefused, kIodbc35216, nullptr, "live bucket R12"},
    // value: the SQLSTATE the manager raises for an unknown fetch orientation.
    {Deviation::FetchOrientationState, kIodbc35216, "HY092", "live bucket R13"},
    {Deviation::ConnectReappliesPreloadLoginTimeout, kIodbc35216, nullptr, "live bucket R14"},
    {Deviation::RefusedLoginTimeoutReplayedOnConnect, kUnixodbc2314, nullptr, "live bucket R15"},
    {Deviation::CancelClosesIdleCursor, kUnixodbc2314, nullptr, "live bucket R16"},
    {Deviation::AnsiNameLengthInCharacters, kIodbc35216, nullptr, "live bucket R17"},
};

inline const Entry* entry(Deviation deviation) {
  for (const auto& candidate : kEntries) {
    if (candidate.deviation == deviation && manager_version() == candidate.manager_version) {
      return &candidate;
    }
  }
  return nullptr;
}

inline bool deviates(Deviation deviation) { return entry(deviation) != nullptr; }

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

#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
namespace dm {
inline bool deviates(Deviation) { return false; }
}  // namespace dm
#endif

namespace dm {

// The value a case expects at a deviating site: the direct value, or the
// value this manager is recorded to produce.
template <class T>
T value(Deviation deviation, T direct, T on_deviation) {
  return deviates(deviation) ? on_deviation : direct;
}

// The manager's own value for a deviation whose entry carries one.
inline const char* text(Deviation deviation, const char* direct) {
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
  if (const auto* found = entry(deviation); found && found->value) return found->value;
#else
  (void)deviation;
#endif
  return direct;
}

// What the server stores for ANSI statement text when the manager widens it
// byte by byte (AnsiTextWidenedPerByte, bucket L): each byte becomes the code
// point of the same value, sent on as UTF-8.
inline std::string latin1_widened(std::string_view text) {
  std::string widened;
  for (const unsigned char byte : text) {
    if (byte < 0x80) {
      widened.push_back(static_cast<char>(byte));
    } else {
      widened.push_back(static_cast<char>(0xc0 | (byte >> 6)));
      widened.push_back(static_cast<char>(0x80 | (byte & 0x3f)));
    }
  }
  return widened;
}

// SQLGetFunctions IDs the manager reports although the driver does not export
// them (Odbc2FunctionsReported, bucket N): ODBC 2.x functions it maps onto the
// driver's ODBC 3 functions, and functions it implements itself. Numeric,
// because not every manager's headers define all of these names.
inline bool manager_reports_function(unsigned id) {
  if (deviates(Deviation::WindowsFunctionsReported)) {
    // Measured full bitmap, including manager-owned connection enumeration.
    // Unlike unixODBC, this manager does not advertise SQLSetScrollOptions.
    static constexpr unsigned kWindowsIds[] = {
        1, 2, 3, 14, 15, 22, 23, 42, 46, 50, 51, 57, 64, 71, 73, 1002, 1550, 1551};
    for (const auto candidate : kWindowsIds) {
      if (candidate == id) return true;
    }
    return false;
  }
  if (!deviates(Deviation::Odbc2FunctionsReported)) return false;
  static constexpr unsigned kIds[] = {
      1,     // SQLAllocConnect
      2,     // SQLAllocEnv
      3,     // SQLAllocStmt
      14,    // SQLFreeConnect
      15,    // SQLFreeEnv
      22,    // SQLSetParam
      23,    // SQLTransact
      42,    // SQLGetConnectOption
      46,    // SQLGetStmtOption
      50,    // SQLSetConnectOption
      51,    // SQLSetStmtOption
      57,    // SQLDataSources
      64,    // SQLParamOptions
      69,    // SQLSetScrollOptions
      71,    // SQLDrivers
      73,    // SQLAllocHandleStd
      1002,  // SQLBindParam
  };
  for (const auto candidate : kIds) {
    if (candidate == id) return true;
  }
  return false;
}

// A deviation after which the rest of the case is unreachable through this
// manager: record which one in the XML, so the stop is visible.
inline bool stops_case(Deviation deviation) {
  if (!deviates(deviation)) return false;
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
  ::testing::Test::RecordProperty("dm_case_stopped_by", entry(deviation)->evidence);
#endif
  return true;
}

}  // namespace dm

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

#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
namespace dm {

// ODBC 3.x replacement for an ODBC 2.x SQLSTATE (ODBC Programmer's Reference,
// appendix A, "SQLSTATE Mappings"), for the states seen so far.
inline const char* odbc3_state(std::string_view odbc2) {
  struct Mapping { const char* odbc2; const char* odbc3; };
  static constexpr Mapping kMappings[] = {
      {"S1000", "HY000"}, {"S1001", "HY001"}, {"S1002", "07009"}, {"S1003", "HY003"},
      {"S1004", "HY004"}, {"S1009", "HY009"}, {"S1010", "HY010"},
      {"S1090", "HY090"}, {"S1091", "HY091"}, {"S1092", "HY092"},
      {"S1093", "07009"}, {"S1096", "HY096"}, {"S1105", "HY105"},
      {"S1C00", "HYC00"}, {"S1T00", "HYT00"},
  };
  for (const auto& mapping : kMappings) {
    if (odbc2 == mapping.odbc2) return mapping.odbc3;
  }
  return nullptr;
}

// A manager-raised record whose SQLSTATE is the ODBC 2.x form although the
// application set SQL_OV_ODBC3: assert that this profile is expected to do
// so, and give the caller the ODBC 3.x state so its own check still applies.
inline void normalize_manager_state(SQLCHAR* sqlstate, Origin origin) {
  const std::string_view state(reinterpret_cast<const char*>(sqlstate));
  if (!state.starts_with("S1")) return;
  const char* replacement = odbc3_state(state);
  EXPECT_TRUE(origin == Origin::Manager && deviates(Deviation::ManagerRaisesOdbc2States) &&
              replacement != nullptr)
      << "ODBC 2.x SQLSTATE " << state << " from a "
      << (origin == Origin::Manager ? "manager" : "driver") << "-raised record; manager "
      << manager_version();
  if (replacement) std::memcpy(sqlstate, replacement, 6);
}

}  // namespace dm
#endif

// SQLGetDiagRec for suites built both ways. Direct builds call the driver
// unchanged. Manager builds assert two known deviations against the table and
// then hand the caller the direct-equivalent answer, so the case goes on to
// check the record itself:
// - the NULL-message, zero-length call (NullBufferDiag): the call is made as
//   given and checked, then the record is read again with a real buffer;
// - manager-raised ODBC 2.x SQLSTATEs (ManagerRaisesOdbc2States).
inline SQLRETURN diag_rec(SQLSMALLINT handle_type, SQLHANDLE handle,
                          SQLSMALLINT record, SQLCHAR* sqlstate,
                          SQLINTEGER* native_error, SQLCHAR* message,
                          SQLSMALLINT buffer_length, SQLSMALLINT* text_length) {
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
  SQLCHAR full_state[6]{};
  SQLINTEGER full_native = 0;
  SQLCHAR full_message[4096]{};
  SQLSMALLINT full_length = 0;
  if (message != nullptr || buffer_length != 0) {
    const SQLRETURN result = ::SQLGetDiagRec(handle_type, handle, record, sqlstate,
                                             native_error, message, buffer_length,
                                             text_length);
    if ((result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO) && sqlstate &&
        ::SQLGetDiagRec(handle_type, handle, record, full_state, &full_native,
                        full_message, static_cast<SQLSMALLINT>(sizeof(full_message)),
                        &full_length) == SQL_SUCCESS) {
      dm::normalize_manager_state(
          sqlstate, dm::record_origin(reinterpret_cast<const char*>(full_message)));
    }
    return result;
  }
  SQLCHAR null_state[6] = {'?', '?', '?', '?', '?', '\0'};
  const SQLRETURN null_result = ::SQLGetDiagRec(
      handle_type, handle, record, null_state, nullptr, nullptr, 0, nullptr);
  const SQLRETURN result = ::SQLGetDiagRec(
      handle_type, handle, record, full_state, &full_native, full_message,
      static_cast<SQLSMALLINT>(sizeof(full_message)), &full_length);
  if (native_error) *native_error = full_native;
  if (text_length) *text_length = full_length;
  if (result != SQL_SUCCESS) {
    // No record (or an unreadable one): the NULL-buffer call must agree.
    if (sqlstate) std::memcpy(sqlstate, full_state, sizeof(full_state));
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
  dm::normalize_manager_state(full_state, origin);
  if (sqlstate) std::memcpy(sqlstate, full_state, sizeof(full_state));
  return result;
#else
  return ::SQLGetDiagRec(handle_type, handle, record, sqlstate, native_error,
                         message, buffer_length, text_length);
#endif
}

}  // namespace odbcpp::test
