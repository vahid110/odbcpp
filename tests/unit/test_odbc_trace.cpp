#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/odbc_trace.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

using rs::odbc::trace::TraceOptions;
using rs::odbc::trace::Tracer;
using rs::odbc::trace::traced_call;

template <typename Result, typename... Args>
constexpr std::size_t arity(Result (SQL_API*)(Args...)) {
  return sizeof...(Args);
}

#define ODBCPP_EXPORT_ARITY(function) {#function, arity(&function)}

// Compile-time signatures of every exported function in odbc/odbcpp.exports.
const std::map<std::string, std::size_t>& exported_arities() {
  static const std::map<std::string, std::size_t> arities{
      ODBCPP_EXPORT_ARITY(SQLAllocHandle), ODBCPP_EXPORT_ARITY(SQLBindCol),
      ODBCPP_EXPORT_ARITY(SQLBindParameter), ODBCPP_EXPORT_ARITY(SQLColAttribute),
      ODBCPP_EXPORT_ARITY(SQLColAttributeW), ODBCPP_EXPORT_ARITY(SQLCloseCursor),
      ODBCPP_EXPORT_ARITY(SQLColumns), ODBCPP_EXPORT_ARITY(SQLColumnsW),
      ODBCPP_EXPORT_ARITY(SQLConnect), ODBCPP_EXPORT_ARITY(SQLConnectW),
      ODBCPP_EXPORT_ARITY(SQLCopyDesc), ODBCPP_EXPORT_ARITY(SQLDescribeCol),
      ODBCPP_EXPORT_ARITY(SQLDescribeColW), ODBCPP_EXPORT_ARITY(SQLDescribeParam),
      ODBCPP_EXPORT_ARITY(SQLDisconnect), ODBCPP_EXPORT_ARITY(SQLDriverConnect),
      ODBCPP_EXPORT_ARITY(SQLDriverConnectW), ODBCPP_EXPORT_ARITY(SQLEndTran),
      ODBCPP_EXPORT_ARITY(SQLError), ODBCPP_EXPORT_ARITY(SQLErrorW),
      ODBCPP_EXPORT_ARITY(SQLExecDirect), ODBCPP_EXPORT_ARITY(SQLExecDirectW),
      ODBCPP_EXPORT_ARITY(SQLExecute), ODBCPP_EXPORT_ARITY(SQLFetch),
      ODBCPP_EXPORT_ARITY(SQLFetchScroll), ODBCPP_EXPORT_ARITY(SQLForeignKeys),
      ODBCPP_EXPORT_ARITY(SQLForeignKeysW), ODBCPP_EXPORT_ARITY(SQLFreeHandle),
      ODBCPP_EXPORT_ARITY(SQLFreeStmt), ODBCPP_EXPORT_ARITY(SQLGetConnectAttr),
      ODBCPP_EXPORT_ARITY(SQLGetConnectAttrW), ODBCPP_EXPORT_ARITY(SQLGetData),
      ODBCPP_EXPORT_ARITY(SQLGetDescField), ODBCPP_EXPORT_ARITY(SQLGetDescFieldW),
      ODBCPP_EXPORT_ARITY(SQLGetDescRec), ODBCPP_EXPORT_ARITY(SQLGetDescRecW),
      ODBCPP_EXPORT_ARITY(SQLGetDiagField), ODBCPP_EXPORT_ARITY(SQLGetDiagFieldW),
      ODBCPP_EXPORT_ARITY(SQLGetDiagRec), ODBCPP_EXPORT_ARITY(SQLGetDiagRecW),
      ODBCPP_EXPORT_ARITY(SQLGetEnvAttr), ODBCPP_EXPORT_ARITY(SQLGetFunctions),
      ODBCPP_EXPORT_ARITY(SQLGetInfo), ODBCPP_EXPORT_ARITY(SQLGetInfoW),
      ODBCPP_EXPORT_ARITY(SQLGetStmtAttr), ODBCPP_EXPORT_ARITY(SQLGetStmtAttrW),
      ODBCPP_EXPORT_ARITY(SQLGetTypeInfo), ODBCPP_EXPORT_ARITY(SQLGetTypeInfoW),
      ODBCPP_EXPORT_ARITY(SQLMoreResults), ODBCPP_EXPORT_ARITY(SQLNativeSql),
      ODBCPP_EXPORT_ARITY(SQLNativeSqlW), ODBCPP_EXPORT_ARITY(SQLNumParams),
      ODBCPP_EXPORT_ARITY(SQLNumResultCols), ODBCPP_EXPORT_ARITY(SQLPrepare),
      ODBCPP_EXPORT_ARITY(SQLPrepareW), ODBCPP_EXPORT_ARITY(SQLPrimaryKeys),
      ODBCPP_EXPORT_ARITY(SQLPrimaryKeysW), ODBCPP_EXPORT_ARITY(SQLProcedureColumns),
      ODBCPP_EXPORT_ARITY(SQLProcedureColumnsW), ODBCPP_EXPORT_ARITY(SQLProcedures),
      ODBCPP_EXPORT_ARITY(SQLProceduresW), ODBCPP_EXPORT_ARITY(SQLRowCount),
      ODBCPP_EXPORT_ARITY(SQLSetConnectAttr), ODBCPP_EXPORT_ARITY(SQLSetConnectAttrW),
      ODBCPP_EXPORT_ARITY(SQLSetDescField), ODBCPP_EXPORT_ARITY(SQLSetDescFieldW),
      ODBCPP_EXPORT_ARITY(SQLSetDescRec), ODBCPP_EXPORT_ARITY(SQLSetEnvAttr),
      ODBCPP_EXPORT_ARITY(SQLSetPos), ODBCPP_EXPORT_ARITY(SQLSetStmtAttr),
      ODBCPP_EXPORT_ARITY(SQLSetStmtAttrW), ODBCPP_EXPORT_ARITY(SQLSpecialColumns),
      ODBCPP_EXPORT_ARITY(SQLSpecialColumnsW), ODBCPP_EXPORT_ARITY(SQLStatistics),
      ODBCPP_EXPORT_ARITY(SQLStatisticsW), ODBCPP_EXPORT_ARITY(SQLTables),
      ODBCPP_EXPORT_ARITY(SQLTablesW), ODBCPP_EXPORT_ARITY(SQLCancel),
      ODBCPP_EXPORT_ARITY(SQLParamData), ODBCPP_EXPORT_ARITY(SQLPutData),
      ODBCPP_EXPORT_ARITY(SQLBulkOperations), ODBCPP_EXPORT_ARITY(SQLCancelHandle),
  };
  return arities;
}

