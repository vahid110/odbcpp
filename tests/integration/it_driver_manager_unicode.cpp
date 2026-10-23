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
#include <cstdint>
#include <cstdlib>
#include <memory>
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

std::string environment_value(const char* variable) {
#ifdef _WIN32
  char* value = nullptr;
  std::size_t size = 0;
  const auto status = _dupenv_s(&value, &size, variable);
  const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
  if (status != 0) {
    throw std::runtime_error(std::string("failed to read environment variable: ") + variable);
  }
  return value ? value : "";
#else
  const char* value = std::getenv(variable);
  return value ? value : "";
#endif
}

std::string connection_string() {
  const auto configured = environment_value("ODBCPP_DM_UNICODE_CONNECTION");
  if (!configured.empty()) return configured;
  return "DRIVER={ODBCPP PostgreSQL};SERVER=127.0.0.1;PORT=5432;"
         "DATABASE=postgres;UID=postgres;PWD=postgres;SSL=off;";
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

class DriverManagerUnicodeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS, allocate_environment(&environment_));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    auto input = connection_string();
    const auto result = SQLDriverConnect(connection_, nullptr,
        reinterpret_cast<SQLCHAR*>(input.data()), SQL_NTS, nullptr, 0, nullptr,
        SQL_DRIVER_NOPROMPT);
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
      EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection_));
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
    return SQLExecDirectW(statement_, text.data(), SQL_NTS);
  }

  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
  SQLHSTMT statement_{SQL_NULL_HSTMT};
  bool connected_{false};
};

// U+00E9, U+8868 and U+1F600: two, three and four UTF-8 bytes; the last is a
// surrogate pair for 2-byte managers and a single unit for 4-byte managers.
constexpr std::u32string_view kUnicodeName = U"\u00e9\u8868\U0001F600";

TEST_F(DriverManagerUnicodeTest, WideDescribeColReturnsUnicodeNameAndReportsZeroCapacity) {
  ASSERT_EQ(SQL_SUCCESS, execute_wide(U"SELECT 1 AS \"\u00e9\u8868\U0001F600\""))
      << first_state(SQL_HANDLE_STMT, statement_);
  const auto expected = wide_text(kUnicodeName);
  const auto units = unit_count(expected);

  std::vector<SQLWCHAR> name(16, SQLWCHAR('x'));
  SQLSMALLINT length = -1;
  ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(statement_, 1, name.data(),
      static_cast<SQLSMALLINT>(name.size()), &length, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(units, length);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), name.begin()));

  std::fill(name.begin(), name.end(), SQLWCHAR('x'));
  length = -1;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLDescribeColW(statement_, 1, name.data(), 0, &length, nullptr, nullptr, nullptr, nullptr));
  EXPECT_TRUE(has_state(diagnostic_states(SQL_HANDLE_STMT, statement_), "01004"));
  EXPECT_EQ(units, length);
  EXPECT_EQ(SQLWCHAR('x'), name[0]);

  length = -1;
  ASSERT_EQ(SQL_SUCCESS,
            SQLDescribeColW(statement_, 1, nullptr, 0, &length, nullptr, nullptr, nullptr, nullptr));
  EXPECT_EQ(units, length);
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
  EXPECT_EQ(units, length);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), record.begin()));
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

TEST(DriverManagerUnicodeConnectTest, WideDriverConnectZeroCapacityOutputReportsTruncation) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  ASSERT_EQ(SQL_SUCCESS, allocate_environment(&environment));
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
  const auto input = connection_string();
  std::u32string wide_input(input.begin(), input.end());
  auto text = wide_text(wide_input);
  std::vector<SQLWCHAR> output(8, SQLWCHAR('x'));
  SQLSMALLINT length = -1;
  const auto result = SQLDriverConnectW(connection, nullptr, text.data(), SQL_NTS,
      output.data(), 0, &length, SQL_DRIVER_NOPROMPT);
  const auto states = diagnostic_states(SQL_HANDLE_DBC, connection);
  if (result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO) {
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, result);
    EXPECT_TRUE(has_state(states, "01004"));
    EXPECT_GT(length, 0);
    EXPECT_EQ(SQLWCHAR('x'), output[0]);
    EXPECT_EQ(SQL_SUCCESS, SQLDisconnect(connection));
  } else {
    ADD_FAILURE() << "SQLDriverConnectW failed: " << (states.empty() ? "none" : states.front());
  }
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

}  // namespace
