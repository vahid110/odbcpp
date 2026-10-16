#include <gtest/gtest.h>
#include <string_view>
#include <cstdlib>
#include "odbcpp/database/backend_provider.h"
#include <algorithm>
#include <chrono>
#include "odbc/odbc_api.h"
#include "odbc/connection_string.h"
#include "core/database/database_factory.h"
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include "tests/test_connection_config.h"
#include "tests/test_handle_helpers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>


namespace {
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
    if (SQLGetConnectAttr(connection, SQL_ATTR_LOGIN_TIMEOUT, &current, sizeof(current), nullptr) != SQL_SUCCESS) return true;
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
        const auto* schema = std::getenv("ODBCPP_REDSHIFT_TIMEOUT_SCHEMA");
        const auto* table = std::getenv("ODBCPP_REDSHIFT_TIMEOUT_TABLE");
        const auto* oid = std::getenv("ODBCPP_REDSHIFT_TIMEOUT_OID");
        if (!schema || std::string_view(schema) != "odbcpp_fixture" || !table ||
            !timeout_table_name(table) || !oid || !timeout_positive_id(oid)) return fail("required public schema/table/OID");
        table_ = table;
        oid_ = std::to_string(*timeout_positive_id(oid));
        relation_ = "\"odbcpp_fixture\".\"" + table_ + "\"";
        property("timeout_fixture_schema", schema);
        property("timeout_fixture_table", table_);
        property("timeout_fixture_oid", oid_);
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
        if (!identity(waiter, "timeout_waiter_before") || !server_timeout(waiter, "timeout_waiter_before")) return false;
        if (!accept(::SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc_), "allocate blocker")) return false;
        if (!fixture_connection_budget(end_, dbc_, true)) return fail("blocker login budget");
        known_ = false;
        if (!accept(::SQLConnect(dbc_, odbcpp::test::configured_connection_string_data(), SQL_NTS,
                nullptr, 0, nullptr, 0), "blocker login")) return false;
        connected_ = true;
        if (!accept(::SQLAllocHandle(SQL_HANDLE_STMT, dbc_, &stmt_), "allocate blocker statement") ||
            !identity(stmt_, "timeout_blocker") || !server_timeout(stmt_, "timeout_blocker")) return false;
        std::array<std::string, 2> object;
        if (!query(stmt_, "SELECT c.oid, u.usename FROM pg_catalog.pg_class c "
            "JOIN pg_catalog.pg_namespace n ON n.oid=c.relnamespace "
            "JOIN pg_catalog.pg_user u ON u.usesysid=c.relowner WHERE c.oid=" + oid_ +
            " AND n.nspname='odbcpp_fixture' AND c.relname='" + table_ + "' AND c.relkind='r'") ||
            !row(stmt_, object) || !end_row(stmt_)) return false;
        if (object[0] != oid_ || object[1] != owner_) return fail("ROOT relation identity/ordinary owner mismatch");
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
        if (!query(statement, "SELECT current_user, current_database(), pg_backend_pid(), starttime FROM stv_sessions "
            "WHERE process=pg_backend_pid() AND user_name=current_user AND db_name=current_database()") ||
            !row(statement, fields) || !end_row(statement)) return false;
        if (!timeout_positive_id(fields[2]) || fields[3].empty()) return fail("session birth identity");
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


namespace {

std::string diagnostic_state(SQLSMALLINT handle_type, SQLHANDLE handle) {
    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(handle_type, handle, 1, state, nullptr, nullptr,
                            0, nullptr));
    return reinterpret_cast<const char*>(state);
}

std::optional<SQLINTEGER> integer_cell(SQLHSTMT statement,
                                       SQLUSMALLINT column) {
    SQLINTEGER value = 0;
    SQLLEN indicator = 0;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetData(statement, column, SQL_C_SLONG, &value,
                         sizeof(value), &indicator));
    if (indicator == SQL_NULL_DATA) return std::nullopt;
    return value;
}

std::optional<std::string> text_cell(SQLHSTMT statement,
                                     SQLUSMALLINT column) {
    std::array<char, 64> value{};
    SQLLEN indicator = 0;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetData(statement, column, SQL_C_CHAR, value.data(),
                         value.size(), &indicator));
    if (indicator == SQL_NULL_DATA) return std::nullopt;
    return std::string(value.data());
}

}  // namespace

class MetadataIntegrationTest : public ::testing::Test {
protected:
    const std::chrono::steady_clock::time_point fixture_end_ =
        std::chrono::steady_clock::now() + std::chrono::seconds(40);

    // Fixture-local forwards cover repetitions, reconnects and sibling handles.
    // The original assertions/arguments below still call the actual public API.
    SQLRETURN SQLColumns(SQLHSTMT statement, SQLCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLCHAR* schema_name, SQLSMALLINT name_length2, SQLCHAR* table_name,
        SQLSMALLINT name_length3, SQLCHAR* column_name, SQLSMALLINT name_length4) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLColumns(statement, catalog_name, name_length1, schema_name, name_length2,
              table_name, name_length3, column_name, name_length4) : SQL_ERROR;
    }
    SQLRETURN SQLColumnsW(SQLHSTMT statement, SQLWCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLWCHAR* schema_name, SQLSMALLINT name_length2, SQLWCHAR* table_name,
        SQLSMALLINT name_length3, SQLWCHAR* column_name, SQLSMALLINT name_length4) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLColumnsW(statement, catalog_name, name_length1, schema_name, name_length2,
              table_name, name_length3, column_name, name_length4) : SQL_ERROR;
    }
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
    SQLRETURN SQLDescribeColW(SQLHSTMT statement, SQLUSMALLINT column_number,
        SQLWCHAR* column_name, SQLSMALLINT name_buffer_length, SQLSMALLINT* name_length,
        SQLSMALLINT* data_type, SQLULEN* column_size, SQLSMALLINT* decimal_digits,
        SQLSMALLINT* nullable) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLDescribeColW(statement, column_number, column_name, name_buffer_length,
              name_length, data_type, column_size, decimal_digits, nullable) : SQL_ERROR;
    }
    SQLRETURN SQLDescribeParam(SQLHSTMT statement, SQLUSMALLINT parameter_number,
        SQLSMALLINT* data_type, SQLULEN* parameter_size, SQLSMALLINT* decimal_digits,
        SQLSMALLINT* nullable) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLDescribeParam(statement, parameter_number, data_type, parameter_size,
              decimal_digits, nullable) : SQL_ERROR;
    }
    SQLRETURN SQLExecDirect(SQLHSTMT statement, SQLCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecDirect(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLExecDirectW(SQLHSTMT statement, SQLWCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecDirectW(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLExecute(SQLHSTMT statement) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLExecute(statement) : SQL_ERROR;
    }
    SQLRETURN SQLFetch(SQLHSTMT statement) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLFetch(statement) : SQL_ERROR;
    }
    SQLRETURN SQLFetchScroll(SQLHSTMT statement, SQLSMALLINT fetch_orientation,
        SQLLEN fetch_offset) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLFetchScroll(statement, fetch_orientation, fetch_offset) : SQL_ERROR;
    }
    SQLRETURN SQLForeignKeys(SQLHSTMT statement, SQLCHAR* pk_catalog_name,
        SQLSMALLINT name_length1, SQLCHAR* pk_schema_name, SQLSMALLINT name_length2,
        SQLCHAR* pk_table_name, SQLSMALLINT name_length3, SQLCHAR* fk_catalog_name,
        SQLSMALLINT name_length4, SQLCHAR* fk_schema_name, SQLSMALLINT name_length5,
        SQLCHAR* fk_table_name, SQLSMALLINT name_length6) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLForeignKeys(statement, pk_catalog_name, name_length1, pk_schema_name,
              name_length2, pk_table_name, name_length3, fk_catalog_name, name_length4,
              fk_schema_name, name_length5, fk_table_name, name_length6) : SQL_ERROR;
    }
    SQLRETURN SQLForeignKeysW(SQLHSTMT statement, SQLWCHAR* pk_catalog_name,
        SQLSMALLINT name_length1, SQLWCHAR* pk_schema_name, SQLSMALLINT name_length2,
        SQLWCHAR* pk_table_name, SQLSMALLINT name_length3, SQLWCHAR* fk_catalog_name,
        SQLSMALLINT name_length4, SQLWCHAR* fk_schema_name, SQLSMALLINT name_length5,
        SQLWCHAR* fk_table_name, SQLSMALLINT name_length6) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLForeignKeysW(statement, pk_catalog_name, name_length1, pk_schema_name,
              name_length2, pk_table_name, name_length3, fk_catalog_name, name_length4,
              fk_schema_name, name_length5, fk_table_name, name_length6) : SQL_ERROR;
    }
    SQLRETURN SQLNumResultCols(SQLHSTMT statement, SQLSMALLINT* column_count) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLNumResultCols(statement, column_count) : SQL_ERROR;
    }
    SQLRETURN SQLPrepare(SQLHSTMT statement, SQLCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLPrepare(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLPrepareW(SQLHSTMT statement, SQLWCHAR* statement_text, SQLINTEGER text_length) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLPrepareW(statement, statement_text, text_length) : SQL_ERROR;
    }
    SQLRETURN SQLPrimaryKeys(SQLHSTMT statement, SQLCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLCHAR* schema_name, SQLSMALLINT name_length2, SQLCHAR* table_name,
        SQLSMALLINT name_length3) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLPrimaryKeys(statement, catalog_name, name_length1, schema_name, name_length2,
              table_name, name_length3) : SQL_ERROR;
    }
    SQLRETURN SQLPrimaryKeysW(SQLHSTMT statement, SQLWCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLWCHAR* schema_name, SQLSMALLINT name_length2, SQLWCHAR* table_name,
        SQLSMALLINT name_length3) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLPrimaryKeysW(statement, catalog_name, name_length1, schema_name, name_length2,
              table_name, name_length3) : SQL_ERROR;
    }
    SQLRETURN SQLProcedureColumns(SQLHSTMT statement, SQLCHAR* catalog_name,
        SQLSMALLINT name_length1, SQLCHAR* schema_name, SQLSMALLINT name_length2,
        SQLCHAR* procedure_name, SQLSMALLINT name_length3, SQLCHAR* column_name,
        SQLSMALLINT name_length4) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLProcedureColumns(statement, catalog_name, name_length1, schema_name, name_length2,
              procedure_name, name_length3, column_name, name_length4) : SQL_ERROR;
    }
    SQLRETURN SQLProcedureColumnsW(SQLHSTMT statement, SQLWCHAR* catalog_name,
        SQLSMALLINT name_length1, SQLWCHAR* schema_name, SQLSMALLINT name_length2,
        SQLWCHAR* procedure_name, SQLSMALLINT name_length3, SQLWCHAR* column_name,
        SQLSMALLINT name_length4) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLProcedureColumnsW(statement, catalog_name, name_length1, schema_name,
              name_length2, procedure_name, name_length3, column_name, name_length4) : SQL_ERROR;
    }
    SQLRETURN SQLProcedures(SQLHSTMT statement, SQLCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLCHAR* schema_name, SQLSMALLINT name_length2, SQLCHAR* procedure_name,
        SQLSMALLINT name_length3) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLProcedures(statement, catalog_name, name_length1, schema_name, name_length2,
              procedure_name, name_length3) : SQL_ERROR;
    }
    SQLRETURN SQLProceduresW(SQLHSTMT statement, SQLWCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLWCHAR* schema_name, SQLSMALLINT name_length2, SQLWCHAR* procedure_name,
        SQLSMALLINT name_length3) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLProceduresW(statement, catalog_name, name_length1, schema_name, name_length2,
              procedure_name, name_length3) : SQL_ERROR;
    }
    SQLRETURN SQLSpecialColumns(SQLHSTMT statement, SQLUSMALLINT identifier_type,
        SQLCHAR* catalog_name, SQLSMALLINT name_length1, SQLCHAR* schema_name,
        SQLSMALLINT name_length2, SQLCHAR* table_name, SQLSMALLINT name_length3,
        SQLUSMALLINT scope, SQLUSMALLINT nullable) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLSpecialColumns(statement, identifier_type, catalog_name, name_length1,
              schema_name, name_length2, table_name, name_length3, scope, nullable) : SQL_ERROR;
    }
    SQLRETURN SQLSpecialColumnsW(SQLHSTMT statement, SQLUSMALLINT identifier_type,
        SQLWCHAR* catalog_name, SQLSMALLINT name_length1, SQLWCHAR* schema_name,
        SQLSMALLINT name_length2, SQLWCHAR* table_name, SQLSMALLINT name_length3,
        SQLUSMALLINT scope, SQLUSMALLINT nullable) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLSpecialColumnsW(statement, identifier_type, catalog_name, name_length1,
              schema_name, name_length2, table_name, name_length3, scope, nullable) : SQL_ERROR;
    }
    SQLRETURN SQLStatistics(SQLHSTMT statement, SQLCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLCHAR* schema_name, SQLSMALLINT name_length2, SQLCHAR* table_name,
        SQLSMALLINT name_length3, SQLUSMALLINT unique, SQLUSMALLINT reserved) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLStatistics(statement, catalog_name, name_length1, schema_name, name_length2,
              table_name, name_length3, unique, reserved) : SQL_ERROR;
    }
    SQLRETURN SQLStatisticsW(SQLHSTMT statement, SQLWCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLWCHAR* schema_name, SQLSMALLINT name_length2, SQLWCHAR* table_name,
        SQLSMALLINT name_length3, SQLUSMALLINT unique, SQLUSMALLINT reserved) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLStatisticsW(statement, catalog_name, name_length1, schema_name, name_length2,
              table_name, name_length3, unique, reserved) : SQL_ERROR;
    }
    SQLRETURN SQLTables(SQLHSTMT statement, SQLCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLCHAR* schema_name, SQLSMALLINT name_length2, SQLCHAR* table_name,
        SQLSMALLINT name_length3, SQLCHAR* table_type, SQLSMALLINT name_length4) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLTables(statement, catalog_name, name_length1, schema_name, name_length2,
              table_name, name_length3, table_type, name_length4) : SQL_ERROR;
    }
    SQLRETURN SQLTablesW(SQLHSTMT statement, SQLWCHAR* catalog_name, SQLSMALLINT name_length1,
        SQLWCHAR* schema_name, SQLSMALLINT name_length2, SQLWCHAR* table_name,
        SQLSMALLINT name_length3, SQLWCHAR* table_type, SQLSMALLINT name_length4) {
      return fixture_statement_budget(fixture_end_, statement)
          ? ::SQLTablesW(statement, catalog_name, name_length1, schema_name, name_length2,
              table_name, name_length3, table_type, name_length4) : SQL_ERROR;
    }

    static bool redshift_profile() {
        return rs::core::database::DatabaseFactory::get_compiled_database_type() ==
            rs::core::database::DatabaseType::Redshift;
    }

    void resolve_redshift_temporary_schema(const char* table, std::string& schema) {
        // PG_TABLE_DEF is documented to expose the current session's visible
        // temporary tables. Resolve its real namespace; never guess pg_temp or
        // replace a session-local object with a persistent shared fixture.
        std::string literal{"'"};
        for (const char* ch = table; *ch; ++ch) {
            if (*ch == '\'') {
                literal += '\'';
            }
            literal += *ch;
        }
        literal += '\'';
        const std::string query = "SELECT DISTINCT schemaname FROM pg_table_def WHERE tablename = " +
            literal + " AND LEFT(schemaname, 8) = 'pg_temp_'";
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
            reinterpret_cast<SQLCHAR*>(const_cast<char*>(query.c_str())), SQL_NTS))
            << diagnostic_state(SQL_HANDLE_STMT, hstmt);
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        const auto resolved = text_cell(hstmt, 1);
        ASSERT_TRUE(resolved.has_value());
        ASSERT_EQ(0u, resolved->find("pg_temp_"));
        ASSERT_GT(resolved->size(), 8u);
        schema = *resolved;
        ASSERT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }

    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        
        // Connect to test database
        SQLRETURN ret = SQLConnect(
            hdbc, odbcpp::test::configured_connection_string_data(), SQL_NTS,
            nullptr, 0, nullptr, 0);
        ASSERT_EQ(SQL_SUCCESS, ret) << "Failed to connect to test database";
        
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

TEST_F(MetadataIntegrationTest, BasicMetadata) {
    // Execute a known query
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Hello' as greeting, 42 as answer", SQL_NTS);
    ASSERT_EQ(SQL_SUCCESS, ret);
    
    // Test column count
    SQLSMALLINT num_cols;
    ret = SQLNumResultCols(hstmt, &num_cols);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ(2, num_cols);
    
    // Test first column description
    SQLCHAR column_name[256];
    SQLSMALLINT name_length;
    SQLSMALLINT data_type;
    SQLULEN column_size;
    SQLSMALLINT decimal_digits;
    SQLSMALLINT nullable;
    
    ret = SQLDescribeCol(hstmt, 1, column_name, sizeof(column_name), &name_length,
                        &data_type, &column_size, &decimal_digits, &nullable);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("greeting", (char*)column_name);
    EXPECT_EQ(8, name_length);
    EXPECT_EQ(SQL_VARCHAR, data_type);
    EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);

    ret = SQLDescribeCol(hstmt, 2, column_name, sizeof(column_name), &name_length,
                        &data_type, &column_size, &decimal_digits, &nullable);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("answer", (char*)column_name);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(10u, column_size);
    
    // Test column attribute
    SQLLEN numeric_attr;
    ret = SQLColAttribute(hstmt, 1, SQL_DESC_TYPE, nullptr, 0, nullptr, &numeric_attr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ(SQL_VARCHAR, numeric_attr);
}

TEST_F(MetadataIntegrationTest, NotificationDoesNotCorruptQueryResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"LISTEN odbcpp_notification_test", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"NOTIFY odbcpp_notification_test, 'delivered'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 42", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(42), integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ReportsCurrentRowNumberOnlyWhilePositioned) {
    SQLCHAR query[] =
        "SELECT value FROM (SELECT 10 AS value UNION ALL SELECT 20) AS rows ORDER BY value";
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, query, SQL_NTS));

    SQLULEN row_number = 99;
    EXPECT_EQ(SQL_ERROR, SQLGetStmtAttr(
        hstmt, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number), nullptr));
    EXPECT_EQ(99u, row_number);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number), nullptr));
    EXPECT_EQ(1u, row_number);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number), nullptr));
    EXPECT_EQ(2u, row_number);

    ASSERT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    row_number = 99;
    EXPECT_EQ(SQL_ERROR, SQLGetStmtAttr(
        hstmt, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number), nullptr));
    EXPECT_EQ(99u, row_number);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));

    SQLINTEGER value = 0;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
        hstmt, SQL_ATTR_CURSOR_TYPE,
        reinterpret_cast<SQLPOINTER>(SQL_CURSOR_FORWARD_ONLY), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    SQLCHAR prepared_query[] = "SELECT 1";
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, prepared_query, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
        hstmt, SQL_ATTR_CURSOR_TYPE,
        reinterpret_cast<SQLPOINTER>(SQL_CURSOR_FORWARD_ONLY), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY011", reinterpret_cast<char*>(state));
}

TEST_F(MetadataIntegrationTest, ReportsAnsiColumnNameTruncation) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 AS long_column_name", SQL_NTS));

    char name[5]{};
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name), &length,
        nullptr, nullptr, nullptr, nullptr));
    EXPECT_STREQ("long", name);
    EXPECT_EQ(16, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));

    name[0] = '\0';
    length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLColAttribute(
        hstmt, 1, SQL_DESC_NAME, name, sizeof(name), &length, nullptr));
    EXPECT_STREQ("long", name);
    EXPECT_EQ(16, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(MetadataIntegrationTest, ImplementationDescriptorReportsResultMetadata) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 'hello'::varchar(12) AS label, "
                  "12.34::numeric(8,2) AS amount",
        SQL_NTS));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_ROW_DESC, &descriptor, 0, nullptr));
    SQLSMALLINT count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 0, SQL_DESC_COUNT, &count, 0, nullptr));
    EXPECT_EQ(2, count);

    char name[16]{};
    SQLINTEGER name_length = 0;
    SQLSMALLINT type = 0;
    SQLULEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 1, SQL_DESC_NAME, name, sizeof(name), &name_length));
    EXPECT_STREQ("label", name);
    EXPECT_EQ(5, name_length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 1, SQL_DESC_CONCISE_TYPE, &type, 0, nullptr));
    EXPECT_EQ(SQL_VARCHAR, type);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 1, SQL_DESC_LENGTH, &length, 0, nullptr));
    EXPECT_EQ(12u, length);

    SQLSMALLINT precision = 0;
    SQLSMALLINT scale = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 2, SQL_DESC_PRECISION, &precision, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 2, SQL_DESC_SCALE, &scale, 0, nullptr));
    EXPECT_EQ(8, precision);
    EXPECT_EQ(2, scale);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 0, SQL_DESC_COUNT, &count, 0, nullptr));
    EXPECT_EQ(0, count);
}

