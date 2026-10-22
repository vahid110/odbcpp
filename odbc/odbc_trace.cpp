#include "odbc_trace.h"

#include "connection_string.h"
#include "odbc_handles.h"
#include "unicode.h"
#include "odbcpp/database/backend_provider.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rs::odbc::trace {
namespace {

using R = ArgRole;
using rs::core::logging::FieldSensitivity;
using rs::core::logging::LogField;
using rs::core::logging::LogLevel;

// ---- Exported signatures ---------------------------------------------------
// Names follow the ODBC reference. ANSI and wide variants share one signature;
// the captured pointee width selects narrow or SQLWCHAR text decoding.

constexpr ArgSpec kAllocHandle[]{
    {"HandleType", R::HandleType}, {"InputHandle", R::Handle},
    {"OutputHandlePtr", R::OutPointer}};
constexpr ArgSpec kHandleTypeAndHandle[]{
    {"HandleType", R::HandleType}, {"Handle", R::Handle}};
constexpr ArgSpec kConnect[]{
    {"ConnectionHandle", R::Handle}, {"ServerName", R::Text, 2},
    {"NameLength1", R::Length}, {"UserName", R::Text, 4},
    {"NameLength2", R::Length}, {"Authentication", R::Secret},
    {"NameLength3", R::Secret}};
constexpr ArgSpec kDriverConnect[]{
    {"ConnectionHandle", R::Handle}, {"WindowHandle", R::Handle},
    {"InConnectionString", R::ConnectionText, 3},
    {"StringLength1", R::Secret},
    {"OutConnectionString", R::OutConnectionText, 5},
    {"BufferLength", R::Integer}, {"StringLength2Ptr", R::Secret},
    {"DriverCompletion", R::Integer}};
constexpr ArgSpec kConnection[]{{"ConnectionHandle", R::Handle}};
constexpr ArgSpec kSetConnectAttr[]{
    {"ConnectionHandle", R::Handle}, {"Attribute", R::Integer},
    {"ValuePtr", R::Pointer}, {"StringLength", R::Length}};
constexpr ArgSpec kGetConnectAttr[]{
    {"ConnectionHandle", R::Handle}, {"Attribute", R::Integer},
    {"ValuePtr", R::Pointer}, {"BufferLength", R::Integer},
    {"StringLengthPtr", R::OutLength}};
constexpr ArgSpec kEndTran[]{
    {"HandleType", R::HandleType}, {"Handle", R::Handle},
    {"CompletionType", R::Integer}};
constexpr ArgSpec kStatementText[]{
    {"StatementHandle", R::Handle}, {"StatementText", R::QueryText, 2},
    {"TextLength", R::Length}};
constexpr ArgSpec kStatement[]{{"StatementHandle", R::Handle}};
constexpr ArgSpec kFetchScroll[]{
    {"StatementHandle", R::Handle}, {"FetchOrientation", R::Integer},
    {"FetchOffset", R::Integer}};
constexpr ArgSpec kBulkOperations[]{
    {"StatementHandle", R::Handle}, {"Operation", R::Integer}};
constexpr ArgSpec kSetPos[]{
    {"StatementHandle", R::Handle}, {"RowNumber", R::Integer},
    {"Operation", R::Integer}, {"LockType", R::Integer}};
constexpr ArgSpec kGetData[]{
    {"StatementHandle", R::Handle}, {"Col_or_Param_Num", R::Integer},
    {"TargetType", R::Integer}, {"TargetValuePtr", R::Pointer},
    {"BufferLength", R::Integer}, {"StrLen_or_IndPtr", R::OutLength}};
constexpr ArgSpec kSetStmtAttr[]{
    {"StatementHandle", R::Handle}, {"Attribute", R::Integer},
    {"ValuePtr", R::Pointer}, {"StringLength", R::Length}};
constexpr ArgSpec kGetStmtAttr[]{
    {"StatementHandle", R::Handle}, {"Attribute", R::Integer},
    {"ValuePtr", R::Pointer}, {"BufferLength", R::Integer},
    {"StringLengthPtr", R::OutLength}};
constexpr ArgSpec kFreeStmt[]{
    {"StatementHandle", R::Handle}, {"Option", R::Integer}};
constexpr ArgSpec kGetDiagRec[]{
    {"HandleType", R::HandleType}, {"Handle", R::Handle},
    {"RecNumber", R::Integer}, {"SQLState", R::OutSqlState},
    {"NativeErrorPtr", R::OutInteger}, {"MessageText", R::OutText, 6},
    {"BufferLength", R::Integer}, {"TextLengthPtr", R::OutLength}};
constexpr ArgSpec kGetDiagField[]{
    {"HandleType", R::HandleType}, {"Handle", R::Handle},
    {"RecNumber", R::Integer}, {"DiagIdentifier", R::Integer},
    {"DiagInfoPtr", R::Pointer}, {"BufferLength", R::Integer},
    {"StringLengthPtr", R::OutLength}};
constexpr ArgSpec kError[]{
    {"EnvironmentHandle", R::Handle}, {"ConnectionHandle", R::Handle},
    {"StatementHandle", R::Handle}, {"SQLState", R::OutSqlState},
    {"NativeErrorPtr", R::OutInteger}, {"MessageText", R::OutText, 6},
    {"BufferLength", R::Integer}, {"TextLengthPtr", R::OutLength}};
constexpr ArgSpec kGetInfo[]{
    {"ConnectionHandle", R::Handle}, {"InfoType", R::Integer},
    {"InfoValuePtr", R::Pointer}, {"BufferLength", R::Integer},
    {"StringLengthPtr", R::OutLength}};
constexpr ArgSpec kGetFunctions[]{
    {"ConnectionHandle", R::Handle}, {"FunctionId", R::Integer},
    {"SupportedPtr", R::Pointer}};
constexpr ArgSpec kNativeSql[]{
    {"ConnectionHandle", R::Handle}, {"InStatementText", R::QueryText, 2},
    {"TextLength1", R::Length}, {"OutStatementText", R::OutQueryText, 4},
    {"BufferLength", R::Integer}, {"TextLength2Ptr", R::OutLength}};
constexpr ArgSpec kSetEnvAttr[]{
    {"EnvironmentHandle", R::Handle}, {"Attribute", R::Integer},
    {"ValuePtr", R::Pointer}, {"StringLength", R::Length}};
constexpr ArgSpec kGetEnvAttr[]{
    {"EnvironmentHandle", R::Handle}, {"Attribute", R::Integer},
    {"ValuePtr", R::Pointer}, {"BufferLength", R::Integer},
    {"StringLengthPtr", R::OutLength}};
constexpr ArgSpec kNumResultCols[]{
    {"StatementHandle", R::Handle}, {"ColumnCountPtr", R::OutInteger}};
constexpr ArgSpec kRowCount[]{
    {"StatementHandle", R::Handle}, {"RowCountPtr", R::OutInteger}};
constexpr ArgSpec kGetTypeInfo[]{
    {"StatementHandle", R::Handle}, {"DataType", R::Integer}};
constexpr ArgSpec kColumns[]{
    {"StatementHandle", R::Handle}, {"CatalogName", R::Text, 2},
    {"NameLength1", R::Length}, {"SchemaName", R::Text, 4},
    {"NameLength2", R::Length}, {"TableName", R::Text, 6},
    {"NameLength3", R::Length}, {"ColumnName", R::Text, 8},
    {"NameLength4", R::Length}};
constexpr ArgSpec kPrimaryKeys[]{
    {"StatementHandle", R::Handle}, {"CatalogName", R::Text, 2},
    {"NameLength1", R::Length}, {"SchemaName", R::Text, 4},
    {"NameLength2", R::Length}, {"TableName", R::Text, 6},
    {"NameLength3", R::Length}};
constexpr ArgSpec kForeignKeys[]{
    {"StatementHandle", R::Handle}, {"PKCatalogName", R::Text, 2},
    {"NameLength1", R::Length}, {"PKSchemaName", R::Text, 4},
    {"NameLength2", R::Length}, {"PKTableName", R::Text, 6},
    {"NameLength3", R::Length}, {"FKCatalogName", R::Text, 8},
    {"NameLength4", R::Length}, {"FKSchemaName", R::Text, 10},
    {"NameLength5", R::Length}, {"FKTableName", R::Text, 12},
    {"NameLength6", R::Length}};
constexpr ArgSpec kStatistics[]{
    {"StatementHandle", R::Handle}, {"CatalogName", R::Text, 2},
    {"NameLength1", R::Length}, {"SchemaName", R::Text, 4},
    {"NameLength2", R::Length}, {"TableName", R::Text, 6},
    {"NameLength3", R::Length}, {"Unique", R::Integer},
    {"Reserved", R::Integer}};
constexpr ArgSpec kProcedures[]{
    {"StatementHandle", R::Handle}, {"CatalogName", R::Text, 2},
    {"NameLength1", R::Length}, {"SchemaName", R::Text, 4},
    {"NameLength2", R::Length}, {"ProcName", R::Text, 6},
    {"NameLength3", R::Length}};
constexpr ArgSpec kProcedureColumns[]{
    {"StatementHandle", R::Handle}, {"CatalogName", R::Text, 2},
    {"NameLength1", R::Length}, {"SchemaName", R::Text, 4},
    {"NameLength2", R::Length}, {"ProcName", R::Text, 6},
    {"NameLength3", R::Length}, {"ColumnName", R::Text, 8},
    {"NameLength4", R::Length}};
constexpr ArgSpec kSpecialColumns[]{
    {"StatementHandle", R::Handle}, {"IdentifierType", R::Integer},
    {"CatalogName", R::Text, 3}, {"NameLength1", R::Length},
    {"SchemaName", R::Text, 5}, {"NameLength2", R::Length},
    {"TableName", R::Text, 7}, {"NameLength3", R::Length},
    {"Scope", R::Integer}, {"Nullable", R::Integer}};
constexpr ArgSpec kTables[]{
    {"StatementHandle", R::Handle}, {"CatalogName", R::Text, 2},
    {"NameLength1", R::Length}, {"SchemaName", R::Text, 4},
    {"NameLength2", R::Length}, {"TableName", R::Text, 6},
    {"NameLength3", R::Length}, {"TableType", R::Text, 8},
    {"NameLength4", R::Length}};
constexpr ArgSpec kDescribeCol[]{
    {"StatementHandle", R::Handle}, {"ColumnNumber", R::Integer},
    {"ColumnName", R::OutText, 3}, {"BufferLength", R::Integer},
    {"NameLengthPtr", R::OutLength}, {"DataTypePtr", R::OutInteger},
    {"ColumnSizePtr", R::OutInteger}, {"DecimalDigitsPtr", R::OutInteger},
    {"NullablePtr", R::OutInteger}};
constexpr ArgSpec kColAttribute[]{
    {"StatementHandle", R::Handle}, {"ColumnNumber", R::Integer},
    {"FieldIdentifier", R::Integer}, {"CharacterAttributePtr", R::Pointer},
    {"BufferLength", R::Integer}, {"StringLengthPtr", R::OutLength},
    {"NumericAttributePtr", R::Pointer}};
constexpr ArgSpec kNumParams[]{
    {"StatementHandle", R::Handle}, {"ParameterCountPtr", R::OutInteger}};
constexpr ArgSpec kParamData[]{
    {"StatementHandle", R::Handle}, {"ValuePtrPtr", R::OutPointer}};
constexpr ArgSpec kPutData[]{
    {"StatementHandle", R::Handle}, {"DataPtr", R::Pointer},
    {"StrLen_or_Ind", R::Length}};
constexpr ArgSpec kBindParameter[]{
    {"StatementHandle", R::Handle}, {"ParameterNumber", R::Integer},
    {"InputOutputType", R::Integer}, {"ValueType", R::Integer},
    {"ParameterType", R::Integer}, {"ColumnSize", R::Integer},
    {"DecimalDigits", R::Integer}, {"ParameterValuePtr", R::Pointer},
    {"BufferLength", R::Integer}, {"StrLen_or_IndPtr", R::Pointer}};
constexpr ArgSpec kBindCol[]{
    {"StatementHandle", R::Handle}, {"ColumnNumber", R::Integer},
    {"TargetType", R::Integer}, {"TargetValuePtr", R::Pointer},
    {"BufferLength", R::Integer}, {"StrLen_or_IndPtr", R::Pointer}};
constexpr ArgSpec kDescribeParam[]{
    {"StatementHandle", R::Handle}, {"ParameterNumber", R::Integer},
    {"DataTypePtr", R::OutInteger}, {"ParameterSizePtr", R::OutInteger},
    {"DecimalDigitsPtr", R::OutInteger}, {"NullablePtr", R::OutInteger}};
constexpr ArgSpec kGetDescField[]{
    {"DescriptorHandle", R::Handle}, {"RecNumber", R::Integer},
    {"FieldIdentifier", R::Integer}, {"ValuePtr", R::Pointer},
    {"BufferLength", R::Integer}, {"StringLengthPtr", R::OutLength}};
constexpr ArgSpec kGetDescRec[]{
    {"DescriptorHandle", R::Handle}, {"RecNumber", R::Integer},
    {"Name", R::OutText, 3}, {"BufferLength", R::Integer},
    {"StringLengthPtr", R::OutLength}, {"TypePtr", R::OutInteger},
    {"SubTypePtr", R::OutInteger}, {"LengthPtr", R::OutInteger},
    {"PrecisionPtr", R::OutInteger}, {"ScalePtr", R::OutInteger},
    {"NullablePtr", R::OutInteger}};
constexpr ArgSpec kSetDescField[]{
    {"DescriptorHandle", R::Handle}, {"RecNumber", R::Integer},
    {"FieldIdentifier", R::Integer}, {"ValuePtr", R::Pointer},
    {"BufferLength", R::Length}};
constexpr ArgSpec kSetDescRec[]{
    {"DescriptorHandle", R::Handle}, {"RecNumber", R::Integer},
    {"Type", R::Integer}, {"SubType", R::Integer}, {"Length", R::Integer},
    {"Precision", R::Integer}, {"Scale", R::Integer},
    {"DataPtr", R::Pointer}, {"StringLengthPtr", R::Pointer},
    {"IndicatorPtr", R::Pointer}};
constexpr ArgSpec kCopyDesc[]{
    {"SourceDescHandle", R::Handle}, {"TargetDescHandle", R::Handle}};

constexpr ApiSpec kApis[]{
    {"SQLAllocHandle", kAllocHandle},
    {"SQLFreeHandle", kHandleTypeAndHandle},
    {"SQLConnect", kConnect},
    {"SQLConnectW", kConnect},
    {"SQLDriverConnect", kDriverConnect},
    {"SQLDriverConnectW", kDriverConnect},
    {"SQLDisconnect", kConnection},
    {"SQLSetConnectAttr", kSetConnectAttr},
    {"SQLSetConnectAttrW", kSetConnectAttr},
    {"SQLGetConnectAttr", kGetConnectAttr},
    {"SQLGetConnectAttrW", kGetConnectAttr},
    {"SQLEndTran", kEndTran},
    {"SQLExecDirect", kStatementText},
    {"SQLExecDirectW", kStatementText},
    {"SQLFetch", kStatement},
    {"SQLFetchScroll", kFetchScroll},
    {"SQLBulkOperations", kBulkOperations},
    {"SQLSetPos", kSetPos},
    {"SQLMoreResults", kStatement},
    {"SQLGetData", kGetData},
    {"SQLSetStmtAttr", kSetStmtAttr},
    {"SQLSetStmtAttrW", kSetStmtAttr},
    {"SQLGetStmtAttr", kGetStmtAttr},
    {"SQLGetStmtAttrW", kGetStmtAttr},
    {"SQLCloseCursor", kStatement},
    {"SQLFreeStmt", kFreeStmt},
    {"SQLGetDiagRec", kGetDiagRec},
    {"SQLGetDiagRecW", kGetDiagRec},
    {"SQLGetDiagField", kGetDiagField},
    {"SQLGetDiagFieldW", kGetDiagField},
    {"SQLError", kError},
    {"SQLErrorW", kError},
    {"SQLGetInfo", kGetInfo},
    {"SQLGetInfoW", kGetInfo},
    {"SQLGetFunctions", kGetFunctions},
    {"SQLNativeSql", kNativeSql},
    {"SQLNativeSqlW", kNativeSql},
    {"SQLSetEnvAttr", kSetEnvAttr},
    {"SQLGetEnvAttr", kGetEnvAttr},
    {"SQLNumResultCols", kNumResultCols},
    {"SQLRowCount", kRowCount},
    {"SQLGetTypeInfo", kGetTypeInfo},
    {"SQLGetTypeInfoW", kGetTypeInfo},
    {"SQLColumns", kColumns},
    {"SQLColumnsW", kColumns},
    {"SQLPrimaryKeys", kPrimaryKeys},
    {"SQLPrimaryKeysW", kPrimaryKeys},
    {"SQLForeignKeys", kForeignKeys},
    {"SQLForeignKeysW", kForeignKeys},
    {"SQLStatistics", kStatistics},
    {"SQLStatisticsW", kStatistics},
    {"SQLProcedures", kProcedures},
    {"SQLProceduresW", kProcedures},
    {"SQLProcedureColumns", kProcedureColumns},
    {"SQLProcedureColumnsW", kProcedureColumns},
    {"SQLSpecialColumns", kSpecialColumns},
    {"SQLSpecialColumnsW", kSpecialColumns},
    {"SQLTables", kTables},
    {"SQLTablesW", kTables},
    {"SQLDescribeCol", kDescribeCol},
    {"SQLDescribeColW", kDescribeCol},
    {"SQLColAttribute", kColAttribute},
    {"SQLColAttributeW", kColAttribute},
    {"SQLPrepare", kStatementText},
    {"SQLPrepareW", kStatementText},
    {"SQLExecute", kStatement},
    {"SQLNumParams", kNumParams},
    {"SQLParamData", kParamData},
    {"SQLPutData", kPutData},
    {"SQLBindParameter", kBindParameter},
    {"SQLBindCol", kBindCol},
    {"SQLDescribeParam", kDescribeParam},
    {"SQLGetDescField", kGetDescField},
    {"SQLGetDescFieldW", kGetDescField},
    {"SQLGetDescRec", kGetDescRec},
    {"SQLGetDescRecW", kGetDescRec},
    {"SQLSetDescField", kSetDescField},
    {"SQLSetDescFieldW", kSetDescField},
    {"SQLSetDescRec", kSetDescRec},
    {"SQLCopyDesc", kCopyDesc},
    {"SQLCancel", kStatement},
    {"SQLCancelHandle", kHandleTypeAndHandle},
};

// ---- Configuration -----------------------------------------------------------

struct TraceKey {
  std::string_view driver_key;   // upper-case driver-section key
  std::string_view environment;  // process override
};

constexpr TraceKey kTraceKeys[]{
    {"ODBCTRACE", "ODBCPP_TRACE"},
    {"ODBCTRACEFILE", "ODBCPP_TRACE_FILE"},
    {"ODBCTRACEFORMAT", "ODBCPP_TRACE_FORMAT"},
    {"ODBCTRACEQUERIES", "ODBCPP_TRACE_QUERIES"},
    {"ODBCTRACESENSITIVE", "ODBCPP_TRACE_SENSITIVE"},
    {"ODBCTRACEMAXSIZE", "ODBCPP_TRACE_MAX_SIZE"},
    {"ODBCTRACEMAXFILES", "ODBCPP_TRACE_MAX_FILES"},
};

std::string upper(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  return result;
}

std::optional<std::string> process_environment(std::string_view name) {
  const std::string key(name);
#ifdef _WIN32
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, key.c_str()) != 0 || value == nullptr) {
    return std::nullopt;
  }
  std::string result(value);
  std::free(value);
  return result;
