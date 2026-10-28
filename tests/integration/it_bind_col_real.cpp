#include <gtest/gtest.h>
#include <cstdlib>
#include <optional>
#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include "odbcpp/database/backend_provider.h"
#include "odbc/connection_string.h"
#endif
#include "tests/test_connection_config.h"
#include "tests/test_dm_expectations.h"
#include "tests/test_product_profile.h"
#include "tests/test_text.h"
#include "tests/test_time_helpers.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <clocale>
#include <ctime>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>


namespace {
// SQL fixture grammar follows the actual compiled native product; byte and
// diagnostic expectations below remain identical across both profiles.
bool fixture_redshift_product() {
  return odbcpp::test::compiled_redshift_product();
}

// One original per-case window, including reconnects and sibling handles.
// Apply caps only on I/O entry, leaving allocation defaults and deliberate shorter
// timeout values intact. Never restore attributes after a call: its diagnostics belong to the test.
constexpr SQLULEN fixture_timeout_seconds(std::int64_t left, SQLULEN requested = 0) {
  if (left <= 0) return 0;
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
static_assert(fixture_timeout_seconds(40, 3) == 3);

SQLULEN fixture_seconds_left(std::chrono::steady_clock::time_point end) {
  const auto seconds = fixture_timeout_seconds(
      std::chrono::duration_cast<std::chrono::seconds>(
          end - std::chrono::steady_clock::now()).count());
  if (seconds == 0) {
    ADD_FAILURE() << "Original forty-second fixture window expired; no new I/O";
  }
  return seconds;
}

bool fixture_statement_budget(std::chrono::steady_clock::time_point end, SQLHSTMT statement) {
  const auto left = fixture_seconds_left(end);
  if (left == 0) return false;
  SQLULEN requested = 0;
  if (SQLGetStmtAttr(statement, SQL_ATTR_QUERY_TIMEOUT, &requested, 0, nullptr) != SQL_SUCCESS) {
    // Preserve the original API's invalid-handle/local-validation assertion.
    return true;
  }
  const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), requested);
  if (seconds == requested) return true;
  if (SQLSetStmtAttr(statement, SQL_ATTR_QUERY_TIMEOUT,
                     reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
    ADD_FAILURE() << "Could not apply finite fixture statement timeout";
    return false;
  }
  return true;
}

bool fixture_connection_budget(std::chrono::steady_clock::time_point end, SQLHDBC connection, bool login) {
  const auto left = fixture_seconds_left(end);
  if (left == 0) return false;
  SQLUINTEGER current = 0;
  if (login) {
    SQLUINTEGER dead = SQL_CD_TRUE;
    if (SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_DEAD, &dead, sizeof(dead), nullptr) == SQL_SUCCESS && dead == SQL_CD_FALSE) {
      // An already-connected connect call is a local 08002 test, not new I/O.
      return true;
    }
    if (odbcpp::test::get_login_timeout(connection, &current) != SQL_SUCCESS) return true;
    const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), current);
    if (SQLSetConnectAttr(connection, SQL_ATTR_LOGIN_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
      ADD_FAILURE() << "Could not apply finite fixture login timeout";
      return false;
    }
  }
  if (SQLGetConnectAttr(connection, SQL_ATTR_CONNECTION_TIMEOUT, &current, sizeof(current), nullptr) != SQL_SUCCESS) return true;
  const auto seconds = fixture_timeout_seconds(static_cast<std::int64_t>(left), current);
  if (SQLSetConnectAttr(connection, SQL_ATTR_CONNECTION_TIMEOUT,
      reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(seconds)), 0) != SQL_SUCCESS) {
    ADD_FAILURE() << "Could not apply finite fixture connection timeout";
    return false;
  }
  return true;
}

// Dedicated ROOT-preowned relation only; this fixture never creates/drops it.
constexpr bool timeout_table_name(std::string_view name) {
    if (!name.starts_with("odbcpp_timeout_gate_") || name.size() <= 20 || name.size() > 63) return false;
    for (char ch : name) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}
constexpr std::optional<std::uint32_t> timeout_positive_id(std::string_view text) {
    std::uint32_t value = 0;
    if (text.empty()) return std::nullopt;
    for (char ch : text) {
        if (ch < '0' || ch > '9' || value > (UINT32_MAX - static_cast<unsigned>(ch - '0')) / 10) return std::nullopt;
        value = value * 10 + static_cast<unsigned>(ch - '0');
    }
    return value == 0 ? std::nullopt : std::optional<std::uint32_t>(value);
}
constexpr std::optional<std::uint64_t> timeout_server_milliseconds(std::string_view text) {
    std::uint64_t value = 0;
    std::size_t digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
        const auto digit = static_cast<unsigned>(text[digits++] - '0');
        if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
        value = value * 10 + digit;
    }
    if (digits == 0) return std::nullopt;
    const auto unit = text.substr(digits);
    const std::uint64_t factor = unit.empty() || unit == "ms" ? 1 : unit == "s" ? 1000 : unit == "min" ? 60000 : 0;
    if (factor == 0 || value > UINT64_MAX / factor) return std::nullopt;
    return value * factor;
}
constexpr bool timeout_server_allows_client(std::string_view text) {
    const auto ms = timeout_server_milliseconds(text);
    return ms && (*ms == 0 || *ms >= 3000);
}
static_assert(timeout_table_name("odbcpp_timeout_gate_0123456789abcdef"));
static_assert(!timeout_table_name("odbcpp_timeout_gate_%"));
static_assert(!timeout_table_name("public_table"));
static_assert(timeout_positive_id("4294967295") == UINT32_MAX);
static_assert(!timeout_positive_id("4294967296") && !timeout_positive_id("0"));
static_assert(timeout_server_milliseconds("3s") == 3000);
static_assert(timeout_server_milliseconds("1000ms") == 1000);
static_assert(!timeout_server_milliseconds("3seconds"));
static_assert(timeout_server_allows_client("0"));
static_assert(timeout_server_allows_client("3000ms"));
static_assert(!timeout_server_allows_client("2999ms"));
static_assert(!timeout_server_allows_client("18446744073709551616"));

// Redshift NAME display values can carry trailing ASCII padding. This is
// fixture identity display only; relation authority also requires the server UID.
constexpr std::string_view timeout_identity_display(std::string_view value) {
    while (!value.empty() && value.back() == ' ') {
        value.remove_suffix(1);
    }
    return value;
}
static_assert(timeout_identity_display("odbcpp_pilot_test   ") == "odbcpp_pilot_test");
static_assert(timeout_identity_display("odbcpp_pilot_other   ") != "odbcpp_pilot_test");
static_assert(timeout_identity_display(" leading ") == " leading");
static_assert(timeout_identity_display("internal space ") == "internal space");
static_assert(timeout_identity_display("tab\t") == "tab\t");
static_assert(timeout_identity_display("name\t ") == "name\t");
static_assert(timeout_identity_display("\xC3\xA9 ") == "\xC3\xA9");
static_assert(timeout_identity_display("").empty());
static_assert(timeout_identity_display("   ").empty());
static_assert([] {
    std::array<char, 127> padded{};
    padded.fill(' ');
    constexpr std::string_view ordinary{"odbcpp_pilot_test"};
    std::copy(ordinary.begin(), ordinary.end(), padded.begin());
    return timeout_identity_display(std::string_view(padded.data(), padded.size())) == ordinary;
}());

class SharedTimeoutBlocker {
public:
    explicit SharedTimeoutBlocker(std::chrono::steady_clock::time_point end) : end_(end) {
        property("timeout_blocker_cleanup_known", "false");
        property("timeout_blocker_lock_ack", "false");
        property("timeout_blocker_rollback_ack", "false");
    }
    ~SharedTimeoutBlocker() {
        try {
            if (!closed_ && known_) (void)finish();
            if (!closed_) property("timeout_fixture_unknown", "true");
        } catch (...) {
            // Initial false cleanup receipt survives diagnostic/allocation failure.
        }
    }
    bool open(SQLHENV env, SQLHSTMT waiter) {
#if defined(_MSC_VER)
        std::array<char, 64> schema_value{}, table_value{};
        // Retain all Windows-valid ASCII OIDs, including leading zeros. The
        // platform environment-value limit is 32767 characters, not ten digits.
        std::array<char, 32768> oid_value{};
        const auto read_public = [](const char* name, auto& value) {
            std::size_t required = 0;
            return ::getenv_s(&required, value.data(), value.size(), name) == 0 &&
                required > 0 && required <= value.size();
        };
        if (!read_public("ODBCPP_REDSHIFT_TIMEOUT_SCHEMA", schema_value) ||
            !read_public("ODBCPP_REDSHIFT_TIMEOUT_TABLE", table_value) ||
            !read_public("ODBCPP_REDSHIFT_TIMEOUT_OID", oid_value)) return fail("public timeout environment read");
        const auto* schema = schema_value.data();
        const auto* table = table_value.data();
        const auto* oid = oid_value.data();
#else
        const auto* schema = std::getenv("ODBCPP_REDSHIFT_TIMEOUT_SCHEMA");
        const auto* table = std::getenv("ODBCPP_REDSHIFT_TIMEOUT_TABLE");
        const auto* oid = std::getenv("ODBCPP_REDSHIFT_TIMEOUT_OID");
#endif
        if (!schema || std::string_view(schema) != "odbcpp_fixture" || !table ||
            !timeout_table_name(table) || !oid || !timeout_positive_id(oid)) return fail("required public schema/table/OID");
        table_ = table;
        oid_ = std::to_string(*timeout_positive_id(oid));
        relation_ = "\"odbcpp_fixture\".\"" + table_ + "\"";
        property("timeout_fixture_schema", schema);
        property("timeout_fixture_table", table_);
        property("timeout_fixture_oid", oid_);
#ifdef ODBCPP_TEST_VIA_DRIVER_MANAGER
        // The verified-TLS check resolves the private DSN through the driver's
        // own parser, which a Driver Manager build cannot link. The blocker is
        // used only for the Redshift product.
        if (fixture_redshift_product()) return fail("verified TLS profile check needs the direct build");
#else
        try {
            const auto resolved = rs::odbc::ConnectionString::resolve(
                odbcpp::test::configured_connection_string(),
                std::string(rs::core::database::configured_backend_provider().identity().driver_name));
            const auto& fields = resolved.effective_parameters;
            if (!fields.contains("SSL") || !fields.contains("SSLCAFILE") || fields.at("SSLCAFILE").empty() ||
                fields.at("SSLCAFILE").find('\0') != std::string::npos) return fail("verified TLS profile required");
            const auto ssl = rs::odbc::ConnectionString::to_upper(rs::odbc::ConnectionString::trim(fields.at("SSL")));
            if (ssl != "1" && ssl != "TRUE" && ssl != "YES" && ssl != "ON") return fail("TLS must be enabled");
        } catch (...) { return fail("private DSN resolution"); }
#endif
        if (!identity(waiter, "timeout_waiter_before") || !server_timeout(waiter, "timeout_waiter_before")) return false;
        if (!accept(::SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc_), "allocate blocker")) return false;
        if (!fixture_connection_budget(end_, dbc_, true)) return fail("blocker login budget");
        known_ = false;
        if (!accept(::SQLConnect(dbc_, odbcpp::test::configured_server_name_data(), SQL_NTS,
                nullptr, 0, nullptr, 0), "blocker login")) return false;
        connected_ = true;
        if (!accept(::SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &stmt_), "allocate blocker statement") ||
            !identity(stmt_, "timeout_blocker") || !server_timeout(stmt_, "timeout_blocker")) return false;
        std::array<std::string, 2> object;
        if (!query(stmt_, "SELECT c.oid, u.usename FROM pg_catalog.pg_class c "
            "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
            "JOIN pg_catalog.pg_user u ON u.usesysid=c.relowner WHERE c.oid=" + oid_ +
            " AND n.nspname='odbcpp_fixture' AND c.relname='" + table_ +
            "' AND c.relkind='r' AND c.relowner=CURRENT_USER_ID") ||
            !row(stmt_, object) || !end_row(stmt_)) return false;
        property("timeout_catalog_oid", object[0]);
        property("timeout_catalog_owner_raw_size", std::to_string(object[1].size()));
        property("timeout_catalog_owner", std::string(timeout_identity_display(object[1])));
        if (object[0] != oid_ || timeout_identity_display(object[1]) != owner_) {
            return fail("ROOT relation identity/ordinary owner mismatch");
        }
        return true;
    }
    bool lock() {
        if (!known_ || !fixture_connection_budget(end_, dbc_, false)) return fail("before lock budget/state");
        if (!accept(::SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT,
                reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_OFF), 0), "disable blocker autocommit")) return false;
        // Arm BEFORE the call that can cause the adapter's implicit bare BEGIN.
        rollback_needed_ = true;
        if (!query(stmt_, "LOCK TABLE " + relation_)) return false;
        property("timeout_blocker_lock_ack", "true");
        std::array<std::string, 1> count;
        if (!query(stmt_, lock_query() + " AND lock_mode='AccessExclusiveLock' AND granted=true") ||
            !row(stmt_, count) || !end_row(stmt_)) return false;
        if (count[0] != "1") return fail("exclusive granted lock witness");
        return true;
    }
    std::string waiter_sql() const { return "SELECT value FROM " + relation_; }
    void waiter_result(bool expected_timeout) {
        property("timeout_waiter_expected_transport_timeout", expected_timeout ? "true" : "false");
        if (!expected_timeout) property("timeout_waiter_completion_unknown", "true");
    }
    bool reconnected(SQLHSTMT waiter) { return identity(waiter, "timeout_waiter_after"); }
    bool finish() {
        if (closed_) return true;
        if (!known_) return false; // No speculative SQL after unknown completion.
        if (rollback_needed_) {
            if (!fixture_connection_budget(end_, dbc_, false)) return fail("rollback remaining budget");
            known_ = false;
            if (!accept(::SQLEndTran(SQL_HANDLE_DBC, dbc_, SQL_ROLLBACK), "blocker rollback")) return false;
            rollback_needed_ = false;
            property("timeout_blocker_rollback_ack", "true");
            if (!accept(::SQLSetConnectAttr(dbc_, SQL_ATTR_AUTOCOMMIT,
                    reinterpret_cast<SQLPOINTER>(SQL_AUTOCOMMIT_ON), 0), "restore blocker autocommit")) return false;
            std::array<std::string, 1> count;
            if (!query(stmt_, lock_query()) || !row(stmt_, count) || !end_row(stmt_)) return false;
            if (count[0] != "0") return fail("blocker lock still present");
        }
        if (stmt_) {
            if (!accept(::SQLFreeHandle(SQL_HANDLE_STMT, stmt_), "free blocker statement")) return false;
            stmt_ = SQL_NULL_HSTMT;
        }
        if (connected_) {
            if (!fixture_connection_budget(end_, dbc_, false)) return fail("disconnect remaining budget");
            known_ = false;
            if (!accept(::SQLDisconnect(dbc_), "disconnect blocker")) return false;
            connected_ = false;
        }
        if (dbc_) {
            if (!accept(::SQLFreeHandle(SQL_HANDLE_DBC, dbc_), "free blocker connection")) return false;
            dbc_ = SQL_NULL_HDBC;
        }
        closed_ = true;
        property("timeout_blocker_cleanup_known", "true");
        return true;
    }
private:
    static void property(const std::string& key, const std::string& value) {
        ::testing::Test::RecordProperty(key, value);
    }
    bool fail(const char* stage) {
        known_ = false;
        property("timeout_fixture_unknown", "true");
        property("timeout_failure_stage", stage);
        ADD_FAILURE() << "Shared timeout fixture stopped at " << stage;
        return false;
    }
    bool accept(SQLRETURN result, const char* stage) {
        if (result != SQL_SUCCESS) return fail(stage);
        known_ = true;
        return true;
    }
    bool query(SQLHSTMT statement, const std::string& sql) {
        if (!known_ || !fixture_statement_budget(end_, statement)) return fail("statement remaining budget/state");
        known_ = false;
        return accept(::SQLExecDirect(statement, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS), "statement completion");
    }
    template<std::size_t N> bool row(SQLHSTMT statement, std::array<std::string, N>& fields) {
        if (!known_ || !fixture_statement_budget(end_, statement)) return fail("row remaining budget/state");
        if (!accept(::SQLFetch(statement), "required row")) return false;
        for (std::size_t i = 0; i < N; ++i) {
            std::array<char, 160> value{};
            SQLLEN length = -1;
            if (!accept(::SQLGetData(statement, static_cast<SQLUSMALLINT>(i + 1), SQL_C_CHAR,
                    value.data(), static_cast<SQLLEN>(value.size()), &length), "identity cell")) return false;
            if (length < 0 || static_cast<std::size_t>(length) >= value.size()) return fail("identity cell width/null");
            fields[i].assign(value.data(), static_cast<std::size_t>(length));
        }
        return true;
    }
    bool end_row(SQLHSTMT statement) {
        if (!known_ || !fixture_statement_budget(end_, statement)) return fail("row end remaining budget/state");
        if (::SQLFetch(statement) != SQL_NO_DATA) return fail("extra row/error");
        return accept(::SQLCloseCursor(statement), "close identity cursor");
    }
    bool identity(SQLHSTMT statement, const std::string& prefix) {
        std::array<std::string, 4> fields;
        if (!query(statement, "SELECT current_user, current_database(), pg_backend_pid(), start_time FROM sys_session_history "
            "WHERE session_id=pg_backend_pid() AND CAST(user_id AS INTEGER)=CURRENT_USER_ID "
            "AND database_name=current_database() AND status='active'") ||
            !row(statement, fields) || !end_row(statement)) return false;
        if (!timeout_positive_id(fields[2]) || fields[3].empty()) return fail("session birth identity");
        property(prefix + "_owner_raw_size", std::to_string(fields[0].size()));
        fields[0] = std::string(timeout_identity_display(fields[0]));
        if (fields[0].empty()) {
            return fail("empty session principal");
        }
        if (owner_.empty()) {
            owner_ = fields[0];
            database_ = fields[1];
        }
        if (fields[0] != owner_ || fields[1] != database_) return fail("session principal/database mismatch");
        for (std::size_t i = 0; i < fields.size(); ++i) {
            constexpr std::array<const char*, 4> suffix{"_owner", "_database", "_pid", "_start"};
            property(prefix + suffix[i], fields[i]);
        }
        if (prefix == "timeout_blocker") pid_ = std::to_string(*timeout_positive_id(fields[2]));
        return true;
    }
    bool server_timeout(SQLHSTMT statement, const std::string& prefix) {
        std::array<std::string, 1> value;
        if (!query(statement, "SHOW statement_timeout") || !row(statement, value) || !end_row(statement)) return false;
        if (!timeout_server_allows_client(value[0])) return fail("server timeout competes with client oracle");
        property(prefix + "_statement_timeout_ms", std::to_string(*timeout_server_milliseconds(value[0])));
        return true;
    }
    std::string lock_query() const {
        return "SELECT COUNT(*) FROM svv_transactions WHERE txn_owner=current_user AND txn_db=current_database() "
            "AND pid=" + pid_ + " AND relation=" + oid_ + " AND lockable_object_type='relation'";
    }
    std::chrono::steady_clock::time_point end_;
    SQLHDBC dbc_{SQL_NULL_HDBC};
    SQLHSTMT stmt_{SQL_NULL_HSTMT};
    bool known_{true}, connected_{false}, rollback_needed_{false}, closed_{false};
    std::string table_, oid_, relation_, owner_, database_, pid_;
};

}  // namespace