TEST_F(MetadataIntegrationTest, ImplementationDescriptorCompletesFieldMatrix) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric(8,2) AS amount, "
                  "'hello'::varchar(12) AS label, CURRENT_DATE AS day",
        SQL_NTS));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_ROW_DESC, &descriptor, 0, nullptr));

    SQLSMALLINT allocation = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 0, SQL_DESC_ALLOC_TYPE, &allocation, 0, nullptr));
    EXPECT_EQ(SQL_DESC_ALLOC_AUTO, allocation);

    struct NumericField {
        SQLSMALLINT identifier;
        SQLLEN expected;
    };
    const NumericField numeric_fields[] = {
        {SQL_DESC_AUTO_UNIQUE_VALUE, SQL_FALSE},
        {SQL_DESC_CASE_SENSITIVE, SQL_FALSE},
        {SQL_DESC_DISPLAY_SIZE, 10},
        {SQL_DESC_FIXED_PREC_SCALE, SQL_TRUE},
        {SQL_DESC_NUM_PREC_RADIX, 10},
        {SQL_DESC_OCTET_LENGTH, 10},
        {SQL_DESC_ROWVER, SQL_FALSE},
        {SQL_DESC_SEARCHABLE, SQL_PRED_SEARCHABLE},
        {SQL_DESC_UNNAMED, SQL_NAMED},
        {SQL_DESC_UNSIGNED, SQL_FALSE},
        {SQL_DESC_UPDATABLE, SQL_ATTR_READONLY},
    };
    for (const auto& field : numeric_fields) {
        SQLLEN value = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
            descriptor, 1, field.identifier, &value, 0, nullptr));
        EXPECT_EQ(field.expected, value) << field.identifier;
    }

    struct TextField {
        SQLSMALLINT identifier;
        const char* expected;
    };
    const TextField text_fields[] = {
        {SQL_DESC_BASE_COLUMN_NAME, ""},
        {SQL_DESC_BASE_TABLE_NAME, ""},
        {SQL_DESC_CATALOG_NAME, ""},
        {SQL_DESC_LABEL, "amount"},
        {SQL_DESC_LITERAL_PREFIX, ""},
        {SQL_DESC_LITERAL_SUFFIX, ""},
        {SQL_DESC_LOCAL_TYPE_NAME, "numeric"},
        {SQL_DESC_NAME, "amount"},
        {SQL_DESC_SCHEMA_NAME, ""},
        {SQL_DESC_TABLE_NAME, ""},
        {SQL_DESC_TYPE_NAME, "numeric"},
    };
    for (const auto& field : text_fields) {
        char value[32]{'x'};
        SQLINTEGER length = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
            descriptor, 1, field.identifier, value, sizeof(value),
            &length));
        EXPECT_STREQ(field.expected, value) << field.identifier;
        EXPECT_EQ(static_cast<SQLINTEGER>(std::strlen(field.expected)),
                  length) << field.identifier;
    }

    char name[16]{};
    SQLSMALLINT name_length = -1;
    SQLSMALLINT type = 0;
    SQLSMALLINT subtype = -1;
    SQLLEN length = -1;
    SQLSMALLINT precision = -1;
    SQLSMALLINT scale = -1;
    SQLSMALLINT nullable = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescRec(
        descriptor, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &type, &subtype, &length, &precision, &scale,
        &nullable));
    EXPECT_STREQ("amount", name);
    EXPECT_EQ(6, name_length);
    EXPECT_EQ(SQL_NUMERIC, type);
    EXPECT_EQ(0, subtype);
    EXPECT_EQ(10, length);
    EXPECT_EQ(8, precision);
    EXPECT_EQ(2, scale);
    EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);

    SQLWCHAR wide_name[16]{};
    name_length = -1;
    type = 0;
    subtype = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescRecW(
        descriptor, 3, wide_name,
        static_cast<SQLSMALLINT>(std::size(wide_name)), &name_length,
        &type, &subtype, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(3, name_length);
    EXPECT_EQ(SQL_DATETIME, type);
    EXPECT_EQ(SQL_CODE_DATE, subtype);
    const auto wide_name_utf8 = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(wide_name, 3));
    ASSERT_TRUE(wide_name_utf8.has_value());
    EXPECT_EQ("day", *wide_name_utf8);

    SQLWCHAR short_type_name[4]{};
    SQLINTEGER required_bytes = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDescFieldW(
        descriptor, 1, SQL_DESC_TYPE_NAME, short_type_name,
        sizeof(short_type_name), &required_bytes));
    EXPECT_EQ(static_cast<SQLINTEGER>(7 * sizeof(SQLWCHAR)), required_bytes);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_DESC, descriptor, 1, state, nullptr, nullptr, 0,
        nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(MetadataIntegrationTest, GetDataWideHandlesUnalignedBuffer) {
    char query[] = "SELECT 'A'::text";
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, reinterpret_cast<SQLCHAR*>(query), SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    alignas(SQLWCHAR) std::array<std::byte, 1 + 2 * sizeof(SQLWCHAR)> storage{};
    auto* output = storage.data() + 1;
    SQLLEN length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_WCHAR, output, 2 * sizeof(SQLWCHAR), &length));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQLWCHAR)), length);

    SQLWCHAR value = 0;
    SQLWCHAR terminator = 1;
    std::memcpy(&value, output, sizeof(value));
    std::memcpy(&terminator, output + sizeof(value), sizeof(terminator));
    EXPECT_EQ(static_cast<SQLWCHAR>('A'), value);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), terminator);
}

TEST_F(MetadataIntegrationTest, GetDataRejectsTargetChangeMidChunk) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT value FROM (SELECT 1 AS ordinal, CAST('abcdef' AS varchar(6)) AS value "
                  "UNION ALL SELECT 2, CAST('ghij' AS varchar(6))) AS rows ORDER BY ordinal",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char first[3]{};
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, first, sizeof(first), &length));
    EXPECT_STREQ("ab", first);
    EXPECT_EQ(6, length);

    SQLWCHAR changed[8]{static_cast<SQLWCHAR>('X')};
    SQLLEN changed_length = 77;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_WCHAR, changed, sizeof(changed), &changed_length));
    EXPECT_EQ("HY010", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(static_cast<SQLWCHAR>('X'), changed[0]);
    EXPECT_EQ(77, changed_length);

    char remaining[8]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, remaining, sizeof(remaining), &length));
    EXPECT_STREQ("cdef", remaining);
    EXPECT_EQ(4, length);
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(
        hstmt, 1, SQL_C_CHAR, remaining, sizeof(remaining), &length));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLWCHAR next_row[8]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_WCHAR, next_row, sizeof(next_row), &length));
    EXPECT_EQ(static_cast<SQLLEN>(4 * sizeof(SQLWCHAR)), length);
    EXPECT_EQ(static_cast<SQLWCHAR>('g'), next_row[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('h'), next_row[1]);
    EXPECT_EQ(static_cast<SQLWCHAR>('i'), next_row[2]);
    EXPECT_EQ(static_cast<SQLWCHAR>('j'), next_row[3]);
}

TEST_F(MetadataIntegrationTest, GetDataCanChangeTypeAfterZeroLengthProbe) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 'abc'::text", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char untouched = 'X';
    SQLLEN length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(
        hstmt, 1, SQL_C_CHAR, &untouched, 0, &length));
    EXPECT_EQ('X', untouched);
    EXPECT_EQ(3, length);

    SQLWCHAR wide[4]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_WCHAR, wide, sizeof(wide), &length));
    EXPECT_EQ(static_cast<SQLLEN>(3 * sizeof(SQLWCHAR)), length);
    EXPECT_EQ(static_cast<SQLWCHAR>('a'), wide[0]);
    EXPECT_EQ(static_cast<SQLWCHAR>('b'), wide[1]);
    EXPECT_EQ(static_cast<SQLWCHAR>('c'), wide[2]);
    EXPECT_EQ(static_cast<SQLWCHAR>(0), wide[3]);
}

TEST_F(MetadataIntegrationTest, ExecutesAndPreparesUnicodeSql) {
    const std::string expected =
        "Gr\xc3\xbc\xc3\x9f" "e \xe4\xb8\x96\xe7\x95\x8c "
        "\xf0\x9f\x99\x82";
    auto direct_sql = rs::odbc::utf8_to_wide(
        "SELECT 'Gr\xc3\xbc\xc3\x9f" "e \xe4\xb8\x96\xe7\x95\x8c "
        "\xf0\x9f\x99\x82'::text");
    ASSERT_TRUE(direct_sql.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirectW(hstmt, direct_sql->data(),
                             static_cast<SQLINTEGER>(direct_sql->size())));
    SQLWCHAR bound_value[64]{};
    SQLLEN bound_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLBindCol(hstmt, 1, SQL_C_WCHAR, bound_value,
                         sizeof(bound_value), &bound_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    const auto converted_bound = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            bound_value, static_cast<std::size_t>(bound_length) /
                             sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_bound.has_value());
    EXPECT_EQ(expected, *converted_bound);
    SQLWCHAR value[64]{};
    SQLLEN value_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 1, SQL_C_WCHAR, value, sizeof(value),
                         &value_length));
    const auto converted_value = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            value, static_cast<std::size_t>(value_length) /
                       sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_value.has_value());
    EXPECT_EQ(expected, *converted_value);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    auto prepared_sql = rs::odbc::utf8_to_wide("SELECT ?::text");
    ASSERT_TRUE(prepared_sql.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLPrepareW(hstmt, prepared_sql->data(),
                          static_cast<SQLINTEGER>(prepared_sql->size())));
    const std::string prepared_expected = "\xf0\x9f\x9a\x80 prepared";
    auto parameter = rs::odbc::utf8_to_wide(prepared_expected);
    ASSERT_TRUE(parameter.has_value());
    parameter->push_back(0);
    SQLLEN parameter_length = SQL_NTS;
    ASSERT_EQ(SQL_SUCCESS,
              SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
                               SQL_WVARCHAR, parameter->size() - 1, 0,
                               parameter->data(),
                               static_cast<SQLLEN>(parameter->size() *
                                                   sizeof(SQLWCHAR)),
                               &parameter_length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLWCHAR prepared_value[64]{};
    SQLLEN prepared_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 1, SQL_C_WCHAR, prepared_value,
                         sizeof(prepared_value), &prepared_length));
    const auto converted_prepared = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            prepared_value, static_cast<std::size_t>(prepared_length) /
                                sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_prepared.has_value());
    EXPECT_EQ(prepared_expected, *converted_prepared);
}

TEST_F(MetadataIntegrationTest, FindsUnicodeCatalogIdentifiers) {
    const std::string table_name =
        "odbcpp\xe4\xb8\x96\xe7\x95\x8c" "table";
    const std::string column_name =
        "gr\xc3\xbc\xc3\x9f" "ecolumn";
    auto create_sql = rs::odbc::utf8_to_wide(
        "CREATE TEMP TABLE \"odbcpp\xe4\xb8\x96\xe7\x95\x8c"
        "table\" (\"gr\xc3\xbc\xc3\x9f" "ecolumn\" text)");
    ASSERT_TRUE(create_sql.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirectW(hstmt, create_sql->data(),
                             static_cast<SQLINTEGER>(create_sql->size())));

    auto wide_table = rs::odbc::utf8_to_wide(table_name);
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLTablesW(hstmt, nullptr, 0, nullptr, 0,
                         wide_table->data(),
                         static_cast<SQLSMALLINT>(wide_table->size()),
                         nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLWCHAR returned_table[128]{};
    SQLLEN returned_table_bytes = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 3, SQL_C_WCHAR, returned_table,
                         sizeof(returned_table), &returned_table_bytes));
    const auto converted_table = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            returned_table,
            static_cast<std::size_t>(returned_table_bytes) /
                sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_table.has_value());
    EXPECT_EQ(table_name, *converted_table);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    auto wide_column = rs::odbc::utf8_to_wide(column_name);
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLColumnsW(hstmt, nullptr, 0, nullptr, 0,
                          wide_table->data(),
                          static_cast<SQLSMALLINT>(wide_table->size()),
                          wide_column->data(),
                          static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLWCHAR returned_column[128]{};
    SQLLEN returned_column_bytes = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 4, SQL_C_WCHAR, returned_column,
                         sizeof(returned_column), &returned_column_bytes));
    const auto converted_column = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            returned_column,
            static_cast<std::size_t>(returned_column_bytes) /
                sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_column.has_value());
    EXPECT_EQ(column_name, *converted_column);
}

TEST_F(MetadataIntegrationTest, DescribesUnicodeColumnNames) {
    const std::string expected =
        "r\xc3\xa9sultat\xe4\xb8\x96\xe7\x95\x8c";
    auto query = rs::odbc::utf8_to_wide(
        "SELECT 1 AS \"r\xc3\xa9sultat\xe4\xb8\x96\xe7\x95\x8c\"");
    ASSERT_TRUE(query.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirectW(hstmt, query->data(),
                             static_cast<SQLINTEGER>(query->size())));

    SQLWCHAR described_name[64]{};
    SQLSMALLINT described_units = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLDescribeColW(hstmt, 1, described_name, 64,
                              &described_units, nullptr, nullptr, nullptr,
                              nullptr));
    const auto described = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            described_name, static_cast<std::size_t>(described_units)));
    ASSERT_TRUE(described.has_value());
    EXPECT_EQ(expected, *described);

    SQLWCHAR attribute_name[64]{};
    SQLSMALLINT attribute_bytes = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLColAttributeW(hstmt, 1, SQL_DESC_NAME, attribute_name,
                               sizeof(attribute_name), &attribute_bytes,
                               nullptr));
    const auto attribute = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            attribute_name,
            static_cast<std::size_t>(attribute_bytes) /
                sizeof(SQLWCHAR)));
    ASSERT_TRUE(attribute.has_value());
    EXPECT_EQ(expected, *attribute);
}

TEST_F(MetadataIntegrationTest, MultipleColumns) {
    // Execute query with different data types
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'text', 123, NOW(), true", SQL_NTS);
    ASSERT_EQ(SQL_SUCCESS, ret);
    
    // Verify column count
    SQLSMALLINT num_cols;
    ret = SQLNumResultCols(hstmt, &num_cols);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ(4, num_cols);
    
    // Test each column can be described
    for (SQLUSMALLINT i = 1; i <= num_cols; i++) {
        SQLCHAR column_name[256];
        SQLSMALLINT data_type;
        
        ret = SQLDescribeCol(hstmt, i, column_name, sizeof(column_name), nullptr,
                            &data_type, nullptr, nullptr, nullptr);
        EXPECT_EQ(SQL_SUCCESS, ret) << "Failed to describe column " << i;
    }
}

TEST_F(MetadataIntegrationTest, ReportsAffectedRows) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(hstmt,
                            (SQLCHAR*)"CREATE TEMP TABLE row_count_test(value int)",
                            SQL_NTS));

    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(hstmt,
                            (SQLCHAR*)"INSERT INTO row_count_test VALUES (1), (2), (3)",
                            SQL_NTS));
    SQLLEN row_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);

    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(hstmt,
                            (SQLCHAR*)"UPDATE row_count_test SET value = value + 1 WHERE value >= 2",
                            SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(2, row_count);

    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));
}

TEST_F(MetadataIntegrationTest, CountOutputsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, reinterpret_cast<SQLCHAR*>(
                   const_cast<char*>("SELECT 1 AS value")), SQL_NTS));

    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> storage{};
    auto* column_output = reinterpret_cast<SQLSMALLINT*>(storage.data() + 1);
    auto* row_output = reinterpret_cast<SQLLEN*>(storage.data() + 1);

    SQLSMALLINT expected_columns = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &expected_columns));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, column_output));
    SQLSMALLINT actual_columns = 0;
    std::memcpy(&actual_columns, storage.data() + 1,
                sizeof(actual_columns));
    EXPECT_EQ(expected_columns, actual_columns);

    SQLLEN expected_rows = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &expected_rows));
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, row_output));
    SQLLEN actual_rows = -2;
    std::memcpy(&actual_rows, storage.data() + 1, sizeof(actual_rows));
    EXPECT_EQ(expected_rows, actual_rows);

    SQLHSTMT empty_statement = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_STMT, hdbc, &empty_statement));
    const auto previous_output = storage;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(empty_statement, column_output));
    EXPECT_EQ(previous_output, storage);
    EXPECT_EQ(SQL_ERROR, SQLRowCount(empty_statement, row_output));
    EXPECT_EQ(previous_output, storage);
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, empty_statement));
}

TEST_F(MetadataIntegrationTest, DescribesPreparedResultsBeforeExecution) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE prepared_metadata_state(value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt,
        (SQLCHAR*)"INSERT INTO prepared_metadata_state VALUES (?) "
                  "RETURNING value AS inserted_value",
        SQL_NTS));

    SQLINTEGER input = 42;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));

    SQLLEN row_count = 91;
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(91, row_count);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY010", reinterpret_cast<char*>(state));

    SQLSMALLINT column_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    char name[32]{};
    SQLSMALLINT data_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name), nullptr,
        &data_type, nullptr, nullptr, nullptr));
    EXPECT_STREQ("inserted_value", name);
    EXPECT_EQ(SQL_INTEGER, data_type);

    SQLHSTMT verification = SQL_NULL_HSTMT;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_STMT, hdbc, &verification));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        verification,
        (SQLCHAR*)"SELECT count(*) FROM prepared_metadata_state", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(verification));
    SQLINTEGER rows_before_execution = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        verification, 1, SQL_C_SLONG, &rows_before_execution, 0, nullptr));
    EXPECT_EQ(0, rows_before_execution);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, verification));

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(1, row_count);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER returned = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &returned, 0, nullptr));
    EXPECT_EQ(input, returned);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    column_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    row_count = 92;
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(92, row_count);
}

TEST_F(MetadataIntegrationTest, DescribesPreparedColumnsDirectly) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric(8,2) AS prepared_amount, "
                  "'x'::varchar(12) AS prepared_label",
        SQL_NTS));

    char name[32]{"unchanged"};
    SQLSMALLINT name_length = -1;
    SQLSMALLINT data_type = 0;
    SQLULEN column_size = 0;
    SQLSMALLINT scale = -1;
    SQLSMALLINT nullable = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    EXPECT_STREQ("prepared_amount", name);
    EXPECT_EQ(15, name_length);
    EXPECT_EQ(SQL_NUMERIC, data_type);
    EXPECT_EQ(8u, column_size);
    EXPECT_EQ(2, scale);
    EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);

    char label[32]{"unchanged"};
    SQLSMALLINT label_length = -1;
    SQLLEN ignored_numeric = 81;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 2, SQL_DESC_LABEL, label, sizeof(label), &label_length,
        &ignored_numeric));
    EXPECT_STREQ("prepared_label", label);
    EXPECT_EQ(14, label_length);
    EXPECT_EQ(81, ignored_numeric);

    struct NumericAttribute {
        SQLUSMALLINT field;
        SQLLEN expected;
    };
    const NumericAttribute attributes[] = {
        {SQL_DESC_TYPE, SQL_NUMERIC},
        {SQL_DESC_CONCISE_TYPE, SQL_NUMERIC},
        {SQL_DESC_LENGTH, 8},
        {SQL_DESC_PRECISION, 8},
        {SQL_DESC_SCALE, 2},
        {SQL_DESC_NULLABLE, SQL_NULLABLE_UNKNOWN},
        {SQL_DESC_UNNAMED, SQL_NAMED},
    };
    for (const auto& attribute : attributes) {
        char ignored_text[]{"keep"};
        SQLSMALLINT ignored_length = 82;
        SQLLEN value = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
            hstmt, 1, attribute.field, ignored_text, -7, &ignored_length,
            &value));
        EXPECT_EQ(attribute.expected, value);
        EXPECT_STREQ("keep", ignored_text);
        EXPECT_EQ(82, ignored_length);
    }

    char ignored_text[]{"keep"};
    SQLSMALLINT ignored_length = 83;
    SQLLEN count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 999, SQL_DESC_COUNT, ignored_text, -7, &ignored_length,
        &count));
    EXPECT_EQ(2, count);
    EXPECT_STREQ("keep", ignored_text);
    EXPECT_EQ(83, ignored_length);

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    data_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 2, nullptr, 0, nullptr, &data_type, nullptr, nullptr,
        nullptr));
    EXPECT_EQ(SQL_VARCHAR, data_type);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    name[0] = '\0';
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name), nullptr,
        nullptr, nullptr, nullptr, nullptr));
    EXPECT_STREQ("prepared_amount", name);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT CURRENT_DATE AS prepared_date", SQL_NTS));
    SQLLEN descriptor_type = 0;
    SQLLEN concise_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_TYPE, nullptr, 0, nullptr, &descriptor_type));
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr,
        &concise_type));
    EXPECT_EQ(SQL_DATETIME, descriptor_type);
    EXPECT_EQ(SQL_TYPE_DATE, concise_type);
}

TEST_F(MetadataIntegrationTest, DescribeOutputsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::numeric(8,2) AS prepared_amount",
        SQL_NTS));

    SQLSMALLINT expected_name_length = -1;
    SQLSMALLINT expected_type = 0;
    SQLULEN expected_size = 0;
    SQLSMALLINT expected_scale = -1;
    SQLSMALLINT expected_nullable = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, &expected_name_length, &expected_type,
        &expected_size, &expected_scale, &expected_nullable));

    alignas(SQLULEN) std::array<std::byte, 9 + sizeof(SQLULEN)> storage{};
    storage.fill(std::byte{0x5a});
    auto* name_length = reinterpret_cast<SQLSMALLINT*>(storage.data() + 1);
    auto* data_type = reinterpret_cast<SQLSMALLINT*>(storage.data() + 3);
    auto* column_size = reinterpret_cast<SQLULEN*>(storage.data() + 5);
    auto* scale = reinterpret_cast<SQLSMALLINT*>(
        storage.data() + 5 + sizeof(SQLULEN));
    auto* nullable = reinterpret_cast<SQLSMALLINT*>(
        storage.data() + 7 + sizeof(SQLULEN));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, name_length, data_type, column_size, scale,
        nullable));

    auto read_smallint = [&](std::size_t offset) {
        SQLSMALLINT value = 0;
        std::memcpy(&value, storage.data() + offset, sizeof(value));
        return value;
    };
    auto read_size = [&](std::size_t offset) {
        SQLULEN value = 0;
        std::memcpy(&value, storage.data() + offset, sizeof(value));
        return value;
    };
    EXPECT_EQ(expected_name_length, read_smallint(1));
    EXPECT_EQ(expected_type, read_smallint(3));
    EXPECT_EQ(expected_size, read_size(5));
    EXPECT_EQ(expected_scale, read_smallint(5 + sizeof(SQLULEN)));
    EXPECT_EQ(expected_nullable, read_smallint(7 + sizeof(SQLULEN)));

    const auto column_output = storage;
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 0, nullptr, 0, name_length, data_type, column_size, scale,
        nullable));
    EXPECT_EQ(column_output, storage);

    SQLSMALLINT expected_parameter_type = 0;
    SQLULEN expected_parameter_size = 0;
    SQLSMALLINT expected_parameter_scale = -1;
    SQLSMALLINT expected_parameter_nullable = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &expected_parameter_type, &expected_parameter_size,
        &expected_parameter_scale, &expected_parameter_nullable));

    storage.fill(std::byte{0x5a});
    auto* parameter_type = reinterpret_cast<SQLSMALLINT*>(storage.data() + 1);
    auto* parameter_size = reinterpret_cast<SQLULEN*>(storage.data() + 3);
    auto* parameter_scale = reinterpret_cast<SQLSMALLINT*>(
        storage.data() + 3 + sizeof(SQLULEN));
    auto* parameter_nullable = reinterpret_cast<SQLSMALLINT*>(
        storage.data() + 5 + sizeof(SQLULEN));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, parameter_type, parameter_size, parameter_scale,
        parameter_nullable));
    EXPECT_EQ(expected_parameter_type, read_smallint(1));
    EXPECT_EQ(expected_parameter_size, read_size(3));
    EXPECT_EQ(expected_parameter_scale, read_smallint(3 + sizeof(SQLULEN)));
    EXPECT_EQ(expected_parameter_nullable,
              read_smallint(5 + sizeof(SQLULEN)));

    const auto parameter_output = storage;
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(
        hstmt, 0, parameter_type, parameter_size, parameter_scale,
        parameter_nullable));
    EXPECT_EQ(parameter_output, storage);
}

