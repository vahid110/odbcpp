#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>
#ifdef ODBCPP_TEST_IODBC
#include <iodbcext.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef ODBCPP_EXPECT_DM_SQLWCHAR_SIZE
static_assert(sizeof(SQLWCHAR) == ODBCPP_EXPECT_DM_SQLWCHAR_SIZE,
              "Driver-manager Unicode test is using an unexpected SQLWCHAR ABI");
#endif

// Application-visible Unicode metadata, truncation and diagnostic contracts
// observed through the real driver manager (Windows ODBC, unixODBC or iODBC).
// The default endpoint is the disposable PostgreSQL server and registered
// "ODBCPP PostgreSQL" driver that CI provisions; ODBCPP_DM_UNICODE_CONNECTION
// overrides it for local runs. Connection strings are never printed.
namespace {

// One original case window, shared by every connect/reconnect and cleanup.
// Attribute changes precede I/O only: restoring them afterwards would replace
// the Unicode/truncation diagnostics that these tests are meant to inspect.
constexpr SQLULEN fixture_timeout_seconds(std::int64_t left, SQLULEN requested = 0) {
  if (left <= 0) {
    return 0;
  }
  const auto cap = static_cast<SQLULEN>(std::min<std::int64_t>(left, 5));
  return requested == 0 ? cap : std::min(cap, requested);
}
static_assert(fixture_timeout_seconds(40) == 5);
static_assert(fixture_timeout_seconds(4) == 4);
static_assert(fixture_timeout_seconds(1) == 1);
static_assert(fixture_timeout_seconds(0) == 0);
static_assert(fixture_timeout_seconds(-1) == 0);
static_assert(fixture_timeout_seconds(40, 1) == 1);
static_assert(fixture_timeout_seconds(40, 2) == 2);

class FixtureBudget {
 public:
  bool connection(SQLHDBC connection, bool login) const {
    // LOGIN_TIMEOUT governs connect. iODBC cannot read CONNECTION_TIMEOUT
    // before a driver is loaded; apply that attribute before subsequent I/O.
    return connection_attribute(connection,
        login ? SQL_ATTR_LOGIN_TIMEOUT : SQL_ATTR_CONNECTION_TIMEOUT);
  }