#undef ODBCPP_EXPORT_ARITY

std::filesystem::path temporary_directory(std::string_view name) {
  static std::atomic<unsigned int> sequence{0};
#ifdef _WIN32
  const auto process_id = _getpid();
#else
  const auto process_id = getpid();
#endif
  auto directory = std::filesystem::temp_directory_path() /
      (std::string(name) + "-" + std::to_string(process_id) + "-" +
       std::to_string(sequence.fetch_add(1)));
  std::filesystem::create_directories(directory);
  return directory;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::vector<std::string> lines_with(const std::string& contents,
                                    std::string_view needle) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < contents.size()) {
    auto end = contents.find('\n', start);
    if (end == std::string::npos) end = contents.size();
    const auto line = contents.substr(start, end - start);
    if (line.find(needle) != std::string::npos) result.push_back(line);
    start = end + 1;
  }
  return result;
}

bool contains(const std::string& haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

TraceOptions trace_to(const std::filesystem::path& path,
                      std::map<std::string, std::string> extra = {}) {
  extra["ODBCTrace"] = "1";
  extra["ODBCTraceFile"] = path.string();
  return TraceOptions::resolve(extra, nullptr);
}

class OdbcTraceTest : public ::testing::Test {
protected:
  void SetUp() override {
    directory_ = temporary_directory("odbcpp-trace");
    path_ = directory_ / "trace.log";
  }

  void TearDown() override {
    Tracer::configure(TraceOptions{});
    std::error_code ignored;
    std::filesystem::remove_all(directory_, ignored);
  }

  // Closes the sink so the file is complete and removable on every platform.
  std::string finish() {
    Tracer::configure(TraceOptions{});
    return read_file(path_);
  }

  std::filesystem::path directory_;
  std::filesystem::path path_;
};

TEST(OdbcTraceSignatureTest, CoversEveryExportedFunctionWithMatchingArity) {
  std::ifstream exports(ODBCPP_EXPORTS_FILE);
  ASSERT_TRUE(exports.is_open());
  std::set<std::string> exported;
  for (std::string line; std::getline(exports, line);) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (!line.empty()) {
      exported.insert(line);
    }
  }
  ASSERT_FALSE(exported.empty());

  std::set<std::string> traced;
  for (const auto& spec : rs::odbc::trace::api_specs()) {
    EXPECT_TRUE(traced.insert(std::string(spec.name)).second) << spec.name;
  }
  EXPECT_EQ(exported, traced);

  const auto& arities = exported_arities();
  EXPECT_EQ(exported.size(), arities.size());
  for (const auto& name : exported) {
    const auto* spec = rs::odbc::trace::find_api_spec(name);
    ASSERT_NE(nullptr, spec) << name;
    const auto arity = arities.find(name);
    ASSERT_NE(arities.end(), arity) << name;
    EXPECT_EQ(arity->second, spec->args.size()) << name;
  }
}