// Local, no-I/O fixture boundary proof; live assertions remain in their original cases.
TEST(BindColFixtureBounds, PreservesExplicitShortTimeouts) {
    EXPECT_EQ(1u, fixture_timeout_seconds(40, 1));
    EXPECT_EQ(2u, fixture_timeout_seconds(40, 2));
    EXPECT_EQ(3u, fixture_timeout_seconds(40, 3));
    EXPECT_EQ(1u, fixture_timeout_seconds(1, 3));
    EXPECT_EQ(0u, fixture_timeout_seconds(0, 1));
}

class BindColIntegrationTest : public ::testing::Test {
protected:
    const std::chrono::steady_clock::time_point fixture_end_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(40);

    // Fixture-local forwards cover repetitions, reconnects and sibling handles.
    // The original assertions/arguments below still call the actual public API.
    SQLRETURN SQLConnect(SQLHDBC connection, SQLCHAR* server_name, SQLSMALLINT name_length1,
        SQLCHAR* user_name, SQLSMALLINT name_length2, SQLCHAR* authentication,
        SQLSMALLINT name_length3) {
      return fixture_connection_budget(fixture_end_, connection, true)
          ? ::SQLConnect(connection, server_name, name_length1, user_name, name_length2,
              authentication, name_length3) : SQL_ERROR;
    }
    SQLRETURN SQLDescribeCol(SQLHSTMT statement, SQLUSMALLINT column_number, SQLCHAR* column_name,
        SQLSMALLINT name_buffer_length, SQLSMALLINT* name_length, SQLSMALLINT* data_type,
        SQLULEN* column_size, SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLDescribeCol(statement, column_number, column_name, name_buffer_length,
              name_length, data_type, column_size, decimal_digits, nullable) : SQL_ERROR;
    }
    SQLRETURN SQLExecDirect(SQLHSTMT statement, SQLCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecDirect(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLExecDirectW(SQLHSTMT statement, SQLWCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecDirectW(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLFetch(SQLHSTMT statement) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLFetch(statement) : SQL_ERROR;
    }
    // iODBC 3.52.16 rejects SQL_ARD_TYPE in SQLGetData without calling the
    // driver (GetDataArdTypeRejected, bucket D). Asserts that and reports
    // whether the case must end, since the rest needs the ARD-typed value.
    bool ard_type_get_data_rejected(SQLUSMALLINT column, SQLPOINTER target,
        SQLLEN buffer_length, SQLLEN* indicator) {
      if (!odbcpp::test::dm::stops_case(
              odbcpp::test::dm::Deviation::GetDataArdTypeRejected)) {
        return false;
      }
      EXPECT_EQ(SQL_ERROR, ::SQLGetData(hstmt, column, SQL_ARD_TYPE, target,
          buffer_length, indicator));
      SQLCHAR state[6]{};
      EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state,
          nullptr, nullptr, 0, nullptr));
      EXPECT_STREQ("HY003", reinterpret_cast<char*>(state));
      return true;
    }
    // Manager builds check NULL-buffer diagnostics against the expectation
    // table and still return the record; direct builds call the driver.
    SQLRETURN SQLGetDiagRec(SQLSMALLINT type, SQLHANDLE handle, SQLSMALLINT record,
        SQLCHAR* state, SQLINTEGER* native, SQLCHAR* message, SQLSMALLINT length,
        SQLSMALLINT* text_length) {
      return odbcpp::test::diag_rec(type, handle, record, state, native, message,
          length, text_length);
    }

    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        
        SQLRETURN ret = SQLConnect(hdbc, odbcpp::test::configured_server_name_data(), SQL_NTS, nullptr, 0, nullptr, 0);
        if (ret != SQL_SUCCESS) {
            GTEST_SKIP() << "Database connection failed - skipping integration tests";
        }
        
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    }
    
    void TearDown() override {
        if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        if (hdbc) {
            SQLDisconnect(hdbc);
            SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        }
        if (henv) SQLFreeHandle(SQL_HANDLE_ENV, henv);
    }
    
    SQLHENV henv = nullptr;
    SQLHDBC hdbc = nullptr;
    SQLHSTMT hstmt = nullptr;
};

TEST_F(BindColIntegrationTest, QueryTimeoutUsesTransportDeadline) {
    if (fixture_redshift_product()) {
        SharedTimeoutBlocker blocker(fixture_end_);
        ASSERT_TRUE(blocker.open(henv, hstmt));
        ASSERT_TRUE(blocker.lock());
        ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT,
            reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(1)), 0));
        const auto sql = blocker.waiter_sql();
        const auto started = std::chrono::steady_clock::now();
        const auto result = SQLExecDirect(hstmt,
            reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        SQLCHAR sqlstate[6]{};
        const auto diagnostic = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            sqlstate, nullptr, nullptr, 0, nullptr);
        const bool expected = result == SQL_ERROR && diagnostic == SQL_SUCCESS &&
            std::strcmp(reinterpret_cast<char*>(sqlstate), "HYT00") == 0 &&
            elapsed < std::chrono::seconds(3);
        blocker.waiter_result(expected);
        EXPECT_EQ(SQL_ERROR, result);
        EXPECT_LT(elapsed, std::chrono::seconds(3));
        EXPECT_EQ(SQL_SUCCESS, diagnostic);
        EXPECT_STREQ("HYT00", reinterpret_cast<char*>(sqlstate));
        ASSERT_TRUE(blocker.finish());
        ASSERT_TRUE(expected);
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_QUERY_TIMEOUT,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(1)), 0));

    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT pg_sleep(3)", SQL_NTS));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(elapsed, std::chrono::seconds(3));

    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HYT00", reinterpret_cast<char*>(sqlstate));
}

// Test basic column binding with different data types
TEST_F(BindColIntegrationTest, BasicColumnBinding) {
    // Execute query with known data types
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Hello' as str_col, 123 as int_col, 45.67 as double_col", SQL_NTS));
    
    // Bind columns to variables
    char str_val[256];
    SQLINTEGER int_val;
    SQLDOUBLE double_val;
    SQLLEN str_len, int_len, double_len;
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, str_val, sizeof(str_val), &str_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_SLONG, &int_val, 0, &int_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_DOUBLE, &double_val, 0, &double_len));
    
    // Fetch should auto-populate bound columns
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // Verify bound data
    EXPECT_STREQ("Hello", str_val);
    EXPECT_EQ(123, int_val);
    EXPECT_DOUBLE_EQ(45.67, double_val);
}

TEST_F(BindColIntegrationTest, WideBoundColumnHandlesUnalignedBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'AB'::text", SQL_NTS));

    alignas(SQLWCHAR) std::array<std::byte, 1 + 3 * sizeof(SQLWCHAR)> storage{};
    void* output = storage.data() + 1;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_WCHAR, output, 3 * sizeof(SQLWCHAR), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(2 * static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);

    SQLWCHAR units[3]{};
    std::memcpy(units, output, sizeof(units));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), units[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('B'), units[1]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), units[2]);
}

TEST_F(BindColIntegrationTest, NumericBoundColumnsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 42::integer, 1.5::double precision", SQL_NTS));

    alignas(SQLINTEGER)
        std::array<std::byte, 1 + sizeof(SQLINTEGER)> integer_bytes{};
    alignas(SQLDOUBLE)
        std::array<std::byte, 1 + sizeof(SQLDOUBLE)> double_bytes{};
    SQLLEN integer_length = -1;
    SQLLEN double_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SLONG,
        integer_bytes.data() + 1, sizeof(SQLINTEGER), &integer_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_DOUBLE,
        double_bytes.data() + 1, sizeof(SQLDOUBLE), &double_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)), integer_length);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLDOUBLE)), double_length);

    SQLINTEGER integer = 0;
    SQLDOUBLE floating = 0;
    std::memcpy(&integer, integer_bytes.data() + 1, sizeof(integer));
    std::memcpy(&floating, double_bytes.data() + 1, sizeof(floating));
    EXPECT_EQ(42, integer);
    EXPECT_DOUBLE_EQ(1.5, floating);
}

TEST_F(BindColIntegrationTest, DateTimeBoundColumnsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56', "
                  "TIMESTAMP '2024-02-29 12:34:56'", SQL_NTS));

    alignas(SQL_DATE_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_DATE_STRUCT)> date_bytes{};
    alignas(SQL_TIME_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_TIME_STRUCT)> time_bytes{};
    alignas(SQL_TIMESTAMP_STRUCT)
        std::array<std::byte, 1 + sizeof(SQL_TIMESTAMP_STRUCT)> timestamp_bytes{};
    SQLLEN lengths[3]{-1, -1, -1};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_DATE,
        date_bytes.data() + 1, sizeof(SQL_DATE_STRUCT), &lengths[0]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_TIME,
        time_bytes.data() + 1, sizeof(SQL_TIME_STRUCT), &lengths[1]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_TIMESTAMP,
        timestamp_bytes.data() + 1, sizeof(SQL_TIMESTAMP_STRUCT), &lengths[2]));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)), lengths[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIME_STRUCT)), lengths[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_TIMESTAMP_STRUCT)), lengths[2]);
    SQL_DATE_STRUCT date{};
    SQL_TIME_STRUCT time{};
    SQL_TIMESTAMP_STRUCT timestamp{};
    std::memcpy(&date, date_bytes.data() + 1, sizeof(date));
    std::memcpy(&time, time_bytes.data() + 1, sizeof(time));
    std::memcpy(&timestamp, timestamp_bytes.data() + 1, sizeof(timestamp));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(34, time.minute);
    EXPECT_EQ(56, time.second);
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
}

TEST_F(BindColIntegrationTest, Odbc3TemporalBoundColumns) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56', "
                  "TIMESTAMP '2024-02-29 12:34:56.123456'", SQL_NTS));

    SQL_DATE_STRUCT date{};
    SQL_TIME_STRUCT time{};
    SQL_TIMESTAMP_STRUCT timestamp{};
    SQLLEN lengths[3]{-1, -1, -1};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &lengths[0]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_TYPE_TIME,
        &time, sizeof(time), &lengths[1]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), &lengths[2]));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), lengths[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(time)), lengths[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), lengths[2]);
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(34, time.minute);
    EXPECT_EQ(56, time.second);
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
    EXPECT_EQ(123456000u, timestamp.fraction);
}

TEST_F(BindColIntegrationTest, Odbc3TemporalGetDataAndInvalidValues) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56', "
                  "TIMESTAMP '2024-02-29 12:34:56.123456', "
                  "'2024-02-30'::text, '25:00:00'::text, 'bad'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_DATE_STRUCT date{};
    SQL_TIME_STRUCT time{};
    SQL_TIMESTAMP_STRUCT timestamp{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), length);
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(29, date.day);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_TYPE_TIME,
        &time, sizeof(time), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(time)), length);
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(56, time.second);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), length);
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(123456000u, timestamp.fraction);

    SQLCHAR state[6]{};
    date.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 4, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(73, date.year);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));

    time.hour = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 5, SQL_C_TYPE_TIME,
        &time, sizeof(time), &length));
    EXPECT_EQ(73, time.hour);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));

    timestamp.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 6, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(73, timestamp.year);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, ApplicationDescriptorDrivesFetchBinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'descriptor'::text", SQL_NTS));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &descriptor));
    char value[32]{};
    SQLLEN indicator = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_C_CHAR}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_OCTET_LENGTH,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{sizeof(value)}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_DATA_PTR, value, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_INDICATOR_PTR, &indicator, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 1, SQL_DESC_OCTET_LENGTH_PTR, &indicator, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_ROW_DESC, descriptor, 0));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_STREQ("descriptor", value);
    EXPECT_EQ(10, indicator);

    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

TEST_F(BindColIntegrationTest, FailedBoundConversionKeepsSeparateIndicator) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'not-a-date'::text", SQL_NTS));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &descriptor));
    SQL_DATE_STRUCT date{4242, 4, 2};
    SQLLEN octet_length = 91;
    SQLLEN indicator = 92;
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescRec(descriptor, 1,
        SQL_C_TYPE_DATE, 0, sizeof(date), 0, 0,
        &date, &octet_length, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_ROW_DESC, descriptor, 0));

    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4242, date.year);
    EXPECT_EQ(91, octet_length);
    EXPECT_EQ(92, indicator);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), octet_length);
    EXPECT_EQ(0, indicator);

    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

TEST_F(BindColIntegrationTest,
       SupportsAttachedColumnRowArraysBeforeFetch) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS));
    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &descriptor));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 0, SQL_DESC_ARRAY_SIZE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_ROW_DESC, descriptor, 0));
    SQLINTEGER values[2]{4242, 4343};
    SQLLEN lengths[2]{91, 92};
    SQLULEN fetched = 91;
    SQLUSMALLINT statuses[2]{SQL_ROW_ERROR, SQL_ROW_ERROR};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_LONG, values, sizeof(values[0]), lengths));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROW_STATUS_PTR, statuses, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(1, values[0]);
    EXPECT_EQ(4343, values[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLINTEGER)), lengths[0]);
    EXPECT_EQ(92, lengths[1]);
    EXPECT_EQ(static_cast<SQLULEN>(1), fetched);
    EXPECT_EQ(SQL_ROW_SUCCESS, statuses[0]);
    EXPECT_EQ(SQL_ROW_NOROW, statuses[1]);
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 0, SQL_DESC_ARRAY_SIZE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{1}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

// Test column binding with NULL values
TEST_F(BindColIntegrationTest, NullValueBinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT NULL as null_col, 'NotNull' as str_col", SQL_NTS));
    
    char null_val[256];
    char str_val[256];
    SQLLEN null_len, str_len;
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, null_val, sizeof(null_val), &null_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_CHAR, str_val, sizeof(str_val), &str_len));
    
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // NULL column should have SQL_NULL_DATA indicator
    EXPECT_EQ(SQL_NULL_DATA, null_len);
    EXPECT_STREQ("NotNull", str_val);
}

TEST_F(BindColIntegrationTest, BoundNullAndEmptyStringRemainDistinct) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text, ''::text", SQL_NTS));

    char null_value[16] = "unchanged";
    char empty_value[16] = "unchanged";
    SQLLEN null_indicator = 99;
    SQLLEN empty_indicator = 99;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, null_value, sizeof(null_value), &null_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 2, SQL_C_CHAR, empty_value, sizeof(empty_value), &empty_indicator));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NULL_DATA, null_indicator);
    EXPECT_STREQ("unchanged", null_value);
    EXPECT_EQ(0, empty_indicator);
    EXPECT_STREQ("", empty_value);
}

TEST_F(BindColIntegrationTest, GetDataReportsNullWithoutTouchingBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text, ''::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char null_value[16] = "unchanged";
    char empty_value[16] = "unchanged";
    SQLLEN null_indicator = 99;
    SQLLEN empty_indicator = 99;
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, null_value, sizeof(null_value), &null_indicator));
    EXPECT_EQ(SQL_NULL_DATA, null_indicator);
    EXPECT_STREQ("unchanged", null_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, empty_value, sizeof(empty_value), &empty_indicator));
    EXPECT_EQ(0, empty_indicator);
    EXPECT_STREQ("", empty_value);
}

TEST_F(BindColIntegrationTest, GetDataSupportsAnyColumnOrder) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'first'::text, 'second'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char second[16]{};
    char first[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, second, sizeof(second), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, first, sizeof(first), nullptr));
    EXPECT_STREQ("second", second);
    EXPECT_STREQ("first", first);
}

TEST_F(BindColIntegrationTest, NullWithoutIndicatorReturns22002) {
    SQLUSMALLINT row_status = SQL_ROW_SUCCESS;
    SQLULEN rows_fetched = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROW_STATUS_PTR, &row_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text", SQL_NTS));

    char value[16] = "unchanged";
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, value, sizeof(value), nullptr));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_STREQ("unchanged", value);
    EXPECT_EQ(1u, rows_fetched);
    EXPECT_EQ(SQL_ROW_ERROR, row_status);

    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22002", reinterpret_cast<char*>(sqlstate));
}