#else
  const char* value = std::getenv(key.c_str());
  if (value == nullptr) return std::nullopt;
  return std::string(value);
#endif
}

std::map<std::string, std::string> trusted_driver_parameters() {
  const auto& provider = rs::core::database::configured_backend_provider();
  auto parameters =
      DSNReader::read_driver_config(provider.identity().driver_name);
  if (parameters.empty()) parameters = DSNReader::read_driver_config("ODBCPP");
  return parameters;
}

// ---- Process tracer state ------------------------------------------------------

enum : int { kUnresolved = 0, kDisabled = 1, kEnabled = 2 };
std::atomic<int> tracer_state{kUnresolved};

// Storage is never freed so ODBC calls from an application's static
// destructors stay safe; the shutdown guard below still closes the sink.
std::mutex& tracer_mutex() {
  static auto* mutex = new std::mutex;
  return *mutex;
}

std::shared_ptr<Tracer>& current_tracer() {
  static auto* tracer = new std::shared_ptr<Tracer>;
  return *tracer;
}

// Any failure, including an unopenable trace file, leaves tracing disabled
// rather than unresolved so it is never retried on every call.
void install_locked(const std::optional<TraceOptions>& options) {
  auto& tracer = current_tracer();
  tracer.reset();
  tracer_state.store(kDisabled, std::memory_order_release);
  if (options && options->enabled()) {
    tracer = std::make_shared<Tracer>(*options);
    tracer_state.store(kEnabled, std::memory_order_release);
  }
}