TEST(OdbcTraceSignatureTest, RelatedLengthAndCapacityIndexesAreInRange) {
  using rs::odbc::trace::ArgRole;
  for (const auto& api : rs::odbc::trace::api_specs()) {
    for (const auto& arg : api.args) {
      const bool needs_related = arg.role == ArgRole::Text ||
          arg.role == ArgRole::QueryText || arg.role == ArgRole::ConnectionText ||
          arg.role == ArgRole::OutText || arg.role == ArgRole::OutQueryText ||
          arg.role == ArgRole::OutConnectionText;
      if (needs_related) {
        EXPECT_GE(arg.related, 0) << api.name << "." << arg.name;
        EXPECT_LT(static_cast<std::size_t>(arg.related), api.args.size())
            << api.name << "." << arg.name;
      }
    }
  }
}

TEST(OdbcTraceOptionsTest, DisabledByDefaultAndWithoutTrustedSources) {
  const auto options = TraceOptions::resolve({}, nullptr);
  EXPECT_FALSE(options.enabled());
  EXPECT_FALSE(options.logging.log_sensitive);
}

TEST(OdbcTraceOptionsTest, EnvironmentOverridesDriverSection) {
  const std::map<std::string, std::string> driver{
      {"ODBCTrace", "0"}, {"odbctracefile", "driver.log"},
      {"ODBCTraceFormat", "Text"}, {"ODBCTraceMaxFiles", "3"}};
  const std::map<std::string, std::string> environment{
      {"ODBCPP_TRACE", "yes"}, {"ODBCPP_TRACE_FILE", "environment.log"},
      {"ODBCPP_TRACE_FORMAT", "Json"}, {"ODBCPP_TRACE_QUERIES", "1"},
      {"ODBCPP_TRACE_SENSITIVE", "true"}};
  const auto options = TraceOptions::resolve(
      driver, [&](std::string_view name) -> std::optional<std::string> {
        const auto entry = environment.find(std::string(name));
        if (entry == environment.end()) return std::nullopt;
        return entry->second;
      });
  ASSERT_TRUE(options.enabled());
  EXPECT_EQ(rs::core::logging::LogLevel::Trace, options.logging.level);
  EXPECT_EQ("environment.log", options.logging.file);
  EXPECT_EQ(rs::core::logging::LogFormat::Json, options.logging.format);
  EXPECT_EQ(3u, options.logging.max_files);
  EXPECT_TRUE(options.logging.log_queries);
  EXPECT_TRUE(options.logging.log_sensitive);
  EXPECT_FALSE(options.logging.asynchronous);
}