TEST_F(BindColIntegrationTest, ReportsSingleRowFetchStatus) {
    SQLUSMALLINT row_status = SQL_ROW_NOROW;
    SQLULEN rows_fetched = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROW_STATUS_PTR, &row_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROWS_FETCHED_PTR, &rows_fetched, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT value FROM (SELECT 1 AS ord, 'abcdef'::text AS value UNION ALL SELECT 2, 'xy'::text) rows ORDER BY ord", SQL_NTS));

    char value[4]{};
    SQLLEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, value, sizeof(value), &length));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_EQ(1u, rows_fetched);
    EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO, row_status);
    EXPECT_STREQ("abc", value);

    EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(1u, rows_fetched);
    EXPECT_EQ(SQL_ROW_SUCCESS, row_status);
    EXPECT_STREQ("xy", value);

    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    EXPECT_EQ(0u, rows_fetched);
    EXPECT_EQ(SQL_ROW_NOROW, row_status);
}

TEST_F(BindColIntegrationTest, RowStatusOutputsHandleUnalignedBuffers) {
    alignas(SQLULEN) std::array<std::byte, 1 + sizeof(SQLULEN)> fetched{};
    alignas(SQLUSMALLINT)
    std::array<std::byte, 1 + sizeof(SQLUSMALLINT)> status{};
    auto* fetched_output = reinterpret_cast<SQLULEN*>(fetched.data() + 1);
    auto* status_output = reinterpret_cast<SQLUSMALLINT*>(status.data() + 1);
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROWS_FETCHED_PTR, fetched_output, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_ROW_STATUS_PTR, status_output, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT value FROM (SELECT 1 AS position, 'abcdef'::text AS value UNION ALL SELECT 2, 'xy'::text UNION ALL SELECT 3, NULL::text) rows ORDER BY position",
        SQL_NTS));

    char value[4]{};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, value, sizeof(value), nullptr));
    const auto expect_fetch = [&](SQLRETURN expected_result,
                                  SQLULEN expected_count,
                                  SQLUSMALLINT expected_status) {
        fetched.fill(std::byte{0x5a});
        status.fill(std::byte{0x5a});
        EXPECT_EQ(expected_result, SQLFetch(hstmt));
        SQLULEN copied_count = 0;
        SQLUSMALLINT copied_status = 0;
        std::memcpy(&copied_count, fetched.data() + 1,
                    sizeof(copied_count));
        std::memcpy(&copied_status, status.data() + 1,
                    sizeof(copied_status));
        EXPECT_EQ(expected_count, copied_count);
        EXPECT_EQ(expected_status, copied_status);
    };
    expect_fetch(SQL_SUCCESS_WITH_INFO, 1, SQL_ROW_SUCCESS_WITH_INFO);
    EXPECT_STREQ("abc", value);
    expect_fetch(SQL_SUCCESS, 1, SQL_ROW_SUCCESS);
    EXPECT_STREQ("xy", value);
    expect_fetch(SQL_ERROR, 1, SQL_ROW_ERROR);
    expect_fetch(SQL_NO_DATA, 0, SQL_ROW_NOROW);
}

TEST_F(BindColIntegrationTest, LengthIndicatorsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text, 'abcd'::text", SQL_NTS));

    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> null_bound{};
    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> text_bound{};
    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> get_data{};
    auto* null_indicator = reinterpret_cast<SQLLEN*>(null_bound.data() + 1);
    auto* text_indicator = reinterpret_cast<SQLLEN*>(text_bound.data() + 1);
    auto* data_indicator = reinterpret_cast<SQLLEN*>(get_data.data() + 1);
    const auto read_indicator = [](const auto& storage) {
        SQLLEN value = 0;
        std::memcpy(&value, storage.data() + 1, sizeof(value));
        return value;
    };

    get_data.fill(std::byte{0x5a});
    char buffer[8]{};
    const auto previous_output = get_data;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), data_indicator));
    EXPECT_EQ(previous_output, get_data);

    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), null_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 2, SQL_C_CHAR, buffer, sizeof(buffer), text_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NULL_DATA, read_indicator(null_bound));
    EXPECT_EQ(4, read_indicator(text_bound));
    EXPECT_STREQ("abcd", buffer);

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), data_indicator));
    EXPECT_EQ(SQL_NULL_DATA, read_indicator(get_data));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, buffer, sizeof(buffer), data_indicator));
    EXPECT_EQ(4, read_indicator(get_data));
    EXPECT_STREQ("abcd", buffer);
}

TEST_F(BindColIntegrationTest, GetDataNullWithoutIndicatorReturns22002) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT NULL::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char value[16] = "unchanged";
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_CHAR, value, sizeof(value), nullptr));
    EXPECT_STREQ("unchanged", value);

    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22002", reinterpret_cast<char*>(sqlstate));
}

TEST_F(BindColIntegrationTest, GetDataRetrievesLongTextInChunks) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'abcdefghijklmnopqrstuvwxyz'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    const std::vector<std::string> expected_chunks{
        "abcde", "fghij", "klmno", "pqrst", "uvwxy", "z"};
    const std::vector<SQLLEN> expected_remaining{26, 21, 16, 11, 6, 1};
    std::string assembled;
    for (std::size_t i = 0; i < expected_chunks.size(); ++i) {
        char chunk[6]{};
        SQLLEN remaining = 0;
        const auto result = SQLGetData(
            hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining);
        EXPECT_EQ(i + 1 == expected_chunks.size()
                      ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO,
                  result);
        EXPECT_EQ(expected_remaining[i], remaining);
        EXPECT_EQ(expected_chunks[i], chunk);
        assembled += chunk;

        if (result == SQL_SUCCESS_WITH_INFO) {
            SQLCHAR sqlstate[6]{};
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
                SQL_HANDLE_STMT, hstmt, 1, sqlstate,
                nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("01004", reinterpret_cast<char*>(sqlstate));
        }
    }
    EXPECT_EQ("abcdefghijklmnopqrstuvwxyz", assembled);

    char exhausted[6] = "keep";
    SQLLEN exhausted_length = 99;
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 1, SQL_C_CHAR, exhausted, sizeof(exhausted),
        &exhausted_length));
    EXPECT_STREQ("keep", exhausted);
    EXPECT_EQ(99, exhausted_length);
}

TEST_F(BindColIntegrationTest, GetDataHonorsNarrowBufferBoundaries) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT ''::text, 'abc'::text, 'abc'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char value[4] = {'x', 'x', 'x', 'x'};
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, value, 0, &length));
    EXPECT_EQ('x', value[0]);
    EXPECT_EQ(0, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, value, 1, &length));
    EXPECT_EQ('\0', value[0]);
    EXPECT_EQ(0, length);

    value[0] = 'x';
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 2, SQL_C_CHAR, value, 1, &length));
    EXPECT_EQ('\0', value[0]);
    EXPECT_EQ(3, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, value, sizeof(value), &length));
    EXPECT_STREQ("abc", value);
    EXPECT_EQ(3, length);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_CHAR, value, 3, &length));
    EXPECT_STREQ("ab", value);
    EXPECT_EQ(3, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_CHAR, value, 3, &length));
    EXPECT_STREQ("c", value);
    EXPECT_EQ(1, length);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 3, SQL_C_CHAR, value, sizeof(value), &length));
}

TEST_F(BindColIntegrationTest, GetDataHonorsWideBufferBoundaries) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT ''::text, 'A'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLWCHAR value[2]{static_cast<SQLWCHAR>('x'),
                      static_cast<SQLWCHAR>('x')};
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_WCHAR, value, sizeof(SQLWCHAR) - 1, &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), value[0]);
    EXPECT_EQ(0, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_WCHAR, value, sizeof(SQLWCHAR), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>(0), value[0]);
    EXPECT_EQ(0, length);

    value[0] = static_cast<SQLWCHAR>('x');
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 2, SQL_C_WCHAR, value, sizeof(SQLWCHAR), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>(0), value[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_WCHAR, value, sizeof(value), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), value[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), value[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
}

TEST_F(BindColIntegrationTest, BoundCharacterBuffersRequireTerminatorSpace) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT ''::text, ''::text", SQL_NTS));

    char narrow = 'x';
    SQLWCHAR wide = static_cast<SQLWCHAR>('x');
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, &narrow, 0, &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 2, SQL_C_WCHAR, &wide, sizeof(SQLWCHAR) - 1, &wide_length));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_EQ('x', narrow);
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide);
    EXPECT_EQ(0, narrow_length);
    EXPECT_EQ(0, wide_length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, GetDataHonorsBinaryBufferBoundaries) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)(fixture_redshift_product()
            ? "SELECT FROM_HEX(''), FROM_HEX('aa')"
            : "SELECT decode('', 'hex'), decode('aa', 'hex')"),
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    unsigned char value = 0x55;
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_BINARY, &value, 0, &length));
    EXPECT_EQ(0x55, value);
    EXPECT_EQ(0, length);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 1, SQL_C_BINARY, &value, 0, &length));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 2, SQL_C_BINARY, &value, 0, &length));
    EXPECT_EQ(0x55, value);
    EXPECT_EQ(1, length);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_BINARY, &value, 1, &length));
    EXPECT_EQ(0xaa, value);
    EXPECT_EQ(1, length);
}

TEST_F(BindColIntegrationTest,
       GetDataFormatsBinaryAsCompleteHexPairs) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)(fixture_redshift_product()
            ? "SELECT FROM_HEX('00ff7f'), FROM_HEX('00ff7f'), FROM_HEX('')"
            : "SELECT decode('00ff7f', 'hex'), decode('00ff7f', 'hex'), decode('', 'hex')"),
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLLEN remaining = -1;
    SQLCHAR state[6]{};

    char narrow_tiny[2]{'x', 'x'};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &remaining));
    EXPECT_EQ(0, narrow_tiny[0]);
    EXPECT_EQ(6, remaining);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    SQLLEN expected_remaining = 6;
    for (const auto* expected : {"00", "ff", "7f"}) {
        char chunk[4]{};
        const auto result = SQLGetData(hstmt, 1, SQL_C_CHAR,
            chunk, sizeof(chunk), &remaining);
        EXPECT_EQ(std::string_view(expected) == "7f"
                      ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO, result);
        EXPECT_EQ(expected_remaining, remaining);
        EXPECT_STREQ(expected, chunk);
        expected_remaining -= 2;
    }
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &remaining));

    SQLWCHAR wide_tiny[2]{'x', 'x'};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &remaining));
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide_tiny[0]);
    EXPECT_EQ(static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)), remaining);
    expected_remaining = 6 * sizeof(SQLWCHAR);
    for (const auto* expected : {"00", "ff", "7f"}) {
        SQLWCHAR chunk[4]{};
        const auto result = SQLGetData(hstmt, 2, SQL_C_WCHAR,
            chunk, sizeof(chunk), &remaining);
        EXPECT_EQ(std::string_view(expected) == "7f"
                      ? SQL_SUCCESS : SQL_SUCCESS_WITH_INFO, result);
        EXPECT_EQ(expected_remaining, remaining);
        EXPECT_EQ(static_cast<SQLWCHAR>(expected[0]), chunk[0]);
        EXPECT_EQ(static_cast<SQLWCHAR>(expected[1]), chunk[1]);
        EXPECT_EQ(static_cast<SQLWCHAR>(0), chunk[2]);
        expected_remaining -= 2 * sizeof(SQLWCHAR);
    }
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &remaining));

    char empty[2]{'x', 'x'};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_CHAR,
        empty, sizeof(empty), &remaining));
    EXPECT_EQ(0, empty[0]);
    EXPECT_EQ(0, remaining);
}

TEST_F(BindColIntegrationTest,
       BoundBinaryTextPreservesCompleteHexPairs) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)(fixture_redshift_product()
            ? "SELECT FROM_HEX('00ff7f'), FROM_HEX('00ff7f'), FROM_HEX(''), FROM_HEX('00ff7f')"
            : "SELECT decode('00ff7f', 'hex'), decode('00ff7f', 'hex'), decode('', 'hex'), decode('00ff7f', 'hex')"),
        SQL_NTS));

    char narrow[4]{'x', 'x', 'x', 'x'};
    SQLWCHAR wide[4]{'x', 'x', 'x', 'x'};
    char empty[2]{'x', 'x'};
    char tiny[2]{'x', 'x'};
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = -1;
    SQLLEN empty_length = -1;
    SQLLEN tiny_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &wide_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_CHAR,
        empty, sizeof(empty), &empty_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 4, SQL_C_CHAR,
        tiny, sizeof(tiny), &tiny_length));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_STREQ("00", narrow);
    EXPECT_EQ(6, narrow_length);
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide[1]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[2]);
    // iODBC 3.52.16 replaces the driver's total with the bytes it copied.
    EXPECT_EQ(odbcpp::test::dm::value(
                  odbcpp::test::dm::Deviation::BoundWideIndicatorIsCopiedBytes,
                  static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)),
                  static_cast<SQLLEN>(2 * sizeof(SQLWCHAR))),
              wide_length);
    EXPECT_EQ(0, empty[0]);
    EXPECT_EQ(0, empty_length);
    EXPECT_EQ(0, tiny[0]);
    EXPECT_EQ(6, tiny_length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BoundBinaryTextDecodesLegacyByteaOutput) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SET bytea_output = 'escape'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT decode('00ff7f', 'hex'), "
                  "decode('00ff7f', 'hex')", SQL_NTS));

    char narrow[7]{};
    SQLWCHAR wide[7]{};
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &wide_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_STREQ("00ff7f", narrow);
    EXPECT_EQ(6, narrow_length);
    for (std::size_t i = 0; i < 6; ++i) {
        EXPECT_EQ(static_cast<SQLWCHAR>(narrow[i]), wide[i]);
    }
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[6]);
    EXPECT_EQ(static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)), wide_length);
}

TEST_F(BindColIntegrationTest, BitToCharacterGetDataUsesZeroAndOne) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true, false, true, false, true, false",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLLEN length = -1;
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        char value[2]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_CHAR,
            value, sizeof(value), &length));
        EXPECT_STREQ(column == 1 ? "1" : "0", value);
        EXPECT_EQ(1, length);
    }
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        SQLWCHAR value[2]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_WCHAR,
            value, sizeof(value), &length));
        EXPECT_EQ(static_cast<SQLWCHAR>(column == 3 ? '1' : '0'), value[0]);
        EXPECT_EQ(static_cast<SQLWCHAR>(0), value[1]);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
    }

    char narrow_tiny = 'x';
    length = 77;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 5, SQL_C_CHAR,
        &narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny);
    EXPECT_EQ(77, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow_retry[2]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, SQL_C_CHAR,
        narrow_retry, sizeof(narrow_retry), &length));
    EXPECT_STREQ("1", narrow_retry);

    SQLWCHAR wide_tiny = 'x';
    length = 78;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 6, SQL_C_WCHAR,
        &wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny);
    EXPECT_EQ(78, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide_retry[2]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_WCHAR,
        wide_retry, sizeof(wide_retry), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide_retry[0]);
}

TEST_F(BindColIntegrationTest, BitToCharacterBoundColumnsUseZeroAndOne) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true, false", SQL_NTS));
    char narrow[2]{};
    SQLWCHAR wide[2]{};
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &wide_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_STREQ("1", narrow);
    EXPECT_EQ(1, narrow_length);
    EXPECT_EQ(static_cast<SQLWCHAR>('0'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), wide_length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true", SQL_NTS));
    char tiny = 'x';
    SQLLEN tiny_length = 79;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        &tiny, sizeof(tiny), &tiny_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', tiny);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT false", SQL_NTS));
    SQLWCHAR wide_tiny = 'x';
    SQLLEN wide_tiny_length = 80;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_WCHAR,
        &wide_tiny, sizeof(wide_tiny), &wide_tiny_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BitToBinaryGetDataReturnsOneByte) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true, false, true", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLLEN length = -1;
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        SQLCHAR value = 0xff;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_BINARY,
            &value, sizeof(value), &length));
        EXPECT_EQ(column == 1 ? 1 : 0, value);
        EXPECT_EQ(1, length);
    }

    SQLCHAR tiny = 0x55;
    length = 81;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_BINARY,
        &tiny, 0, &length));
    EXPECT_EQ(0x55, tiny);
    EXPECT_EQ(81, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_BINARY,
        &tiny, sizeof(tiny), &length));
    EXPECT_EQ(1, tiny);
    EXPECT_EQ(1, length);
}

TEST_F(BindColIntegrationTest, BitToBinaryBoundColumnsReturnOneByte) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT false, true", SQL_NTS));
    SQLCHAR first = 0xff;
    SQLCHAR second = 0xff;
    SQLLEN first_length = -1;
    SQLLEN second_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BINARY,
        &first, sizeof(first), &first_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_BINARY,
        &second, sizeof(second), &second_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0, first);
    EXPECT_EQ(1, second);
    EXPECT_EQ(1, first_length);
    EXPECT_EQ(1, second_length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true", SQL_NTS));
    SQLCHAR tiny = 0x55;
    SQLLEN tiny_length = 82;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BINARY,
        &tiny, 0, &tiny_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(0x55, tiny);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BitToNumericGetDataUsesZeroAndOne) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true, true, true, true, true", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLSMALLINT short_value = -1;
    SQLINTEGER integer = -1;
    SQLBIGINT big = -1;
    SQLREAL real = -1;
    SQLDOUBLE double_value = -1;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_SSHORT,
        &short_value, 0, &length));
    EXPECT_EQ(1, short_value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(short_value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_SLONG,
        &integer, 0, &length));
    EXPECT_EQ(1, integer);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_SBIGINT,
        &big, 0, &length));
    EXPECT_EQ(1, big);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(big)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_FLOAT,
        &real, 0, &length));
    EXPECT_FLOAT_EQ(1.0f, real);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(real)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, SQL_C_DOUBLE,
        &double_value, 0, &length));
    EXPECT_DOUBLE_EQ(1.0, double_value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(double_value)), length);
}