  bool statement(SQLHDBC connection, SQLHSTMT statement) const {
    if (!this->connection(connection, false)) {
      return false;
    }
    SQLULEN requested = 0;
    if (SQLGetStmtAttr(statement, SQL_ATTR_QUERY_TIMEOUT, &requested, 0, nullptr) != SQL_SUCCESS) {
      ADD_FAILURE() << "Could not read finite fixture statement timeout";
      return false;
    }
    const auto seconds = seconds_left(requested);
    if (seconds == 0) {
      return false;
    }
    if (seconds != requested && SQLSetStmtAttr(statement, SQL_ATTR_QUERY_TIMEOUT,
            reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
      ADD_FAILURE() << "Could not apply finite fixture statement timeout";
      return false;
    }
    return true;
  }

 private:
  SQLULEN seconds_left(SQLULEN requested) const {
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(
        end_ - std::chrono::steady_clock::now()).count();
    const auto seconds = fixture_timeout_seconds(left, requested);
    if (seconds == 0) {
      ADD_FAILURE() << "Original forty-second fixture window expired; no new I/O";
    }
    return seconds;
  }

  bool connection_attribute(SQLHDBC connection, SQLINTEGER attribute) const {
    SQLULEN requested = 0;
    if (SQLGetConnectAttr(connection, attribute, &requested, 0, nullptr) != SQL_SUCCESS) {
      ADD_FAILURE() << "Could not read finite fixture connection timeout";
      return false;
    }
    const auto seconds = seconds_left(requested);
    if (seconds == 0) {
      return false;
    }
    if (seconds != requested && SQLSetConnectAttr(connection, attribute,
            reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
      ADD_FAILURE() << "Could not apply finite fixture connection timeout";
      return false;
    }
    return true;
  }

  const std::chrono::steady_clock::time_point end_ =
      std::chrono::steady_clock::now() + std::chrono::seconds(40);
};

// One Unicode scalar as UTF-8.
void append_utf8(std::string& output, std::uint32_t code_point) {
  if (code_point < 0x80) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    output.push_back(static_cast<char>(0xc0 | (code_point >> 6)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else if (code_point < 0x10000) {
    output.push_back(static_cast<char>(0xe0 | (code_point >> 12)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (code_point >> 18)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
  }
}

// Strict UTF-8 to Unicode scalars. Overlong forms, surrogates, values above
// U+10FFFF, stray continuation bytes and truncated sequences are refused.
std::optional<std::u32string> decode_utf8(std::string_view input) {
  std::u32string output;
  for (std::size_t index = 0; index < input.size();) {
    const auto lead = static_cast<unsigned char>(input[index]);
    if (lead < 0x80) {
      output.push_back(lead);
      ++index;
      continue;
    }
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if ((lead & 0xe0) == 0xc0) {
      length = 2;
      code_point = lead & 0x1f;
      minimum = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
      length = 3;
      code_point = lead & 0x0f;
      minimum = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
      length = 4;
      code_point = lead & 0x07;
      minimum = 0x10000;
    } else {
      return std::nullopt;
    }
    if (input.size() - index < length) return std::nullopt;
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(input[index + offset]);
      if ((next & 0xc0) != 0x80) return std::nullopt;
      code_point = (code_point << 6) | (next & 0x3f);
    }
    if (code_point < minimum || code_point > 0x10ffff ||
        (code_point >= 0xd800 && code_point <= 0xdfff)) {
      return std::nullopt;
    }
    output.push_back(static_cast<char32_t>(code_point));
    index += length;
  }
  return output;
}

#ifdef _WIN32
// Strict UTF-16 to Unicode scalars; unpaired surrogates are refused.
std::optional<std::u32string> decode_utf16(std::wstring_view input) {
  std::u32string output;
  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto unit = static_cast<std::uint32_t>(input[index]);
    if (unit >= 0xdc00 && unit <= 0xdfff) return std::nullopt;
    if (unit < 0xd800 || unit > 0xdbff) {
      output.push_back(static_cast<char32_t>(unit));
      continue;
    }
    if (index + 1 == input.size()) return std::nullopt;
    const auto low = static_cast<std::uint32_t>(input[++index]);
    if (low < 0xdc00 || low > 0xdfff) return std::nullopt;
    output.push_back(static_cast<char32_t>(0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00)));
  }
  return output;
}
#endif

constexpr std::string_view kDefaultConnection =
    "DRIVER={ODBCPP PostgreSQL};SERVER=127.0.0.1;PORT=5432;"
    "DATABASE=postgres;UID=postgres;PWD=postgres;SSL=off;";

// The override is Unicode text: UTF-8 bytes on POSIX and the UTF-16
// environment block on Windows, never the ANSI code page. Both W and A inputs
// are built from these complete scalars. The content is never printed.
std::u32string connection_text() {
#ifdef _WIN32
  wchar_t* value = nullptr;
  std::size_t size = 0;
  const auto status = _wdupenv_s(&value, &size, L"ODBCPP_DM_UNICODE_CONNECTION");
  const std::unique_ptr<wchar_t, decltype(&std::free)> owned(value, &std::free);
  if (status != 0) {
    throw std::runtime_error("failed to read ODBCPP_DM_UNICODE_CONNECTION");
  }
  const auto decoded = value && *value ? decode_utf16(value) : decode_utf8(kDefaultConnection);
#else
  const char* value = std::getenv("ODBCPP_DM_UNICODE_CONNECTION");
  const auto decoded = decode_utf8(value && *value ? std::string_view(value) : kDefaultConnection);
#endif
  if (!decoded) {
    throw std::runtime_error("ODBCPP_DM_UNICODE_CONNECTION is not valid Unicode text");
  }
  return *decoded;
}

// The ANSI form an application would pass: the active code page on Windows,
// UTF-8 elsewhere. Text the code page cannot represent is refused.
std::string connection_string() {
  const auto text = connection_text();
#ifdef _WIN32
  std::wstring units;
  for (const auto character : text) {
    const auto code_point = static_cast<std::uint32_t>(character);
    if (code_point > 0xffff) {
      units.push_back(static_cast<wchar_t>(0xd800 + ((code_point - 0x10000) >> 10)));
      units.push_back(static_cast<wchar_t>(0xdc00 + ((code_point - 0x10000) & 0x3ff)));
    } else {
      units.push_back(static_cast<wchar_t>(code_point));
    }
  }
  if (units.empty()) return {};
  // CP_UTF8 accepts neither best-fit flags nor a default-character report.
  const bool utf8_code_page = GetACP() == CP_UTF8;
  BOOL used_default = FALSE;
  const auto flags = utf8_code_page ? 0 : WC_NO_BEST_FIT_CHARS;
  auto* const report = utf8_code_page ? nullptr : &used_default;
  const int size = WideCharToMultiByte(CP_ACP, flags, units.data(), static_cast<int>(units.size()),
                                       nullptr, 0, nullptr, report);
  std::string output(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
  if (size <= 0 ||
      WideCharToMultiByte(CP_ACP, flags, units.data(), static_cast<int>(units.size()),
                          output.data(), size, nullptr, report) != size ||
      used_default) {
    throw std::runtime_error(
        "ODBCPP_DM_UNICODE_CONNECTION is not representable in the ANSI code page");
  }
  return output;
#else
  std::string output;
  for (const auto character : text) append_utf8(output, static_cast<std::uint32_t>(character));
  return output;
#endif
}

// UTF-32 to the manager's SQLWCHAR form, surrogate pairs for 2-byte units.
std::vector<SQLWCHAR> wide_text(std::u32string_view input) {
  std::vector<SQLWCHAR> output;
  output.reserve(input.size() * 2 + 1);
  for (const auto character : input) {
    const auto code_point = static_cast<std::uint32_t>(character);
    if (sizeof(SQLWCHAR) == 2 && code_point > 0xffff) {
      const auto value = code_point - 0x10000;
      output.push_back(static_cast<SQLWCHAR>(0xd800 + (value >> 10)));
      output.push_back(static_cast<SQLWCHAR>(0xdc00 + (value & 0x3ff)));
    } else {
      output.push_back(static_cast<SQLWCHAR>(code_point));
    }
  }
  output.push_back(0);
  return output;
}

SQLSMALLINT unit_count(const std::vector<SQLWCHAR>& terminated) {
  return static_cast<SQLSMALLINT>(terminated.size() - 1);
}

std::vector<SQLWCHAR> without_terminator(std::vector<SQLWCHAR> terminated) {
  terminated.pop_back();
  return terminated;
}

// SQLWCHAR units to UTF-8 for failure messages; unpaired surrogates become U+FFFD.
std::string utf8(const std::vector<SQLWCHAR>& units) {
  std::string output;
  for (std::size_t index = 0; index < units.size(); ++index) {
    auto code_point = static_cast<std::uint32_t>(units[index]);
    if (sizeof(SQLWCHAR) == 2 && code_point >= 0xd800 && code_point <= 0xdfff) {
      const bool paired = code_point <= 0xdbff && index + 1 < units.size() &&
          units[index + 1] >= 0xdc00 && units[index + 1] <= 0xdfff;
      code_point = paired ? 0x10000 + ((code_point - 0xd800) << 10) +
                                (static_cast<std::uint32_t>(units[++index]) - 0xdc00)
                          : 0xfffd;
    }
    append_utf8(output, code_point);
  }
  return output;
}

bool contains_units(const std::vector<SQLWCHAR>& haystack, const std::vector<SQLWCHAR>& needle) {
  return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) !=
         haystack.end();
}

// Every diagnostic state on a handle, in record order. Wide calls with real
// buffers, because a manager may reject NULL message buffers on mapped calls.
std::vector<std::string> diagnostic_states(SQLSMALLINT handle_type, SQLHANDLE handle) {
  std::vector<std::string> states;
  for (SQLSMALLINT record = 1; record <= 16; ++record) {
    SQLWCHAR state[6]{};
    SQLINTEGER native = 0;
    SQLWCHAR message[512]{};
    SQLSMALLINT length = 0;
    const auto result = SQLGetDiagRecW(handle_type, handle, record, state, &native, message,
                                       512, &length);
    if (result != SQL_SUCCESS && result != SQL_SUCCESS_WITH_INFO) break;
    std::string text;
    for (const auto unit : state) {
      if (unit == 0) break;
      text.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    states.push_back(text);
  }
  return states;
}

std::string joined(const std::vector<std::string>& states) {
  std::string text;
  for (const auto& state : states) text += (text.empty() ? "" : ",") + state;
  return text.empty() ? std::string("none") : text;
}

::testing::AssertionResult has_state(const std::vector<std::string>& states,
                                     std::string_view expected) {
  if (std::find(states.begin(), states.end(), expected) != states.end()) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure() << "expected " << expected << " among " << joined(states);
}

std::string first_state(SQLSMALLINT handle_type, SQLHANDLE handle) {
  const auto states = diagnostic_states(handle_type, handle);
  return states.empty() ? std::string("none") : states.front();
}

SQLRETURN allocate_environment(SQLHENV* environment) {
  auto result = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, environment);
  if (result != SQL_SUCCESS) return result;
  result = SQLSetEnvAttr(*environment, SQL_ATTR_ODBC_VERSION,
                         reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0);
#ifdef ODBCPP_TEST_IODBC
  if (result == SQL_SUCCESS) {
    const auto application_unicode_type = sizeof(SQLWCHAR) == 2 ? SQL_DM_CP_UTF16 : SQL_DM_CP_UCS4;
    result = SQLSetEnvAttr(*environment, SQL_ATTR_APP_UNICODE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(application_unicode_type)),
        SQL_IS_UINTEGER);
  }
#endif
  return result;
}

std::vector<SQLWCHAR> wide_connection_string() {
  return wide_text(connection_text());
}

class DriverManagerUnicodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS, allocate_environment(&environment_));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    // A Unicode application connects wide; unixODBC otherwise treats the
    // connection as ANSI and widens driver diagnostics through the C locale.
    auto input = wide_connection_string();
    ASSERT_TRUE(budget_.connection(connection_, true));
    const auto result = SQLDriverConnectW(connection_, nullptr, input.data(), SQL_NTS, nullptr,
                                          0, nullptr, SQL_DRIVER_NOPROMPT);
    ASSERT_TRUE(result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO)
        << "Driver-manager connect failed: " << first_state(SQL_HANDLE_DBC, connection_);
    connected_ = true;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  }