TEST(OdbcTraceOptionsTest, SensitiveOptInRequiresTraceAndInvalidValuesAreRejected) {
  EXPECT_FALSE(TraceOptions::resolve({{"ODBCTraceSensitive", "1"}}, nullptr)
                   .logging.log_sensitive);
  EXPECT_THROW(TraceOptions::resolve({{"ODBCTrace", "maybe"}}, nullptr),
               std::invalid_argument);
  EXPECT_THROW(TraceOptions::resolve(
                   {{"ODBCTrace", "1"}, {"ODBCTraceMaxSize", "0"}}, nullptr),
               std::invalid_argument);
  // Generic logging keys belong to the per-connection logger, not the trace.
  EXPECT_FALSE(TraceOptions::resolve({{"LogLevel", "Trace"}}, nullptr).enabled());
}

TEST(OdbcTraceMaskTest, MasksPasswordLikeValuesIncludingBracedValues) {
  using rs::odbc::trace::mask_connection_string;
  EXPECT_EQ("SERVER=h;UID=u;PWD=***;PORT=5439",
            mask_connection_string("SERVER=h;UID=u;PWD=top-secret;PORT=5439"));
  EXPECT_EQ("PWD=***;DATABASE=dev",
            mask_connection_string("PWD={a;b}}c};DATABASE=dev"));
  EXPECT_EQ("Password=***", mask_connection_string("Password=secret"));
  EXPECT_EQ("DSN=x;client_secret=***;",
            mask_connection_string("DSN=x;client_secret=s1;"));
  EXPECT_EQ("DSN=redshift", mask_connection_string("DSN=redshift"));
  EXPECT_EQ("DSN=***", mask_connection_string("DSN={x;PWD=secret"));
  EXPECT_EQ("UID=u;DSN=***", mask_connection_string("UID=u;DSN={x}};PWD=secret"));
  EXPECT_EQ("PWD=***;UID=u", mask_connection_string("PWD={a}tail;UID=u"));
}

TEST(OdbcTraceLifecycleTest, ResolvesTrustedEnvironmentLazilyAndDisablesUnusableSinks) {
  const auto directory = temporary_directory("odbcpp-trace-lazy");
  const auto path = directory / "trace.log";
  const auto set_environment = [](const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
      unsetenv(name);
    } else {
      setenv(name, value.c_str(), 1);
    }
#endif
  };
  set_environment("ODBCPP_TRACE", "1");
  set_environment("ODBCPP_TRACE_FILE", path.string());
  Tracer::configure(std::nullopt);
  EXPECT_NE(nullptr, Tracer::active());

  // An unopenable path disables tracing instead of failing calls or retrying.
  const auto blocker = directory / "blocker";
  std::ofstream(blocker) << "file, not a directory";
  set_environment("ODBCPP_TRACE_FILE", (blocker / "x.log").string());
  Tracer::configure(std::nullopt);
  EXPECT_EQ(nullptr, Tracer::active());
  EXPECT_EQ(nullptr, Tracer::active());
  set_environment("ODBCPP_TRACE", "");
  set_environment("ODBCPP_TRACE_FILE", "");
  Tracer::configure(TraceOptions{});
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