TEST_F(BindColIntegrationTest, SignedTinyintGetDataChecksRangeAndFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT -128::numeric(3,0), 127::numeric(3,0), -129::numeric(3,0), "
                  "128::numeric(3,0), 12.75::numeric(4,2), true, false", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLSCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(-128, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(127, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_STINYINT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(12, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(1, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_STINYINT,
        &value, 0, &length));
    EXPECT_EQ(0, value);
}

TEST_F(BindColIntegrationTest, SignedTinyintBoundColumnHandlesNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 127::numeric UNION ALL SELECT NULL::numeric "
                  "ORDER BY 1 NULLS LAST", SQL_NTS));
    SQLSCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_STINYINT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(127, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(127, value);
    EXPECT_EQ(SQL_NULL_DATA, length);
}

TEST_F(BindColIntegrationTest, SignedTinyintBoundColumnRejectsOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 127::numeric UNION ALL SELECT 128::numeric "
                  "ORDER BY 1", SQL_NTS));
    SQLSCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_STINYINT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(127, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(127, value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, SignedTinyintRejectsTemporalResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-01-01', NULL::date", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLSCHAR value = 44;
    SQLLEN length = 91;
    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_STINYINT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }
    SQL_DATE_STRUCT date{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(2024, date.year);
}

TEST_F(BindColIntegrationTest, UnsignedTinyintGetDataChecksRangeAndFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 0::numeric(1,0), 255::numeric(3,0), -1::numeric(1,0), "
                  "256::numeric(3,0), 255.75::numeric(5,2), true, false", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(0, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(255, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_UTINYINT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(255, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(1, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(0, value);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'not-a-number'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    value = 44;
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_UTINYINT,
        &value, 0, &length));
    EXPECT_EQ(44, value);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
    char recovered[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        recovered, sizeof(recovered), nullptr));
    EXPECT_STREQ("not-a-number", recovered);
}

TEST_F(BindColIntegrationTest, UnsignedTinyintBoundColumnChecksNullAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (SELECT 1 AS ord, 255::numeric(3,0) AS value UNION ALL SELECT 2, NULL::numeric(3,0) UNION ALL SELECT 3, 256::numeric(3,0)) v ORDER BY ord", SQL_NTS));
    SQLCHAR value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_UTINYINT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(255, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(255, value);
    EXPECT_EQ(SQL_NULL_DATA, length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(255, value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, UnsignedTinyintRejectsTemporalResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-01-01', NULL::date", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLCHAR value = 44;
    SQLLEN length = 91;
    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_UTINYINT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }
}

TEST_F(BindColIntegrationTest, UnsignedShortGetDataChecksRangeAndFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 0::numeric(1,0), 65535::numeric(5,0), -1::numeric(1,0), "
                  "65536::numeric(5,0), 65535.75::numeric(7,2), true, false",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLUSMALLINT value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(0, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(65535, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_USHORT,
            &value, 0, &length));
        EXPECT_EQ(44, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(65535, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(1, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_USHORT,
        &value, 0, &length));
    EXPECT_EQ(0, value);
}

TEST_F(BindColIntegrationTest, UnsignedShortBoundColumnChecksNullAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (SELECT 1 AS ord, 65535::numeric(5,0) AS value UNION ALL SELECT 2, NULL::numeric(5,0) UNION ALL SELECT 3, 65536::numeric(5,0)) v ORDER BY ord", SQL_NTS));
    SQLUSMALLINT value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_USHORT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(65535, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(65535, value);
    EXPECT_EQ(SQL_NULL_DATA, length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(65535, value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, UnsignedWideTargetsRejectTemporalResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-01-01', NULL::date", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLUBIGINT value = 44;
    SQLLEN length = 91;
    SQLCHAR state[6]{};
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{SQL_C_USHORT, SQL_C_ULONG, SQL_C_UBIGINT}) {
        SCOPED_TRACE(target);
        for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
            EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, target,
                &value, 0, &length));
            EXPECT_EQ(44u, value);
            EXPECT_EQ(91, length);
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
        }
    }
}

TEST_F(BindColIntegrationTest, UnsignedLongGetDataChecksRangeAndFraction) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 0::numeric(1,0), 4294967295::numeric(10,0), -1::numeric(1,0), "
                  "4294967296::numeric(10,0), 4294967295.75::numeric(12,2), true, false",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLUINTEGER value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(0u, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(4294967295u, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_ULONG,
            &value, 0, &length));
        EXPECT_EQ(44u, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(4294967295u, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(1u, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_ULONG,
        &value, 0, &length));
    EXPECT_EQ(0u, value);
}

TEST_F(BindColIntegrationTest, UnsignedLongBoundColumnChecksNullAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (SELECT 1 AS ord, 4294967295::numeric(10,0) AS value UNION ALL SELECT 2, NULL::numeric(10,0) UNION ALL SELECT 3, 4294967296::numeric(10,0)) v ORDER BY ord", SQL_NTS));
    SQLUINTEGER value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_ULONG,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(4294967295u, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(4294967295u, value);
    EXPECT_EQ(SQL_NULL_DATA, length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4294967295u, value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, UnsignedBigintGetDataChecksExactLimits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 0::numeric(1,0), 9223372036854775808::numeric(19,0), "
                  "18446744073709551615::numeric(20,0), -1::numeric(1,0), "
                  "18446744073709551616::numeric(20,0), "
                  "18446744073709551615.75::numeric(22,2), true, false, "
                  "'1.8446744073709551615e19'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLUBIGINT value = 44;
    SQLLEN length = 91;
    const auto maximum = std::numeric_limits<SQLUBIGINT>::max();
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(0u, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(static_cast<SQLUBIGINT>(1) << 63, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(maximum, value);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{4, 5}) {
        value = 44;
        length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_UBIGINT,
            &value, 0, &length));
        EXPECT_EQ(44u, value);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 6, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(maximum, value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(1u, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 8, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(0u, value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 9, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(maximum, value);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'not-a-number'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    value = 44;
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_UBIGINT,
        &value, 0, &length));
    EXPECT_EQ(44u, value);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
    char recovered[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        recovered, sizeof(recovered), nullptr));
    EXPECT_STREQ("not-a-number", recovered);
}

TEST_F(BindColIntegrationTest, UnsignedBigintBoundColumnChecksNullAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (SELECT 1 AS ord, 18446744073709551615::numeric(20,0) AS value UNION ALL SELECT 2, NULL::numeric(20,0) UNION ALL SELECT 3, 18446744073709551616::numeric(20,0)) v ORDER BY ord", SQL_NTS));
    SQLUBIGINT value = 44;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_UBIGINT,
        &value, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::numeric_limits<SQLUBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::numeric_limits<SQLUBIGINT>::max(), value);
    EXPECT_EQ(SQL_NULL_DATA, length);
    length = 91;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(std::numeric_limits<SQLUBIGINT>::max(), value);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BitToNumericBoundColumnsUseZero) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT false, false, false, false, false", SQL_NTS));
    SQLSMALLINT short_value = -1;
    SQLINTEGER integer = -1;
    SQLBIGINT big = -1;
    SQLREAL real = -1;
    SQLDOUBLE double_value = -1;
    SQLLEN lengths[5]{-1, -1, -1, -1, -1};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SSHORT,
        &short_value, 0, &lengths[0]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_SLONG,
        &integer, 0, &lengths[1]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_SBIGINT,
        &big, 0, &lengths[2]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 4, SQL_C_FLOAT,
        &real, 0, &lengths[3]));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 5, SQL_C_DOUBLE,
        &double_value, 0, &lengths[4]));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0, short_value);
    EXPECT_EQ(0, integer);
    EXPECT_EQ(0, big);
    EXPECT_FLOAT_EQ(0.0f, real);
    EXPECT_DOUBLE_EQ(0.0, double_value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(short_value)), lengths[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer)), lengths[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(big)), lengths[2]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(real)), lengths[3]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(double_value)), lengths[4]);
}

TEST_F(BindColIntegrationTest, BitRejectsTemporalResultTargets) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT true", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{
             SQL_C_DATE, SQL_C_TYPE_DATE,
             SQL_C_TIME, SQL_C_TYPE_TIME,
             SQL_C_TIMESTAMP, SQL_C_TYPE_TIMESTAMP}) {
        SCOPED_TRACE(target);
        std::array<unsigned char, sizeof(SQL_TIMESTAMP_STRUCT)> output;
        output.fill(0x5a);
        SQLLEN length = 83;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, target,
            output.data(), static_cast<SQLLEN>(output.size()), &length));
        EXPECT_EQ(83, length);
        EXPECT_TRUE(std::all_of(output.begin(), output.end(),
            [](unsigned char byte) { return byte == 0x5a; }));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT false", SQL_NTS));
    SQL_DATE_STRUCT date{};
    date.year = 4242;
    SQLLEN length = 84;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4242, date.year);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BinaryRejectsNonCharacterScalarTargets) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)(fixture_redshift_product()
            ? "SELECT FROM_HEX('01')"
            : "SELECT decode('01', 'hex')"), SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{
             SQL_C_BIT, SQL_C_SSHORT, SQL_C_SLONG, SQL_C_SBIGINT,
             SQL_C_FLOAT, SQL_C_DOUBLE, SQL_C_TYPE_DATE,
             SQL_C_TYPE_TIME, SQL_C_TYPE_TIMESTAMP}) {
        SCOPED_TRACE(target);
        std::array<unsigned char, sizeof(SQL_TIMESTAMP_STRUCT)> output;
        output.fill(0x5a);
        SQLLEN length = 85;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, target,
            output.data(), static_cast<SQLLEN>(output.size()), &length));
        EXPECT_EQ(85, length);
        EXPECT_TRUE(std::all_of(output.begin(), output.end(),
            [](unsigned char byte) { return byte == 0x5a; }));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)(fixture_redshift_product()
            ? "SELECT FROM_HEX('01')"
            : "SELECT decode('01', 'hex')"), SQL_NTS));
    SQLINTEGER number = 4242;
    SQLLEN length = 86;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SLONG,
        &number, sizeof(number), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4242, number);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, NumericRejectsTemporalResultTargets) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 42::integer, 1.5::double precision, "
                  "2.5::numeric", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2, 3}) {
        for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{
                 SQL_C_TYPE_DATE, SQL_C_TYPE_TIME,
                 SQL_C_TYPE_TIMESTAMP}) {
            SCOPED_TRACE(column);
            SCOPED_TRACE(target);
            std::array<unsigned char, sizeof(SQL_TIMESTAMP_STRUCT)> output;
            output.fill(0x5a);
            SQLLEN length = 87;
            EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, target,
                output.data(), static_cast<SQLLEN>(output.size()), &length));
            EXPECT_EQ(87, length);
            EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                [](unsigned char byte) { return byte == 0x5a; }));
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
        }
    }

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 42::integer", SQL_NTS));
    SQL_DATE_STRUCT date{};
    date.year = 4242;
    SQLLEN length = 88;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(4242, date.year);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TemporalRejectsNumericResultTargets) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56', "
                  "TIMESTAMP '2024-02-29 12:34:56'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2, 3}) {
        for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{
                 SQL_C_BIT, SQL_C_SSHORT, SQL_C_SLONG, SQL_C_SBIGINT,
                 SQL_C_FLOAT, SQL_C_DOUBLE}) {
            SCOPED_TRACE(column);
            SCOPED_TRACE(target);
            std::array<unsigned char, sizeof(SQLDOUBLE)> output;
            output.fill(0x5a);
            SQLLEN length = 89;
            EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, target,
                output.data(), static_cast<SQLLEN>(output.size()), &length));
            EXPECT_EQ(89, length);
            EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                [](unsigned char byte) { return byte == 0x5a; }));
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
                state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
        }
    }

    char valid_date[11]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        valid_date, sizeof(valid_date), nullptr));
    EXPECT_STREQ("2024-02-29", valid_date);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIME '12:34:56'", SQL_NTS));
    SQLDOUBLE bound = 42.0;
    SQLLEN length = 90;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_DOUBLE,
        &bound, sizeof(bound), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_DOUBLE_EQ(42.0, bound);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TemporalCrossConversionsRespectTypeMatrix) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', TIME '12:34:56'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{SQL_C_TIME, SQL_C_TYPE_TIME}) {
        SQL_TIME_STRUCT output{42, 42, 42};
        SQLLEN length = 91;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, target,
            &output, sizeof(output), &length));
        EXPECT_EQ(42, output.hour);
        EXPECT_EQ(91, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }
    for (SQLSMALLINT target : std::initializer_list<SQLSMALLINT>{SQL_C_DATE, SQL_C_TYPE_DATE}) {
        SQL_DATE_STRUCT output{4242, 42, 42};
        SQLLEN length = 92;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, target,
            &output, sizeof(output), &length));
        EXPECT_EQ(4242, output.year);
        EXPECT_EQ(92, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    }

    SQL_TIMESTAMP_STRUCT timestamp{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), nullptr));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(0, timestamp.hour);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_TYPE_TIMESTAMP,
        &timestamp, sizeof(timestamp), nullptr));
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29'", SQL_NTS));
    SQL_TIME_STRUCT bound{42, 42, 42};
    SQLLEN length = 93;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_TIME,
        &bound, sizeof(bound), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(42, bound.hour);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, NullValueStillChecksResultConversion) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT NULL::date, NULL::date, DATE '2024-02-29'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLINTEGER number = 42;
    SQLLEN length = 91;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_SLONG,
        &number, sizeof(number), &length));
    EXPECT_EQ(42, number);
    EXPECT_EQ(91, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));

    SQL_DATE_STRUCT date{4242, 4, 2};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_TYPE_DATE,
        &date, sizeof(date), &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(4242, date.year);
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_SLONG,
        &number, sizeof(number), &length));
    EXPECT_EQ(42, number);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT NULL::date", SQL_NTS));
    length = 93;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SLONG,
        &number, sizeof(number), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(42, number);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, NumericGetDataDefaultsAndNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric(4,2), NULL::numeric", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 91;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_NUMERIC,
        &numeric, 0, &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);
    EXPECT_EQ(38, numeric.precision);
    EXPECT_EQ(0, numeric.scale);
    EXPECT_EQ(1, numeric.sign);
    EXPECT_EQ(12, numeric.val[0]);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_NUMERIC,
        &numeric, 0, &length));
    std::memset(&numeric, 0x5a, sizeof(numeric));
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_NUMERIC,
        &numeric, 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
}

TEST_F(BindColIntegrationTest, NumericArdScaleAndOverflow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric(4,2), 12345.67::numeric(7,2)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_C_NUMERIC)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 92;
    if (ard_type_get_data_rejected(1, &numeric, 0, &length)) {
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_ARD_TYPE,
        &numeric, 0, &length));
    EXPECT_EQ(5, numeric.precision);
    EXPECT_EQ(2, numeric.scale);
    EXPECT_EQ(0xd2, numeric.val[0]);
    EXPECT_EQ(0x04, numeric.val[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_C_NUMERIC)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    std::memset(&numeric, 0x5a, sizeof(numeric));
    length = 93;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_ARD_TYPE,
        &numeric, 0, &length));
    EXPECT_EQ(93, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BoundNumericUsesArdScaleAndPreservesNull) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric(4,2) UNION ALL SELECT NULL::numeric",
        SQL_NTS));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 92;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_NUMERIC,
        &numeric, sizeof(numeric), &length));
    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    // Descriptor metadata changes unbind the data pointer; restore it last.
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_DATA_PTR,
        &numeric, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0xd2, numeric.val[0]);
    EXPECT_EQ(0x04, numeric.val[1]);
    EXPECT_EQ(5, numeric.precision);
    EXPECT_EQ(2, numeric.scale);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);
    std::memset(&numeric, 0x5a, sizeof(numeric));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
}

TEST_F(BindColIntegrationTest, BoundNumericOverflowPreservesOutput) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 12345.67::numeric", SQL_NTS));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 92;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_NUMERIC,
        &numeric, sizeof(numeric), &length));
    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_DATA_PTR,
        &numeric, 0));
    std::memset(&numeric, 0x5a, sizeof(numeric));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(92, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, CharacterNumericGetDataReportsExactDiagnostics) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT '1.2345e2'::text, 'oops'::text, "
                  "'1e39'::text, NULL::text, '1.2345e2'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_C_NUMERIC)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 91;
    if (ard_type_get_data_rejected(1, &numeric, 0, &length)) {
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_ARD_TYPE,
        &numeric, 0, &length));
    EXPECT_EQ(0x39, numeric.val[0]);
    EXPECT_EQ(0x30, numeric.val[1]);
    EXPECT_EQ(5, numeric.precision);
    EXPECT_EQ(2, numeric.scale);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : {SQLUSMALLINT{2}, SQLUSMALLINT{3}}) {
        std::memset(&numeric, 0x5a, sizeof(numeric));
        length = 92;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_NUMERIC,
            &numeric, 0, &length));
        EXPECT_EQ(92, length);
        EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
            reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
            [](unsigned char byte) { return byte == 0x5a; }));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(column == 2 ? "22018" : "22003",
            reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_NUMERIC,
        &numeric, 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 5, SQL_C_NUMERIC,
        &numeric, 0, &length));
    EXPECT_EQ(123, numeric.val[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BoundCharacterNumericErrorPreservesOutput) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (SELECT 1 AS ord, '1.25e1'::text AS value UNION ALL SELECT 2, 'bad'::text UNION ALL SELECT 3, NULL::text) rows ORDER BY ord", SQL_NTS));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_NUMERIC,
        &numeric, sizeof(numeric), &length));
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_EQ(12, numeric.val[0]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    std::memset(&numeric, 0x5a, sizeof(numeric));
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(92, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
}

TEST_F(BindColIntegrationTest, ApproximateNumericToNumericHandlesBoundaries) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1.25::real, 1.2345e2::double precision, "
                  "1e39::double precision, 'NaN'::double precision, "
                  "'Infinity'::double precision, '-Infinity'::real, "
                  "NULL::real", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 91;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_NUMERIC,
        &numeric, 0, &length));
    EXPECT_EQ(1, numeric.val[0]);
    EXPECT_EQ(38, numeric.precision);
    EXPECT_EQ(0, numeric.scale);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric)), length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_C_NUMERIC)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 2, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    if (ard_type_get_data_rejected(2, &numeric, 0, &length)) {
        return;
    }
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_ARD_TYPE,
        &numeric, 0, &length));
    EXPECT_EQ(0x39, numeric.val[0]);
    EXPECT_EQ(0x30, numeric.val[1]);
    EXPECT_EQ(5, numeric.precision);
    EXPECT_EQ(2, numeric.scale);

    for (SQLUSMALLINT column : {SQLUSMALLINT{3}, SQLUSMALLINT{4},
                                SQLUSMALLINT{5}, SQLUSMALLINT{6}}) {
        std::memset(&numeric, 0x5a, sizeof(numeric));
        length = 92;
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_NUMERIC,
            &numeric, 0, &length));
        EXPECT_EQ(92, length);
        EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
            reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
            [](unsigned char byte) { return byte == 0x5a; }));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_NUMERIC,
        &numeric, 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
}