  void TearDown() override {
    if (statement_ != SQL_NULL_HSTMT) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement_));
    }
    if (connected_) {
      if (budget_.connection(connection_, false)) {
        EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));
      }
    }
    if (connection_ != SQL_NULL_HDBC) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection_));
    }
    if (environment_ != SQL_NULL_HENV) {
      EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment_));
    }
  }

  SQLRETURN execute_wide(std::u32string_view sql) {
    auto text = wide_text(sql);
    if (!budget_.statement(connection_, statement_)) {
      return SQL_ERROR;
    }
    return SQLExecDirectW(statement_, text.data(), SQL_NTS);
  }

  const FixtureBudget budget_;
  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  SQLHSTMT statement_{SQL_NULL_HSTMT};
  bool connected_{false};
};

// U+00E9, U+8868 and U+1F600: two, three and four UTF-8 bytes; the last is a
// surrogate pair for 2-byte managers and a single unit for 4-byte managers.
constexpr std::u32string_view kUnicodeName = U"\u00e9\u8868\U0001F600";

// iODBC 3.52.16 converts the text of a non-BMP name between a UTF-16 driver and
// a UCS4 application but passes the driver's character counts through
// unconverted (4 instead of 3) for SQLDescribeColW and SQLGetDescRecW, while
// SQLColAttributeW byte lengths are converted. Content, return codes and 01004
// stay asserted; only those unit counts are skipped for that pairing.
#if defined(ODBCPP_TEST_IODBC) && defined(ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE) && \
    defined(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE) && \
    ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE != ODBCPP_EXPECT_DM_SQLWCHAR_SIZE