// Driver Managers unload and reload the driver library. Closing the sink at
// unload/exit releases the file for rotation and the next load.
// At ExitProcess other threads are already terminated; one may have held the
// mutex. Never block there: Disabled is always published, the sink is closed
// only when the mutex is free (the file is then released by process exit).
struct ShutdownGuard {
  ~ShutdownGuard() {
    tracer_state.store(kDisabled, std::memory_order_release);
    try {
      std::unique_lock lock(tracer_mutex(), std::try_to_lock);
      if (lock.owns_lock()) current_tracer().reset();
    } catch (...) {
    }
  }
} shutdown_guard;

// ---- Rendering -----------------------------------------------------------------

constexpr std::size_t kMaxTextUnits = 4096;

std::string address_text(const void* address) {
  if (address == nullptr) return "NULL";
  char buffer[2 + 2 * sizeof(std::uintptr_t) + 1];
  std::snprintf(buffer, sizeof(buffer), "0x%" PRIxPTR,
                reinterpret_cast<std::uintptr_t>(address));
  return buffer;
}

std::optional<std::int64_t> signed_of(const ArgValue& value) {
  switch (value.kind) {
    case ArgValue::Kind::Signed: return value.signed_value;
    case ArgValue::Kind::Unsigned:
      if (value.unsigned_value <= static_cast<std::uint64_t>(INT64_MAX)) {
        return static_cast<std::int64_t>(value.unsigned_value);
      }
      return std::nullopt;
    case ArgValue::Kind::Address: return std::nullopt;
  }
  return std::nullopt;
}