TEST_F(BindColIntegrationTest, BoundApproximateNumericUsesScale) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT value FROM (SELECT 1 AS ord, 1.25::real AS value UNION ALL SELECT 2, 1e30::real UNION ALL SELECT 3, NULL::real) rows ORDER BY ord", SQL_NTS));
    SQL_NUMERIC_STRUCT numeric{};
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_NUMERIC,
        &numeric, sizeof(numeric), &length));
    SQLHDESC ard = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
        &ard, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(5)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(1)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_DATA_PTR,
        &numeric, 0));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_EQ(12, numeric.val[0]);
    EXPECT_EQ(5, numeric.precision);
    EXPECT_EQ(1, numeric.scale);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    std::memset(&numeric, 0x5a, sizeof(numeric));
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(92, length);
    EXPECT_TRUE(std::all_of(reinterpret_cast<const unsigned char*>(&numeric),
        reinterpret_cast<const unsigned char*>(&numeric) + sizeof(numeric),
        [](unsigned char byte) { return byte == 0x5a; }));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NULL_DATA, length);
}

TEST_F(BindColIntegrationTest, CharacterToBinaryReturnsRawUtf8Bytes) {
    auto first_sql = odbcpp::test::text::utf8_to_ansi_input("SELECT 'AéZ'::text, ''::text");
    ASSERT_TRUE(first_sql.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        reinterpret_cast<SQLCHAR*>(first_sql->data()), SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    // iODBC 3.52.16 widens the ANSI statement byte by byte, so the server
    // stores two characters for the UTF-8 'é'. The byte checks below assume
    // the original text, so the case asserts the stored bytes and ends.
    if (odbcpp::test::dm::stops_case(
            odbcpp::test::dm::Deviation::AnsiTextWidenedPerByte)) {
        SQLCHAR stored[16]{};
        SQLLEN stored_length = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY,
            stored, sizeof(stored), &stored_length));
        const auto expected_stored = odbcpp::test::dm::ansi_widened("AéZ");
        ASSERT_EQ(static_cast<SQLLEN>(expected_stored.size()), stored_length);
        ASSERT_LE(stored_length, static_cast<SQLLEN>(sizeof(stored)));
        EXPECT_EQ(expected_stored,
                  std::string(reinterpret_cast<const char*>(stored),
                              static_cast<std::size_t>(stored_length)));
        return;
    }

    SQLCHAR first[2]{0x55, 0x55};
    SQLCHAR second[2]{0x55, 0x55};
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_BINARY,
        first, sizeof(first), &length));
    EXPECT_EQ(4, length);
    EXPECT_EQ(0x41, first[0]);
    EXPECT_EQ(0xc3, first[1]);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY,
        second, sizeof(second), &length));
    EXPECT_EQ(2, length);
    EXPECT_EQ(0xa9, second[0]);
    EXPECT_EQ(0x5a, second[1]);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_BINARY,
        second, sizeof(second), &length));

    SQLCHAR empty = 0x55;
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_BINARY,
        &empty, 0, &length));
    EXPECT_EQ(0x55, empty);
    EXPECT_EQ(0, length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    auto second_sql = odbcpp::test::text::utf8_to_ansi_input("SELECT 'AéZ'::text, 'AéZ'::text");
    ASSERT_TRUE(second_sql.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        reinterpret_cast<SQLCHAR*>(second_sql->data()), SQL_NTS));
    SQLCHAR full[4]{};
    SQLCHAR truncated[2]{0x55, 0x55};
    SQLLEN full_length = -1;
    SQLLEN truncated_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BINARY,
        full, sizeof(full), &full_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_BINARY,
        truncated, sizeof(truncated), &truncated_length));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    EXPECT_EQ(4, full_length);
    EXPECT_EQ(4, truncated_length);
    EXPECT_EQ(0x41, full[0]);
    EXPECT_EQ(0xc3, full[1]);
    EXPECT_EQ(0xa9, full[2]);
    EXPECT_EQ(0x5a, full[3]);
    EXPECT_EQ(0x41, truncated[0]);
    EXPECT_EQ(0xc3, truncated[1]);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, IntegerTextNeedsWholeValueBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1234::integer, 1234::integer", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 89;
    char narrow_tiny[4]{'x', 'x', 'x', 'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(89, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[5]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("1234", narrow);
    EXPECT_EQ(4, length);

    SQLWCHAR wide_tiny[4]{'x', 'x', 'x', 'x'};
    length = 90;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(90, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[5]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('4'), wide[3]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[4]);
    EXPECT_EQ(static_cast<SQLLEN>(4 * sizeof(SQLWCHAR)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1234::integer, 1234::integer", SQL_NTS));
    char bound_narrow[5]{};
    SQLWCHAR bound_wide[4]{'x', 'x', 'x', 'x'};
    SQLLEN narrow_length = -1;
    SQLLEN wide_length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_narrow, sizeof(bound_narrow), &narrow_length));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_WCHAR,
        bound_wide, sizeof(bound_wide), &wide_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_STREQ("1234", bound_narrow);
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), bound_wide[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT -1234::integer", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char signed_tiny[5]{'x', 'x', 'x', 'x', 'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        signed_tiny, sizeof(signed_tiny), &length));
    EXPECT_EQ('x', signed_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char signed_full[6]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        signed_full, sizeof(signed_full), &length));
    EXPECT_STREQ("-1234", signed_full);
}

TEST_F(BindColIntegrationTest, DecimalTextCanTruncateOnlyFractionalDigits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1234.56::numeric(6,2), 1234.56::numeric(6,2)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 93;
    char narrow_tiny[4]{'x', 'x', 'x', 'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[5]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("1234", narrow);
    EXPECT_EQ(7, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    char fractional[2]{'x', 'y'};
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_CHAR,
        fractional, sizeof(fractional), &length));
    EXPECT_EQ('x', fractional[0]);
    EXPECT_EQ('y', fractional[1]);
    EXPECT_EQ(7, length);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("1234", narrow);
    EXPECT_EQ(7, length);

    SQLWCHAR wide_tiny[4]{'x', 'x', 'x', 'x'};
    length = 94;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(94, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[5]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('4'), wide[3]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[4]);
    EXPECT_EQ(static_cast<SQLLEN>(7 * sizeof(SQLWCHAR)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    SQLWCHAR fractional_wide[2]{'x', 'y'};
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        fractional_wide, sizeof(fractional_wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), fractional_wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('y'), fractional_wide[1]);
    EXPECT_EQ(static_cast<SQLLEN>(7 * sizeof(SQLWCHAR)), length);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('4'), wide[3]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[4]);
    EXPECT_EQ(static_cast<SQLLEN>(7 * sizeof(SQLWCHAR)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT -1234.56::numeric(6,2)", SQL_NTS));
    char bound_tiny[5]{'x', 'x', 'x', 'x', 'x'};
    SQLLEN bound_length = 95;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, ApproximateNumericTextPreservesMagnitude) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1234.56::double precision, "
                  "1.23e+30::double precision, 1.23e-30::double precision",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 91;
    char tiny[4]{'x', 'x', 'x', 'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        tiny, sizeof(tiny), &length));
    EXPECT_EQ('x', tiny[0]);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char whole[5]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        whole, sizeof(whole), &length));
    EXPECT_STREQ("1234", whole);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    char fraction[8]{'x', 'y', 'z', 'x', 'y', 'z', 'x', 'y'};
    const auto before_fraction = std::array<char, 8>{'x', 'y', 'z', 'x', 'y', 'z', 'x', 'y'};
    const auto before_length = length;
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_CHAR,
        fraction, sizeof(fraction), &length));
    EXPECT_EQ(0, std::memcmp(fraction, before_fraction.data(), sizeof(fraction)));
    EXPECT_EQ(before_length, length);

    char exponent_tiny[5]{'x', 'x', 'x', 'x', 'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_CHAR,
        exponent_tiny, sizeof(exponent_tiny), &length));
    EXPECT_EQ('x', exponent_tiny[0]);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char exponent[32]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_CHAR,
        exponent, sizeof(exponent), &length));
    EXPECT_STREQ("1.23e+30", exponent);

    SQLWCHAR exponent_wide_tiny[5]{'x', 'x', 'x', 'x', 'x'};
    length = 93;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_WCHAR,
        exponent_wide_tiny, sizeof(exponent_wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), exponent_wide_tiny[0]);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR exponent_wide[32]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_WCHAR,
        exponent_wide, sizeof(exponent_wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), exponent_wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('e'), exponent_wide[4]);
    EXPECT_EQ(static_cast<SQLWCHAR>('-'), exponent_wide[5]);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 1.23e+30::double precision", SQL_NTS));
    char bound_tiny[6]{'x', 'x', 'x', 'x', 'x', 'x'};
    SQLLEN bound_length = 94;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, DateTextNeedsCompleteBuffer) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29', DATE '2024-02-29'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 91;
    char narrow_tiny[10]{'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[11]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("2024-02-29", narrow);
    EXPECT_EQ(10, length);

    SQLWCHAR wide_tiny[10]{'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[11]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('2'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('9'), wide[9]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[10]);
    EXPECT_EQ(static_cast<SQLLEN>(10 * sizeof(SQLWCHAR)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT DATE '2024-02-29'", SQL_NTS));
    char bound_tiny[10]{'x'};
    SQLLEN bound_length = 93;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TimeTextKeepsSecondsAndZone) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIME '12:34:56.78', TIME '12:34:56.78', "
                  "TIMETZ '12:34:56+02'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 91;
    char narrow_tiny[8]{'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[9]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("12:34:56", narrow);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    char fraction[4]{'x', 'y', 'z', 'w'};
    const auto fraction_poison = std::array<char, 4>{'x', 'y', 'z', 'w'};
    length = 73;
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_CHAR,
        fraction, sizeof(fraction), &length));
    EXPECT_EQ(0, std::memcmp(fraction, fraction_poison.data(), sizeof(fraction)));
    EXPECT_EQ(73, length);

    SQLWCHAR wide_tiny[8]{'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[9]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('1'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('6'), wide[7]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[8]);
    EXPECT_EQ(static_cast<SQLLEN>(11 * sizeof(SQLWCHAR)), length);

    char zone_tiny[9]{'x'};
    length = 93;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_CHAR,
        zone_tiny, sizeof(zone_tiny), &length));
    EXPECT_EQ('x', zone_tiny[0]);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char zone[20]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_CHAR,
        zone, sizeof(zone), &length));
    EXPECT_STREQ("12:34:56+02", zone);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIME '12:34:56'", SQL_NTS));
    char bound_tiny[8]{'x'};
    SQLLEN bound_length = 94;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TimestampTextKeepsSecondsAndZone) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SET TIME ZONE 'UTC'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIMESTAMP '2024-02-29 12:34:56.78', "
                  "TIMESTAMP '2024-02-29 12:34:56.78', "
                  "TIMESTAMPTZ '2024-02-29 12:34:56.78+00'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR state[6]{};
    SQLLEN length = 91;
    char narrow_tiny[19]{'x'};
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow_tiny, sizeof(narrow_tiny), &length));
    EXPECT_EQ('x', narrow_tiny[0]);
    EXPECT_EQ(91, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char narrow[20]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 1, SQL_C_CHAR,
        narrow, sizeof(narrow), &length));
    EXPECT_STREQ("2024-02-29 12:34:56", narrow);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
    char fraction[4]{'x', 'y', 'z', 'w'};
    const auto fraction_poison = std::array<char, 4>{'x', 'y', 'z', 'w'};
    length = 73;
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_CHAR,
        fraction, sizeof(fraction), &length));
    EXPECT_EQ(0, std::memcmp(fraction, fraction_poison.data(), sizeof(fraction)));
    EXPECT_EQ(73, length);

    SQLWCHAR wide_tiny[19]{'x'};
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide_tiny, sizeof(wide_tiny), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), wide_tiny[0]);
    EXPECT_EQ(92, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    SQLWCHAR wide[20]{};
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(hstmt, 2, SQL_C_WCHAR,
        wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>('2'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('6'), wide[18]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[19]);
    EXPECT_EQ(static_cast<SQLLEN>(22 * sizeof(SQLWCHAR)), length);

    char zone_tiny[20]{'x'};
    length = 93;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_CHAR,
        zone_tiny, sizeof(zone_tiny), &length));
    EXPECT_EQ('x', zone_tiny[0]);
    EXPECT_EQ(93, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    char zone[40]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_CHAR,
        zone, sizeof(zone), &length));
    EXPECT_STREQ("2024-02-29 12:34:56.78+00", zone);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT TIMESTAMP '2024-02-29 12:34:56'", SQL_NTS));
    char bound_tiny[19]{'x'};
    SQLLEN bound_length = 94;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR,
        bound_tiny, sizeof(bound_tiny), &bound_length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ('x', bound_tiny[0]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, FailedGetDataDoesNotDiscardPartialOffset) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'abcdef'::text, 'other'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char chunk[4]{};
    SQLLEN length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &length));
    EXPECT_STREQ("abc", chunk);

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 2, SQL_C_CHAR, nullptr, 0, &length));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY009", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &length));
    EXPECT_STREQ("def", chunk);
    EXPECT_EQ(3, length);
}

TEST_F(BindColIntegrationTest, GetDataRetrievesWideTextInWholeCodePoints) {
    // iODBC 3.52.16 widens the ANSI statement byte by byte, so the server
    // stores one code point per byte of the UTF-8 literal (ansi_widened).
    const std::string expected = odbcpp::test::dm::value(
        odbcpp::test::dm::Deviation::AnsiTextWidenedPerByte,
        std::string("A\xf0\x9f\x99\x82" "BC"),
        odbcpp::test::dm::ansi_widened("A\xf0\x9f\x99\x82" "BC"));
#if defined(_WIN32) && defined(ODBCPP_TEST_VIA_DRIVER_MANAGER)
    // Supplementary characters cannot be represented by every Windows ANSI
    // code page. They are fixture data for this wide-output chunking test.
    auto sql = odbcpp::test::text::utf8_to_wide("SELECT 'A\xf0\x9f\x99\x82" "BC'::text");
    ASSERT_TRUE(sql.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirectW(
        hstmt, sql->data(), static_cast<SQLINTEGER>(sql->size())));
#else
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 'A\xf0\x9f\x99\x82" "BC'::text",
        SQL_NTS));
#endif
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    std::string assembled;
    SQLRETURN result = SQL_SUCCESS_WITH_INFO;
    for (int call = 0; call < 4 && result == SQL_SUCCESS_WITH_INFO; ++call) {
        SQLWCHAR chunk[3]{};
        SQLLEN remaining_bytes = 0;
        result = SQLGetData(hstmt, 1, SQL_C_WCHAR, chunk, sizeof(chunk),
                            &remaining_bytes);
        ASSERT_TRUE(result == SQL_SUCCESS || result == SQL_SUCCESS_WITH_INFO);
        std::size_t chunk_units = 0;
        while (chunk_units < 2 && chunk[chunk_units] != 0) ++chunk_units;
        const auto converted = odbcpp::test::text::wide_to_utf8(
            std::span<const SQLWCHAR>(chunk, chunk_units));
        ASSERT_TRUE(converted.has_value());
        assembled += *converted;
        EXPECT_GT(remaining_bytes, 0);
    }
    EXPECT_EQ(SQL_SUCCESS, result);
    EXPECT_EQ(expected, assembled);
}

TEST_F(BindColIntegrationTest, RequestsUtf8ClientEncoding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT current_setting('client_encoding'), chr(233)::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char encoding[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, encoding, sizeof(encoding), nullptr));
    EXPECT_STREQ("UTF8", encoding);

    SQLWCHAR wide[2]{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_WCHAR, wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLWCHAR>(0x00e9), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[1]);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);
}

TEST_F(BindColIntegrationTest, RequestsIsoDateStyle) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT current_setting('DateStyle'), DATE '2024-02-29', "
            "TIMESTAMP '2024-02-29 12:34:56'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char date_style[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, date_style, sizeof(date_style), nullptr));
    EXPECT_TRUE(std::string_view(date_style).starts_with("ISO"));

    SQL_DATE_STRUCT date{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_DATE, &date, sizeof(date), nullptr));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);

    SQL_TIMESTAMP_STRUCT timestamp{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp),
        nullptr));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
}