TEST_F(OdbcTraceTest, TracesPreConnectionCallsWithOutputsAndReturnCodes) {
  Tracer::configure(trace_to(path_));
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  SQLUINTEGER value = 0;
  EXPECT_EQ(SQL_ERROR, SQLGetConnectAttr(
      connection, -12345, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
  const auto contents = finish();

  EXPECT_EQ(6u, lines_with(contents, "event=odbc_trace_enter").size());
  EXPECT_EQ(6u, lines_with(contents, "event=odbc_trace_exit").size());
  const auto allocations = lines_with(contents, "message=\"SQLAllocHandle\"");
  ASSERT_EQ(4u, allocations.size());
  EXPECT_TRUE(contains(allocations[0], "HandleType=\"SQL_HANDLE_ENV\""));
  EXPECT_TRUE(contains(allocations[0], "InputHandle=\"NULL\""));
  EXPECT_TRUE(contains(allocations[1], "return=\"SQL_SUCCESS\""));
  EXPECT_TRUE(contains(allocations[1], "OutputHandlePtr=\"0x"));
  EXPECT_TRUE(contains(allocations[2], "HandleType=\"SQL_HANDLE_DBC\""));

  const auto failure = lines_with(contents, "message=\"SQLGetConnectAttr\"");
  ASSERT_EQ(2u, failure.size());
  EXPECT_TRUE(contains(failure[0], "Attribute=\"-12345\""));
  EXPECT_TRUE(contains(failure[1], "return=\"SQL_ERROR\""));
  EXPECT_TRUE(contains(failure[1], "return_code=\"-1\""));
  EXPECT_TRUE(contains(failure[1], "sqlstate=\"HY092\""));
  EXPECT_TRUE(contains(failure[1], "duration_us=\""));
  EXPECT_FALSE(contains(failure[1], "StringLengthPtr="));
}

TEST_F(OdbcTraceTest, ConnectionSecretsNeverReachTheTraceEvenWithSensitiveOptIn) {
  Tracer::configure(trace_to(path_, {{"ODBCTraceSensitive", "1"}}));
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  ASSERT_EQ(SQL_SUCCESS,
            SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                          reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  ASSERT_EQ(SQL_SUCCESS,
            SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  const std::string connection_string =
      "SERVER=127.0.0.1;PORT=1;UID=trace-user;PWD={top;secret};SSL=0;"
      "TransportMode=Sync";
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(
      connection, nullptr,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(connection_string.c_str())),
      SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  SQLCHAR server[] = "nowhere-dsn";
  SQLCHAR user[] = "trace-user";
  SQLCHAR password[] = "another-secret";
  EXPECT_EQ(SQL_ERROR, SQLConnect(connection, server, SQL_NTS, user, SQL_NTS,
                                  password, SQL_NTS));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
  const auto contents = finish();

  EXPECT_FALSE(contains(contents, "top;secret"));
  EXPECT_FALSE(contains(contents, "secret}"));
  EXPECT_FALSE(contains(contents, "another-secret"));
  EXPECT_FALSE(contains(contents, "Authentication"));
  EXPECT_FALSE(contains(contents, "NameLength3"));
  const auto driver_connect = lines_with(contents, "message=\"SQLDriverConnect\"");
  ASSERT_EQ(2u, driver_connect.size());
  EXPECT_TRUE(contains(driver_connect[0], "PWD=***"));
  EXPECT_TRUE(contains(driver_connect[0], "UID=trace-user"));
  // The total length would reveal the password length.
  EXPECT_FALSE(contains(contents, "StringLength1"));
  EXPECT_FALSE(contains(contents, "StringLength2Ptr"));
  EXPECT_TRUE(contains(driver_connect[1], "return=\"SQL_ERROR\""));
  EXPECT_TRUE(contains(driver_connect[1], "sqlstate=\""));
  const auto connect = lines_with(contents, "message=\"SQLConnect\"");
  ASSERT_EQ(2u, connect.size());
  EXPECT_TRUE(contains(connect[0], "ServerName=\"nowhere-dsn\""));
}

TEST_F(OdbcTraceTest, IdentifiersAreRedactedAndQueryTextNeedsItsOwnOptIn) {
  Tracer::configure(trace_to(path_));
  const auto statement = reinterpret_cast<SQLHSTMT>(std::uintptr_t{0x1234});
  SQLCHAR sql[] = "SELECT private_value";
  SQLCHAR schema[] = "private_schema";
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLExecDirect", SQL_NULL_HANDLE, true,
      []() noexcept -> SQLRETURN { return SQL_SUCCESS; },
      statement, &sql[0], SQLINTEGER{SQL_NTS}));
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLTables", SQL_NULL_HANDLE, true,
      []() noexcept -> SQLRETURN { return SQL_SUCCESS; },
      statement, static_cast<SQLCHAR*>(nullptr), SQLSMALLINT{0}, &schema[0],
      SQLSMALLINT{7}, static_cast<SQLCHAR*>(nullptr), SQLSMALLINT{0},
      static_cast<SQLCHAR*>(nullptr), SQLSMALLINT{0}));
  auto contents = finish();
  EXPECT_FALSE(contains(contents, "private_value"));
  EXPECT_FALSE(contains(contents, "StatementText="));
  EXPECT_FALSE(contains(contents, "private"));
  EXPECT_TRUE(contains(contents, "StatementHandle=\"0x1234\""));
  EXPECT_TRUE(contains(contents, "SchemaName=\"[redacted]\""));
  EXPECT_TRUE(contains(contents, "CatalogName=\"NULL\""));

  Tracer::configure(trace_to(path_, {{"ODBCTraceQueries", "1"},
                                     {"ODBCTraceSensitive", "1"}}));
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLExecDirect", SQL_NULL_HANDLE, true,
      []() noexcept -> SQLRETURN { return SQL_SUCCESS; },
      statement, &sql[0], SQLINTEGER{SQL_NTS}));
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLTables", SQL_NULL_HANDLE, true,
      []() noexcept -> SQLRETURN { return SQL_SUCCESS; },
      statement, static_cast<SQLCHAR*>(nullptr), SQLSMALLINT{0}, &schema[0],
      SQLSMALLINT{7}, static_cast<SQLCHAR*>(nullptr), SQLSMALLINT{0},
      static_cast<SQLCHAR*>(nullptr), SQLSMALLINT{0}));
  contents = finish();
  EXPECT_TRUE(contains(contents, "StatementText=\"SELECT private_value\""));
  // The explicit length bounds the read; the remainder of the buffer is unread.
  EXPECT_TRUE(contains(contents, "SchemaName=\"private\""));
  EXPECT_TRUE(contains(contents, "NameLength2=\"7\""));
}