std::string integer_text(const ArgValue& value) {
  switch (value.kind) {
    case ArgValue::Kind::Signed: return std::to_string(value.signed_value);
    case ArgValue::Kind::Unsigned: return std::to_string(value.unsigned_value);
    case ArgValue::Kind::Address: return address_text(value.address);
  }
  return {};
}

std::string length_text(std::int64_t value) {
  switch (value) {
    case SQL_NULL_DATA: return "SQL_NULL_DATA";
    case SQL_DATA_AT_EXEC: return "SQL_DATA_AT_EXEC";
    case SQL_NTS: return "SQL_NTS";
    case SQL_NO_TOTAL: return "SQL_NO_TOTAL";
    case SQL_DEFAULT_PARAM: return "SQL_DEFAULT_PARAM";
    case SQL_IGNORE: return "SQL_IGNORE";
    default: return std::to_string(value);
  }
}

std::string handle_type_text(const ArgValue& value) {
  switch (signed_of(value).value_or(0)) {
    case SQL_HANDLE_ENV: return "SQL_HANDLE_ENV";
    case SQL_HANDLE_DBC: return "SQL_HANDLE_DBC";
    case SQL_HANDLE_STMT: return "SQL_HANDLE_STMT";
    case SQL_HANDLE_DESC: return "SQL_HANDLE_DESC";
    default: return integer_text(value);
  }
}