constexpr bool kManagerConvertsNonBmpCharacterCounts = false;
#else
constexpr bool kManagerConvertsNonBmpCharacterCounts = true;
#endif
constexpr const char* kMixedWidthCountSkip =
    "iODBC passes a narrower driver's non-BMP character counts through unconverted";

TEST_F(DriverManagerUnicodeTest, WideDescribeColReturnsUnicodeNameAndReportsZeroCapacity) {
  ASSERT_EQ(SQL_SUCCESS, execute_wide(U"SELECT 1 AS \"\u00e9\u8868\U0001F600\""))
      << first_state(SQL_HANDLE_STMT, statement_);
  const auto expected = wide_text(kUnicodeName);
  const auto units = unit_count(expected);

  std::vector<SQLWCHAR> name(16, SQLWCHAR('x'));
  SQLSMALLINT full_length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(statement_, 1, name.data(),
      static_cast<SQLSMALLINT>(name.size()), &full_length, nullptr, nullptr, nullptr, nullptr));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), name.begin()));

  std::fill(name.begin(), name.end(), SQLWCHAR('x'));
  SQLSMALLINT zero_length = -1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLDescribeColW(statement_, 1, name.data(), 0, &zero_length,
      nullptr, nullptr, nullptr, nullptr));
  EXPECT_TRUE(has_state(diagnostic_states(SQL_HANDLE_STMT, statement_), "01004"));
  EXPECT_EQ(SQLWCHAR('x'), name[0]);

  SQLSMALLINT null_length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(statement_, 1, nullptr, 0, &null_length, nullptr,
      nullptr, nullptr, nullptr));

  if (!kManagerConvertsNonBmpCharacterCounts) {
    GTEST_SKIP() << kMixedWidthCountSkip << " (reported " << full_length << ", expected "
                 << units << ")";
  }
  EXPECT_EQ(units, full_length);
  EXPECT_EQ(units, zero_length);
  EXPECT_EQ(units, null_length);
}