TEST_F(MetadataIntegrationTest, ColumnAttributeOutputsHandleUnalignedBuffers) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 12.34::numeric(8,2) AS amount", SQL_NTS));

    alignas(SQLLEN) std::array<std::byte, 1 + sizeof(SQLLEN)> storage{};
    auto* string_length = reinterpret_cast<SQLSMALLINT*>(storage.data() + 1);
    auto* numeric_value = reinterpret_cast<SQLLEN*>(storage.data() + 1);
    char label[32]{};
    SQLSMALLINT expected_length = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_LABEL, label, sizeof(label), &expected_length,
        nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_LABEL, label, sizeof(label), string_length,
        nullptr));
    SQLSMALLINT actual_length = 0;
    std::memcpy(&actual_length, storage.data() + 1, sizeof(actual_length));
    EXPECT_EQ(expected_length, actual_length);

    const auto expect_numeric = [&](SQLUSMALLINT column, SQLUSMALLINT field) {
        SQLLEN expected = -1;
        EXPECT_EQ(SQL_SUCCESS, SQLColAttribute(
            hstmt, column, field, nullptr, 0, nullptr, &expected));
        storage.fill(std::byte{0x5a});
        EXPECT_EQ(SQL_SUCCESS, SQLColAttribute(
            hstmt, column, field, nullptr, 0, nullptr, numeric_value));
        SQLLEN actual = -1;
        std::memcpy(&actual, storage.data() + 1, sizeof(actual));
        EXPECT_EQ(expected, actual);
    };
    expect_numeric(999, SQL_DESC_COUNT);
    expect_numeric(1, SQL_DESC_PRECISION);

    const auto previous_output = storage;
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, 9999, nullptr, 0, string_length, numeric_value));
    EXPECT_EQ(previous_output, storage);
}

TEST_F(MetadataIntegrationTest, ColumnAttributesMatchImplementationDescriptor) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric(8,2) AS amount, "
                  "'x'::varchar(12) AS label",
        SQL_NTS));
    SQLSMALLINT column_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    ASSERT_EQ(2, column_count);

    SQLHDESC row_descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_ROW_DESC, &row_descriptor, 0, nullptr));
    struct NumericField {
        SQLUSMALLINT identifier;
        SQLLEN expected;
    };
    const NumericField numeric_fields[] = {
        {SQL_DESC_AUTO_UNIQUE_VALUE, SQL_FALSE},
        {SQL_DESC_CASE_SENSITIVE, SQL_FALSE},
        {SQL_DESC_DISPLAY_SIZE, 10},
        {SQL_DESC_FIXED_PREC_SCALE, SQL_TRUE},
        {SQL_DESC_NUM_PREC_RADIX, 10},
        {SQL_DESC_OCTET_LENGTH, 10},
        {SQL_DESC_SEARCHABLE, SQL_PRED_SEARCHABLE},
        {SQL_DESC_UNSIGNED, SQL_FALSE},
        {SQL_DESC_UPDATABLE, SQL_ATTR_READONLY},
    };
    for (const auto& field : numeric_fields) {
        SQLLEN descriptor_value = 0;
        SQLLEN attribute_value = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
            row_descriptor, 1, field.identifier, &descriptor_value, 0,
            nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
            hstmt, 1, field.identifier, nullptr, 0, nullptr,
            &attribute_value));
        EXPECT_EQ(field.expected, descriptor_value) << field.identifier;
        EXPECT_EQ(descriptor_value, attribute_value) << field.identifier;
    }

    SQLLEN legacy_display_size = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 2, SQL_COLUMN_DISPLAY_SIZE, nullptr, 0, nullptr,
        &legacy_display_size));
    EXPECT_EQ(12, legacy_display_size);
    SQLLEN case_sensitive = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 2, SQL_DESC_CASE_SENSITIVE, nullptr, 0, nullptr,
        &case_sensitive));
    EXPECT_EQ(SQL_TRUE, case_sensitive);
    SQLSMALLINT descriptor_unsigned = SQL_FALSE;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        row_descriptor, 2, SQL_DESC_UNSIGNED, &descriptor_unsigned, 0,
        nullptr));
    SQLLEN attribute_unsigned = SQL_FALSE;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 2, SQL_DESC_UNSIGNED, nullptr, 0, nullptr,
        &attribute_unsigned));
    EXPECT_EQ(SQL_TRUE, descriptor_unsigned);
    EXPECT_EQ(SQL_TRUE, attribute_unsigned);
    SQLLEN descriptor_precision = -1;
    SQLLEN legacy_precision = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 2, SQL_DESC_PRECISION, nullptr, 0, nullptr,
        &descriptor_precision));
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 2, SQL_COLUMN_PRECISION, nullptr, 0, nullptr,
        &legacy_precision));
    EXPECT_EQ(0, descriptor_precision);
    EXPECT_EQ(12, legacy_precision);

    struct TextField {
        SQLUSMALLINT identifier;
        const char* expected;
    };
    const TextField text_fields[] = {
        {SQL_DESC_TYPE_NAME, "numeric"},
        {SQL_DESC_LOCAL_TYPE_NAME, "numeric"},
        {SQL_DESC_LITERAL_PREFIX, ""},
        {SQL_DESC_LITERAL_SUFFIX, ""},
    };
    for (const auto& field : text_fields) {
        char descriptor_value[32]{};
        char attribute_value[32]{};
        SQLINTEGER descriptor_length = -1;
        SQLSMALLINT attribute_length = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
            row_descriptor, 1, field.identifier, descriptor_value,
            sizeof(descriptor_value), &descriptor_length));
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
            hstmt, 1, field.identifier, attribute_value,
            sizeof(attribute_value), &attribute_length, nullptr));
        EXPECT_STREQ(field.expected, descriptor_value) << field.identifier;
        EXPECT_STREQ(descriptor_value, attribute_value) << field.identifier;
        EXPECT_EQ(descriptor_length, attribute_length) << field.identifier;
    }

    char short_type_name[4]{};
    SQLSMALLINT required_length = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLColAttribute(
        hstmt, 1, SQL_DESC_TYPE_NAME, short_type_name,
        sizeof(short_type_name), &required_length, nullptr));
    EXPECT_STREQ("num", short_type_name);
    EXPECT_EQ(7, required_length);
    EXPECT_EQ("01004", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    SQLWCHAR wide_type_name[16]{};
    SQLSMALLINT required_bytes = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttributeW(
        hstmt, 2, SQL_DESC_TYPE_NAME, wide_type_name,
        sizeof(wide_type_name), &required_bytes, nullptr));
    EXPECT_EQ(static_cast<SQLSMALLINT>(7 * sizeof(SQLWCHAR)), required_bytes);
    const auto wide_text = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(wide_type_name, 7));
    ASSERT_TRUE(wide_text.has_value());
    EXPECT_EQ("varchar", *wide_text);
}

TEST_F(MetadataIntegrationTest, ColumnMetadataStateErrorsPreserveOutputs) {
    const auto expect_state = [this](const char* expected) {
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0,
            nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(state));
    };

    char name[16]{"unchanged"};
    SQLSMALLINT name_length = 71;
    SQLSMALLINT data_type = 72;
    SQLULEN column_size = 73;
    SQLSMALLINT scale = 74;
    SQLSMALLINT nullable = 75;
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    expect_state("HY010");
    EXPECT_STREQ("unchanged", name);
    EXPECT_EQ(71, name_length);
    EXPECT_EQ(72, data_type);
    EXPECT_EQ(73u, column_size);
    EXPECT_EQ(74, scale);
    EXPECT_EQ(75, nullable);

    SQLLEN numeric = 76;
    SQLSMALLINT string_length = 77;
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, SQL_DESC_TYPE, name, sizeof(name), &string_length,
        &numeric));
    expect_state("HY010");
    EXPECT_EQ(76, numeric);
    EXPECT_EQ(77, string_length);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"UPDATE pg_catalog.pg_class SET relname = relname "
                         "WHERE false",
        SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    expect_state("07005");
    EXPECT_STREQ("unchanged", name);
    EXPECT_EQ(71, name_length);

    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, SQL_DESC_NAME, name, sizeof(name), &string_length,
        &numeric));
    expect_state("07005");
    EXPECT_EQ(76, numeric);
    EXPECT_EQ(77, string_length);

    numeric = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 999, SQL_DESC_COUNT, nullptr, 0, nullptr, &numeric));
    EXPECT_EQ(0, numeric);

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    expect_state("07005");
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, SQL_DESC_NAME, name, sizeof(name), &string_length,
        &numeric));
    expect_state("07005");

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 1 AS value", SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 0, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    expect_state("07009");
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 2, SQL_DESC_TYPE, nullptr, 0, nullptr, &numeric));
    expect_state("07009");

    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, 9999, nullptr, 0, nullptr, &numeric));
    expect_state("HY091");
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, SQL_DESC_BASE_TABLE_NAME, name, sizeof(name),
        &string_length, nullptr));
    expect_state("HYC00");

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 AS direct_value", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name), nullptr,
        nullptr, nullptr, nullptr, nullptr));
    expect_state("HY010");
}

TEST_F(MetadataIntegrationTest, DescribesPreparedWideColumnNames) {
    const std::string expected =
        "pr\xc3\xa9par\xc3\xa9\xe4\xb8\x96\xe7\x95\x8c";
    auto query = rs::odbc::utf8_to_wide(
        "SELECT 1 AS \"pr\xc3\xa9par\xc3\xa9\xe4\xb8\x96\xe7\x95\x8c\"");
    ASSERT_TRUE(query.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLPrepareW(
        hstmt, query->data(), static_cast<SQLINTEGER>(query->size())));

    SQLWCHAR name[32]{};
    SQLSMALLINT name_units = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(
        hstmt, 1, name, 32, &name_units, nullptr, nullptr, nullptr,
        nullptr));
    const auto described = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(name, static_cast<std::size_t>(name_units)));
    ASSERT_TRUE(described.has_value());
    EXPECT_EQ(expected, *described);

    SQLWCHAR label[32]{};
    SQLSMALLINT label_bytes = -1;
    SQLLEN ignored_numeric = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttributeW(
        hstmt, 1, SQL_DESC_LABEL, label, sizeof(label), &label_bytes,
        &ignored_numeric));
    const auto described_label = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            label, static_cast<std::size_t>(label_bytes) /
                       sizeof(SQLWCHAR)));
    ASSERT_TRUE(described_label.has_value());
    EXPECT_EQ(expected, *described_label);
    EXPECT_EQ(91, ignored_numeric);

    label[0] = static_cast<SQLWCHAR>('x');
    label_bytes = 92;
    EXPECT_EQ(SQL_ERROR, SQLColAttributeW(
        hstmt, 1, SQL_DESC_LABEL, label,
        static_cast<SQLSMALLINT>(sizeof(SQLWCHAR) + 1), &label_bytes,
        nullptr));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY090", reinterpret_cast<char*>(state));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), label[0]);
    EXPECT_EQ(92, label_bytes);
}

TEST_F(MetadataIntegrationTest, RefreshesPreparedShapeAfterIpdTypeChange) {
    if (redshift_profile()) {
        // The IPD supplies a text-family OID, not a VARCHAR result typmod.
        // Keep this cache-invalidation oracle about native type changes; the
        // separate sizing-policy tests still cover LONGVARCHAR presentation.
        ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, hstmt));
        hstmt = nullptr;
        ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(hdbc));
        const std::string configured = odbcpp::test::configured_connection_string();
        auto options = rs::odbc::ConnectionString::parse(configured);
        if (configured.find('=') == std::string::npos) {
            options["DSN"] = configured;
        }
        options["MAXVARCHARSIZE"] = "0";
        std::string connection;
        for (const auto& [key, value] : options) {
            connection += key + "={";
            for (char ch : value) {
                connection += ch;
                if (ch == '}') {
                    connection += '}';
                }
            }
            connection += "};";
        }
        ASSERT_TRUE(options == rs::odbc::ConnectionString::parse(connection))
            << "Sizing override must preserve all other connection fields";
        ASSERT_EQ(SQL_SUCCESS, SQLConnect(hdbc,
            reinterpret_cast<SQLCHAR*>(connection.data()), SQL_NTS,
            nullptr, 0, nullptr, 0));
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
            (SQLCHAR*)"SELECT ? AS typed_value", SQL_NTS));
        SQLHDESC implementation = SQL_NULL_HDESC;
        ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(hstmt,
            SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
        const auto number = [](SQLULEN value) {
            return reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(value));
        };
        ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(implementation, 1,
            SQL_DESC_CONCISE_TYPE, number(SQL_INTEGER), 0));
        SQLSMALLINT count = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &count));
        EXPECT_EQ(1, count);
        SQLSMALLINT type = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1,
            nullptr, 0, nullptr, &type, nullptr, nullptr, nullptr));
        EXPECT_EQ(SQL_INTEGER, type);
        std::array<char, 16> type_name{};
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(hstmt, 1, SQL_DESC_TYPE_NAME,
            type_name.data(), static_cast<SQLSMALLINT>(type_name.size()), nullptr, nullptr));
        EXPECT_STREQ("integer", type_name.data());

        // Change only the IPD, not the prepared SQL: a stale description would
        // incorrectly retain INTEGER. The bound length is an independent IPD
        // contract and must not be guessed to constrain a native result field.
        ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(implementation, 1,
            SQL_DESC_CONCISE_TYPE, number(SQL_VARCHAR), 0));
        ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(implementation, 1,
            SQL_DESC_LENGTH, number(17), 0));
        SQLULEN parameter_length = 99;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(implementation, 1,
            SQL_DESC_LENGTH, &parameter_length, 0, nullptr));
        EXPECT_EQ(17u, parameter_length);
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &count));
        EXPECT_EQ(1, count);
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1,
            nullptr, 0, nullptr, &type, nullptr, nullptr, nullptr));
        EXPECT_EQ(SQL_VARCHAR, type);
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(hstmt, 1, SQL_DESC_TYPE_NAME,
            type_name.data(), static_cast<SQLSMALLINT>(type_name.size()), nullptr, nullptr));
        EXPECT_STREQ("varchar", type_name.data());
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(implementation, 1,
            SQL_DESC_LENGTH, &parameter_length, 0, nullptr));
        EXPECT_EQ(17u, parameter_length);

        // A literal SQL cast supplies the result typmod the IPD cannot carry.
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
            (SQLCHAR*)"SELECT CAST(? AS VARCHAR(17)) AS typed_value", SQL_NTS));
        char parameter[18] = "12345678901234567";
        SQLLEN indicator = SQL_NULL_DATA;
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT,
            SQL_C_CHAR, SQL_VARCHAR, 17, 0, parameter, sizeof(parameter), &indicator));
        SQLULEN result_length = 99;
        SQLSMALLINT scale = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt, 1,
            nullptr, 0, nullptr, &type, &result_length, &scale, nullptr));
        EXPECT_EQ(SQL_VARCHAR, type);
        EXPECT_EQ(17u, result_length);
        EXPECT_EQ(0, scale);
        std::array<char, 20> output;
        output.fill('z');
        SQLLEN output_length = 99;
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
            output.data() + 1, 18, &output_length));
        EXPECT_EQ(SQL_NULL_DATA, output_length);
        EXPECT_EQ(std::string(20, 'z'), std::string(output.data(), output.size()));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        indicator = 17;
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR,
            output.data() + 1, 18, &output_length));
        EXPECT_EQ(17, output_length);
        EXPECT_STREQ(parameter, output.data() + 1);
        EXPECT_EQ('z', output.front());
        EXPECT_EQ('z', output.back());
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ? AS typed_value", SQL_NTS));
    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    const auto number = [](SQLSMALLINT value) {
        return reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(value));
    };

    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_CONCISE_TYPE,
        number(SQL_INTEGER), 0));
    SQLSMALLINT column_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    SQLSMALLINT data_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &data_type,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, data_type);
    char type_name[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_TYPE_NAME, type_name, sizeof(type_name),
        nullptr, nullptr));
    EXPECT_STREQ("integer", type_name);

    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_CONCISE_TYPE,
        number(SQL_VARCHAR), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &data_type,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_VARCHAR, data_type);
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_TYPE_NAME, type_name, sizeof(type_name),
        nullptr, nullptr));
    EXPECT_STREQ("varchar", type_name);
}

TEST_F(MetadataIntegrationTest, ResultShapeAndRowCountFollowStatementState) {
    SQLSMALLINT column_count = 71;
    SQLLEN row_count = 72;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(71, column_count);
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(72, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE result_state(value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(0, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"INSERT INTO result_state VALUES (1), (2), (3)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT value FROM result_state ORDER BY value",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    column_count = 73;
    row_count = 74;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(73, column_count);
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(74, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"UPDATE result_state SET value = value + 10",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    row_count = 75;
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(75, row_count);
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT * FROM", SQL_NTS));
    column_count = 76;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(76, column_count);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("42000", reinterpret_cast<char*>(state));
    row_count = 77;
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(77, row_count);

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT * FROM definitely_missing_odbcpp_table",
        SQL_NTS));
    column_count = 78;
    row_count = 79;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(78, column_count);
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(79, row_count);
}

TEST_F(MetadataIntegrationTest, TypeInfoSchemaUsesNormalizedDescriptorMetadata) {
    for (const SQLSMALLINT requested : {SQLSMALLINT(SQL_NUMERIC), SQLSMALLINT(SQL_WVARCHAR)}) {
        ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfoW(hstmt, requested));
        SQLSMALLINT count = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &count));
        ASSERT_EQ(19, count);
        for (SQLUSMALLINT column = 1; column <= 19; ++column) {
            const bool text = column == 1 || column == 4 || column == 5 ||
                column == 6 || column == 13;
            const bool integer = column == 3 || column == 18;
            SQLSMALLINT type = 0, scale = -1;
            SQLULEN size = 99;
            ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
                hstmt, column, nullptr, 0, nullptr, &type, &size, &scale, nullptr));
            EXPECT_EQ(text ? SQL_VARCHAR : integer ? SQL_INTEGER : SQL_SMALLINT, type);
            // Text capacities cover the complete TypeInfo catalog even when
            // this requested filter has no rows.
            const SQLULEN text_size = column == 1 ? 16u : column == 6 ? 15u : 1u;
            EXPECT_EQ(text ? text_size : integer ? 10u : 5u, size);
            EXPECT_EQ(0, scale);
            char name[32]{};
            ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
                hstmt, column, SQL_DESC_TYPE_NAME, name, sizeof(name), nullptr, nullptr));
            EXPECT_STREQ(text ? "varchar" : integer ? "integer" : "smallint", name);
        }
        if (requested == SQL_NUMERIC) {
            ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
            EXPECT_EQ(std::optional<std::string>("numeric"), text_cell(hstmt, 1));
        }
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
}