std::optional<std::int64_t> read_integer(const ArgValue& value) {
  if (value.address == nullptr) return std::nullopt;
  const auto* bytes = value.address;
  switch (value.pointee_size) {
    case 1: return value.pointee_signed
        ? std::int64_t{*static_cast<const std::int8_t*>(bytes)}
        : std::int64_t{*static_cast<const std::uint8_t*>(bytes)};
    case 2: {
      std::uint16_t raw; std::memcpy(&raw, bytes, sizeof(raw));
      return value.pointee_signed ? std::int64_t{static_cast<std::int16_t>(raw)}
                                  : std::int64_t{raw};
    }
    case 4: {
      std::uint32_t raw; std::memcpy(&raw, bytes, sizeof(raw));
      return value.pointee_signed ? std::int64_t{static_cast<std::int32_t>(raw)}
                                  : std::int64_t{raw};
    }
    case 8: {
      std::uint64_t raw; std::memcpy(&raw, bytes, sizeof(raw));
      if (!value.pointee_signed && raw > static_cast<std::uint64_t>(INT64_MAX)) {
        return std::nullopt;
      }
      return static_cast<std::int64_t>(raw);
    }
    default: return std::nullopt;
  }
}

// Reads at most `limit` units and never past the first NUL when the length is
// unknown. Narrow text is passed through; SQLWCHAR text is decoded to UTF-8.
std::optional<std::string> read_text(const ArgValue& value,
                                     std::optional<std::int64_t> units,
                                     bool stop_at_nul) {
  if (value.address == nullptr) return std::nullopt;
  std::size_t limit = kMaxTextUnits;
  if (units) {
    if (*units < 0) return std::nullopt;
    limit = (std::min)(limit, static_cast<std::size_t>(*units));
  }
  if (value.pointee_size == 1) {
    const auto* text = static_cast<const char*>(value.address);
    std::size_t length = 0;
    if (stop_at_nul) {
      while (length < limit && text[length] != '\0') ++length;
    } else {
      length = limit;
    }
    return std::string(text, length);
  }
  if (value.pointee_size != sizeof(SQLWCHAR)) return std::nullopt;
  const auto* text = static_cast<const SQLWCHAR*>(value.address);
  std::size_t length = 0;
  if (stop_at_nul) {
    while (length < limit && text[length] != 0) ++length;
  } else {
    length = limit;
  }
  // A bound that splits a UTF-16 surrogate pair keeps the decodable prefix.
  if constexpr (sizeof(SQLWCHAR) == 2) {
    if (length == limit && length > 0 &&
        text[length - 1] >= 0xD800 && text[length - 1] <= 0xDBFF) {
      --length;
    }
  }
  auto decoded = wide_to_utf8(std::span<const SQLWCHAR>(text, length));
  if (!decoded) return std::string("[invalid wide text]");
  return decoded;
}