TEST_F(DriverManagerUnicodeTest, AnsiDescribeColReportsZeroCapacityTruncation) {
  ASSERT_EQ(SQL_SUCCESS, execute_wide(U"SELECT 1 AS ascii_col"))
      << first_state(SQL_HANDLE_STMT, statement_);
  SQLCHAR name[8];
  std::fill(std::begin(name), std::end(name), SQLCHAR('x'));
  SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLDescribeCol(statement_, 1, name, 0, &length, nullptr, nullptr, nullptr, nullptr));
  EXPECT_TRUE(has_state(diagnostic_states(SQL_HANDLE_STMT, statement_), "01004"));
  EXPECT_EQ(9, length);
  EXPECT_EQ(SQLCHAR('x'), name[0]);
}

TEST_F(DriverManagerUnicodeTest, WideColAttributeAndDescRecAgreeOnUnicodeName) {
  ASSERT_EQ(SQL_SUCCESS, execute_wide(U"SELECT 1 AS \"\u00e9\u8868\U0001F600\""))
      << first_state(SQL_HANDLE_STMT, statement_);
  const auto expected = wide_text(kUnicodeName);
  const auto units = unit_count(expected);

  std::vector<SQLWCHAR> attribute(16, SQLWCHAR('x'));
  SQLSMALLINT bytes = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLColAttributeW(statement_, 1, SQL_DESC_NAME, attribute.data(),
      static_cast<SQLSMALLINT>(attribute.size() * sizeof(SQLWCHAR)), &bytes, nullptr));
  EXPECT_EQ(static_cast<SQLSMALLINT>(units * sizeof(SQLWCHAR)), bytes);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), attribute.begin()));

  SQLHDESC ird = SQL_NULL_HDESC;
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttrW(statement_, SQL_ATTR_IMP_ROW_DESC, &ird, 0, nullptr));
  std::vector<SQLWCHAR> record(16, SQLWCHAR('x'));
  SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescRecW(ird, 1, record.data(),
      static_cast<SQLSMALLINT>(record.size()), &length, nullptr, nullptr, nullptr, nullptr,
      nullptr, nullptr));
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), record.begin()));

  if (!kManagerConvertsNonBmpCharacterCounts) {
    GTEST_SKIP() << kMixedWidthCountSkip << " (reported " << length << ", expected " << units
                 << ")";
  }
  EXPECT_EQ(units, length);
}