TEST_F(BindColIntegrationTest, GetDataOffsetsResetForEachFetchedRow) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT value FROM (SELECT 1 AS id, 'abcdefgh'::text AS value UNION ALL SELECT 2, 'ijklmnop'::text) rows ORDER BY id",
        SQL_NTS));

    char chunk[5]{};
    SQLLEN remaining = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining));
    EXPECT_STREQ("abcd", chunk);
    EXPECT_EQ(8, remaining);

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining));
    EXPECT_STREQ("ijkl", chunk);
    EXPECT_EQ(8, remaining);
}

TEST_F(BindColIntegrationTest, SwitchingColumnsInvalidatesPartialOffset) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'abcdefgh'::text, 'other'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char chunk[5]{};
    SQLLEN remaining = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining));
    EXPECT_STREQ("abcd", chunk);
    EXPECT_EQ(8, remaining);

    char other[8]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, other, sizeof(other), &remaining));
    EXPECT_STREQ("other", other);

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, chunk, sizeof(chunk), &remaining));
    EXPECT_STREQ("abcd", chunk);
    EXPECT_EQ(8, remaining);
}

// Test mixed binding (some bound, some unbound)
TEST_F(BindColIntegrationTest, MixedBinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Bound' as col1, 'Unbound' as col2, 999 as col3", SQL_NTS));
    
    char bound_val[256];
    SQLINTEGER int_val;
    SQLLEN bound_len, int_len;
    
    // Bind only columns 1 and 3, leave column 2 unbound
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, bound_val, sizeof(bound_val), &bound_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_SLONG, &int_val, 0, &int_len));
    
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // Bound columns should be populated
    EXPECT_STREQ("Bound", bound_val);
    EXPECT_EQ(999, int_val);
    
    // Unbound column should still be accessible via SQLGetData
    char unbound_val[256];
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_CHAR, unbound_val, sizeof(unbound_val), nullptr));
    EXPECT_STREQ("Unbound", unbound_val);
}

// Test column binding with multiple rows
TEST_F(BindColIntegrationTest, MultipleRowBinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT generate_series(1,3) as num, 'Row' || generate_series(1,3) as text", SQL_NTS));
    
    SQLINTEGER num_val;
    char text_val[256];
    SQLLEN num_len, text_len;
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_SLONG, &num_val, 0, &num_len));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_CHAR, text_val, sizeof(text_val), &text_len));
    
    // Fetch multiple rows
    std::vector<std::pair<int, std::string>> results;
    
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        results.emplace_back(num_val, std::string(text_val));
    }
    
    // Verify all rows were fetched with bound data
    ASSERT_EQ(3, results.size());
    EXPECT_EQ(1, results[0].first);
    EXPECT_EQ("Row1", results[0].second);
    EXPECT_EQ(2, results[1].first);
    EXPECT_EQ("Row2", results[1].second);
    EXPECT_EQ(3, results[2].first);
    EXPECT_EQ("Row3", results[2].second);
}

// Test column binding with data type conversions
TEST_F(BindColIntegrationTest, DataTypeConversions) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 123 as int_as_str, '456' as str_as_int, 78.9 as double_as_int", SQL_NTS));
    
    char int_as_str[256];
    SQLINTEGER str_as_int;
    SQLINTEGER double_as_int;
    SQLLEN len1, len2, len3;
    
    // Bind with type conversions
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, int_as_str, sizeof(int_as_str), &len1));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_SLONG, &str_as_int, 0, &len2));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 3, SQL_C_SLONG, &double_as_int, 0, &len3));
    
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    
    // Verify conversions
    EXPECT_STREQ("123", int_as_str);
    EXPECT_EQ(456, str_as_int);
    EXPECT_EQ(78, double_as_int);
}

TEST_F(BindColIntegrationTest, BigintTextConversionAtExactLimits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT '9223372036854775807'::text, "
            "'-9223372036854775808'::text, "
            "'9223372036854775807.9'::text, "
            "'9223372036854775808'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLBIGINT value = 0;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    value = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BigintScientificTextConversionAtExactLimits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT '9.223372036854775807e18'::text, "
            "'-9.223372036854775808e18'::text, "
            "'9.2233720368547758079e18'::text, "
            "'9.223372036854775808e18'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLBIGINT value = 0;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    value = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, FloatingConversionRejectsUnderflowToZero) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT '1e-50'::text, '-1e-50'::text, "
            "'1e-400'::text, '-1e-400'::text, "
            "'1e-30'::text, '1e-300'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    for (const SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        SQLREAL value = 7.0f;
        SQLLEN length = 8;
        EXPECT_EQ(SQL_ERROR, SQLGetData(
            hstmt, column, SQL_C_FLOAT, &value, sizeof(value), &length));
        EXPECT_EQ(7.0f, value);
        EXPECT_EQ(8, length);
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }
    for (const SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{3, 4}) {
        SQLDOUBLE value = 7.0;
        SQLLEN length = 8;
        EXPECT_EQ(SQL_ERROR, SQLGetData(
            hstmt, column, SQL_C_DOUBLE, &value, sizeof(value), &length));
        EXPECT_EQ(7.0, value);
        EXPECT_EQ(8, length);
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    }

    SQLREAL small_float = 0;
    SQLDOUBLE small_double = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_FLOAT, &small_float, sizeof(small_float), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 6, SQL_C_DOUBLE, &small_double, sizeof(small_double), nullptr));
    EXPECT_GT(small_float, 0);
    EXPECT_GT(small_double, 0);
}

TEST_F(BindColIntegrationTest, FloatingSpecialValuesPreserveIeeeResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'NaN'::double precision, "
                  "'Infinity'::double precision, "
                  "'-Infinity'::real", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLDOUBLE nan_value = 7.0;
    SQLLEN length = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_DOUBLE,
        &nan_value, sizeof(nan_value), &length));
    EXPECT_TRUE(std::isnan(nan_value));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(nan_value)), length);

    SQLDOUBLE positive = 7.0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_DOUBLE,
        &positive, sizeof(positive), &length));
    EXPECT_TRUE(std::isinf(positive));
    EXPECT_GT(positive, 0);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(positive)), length);

    SQLREAL negative = 7.0f;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_FLOAT,
        &negative, sizeof(negative), &length));
    EXPECT_TRUE(std::isinf(negative));
    EXPECT_LT(negative, 0);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(negative)), length);

    SQLINTEGER integer = 73;
    length = 92;
    EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 2, SQL_C_SLONG,
        &integer, sizeof(integer), &length));
    EXPECT_EQ(73, integer);
    EXPECT_EQ(92, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
        state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"SELECT 'Infinity'::double precision", SQL_NTS));
    SQLREAL bound = 0;
    SQLLEN bound_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_FLOAT,
        &bound, sizeof(bound), &bound_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_TRUE(std::isinf(bound));
    EXPECT_GT(bound, 0);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(bound)), bound_length);
}

TEST_F(BindColIntegrationTest, BoundFloatingUnderflowPreservesOutput) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT '1e-50'::text", SQL_NTS));
    SQLREAL value = 7.0f;
    SQLLEN length = 8;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_FLOAT, &value, sizeof(value), &length));
    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    EXPECT_EQ(7.0f, value);
    EXPECT_EQ(8, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, FloatingConversionIgnoresProcessLocale) {
    const char* current = std::setlocale(LC_NUMERIC, nullptr);
    const std::string original = current ? current : "C";
    const char* german_locale = std::setlocale(LC_NUMERIC, "de_DE.UTF-8");
#ifdef _WIN32
    if (!german_locale) {
        german_locale = std::setlocale(LC_NUMERIC, "German_Germany.1252");
    }
#endif
    if (!german_locale) GTEST_SKIP() << "German numeric locale unavailable";
    struct LocaleRestore {
        std::string name;
        ~LocaleRestore() { std::setlocale(LC_NUMERIC, name.c_str()); }
    } restore{original};
    if (std::localeconv()->decimal_point[0] != ',') {
        GTEST_SKIP() << "Selected locale does not use a decimal comma";
    }

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT 1.5::numeric(2,1), '1.5e2'::text, '1,5'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLREAL real_value = 0;
    SQLDOUBLE double_value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_FLOAT, &real_value, sizeof(real_value), nullptr));
    EXPECT_FLOAT_EQ(1.5f, real_value);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_DOUBLE, &double_value, sizeof(double_value), nullptr));
    EXPECT_DOUBLE_EQ(150.0, double_value);

    double_value = 73.0;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 3, SQL_C_DOUBLE, &double_value, sizeof(double_value), nullptr));
    EXPECT_DOUBLE_EQ(73.0, double_value);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, BigintLeadingWhitespaceConversionAtLimits) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT ' \t9223372036854775807'::text, "
            "' \t-9.223372036854775808e18'::text, "
            "' \t9.2233720368547758079e18'::text, "
            "' \t9.223372036854775808e18'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLBIGINT value = 0;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(value)), length);

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::min(), value);

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), value);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    value = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_SBIGINT, &value, sizeof(value), &length));
    EXPECT_EQ(73, value);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, CharacterConversionsIgnoreOuterWhitespace) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT ' 9223372036854775807 '::text, "
            "' 1.25e2 '::text, ' 2024-02-29 '::text, "
            "' 12:34:56 '::text, ' 2024-02-29 12:34:56 '::text, "
            "' 12:34:56 x'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLLEN length = -1;
    SQLBIGINT integer = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SBIGINT, &integer, sizeof(integer), &length));
    EXPECT_EQ(std::numeric_limits<SQLBIGINT>::max(), integer);

    SQLDOUBLE floating = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_DOUBLE, &floating, sizeof(floating), &length));
    EXPECT_DOUBLE_EQ(125.0, floating);

    SQL_DATE_STRUCT date{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_DATE, &date, sizeof(date), &length));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);

    SQL_TIME_STRUCT time{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_TIME, &time, sizeof(time), &length));
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(34, time.minute);
    EXPECT_EQ(56, time.second);

    SQL_TIMESTAMP_STRUCT timestamp{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(12, timestamp.hour);

    time.hour = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 6, SQL_C_TIME, &time, sizeof(time), &length));
    EXPECT_EQ(73, time.hour);
    EXPECT_EQ(74, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TemporalFractionTruncationHasDiagnostic) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT TIME '12:34:56.123456', TIME '12:34:56', "
            "TIMESTAMP '2024-02-29 12:34:56.123456', "
            "'2024-02-29 12:34:56.1234567891'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_TIME_STRUCT time{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_TIME, &time, sizeof(time), &length));
    EXPECT_EQ(12, time.hour);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(time)), length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_TIME, &time, sizeof(time), &length));
    EXPECT_EQ(12, time.hour);

    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 3, SQL_C_TIME, &time, sizeof(time), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    SQL_TIMESTAMP_STRUCT timestamp{};
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 4, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(123456789u, timestamp.fraction);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TimestampToDateReportsLostTime) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT TIMESTAMP '2024-02-29 00:00:00', "
            "TIMESTAMP '2024-02-29 12:34:56', "
            "'2024-02-29 00:00:00.0000000001'::text, "
            "'2024-02-30 12:00:00'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_DATE_STRUCT date{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_DATE, &date, sizeof(date), &length));
    EXPECT_EQ(2024, date.year);
    EXPECT_EQ(2, date.month);
    EXPECT_EQ(29, date.day);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(date)), length);

    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{2, 3}) {
        ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
            hstmt, column, SQL_C_DATE, &date, sizeof(date), &length));
        EXPECT_EQ(29, date.day);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    }

    date.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_DATE, &date, sizeof(date), &length));
    EXPECT_EQ(73, date.year);
    EXPECT_EQ(74, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, DateToTimestampZeroesTimeFields) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT DATE '2024-02-29', '2024-02-30'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQL_TIMESTAMP_STRUCT timestamp{};
    timestamp.hour = 73;
    timestamp.minute = 74;
    timestamp.second = 75;
    timestamp.fraction = 76;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(2024, timestamp.year);
    EXPECT_EQ(2, timestamp.month);
    EXPECT_EQ(29, timestamp.day);
    EXPECT_EQ(0, timestamp.hour);
    EXPECT_EQ(0, timestamp.minute);
    EXPECT_EQ(0, timestamp.second);
    EXPECT_EQ(0u, timestamp.fraction);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), length);

    timestamp.year = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 2, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(73, timestamp.year);
    EXPECT_EQ(74, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, TimeToTimestampUsesCurrentLocalDate) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT TIME '12:34:56', '25:00:00'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    const auto before = odbcpp::test::local_calendar(std::time(nullptr));
    ASSERT_TRUE(before.has_value());

    SQL_TIMESTAMP_STRUCT timestamp{};
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));

    const auto after = odbcpp::test::local_calendar(std::time(nullptr));
    ASSERT_TRUE(after.has_value());
    const auto matches = [&](const std::tm& calendar) {
        return timestamp.year == calendar.tm_year + 1900 &&
            timestamp.month == calendar.tm_mon + 1 &&
            timestamp.day == calendar.tm_mday;
    };
    EXPECT_TRUE(matches(*before) || matches(*after));
    EXPECT_EQ(12, timestamp.hour);
    EXPECT_EQ(34, timestamp.minute);
    EXPECT_EQ(56, timestamp.second);
    EXPECT_EQ(0u, timestamp.fraction);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(timestamp)), length);

    timestamp.hour = 73;
    length = 74;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 2, SQL_C_TIMESTAMP, &timestamp, sizeof(timestamp), &length));
    EXPECT_EQ(73, timestamp.hour);
    EXPECT_EQ(74, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

TEST_F(BindColIntegrationTest, NumericTextToBitDiagnostics) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)
            "SELECT '0.5'::text, '1.999999999999999999999'::text, true",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLCHAR bit = 73;
    SQLLEN length = -1;
    SQLCHAR state[6]{};
    for (SQLUSMALLINT column : std::initializer_list<SQLUSMALLINT>{1, 2}) {
        ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
            hstmt, column, SQL_C_BIT, &bit, sizeof(bit), &length));
        EXPECT_EQ(column == 1 ? 0 : 1, bit);
        EXPECT_EQ(static_cast<SQLLEN>(sizeof(bit)), length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_BIT, &bit, sizeof(bit), &length));
    EXPECT_EQ(1, bit);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    struct Failure {
        const char* query;
        const char* state;
    };
    for (const Failure& failure : {
             Failure{"SELECT '-0.5'::text", "22003"},
             Failure{"SELECT '2'::text", "22003"},
             Failure{"SELECT '1.5x'::text", "22018"}}) {
        SCOPED_TRACE(failure.query);
        std::string query(failure.query);
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
            hstmt, reinterpret_cast<SQLCHAR*>(query.data()), SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        bit = 73;
        length = 74;
        EXPECT_EQ(SQL_ERROR, SQLGetData(
            hstmt, 1, SQL_C_BIT, &bit, sizeof(bit), &length));
        EXPECT_EQ(73, bit);
        EXPECT_EQ(74, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(failure.state, reinterpret_cast<char*>(state));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
}

TEST_F(BindColIntegrationTest, MetadataDrivenDefaultConversions) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 42::smallint, 1234567890123::bigint, "
                  "1.25::real, 3.5::double precision, true, "
                  "DATE '2024-02-29', TIME '23:45:30', "
                  "TIMESTAMP '2024-02-29 12:34:56.123456'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLSMALLINT small_value = 0;
    SQLBIGINT big_value = 0;
    SQLREAL real_value = 0;
    SQLDOUBLE double_value = 0;
    SQLCHAR bool_value = 0;
    SQL_DATE_STRUCT date_value{};
    SQL_TIME_STRUCT time_value{};
    SQL_TIMESTAMP_STRUCT timestamp_value{};
    SQLLEN indicator = 0;

    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_DEFAULT,
        &small_value, 0, &indicator));
    EXPECT_EQ(42, small_value);
    EXPECT_EQ(sizeof(SQLSMALLINT), indicator);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_DEFAULT,
        &big_value, 0, &indicator));
    EXPECT_EQ(1234567890123LL, big_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_DEFAULT,
        &real_value, 0, &indicator));
    EXPECT_FLOAT_EQ(1.25f, real_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_DEFAULT,
        &double_value, 0, &indicator));
    EXPECT_DOUBLE_EQ(3.5, double_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, SQL_C_DEFAULT,
        &bool_value, 0, &indicator));
    EXPECT_EQ(1, bool_value);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_DEFAULT,
        &date_value, 0, &indicator));
    EXPECT_EQ(2024, date_value.year);
    EXPECT_EQ(2, date_value.month);
    EXPECT_EQ(29, date_value.day);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 7, SQL_C_DEFAULT,
        &time_value, 0, &indicator));
    EXPECT_EQ(23, time_value.hour);
    EXPECT_EQ(45, time_value.minute);
    EXPECT_EQ(30, time_value.second);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 8, SQL_C_DEFAULT,
        &timestamp_value, 0, &indicator));
    EXPECT_EQ(2024, timestamp_value.year);
    EXPECT_EQ(12, timestamp_value.hour);
    EXPECT_EQ(123456000u, timestamp_value.fraction);
}