std::optional<std::string> input_text(const ArgSpec& spec, const ArgValue& value,
                                      std::span<const ArgValue> args) {
  std::optional<std::int64_t> length;
  if (spec.related >= 0 && static_cast<std::size_t>(spec.related) < args.size()) {
    length = signed_of(args[static_cast<std::size_t>(spec.related)]);
  }
  if (!length || *length == SQL_NTS) return read_text(value, std::nullopt, true);
  return read_text(value, length, false);
}

std::optional<std::string> output_text(const ArgSpec& spec, const ArgValue& value,
                                       std::span<const ArgValue> args) {
  if (spec.role == R::OutSqlState) return read_text(value, 6, true);
  if (spec.related < 0 || static_cast<std::size_t>(spec.related) >= args.size()) {
    return std::nullopt;
  }
  const auto capacity = signed_of(args[static_cast<std::size_t>(spec.related)]);
  if (!capacity || *capacity <= 0) return std::nullopt;
  return read_text(value, capacity, true);
}

bool is_output(ArgRole role) noexcept {
  switch (role) {
    case R::OutPointer: case R::OutInteger: case R::OutLength:
    case R::OutText: case R::OutQueryText: case R::OutConnectionText:
    case R::OutSqlState:
      return true;
    default:
      return false;
  }
}

FieldSensitivity text_sensitivity(ArgRole role) noexcept {
  switch (role) {
    case R::QueryText: case R::OutQueryText: return FieldSensitivity::QueryText;
    case R::OutSqlState: return FieldSensitivity::Public;
    default: return FieldSensitivity::Sensitive;
  }
}

bool is_connection_text(ArgRole role) noexcept {
  return role == R::ConnectionText || role == R::OutConnectionText;
}

std::optional<LogField> render_input(const ArgSpec& spec, const ArgValue& value,
                                     std::span<const ArgValue> args) {
  const auto field = [&](std::string text,
                         FieldSensitivity sensitivity = FieldSensitivity::Public) {
    return LogField{spec.name, std::move(text), sensitivity};
  };
  if (is_output(spec.role)) return field(address_text(value.address));
  switch (spec.role) {
    case R::Secret: return std::nullopt;
    case R::Handle: case R::Pointer: return field(integer_text(value));
    case R::HandleType: return field(handle_type_text(value));
    case R::Integer: return field(integer_text(value));
    case R::Length: {
      const auto number = signed_of(value);
      return field(number ? length_text(*number) : integer_text(value));
    }
    case R::Text: case R::QueryText: case R::ConnectionText: {
      if (value.address == nullptr) return field("NULL");
      auto text = input_text(spec, value, args);
      if (!text) return field(address_text(value.address));
      if (is_connection_text(spec.role)) *text = mask_connection_string(*text);
      return field(std::move(*text), text_sensitivity(spec.role));
    }
    default: return field(address_text(value.address));
  }
}