TEST_F(MetadataIntegrationTest, ReportsSupportedTypeInformation) {
    const bool redshift = rs::core::database::DatabaseFactory::get_compiled_database_type() ==
        rs::core::database::DatabaseType::Redshift;
    std::optional<SQLINTEGER> server_version = 0;
    if (!redshift) {
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
            hstmt, (SQLCHAR*)"SELECT current_setting('server_version_num')::integer", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        server_version = integer_cell(hstmt, 1);
        ASSERT_TRUE(server_version.has_value());
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }

    EXPECT_EQ(SQL_ERROR, SQLGetTypeInfo(hstmt, 12345));
    EXPECT_EQ("HY004", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(hstmt, SQL_WVARCHAR));
    SQLSMALLINT columns = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
    EXPECT_EQ(19, columns);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    EXPECT_EQ(SQL_ERROR, SQLGetTypeInfoW(hstmt, SQL_INTEGER));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    struct ExpectedType {
        const char* name;
        SQLSMALLINT data_type;
        SQLINTEGER column_size;
    };
    constexpr std::array<ExpectedType, 18> postgres_types{{
        {"boolean", SQL_BIT, 1},
        {"bigint", SQL_BIGINT, 19},
        {"bytea", SQL_VARBINARY, 1073741824},
        {"text", SQL_LONGVARCHAR, 1073741824},
        {"char", SQL_CHAR, 10485760},
        {"numeric", SQL_NUMERIC, 1000},
        {"decimal", SQL_DECIMAL, 1000},
        {"integer", SQL_INTEGER, 10},
        {"smallint", SQL_SMALLINT, 5},
        {"real", SQL_REAL, 7},
        {"double precision", SQL_DOUBLE, 15},
        {"date", SQL_DATE, 10},
        {"time", SQL_TIME, 15},
        {"timestamp", SQL_TIMESTAMP, 26},
        {"varchar", SQL_VARCHAR, 10485760},
        {"date", SQL_TYPE_DATE, 10},
        {"time", SQL_TYPE_TIME, 15},
        {"timestamp", SQL_TYPE_TIMESTAMP, 26},
    }};

    // Redshift DDL advertisements are separate from PostgreSQL limits and from
    // actual result dimensions; no PostgreSQL server_version_num query is used.
    constexpr std::array<ExpectedType, 17> redshift_types{{
        {"boolean", SQL_BIT, 1}, {"bigint", SQL_BIGINT, 19},
        {"char", SQL_CHAR, 4096}, {"numeric", SQL_NUMERIC, 38},
        {"decimal", SQL_DECIMAL, 38}, {"integer", SQL_INTEGER, 10},
        {"smallint", SQL_SMALLINT, 5}, {"real", SQL_REAL, 7},
        {"double precision", SQL_DOUBLE, 15}, {"date", SQL_DATE, 10},
        {"time", SQL_TIME, 15}, {"timestamp", SQL_TIMESTAMP, 26},
        {"varchar", SQL_VARCHAR, 65535}, {"varbyte", SQL_LONGVARBINARY, 16777216},
        {"date", SQL_TYPE_DATE, 10}, {"time", SQL_TYPE_TIME, 15},
        {"timestamp", SQL_TYPE_TIMESTAMP, 26},
    }};
    const auto expected_types = redshift
        ? std::span<const ExpectedType>(redshift_types)
        : std::span<const ExpectedType>(postgres_types);

    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(hstmt, SQL_ALL_TYPES));
    SQLLEN row_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(static_cast<SQLLEN>(expected_types.size()), row_count);

    for (std::size_t index = 0; index < expected_types.size(); ++index) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        if (index == 0) {
            EXPECT_EQ(SQL_ERROR, SQLGetTypeInfo(hstmt, SQL_INTEGER));
            EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        }
        char type_name[32]{};
        SQLSMALLINT data_type = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(
            hstmt, 1, SQL_C_CHAR, type_name, sizeof(type_name), nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(
            hstmt, 2, SQL_C_SSHORT, &data_type, 0, nullptr));
        EXPECT_STREQ(expected_types[index].name, type_name);
        EXPECT_EQ(expected_types[index].data_type, data_type);

        const auto type = expected_types[index].data_type;
        const bool character_type = type == SQL_CHAR ||
            type == SQL_VARCHAR || type == SQL_LONGVARCHAR;
        const bool quoted_type = character_type || type == SQL_VARBINARY ||
            type == SQL_DATE || type == SQL_TIME || type == SQL_TIMESTAMP ||
            type == SQL_TYPE_DATE || type == SQL_TYPE_TIME ||
            type == SQL_TYPE_TIMESTAMP;
        const bool numeric_type = type == SQL_BIGINT || type == SQL_NUMERIC ||
            type == SQL_DECIMAL || type == SQL_INTEGER ||
            type == SQL_SMALLINT || type == SQL_REAL || type == SQL_DOUBLE;
        const bool exact_numeric_type = type == SQL_BIGINT ||
            type == SQL_NUMERIC || type == SQL_DECIMAL ||
            type == SQL_INTEGER || type == SQL_SMALLINT;
        const bool datetime_type = type == SQL_DATE || type == SQL_TIME ||
            type == SQL_TIMESTAMP || type == SQL_TYPE_DATE ||
            type == SQL_TYPE_TIME || type == SQL_TYPE_TIMESTAMP;
        const bool time_type = type == SQL_TIME || type == SQL_TIMESTAMP ||
            type == SQL_TYPE_TIME || type == SQL_TYPE_TIMESTAMP;

        EXPECT_EQ(std::optional<SQLINTEGER>(expected_types[index].column_size),
                  integer_cell(hstmt, 3));
        EXPECT_EQ(quoted_type ? std::optional<std::string>("'") : std::nullopt,
                  text_cell(hstmt, 4));
        EXPECT_EQ(quoted_type ? std::optional<std::string>("'") : std::nullopt,
                  text_cell(hstmt, 5));
        if (type == SQL_CHAR || type == SQL_VARCHAR || type == SQL_LONGVARBINARY) {
            EXPECT_EQ(std::optional<std::string>("length"), text_cell(hstmt, 6));
        } else if (type == SQL_NUMERIC || type == SQL_DECIMAL) {
            EXPECT_EQ(std::optional<std::string>("precision,scale"),
                      text_cell(hstmt, 6));
        } else if (time_type) {
            EXPECT_EQ(std::optional<std::string>("precision"),
                      text_cell(hstmt, 6));
        } else {
            EXPECT_EQ(std::nullopt, text_cell(hstmt, 6));
        }
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NULLABLE),
                  integer_cell(hstmt, 7));
        EXPECT_EQ(std::optional<SQLINTEGER>(
                      character_type ? SQL_TRUE : SQL_FALSE),
                  integer_cell(hstmt, 8));
        EXPECT_EQ(std::optional<SQLINTEGER>(
                      character_type ? SQL_SEARCHABLE : SQL_PRED_BASIC),
                  integer_cell(hstmt, 9));
        EXPECT_EQ(numeric_type ? std::optional<SQLINTEGER>(SQL_FALSE)
                               : std::nullopt,
                  integer_cell(hstmt, 10));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_FALSE),
                  integer_cell(hstmt, 11));
        EXPECT_EQ(numeric_type ? std::optional<SQLINTEGER>(SQL_FALSE)
                               : std::nullopt,
                  integer_cell(hstmt, 12));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 13));
        const auto minimum_scale =
            (type == SQL_NUMERIC || type == SQL_DECIMAL) &&
                    !redshift && *server_version >= 150000
                ? std::optional<SQLINTEGER>(-1000)
                : ((exact_numeric_type || time_type)
                       ? std::optional<SQLINTEGER>(0) : std::nullopt);
        EXPECT_EQ(minimum_scale, integer_cell(hstmt, 14));
        const auto maximum_scale = type == SQL_NUMERIC || type == SQL_DECIMAL
            ? std::optional<SQLINTEGER>(redshift ? 37 : 1000)
            : (time_type ? std::optional<SQLINTEGER>(6)
                         : (exact_numeric_type
                                ? std::optional<SQLINTEGER>(0)
                                : std::nullopt));
        EXPECT_EQ(maximum_scale, integer_cell(hstmt, 15));
        EXPECT_EQ(std::optional<SQLINTEGER>(
                      datetime_type ? SQL_DATETIME : type),
                  integer_cell(hstmt, 16));
        const auto datetime_subtype =
            type == SQL_DATE || type == SQL_TYPE_DATE
                ? std::optional<SQLINTEGER>(SQL_CODE_DATE)
                : (type == SQL_TIME || type == SQL_TYPE_TIME
                       ? std::optional<SQLINTEGER>(SQL_CODE_TIME)
                       : (type == SQL_TIMESTAMP || type == SQL_TYPE_TIMESTAMP
                              ? std::optional<SQLINTEGER>(SQL_CODE_TIMESTAMP)
                              : std::nullopt));
        EXPECT_EQ(datetime_subtype, integer_cell(hstmt, 17));
        EXPECT_EQ(exact_numeric_type
                      ? std::optional<SQLINTEGER>(10)
                      : ((type == SQL_REAL || type == SQL_DOUBLE)
                             ? std::optional<SQLINTEGER>(2)
                             : std::nullopt),
                  integer_cell(hstmt, 18));
        EXPECT_EQ(std::nullopt, integer_cell(hstmt, 19));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfoW(hstmt, SQL_INTEGER));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char type_name[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, type_name, sizeof(type_name), nullptr));
    EXPECT_STREQ("integer", type_name);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfoW(hstmt, SQL_NUMERIC));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(!redshift && *server_version >= 150000
                  ? std::optional<SQLINTEGER>(-1000)
                  : std::optional<SQLINTEGER>(0),
              integer_cell(hstmt, 14));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, BackendColumnCatalogsPreserveDomainDimensionsAndNames) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_batch_domain AS numeric(8,3)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp'catalog_%é\"(\"id'é\" pg_temp.odbcpp_catalog_batch_domain PRIMARY KEY)",
        SQL_NTS));
    SQLCHAR pattern[] = "odbcpp'catalog\\_\\%é";
    SQLCHAR name[] = "odbcpp'catalog_%é";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, pattern, SQL_NTS, nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("id'é"), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NUMERIC), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(8), integer_cell(hstmt, 7));
    EXPECT_EQ(std::optional<SQLINTEGER>(3), integer_cell(hstmt, 9));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLWCHAR wide_name[] = {'o','d','b','c','p','p','\'','c','a','t','a','l','o','g','_','%',0xE9,0};
    ASSERT_EQ(SQL_SUCCESS, SQLStatisticsW(
        hstmt, nullptr, 0, nullptr, 0, wide_name, SQL_NTS,
        SQL_INDEX_UNIQUE, SQL_QUICK));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("id'é"), text_cell(hstmt, 9));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_FALSE), integer_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_ROWVER, nullptr, 0, nullptr, 0, name, SQL_NTS,
        SQL_SCOPE_CURROW, SQL_NO_NULLS));
    SQLSMALLINT count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &count));
    EXPECT_EQ(8, count);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumnsW(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0, wide_name, SQL_NTS,
        SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("id'é"), text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NUMERIC), integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<SQLINTEGER>(8), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(3), integer_cell(hstmt, 7));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, BackendRoutineCatalogsPreserveDomainArgumentsAndNames) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_routine_batch_domain AS varchar(11)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE FUNCTION pg_temp.\"odbcpp'routine_%é\"(\"arg'é\" pg_temp.odbcpp_routine_batch_domain) "
                        "RETURNS pg_temp.odbcpp_routine_batch_domain LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    SQLCHAR pattern[] = "odbcpp'routine\\_\\%é";
    SQLCHAR empty[] = "";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, empty, SQL_NTS, nullptr, 0, pattern, SQL_NTS));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, pattern, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp'routine_%é"), text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLWCHAR wide_pattern[] = {'o','d','b','c','p','p','\'','r','o','u','t','i','n','e','\\','_','\\','%',0xE9,0};
    SQLWCHAR argument[] = {'a','r','g','\'',0xE9,0};
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumnsW(
        hstmt, nullptr, 0, nullptr, 0, wide_pattern, SQL_NTS, argument, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("arg'é"), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PARAM_INPUT), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_VARCHAR), integer_cell(hstmt, 6));
    EXPECT_EQ(std::optional<SQLINTEGER>(11), integer_cell(hstmt, 8));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, BackendCatalogsPreserveQuotedUnicodeNames) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp'parent_%é\"(id integer PRIMARY KEY)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp'child_%é\"(id integer REFERENCES \"odbcpp'parent_%é\"(id))",
        SQL_NTS));
    SQLCHAR pattern[] = "odbcpp'parent\\_\\%é";
    SQLCHAR types[] = " 'local temporary', 'unknown' ";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, pattern, SQL_NTS, types, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp'parent_%é"), text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("LOCAL TEMPORARY"), text_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR unknown_types[] = "'unknown'";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, pattern, SQL_NTS, unknown_types, SQL_NTS));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLWCHAR parent[] = {'o','d','b','c','p','p','\'','p','a','r','e','n','t','_','%',0xE9,0};
    SQLWCHAR child[] = {'o','d','b','c','p','p','\'','c','h','i','l','d','_','%',0xE9,0};
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeysW(
        hstmt, nullptr, 0, nullptr, 0, parent, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp'parent_%é"), text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("id"), text_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeysW(
        hstmt, nullptr, 0, nullptr, 0, parent, SQL_NTS,
        nullptr, 0, nullptr, 0, child, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp'parent_%é"), text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("odbcpp'child_%é"), text_cell(hstmt, 7));
    EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 9));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLTables) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_catalog_test(value int)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_catalog_test";
    SQLCHAR table_type[] = "LOCAL TEMPORARY";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        table_type, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char returned_name[64]{};
    char returned_type[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_CHAR, returned_name, sizeof(returned_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, returned_type, sizeof(returned_type), nullptr));
    EXPECT_STREQ("odbcpp_catalog_test", returned_name);
    EXPECT_STREQ("LOCAL TEMPORARY", returned_type);
}

TEST_F(MetadataIntegrationTest, TablePatternsTypesAndEnumerationsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_table_a_b(value int)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_table_axb(value int)",
        SQL_NTS));

    SQLCHAR wildcard_pattern[] = "odbcpp_table_a_b";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, wildcard_pattern, SQL_NTS,
        nullptr, 0));
    for (const char* expected : {"odbcpp_table_a_b", "odbcpp_table_axb"}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>(expected), text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<std::string>("LOCAL TEMPORARY"),
                  text_cell(hstmt, 4));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR escaped_pattern[] = "odbcpp\\_table\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, escaped_pattern, SQL_NTS,
        nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_table_a_b"),
              text_cell(hstmt, 3));
    EXPECT_EQ(SQL_ERROR, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, escaped_pattern, SQL_NTS,
        nullptr, 0));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR local_temporary[] = "'local temporary'";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, wildcard_pattern, SQL_NTS,
        local_temporary, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("LOCAL TEMPORARY"),
              text_cell(hstmt, 4));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR all[]{'%', 0};
    constexpr std::array<const char*, 5> expected_types{
        "FOREIGN TABLE", "LOCAL TEMPORARY", "SYSTEM TABLE", "TABLE", "VIEW"};
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, empty, SQL_NTS, empty, SQL_NTS, empty, SQL_NTS,
        nullptr, 0));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, all, SQL_NTS, empty, SQL_NTS, empty, SQL_NTS,
        empty, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_TRUE(text_cell(hstmt, 1).has_value());
    EXPECT_EQ(std::nullopt, text_cell(hstmt, 2));
    EXPECT_EQ(std::nullopt, text_cell(hstmt, 3));
    EXPECT_EQ(std::nullopt, text_cell(hstmt, 4));
    EXPECT_EQ(std::nullopt, text_cell(hstmt, 5));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLWCHAR wide_empty[]{0};
    SQLWCHAR wide_all[]{'%', 0};
    ASSERT_EQ(SQL_SUCCESS, SQLTablesW(
        hstmt, wide_empty, SQL_NTS, wide_empty, SQL_NTS,
        wide_empty, SQL_NTS, wide_all, SQL_NTS));
    for (const auto* expected : expected_types) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>(expected), text_cell(hstmt, 4));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, empty, SQL_NTS, all, SQL_NTS, empty, SQL_NTS,
        empty, SQL_NTS));
    bool found_information_schema = false;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 1));
        const auto schema = text_cell(hstmt, 2);
        found_information_schema = found_information_schema ||
            schema == std::optional<std::string>("information_schema");
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 3));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 4));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 5));
    }
    EXPECT_TRUE(found_information_schema);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, empty, SQL_NTS, empty, SQL_NTS, empty, SQL_NTS,
        all, SQL_NTS));
    for (const auto* expected : expected_types) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 1));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 2));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<std::string>(expected), text_cell(hstmt, 4));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 5));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, all, SQL_NTS, nullptr, 0, nullptr, 0,
        nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_TRUE(text_cell(hstmt, 3).has_value());
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLColumns) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_columns_test("
                  "id integer NOT NULL, name varchar(32))",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_columns_test";
    SQLCHAR column_pattern[] = "%";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        column_pattern, SQL_NTS));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char column_name[64]{};
    SQLSMALLINT data_type = 0;
    SQLINTEGER ordinal_position = 0;
    SQLSMALLINT nullable = SQL_NULLABLE_UNKNOWN;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &data_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 11, SQL_C_SSHORT, &nullable, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 17, SQL_C_SLONG, &ordinal_position, 0, nullptr));
    EXPECT_STREQ("id", column_name);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(SQL_NO_NULLS, nullable);
    EXPECT_EQ(1, ordinal_position);

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER column_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &data_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 7, SQL_C_SLONG, &column_size, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 11, SQL_C_SSHORT, &nullable, 0, nullptr));
    EXPECT_STREQ("name", column_name);
    EXPECT_EQ(SQL_VARCHAR, data_type);
    EXPECT_EQ(32, column_size);
    EXPECT_EQ(SQL_NULLABLE, nullable);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ColumnPatternsAndCatalogArgumentsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_columns_a_b("
                  "column_a_b integer, column_axb integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_columns_axb(marker integer)",
        SQL_NTS));

    SQLCHAR table_pattern[] = "odbcpp_columns_a_b";
    SQLCHAR column_pattern[] = "column_a_b";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_pattern, SQL_NTS,
        column_pattern, SQL_NTS));
    for (const char* expected : {"column_a_b", "column_axb"}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>("odbcpp_columns_a_b"),
                  text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<std::string>(expected), text_cell(hstmt, 4));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR escaped_table[] = "odbcpp\\_columns\\_a\\_b";
    SQLCHAR escaped_column[] = "column\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, escaped_table, SQL_NTS,
        escaped_column, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_columns_a_b"),
              text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("column_a_b"),
              text_cell(hstmt, 4));
    EXPECT_EQ(SQL_ERROR, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, escaped_table, SQL_NTS,
        escaped_column, SQL_NTS));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR exact_table[] = "odbcpp_columns_a_b";
    SQLCHAR exact_column[] = "column_a_b";
    auto expect_no_columns = [&](SQLCHAR* catalog, SQLSMALLINT catalog_length,
                                 SQLCHAR* schema, SQLSMALLINT schema_length,
                                 SQLCHAR* table, SQLSMALLINT table_length,
                                 SQLCHAR* column, SQLSMALLINT column_length) {
        ASSERT_EQ(SQL_SUCCESS, SQLColumns(
            hstmt, catalog, catalog_length, schema, schema_length,
            table, table_length, column, column_length));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_columns(empty, SQL_NTS, nullptr, 0, exact_table, SQL_NTS,
                      exact_column, SQL_NTS);
    expect_no_columns(nullptr, 0, empty, SQL_NTS, exact_table, SQL_NTS,
                      exact_column, SQL_NTS);
    expect_no_columns(nullptr, 0, nullptr, 0, empty, SQL_NTS,
                      exact_column, SQL_NTS);
    expect_no_columns(nullptr, 0, nullptr, 0, exact_table, SQL_NTS,
                      empty, SQL_NTS);

    SQLCHAR percent[] = "%";
    expect_no_columns(percent, SQL_NTS, nullptr, 0, exact_table, SQL_NTS,
                      exact_column, SQL_NTS);

    std::array<SQLCHAR, 128> catalog{};
    SQLSMALLINT catalog_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
        hdbc, SQL_DATABASE_NAME, catalog.data(),
        static_cast<SQLSMALLINT>(catalog.size()),
        &catalog_length));
    ASSERT_GT(catalog_length, 0);
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, catalog.data(), catalog_length, nullptr, 0,
        escaped_table, SQL_NTS, escaped_column, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>(
                  reinterpret_cast<const char*>(catalog.data())),
              text_cell(hstmt, 1));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp\\_columns\\_a\\_b");
    auto wide_column = rs::odbc::utf8_to_wide("column\\_a\\_b");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_columns_a_b"),
              text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("column_a_b"),
              text_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, CatalogTypeNamesPreservePostgreSQLTypes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_domain AS integer",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_catalog_type_test("
                  "uuid_key uuid NOT NULL, document jsonb, tags integer[], "
                  "domain_key pg_temp.odbcpp_catalog_domain NOT NULL, "
                  "numeric_key numeric(12,3), memo text, "
                  "PRIMARY KEY(uuid_key, domain_key))",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_catalog_type_test";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));

    struct ExpectedColumn {
        const char* name;
        SQLINTEGER type;
        const char* type_name;
    };
    constexpr std::array<ExpectedColumn, 6> expected_columns{{
        {"uuid_key", SQL_VARCHAR, "uuid"},
        {"document", SQL_VARCHAR, "jsonb"},
        {"tags", SQL_VARCHAR, "integer[]"},
        {"domain_key", SQL_INTEGER, "odbcpp_catalog_domain"},
        {"numeric_key", SQL_NUMERIC, "numeric"},
        {"memo", SQL_LONGVARCHAR, "text"},
    }};
    for (const auto& expected : expected_columns) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>(expected.name), text_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected.type), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<std::string>(expected.type_name),
                  text_cell(hstmt, 6));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected.type),
                  integer_cell(hstmt, 14));
        if (std::strcmp(expected.name, "uuid_key") == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 7));
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 8));
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 16));
        } else if (std::strcmp(expected.name, "domain_key") == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 7));
            EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 8));
        } else if (std::strcmp(expected.name, "numeric_key") == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(12), integer_cell(hstmt, 7));
            EXPECT_EQ(std::optional<SQLINTEGER>(3), integer_cell(hstmt, 9));
        } else if (std::strcmp(expected.name, "memo") == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(1073741824),
                      integer_cell(hstmt, 16));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp_catalog_type_test");
    auto wide_column = rs::odbc::utf8_to_wide("domain_key");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<std::string>("odbcpp_catalog_domain"),
              text_cell(hstmt, 6));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("uuid_key"), text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_VARCHAR), integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("uuid"), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 6));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("domain_key"), text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("odbcpp_catalog_domain"),
              text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 6));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumnsW(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("uuid"), text_cell(hstmt, 4));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_catalog_domain"),
              text_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_catalog_type_function("
                  "id uuid, key pg_temp.odbcpp_catalog_domain, "
                  "tags integer[]) RETURNS jsonb LANGUAGE SQL "
                  "AS 'SELECT ''{}''::jsonb'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp\\_catalog\\_type\\_function";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    constexpr std::array<SQLINTEGER, 4> expected_routine_types{
        SQL_VARCHAR, SQL_INTEGER, SQL_VARCHAR, SQL_VARCHAR};
    for (std::size_t i = 0; i < expected_routine_types.size(); ++i) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_routine_types[i]),
                  integer_cell(hstmt, 6));
        if (i == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 8));
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 9));
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 17));
        } else if (i == 1) {
            const auto type_name = text_cell(hstmt, 7);
            ASSERT_TRUE(type_name.has_value());
            EXPECT_NE(std::string::npos,
                      type_name->find("odbcpp_catalog_domain"));
            EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 8));
            EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 9));
            EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER),
                      integer_cell(hstmt, 15));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT uuid_key FROM odbcpp_catalog_type_test",
        SQL_NTS));
    SQLSMALLINT result_type = 0;
    SQLULEN result_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &result_type, &result_size,
        nullptr, nullptr));
    EXPECT_EQ(SQL_VARCHAR, result_type);
    EXPECT_EQ(36u, result_size);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_procedure = rs::odbc::utf8_to_wide(
        "odbcpp\\_catalog\\_type\\_function");
    ASSERT_TRUE(wide_procedure.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_procedure->data(),
        static_cast<SQLSMALLINT>(wide_procedure->size()),
        nullptr, 0));
    for (const auto expected_type : expected_routine_types) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_type),
                  integer_cell(hstmt, 6));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, NestedDomainsKeepBaseCatalogMetadata) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_inner_domain AS integer",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_outer_domain "
                  "AS pg_temp.odbcpp_inner_domain",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_nested_domain_test("
                  "id pg_temp.odbcpp_outer_domain NOT NULL PRIMARY KEY)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_nested_domain_test";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<std::string>("odbcpp_outer_domain"),
              text_cell(hstmt, 6));
    EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 7));
    EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 8));
    EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 9));
    EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 10));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 14));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp_nested_domain_test");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<std::string>("odbcpp_outer_domain"),
              text_cell(hstmt, 6));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("odbcpp_outer_domain"),
              text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 6));
    EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 7));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_nested_domain_func("
                  "value pg_temp.odbcpp_outer_domain) "
                  "RETURNS pg_temp.odbcpp_outer_domain "
                  "LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp\\_nested\\_domain\\_func";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    for (SQLINTEGER column_type : {1, 5}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(column_type), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 6));
        EXPECT_EQ(std::optional<std::string>("odbcpp_outer_domain"),
                  text_cell(hstmt, 7));
        EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 8));
        EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 9));
        EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 10));
        EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 11));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 15));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT id FROM odbcpp_nested_domain_test",
        SQL_NTS));
    SQLSMALLINT result_type = 0;
    SQLULEN result_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &result_type, &result_size,
        nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, result_type);
    EXPECT_EQ(10u, result_size);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::pg_temp.odbcpp_outer_domain",
        SQL_NTS));
    SQLSMALLINT parameter_type = 0;
    SQLULEN parameter_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &parameter_type, &parameter_size, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, parameter_type);
    EXPECT_EQ(10u, parameter_size);
    result_type = 0;
    result_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &result_type, &result_size,
        nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, result_type);
    EXPECT_EQ(10u, result_size);

    SQLINTEGER input = 37;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &parameter_type, &parameter_size, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, parameter_type);
    EXPECT_EQ(10u, parameter_size);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(input), integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, NestedDomainParameterMetadataKeepsTypmods) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_length_domain "
                  "AS varchar(13)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_nested_length_domain "
                  "AS pg_temp.odbcpp_length_domain",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_scale_domain "
                  "AS numeric(8,3)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt,
        (SQLCHAR*)"SELECT ?::pg_temp.odbcpp_nested_length_domain, "
                  "?::pg_temp.odbcpp_nested_length_domain, "
                  "?::pg_temp.odbcpp_scale_domain",
        SQL_NTS));

    for (SQLUSMALLINT parameter :
         std::array<SQLUSMALLINT, 3>{1, 2, 3}) {
        SQLSMALLINT data_type = 0;
        SQLULEN size = 0;
        SQLSMALLINT scale = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
            hstmt, parameter, &data_type, &size, &scale, nullptr));
        if (parameter < 3) {
            EXPECT_EQ(SQL_VARCHAR, data_type);
            EXPECT_EQ(13u, size);
            EXPECT_EQ(0, scale);
        } else {
            EXPECT_EQ(SQL_NUMERIC, data_type);
            EXPECT_EQ(8u, size);
            EXPECT_EQ(3, scale);
        }
    }

    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLULEN length = 0;
    SQLSMALLINT precision = 0;
    SQLSMALLINT scale = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 1, SQL_DESC_LENGTH, &length, 0, nullptr));
    EXPECT_EQ(13u, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 3, SQL_DESC_PRECISION, &precision, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 3, SQL_DESC_SCALE, &scale, 0, nullptr));
    EXPECT_EQ(8, precision);
    EXPECT_EQ(3, scale);

    SQLSMALLINT result_type = 0;
    SQLULEN result_size = 0;
    SQLSMALLINT result_scale = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &result_type, &result_size,
        &result_scale, nullptr));
    EXPECT_EQ(SQL_VARCHAR, result_type);
    EXPECT_EQ(13u, result_size);
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 3, nullptr, 0, nullptr, &result_type, &result_size,
        &result_scale, nullptr));
    EXPECT_EQ(SQL_NUMERIC, result_type);
    EXPECT_EQ(8u, result_size);
    EXPECT_EQ(3, result_scale);
}