TEST_F(BindColIntegrationTest, ByteaUsesBinaryDefaultAndSupportsChunks) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)(fixture_redshift_product()
            ? "SELECT FROM_HEX('00017fff')"
            : "SELECT decode('00017fff', 'hex')"), SQL_NTS));

    SQLSMALLINT sql_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &sql_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(fixture_redshift_product() ? SQL_LONGVARBINARY : SQL_VARBINARY, sql_type);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    unsigned char first[2]{};
    unsigned char second[2]{};
    SQLLEN indicator = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_DEFAULT, first, sizeof(first), &indicator));
    EXPECT_EQ(4, indicator);
    EXPECT_EQ(0x00, first[0]);
    EXPECT_EQ(0x01, first[1]);
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_DEFAULT, second, sizeof(second), &indicator));
    EXPECT_EQ(2, indicator);
    EXPECT_EQ(0x7f, second[0]);
    EXPECT_EQ(0xff, second[1]);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 1, SQL_C_DEFAULT, second, sizeof(second), &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)(fixture_redshift_product()
            ? "SELECT FROM_HEX('10203040')"
            : "SELECT decode('10203040', 'hex')"), SQL_NTS));
    unsigned char bound[4]{};
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_DEFAULT, bound, sizeof(bound), &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(4, indicator);
    EXPECT_EQ(0x10, bound[0]);
    EXPECT_EQ(0x20, bound[1]);
    EXPECT_EQ(0x30, bound[2]);
    EXPECT_EQ(0x40, bound[3]);
}

TEST_F(BindColIntegrationTest, BoundColumnsResolveSqlCDefault) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 42::integer, DATE '2024-02-29'", SQL_NTS));

    SQLINTEGER integer_value = 0;
    SQL_DATE_STRUCT date_value{};
    SQLLEN integer_indicator = 0;
    SQLLEN date_indicator = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_DEFAULT,
        &integer_value, 0, &integer_indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_DEFAULT,
        &date_value, 0, &date_indicator));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(42, integer_value);
    EXPECT_EQ(sizeof(SQLINTEGER), integer_indicator);
    EXPECT_EQ(2024, date_value.year);
    EXPECT_EQ(2, date_value.month);
    EXPECT_EQ(29, date_value.day);
    EXPECT_EQ(sizeof(SQL_DATE_STRUCT), date_indicator);
}

TEST_F(BindColIntegrationTest, GetDataValidatesAndResolvesTargetTypes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 42::integer, 43::integer, 44::integer, DATE '2024-02-29'",
        SQL_NTS));
    char bound[16]{};
    SQLLEN bound_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, bound, sizeof(bound), &bound_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char value[16]{};
    SQLLEN value_length = 0;
    if (ard_type_get_data_rejected(1, value, sizeof(value), &value_length)) {
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_ARD_TYPE, value, sizeof(value), &value_length));
    EXPECT_STREQ("42", value);
    EXPECT_EQ(2, value_length);

    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 2, 12345, value, sizeof(value), &value_length));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY003", reinterpret_cast<char*>(state));

    std::array<unsigned char, sizeof(SQLINTEGER) + 2> binary;
    binary.fill(0xa5);
    const auto before = binary;
    SQLLEN binary_length = 79;
    for (SQLLEN capacity = 0; capacity < static_cast<SQLLEN>(sizeof(SQLINTEGER)); ++capacity) {
        SCOPED_TRACE(capacity);
        ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, 3, SQL_C_BINARY,
            binary.data() + 1, capacity, &binary_length));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
        EXPECT_EQ(before, binary);
        EXPECT_EQ(79, binary_length);
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_BINARY,
        binary.data() + 1, sizeof(SQLINTEGER), &binary_length));
    const SQLINTEGER expected = 44;
    EXPECT_EQ(sizeof(SQLINTEGER), binary_length);
    EXPECT_EQ(0, std::memcmp(&expected, binary.data() + 1, sizeof(expected)));
    EXPECT_EQ(0xa5, binary.front());
    EXPECT_EQ(0xa5, binary.back());
    const auto completed = binary;
    binary_length = 83;
    ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 3, SQL_C_BINARY,
        binary.data() + 1, sizeof(SQLINTEGER), &binary_length));
    EXPECT_EQ(completed, binary);
    EXPECT_EQ(83, binary_length);

    // Preserve unsupported DATE-to-numeric coverage after enabling DATE binary.
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_SLONG, value, sizeof(value), &value_length));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(state));
    EXPECT_STREQ("42", value);
    EXPECT_EQ(2, value_length);
    std::array<unsigned char, sizeof(SQL_DATE_STRUCT) + 2> date_bytes;
    date_bytes.fill(0xa5);
    const auto date_before = date_bytes;
    SQLLEN date_length = 79;
    for (SQLLEN capacity = 0; capacity < static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)); ++capacity) {
        SCOPED_TRACE(capacity);
        ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, 4, SQL_C_BINARY,
            date_bytes.data() + 1, capacity, &date_length));
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
        EXPECT_EQ(date_before, date_bytes);
        EXPECT_EQ(79, date_length);
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_BINARY,
        date_bytes.data() + 1, sizeof(SQL_DATE_STRUCT), &date_length));
    const SQL_DATE_STRUCT expected_date{2024, 2, 29};
    EXPECT_EQ(0, std::memcmp(&expected_date, date_bytes.data() + 1, sizeof(expected_date)));
    EXPECT_EQ(sizeof(expected_date), date_length);
    EXPECT_EQ(0xa5, date_bytes.front());
    EXPECT_EQ(0xa5, date_bytes.back());
    const auto date_completed = date_bytes;
    date_length = 83;
    ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 4, SQL_C_BINARY,
        date_bytes.data() + 1, sizeof(expected_date), &date_length));
    EXPECT_EQ(date_completed, date_bytes);
    EXPECT_EQ(83, date_length);
}

TEST_F(BindColIntegrationTest, ConversionFailuresUseSpecificSqlstates) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'not-an-integer'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLINTEGER integer_value = 0;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &integer_value, 0, nullptr));
    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(sqlstate));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)(fixture_redshift_product()
            ? "SELECT FROM_HEX('01')"
            : "SELECT decode('01', 'hex')"), SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER binary_to_number = 42;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &binary_to_number,
        sizeof(binary_to_number), nullptr));
    EXPECT_EQ(42, binary_to_number);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07006", reinterpret_cast<char*>(sqlstate));
}

TEST_F(BindColIntegrationTest, ConversionDiagnosticsDistinguishFailureKinds) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 32768::integer, 12.75::numeric(4,2), "
                  "'not-a-number'::text, '2025-02-29'::text",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    SQLSMALLINT number = 0;
    SQLINTEGER integer = 0;
    SQL_DATE_STRUCT date{};
    SQLCHAR state[6]{};

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_SSHORT, &number, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 2, SQL_C_SSHORT, &number, 0, nullptr));
    EXPECT_EQ(12, number);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01S07", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 3, SQL_C_SLONG, &integer, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22018", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 4, SQL_C_DATE, &date, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22007", reinterpret_cast<char*>(state));
}

// Test rebinding columns
TEST_F(BindColIntegrationTest, ColumnRebinding) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Test' as col1, 789 as col2", SQL_NTS));
    
    // First binding
    char str_val1[256];
    SQLLEN len1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, str_val1, sizeof(str_val1), &len1));
    
    // Rebind same column to different buffer
    char str_val2[256];
    SQLLEN len2;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, str_val2, sizeof(str_val2), &len2));
    
    SQLINTEGER int_val;
    SQLLEN int_len;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 2, SQL_C_SLONG, &int_val, 0, &int_len));
    
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // Second buffer should be populated (rebinding worked)
    EXPECT_STREQ("Test", str_val2);
    EXPECT_EQ(789, int_val);
}

// Test error conditions
TEST_F(BindColIntegrationTest, ErrorConditions) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Test' as col1", SQL_NTS));
    
    char buffer[256];
    SQLLEN len;
    
    // Invalid column number
    EXPECT_EQ(SQL_ERROR, SQLBindCol(hstmt, 0, SQL_C_CHAR, buffer, sizeof(buffer), &len));
    EXPECT_EQ(SQL_ERROR, SQLBindCol(hstmt, 999, SQL_C_CHAR, buffer, sizeof(buffer), &len));

    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_ERROR, SQLBindCol(
        hstmt, 1, 12345, buffer, sizeof(buffer), &len));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY003", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLBindCol(
        hstmt, 1, SQL_C_CHAR, buffer, -1, &len));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY090", reinterpret_cast<char*>(state));
    
    // Valid binding should still work
    EXPECT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &len));
}

TEST_F(BindColIntegrationTest, NegativeTests) {
    // Test SQLBindCol with invalid handle
    char buffer[256];
    SQLLEN len;
    SQLRETURN ret = SQLBindCol(nullptr, 1, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
    
    // Test SQLBindCol with invalid column number
    ret = SQLBindCol(hstmt, 0, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Verify diagnostic is set
    SQLCHAR sqlstate[6], message[256];
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    if (odbcpp::test::dm::deviates(
            odbcpp::test::dm::Deviation::BookmarkBindTypeRejected)) {
        EXPECT_STREQ("07006", reinterpret_cast<char*>(sqlstate));
        EXPECT_TRUE(std::string_view(reinterpret_cast<char*>(message)).starts_with(
            "[Microsoft][ODBC Driver Manager]"));
    } else {
        EXPECT_STREQ("07009", (char*)sqlstate);
        EXPECT_TRUE(strstr((char*)message, "column number") != nullptr);
    }
    
    // Test SQLFetch without execution
    SQLHSTMT new_stmt;
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &new_stmt);
    ret = SQLFetch(new_stmt);
    EXPECT_EQ(SQL_ERROR, ret);
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, new_stmt, 1, sqlstate,
                        nullptr, message, sizeof(message), nullptr);
    ASSERT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("HY010", reinterpret_cast<char*>(sqlstate));
    SQLFreeHandle(SQL_HANDLE_STMT, new_stmt);
    
    // Test SQLGetData without execution
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &new_stmt);
    ret = SQLGetData(new_stmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Verify diagnostic
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, new_stmt, 1, sqlstate, nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    // The manager's state table answers before the driver: HY010, not 24000.
    EXPECT_STREQ(odbcpp::test::dm::value(
                     odbcpp::test::dm::Deviation::GetDataBeforeExecuteIsSequenceError,
                     "24000", "HY010"),
                 (char*)sqlstate);
    SQLFreeHandle(SQL_HANDLE_STMT, new_stmt);
    
    // Test SQLGetData with invalid column
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    ret = SQLGetData(hstmt, 0, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
    
    ret = SQLGetData(hstmt, 999, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
}

TEST_F(BindColIntegrationTest, LegacySignedIntegerTargetsFetchBindAndNull) {
    for (const auto type : std::array<SQLSMALLINT, 3>{
             SQL_C_LONG, SQL_C_SHORT, SQL_C_TINYINT}) {
        std::array<unsigned char, 8> value{};
        SQLLEN length = 0;
        const auto expect_value = [&](SQLINTEGER expected) {
            if (type == SQL_C_LONG) {
                SQLINTEGER actual;
                std::memcpy(&actual, value.data(), sizeof(actual));
                EXPECT_EQ(expected, actual);
                EXPECT_EQ(sizeof(actual), length);
            } else if (type == SQL_C_SHORT) {
                SQLSMALLINT actual;
                std::memcpy(&actual, value.data(), sizeof(actual));
                EXPECT_EQ(expected, actual);
                EXPECT_EQ(sizeof(actual), length);
            } else {
                SQLSCHAR actual;
                std::memcpy(&actual, value.data(), sizeof(actual));
                EXPECT_EQ(expected, actual);
                EXPECT_EQ(sizeof(actual), length);
            }
        };
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT -7::integer", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, type, value.data(), value.size(), &length));
        expect_value(-7);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, type, value.data(), value.size(), &length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT NULL::integer", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt)); EXPECT_EQ(SQL_NULL_DATA,length);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        value.fill(0xa5);
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT -7::integer", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_NE(SQL_NULL_DATA,length);
        expect_value(-7);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
        // Resolve the legacy target from the ARD without rewriting its type.
        SQLHDESC ard = SQL_NULL_HDESC;
        ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt, SQL_ATTR_APP_ROW_DESC,
            &ard, 0, nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_CONCISE_TYPE,
            reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(type)), 0));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1::bit", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        if (ard_type_get_data_rejected(1, value.data(),
                static_cast<SQLLEN>(value.size()), &length)) {
            return;
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_ARD_TYPE,
            value.data(), value.size(), &length));
        expect_value(1);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 2147483648::bigint", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, type, value.data(), value.size(), &length));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
}


// Native metadata control only: neither PostgreSQL nor the current normalized
// SDK exposes TINYINT. Synthetic TINYINT runtime coverage lives in unit tests.
TEST_F(BindColIntegrationTest, SmallintBinaryKeepsNativeWidthBesideSignedByteTarget) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT -128::smallint", SQL_NTS));
    SQLSMALLINT type{};
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1, nullptr, 0, nullptr, &type, nullptr, nullptr, nullptr));
    ASSERT_EQ(SQL_SMALLINT, type);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    unsigned char bytes[4]{0xa5, 0xa5, 0xa5, 0xa5};
    SQLLEN length = 79;
    ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes + 1, 1, &length));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
    EXPECT_EQ(79, length);
    EXPECT_EQ(0xa5, bytes[1]);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes + 1, 2, &length));
    const SQLSMALLINT expected = -128;
    EXPECT_EQ(0, std::memcmp(bytes + 1, &expected, 2));
    EXPECT_EQ(2, length);
    EXPECT_EQ(0xa5, bytes[0]);
    EXPECT_EQ(0xa5, bytes[3]);
    length = 83;
    ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes + 1, 2, &length));
    EXPECT_EQ(83, length);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT -128::smallint", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLSCHAR tiny{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_STINYINT, &tiny, 0, &length));
    EXPECT_EQ(-128, tiny);
    EXPECT_EQ(1, length);
}


TEST_F(BindColIntegrationTest, DateBinaryResultWholeStructureShortRetryNullAndRefill) {
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0));
    const SQL_DATE_STRUCT expected{2024, 2, 29};
    std::array<unsigned char, sizeof(SQL_DATE_STRUCT) + 2> bytes;
    bytes.fill(0xa5);
    SQLLEN length = 79;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BINARY, bytes.data() + 1, sizeof(expected), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT DATE '2024-02-29'", SQL_NTS));
    SQLSMALLINT type{};
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1, nullptr, 0, nullptr, &type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_TYPE_DATE, type);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0, std::memcmp(&expected, bytes.data() + 1, sizeof(expected)));
    EXPECT_EQ(sizeof(expected), length);
    EXPECT_EQ(0xa5, bytes.front());
    EXPECT_EQ(0xa5, bytes.back());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT DATE '2000-02-29'", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    bytes.fill(0xa5);
    const auto before = bytes;
    length = 79;
    for (SQLLEN capacity = 0; capacity < static_cast<SQLLEN>(sizeof(SQL_DATE_STRUCT)); ++capacity) {
        ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes.data() + 1, capacity, &length));
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
        EXPECT_EQ(before, bytes);
        EXPECT_EQ(79, length);
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes.data() + 1, sizeof(expected), &length));
    const SQL_DATE_STRUCT refill{2000, 2, 29};
    EXPECT_EQ(0, std::memcmp(&refill, bytes.data() + 1, sizeof(refill)));
    EXPECT_EQ(sizeof(refill), length);
    const auto completed = bytes;
    length = 83;
    ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes.data() + 1, sizeof(expected), &length));
    EXPECT_EQ(completed, bytes);
    EXPECT_EQ(83, length);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT CAST(NULL AS DATE)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY,
        reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)()), 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(40));
}

TEST_F(BindColIntegrationTest, TimeTimestampBinaryWholeFieldsShortRetryNullAndRefill) {
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0));
    struct Case { const char* query; const char* null_query; SQLSMALLINT type; SQLLEN width; };
    const Case cases[] = {
        {"SELECT TIME '12:34:56'", "SELECT CAST(NULL AS TIME)", SQL_TYPE_TIME, sizeof(SQL_TIME_STRUCT)},
        {"SELECT TIMESTAMP '2024-02-29 12:34:56.123456'", "SELECT CAST(NULL AS TIMESTAMP)",
         SQL_TYPE_TIMESTAMP, sizeof(SQL_TIMESTAMP_STRUCT)}};
    const SQL_TIME_STRUCT time{12, 34, 56};
    const SQL_TIMESTAMP_STRUCT timestamp{2024, 2, 29, 12, 34, 56, 123456000};
    for (const auto& c : cases) {
        SCOPED_TRACE(c.type);
        std::array<unsigned char, sizeof(SQL_TIMESTAMP_STRUCT) + 2> bytes;
        bytes.fill(0xa5);
        const auto before = bytes;
        SQLLEN length = 79;
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)c.query, SQL_NTS));
        SQLSMALLINT type{};
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1, nullptr, 0, nullptr, &type, nullptr, nullptr, nullptr));
        EXPECT_EQ(c.type, type);
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (SQLLEN capacity = 0; capacity < c.width; ++capacity) {
            ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes.data() + 1, capacity, &length));
            SQLCHAR state[6]{};
            ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
            EXPECT_STREQ("22003", reinterpret_cast<char*>(state));
            EXPECT_EQ(before, bytes);
            EXPECT_EQ(79, length);
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes.data() + 1, c.width, &length));
        const void* expected = c.type == SQL_TYPE_TIME ? static_cast<const void*>(&time) : &timestamp;
        EXPECT_EQ(0, std::memcmp(expected, bytes.data() + 1, c.width));
        EXPECT_EQ(c.width, length);
        EXPECT_EQ(0xa5, bytes.front());
        EXPECT_EQ(0xa5, bytes[c.width + 1]);
        const auto completed = bytes;
        length = 83;
        ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_BINARY, bytes.data() + 1, c.width, &length));
        EXPECT_EQ(completed, bytes);
        EXPECT_EQ(83, length);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BINARY, bytes.data() + 1, c.width, &length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)c.query, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(0, std::memcmp(expected, bytes.data() + 1, c.width));
        EXPECT_EQ(c.width, length);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)c.null_query, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_BINARY,
            reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)()), 0, &length));
        EXPECT_EQ(SQL_NULL_DATA, length);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60));
}