TEST_F(OdbcTraceTest, OutputsAreDecodedOnlyWhenTheDriverWroteThem) {
  Tracer::configure(trace_to(path_, {{"ODBCTraceSensitive", "1"}}));
  const auto statement = reinterpret_cast<SQLHSTMT>(std::uintptr_t{0xabc});
  SQLCHAR name[16]{};
  SQLSMALLINT name_length = 0, data_type = 0, digits = 0, nullable = 0;
  SQLULEN column_size = 0;
  const auto describe = [&]() noexcept -> SQLRETURN {
    std::memcpy(name, "region", 7);
    name_length = 6; data_type = SQL_VARCHAR; column_size = 256;
    digits = 0; nullable = SQL_NULLABLE;
    return SQL_SUCCESS;
  };
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLDescribeCol", SQL_NULL_HANDLE, true, describe, statement,
      SQLUSMALLINT{1}, &name[0], SQLSMALLINT{16}, &name_length, &data_type,
      &column_size, &digits, &nullable));

  SQLLEN indicator = 0;
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLGetData", SQL_NULL_HANDLE, true,
      [&]() noexcept -> SQLRETURN { indicator = SQL_NULL_DATA; return SQL_SUCCESS; },
      statement, SQLUSMALLINT{1}, SQLSMALLINT{SQL_C_CHAR},
      static_cast<SQLPOINTER>(nullptr), SQLLEN{0}, &indicator));

  SQLWCHAR state[6]{};
  SQLWCHAR message[32]{};
  SQLINTEGER native = 0;
  SQLSMALLINT message_length = 0;
  const auto diagnostic = [&]() noexcept -> SQLRETURN {
    const char* text_state = "42P01";
    for (int index = 0; index < 5; ++index) state[index] = static_cast<SQLWCHAR>(text_state[index]);
    const char* text = "no relation";
    for (int index = 0; text[index] != '\0'; ++index) message[index] = static_cast<SQLWCHAR>(text[index]);
    native = 7; message_length = 11;
    return SQL_SUCCESS;
  };
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLGetDiagRecW", SQL_NULL_HANDLE, true, diagnostic,
      SQLSMALLINT{SQL_HANDLE_STMT}, static_cast<SQLHANDLE>(statement),
      SQLSMALLINT{1}, &state[0], &native, &message[0], SQLSMALLINT{32},
      &message_length));

  SQLPOINTER token = nullptr;
  EXPECT_EQ(SQL_NEED_DATA, traced_call(
      "SQLParamData", SQL_NULL_HANDLE, true,
      [&]() noexcept -> SQLRETURN {
        token = reinterpret_cast<SQLPOINTER>(std::uintptr_t{0x77});
        return SQL_NEED_DATA;
      },
      statement, &token));

  SQLSMALLINT columns = 99;
  EXPECT_EQ(SQL_ERROR, traced_call(
      "SQLNumResultCols", SQL_NULL_HANDLE, true,
      []() noexcept -> SQLRETURN { return SQL_ERROR; }, statement, &columns));
  const auto contents = finish();

  const auto described = lines_with(contents, "event=odbc_trace_exit");
  ASSERT_EQ(5u, described.size());
  EXPECT_TRUE(contains(described[0], "ColumnName=\"region\""));
  EXPECT_TRUE(contains(described[0], "NameLengthPtr=\"6\""));
  EXPECT_TRUE(contains(described[0], "DataTypePtr=\"12\""));
  EXPECT_TRUE(contains(described[0], "ColumnSizePtr=\"256\""));
  EXPECT_TRUE(contains(described[0], "NullablePtr=\"1\""));
  EXPECT_TRUE(contains(described[1], "StrLen_or_IndPtr=\"SQL_NULL_DATA\""));
  EXPECT_TRUE(contains(described[2], "SQLState=\"42P01\""));
  EXPECT_TRUE(contains(described[2], "MessageText=\"no relation\""));
  EXPECT_TRUE(contains(described[2], "NativeErrorPtr=\"7\""));
  EXPECT_TRUE(contains(described[3], "return=\"SQL_NEED_DATA\""));
  EXPECT_TRUE(contains(described[3], "ValuePtrPtr=\"0x77\""));
  EXPECT_TRUE(contains(described[4], "return=\"SQL_ERROR\""));
  EXPECT_FALSE(contains(described[4], "ColumnCountPtr"));
}