std::optional<LogField> render_output(const ArgSpec& spec, const ArgValue& value,
                                      std::span<const ArgValue> args,
                                      SQLRETURN result) {
  if (!is_output(spec.role) || value.address == nullptr) return std::nullopt;
  const bool written = result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO;
  // SQLParamData publishes its token together with SQL_NEED_DATA.
  if (!written && !(result == SQL_NEED_DATA && spec.role == R::OutPointer)) {
    return std::nullopt;
  }
  const auto field = [&](std::string text,
                         FieldSensitivity sensitivity = FieldSensitivity::Public) {
    return LogField{spec.name, std::move(text), sensitivity};
  };
  switch (spec.role) {
    case R::OutPointer: {
      if (!value.pointee_address) return std::nullopt;
      const void* written_address = nullptr;
      std::memcpy(&written_address, value.address, sizeof(written_address));
      return field(address_text(written_address));
    }
    case R::OutInteger: {
      const auto number = read_integer(value);
      return number ? std::optional<LogField>(field(std::to_string(*number)))
                    : std::nullopt;
    }
    case R::OutLength: {
      const auto number = read_integer(value);
      return number ? std::optional<LogField>(field(length_text(*number)))
                    : std::nullopt;
    }
    default: {
      auto text = output_text(spec, value, args);
      if (!text) return std::nullopt;
      if (is_connection_text(spec.role)) *text = mask_connection_string(*text);
      return field(std::move(*text), text_sensitivity(spec.role));
    }
  }
}

std::string thread_text() {
#ifdef _WIN32
  return std::to_string(GetCurrentThreadId());
#else
  return std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
#endif
}

std::optional<std::string> diagnostic_sqlstate(SQLHANDLE handle) {
  if (handle == SQL_NULL_HANDLE) return std::nullopt;
  const auto retained = HandleRegistry::instance().get_handle(handle);
  if (!retained) return std::nullopt;
  const auto record = retained->get_diagnostic_record(1);
  if (!record) return std::nullopt;
  return record->sqlstate;
}

bool password_key(std::string_view key) {
  const auto normalized = upper(ConnectionString::trim(std::string(key)));
  return normalized == "PWD" ||
      normalized.find("PASSWORD") != std::string::npos ||
      normalized.find("SECRET") != std::string::npos ||
      normalized.find("TOKEN") != std::string::npos;
}

}  // namespace

std::span<const ApiSpec> api_specs() noexcept { return kApis; }

const ApiSpec* find_api_spec(std::string_view name) noexcept {
  for (const auto& spec : kApis) {
    if (spec.name == name) return &spec;
  }
  return nullptr;
}

TraceOptions TraceOptions::resolve(
    const std::map<std::string, std::string>& driver_parameters,
    const EnvironmentLookup& environment) {
  // Last writer wins: driver section first, then the process environment.
  std::map<std::string, std::string> trace;
  for (const auto& [key, value] : driver_parameters) {
    const auto normalized = upper(key);
    for (const auto& known : kTraceKeys) {
      if (normalized == known.driver_key) trace[std::string(known.driver_key)] = value;
    }
  }
  if (environment) {
    for (const auto& known : kTraceKeys) {
      if (auto value = environment(known.environment)) {
        trace[std::string(known.driver_key)] = std::move(*value);
      }
    }
  }

  const auto find = [&](std::string_view key) -> const std::string* {
    const auto entry = trace.find(std::string(key));
    return entry == trace.end() ? nullptr : &entry->second;
  };
  const auto* flag = find("ODBCTRACE");
  const bool enabled = flag && rs::core::logging::parse_flag("ODBCTrace", *flag);
  const auto* sensitive = find("ODBCTRACESENSITIVE");

  // Adapt trace keys to the SDK logger vocabulary so its validation, sink
  // selection and bounds apply unchanged. Trace output is synchronous so a
  // crash cannot drop records the application already returned from.
  std::map<std::string, std::string> logging{
      {"LogLevel", enabled ? "Trace" : "Off"}, {"LogAsync", "false"}};
  const std::pair<std::string_view, std::string_view> translated[]{
      {"ODBCTRACEFILE", "LogFile"}, {"ODBCTRACEFORMAT", "LogFormat"},
      {"ODBCTRACEQUERIES", "LogQueries"}, {"ODBCTRACEMAXSIZE", "LogMaxSize"},
      {"ODBCTRACEMAXFILES", "LogMaxFiles"}};
  for (const auto& [from, to] : translated) {
    if (const auto* value = find(from)) logging[std::string(to)] = *value;
  }

  TraceOptions options;
  options.logging = rs::core::logging::LoggingOptions::resolve({}, {}, logging);
  options.logging.log_sensitive = enabled && sensitive &&
      rs::core::logging::parse_flag("ODBCTraceSensitive", *sensitive);
  return options;
}

std::shared_ptr<Tracer> Tracer::active() noexcept {
  if (tracer_state.load(std::memory_order_acquire) == kDisabled) return {};
  try {
    std::lock_guard lock(tracer_mutex());
    if (tracer_state.load(std::memory_order_relaxed) == kUnresolved) {
      std::optional<TraceOptions> options;
      try {
        options = TraceOptions::resolve(trusted_driver_parameters(),
                                        process_environment);
      } catch (...) {
        // Invalid trace configuration disables tracing; it never fails a call.
      }
      install_locked(options);
    }
    return current_tracer();
  } catch (...) {
    // install_locked already recorded Disabled for an unusable sink.
    return {};
  }
}

void Tracer::configure(std::optional<TraceOptions> options) {
  std::lock_guard lock(tracer_mutex());
  if (!options) {
    current_tracer().reset();
    tracer_state.store(kUnresolved, std::memory_order_release);
    return;
  }
  install_locked(options);
}