TEST_F(MetadataIntegrationTest, DomainMetadataReprepareAndFailureRecovery) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_reprepare_short AS varchar(13)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_reprepare_long AS varchar(31)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_reprepare_numeric AS numeric(8,3)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_reprepare_numeric2 AS numeric(10,1)",
        SQL_NTS));
    struct ExpectedParameter {
        const char* sql;
        SQLSMALLINT type;
        SQLULEN size;
        SQLSMALLINT scale;
    };
    for (const auto& expected : std::array<ExpectedParameter, 6>{{
             {"SELECT ?::pg_temp.odbcpp_reprepare_short", SQL_VARCHAR, 13, 0},
             {"SELECT ?::pg_temp.odbcpp_reprepare_long", SQL_VARCHAR, 31, 0},
             {"SELECT ?::integer", SQL_INTEGER, 10, 0},
             {"SELECT ?::pg_temp.odbcpp_reprepare_numeric", SQL_NUMERIC, 8, 3},
             {"SELECT ?::pg_temp.odbcpp_reprepare_numeric2", SQL_NUMERIC, 10, 1},
             {"SELECT ?::pg_temp.odbcpp_reprepare_short", SQL_VARCHAR, 13, 0}}}) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
            hstmt, (SQLCHAR*)expected.sql, SQL_NTS));
        // Repeated description exercises cached metadata; reprepare must replace it.
        for (int repeat = 0; repeat < 2; ++repeat) {
            SQLSMALLINT type = 0, scale = -1;
            SQLULEN size = 0;
            ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
                hstmt, 1, &type, &size, &scale, nullptr));
            EXPECT_EQ(expected.type, type);
            EXPECT_EQ(expected.size, size);
            EXPECT_EQ(expected.scale, scale);
        }
    }

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::pg_temp.odbcpp_missing_domain", SQL_NTS));
    SQLSMALLINT type = SQL_SMALLINT, scale = 7;
    SQLULEN size = 123;
    EXPECT_EQ(SQL_ERROR, SQLDescribeParam(
        hstmt, 1, &type, &size, &scale, nullptr));
    EXPECT_EQ(SQL_SMALLINT, type);
    EXPECT_EQ(123u, size);
    EXPECT_EQ(7, scale);
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::pg_temp.odbcpp_reprepare_long", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &type, &size, &scale, nullptr));
    EXPECT_EQ(SQL_VARCHAR, type);
    EXPECT_EQ(31u, size);
    EXPECT_EQ(0, scale);
}

TEST_F(MetadataIntegrationTest, ReprepareRetainsExplicitParameterTypeAndLength) {
    char input[] = "bound value";
    SQLLEN input_length = SQL_NTS;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        42, 0, input, sizeof(input), &input_length));
    for (const auto* sql : {"SELECT ?::text", "SELECT ?::varchar(50)"}) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)sql, SQL_NTS));
        SQLSMALLINT type = 0;
        SQLULEN size = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
            hstmt, 1, &type, &size, nullptr, nullptr));
        EXPECT_EQ(SQL_VARCHAR, type);
        EXPECT_EQ(42u, size);
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        char output[64]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(
            hstmt, 1, SQL_C_CHAR, output, sizeof(output), nullptr));
        EXPECT_STREQ(input, output);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
}

TEST_F(MetadataIntegrationTest, NestedDomainCatalogDimensionsMatchBaseColumns) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_length "
                  "AS varchar(13)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_nested_length "
                  "AS pg_temp.odbcpp_catalog_length",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_scale "
                  "AS numeric(8,3)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_nested_scale "
                  "AS pg_temp.odbcpp_catalog_scale",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_catalog_domain_dimensions("
                  "base_v varchar(13), nested_v "
                  "pg_temp.odbcpp_catalog_nested_length, "
                  "base_n numeric(8,3), domain_n "
                  "pg_temp.odbcpp_catalog_nested_scale, "
                  "PRIMARY KEY(nested_v, domain_n))",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp_catalog_domain_dimensions";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));
    constexpr std::array<SQLUSMALLINT, 6> fields{5, 7, 8, 9, 10, 16};
    std::array<std::optional<SQLINTEGER>, fields.size()> base{};
    std::array<std::optional<SQLINTEGER>, fields.size()> nested_character{};
    std::array<std::optional<SQLINTEGER>, fields.size()> nested_numeric{};
    for (int pair = 0; pair < 2; ++pair) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < fields.size(); ++index) {
            base[index] = integer_cell(hstmt, fields[index]);
        }
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto nested = integer_cell(hstmt, fields[index]);
            EXPECT_EQ(base[index], nested)
                << "pair=" << pair << " field=" << fields[index];
            if (pair == 0) nested_character[index] = nested;
            else nested_numeric[index] = nested;
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp_catalog_domain_dimensions");
    auto wide_column = rs::odbc::utf8_to_wide("nested_v");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    for (std::size_t index = 0; index < fields.size(); ++index) {
        EXPECT_EQ(nested_character[index], integer_cell(hstmt, fields[index]));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    constexpr std::array<SQLUSMALLINT, 4> special_fields{3, 5, 6, 7};
    for (const auto& expected : {nested_character, nested_numeric}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < special_fields.size(); ++index) {
            EXPECT_EQ(expected[index],
                      integer_cell(hstmt, special_fields[index]));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_catalog_domain_func("
                  "v pg_temp.odbcpp_catalog_nested_length, "
                  "n pg_temp.odbcpp_catalog_nested_scale) "
                  "RETURNS pg_temp.odbcpp_catalog_nested_scale "
                  "LANGUAGE SQL AS 'SELECT $2'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp\\_catalog\\_domain\\_func";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    constexpr std::array<SQLUSMALLINT, 6> procedure_fields{
        6, 8, 9, 10, 11, 17};
    for (const auto& expected :
         {nested_character, nested_numeric, nested_numeric}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < procedure_fields.size(); ++index) {
            EXPECT_EQ(expected[index],
                      integer_cell(hstmt, procedure_fields[index]));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumnsW(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        SQL_SCOPE_CURROW, SQL_NO_NULLS));
    for (const auto& expected : {nested_character, nested_numeric}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < special_fields.size(); ++index) {
            EXPECT_EQ(expected[index],
                      integer_cell(hstmt, special_fields[index]));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_procedure = rs::odbc::utf8_to_wide(
        "odbcpp\\_catalog\\_domain\\_func");
    ASSERT_TRUE(wide_procedure.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_procedure->data(),
        static_cast<SQLSMALLINT>(wide_procedure->size()),
        nullptr, 0));
    for (const auto& expected :
         {nested_character, nested_numeric, nested_numeric}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < procedure_fields.size(); ++index) {
            EXPECT_EQ(expected[index],
                      integer_cell(hstmt, procedure_fields[index]));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, NegativeNumericScaleMetadata) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT current_setting('server_version_num')::integer",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    const auto server_version = integer_cell(hstmt, 1);
    ASSERT_TRUE(server_version.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    if (*server_version < 150000) {
        GTEST_SKIP() << "Negative numeric scales require PostgreSQL 15";
    }

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_negative_scale "
                  "AS numeric(8,-2)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_nested_negative_scale "
                  "AS pg_temp.odbcpp_negative_scale",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_negative_scale_metadata("
                  "base_n numeric(8,-2), domain_n "
                  "pg_temp.odbcpp_nested_negative_scale PRIMARY KEY)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"INSERT INTO odbcpp_negative_scale_metadata "
                  "VALUES (-9999999900, -9999999900)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_negative_scale_func("
                  "n pg_temp.odbcpp_nested_negative_scale) "
                  "RETURNS pg_temp.odbcpp_nested_negative_scale "
                  "LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp_negative_scale_metadata";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));
    for (int column = 0; column < 2; ++column) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(-2), integer_cell(hstmt, 9));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp_negative_scale_metadata");
    auto wide_column = rs::odbc::utf8_to_wide("domain_n");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(-2), integer_cell(hstmt, 9));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT base_n, domain_n "
                  "FROM odbcpp_negative_scale_metadata",
        SQL_NTS));
    SQLHDESC row_descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_ROW_DESC, &row_descriptor, 0, nullptr));
    for (SQLUSMALLINT column : std::array<SQLUSMALLINT, 2>{1, 2}) {
        SQLSMALLINT scale = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
            hstmt, column, nullptr, 0, nullptr, nullptr, nullptr,
            &scale, nullptr));
        EXPECT_EQ(-2, scale);
        SQLLEN display_size = 0;
        SQLLEN octet_length = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
            hstmt, column, SQL_DESC_DISPLAY_SIZE, nullptr, 0, nullptr,
            &display_size));
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
            hstmt, column, SQL_DESC_OCTET_LENGTH, nullptr, 0, nullptr,
            &octet_length));
        EXPECT_EQ(11, display_size);
        EXPECT_EQ(11, octet_length);
        SQLLEN descriptor_display = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
            row_descriptor, column, SQL_DESC_DISPLAY_SIZE,
            &descriptor_display, 0, nullptr));
        EXPECT_EQ(display_size, descriptor_display);
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("-9999999900"), text_cell(hstmt, 1));
    EXPECT_EQ(std::optional<std::string>("-9999999900"), text_cell(hstmt, 2));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fractional_numeric("
                  "value numeric(8,10))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"INSERT INTO odbcpp_fractional_numeric "
                  "VALUES (-0.0012345678)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT value FROM odbcpp_fractional_numeric",
        SQL_NTS));
    SQLLEN fractional_display = 0;
    SQLLEN fractional_octets = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_DISPLAY_SIZE, nullptr, 0, nullptr,
        &fractional_display));
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_OCTET_LENGTH, nullptr, 0, nullptr,
        &fractional_octets));
    EXPECT_EQ(13, fractional_display);
    EXPECT_EQ(13, fractional_octets);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("-0.0012345678"), text_cell(hstmt, 1));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(-2), integer_cell(hstmt, 7));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR procedure_name[] = "odbcpp\\_negative\\_scale\\_func";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    for (int column = 0; column < 2; ++column) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(-2), integer_cell(hstmt, 10));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::pg_temp.odbcpp_nested_negative_scale",
        SQL_NTS));
    SQLSMALLINT parameter_scale = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, nullptr, nullptr, &parameter_scale, nullptr));
    EXPECT_EQ(-2, parameter_scale);
    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLSMALLINT descriptor_scale = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 1, SQL_DESC_SCALE, &descriptor_scale, 0, nullptr));
    EXPECT_EQ(-2, descriptor_scale);
}