TEST_F(OdbcTraceTest, WideTextHonorsExplicitLengthsAndReadBounds) {
  Tracer::configure(trace_to(path_, {{"ODBCTraceSensitive", "1"}}));
  const auto statement = reinterpret_cast<SQLHSTMT>(std::uintptr_t{0x3});
  std::vector<SQLWCHAR> schema{'s', 'a', 'l', 'e', 's', 'X', 'Y', 0};
  // 4095 ASCII units, then (UTF-16) a surrogate pair straddling the 4096-unit bound.
  std::vector<SQLWCHAR> table(4095, static_cast<SQLWCHAR>('t'));
  if constexpr (sizeof(SQLWCHAR) == 2) {
    table.push_back(static_cast<SQLWCHAR>(0xD83D));
    table.push_back(static_cast<SQLWCHAR>(0xDE00));
  } else {
    table.push_back(static_cast<SQLWCHAR>(0x1F600));
  }
  table.push_back(0);
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLTablesW", SQL_NULL_HANDLE, true,
      []() noexcept -> SQLRETURN { return SQL_SUCCESS; },
      statement, static_cast<SQLWCHAR*>(nullptr), SQLSMALLINT{0},
      schema.data(), SQLSMALLINT{5}, table.data(), SQLSMALLINT{SQL_NTS},
      static_cast<SQLWCHAR*>(nullptr), SQLSMALLINT{0}));
  const auto contents = finish();
  EXPECT_TRUE(contains(contents, "SchemaName=\"sales\""));
  EXPECT_FALSE(contains(contents, "salesXY"));
  EXPECT_FALSE(contains(contents, "[invalid wide text]"));
  EXPECT_TRUE(contains(contents, "TableName=\"tttt"));
}

std::string field_value(const std::string& line, std::string_view key) {
  const auto marker = std::string(key) + "=\"";
  const auto start = line.find(marker);
  if (start == std::string::npos) return {};
  const auto value = start + marker.size();
  return line.substr(value, line.find('"', value) - value);
}