// PostgreSQL's 63-byte identifier limit: 21 three-byte characters.
TEST_F(DriverManagerUnicodeTest, WideDescribeColReturnsMaximumLengthMultibyteName) {
  std::u32string name(21, U'\u8868');
  ASSERT_EQ(SQL_SUCCESS, execute_wide(U"SELECT 1 AS \"" + name + U"\""))
      << first_state(SQL_HANDLE_STMT, statement_);
  const auto expected = wide_text(name);
  std::vector<SQLWCHAR> actual(64, SQLWCHAR('x'));
  SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(statement_, 1, actual.data(),
      static_cast<SQLSMALLINT>(actual.size()), &length, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(21, length);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), actual.begin()));

  // A buffer one unit short truncates on a whole character with 01004.
  std::fill(actual.begin(), actual.end(), SQLWCHAR('x'));
  length = -1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLDescribeColW(statement_, 1, actual.data(), 21, &length,
      nullptr, nullptr, nullptr, nullptr));
  EXPECT_TRUE(has_state(diagnostic_states(SQL_HANDLE_STMT, statement_), "01004"));
  EXPECT_EQ(21, length);
  EXPECT_TRUE(std::equal(expected.begin(), expected.begin() + 20, actual.begin()));
  EXPECT_EQ(SQLWCHAR(0), actual[20]);
}

TEST_F(DriverManagerUnicodeTest, WideDiagnosticCarriesUnicodeServerText) {
  ASSERT_EQ(SQL_ERROR, execute_wide(U"SELECT * FROM \"\u00e9\u8868\U0001F600_missing\""));
  SQLWCHAR state[6]{};
  SQLINTEGER native = 0;
  std::vector<SQLWCHAR> message(512, 0);
  SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRecW(SQL_HANDLE_STMT, statement_, 1, state, &native,
      message.data(), static_cast<SQLSMALLINT>(message.size()), &length));
  EXPECT_EQ(std::vector<SQLWCHAR>(state, state + 6), wide_text(U"42S02"));
  ASSERT_GT(length, 0);
  ASSERT_LT(length, static_cast<SQLSMALLINT>(message.size()));
  message.resize(static_cast<std::size_t>(length));
  EXPECT_TRUE(contains_units(message, without_terminator(wide_text(kUnicodeName))))
      << "message: " << utf8(message);
}

TEST_F(DriverManagerUnicodeTest, LegacySqlErrorWDrainsRecordsThenReturnsNoData) {
  ASSERT_EQ(SQL_ERROR, execute_wide(U"SELECT * FROM odbcpp_dm_unicode_missing"));
  int records = 0;
  SQLRETURN result = SQL_SUCCESS;
  std::vector<SQLWCHAR> first_state;
  while (records < 8) {
    SQLWCHAR state[6]{};
    SQLINTEGER native = 0;
    SQLWCHAR message[256]{};
    SQLSMALLINT length = -1;
    result = SQLErrorW(SQL_NULL_HENV, SQL_NULL_HDBC, statement_, state, &native, message,
                       256, &length);
    if (result == SQL_NO_DATA) {
      break;
    }
    ASSERT_TRUE(result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO) << "SQLErrorW " << result;
    if (records == 0) {
      first_state.assign(state, state + 6);
    }
    ++records;
  }
  EXPECT_EQ(SQL_NO_DATA, result);
  EXPECT_GE(records, 1);
  EXPECT_EQ(wide_text(U"42S02"), first_state);
}