TEST_F(MetadataIntegrationTest, TemporalMetadataUsesDeclaredPrecision) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_temporal_metadata("
                  "d date, t0 time(0), t2 time(2), ts0 timestamp(0), "
                  "ts3 timestamp(3) NOT NULL, tz0 timetz(0), "
                  "ttz3 timestamptz(3), PRIMARY KEY(ts3))",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_temporal_metadata";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));
    struct ExpectedColumn {
        const char* name;
        SQLSMALLINT type;
        SQLINTEGER size;
        SQLINTEGER transfer_size;
        SQLINTEGER scale;
    };
    constexpr std::array<ExpectedColumn, 7> expected{{
        {"d", SQL_TYPE_DATE, 10, 6, -1},
        {"t0", SQL_TYPE_TIME, 8, 6, 0},
        {"t2", SQL_TYPE_TIME, 11, 6, 2},
        {"ts0", SQL_TYPE_TIMESTAMP, 19, 16, 0},
        {"ts3", SQL_TYPE_TIMESTAMP, 23, 16, 3},
        {"tz0", SQL_TYPE_TIME, 14, 6, 0},
        {"ttz3", SQL_TYPE_TIMESTAMP, 29, 16, 3},
    }};
    for (const auto& column : expected) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>(column.name), text_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(column.type), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(column.size), integer_cell(hstmt, 7));
        EXPECT_EQ(std::optional<SQLINTEGER>(column.transfer_size),
                  integer_cell(hstmt, 8));
        if (column.scale >= 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(column.scale),
                      integer_cell(hstmt, 9));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("ts3"), text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_TYPE_TIMESTAMP),
              integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<SQLINTEGER>(23), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(16), integer_cell(hstmt, 6));
    EXPECT_EQ(std::optional<SQLINTEGER>(3), integer_cell(hstmt, 7));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT d, t0, t2, ts0, ts3, tz0, ttz3 "
                  "FROM odbcpp_temporal_metadata",
        SQL_NTS));
    for (SQLUSMALLINT i = 1; i <= expected.size(); ++i) {
        SQLSMALLINT type = 0;
        SQLULEN size = 0;
        SQLSMALLINT scale = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
            hstmt, i, nullptr, 0, nullptr, &type, &size, &scale, nullptr));
        EXPECT_EQ(expected[i - 1].type, type);
        EXPECT_EQ(static_cast<SQLULEN>(expected[i - 1].size), size);
        if (expected[i - 1].scale >= 0) {
            EXPECT_EQ(expected[i - 1].scale, scale);
        }
    }
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_temporal_metadata_func("
                  "d date, t time, ts timestamp) RETURNS integer "
                  "LANGUAGE SQL AS 'SELECT 1'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp\\_temporal\\_metadata\\_func";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    for (const SQLINTEGER transfer_size : {6, 6, 16}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(transfer_size), integer_cell(hstmt, 9));
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLPrimaryKeys) {
    if (redshift_profile()) {
        // ODBC ordinary optional NULL arguments are driver-dependent. This
        // modern Redshift scope requires the actual nonempty session schema;
        // the PostgreSQL NULL-schema behavior below remains unchanged.
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
            (SQLCHAR*)"CREATE TEMP TABLE odbcpp_primary_key_test("
                      "tenant_id integer, item_id integer, "
                      "CONSTRAINT odbcpp_primary_key_test_pkey "
                      "PRIMARY KEY(tenant_id, item_id))", SQL_NTS));
        std::string session_schema;
        ASSERT_NO_FATAL_FAILURE(resolve_redshift_temporary_schema("odbcpp_primary_key_test", session_schema));
        std::array<SQLCHAR, 1025> database_name{};
        SQLSMALLINT database_length = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(hdbc, SQL_DATABASE_NAME, database_name.data(),
            static_cast<SQLSMALLINT>(database_name.size()), &database_length));
        ASSERT_GT(database_length, 0);
        ASSERT_LT(static_cast<std::size_t>(database_length), database_name.size());
        const std::string database(reinterpret_cast<const char*>(database_name.data()),
            static_cast<std::size_t>(database_length));
        SQLCHAR table_name[] = "odbcpp_primary_key_test";
        auto* schema_name = reinterpret_cast<SQLCHAR*>(session_schema.data());
        const auto read_keys = [&] {
            SQLSMALLINT columns = -1;
            ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
            ASSERT_EQ(6, columns);
            char column_name[64]{};
            char key_name[128]{};
            SQLSMALLINT key_sequence = 0;
            for (SQLSMALLINT row = 1; row <= 2; ++row) {
                ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
                EXPECT_EQ(std::optional<std::string>(database), text_cell(hstmt, 1));
                EXPECT_EQ(std::optional<std::string>(session_schema), text_cell(hstmt, 2));
                EXPECT_EQ(std::optional<std::string>("odbcpp_primary_key_test"), text_cell(hstmt, 3));
                ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 4, SQL_C_CHAR,
                    column_name, sizeof(column_name), nullptr));
                ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 5, SQL_C_SSHORT,
                    &key_sequence, 0, nullptr));
                ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 6, SQL_C_CHAR,
                    key_name, sizeof(key_name), nullptr));
                EXPECT_STREQ(row == 1 ? "tenant_id" : "item_id", column_name);
                EXPECT_EQ(row, key_sequence);
                EXPECT_STREQ("odbcpp_primary_key_test_pkey", key_name);
            }
            EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
            ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        };
        ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(hstmt,
            nullptr, 0, schema_name, SQL_NTS, table_name, SQL_NTS));
        ASSERT_NO_FATAL_FAILURE(read_keys());

        SQLCHAR empty[] = "";
        EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(hstmt,
            nullptr, 0, nullptr, 0, table_name, SQL_NTS));
        EXPECT_EQ("HY000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(hstmt,
            nullptr, 0, empty, SQL_NTS, table_name, SQL_NTS));
        EXPECT_EQ("HY000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(hstmt,
            nullptr, 0, schema_name, SQL_NTS, empty, SQL_NTS));
        EXPECT_EQ("HY000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(hstmt,
            nullptr, 0, schema_name, SQL_NTS, table_name, SQL_NTS));
        ASSERT_NO_FATAL_FAILURE(read_keys());
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_primary_key_test("
                  "tenant_id integer, item_id integer, "
                  "PRIMARY KEY(tenant_id, item_id))",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_primary_key_test";
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS));

    char column_name[64]{};
    char key_name[128]{};
    SQLSMALLINT key_sequence = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &key_sequence, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 6, SQL_C_CHAR, key_name, sizeof(key_name), nullptr));
    EXPECT_STREQ("tenant_id", column_name);
    EXPECT_EQ(1, key_sequence);
    EXPECT_STREQ("odbcpp_primary_key_test_pkey", key_name);

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &key_sequence, 0, nullptr));
    EXPECT_STREQ("item_id", column_name);
    EXPECT_EQ(2, key_sequence);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, PrimaryKeyArgumentsAreLiteralAndKeepEmpty) {
    if (redshift_profile()) {
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
            (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp.primary_'_keys\"("
                      "second_key integer, first_key integer, "
                      "CONSTRAINT odbcpp_literal_pk PRIMARY KEY(first_key, second_key))", SQL_NTS));
        std::string session_schema;
        ASSERT_NO_FATAL_FAILURE(resolve_redshift_temporary_schema("odbcpp.primary_'_keys", session_schema));
        SQLCHAR table_name[] = "odbcpp.primary_'_keys";
        auto* schema_name = reinterpret_cast<SQLCHAR*>(session_schema.data());
        ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(hstmt,
            nullptr, 0, schema_name, SQL_NTS, table_name, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        const auto catalog = text_cell(hstmt, 1);
        ASSERT_TRUE(catalog.has_value());
        EXPECT_EQ(std::optional<std::string>(session_schema), text_cell(hstmt, 2));
        EXPECT_EQ(std::optional<std::string>("odbcpp.primary_'_keys"), text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<std::string>("odbcpp_literal_pk"), text_cell(hstmt, 6));
        EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(hstmt,
            nullptr, 0, schema_name, SQL_NTS, table_name, SQL_NTS));
        EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>("second_key"), text_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(2), integer_cell(hstmt, 5));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

        SQLCHAR empty[] = "";
        SQLCHAR wildcard[] = "%";
        const auto expect_local_refusal = [&](SQLCHAR* requested_catalog,
                                              SQLCHAR* requested_schema,
                                              SQLCHAR* requested_table) {
            EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(hstmt,
                requested_catalog, requested_catalog ? SQL_NTS : 0,
                requested_schema, requested_schema ? SQL_NTS : 0,
                requested_table, SQL_NTS));
            EXPECT_EQ("HY000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        };
        // All five original shapes fail exact-name validation before native I/O.
        expect_local_refusal(empty, nullptr, table_name);
        expect_local_refusal(nullptr, empty, table_name);
        expect_local_refusal(nullptr, nullptr, empty);
        expect_local_refusal(wildcard, nullptr, table_name);
        expect_local_refusal(nullptr, nullptr, wildcard);

        // A literal schema named '%' passes the local guard and reaches SHOW.
        // Preserve the original successful-empty oracle; its native outcome is
        // still an open predicate, not a manufactured HY000 or a waived case.
        ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(hstmt,
            nullptr, 0, wildcard, SQL_NTS, table_name, SQL_NTS));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

        std::string catalog_name = *catalog;
        ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(hstmt,
            reinterpret_cast<SQLCHAR*>(catalog_name.data()), SQL_NTS,
            schema_name, SQL_NTS, table_name, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        auto wide_catalog = rs::odbc::utf8_to_wide(catalog_name);
        auto wide_schema = rs::odbc::utf8_to_wide(session_schema);
        auto wide_table = rs::odbc::utf8_to_wide("odbcpp.primary_'_keys");
        ASSERT_TRUE(wide_catalog.has_value());
        ASSERT_TRUE(wide_schema.has_value());
        ASSERT_TRUE(wide_table.has_value());
        ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeysW(hstmt,
            wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
            wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
            wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size())));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp.primary_'_keys\"("
                  "second_key integer, first_key integer, "
                  "CONSTRAINT odbcpp_literal_pk "
                  "PRIMARY KEY(first_key, second_key))",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp.primary_'_keys";
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    const auto catalog = text_cell(hstmt, 1);
    const auto schema = text_cell(hstmt, 2);
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(std::optional<std::string>("odbcpp.primary_'_keys"),
              text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<std::string>("odbcpp_literal_pk"),
              text_cell(hstmt, 6));

    EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("second_key"), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(2), integer_cell(hstmt, 5));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR wildcard[] = "%";
    auto expect_no_keys = [&](SQLCHAR* catalog_name,
                              SQLSMALLINT catalog_length,
                              SQLCHAR* schema_name,
                              SQLSMALLINT schema_length,
                              SQLCHAR* requested_table,
                              SQLSMALLINT table_length) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(
            hstmt, catalog_name, catalog_length, schema_name, schema_length,
            requested_table, table_length));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_keys(empty, SQL_NTS, nullptr, 0, table_name, SQL_NTS);
    expect_no_keys(nullptr, 0, empty, SQL_NTS, table_name, SQL_NTS);
    expect_no_keys(nullptr, 0, nullptr, 0, empty, SQL_NTS);
    expect_no_keys(wildcard, SQL_NTS, nullptr, 0, table_name, SQL_NTS);
    expect_no_keys(nullptr, 0, wildcard, SQL_NTS, table_name, SQL_NTS);
    expect_no_keys(nullptr, 0, nullptr, 0, wildcard, SQL_NTS);

    std::string catalog_name = *catalog;
    std::string schema_name = *schema;
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(
        hstmt, reinterpret_cast<SQLCHAR*>(catalog_name.data()), SQL_NTS,
        reinterpret_cast<SQLCHAR*>(schema_name.data()), SQL_NTS,
        table_name, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_catalog = rs::odbc::utf8_to_wide(catalog_name);
    auto wide_schema = rs::odbc::utf8_to_wide(schema_name);
    auto wide_table = rs::odbc::utf8_to_wide("odbcpp.primary_'_keys");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeysW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLForeignKeys) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_foreign_parent("
                  "tenant_id integer, item_id integer, "
                  "PRIMARY KEY(tenant_id, item_id))",
        SQL_NTS));
    const char* foreign_definition = redshift_profile()
        ? "CREATE TEMP TABLE odbcpp_foreign_child(tenant_id integer, item_id integer, "
          "CONSTRAINT odbcpp_child_parent_fk FOREIGN KEY(tenant_id, item_id) REFERENCES "
          "odbcpp_foreign_parent(tenant_id, item_id))"
        : "CREATE TEMP TABLE odbcpp_foreign_child(tenant_id integer, item_id integer, "
          "CONSTRAINT odbcpp_child_parent_fk FOREIGN KEY(tenant_id, item_id) REFERENCES "
          "odbcpp_foreign_parent(tenant_id, item_id) ON UPDATE CASCADE ON DELETE SET NULL)";
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        reinterpret_cast<SQLCHAR*>(const_cast<char*>(foreign_definition)), SQL_NTS));
    std::string session_schema;
    if (redshift_profile()) {
        ASSERT_NO_FATAL_FAILURE(resolve_redshift_temporary_schema("odbcpp_foreign_child", session_schema));
    }
    SQLCHAR foreign_table[] = "odbcpp_foreign_child";
    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0,
        nullptr, 0,
        redshift_profile() ? reinterpret_cast<SQLCHAR*>(session_schema.data()) : nullptr,
        redshift_profile() ? SQL_NTS : 0, foreign_table, SQL_NTS));

    char primary_column[64]{};
    char foreign_column[64]{};
    char foreign_key_name[128]{};
    SQLSMALLINT key_sequence = 0;
    SQLSMALLINT update_rule = -1;
    SQLSMALLINT delete_rule = -1;
    SQLSMALLINT deferrability = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, primary_column, sizeof(primary_column), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_CHAR, foreign_column, sizeof(foreign_column), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 9, SQL_C_SSHORT, &key_sequence, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 10, SQL_C_SSHORT, &update_rule, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 11, SQL_C_SSHORT, &delete_rule, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 12, SQL_C_CHAR, foreign_key_name,
        sizeof(foreign_key_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 14, SQL_C_SSHORT, &deferrability, 0, nullptr));
    EXPECT_STREQ("tenant_id", primary_column);
    EXPECT_STREQ("tenant_id", foreign_column);
    EXPECT_EQ(1, key_sequence);
    EXPECT_EQ(redshift_profile() ? SQL_NO_ACTION : SQL_CASCADE, update_rule);
    EXPECT_EQ(redshift_profile() ? SQL_NO_ACTION : SQL_SET_NULL, delete_rule);
    EXPECT_STREQ("odbcpp_child_parent_fk", foreign_key_name);
    EXPECT_EQ(SQL_NOT_DEFERRABLE, deferrability);
    if (redshift_profile()) {
        EXPECT_EQ(std::optional<std::string>(session_schema), text_cell(hstmt, 2));
        EXPECT_EQ(std::optional<std::string>(session_schema), text_cell(hstmt, 6));
    }

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, primary_column, sizeof(primary_column), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_CHAR, foreign_column, sizeof(foreign_column), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 9, SQL_C_SSHORT, &key_sequence, 0, nullptr));
    EXPECT_STREQ("item_id", primary_column);
    EXPECT_STREQ("item_id", foreign_column);
    EXPECT_EQ(2, key_sequence);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ForeignKeyFiltersRulesAndPrimaryTargetsFollowOdbc) {
    if (redshift_profile()) {
        // These five constraints refer to the primary key. The sixth declared
        // reference targets UNIQUE and must NOT appear in SQLForeignKeys.
        const std::map<std::string, std::string> primary_references{
            {"fk_rule_cascade", "cascade_id"},
            {"fk_rule_restrict", "restrict_id"},
            {"fk_rule_set_null", "null_id"},
            {"fk_rule_set_default", "default_id"},
            {"fk_rule_no_action", "no_action_id"},
        };
        ASSERT_EQ(5u, primary_references.size());
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
            (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_parent("
                      "id integer PRIMARY KEY, alternate integer UNIQUE)", SQL_NTS));
        // Redshift declarations are informational: PostgreSQL referential
        // actions and DEFERRABLE syntax are not part of its CREATE TABLE grammar.
        // Retain the same six relationships and constraint names, not six
        // expected primary-reference rows.
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
            (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_child("
                      "cascade_id integer, restrict_id integer, null_id integer, "
                      "default_id integer DEFAULT 1, no_action_id integer, unique_id integer, "
                      "CONSTRAINT fk_rule_cascade FOREIGN KEY(cascade_id) "
                      "REFERENCES odbcpp_fk_rules_parent(id), "
                      "CONSTRAINT fk_rule_restrict FOREIGN KEY(restrict_id) "
                      "REFERENCES odbcpp_fk_rules_parent(id), "
                      "CONSTRAINT fk_rule_set_null FOREIGN KEY(null_id) "
                      "REFERENCES odbcpp_fk_rules_parent(id), "
                      "CONSTRAINT fk_rule_set_default FOREIGN KEY(default_id) "
                      "REFERENCES odbcpp_fk_rules_parent(id), "
                      "CONSTRAINT fk_rule_no_action FOREIGN KEY(no_action_id) "
                      "REFERENCES odbcpp_fk_rules_parent(id), "
                      "CONSTRAINT fk_rule_unique FOREIGN KEY(unique_id) "
                      "REFERENCES odbcpp_fk_rules_parent(alternate))", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
            (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_other("
                      "id integer, CONSTRAINT fk_rule_cascade FOREIGN KEY(id) "
                      "REFERENCES odbcpp_fk_rules_parent(id))", SQL_NTS));

        std::string session_schema;
        ASSERT_NO_FATAL_FAILURE(resolve_redshift_temporary_schema("odbcpp_fk_rules_child", session_schema));
        std::array<SQLCHAR, 1025> database_name{};
        SQLSMALLINT database_length = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(hdbc, SQL_DATABASE_NAME, database_name.data(),
            static_cast<SQLSMALLINT>(database_name.size()), &database_length));
        ASSERT_GT(database_length, 0);
        ASSERT_LT(static_cast<std::size_t>(database_length), database_name.size());
        const std::string database(reinterpret_cast<const char*>(database_name.data()),
            static_cast<std::size_t>(database_length));
        SQLCHAR parent[] = "odbcpp_fk_rules_parent";
        SQLCHAR child[] = "odbcpp_fk_rules_child";
        auto* schema_name = reinterpret_cast<SQLCHAR*>(session_schema.data());

        // Own all ten text fields and assert all four numeric fields. A row
        // belonging to the UNIQUE reference is an error, never filtered here.
        using Names = std::array<std::optional<std::string>, 10>;
        using Rows = std::map<std::pair<std::string, std::string>, Names>;
        const auto read_keys = [&](Rows& rows, std::vector<std::string>* table_order) {
            SQLRETURN fetched = SQL_ERROR;
            while ((fetched = SQLFetch(hstmt)) == SQL_SUCCESS) {
                Names names;
                std::array<std::optional<SQLINTEGER>, 4> numbers;
                std::size_t name_index = 0;
                std::size_t number_index = 0;
                for (SQLUSMALLINT column = 1; column <= 14; ++column) {
                    if ((column >= 9 && column <= 11) || column == 14) {
                        numbers[number_index++] = integer_cell(hstmt, column);
                    } else {
                        names[name_index++] = text_cell(hstmt, column);
                    }
                }
                for (std::size_t i = 0; i < names.size() - 1; ++i) {
                    ASSERT_TRUE(names[i].has_value());
                }
                EXPECT_EQ(std::optional<std::string>(database), names[0]);
                EXPECT_EQ(std::optional<std::string>(session_schema), names[1]);
                EXPECT_EQ(std::optional<std::string>("odbcpp_fk_rules_parent"), names[2]);
                EXPECT_EQ(std::optional<std::string>("id"), names[3]);
                EXPECT_EQ(std::optional<std::string>(database), names[4]);
                EXPECT_EQ(std::optional<std::string>(session_schema), names[5]);
                EXPECT_NE("fk_rule_unique", *names[8]);
                EXPECT_EQ(std::optional<SQLINTEGER>(1), numbers[0]);
                EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NO_ACTION), numbers[1]);
                EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NO_ACTION), numbers[2]);
                EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NOT_DEFERRABLE), numbers[3]);
                if (*names[6] == "odbcpp_fk_rules_child") {
                    const auto expected = primary_references.find(*names[8]);
                    ASSERT_NE(primary_references.end(), expected);
                    EXPECT_EQ(std::optional<std::string>(expected->second), names[7]);
                } else {
                    EXPECT_EQ(std::optional<std::string>("odbcpp_fk_rules_other"), names[6]);
                    EXPECT_EQ(std::optional<std::string>("id"), names[7]);
                    EXPECT_EQ(std::optional<std::string>("fk_rule_cascade"), names[8]);
                }
                const auto key = std::pair{*names[6], *names[8]};
                ASSERT_TRUE(rows.emplace(key, names).second);
                if (table_order) {
                    table_order->push_back(*names[6]);
                }
            }
            ASSERT_EQ(SQL_NO_DATA, fetched);
            ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        };

        ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(hstmt,
            nullptr, 0, nullptr, 0, nullptr, 0,
            nullptr, 0, schema_name, SQL_NTS, child, SQL_NTS));
        Rows imported;
        ASSERT_NO_FATAL_FAILURE(read_keys(imported, nullptr));
        ASSERT_EQ(5u, imported.size());
        std::map<std::string, std::string> imported_references;
        for (const auto& [key, names] : imported) {
            EXPECT_EQ("odbcpp_fk_rules_child", key.first);
            imported_references.emplace(key.second, *names[7]);
        }
        EXPECT_EQ(primary_references, imported_references);

        ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(hstmt,
            nullptr, 0, schema_name, SQL_NTS, parent, SQL_NTS,
            nullptr, 0, nullptr, 0, nullptr, 0));
        Rows exported;
        std::vector<std::string> exported_order;
        ASSERT_NO_FATAL_FAILURE(read_keys(exported, &exported_order));
        EXPECT_EQ(6u, exported.size());
        const std::vector<std::string> expected_order{
            "odbcpp_fk_rules_child", "odbcpp_fk_rules_child", "odbcpp_fk_rules_child",
            "odbcpp_fk_rules_child", "odbcpp_fk_rules_child", "odbcpp_fk_rules_other"};
        EXPECT_EQ(expected_order, exported_order);
        for (const auto& [key, names] : imported) {
            const auto match = exported.find(key);
            ASSERT_NE(exported.end(), match);
            EXPECT_EQ(names, match->second);
        }

        ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(hstmt,
            nullptr, 0, schema_name, SQL_NTS, parent, SQL_NTS,
            nullptr, 0, schema_name, SQL_NTS, child, SQL_NTS));
        Rows both;
        ASSERT_NO_FATAL_FAILURE(read_keys(both, nullptr));
        EXPECT_EQ(5u, both.size());
        EXPECT_EQ(imported, both);

        auto wide_catalog = rs::odbc::utf8_to_wide(database);
        auto wide_schema = rs::odbc::utf8_to_wide(session_schema);
        auto wide_parent = rs::odbc::utf8_to_wide("odbcpp_fk_rules_parent");
        auto wide_child = rs::odbc::utf8_to_wide("odbcpp_fk_rules_child");
        ASSERT_TRUE(wide_catalog.has_value());
        ASSERT_TRUE(wide_schema.has_value());
        ASSERT_TRUE(wide_parent.has_value());
        ASSERT_TRUE(wide_child.has_value());
        ASSERT_EQ(SQL_SUCCESS, SQLForeignKeysW(hstmt,
            wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
            wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
            wide_parent->data(), static_cast<SQLSMALLINT>(wide_parent->size()),
            wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
            wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
            wide_child->data(), static_cast<SQLSMALLINT>(wide_child->size())));
        Rows both_wide;
        ASSERT_NO_FATAL_FAILURE(read_keys(both_wide, nullptr));
        EXPECT_EQ(5u, both_wide.size());
        EXPECT_EQ(both, both_wide);

        // Modern ordinary requests require exact nonempty selected schemas.
        // Preserve the PostgreSQL empty/wildcard successful-empty controls below;
        // here explicitly assert the existing local NULL-schema refusal instead.
        EXPECT_EQ(SQL_ERROR, SQLForeignKeys(hstmt,
            nullptr, 0, nullptr, 0, parent, SQL_NTS,
            nullptr, 0, nullptr, 0, nullptr, 0));
        EXPECT_EQ("HY000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        EXPECT_EQ(SQL_ERROR, SQLForeignKeys(hstmt,
            nullptr, 0, nullptr, 0, nullptr, 0,
            nullptr, 0, nullptr, 0, child, SQL_NTS));
        EXPECT_EQ("HY000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT CAST(1 AS INTEGER)", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 1));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
        return;
    }
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_parent("
                  "id integer PRIMARY KEY, alternate integer UNIQUE)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_child("
                  "cascade_id integer, restrict_id integer, null_id integer, "
                  "default_id integer DEFAULT 1, no_action_id integer, "
                  "unique_id integer, "
                  "CONSTRAINT fk_rule_cascade FOREIGN KEY(cascade_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id) "
                  "ON UPDATE CASCADE ON DELETE CASCADE, "
                  "CONSTRAINT fk_rule_restrict FOREIGN KEY(restrict_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id) "
                  "ON UPDATE RESTRICT ON DELETE RESTRICT "
                  "DEFERRABLE INITIALLY IMMEDIATE, "
                  "CONSTRAINT fk_rule_set_null FOREIGN KEY(null_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id) "
                  "ON UPDATE SET NULL ON DELETE SET NULL "
                  "DEFERRABLE INITIALLY DEFERRED, "
                  "CONSTRAINT fk_rule_set_default FOREIGN KEY(default_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id) "
                  "ON UPDATE SET DEFAULT ON DELETE SET DEFAULT, "
                  "CONSTRAINT fk_rule_no_action FOREIGN KEY(no_action_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id), "
                  "CONSTRAINT fk_rule_unique FOREIGN KEY(unique_id) "
                  "REFERENCES odbcpp_fk_rules_parent(alternate))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_other("
                  "id integer, CONSTRAINT fk_rule_cascade FOREIGN KEY(id) "
                  "REFERENCES odbcpp_fk_rules_parent(id))",
        SQL_NTS));

    SQLCHAR parent[] = "odbcpp_fk_rules_parent";
    SQLCHAR child[] = "odbcpp_fk_rules_child";
    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0,
        nullptr, 0, nullptr, 0, child, SQL_NTS));

    std::optional<std::string> catalog;
    std::optional<std::string> schema;
    std::map<std::string, std::array<SQLINTEGER, 3>> rules;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        if (!catalog) catalog = text_cell(hstmt, 1);
        if (!schema) schema = text_cell(hstmt, 2);
        const auto update_rule = integer_cell(hstmt, 10);
        const auto delete_rule = integer_cell(hstmt, 11);
        const auto name = text_cell(hstmt, 12);
        const auto deferrability = integer_cell(hstmt, 14);
        ASSERT_TRUE(update_rule.has_value());
        ASSERT_TRUE(delete_rule.has_value());
        ASSERT_TRUE(name.has_value());
        ASSERT_TRUE(deferrability.has_value());
        rules.emplace(*name, std::array<SQLINTEGER, 3>{
            *update_rule, *delete_rule, *deferrability});
    }
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    const std::map<std::string, std::array<SQLINTEGER, 3>> expected_rules{
        {"fk_rule_cascade", {SQL_CASCADE, SQL_CASCADE, SQL_NOT_DEFERRABLE}},
        {"fk_rule_restrict", {SQL_RESTRICT, SQL_RESTRICT,
                               SQL_INITIALLY_IMMEDIATE}},
        {"fk_rule_set_null", {SQL_SET_NULL, SQL_SET_NULL,
                               SQL_INITIALLY_DEFERRED}},
        {"fk_rule_set_default", {SQL_SET_DEFAULT, SQL_SET_DEFAULT,
                                  SQL_NOT_DEFERRABLE}},
        {"fk_rule_no_action", {SQL_NO_ACTION, SQL_NO_ACTION,
                                SQL_NOT_DEFERRABLE}},
    };
    EXPECT_EQ(expected_rules, rules);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, parent, SQL_NTS,
        nullptr, 0, nullptr, 0, nullptr, 0));
    int referring_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        ++referring_rows;
        EXPECT_EQ(std::optional<std::string>("odbcpp_fk_rules_parent"),
                  text_cell(hstmt, 3));
        const auto foreign_table = text_cell(hstmt, 7);
        ASSERT_TRUE(foreign_table.has_value());
        if (referring_rows <= 5) {
            EXPECT_EQ("odbcpp_fk_rules_child", *foreign_table);
        } else {
            EXPECT_EQ("odbcpp_fk_rules_other", *foreign_table);
        }
    }
    EXPECT_EQ(6, referring_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, parent, SQL_NTS,
        nullptr, 0, nullptr, 0, child, SQL_NTS));
    int matching_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++matching_rows;
    EXPECT_EQ(5, matching_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR wildcard[] = "%";
    auto expect_no_foreign_keys = [&](SQLCHAR* pk_catalog,
                                      SQLCHAR* pk_schema,
                                      SQLCHAR* pk_table,
                                      SQLCHAR* fk_catalog,
                                      SQLCHAR* fk_schema,
                                      SQLCHAR* fk_table) {
        ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
            hstmt, pk_catalog, pk_catalog ? SQL_NTS : 0,
            pk_schema, pk_schema ? SQL_NTS : 0,
            pk_table, pk_table ? SQL_NTS : 0,
            fk_catalog, fk_catalog ? SQL_NTS : 0,
            fk_schema, fk_schema ? SQL_NTS : 0,
            fk_table, fk_table ? SQL_NTS : 0));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_foreign_keys(empty, nullptr, parent,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(nullptr, empty, parent,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(wildcard, nullptr, parent,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(nullptr, wildcard, parent,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(nullptr, nullptr, wildcard,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           empty, nullptr, child);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           nullptr, empty, child);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           wildcard, nullptr, child);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           nullptr, wildcard, child);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           nullptr, nullptr, wildcard);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_parent = rs::odbc::utf8_to_wide("odbcpp_fk_rules_parent");
    auto wide_child = rs::odbc::utf8_to_wide("odbcpp_fk_rules_child");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_parent.has_value());
    ASSERT_TRUE(wide_child.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeysW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_parent->data(), static_cast<SQLSMALLINT>(wide_parent->size()),
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_child->data(), static_cast<SQLSMALLINT>(wide_child->size())));
    matching_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++matching_rows;
    EXPECT_EQ(5, matching_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLIndexes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_index_test("
                  "id integer, value text)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_index_test_id_idx "
                  "ON odbcpp_index_test(id DESC)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_index_test";
    ASSERT_EQ(SQL_SUCCESS, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_UNIQUE, SQL_QUICK));

    char index_name[128]{};
    char column_name[64]{};
    char ordering[2]{};
    SQLSMALLINT non_unique = SQL_TRUE;
    SQLSMALLINT type = 0;
    SQLSMALLINT ordinal_position = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_SSHORT, &non_unique, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 6, SQL_C_CHAR, index_name, sizeof(index_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 7, SQL_C_SSHORT, &type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_SSHORT, &ordinal_position, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 9, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 10, SQL_C_CHAR, ordering, sizeof(ordering), nullptr));
    EXPECT_EQ(SQL_FALSE, non_unique);
    EXPECT_STREQ("odbcpp_index_test_id_idx", index_name);
    EXPECT_EQ(SQL_INDEX_OTHER, type);
    EXPECT_EQ(1, ordinal_position);
    EXPECT_STREQ("id", column_name);
    EXPECT_STREQ("D", ordering);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, StatisticsReportPostgreSQLIndexSemantics) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp.stats_'_table\"("
                  "id integer, value text)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_stats_unique "
                  "ON \"odbcpp.stats_'_table\"(id DESC) "
                  "WHERE value IS NOT NULL",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE INDEX odbcpp_stats_expression "
                  "ON \"odbcpp.stats_'_table\"((lower(value)))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE INDEX odbcpp_stats_hash "
                  "ON \"odbcpp.stats_'_table\" USING hash(value)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE INDEX odbcpp_stats_covering "
                  "ON \"odbcpp.stats_'_table\"(id) INCLUDE(value)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE INDEX odbcpp_stats_clustered "
                  "ON \"odbcpp.stats_'_table\"(id)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CLUSTER \"odbcpp.stats_'_table\" "
                  "USING odbcpp_stats_clustered",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp.stats_'_table";
    ASSERT_EQ(SQL_SUCCESS, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_ALL, SQL_QUICK));
    EXPECT_EQ(SQL_ERROR, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_ALL, SQL_QUICK));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    std::optional<std::string> catalog;
    std::optional<std::string> schema;
    bool found_unique = false;
    bool found_expression = false;
    bool found_hash = false;
    bool found_covering_key = false;
    bool found_covering_include = false;
    bool found_clustered = false;
    int index_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        ++index_rows;
        if (!catalog) catalog = text_cell(hstmt, 1);
        if (!schema) schema = text_cell(hstmt, 2);
        const auto non_unique = integer_cell(hstmt, 4);
        const auto index_name = text_cell(hstmt, 6);
        const auto type = integer_cell(hstmt, 7);
        const auto ordinal = integer_cell(hstmt, 8);
        const auto column = text_cell(hstmt, 9);
        const auto direction = text_cell(hstmt, 10);
        const auto cardinality = integer_cell(hstmt, 11);
        const auto pages = integer_cell(hstmt, 12);
        const auto filter = text_cell(hstmt, 13);
        ASSERT_TRUE(non_unique.has_value());
        ASSERT_TRUE(index_name.has_value());
        ASSERT_TRUE(type.has_value());
        ASSERT_TRUE(ordinal.has_value());
        ASSERT_TRUE(column.has_value());
        EXPECT_EQ(std::nullopt, cardinality);
        EXPECT_TRUE(pages.has_value());

        if (*index_name == "odbcpp_stats_unique") {
            found_unique = true;
            EXPECT_EQ(SQL_FALSE, *non_unique);
            EXPECT_EQ(SQL_INDEX_OTHER, *type);
            EXPECT_EQ(1, *ordinal);
            EXPECT_EQ("id", *column);
            EXPECT_EQ(std::optional<std::string>("D"), direction);
            ASSERT_TRUE(filter.has_value());
            EXPECT_NE(std::string::npos, filter->find("value IS NOT NULL"));
        } else if (*index_name == "odbcpp_stats_expression") {
            found_expression = true;
            EXPECT_EQ(SQL_TRUE, *non_unique);
            EXPECT_EQ(SQL_INDEX_OTHER, *type);
            EXPECT_EQ(1, *ordinal);
            EXPECT_EQ("lower(value)", *column);
            EXPECT_EQ(std::optional<std::string>("A"), direction);
            EXPECT_EQ(std::nullopt, filter);
        } else if (*index_name == "odbcpp_stats_hash") {
            found_hash = true;
            EXPECT_EQ(SQL_TRUE, *non_unique);
            EXPECT_EQ(SQL_INDEX_HASHED, *type);
            EXPECT_EQ(1, *ordinal);
            EXPECT_EQ("value", *column);
            EXPECT_EQ(std::nullopt, direction);
            EXPECT_EQ(std::nullopt, filter);
        } else if (*index_name == "odbcpp_stats_covering") {
            EXPECT_EQ(SQL_TRUE, *non_unique);
            EXPECT_EQ(SQL_INDEX_OTHER, *type);
            EXPECT_EQ(std::nullopt, filter);
            if (*ordinal == 1) {
                found_covering_key = true;
                EXPECT_EQ("id", *column);
                EXPECT_EQ(std::optional<std::string>("A"), direction);
            } else if (*ordinal == 2) {
                found_covering_include = true;
                EXPECT_EQ("value", *column);
                EXPECT_EQ(std::nullopt, direction);
            } else {
                ADD_FAILURE() << "Unexpected covering-index ordinal "
                              << *ordinal;
            }
        } else if (*index_name == "odbcpp_stats_clustered") {
            found_clustered = true;
            EXPECT_EQ(SQL_TRUE, *non_unique);
            EXPECT_EQ(SQL_INDEX_CLUSTERED, *type);
            EXPECT_EQ(1, *ordinal);
            EXPECT_EQ("id", *column);
            EXPECT_EQ(std::optional<std::string>("A"), direction);
            EXPECT_EQ(std::nullopt, filter);
        } else {
            ADD_FAILURE() << "Unexpected index " << *index_name;
        }
    }
    EXPECT_EQ(6, index_rows);
    EXPECT_TRUE(found_unique);
    EXPECT_TRUE(found_expression);
    EXPECT_TRUE(found_hash);
    EXPECT_TRUE(found_covering_key);
    EXPECT_TRUE(found_covering_include);
    EXPECT_TRUE(found_clustered);
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_UNIQUE, SQL_QUICK));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_stats_unique"),
              text_cell(hstmt, 6));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    EXPECT_EQ(SQL_ERROR, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_ALL, SQL_ENSURE));
    EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR wildcard[] = "%";
    auto expect_no_statistics = [&](SQLCHAR* requested_catalog,
                                    SQLCHAR* requested_schema,
                                    SQLCHAR* requested_table) {
        ASSERT_EQ(SQL_SUCCESS, SQLStatistics(
            hstmt, requested_catalog, requested_catalog ? SQL_NTS : 0,
            requested_schema, requested_schema ? SQL_NTS : 0,
            requested_table, SQL_NTS, SQL_INDEX_ALL, SQL_QUICK));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_statistics(empty, nullptr, table_name);
    expect_no_statistics(nullptr, empty, table_name);
    expect_no_statistics(nullptr, nullptr, empty);
    expect_no_statistics(wildcard, nullptr, table_name);
    expect_no_statistics(nullptr, wildcard, table_name);
    expect_no_statistics(nullptr, nullptr, wildcard);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_table = rs::odbc::utf8_to_wide("odbcpp.stats_'_table");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLStatisticsW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        SQL_INDEX_ALL, SQL_QUICK));
    index_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++index_rows;
    EXPECT_EQ(6, index_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLRoutines) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_routine_anchor(value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_catalog_function("
                  "first integer) RETURNS integer LANGUAGE SQL "
                  "AS 'SELECT first'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp_catalog_function";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS));

    char returned_name[128]{};
    SQLSMALLINT input_count = -1;
    SQLSMALLINT output_count = -1;
    SQLSMALLINT procedure_type = SQL_PT_UNKNOWN;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_CHAR, returned_name, sizeof(returned_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_SSHORT, &input_count, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &output_count, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_SSHORT, &procedure_type, 0, nullptr));
    EXPECT_STREQ("odbcpp_catalog_function", returned_name);
    EXPECT_EQ(1, input_count);
    EXPECT_EQ(0, output_count);
    EXPECT_EQ(SQL_PT_FUNCTION, procedure_type);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, RoutinePatternsTypesAndOverloadsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_routine_a_b(integer) "
                  "RETURNS integer LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_routine_a_b(text) "
                  "RETURNS text LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_routine_axb(integer) "
                  "RETURNS integer LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE PROCEDURE pg_temp.odbcpp_procedure_a_b(integer) "
                  "LANGUAGE SQL AS 'SELECT 1'",
        SQL_NTS));

    SQLCHAR wildcard_pattern[] = "odbcpp_routine_a_b";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, wildcard_pattern, SQL_NTS));
    int wildcard_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++wildcard_rows;
    EXPECT_EQ(3, wildcard_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR escaped_pattern[] = "odbcpp\\_routine\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, escaped_pattern, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, escaped_pattern, SQL_NTS));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    std::optional<std::string> catalog;
    std::optional<std::string> schema;
    int overload_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        ++overload_rows;
        if (!catalog) catalog = text_cell(hstmt, 1);
        if (!schema) schema = text_cell(hstmt, 2);
        EXPECT_EQ(std::optional<std::string>("odbcpp_routine_a_b"),
                  text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(-1), integer_cell(hstmt, 6));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PT_FUNCTION),
                  integer_cell(hstmt, 8));
    }
    EXPECT_EQ(2, overload_rows);
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR procedure_pattern[] = "odbcpp\\_procedure\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, procedure_pattern, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PT_PROCEDURE),
              integer_cell(hstmt, 8));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR percent[] = "%";
    auto expect_no_routines = [&](SQLCHAR* requested_catalog,
                                  SQLCHAR* requested_schema,
                                  SQLCHAR* requested_procedure) {
        ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
            hstmt, requested_catalog, requested_catalog ? SQL_NTS : 0,
            requested_schema, requested_schema ? SQL_NTS : 0,
            requested_procedure,
            requested_procedure ? SQL_NTS : 0));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_routines(empty, nullptr, escaped_pattern);
    expect_no_routines(percent, nullptr, escaped_pattern);
    expect_no_routines(nullptr, empty, escaped_pattern);
    expect_no_routines(nullptr, nullptr, empty);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_pattern = rs::odbc::utf8_to_wide("odbcpp\\_routine\\_a\\_b");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_pattern.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLProceduresW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_pattern->data(), static_cast<SQLSMALLINT>(wide_pattern->size())));
    overload_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++overload_rows;
    EXPECT_EQ(2, overload_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, ProcedureCountsFollowPostgreSQLArgumentModes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_modes_inout("
                  "IN first integer, INOUT shared integer, OUT final text) "
                  "LANGUAGE SQL AS "
                  "'SELECT shared + first, first::text'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_modes_variadic("
                  "VARIADIC items integer[]) RETURNS integer "
                  "LANGUAGE SQL AS 'SELECT cardinality(items)'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_modes_table(first integer) "
                  "RETURNS TABLE(total integer, label text) LANGUAGE SQL "
                  "AS 'SELECT first, first::text'",
        SQL_NTS));

    auto expect_counts = [&](SQLCHAR* routine_pattern,
                             SQLINTEGER expected_inputs,
                             SQLINTEGER expected_outputs) {
        ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
            hstmt, nullptr, 0, nullptr, 0,
            routine_pattern, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_inputs),
                  integer_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_outputs),
                  integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(-1), integer_cell(hstmt, 6));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PT_FUNCTION),
                  integer_cell(hstmt, 8));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };

    SQLCHAR inout_name[] = "odbcpp\\_modes\\_inout";
    SQLCHAR variadic_name[] = "odbcpp\\_modes\\_variadic";
    SQLCHAR table_name[] = "odbcpp\\_modes\\_table";
    expect_counts(inout_name, 2, 2);
    expect_counts(variadic_name, 1, 0);
    expect_counts(table_name, 1, 2);
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLRoutineColumns) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_routine_column_anchor("
                  "value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_column_function("
                  "first integer) RETURNS integer LANGUAGE SQL "
                  "AS 'SELECT first'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp_column_function";
    SQLCHAR column_name_pattern[] = "first";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        column_name_pattern, SQL_NTS));

    char returned_name[128]{};
    SQLSMALLINT column_type = SQL_PARAM_TYPE_UNKNOWN;
    SQLSMALLINT data_type = 0;
    SQLINTEGER ordinal_position = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, returned_name, sizeof(returned_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &column_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 6, SQL_C_SSHORT, &data_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 18, SQL_C_SLONG, &ordinal_position, 0, nullptr));
    EXPECT_STREQ("first", returned_name);
    EXPECT_EQ(SQL_PARAM_INPUT, column_type);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(1, ordinal_position);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ProcedureColumnModesAndPatternsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_param_a_b("
                  "IN input_arg integer, INOUT inout_arg text, "
                  "OUT output_arg bigint) LANGUAGE SQL "
                  "AS 'SELECT inout_arg, input_arg::bigint'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_param_axb(input_arg integer) "
                  "RETURNS integer LANGUAGE SQL AS 'SELECT input_arg'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_unnamed(integer) "
                  "RETURNS integer LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));

    SQLCHAR wildcard_procedure[] = "odbcpp_param_a_b";
    SQLCHAR all_columns[] = "%";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, wildcard_procedure, SQL_NTS,
        all_columns, SQL_NTS));
    int wildcard_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++wildcard_rows;
    EXPECT_EQ(5, wildcard_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR escaped_procedure[] = "odbcpp\\_param\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, escaped_procedure, SQL_NTS,
        all_columns, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, escaped_procedure, SQL_NTS,
        all_columns, SQL_NTS));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    const std::array<const char*, 3> expected_names{
        "input_arg", "inout_arg", "output_arg"};
    const std::array<SQLINTEGER, 3> expected_modes{
        SQL_PARAM_INPUT, SQL_PARAM_INPUT_OUTPUT, SQL_PARAM_OUTPUT};
    const std::array<SQLINTEGER, 3> expected_types{
        SQL_INTEGER, SQL_LONGVARCHAR, SQL_BIGINT};
    std::optional<std::string> catalog;
    std::optional<std::string> schema;
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        if (!catalog) catalog = text_cell(hstmt, 1);
        if (!schema) schema = text_cell(hstmt, 2);
        EXPECT_EQ(std::optional<std::string>(expected_names[i]),
                  text_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_modes[i]),
                  integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_types[i]),
                  integer_cell(hstmt, 6));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NULLABLE_UNKNOWN),
                  integer_cell(hstmt, 12));
        EXPECT_EQ(std::optional<SQLINTEGER>(static_cast<SQLINTEGER>(i + 1)),
                  integer_cell(hstmt, 18));
        EXPECT_EQ(std::optional<std::string>(""), text_cell(hstmt, 19));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR unnamed_procedure[] = "odbcpp\\_unnamed";
    SQLCHAR empty[] = "";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, unnamed_procedure, SQL_NTS,
        empty, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>(""), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PARAM_INPUT),
              integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 18));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>(""), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_RETURN_VALUE),
              integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 18));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR percent[] = "%";
    auto expect_no_procedure_columns = [&](SQLCHAR* requested_catalog,
                                           SQLCHAR* requested_schema,
                                           SQLCHAR* requested_procedure,
                                           SQLCHAR* requested_column) {
        ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
            hstmt, requested_catalog, requested_catalog ? SQL_NTS : 0,
            requested_schema, requested_schema ? SQL_NTS : 0,
            requested_procedure, requested_procedure ? SQL_NTS : 0,
            requested_column, requested_column ? SQL_NTS : 0));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_procedure_columns(empty, nullptr, escaped_procedure,
                                all_columns);
    expect_no_procedure_columns(percent, nullptr, escaped_procedure,
                                all_columns);
    expect_no_procedure_columns(nullptr, empty, escaped_procedure,
                                all_columns);
    expect_no_procedure_columns(nullptr, nullptr, empty, all_columns);
    expect_no_procedure_columns(nullptr, nullptr, escaped_procedure, empty);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_procedure = rs::odbc::utf8_to_wide("odbcpp\\_param\\_a\\_b");
    auto wide_column = rs::odbc::utf8_to_wide("input%");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_procedure.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumnsW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_procedure->data(),
        static_cast<SQLSMALLINT>(wide_procedure->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("input_arg"), text_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, ReportsBestRowIdentifier) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_special_column_test("
                  "id integer PRIMARY KEY, value text)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_special_column_test";
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));

    SQLSMALLINT scope = -1;
    char column_name[64]{};
    SQLSMALLINT data_type = 0;
    SQLSMALLINT pseudo_column = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SSHORT, &scope, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_SSHORT, &data_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_SSHORT, &pseudo_column, 0, nullptr));
    EXPECT_EQ(SQL_SCOPE_CURROW, scope);
    EXPECT_STREQ("id", column_name);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(SQL_PC_NOT_PSEUDO, pseudo_column);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, SpecialColumnScopeAndArgumentsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp.special_'_table\"("
                  "second_key bigint, first_key integer, value text, "
                  "PRIMARY KEY(first_key, second_key))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_special_shorter_unique "
                  "ON \"odbcpp.special_'_table\" (value)",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp.special_'_table";
    SQLCHAR column_pattern[] = "first_key";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        column_pattern, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    const auto catalog = text_cell(hstmt, 1);
    const auto schema = text_cell(hstmt, 2);
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
    EXPECT_EQ(SQL_ERROR, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    const std::array<const char*, 2> expected_names{"first_key", "second_key"};
    const std::array<SQLINTEGER, 2> expected_types{SQL_INTEGER, SQL_BIGINT};
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_SCOPE_CURROW),
                  integer_cell(hstmt, 1));
        EXPECT_EQ(std::optional<std::string>(expected_names[i]),
                  text_cell(hstmt, 2));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_types[i]),
                  integer_cell(hstmt, 3));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PC_NOT_PSEUDO),
                  integer_cell(hstmt, 8));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    constexpr std::array<SQLUSMALLINT, 2> broader_scopes{
        static_cast<SQLUSMALLINT>(SQL_SCOPE_TRANSACTION),
        static_cast<SQLUSMALLINT>(SQL_SCOPE_SESSION)};
    for (const auto scope : broader_scopes) {
        ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
            hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
            table_name, SQL_NTS, scope, SQL_NULLABLE));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_ROWVER, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR wildcard[] = "%";
    auto expect_no_special_columns = [&](SQLCHAR* requested_catalog,
                                         SQLCHAR* requested_schema,
                                         SQLCHAR* requested_table) {
        ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
            hstmt, SQL_BEST_ROWID,
            requested_catalog, requested_catalog ? SQL_NTS : 0,
            requested_schema, requested_schema ? SQL_NTS : 0,
            requested_table, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_special_columns(empty, nullptr, table_name);
    expect_no_special_columns(nullptr, empty, table_name);
    expect_no_special_columns(nullptr, nullptr, empty);
    expect_no_special_columns(wildcard, nullptr, table_name);
    expect_no_special_columns(nullptr, wildcard, table_name);
    expect_no_special_columns(nullptr, nullptr, wildcard);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_table = rs::odbc::utf8_to_wide("odbcpp.special_'_table");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumnsW(
        hstmt, SQL_BEST_ROWID,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        SQL_SCOPE_CURROW, SQL_NULLABLE));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 2));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, SpecialColumnsFallsBackToUsableUniqueIndex) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_unique_rowid_test("
                  "nullable_code integer, stable_code bigint NOT NULL, "
                  "included_value text, spare integer NOT NULL)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_rowid_expression "
                  "ON odbcpp_unique_rowid_test ((lower(included_value)))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_rowid_partial "
                  "ON odbcpp_unique_rowid_test (spare) WHERE spare > 0",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_rowid_nullable "
                  "ON odbcpp_unique_rowid_test (nullable_code)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_rowid_nonnull "
                  "ON odbcpp_unique_rowid_test (stable_code) "
                  "INCLUDE (included_value)",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp_unique_rowid_test";
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("nullable_code"),
              text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 3));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("stable_code"),
              text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_BIGINT), integer_cell(hstmt, 3));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_nullable_rowid_test("
                  "only_key integer UNIQUE)",
        SQL_NTS));
    SQLCHAR nullable_table[] = "odbcpp_nullable_rowid_test";
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        nullable_table, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, LimitsRowsAndCompletesResultSequence) {
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_MAX_ROWS,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT value FROM generate_series(1, 3) value",
        SQL_NTS));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));

    SQLSMALLINT column_count = -1;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
}