TEST_F(OdbcTraceTest, CancelTracingFromAnotherThreadNeitherWaitsNorBorrowsDiagnostics) {
  Tracer::configure(trace_to(path_));
  const auto statement = reinterpret_cast<SQLHSTMT>(std::uintptr_t{0x5150});
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::thread executing([&] {
    traced_call("SQLExecute", SQL_NULL_HANDLE, true,
        [&]() noexcept -> SQLRETURN {
          entered.store(true);
          const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{10};
          while (!release.load() && std::chrono::steady_clock::now() < give_up) {
            std::this_thread::yield();
          }
          return SQL_SUCCESS;
        },
        statement);
  });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!entered.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool was_in_flight = entered.load();
  // The real export: an unregistered statement is SQL_INVALID_HANDLE. Tracing
  // must not wait for the in-flight call or borrow its diagnostics.
  const auto cancelled = SQLCancel(statement);
  release.store(true);
  executing.join();
  const auto contents = finish();

  ASSERT_TRUE(was_in_flight);
  EXPECT_EQ(SQL_INVALID_HANDLE, cancelled);
  const auto events = lines_with(contents, "event=odbc_trace_");
  ASSERT_EQ(4u, events.size());
  EXPECT_TRUE(contains(events[0], "event=odbc_trace_enter") && contains(events[0], "\"SQLExecute\""));
  EXPECT_TRUE(contains(events[1], "event=odbc_trace_enter") && contains(events[1], "\"SQLCancel\""));
  EXPECT_TRUE(contains(events[2], "event=odbc_trace_exit") && contains(events[2], "\"SQLCancel\""));
  EXPECT_TRUE(contains(events[3], "event=odbc_trace_exit") && contains(events[3], "\"SQLExecute\""));
  EXPECT_TRUE(contains(events[2], "return=\"SQL_INVALID_HANDLE\""));
  EXPECT_FALSE(contains(events[2], "sqlstate="));
  EXPECT_TRUE(contains(events[1], "StatementHandle=\"0x5150\""));
  EXPECT_EQ(field_value(events[1], "call"), field_value(events[2], "call"));
  EXPECT_EQ(field_value(events[0], "call"), field_value(events[3], "call"));
  EXPECT_NE(field_value(events[0], "call"), field_value(events[1], "call"));
  EXPECT_NE(field_value(events[0], "thread"), field_value(events[1], "thread"));
}

TEST_F(OdbcTraceTest, NestedCallsAndDisabledTracingWriteNothing) {
  Tracer::configure(trace_to(path_));
  const auto statement = reinterpret_cast<SQLHSTMT>(std::uintptr_t{0x1});
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLFetch", SQL_NULL_HANDLE, false,
      []() noexcept -> SQLRETURN { return SQL_SUCCESS; }, statement));
  Tracer::configure(TraceOptions{});
  EXPECT_EQ(nullptr, Tracer::active());
  EXPECT_EQ(SQL_SUCCESS, traced_call(
      "SQLFetch", SQL_NULL_HANDLE, true,
      []() noexcept -> SQLRETURN { return SQL_SUCCESS; }, statement));
  EXPECT_FALSE(contains(read_file(path_), "SQLFetch"));
}

TEST_F(OdbcTraceTest, JsonRecordsCarryEventsAndCorrelatedCallIdentifiers) {
  Tracer::configure(trace_to(path_, {{"ODBCTraceFormat", "Json"}}));
  const auto statement = reinterpret_cast<SQLHSTMT>(std::uintptr_t{0x2});
  EXPECT_EQ(SQL_NO_DATA, traced_call(
      "SQLFetch", SQL_NULL_HANDLE, true,
      []() noexcept -> SQLRETURN { return SQL_NO_DATA; }, statement));
  const auto contents = finish();
  const auto enter = lines_with(contents, "\"event\":\"odbc_trace_enter\"");
  const auto exit = lines_with(contents, "\"event\":\"odbc_trace_exit\"");
  ASSERT_EQ(1u, enter.size());
  ASSERT_EQ(1u, exit.size());
  const auto call_field = [](const std::string& line) {
    const auto start = line.find("\"call\":\"");
    return start == std::string::npos
        ? std::string() : line.substr(start, line.find('"', start + 8) - start);
  };
  EXPECT_FALSE(call_field(enter[0]).empty());
  EXPECT_EQ(call_field(enter[0]), call_field(exit[0]));
  EXPECT_TRUE(contains(exit[0], "\"return\":\"SQL_NO_DATA\""));
  EXPECT_TRUE(contains(exit[0], "\"message\":\"SQLFetch\""));
}

}  // namespace