// One wide driver connect with the given output capacity; disconnects on success.
struct ConnectOutcome {
  SQLRETURN result{SQL_ERROR};
  SQLSMALLINT length{-1};
  std::vector<SQLWCHAR> output;
  std::vector<std::string> states;
};

// Room for the completed string; Redshift IAM passwords alone exceed 1700 units.
std::size_t full_output_capacity() {
  return std::max<std::size_t>(1024, wide_connection_string().size() + 256);
}

ConnectOutcome wide_connect(SQLSMALLINT capacity, const FixtureBudget& budget) {
  ConnectOutcome outcome;
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  if (allocate_environment(&environment) != SQL_SUCCESS ||
      SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection) != SQL_SUCCESS) {
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
    return outcome;
  }
  auto input = wide_connection_string();
  outcome.output.assign(full_output_capacity(), SQLWCHAR('x'));
  if (budget.connection(connection, true)) {
    outcome.result = SQLDriverConnectW(connection, nullptr, input.data(), SQL_NTS,
        outcome.output.data(), capacity, &outcome.length, SQL_DRIVER_NOPROMPT);
    outcome.states = diagnostic_states(SQL_HANDLE_DBC, connection);
  }
  if (outcome.result == SQL_SUCCESS || outcome.result == SQL_SUCCESS_WITH_INFO) {
    if (budget.connection(connection, false)) {
      EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
    }
  }
  SQLFreeHandle(SQL_HANDLE_DBC, connection);
  SQLFreeHandle(SQL_HANDLE_ENV, environment);
  return outcome;
}

TEST(DriverManagerUnicodeConnectTest, WideDriverConnectTruncatedOutputReportsFullLength) {
  const FixtureBudget budget;
#ifdef ODBCPP_TEST_IODBC
  // iODBC 3.52.16 (A and W) returns SQL_SUCCESS_WITH_INFO without the driver's
  // 01004 and reports the copied length (capacity 8 -> 7) or a length scanned
  // from the application's unterminated buffer (capacity 0); unixODBC and the
  // directly linked driver report 01004 and the full length. Manager defect.
  GTEST_SKIP() << "iODBC drops driver-connect truncation diagnostics and lengths";
#endif
  const auto full = wide_connect(static_cast<SQLSMALLINT>(full_output_capacity()), budget);
  ASSERT_EQ(SQL_SUCCESS, full.result) << joined(full.states);
  ASSERT_GT(full.length, 8);

  for (const SQLSMALLINT capacity : {SQLSMALLINT{8}, SQLSMALLINT{0}}) {
    SCOPED_TRACE(capacity);
    const auto truncated = wide_connect(capacity, budget);
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, truncated.result) << joined(truncated.states);
    EXPECT_TRUE(has_state(truncated.states, "01004"));
    EXPECT_EQ(full.length, truncated.length);
    if (capacity == 0) {
      EXPECT_EQ(SQLWCHAR('x'), truncated.output[0]);
    } else {
      EXPECT_TRUE(std::equal(full.output.begin(), full.output.begin() + capacity - 1,
                             truncated.output.begin()));
      EXPECT_EQ(SQLWCHAR(0), truncated.output[static_cast<std::size_t>(capacity - 1)]);
    }
  }
}