TEST_F(MetadataIntegrationTest, FetchScrollSupportsNextRows) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT value FROM generate_series(1, 2) value",
        SQL_NTS));
    EXPECT_EQ(SQL_SUCCESS, SQLFetchScroll(hstmt, SQL_FETCH_NEXT, 0));
    EXPECT_EQ(SQL_SUCCESS, SQLFetchScroll(hstmt, SQL_FETCH_NEXT, 0));
    EXPECT_EQ(SQL_NO_DATA, SQLFetchScroll(hstmt, SQL_FETCH_NEXT, 0));
    EXPECT_EQ(SQL_ERROR, SQLFetchScroll(hstmt, SQL_FETCH_FIRST, 0));
}

TEST_F(MetadataIntegrationTest, TraversesMultiplePostgreSQLResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_more_results(value int)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_more_results VALUES (1)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 11 AS first_value; "
                  "UPDATE odbcpp_more_results SET value = 2; "
                  "SELECT 'done'::text AS final_value",
        SQL_NTS));

    SQLSMALLINT column_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER first_value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &first_value, 0, nullptr));
    EXPECT_EQ(11, first_value);

    ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    SQLLEN affected_rows = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &affected_rows));
    EXPECT_EQ(1, affected_rows);

    ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char final_value[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, final_value, sizeof(final_value), nullptr));
    EXPECT_STREQ("done", final_value);
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));
}