TEST_F(BindColIntegrationTest, NonfiniteNumericBoundAndGetDataRangeRecovery) {
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0));
    const char* query = "SELECT 'NaN'::real, 'Infinity'::double precision, "
        "'-Infinity'::real, 42::double precision, CAST(NULL AS real)";
    const auto state = [&]() {
        SQLCHAR value[6]{};
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            value, nullptr, nullptr, 0, nullptr));
        return std::string(reinterpret_cast<char*>(value));
    };
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQL_NUMERIC_STRUCT numeric{};
    std::memset(&numeric, 0xa5, sizeof(numeric));
    const auto original = numeric;
    SQLLEN length = 79;
    for (SQLUSMALLINT column = 1; column <= 3; ++column) {
        ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_NUMERIC, &numeric, 0, &length));
        EXPECT_EQ("22003", state());
        EXPECT_EQ(79, length);
        EXPECT_EQ(0, std::memcmp(&original, &numeric, sizeof(numeric)));
        SQLDOUBLE floating = 7;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_DOUBLE, &floating, 0, &length));
        if (column == 1) {
            EXPECT_TRUE(std::isnan(floating));
        } else {
            EXPECT_TRUE(std::isinf(floating));
            EXPECT_EQ(column == 3, std::signbit(floating));
        }
        length = 79;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_NUMERIC, &numeric, 0, &length));
    EXPECT_EQ(1, numeric.sign);
    EXPECT_EQ(42, numeric.val[0]);
    EXPECT_EQ(sizeof(numeric), length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, SQL_C_NUMERIC,
        reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)()), 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    for (bool indicator_only : {false, true}) {
        std::memset(&numeric, 0xa5, sizeof(numeric));
        length = 79;
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_NUMERIC,
            indicator_only ? nullptr : &numeric, 0, &length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
        ASSERT_EQ(SQL_ERROR, SQLFetch(hstmt));
        EXPECT_EQ("22003", state());
        EXPECT_EQ(79, length);
        EXPECT_EQ(0, std::memcmp(&original, &numeric, sizeof(numeric)));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
    }
    SQLREAL recovered = 7;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_FLOAT, &recovered, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_TRUE(std::isnan(recovered));
    EXPECT_EQ(sizeof(recovered), length);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60));
}

TEST_F(BindColIntegrationTest, CharacterNonfiniteFloatingRefusalAndNativeIeeeRecovery) {
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0));
    const char* query = "SELECT 'NaN'::varchar(16), 'Infinity'::varchar(16), "
        "'-Infinity'::varchar(16), 'NaN'::real, '1.25'::varchar(16), CAST(NULL AS varchar(16))";
    const auto state = [&]() {
        SQLCHAR value[6]{};
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            value, nullptr, nullptr, 0, nullptr));
        return std::string(reinterpret_cast<char*>(value));
    };
    for (SQLSMALLINT target : {SQLSMALLINT(SQL_C_FLOAT), SQLSMALLINT(SQL_C_DOUBLE)}) {
        const std::size_t width = target == SQL_C_FLOAT ? sizeof(SQLREAL) : sizeof(SQLDOUBLE);
        std::array<unsigned char, sizeof(SQLDOUBLE) + 2> bytes;
        bytes.fill(0xa5);
        const auto before = bytes;
        SQLLEN length = 79;
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        const char* tokens[] = {"NaN", "Infinity", "-Infinity"};
        for (SQLUSMALLINT column = 1; column <= 3; ++column) {
            SQLSMALLINT source_type{};
            ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, column, nullptr, 0,
                nullptr, &source_type, nullptr, nullptr, nullptr));
            EXPECT_EQ(SQL_VARCHAR, source_type);
            length = 79;
            ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, column, target, bytes.data() + 1, 0, &length));
            EXPECT_EQ("22018", state());
            EXPECT_EQ(before, bytes);
            EXPECT_EQ(79, length);
            char text[16]{};
            ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_CHAR, text, sizeof(text), &length));
            EXPECT_STREQ(tokens[column - 1], text);
            ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, column, target, bytes.data() + 1, 0, &length));
        }
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, target, bytes.data() + 1, 0, &length));
        SQLDOUBLE special{};
        if (target == SQL_C_FLOAT) {
            SQLREAL single{};
            std::memcpy(&single, bytes.data() + 1, sizeof(single));
            special = single;
        } else {
            std::memcpy(&special, bytes.data() + 1, sizeof(special));
        }
        EXPECT_TRUE(std::isnan(special));
        EXPECT_EQ(width, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, target, bytes.data() + 1, 0, &length));
        SQLDOUBLE finite{};
        if (target == SQL_C_FLOAT) {
            SQLREAL single{};
            std::memcpy(&single, bytes.data() + 1, sizeof(single));
            finite = single;
        } else {
            std::memcpy(&finite, bytes.data() + 1, sizeof(finite));
        }
        EXPECT_DOUBLE_EQ(1.25, finite);
        EXPECT_EQ(width, length);
        const auto finite_bytes = bytes;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, target, bytes.data() + 1, 0, &length));
        EXPECT_EQ(SQL_NULL_DATA, length);
        EXPECT_EQ(finite_bytes, bytes);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        for (bool indicator_only : {false, true}) {
            bytes.fill(0xa5);
            length = 79;
            ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, target,
                indicator_only ? nullptr : bytes.data() + 1, 0, &length));
            ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
            ASSERT_EQ(SQL_ERROR, SQLFetch(hstmt));
            EXPECT_EQ("22018", state());
            EXPECT_EQ(before, bytes);
            EXPECT_EQ(79, length);
            ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
            ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
        }
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60));
}

TEST_F(BindColIntegrationTest, CharacterNonfiniteIntegralRefusalAndNumericRecovery) {
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0));
    const char* query = "SELECT 'NaN'::varchar(16), 'Infinity'::varchar(16), "
        "'-Infinity'::varchar(16), 'NaN'::real, '1'::varchar(16), CAST(NULL AS varchar(16))";
    const auto state = [&]() {
        SQLCHAR value[6]{};
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            value, nullptr, nullptr, 0, nullptr));
        return std::string(reinterpret_cast<char*>(value));
    };
    for (SQLSMALLINT target : {SQLSMALLINT(SQL_C_SLONG), SQLSMALLINT(SQL_C_BIT)}) {
        const std::size_t width = target == SQL_C_SLONG ? sizeof(SQLINTEGER) : sizeof(SQLCHAR);
        std::array<unsigned char, sizeof(SQLDOUBLE) + 2> bytes;
        bytes.fill(0xa5);
        const auto before = bytes;
        SQLLEN length = 79;
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        const char* tokens[] = {"NaN", "Infinity", "-Infinity"};
        for (SQLUSMALLINT column = 1; column <= 3; ++column) {
            SQLSMALLINT source_type{};
            ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, column, nullptr, 0,
                nullptr, &source_type, nullptr, nullptr, nullptr));
            EXPECT_EQ(SQL_VARCHAR, source_type);
            length = 79;
            ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, column, target, bytes.data() + 1, 0, &length));
            EXPECT_EQ("22018", state());
            EXPECT_EQ(before, bytes);
            EXPECT_EQ(79, length);
            char text[16]{};
            ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_CHAR, text, sizeof(text), &length));
            EXPECT_STREQ(tokens[column - 1], text);
            ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, column, target, bytes.data() + 1, 0, &length));
        }
        length = 79;
        ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, 4, target, bytes.data() + 1, 0, &length));
        EXPECT_EQ("22003", state()); // Normalized REAL retains its numeric-source policy.
        EXPECT_EQ(before, bytes);
        EXPECT_EQ(79, length);
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, target, bytes.data() + 1, 0, &length));
        if (target == SQL_C_SLONG) {
            SQLINTEGER finite{};
            std::memcpy(&finite, bytes.data() + 1, sizeof(finite));
            EXPECT_EQ(1, finite);
        } else {
            SQLCHAR finite{};
            std::memcpy(&finite, bytes.data() + 1, sizeof(finite));
            EXPECT_EQ(1, finite);
        }
        EXPECT_EQ(width, length);
        const auto finite_bytes = bytes;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, target, bytes.data() + 1, 0, &length));
        EXPECT_EQ(SQL_NULL_DATA, length);
        EXPECT_EQ(finite_bytes, bytes);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        for (bool indicator_only : {false, true}) {
            bytes.fill(0xa5);
            length = 79;
            ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, target,
                indicator_only ? nullptr : bytes.data() + 1, 0, &length));
            ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
            ASSERT_EQ(SQL_ERROR, SQLFetch(hstmt));
            EXPECT_EQ("22018", state());
            EXPECT_EQ(before, bytes);
            EXPECT_EQ(79, length);
            ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
            ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
        }
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60));
}

TEST_F(BindColIntegrationTest, CharacterBooleanWordsRefusalAndNativeBooleanRecovery) {
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0));
    const char* query = "SELECT 't'::varchar(5), 'true'::varchar(5), 'f'::varchar(5), "
        "'false'::varchar(5), TRUE, FALSE, '1'::varchar(5), CAST(NULL AS varchar(5))";
    const auto state = [&]() {
        SQLCHAR value[6]{};
        EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1,
            value, nullptr, nullptr, 0, nullptr));
        return std::string(reinterpret_cast<char*>(value));
    };
    std::array<unsigned char, 3> bytes{0xa5, 0xa5, 0xa5};
    const auto before = bytes;
    SQLLEN length = 79;
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    const char* tokens[] = {"t", "true", "f", "false"};
    for (SQLUSMALLINT column = 1; column <= 4; ++column) {
        SQLSMALLINT source_type{};
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, column, nullptr, 0,
            nullptr, &source_type, nullptr, nullptr, nullptr));
        EXPECT_EQ(SQL_VARCHAR, source_type);
        length = 79;
        ASSERT_EQ(SQL_ERROR, SQLGetData(hstmt, column, SQL_C_BIT, bytes.data() + 1, 0, &length));
        EXPECT_EQ("22018", state());
        EXPECT_EQ(before, bytes);
        EXPECT_EQ(79, length);
        char text[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_CHAR, text, sizeof(text), &length));
        EXPECT_STREQ(tokens[column - 1], text);
        ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, column, SQL_C_BIT, bytes.data() + 1, 0, &length));
    }
    for (SQLUSMALLINT column = 5; column <= 7; ++column) {
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_BIT, bytes.data() + 1, 0, &length));
        EXPECT_EQ(column == 6 ? 0 : 1, bytes[1]);
        EXPECT_EQ(1, length);
        EXPECT_EQ(0xa5, bytes.front());
        EXPECT_EQ(0xa5, bytes.back());
    }
    const auto finite_bytes = bytes;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 8, SQL_C_BIT, bytes.data() + 1, 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(finite_bytes, bytes);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    for (bool indicator_only : {false, true}) {
        bytes = before;
        length = 79;
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_BIT,
            indicator_only ? nullptr : bytes.data() + 1, 0, &length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
        ASSERT_EQ(SQL_ERROR, SQLFetch(hstmt));
        EXPECT_EQ("22018", state());
        EXPECT_EQ(before, bytes);
        EXPECT_EQ(79, length);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60));
}

TEST_F(BindColIntegrationTest, TimeTimestampFractionProjectionAndSourceRecovery) {
    const auto started = std::chrono::steady_clock::now();
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(hstmt, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)5, 0));
    const char* query = "SELECT CAST('12:34:56.123456' AS time), '12:34:56.123456'::varchar(15), "
        "'2024-02-29 12:34:56.123456'::timestamp, CAST(NULL AS time)";
    std::array<unsigned char, sizeof(SQL_TIMESTAMP_STRUCT) + 2> bytes;
    bytes.fill(0xa5);
    SQLLEN length = 79;
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLSMALLINT source_type{};
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1, nullptr, 0, nullptr,
        &source_type, nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_TYPE_TIME, source_type);
    const auto before = odbcpp::test::local_calendar(std::time(nullptr));
    ASSERT_TRUE(before);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP, bytes.data() + 1, 0, &length));
    const auto after = odbcpp::test::local_calendar(std::time(nullptr));
    ASSERT_TRUE(after);
    SQL_TIMESTAMP_STRUCT value{};
    std::memcpy(&value, bytes.data() + 1, sizeof(value));
    const auto matches = [&](const std::tm& date) {
        return value.year == date.tm_year + 1900 && value.month == date.tm_mon + 1 && value.day == date.tm_mday;
    };
    EXPECT_TRUE(matches(*before) || matches(*after));
    EXPECT_EQ(12, value.hour);
    EXPECT_EQ(34, value.minute);
    EXPECT_EQ(56, value.second);
    EXPECT_EQ(0u, value.fraction);
    EXPECT_EQ(sizeof(value), length);
    EXPECT_EQ(0xa5, bytes.front());
    EXPECT_EQ(0xa5, bytes.back());
    ASSERT_EQ(SQL_NO_DATA, SQLGetData(hstmt, 1, SQL_C_TYPE_TIMESTAMP, bytes.data() + 1, 0, &length));
    for (SQLUSMALLINT column : {SQLUSMALLINT{2}, SQLUSMALLINT{3}}) {
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, column, SQL_C_TYPE_TIMESTAMP, bytes.data() + 1, 0, &length));
        std::memcpy(&value, bytes.data() + 1, sizeof(value));
        EXPECT_EQ(123456000u, value.fraction);
        EXPECT_EQ(sizeof(value), length);
    }
    const auto published = bytes;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_TYPE_TIMESTAMP, bytes.data() + 1, 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(published, bytes);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    for (bool indicator_only : {false, true}) {
        bytes.fill(0xa5);
        const auto untouched = bytes;
        length = 79;
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(hstmt, 1, SQL_C_TYPE_TIMESTAMP,
            indicator_only ? nullptr : bytes.data() + 1, 0, &length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)query, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(sizeof(SQL_TIMESTAMP_STRUCT), length);
        if (indicator_only) {
            EXPECT_EQ(untouched, bytes);
        } else {
            std::memcpy(&value, bytes.data() + 1, sizeof(value));
            EXPECT_EQ(12, value.hour);
            EXPECT_EQ(0u, value.fraction);
        }
        EXPECT_EQ(0xa5, bytes.front());
        EXPECT_EQ(0xa5, bytes.back());
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_UNBIND));
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(60));
}

TEST_F(BindColIntegrationTest, DecimalBinarySourceDimensionsAndRecovery) {
    const auto started=std::chrono::steady_clock::now();
    const auto state = [&]() {
        SQLCHAR value[6]{};
        EXPECT_EQ(SQL_SUCCESS,SQLGetDiagRec(SQL_HANDLE_STMT,hstmt,1,
            value,nullptr,nullptr,0,nullptr));
        return std::string(reinterpret_cast<char*>(value));
    };
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(hstmt,SQL_ATTR_QUERY_TIMEOUT,(SQLPOINTER)5,0));
    const char* query="SELECT CAST('-12.34' AS numeric(10,2)), CAST(NULL AS numeric(10,2))";
    std::array<unsigned char,sizeof(SQL_NUMERIC_STRUCT)+2> bytes;
    bytes.fill(0xa5);
    const auto untouched=bytes;
    SQLLEN length=79;
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt,(SQLCHAR*)query,SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt));
    SQLSMALLINT type{},scale{};SQLULEN precision{};
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeCol(hstmt,1,nullptr,0,nullptr,&type,&precision,&scale,nullptr));
    EXPECT_EQ(SQL_NUMERIC,type);
    EXPECT_EQ(10u,precision);
    EXPECT_EQ(2,scale);
    for (SQLLEN capacity=0;capacity<SQLLEN(sizeof(SQL_NUMERIC_STRUCT));++capacity) {
        ASSERT_EQ(SQL_ERROR,SQLGetData(hstmt,1,SQL_C_BINARY,bytes.data()+1,capacity,&length));
        EXPECT_EQ("22003",state());
        EXPECT_EQ(untouched,bytes);
        EXPECT_EQ(79,length);
    }
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt,1,SQL_C_BINARY,bytes.data()+1,sizeof(SQL_NUMERIC_STRUCT),&length));
    SQL_NUMERIC_STRUCT expected{};
    expected.precision=10;expected.scale=2;expected.sign=0;expected.val[0]=0xd2;expected.val[1]=0x04;
    EXPECT_EQ(0,std::memcmp(bytes.data()+1,&expected,sizeof(expected)));
    EXPECT_EQ(sizeof(expected),length);
    EXPECT_EQ(0xa5,bytes.front());
    EXPECT_EQ(0xa5,bytes.back());
    ASSERT_EQ(SQL_NO_DATA,SQLGetData(hstmt,1,SQL_C_BINARY,bytes.data()+1,0,&length));
    const auto published=bytes;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(hstmt,2,SQL_C_BINARY,bytes.data()+1,0,&length));
    EXPECT_EQ(SQL_NULL_DATA,length);
    EXPECT_EQ(published,bytes);
    ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt));
    for (bool indicator_only : {false,true}) {
        bytes=untouched;length=79;
        ASSERT_EQ(SQL_SUCCESS,SQLBindCol(hstmt,1,SQL_C_BINARY,
            indicator_only ? nullptr : bytes.data()+1,sizeof(SQL_NUMERIC_STRUCT),&length));
        ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(hstmt,(SQLCHAR*)query,SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS,SQLFetch(hstmt));
        EXPECT_EQ(sizeof(expected),length);
        if (indicator_only) {
            EXPECT_EQ(untouched,bytes);
        } else {
            EXPECT_EQ(0,std::memcmp(bytes.data()+1,&expected,sizeof(expected)));
        }
        ASSERT_EQ(SQL_SUCCESS,SQLCloseCursor(hstmt));
        ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(hstmt,SQL_UNBIND));
    }
    EXPECT_LT(std::chrono::steady_clock::now()-started,std::chrono::seconds(60));
}