// A legacy code-page ANSI application passes a Latin-1 byte (0xE9) in an
// unknown keyword. The driver echoes the keyword into its 01S00 warning; the
// warning's SQLSTATE must stay readable through the manager, ANSI and wide.
TEST(DriverManagerUnicodeConnectTest, AnsiLatin1KeywordWarningStateStaysReadable) {
  const FixtureBudget budget;
#ifdef ODBCPP_TEST_IODBC
  // iODBC 3.52.16 returns the driver's SQL_SUCCESS_WITH_INFO from
  // SQLDriverConnect(A/W) but then reports SQL_NO_DATA for every record, also
  // for a plain ASCII unknown keyword (01S00) and for 01004. Manager defect.
  GTEST_SKIP() << "iODBC drops driver-connect warning records";
#endif
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS, allocate_environment(&environment));
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  auto input = connection_string();
  if (!input.empty() && input.back() != ';') {
    input.push_back(';');
  }
  input += "ODBCPP_ECHO_\xe9=1;";
  const auto result = budget.connection(connection, true)
      ? SQLDriverConnect(connection, nullptr, reinterpret_cast<SQLCHAR*>(input.data()),
                         SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT)
      : SQL_ERROR;
  const bool connected = result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO;
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO, result);

  SQLCHAR narrow_state[6]{};
  SQLINTEGER narrow_native = -1;
  SQLCHAR narrow_message[512]{};
  SQLSMALLINT narrow_length = -1;
  const auto narrow = SQLGetDiagRec(SQL_HANDLE_DBC, connection, 1, narrow_state,
      &narrow_native, narrow_message, sizeof(narrow_message), &narrow_length);
  EXPECT_EQ(SQL_SUCCESS, narrow) << "ANSI SQLGetDiagRec";
  EXPECT_STREQ("01S00", reinterpret_cast<char*>(narrow_state));

  SQLWCHAR wide_state[6]{};
  SQLINTEGER wide_native = -1;
  SQLWCHAR wide_message[512]{};
  SQLSMALLINT wide_length = -1;
  const auto wide = SQLGetDiagRecW(SQL_HANDLE_DBC, connection, 1, wide_state, &wide_native,
      wide_message, 512, &wide_length);
  EXPECT_EQ(SQL_SUCCESS, wide) << "SQLGetDiagRecW";
  EXPECT_EQ(std::vector<SQLWCHAR>(wide_state, wide_state + 6), wide_text(U"01S00"));

  if (connected) {
    if (budget.connection(connection, false)) {
      EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
    }
  }
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

// No connection: the override path decodes complete scalars before building W
// input. U+00E9 is one unit; U+1F600 is a surrogate pair for 2-byte SQLWCHAR
// and one unit for 4-byte SQLWCHAR; exactly one terminator follows.
TEST(DriverManagerUnicodeHarnessTest, OverrideTextDecodesCompleteScalarsIntoManagerUnits) {
  const auto decoded = decode_utf8("DATABASE=caf\xc3\xa9\xf0\x9f\x98\x80;");
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(std::u32string(U"DATABASE=café\U0001F600;"), *decoded);

  std::vector<SQLWCHAR> expected;
  for (const char character : std::string_view("DATABASE=caf")) {
    expected.push_back(static_cast<SQLWCHAR>(character));
  }
  expected.push_back(static_cast<SQLWCHAR>(0xe9));
  if constexpr (sizeof(SQLWCHAR) == 2) {
    expected.push_back(static_cast<SQLWCHAR>(0xd83d));
    expected.push_back(static_cast<SQLWCHAR>(0xde00));
  } else {
    expected.push_back(static_cast<SQLWCHAR>(0x1f600));
  }
  expected.push_back(static_cast<SQLWCHAR>(';'));
  expected.push_back(0);
  const auto units = wide_text(*decoded);
  EXPECT_EQ(expected, units);
  EXPECT_EQ(1, std::count(units.begin(), units.end(), SQLWCHAR{0}));

  // Truncated, overlong, surrogate, above U+10FFFF and stray continuation.
  for (const std::string_view invalid :
       {"\xc3", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xa9"}) {
    EXPECT_FALSE(decode_utf8(invalid).has_value()) << "invalid " << invalid.size() << "-byte input";
  }
#ifdef _WIN32
  const auto wide = decode_utf16(L"café\U0001F600");
  ASSERT_TRUE(wide.has_value());
  EXPECT_EQ(std::u32string(U"café\U0001F600"), *wide);
  EXPECT_FALSE(decode_utf16(std::wstring(1, static_cast<wchar_t>(0xd83d))).has_value());
  EXPECT_FALSE(decode_utf16(std::wstring(1, static_cast<wchar_t>(0xde00))).has_value());
#endif
}

}  // namespace