TEST_F(MetadataIntegrationTest, LaterBatchErrorSurfacesThroughMoreResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 11 AS first_value; SELECT 1 / 0", SQL_NTS));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(11, integer_cell(hstmt, 1));

    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("22012", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 / 0", SQL_NTS));
    EXPECT_EQ("22012", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 1 / 0", SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("22012", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 22 AS recovered", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(22, integer_cell(hstmt, 1));
}

TEST_F(MetadataIntegrationTest, InvalidTextCastUsesOdbcSqlstate) {
    constexpr auto invalid_cast = "SELECT 'not-a-number'::integer";

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)invalid_cast, SQL_NTS));
    EXPECT_EQ("22018", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)invalid_cast, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("22018", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 7; SELECT 'not-a-number'::integer",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(7, integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("22018", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 AS recovered", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(1, integer_cell(hstmt, 1));
}

TEST_F(MetadataIntegrationTest, IntegrityViolationsUseOdbcSqlstate) {
    // NOT NULL is enforced by both profiles; Redshift primary keys are informational.
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_integrity_diag("
                         "id integer NOT NULL PRIMARY KEY)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_integrity_diag VALUES (1)",
        SQL_NTS));

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_integrity_diag VALUES (NULL)",
        SQL_NTS));
    EXPECT_EQ("23000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_integrity_diag VALUES (NULL)",
        SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("23000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 7; "
                         "INSERT INTO odbcpp_integrity_diag VALUES (NULL)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(7, integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("23000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT count(*) FROM odbcpp_integrity_diag",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(1, integer_cell(hstmt, 1));
}

TEST_F(MetadataIntegrationTest, MissingObjectsUseOdbcSqlstates) {
    constexpr auto missing_table =
        "SELECT * FROM odbcpp_missing_diag_table";
    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)missing_table, SQL_NTS));
    EXPECT_EQ("42S02", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)missing_table, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("42S02", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 7; "
                         "SELECT * FROM odbcpp_missing_diag_table",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(7, integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("42S02", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    constexpr auto missing_column =
        "SELECT absent_column FROM (SELECT 1 AS existing_column) AS t";
    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)missing_column, SQL_NTS));
    EXPECT_EQ("42S22", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)missing_column, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("42S22", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 AS recovered", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(1, integer_cell(hstmt, 1));
}

TEST_F(MetadataIntegrationTest, DuplicateObjectsUseOdbcSqlstates) {
    constexpr auto create_table =
        "CREATE TEMP TABLE odbcpp_duplicate_diag(id integer)";
    constexpr auto add_column =
        "ALTER TABLE odbcpp_duplicate_diag ADD COLUMN id integer";
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)create_table, SQL_NTS));

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)create_table, SQL_NTS));
    EXPECT_EQ("42S01", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)create_table, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("42S01", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)add_column, SQL_NTS));
    EXPECT_EQ("42S21", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)add_column, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("42S21", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 7; "
                         "ALTER TABLE odbcpp_duplicate_diag "
                         "ADD COLUMN id integer", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(7, integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("42S21", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 8; "
                         "CREATE TEMP TABLE odbcpp_duplicate_diag(id integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(8, integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT count(*) FROM odbcpp_duplicate_diag",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0, integer_cell(hstmt, 1));
}

TEST_F(MetadataIntegrationTest, DuplicateIndexUsesIndexSqlstate) {
    constexpr auto create_index =
        "CREATE INDEX odbcpp_duplicate_index_diag_idx "
        "ON odbcpp_duplicate_index_diag(id)";
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE "
                         "odbcpp_duplicate_index_diag(id integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)create_index, SQL_NTS));

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)create_index, SQL_NTS));
    EXPECT_EQ("42S11", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)create_index, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("42S11", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 7; "
                         "CREATE INDEX odbcpp_duplicate_index_diag_idx "
                         "ON odbcpp_duplicate_index_diag(id)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(7, integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT count(*) FROM odbcpp_duplicate_index_diag",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(0, integer_cell(hstmt, 1));
}

TEST_F(MetadataIntegrationTest, MissingIndexUsesIndexSqlstate) {
    constexpr auto drop_index =
        "DROP INDEX odbcpp_missing_index_diag_9b41";

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)drop_index, SQL_NTS));
    EXPECT_EQ("42S12", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)drop_index, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("42S12", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 7; "
                         "DROP INDEX odbcpp_missing_index_diag_9b41",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(7, integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("42000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 AS recovered", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(1, integer_cell(hstmt, 1));
}

TEST_F(MetadataIntegrationTest, MissingSchemaUsesOdbcSqlstate) {
    constexpr auto create_table =
        "CREATE TABLE odbcpp_missing_schema_diag_6b31.example(id integer)";

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)create_table, SQL_NTS));
    EXPECT_EQ("3F000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)create_table, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ("3F000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 7; "
                         "CREATE TABLE "
                         "odbcpp_missing_schema_diag_6b31.example(id integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(7, integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
    EXPECT_EQ("3F000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 AS recovered", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(1, integer_cell(hstmt, 1));
}

TEST_F(MetadataIntegrationTest, ClosingCursorDiscardsPendingResults) {
    constexpr auto batch =
        "SELECT 1 AS first_value; SELECT 2 AS second_value";

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)batch, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)batch, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_CLOSE));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));
}

TEST_F(MetadataIntegrationTest, ErrorCases) {
    // Execute query first
    SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS);
    ASSERT_EQ(SQL_SUCCESS, ret);
    
    // Test invalid column numbers
    SQLCHAR column_name[256];
    ret = SQLDescribeCol(hstmt, 0, column_name, sizeof(column_name), nullptr,
                        nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(SQL_ERROR, ret);
    
    ret = SQLDescribeCol(hstmt, 999, column_name, sizeof(column_name), nullptr,
                        nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(SQL_ERROR, ret);
}

TEST_F(MetadataIntegrationTest, NegativeTests) {
    // Test SQLNumResultCols without execution
    SQLSMALLINT num_cols;
    SQLRETURN ret = SQLNumResultCols(hstmt, &num_cols);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Verify diagnostic is set
    SQLCHAR sqlstate[6], message[256];
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("HY010", (char*)sqlstate);
    
    // Test SQLNumResultCols with null pointer
    ret = SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS);
    ASSERT_EQ(SQL_SUCCESS, ret);
    
    ret = SQLNumResultCols(hstmt, nullptr);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test SQLDescribeCol with invalid parameters
    ret = SQLDescribeCol(hstmt, 0, nullptr, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test SQLColAttribute with invalid column
    SQLLEN numeric_attr;
    ret = SQLColAttribute(hstmt, 999, SQL_DESC_TYPE, nullptr, 0, nullptr, &numeric_attr);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test SQLColAttribute with invalid field identifier
    ret = SQLColAttribute(hstmt, 1, 9999, nullptr, 0, nullptr, &numeric_attr);
    EXPECT_EQ(SQL_ERROR, ret);
}

TEST_F(MetadataIntegrationTest, InvalidHandleTests) {
    // Test with invalid statement handle
    SQLSMALLINT num_cols;
    SQLRETURN ret = SQLNumResultCols(nullptr, &num_cols);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
    
    // Test SQLDescribeCol with invalid handle
    SQLCHAR column_name[256];
    ret = SQLDescribeCol(nullptr, 1, column_name, sizeof(column_name), nullptr,
                        nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
    
    // Test SQLColAttribute with invalid handle
    SQLLEN numeric_attr;
    ret = SQLColAttribute(nullptr, 1, SQL_DESC_TYPE, nullptr, 0, nullptr, &numeric_attr);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
}

TEST_F(MetadataIntegrationTest, BackendScalarMetadataAgreesBeforeAndAfterExecution) {
    const char* postgres_sql = "SELECT NULL::numeric(12,3), NULL::numeric(8,-2), "
                    "NULL::varchar(17), NULL::time(3), NULL::timestamp(0), "
                    "NULL::uuid, NULL::int4[]";
    struct Expected { SQLSMALLINT type; SQLULEN size; SQLSMALLINT scale; };
    const std::array<Expected, 7> postgres_expected{{
        {SQL_NUMERIC, 12, 3}, {SQL_NUMERIC, 8, -2}, {SQL_VARCHAR, 17, 0},
        {SQL_TYPE_TIME, 12, 3}, {SQL_TYPE_TIMESTAMP, 19, 0},
        {SQL_VARCHAR, 36, 0}, {SQL_VARCHAR, 0, 0}}};
    // Redshift has no UUID/array or negative-scale numeric declaration. Keep
    // the PostgreSQL matrix intact and assert seven supported native families
    // with independent literal dimensions on Redshift, including the NULL cells.
    const char* redshift_sql = "SELECT NULL::numeric(12,3), NULL::numeric(8,2), "
        "NULL::varchar(17), NULL::time, NULL::timestamp, NULL::date, NULL::bigint";
    const std::array<Expected, 7> redshift_expected{{
        {SQL_NUMERIC, 12, 3}, {SQL_NUMERIC, 8, 2}, {SQL_VARCHAR, 17, 0},
        {SQL_TYPE_TIME, 15, 6}, {SQL_TYPE_TIMESTAMP, 26, 6},
        {SQL_TYPE_DATE, 10, 0}, {SQL_BIGINT, 19, 0}}};
    const auto& expected = redshift_profile() ? redshift_expected : postgres_expected;
    const auto* selected_sql = redshift_profile() ? redshift_sql : postgres_sql;
    auto* sql = reinterpret_cast<SQLCHAR*>(const_cast<char*>(selected_sql));
    for (const bool prepared : {false, true}) {
        SCOPED_TRACE(prepared);
        ASSERT_EQ(SQL_SUCCESS, prepared ? SQLPrepare(hstmt, sql, SQL_NTS)
                                       : SQLExecDirect(hstmt, sql, SQL_NTS));
        for (int phase = 0; phase < (prepared ? 2 : 1); ++phase) {
            if (phase == 1) {
                ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
            }
            for (std::size_t i = 0; i < expected.size(); ++i) {
                SCOPED_TRACE(i);
                SQLSMALLINT type = 0, scale = 0, nullable = 0;
                SQLULEN size = 999;
                ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(hstmt,
                    static_cast<SQLUSMALLINT>(i + 1), nullptr, 0, nullptr,
                    &type, &size, &scale, &nullable));
                EXPECT_EQ(expected[i].type, type);
                EXPECT_EQ(expected[i].size, size);
                EXPECT_EQ(expected[i].scale, scale);
                EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);
            }
        }
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (SQLUSMALLINT i = 1; i <= expected.size(); ++i) {
            char value = 'x';
            SQLLEN indicator = 0;
            EXPECT_EQ(SQL_SUCCESS, SQLGetData(hstmt, i, SQL_C_CHAR,
                                             &value, 1, &indicator));
            EXPECT_EQ(SQL_NULL_DATA, indicator);
            EXPECT_EQ('x', value);
        }
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
}

TEST_F(MetadataIntegrationTest, BackendDiagnosticFallbackAndRecoveryAcrossExecutionPaths) {
    for (const auto native_state : {"22012", "P0001"}) {
        SCOPED_TRACE(native_state);
        const std::string sql = "DO $$ BEGIN RAISE EXCEPTION 'backend contract error' "
            "USING ERRCODE = '" + std::string(native_state) + "'; END $$";
        const std::string expected = std::string(native_state) == "22012"
            ? "22012" : "42000";
        EXPECT_EQ(SQL_ERROR, SQLExecDirect(hstmt, (SQLCHAR*)sql.c_str(), SQL_NTS));
        EXPECT_EQ(expected, diagnostic_state(SQL_HANDLE_STMT, hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)sql.c_str(), SQL_NTS));
        EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
        EXPECT_EQ(expected, diagnostic_state(SQL_HANDLE_STMT, hstmt));
        const std::string batch = "SELECT 17; " + sql;
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)batch.c_str(), SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(17, integer_cell(hstmt, 1));
        EXPECT_EQ(SQL_ERROR, SQLMoreResults(hstmt));
        EXPECT_EQ(expected, diagnostic_state(SQL_HANDLE_STMT, hstmt));
        EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 19", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(19, integer_cell(hstmt, 1));
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_CLOSE));
    }
}

TEST_F(MetadataIntegrationTest, CompletionKindsDriveEachBatchResultDiagnostic) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt,
        (SQLCHAR*)"WITH item AS (SELECT 1) SELECT * FROM item; "
                   "SET application_name = 'odbcpp'; SELECT 2", SQL_NTS));
    const auto expect_kind = [&](SQLINTEGER expected, const char* name) {
        SQLINTEGER code = -1;
        SQLCHAR text[40]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
            SQL_DIAG_DYNAMIC_FUNCTION_CODE, &code, 0, nullptr));
        EXPECT_EQ(expected, code);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagField(SQL_HANDLE_STMT, hstmt, 0,
            SQL_DIAG_DYNAMIC_FUNCTION, text, sizeof(text), nullptr));
        EXPECT_STREQ(name, reinterpret_cast<char*>(text));
    };
    expect_kind(SQL_DIAG_SELECT_CURSOR, "SELECT CURSOR");
    ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(hstmt));
    expect_kind(SQL_DIAG_UNKNOWN_STATEMENT, "");
    ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(hstmt));
    expect_kind(SQL_DIAG_SELECT_CURSOR, "SELECT CURSOR");
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));
}

TEST_F(MetadataIntegrationTest, PreparedMetadataRefreshesAfterTimeoutReconnect) {
    if (redshift_profile()) {
        SQLHSTMT other{};
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &other));
        struct Cleanup { SQLHSTMT value; ~Cleanup() { SQLFreeHandle(SQL_HANDLE_STMT, value); } } cleanup{other};
        SharedTimeoutBlocker blocker(fixture_end_);
        ASSERT_TRUE(blocker.open(henv, other));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other,
            (SQLCHAR*)"CREATE TEMP TABLE metadata_session_scope (value integer)", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
            (SQLCHAR*)"SELECT * FROM metadata_session_scope", SQL_NTS));
        SQLSMALLINT columns{};
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
        EXPECT_EQ(1, columns);
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
        EXPECT_EQ(1, columns);
        ASSERT_TRUE(blocker.lock());
        ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(other, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)1, 0));
        const auto sql = blocker.waiter_sql();
        const auto started = std::chrono::steady_clock::now();
        const auto result = SQLExecDirect(other,
            reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto state = diagnostic_state(SQL_HANDLE_STMT, other);
        const bool expected = result == SQL_ERROR && state == "HYT00" &&
            elapsed < std::chrono::seconds(3);
        blocker.waiter_result(expected);
        EXPECT_EQ(SQL_ERROR, result);
        EXPECT_EQ("HYT00", state);
        EXPECT_LT(elapsed, std::chrono::seconds(3));
        ASSERT_TRUE(blocker.finish());
        ASSERT_TRUE(expected);
        // Internal timeout close must preserve BOTH existing child handles.
        ASSERT_EQ(SQL_SUCCESS, SQLConnect(hdbc,
            odbcpp::test::configured_connection_string_data(), SQL_NTS, nullptr, 0, nullptr, 0));
        ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(other, SQL_ATTR_QUERY_TIMEOUT, nullptr, 0));
        ASSERT_TRUE(blocker.reconnected(other));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other,
            (SQLCHAR*)"CREATE TEMP TABLE metadata_session_scope (value text, extra boolean)", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
        EXPECT_EQ(2, columns);
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
        EXPECT_EQ(2, columns);
        return;
    }
    SQLHSTMT other{};
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &other));
    struct Cleanup { SQLHSTMT value; ~Cleanup() { SQLFreeHandle(SQL_HANDLE_STMT, value); } } cleanup{other};
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other,
        (SQLCHAR*)"CREATE TEMP TABLE metadata_session_scope (value integer)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
        (SQLCHAR*)"SELECT * FROM metadata_session_scope", SQL_NTS));
    SQLSMALLINT columns{};
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(1, columns);
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(1, columns);
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(other, SQL_ATTR_QUERY_TIMEOUT, (SQLPOINTER)1, 0));
    ASSERT_EQ(SQL_ERROR, SQLExecDirect(other, (SQLCHAR*)"SELECT pg_sleep(2)", SQL_NTS));
    EXPECT_EQ("HYT00", diagnostic_state(SQL_HANDLE_STMT, other));
    // Internal timeout close preserves child handles, unlike public SQLDisconnect.
    ASSERT_EQ(SQL_SUCCESS, SQLConnect(hdbc,
        odbcpp::test::configured_connection_string_data(), SQL_NTS, nullptr, 0, nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(other, SQL_ATTR_QUERY_TIMEOUT, nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other,
        (SQLCHAR*)"CREATE TEMP TABLE metadata_session_scope (value text, extra boolean)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(2, columns);
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(2, columns);
}

TEST_F(MetadataIntegrationTest, PreparedMetadataRefreshesAfterLocalSchemaMutationAndReset) {
    SQLHSTMT other{};
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &other));
    struct Cleanup { SQLHSTMT value; ~Cleanup() { SQLFreeHandle(SQL_HANDLE_STMT, value); } } cleanup{other};
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other,
        (SQLCHAR*)"CREATE TEMP TABLE metadata_epoch_scope (value integer)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT * FROM metadata_epoch_scope", SQL_NTS));
    SQLSMALLINT columns{};
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(1, columns);
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other,
        (SQLCHAR*)"ALTER TABLE metadata_epoch_scope ADD COLUMN extra boolean", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(2, columns);
    auto view = rs::odbc::detail::ODBCBackendTestAccess::view(
        rs::odbc::HandleRegistry::instance().get_handle_as<rs::odbc::ODBCConnection>(hdbc));
    ASSERT_TRUE(view.check_health(std::min(fixture_end_, rs::util::make_deadline(std::chrono::seconds(3)))));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(2, columns);
    if (redshift_profile()) {
        // The compiled Redshift provider deliberately has no same-session reset
        // facet. Refusal must invalidate cached descriptions and retire the lease.
        ASSERT_FALSE(view.has_reset());
        const auto reset = view.reset_session(
            std::min(fixture_end_, rs::util::make_deadline(std::chrono::seconds(3))));
        ASSERT_FALSE(reset);
        EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature), reset.error());
        EXPECT_EQ(rs::core::database::SessionState::Disconnected, reset.session_snapshot().state);
        EXPECT_EQ(rs::core::database::SessionDisposition::Retire, reset.session_snapshot().disposition);
        EXPECT_FALSE(view.is_connected());
        columns = 77;
        EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &columns));
        EXPECT_EQ(77, columns);
        EXPECT_EQ("08S01", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        // Public disconnect invalidates child handles. Release both explicitly,
        // then allocate fresh handles and reprepare in the new server session.
        ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, hstmt));
        hstmt = SQL_NULL_HSTMT;
        ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, other));
        other = SQL_NULL_HSTMT;
        cleanup.value = SQL_NULL_HSTMT;
        ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(hdbc));
        ASSERT_EQ(SQL_SUCCESS, SQLConnect(hdbc,
            odbcpp::test::configured_connection_string_data(), SQL_NTS, nullptr, 0, nullptr, 0));
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &other));
        cleanup.value = other;
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other,
            (SQLCHAR*)"CREATE TEMP TABLE metadata_epoch_scope (value text, extra boolean, third integer)", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt,
            (SQLCHAR*)"SELECT * FROM metadata_epoch_scope", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
        EXPECT_EQ(3, columns);
        ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
        EXPECT_EQ(3, columns);
        return;
    }
    ASSERT_TRUE(view.reset_session(std::min(fixture_end_, rs::util::make_deadline(std::chrono::seconds(3)))));
    columns = 77;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(77, columns);
    EXPECT_EQ("42S02", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(other,
        (SQLCHAR*)"CREATE TEMP TABLE metadata_epoch_scope (value text, extra boolean, third integer)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns)); EXPECT_EQ(3, columns);
}