Tracer::Tracer(TraceOptions options)
    : options_(std::move(options)),
      logger_(rs::core::logging::DriverLogger::create(options_.logging, 0)) {}

void Tracer::enter(const ApiSpec& spec, std::uint64_t call,
                   std::span<const ArgValue> args) const noexcept {
  try {
    std::vector<LogField> fields;
    fields.reserve(args.size() + 2);
    fields.push_back({"call", std::to_string(call), FieldSensitivity::Public});
    fields.push_back({"thread", thread_text(), FieldSensitivity::Public});
    for (std::size_t index = 0; index < args.size(); ++index) {
      if (auto field = render_input(spec.args[index], args[index], args)) {
        fields.push_back(std::move(*field));
      }
    }
    logger_->log(LogLevel::Trace, "odbc_trace_enter", spec.name, fields);
  } catch (...) {
    // Tracing must never change ODBC behavior.
  }
}

void Tracer::exit(const ApiSpec& spec, std::uint64_t call,
                  std::span<const ArgValue> args, SQLRETURN result,
                  std::chrono::steady_clock::duration elapsed,
                  SQLHANDLE diagnostic_handle) const noexcept {
  try {
    std::vector<LogField> fields;
    fields.reserve(args.size() + 6);
    fields.push_back({"call", std::to_string(call), FieldSensitivity::Public});
    fields.push_back({"thread", thread_text(), FieldSensitivity::Public});
    fields.push_back({"return", std::string(return_code_name(result)),
                      FieldSensitivity::Public});
    fields.push_back({"return_code", std::to_string(result),
                      FieldSensitivity::Public});
    fields.push_back({"duration_us", std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()),
        FieldSensitivity::Public});
    if (result == SQL_ERROR || result == SQL_SUCCESS_WITH_INFO) {
      if (auto sqlstate = diagnostic_sqlstate(diagnostic_handle)) {
        fields.push_back({"sqlstate", std::move(*sqlstate), FieldSensitivity::Public});
      }
    }
    for (std::size_t index = 0; index < args.size(); ++index) {
      if (auto field = render_output(spec.args[index], args[index], args, result)) {
        fields.push_back(std::move(*field));
      }
    }
    logger_->log(LogLevel::Trace, "odbc_trace_exit", spec.name, fields);
    logger_->flush();
  } catch (...) {
    // Tracing must never change ODBC behavior.
  }
}

std::string_view return_code_name(SQLRETURN result) noexcept {
  switch (result) {
    case SQL_SUCCESS: return "SQL_SUCCESS";
    case SQL_SUCCESS_WITH_INFO: return "SQL_SUCCESS_WITH_INFO";
    case SQL_NO_DATA: return "SQL_NO_DATA";
    case SQL_ERROR: return "SQL_ERROR";
    case SQL_INVALID_HANDLE: return "SQL_INVALID_HANDLE";
    case SQL_STILL_EXECUTING: return "SQL_STILL_EXECUTING";
    case SQL_NEED_DATA: return "SQL_NEED_DATA";
    case 101: return "SQL_PARAM_DATA_AVAILABLE";
    default: return "SQL_UNKNOWN_RETURN";
  }
}

std::string mask_connection_string(std::string_view connection_string) {
  std::string result;
  result.reserve(connection_string.size());
  std::size_t position = 0;
  while (position < connection_string.size()) {
    const auto equals = connection_string.find('=', position);
    const auto semicolon = connection_string.find(';', position);
    if (equals == std::string_view::npos ||
        (semicolon != std::string_view::npos && semicolon < equals)) {
      const auto end = semicolon == std::string_view::npos
          ? connection_string.size() : semicolon + 1;
      result.append(connection_string.substr(position, end - position));
      position = end;
      continue;
    }
    const auto key = connection_string.substr(position, equals - position);
    // A braced value may contain ';' and escapes '}' as '}}'.
    auto value_end = equals + 1;
    while (value_end < connection_string.size() &&
           std::isspace(static_cast<unsigned char>(connection_string[value_end]))) {
      ++value_end;
    }
    bool terminated = true;
    if (value_end < connection_string.size() && connection_string[value_end] == '{') {
      ++value_end;
      terminated = false;
      while (value_end < connection_string.size()) {
        if (connection_string[value_end] == '}') {
          if (value_end + 1 < connection_string.size() &&
              connection_string[value_end + 1] == '}') {
            value_end += 2;
            continue;
          }
          ++value_end;
          terminated = true;
          break;
        }
        ++value_end;
      }
    }
    result.append(key);
    result.push_back('=');
    if (!terminated) {
      // The real parser rejects this; the remainder may hide other keys.
      result.append("***");
      break;
    }
    value_end = (std::min)(connection_string.find(';', value_end),
                           connection_string.size());
    if (password_key(key)) {
      result.append("***");
    } else {
      result.append(connection_string.substr(equals + 1, value_end - equals - 1));
    }
    if (value_end < connection_string.size()) result.push_back(';');
    position = value_end + 1;
  }
  return result;
}

}  // namespace rs::odbc::trace
