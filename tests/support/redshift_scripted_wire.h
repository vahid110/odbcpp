#pragma once
// Literal Redshift protocol frames for in-process unit tests: a real
// Redshift-profile PostgreSQL-family session over a local scripted transport,
// driven through the ODBC API. No database service is used.
#include <gtest/gtest.h>

#include "core/database/postgres/pg_backend_provider.h"
#include "core/database/postgres/pg_database_connection.h"
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "odbc/unicode.h"
#include "odbcpp/transport/i_transport.h"
#include "odbcpp/util/hex.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rs::tests::redshift_wire {
using namespace rs::core::database;

inline std::string be32(std::uint32_t value) {
  return {static_cast<char>(value >> 24), static_cast<char>(value >> 16),
          static_cast<char>(value >> 8), static_cast<char>(value)};
}
inline std::string be16(std::uint16_t value) {
  return {static_cast<char>(value >> 8), static_cast<char>(value)};
}
inline std::string frame(char tag, std::string_view body) {
  return std::string(1, tag) + be32(static_cast<std::uint32_t>(body.size() + 4)) +
      std::string(body);
}
inline std::string row_description(
    const std::vector<std::pair<std::string, std::uint32_t>>& columns) {
  std::string body = be16(static_cast<std::uint16_t>(columns.size()));
  for (const auto& [name, oid] : columns) {
    body += name; body.push_back('\0');
    body += be32(0) + be16(0) + be32(oid) + be16(0xffff) + be32(0xffffffff) + be16(0);
  }
  return frame('T', body);
}
inline std::string data_row(const std::vector<std::optional<std::string>>& cells) {
  std::string body = be16(static_cast<std::uint16_t>(cells.size()));
  for (const auto& cell : cells) {
    if (!cell) {
      body += be32(0xffffffff);
      continue;
    }
    body += be32(static_cast<std::uint32_t>(cell->size())) + *cell;
  }
  return frame('D', body);
}
inline std::string complete(std::string_view tag) {
  std::string body(tag); body.push_back('\0');
  return frame('C', body);
}

// Shared script: SQL text -> backend frames before ReadyForQuery.
struct WireScript {
  std::map<std::string, std::string> responses;
  std::vector<std::pair<std::string, std::string>> prefix_responses;
  std::vector<std::string> received_sql;
  std::string startup; // The StartupMessage bytes as sent.
};

class RedshiftWire final : public rs::core::transport::ITransport {
 public:
  explicit RedshiftWire(std::shared_ptr<WireScript> script) : script_(std::move(script)) {}
  rs::util::Result<void> connect(std::string_view, uint16_t, rs::util::Deadline) override {
    return {};
  }
  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> buffer, rs::util::Deadline) override {
    pending_.append(reinterpret_cast<const char*>(buffer.data()), buffer.size());
    consume();
    return rs::core::transport::IOResult{buffer.size(), false};
  }
  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> buffer, rs::util::Deadline) override {
    if (output_offset_ == output_.size()) {
      return {rs::util::DbErrorCode::NetworkError, "scripted wire has no response"};
    }
    const auto count = std::min(buffer.size(), output_.size() - output_offset_);
    std::memcpy(buffer.data(), output_.data() + output_offset_, count);
    output_offset_ += count;
    return rs::core::transport::IOResult{count, false};
  }
  void close() noexcept override {}

 private:
  static std::uint32_t read32(std::string_view bytes) {
    return (std::uint32_t{static_cast<unsigned char>(bytes[0])} << 24) |
        (std::uint32_t{static_cast<unsigned char>(bytes[1])} << 16) |
        (std::uint32_t{static_cast<unsigned char>(bytes[2])} << 8) |
        std::uint32_t{static_cast<unsigned char>(bytes[3])};
  }
  std::string respond(const std::string& sql) {
    script_->received_sql.push_back(sql);
    const auto found = script_->responses.find(sql);
    if (found != script_->responses.end()) return found->second;
    for (const auto& [prefix, response] : script_->prefix_responses) {
      if (sql.starts_with(prefix)) return response;
    }
    static constexpr char error[] = "SERROR\0C42601\0Munscripted statement\0";
    return frame('E', std::string_view(error, sizeof(error)));
  }
  void consume() {
    for (;;) {
      if (!started_) {
        if (pending_.size() < 4) return;
        const auto length = read32(pending_);
        if (pending_.size() < length) return;
        script_->startup = pending_.substr(0, length);
        pending_.erase(0, length);
        started_ = true;
        output_ += frame('R', be32(0));
        output_ += frame('S', std::string_view("server_version\0" "8.0.2\0", 21));
        output_ += frame('K', std::string(8, '\0'));
        output_ += frame('Z', "I");
        continue;
      }
      if (pending_.size() < 5) return;
      const auto length = read32(std::string_view(pending_).substr(1));
      if (pending_.size() < length + 1) return;
      const char tag = pending_[0];
      const std::string body = pending_.substr(5, length - 4);
      pending_.erase(0, length + 1);
      if (tag == 'Q') {
        output_ += respond(body.substr(0, body.find('\0')));
        output_ += frame('Z', "I");
      } else if (tag == 'P') {
        const auto name_end = body.find('\0');
        parsed_sql_ = body.substr(name_end + 1, body.find('\0', name_end + 1) - name_end - 1);
      } else if (tag == 'S') {
        output_ += respond(parsed_sql_);
        output_ += frame('Z', "I");
      } else if (tag == 'X') {
        return;
      }
    }
  }
  std::shared_ptr<WireScript> script_;
  std::string pending_, output_, parsed_sql_;
  std::size_t output_offset_{0};
  bool started_{false};
};

// The product Redshift provider, except that each session uses the local wire.
class ScriptedRedshiftProvider final : public IBackendProvider {
 public:
  explicit ScriptedRedshiftProvider(std::shared_ptr<WireScript> script)
      : script_(std::move(script)),
        product_(BackendIdentity{"redshift", "Amazon Redshift", "ODBCPP Redshift"},
                 BackendConnectionDefaults{"localhost", 5439, std::nullopt, true},
                 std::nullopt, postgres::PgCatalogProfile::Redshift) {}
  const BackendIdentity& identity() const noexcept override { return product_.identity(); }
  const BackendConnectionDefaults& connection_defaults() const noexcept override {
    return product_.connection_defaults();
  }
  const ISqlDialect& sql_dialect() const noexcept override { return product_.sql_dialect(); }
  BackendCapabilities capabilities() const noexcept override { return product_.capabilities(); }
  std::optional<std::string> normalize_error_sqlstate(
      std::string_view state, ErrorContext context) const override {
    return product_.normalize_error_sqlstate(state, context);
  }
  std::span<const TypeDefinition> type_catalog(std::string_view version = {}) const noexcept override {
    return product_.type_catalog(version);
  }
  std::span<const TypeDefinition> result_type_catalog(
      std::string_view version = {}) const noexcept override {
    return product_.result_type_catalog(version);
  }
  TransactionCapabilities transaction_capabilities() const noexcept override {
    return product_.transaction_capabilities();
  }
  rs::util::Result<ConnectionSettings> resolve_connection_options(
      ConnectionOptions options) const override {
    return product_.resolve_connection_options(std::move(options));
  }
  std::unique_ptr<IDatabaseConnection> create_session(
      std::unique_ptr<rs::core::transport::ITransport>) const override {
    return std::make_unique<postgres::PgDatabaseConnection>(
        std::make_unique<RedshiftWire>(script_), std::nullopt,
        postgres::PgCatalogProfile::Redshift);
  }

 private:
  std::shared_ptr<WireScript> script_;
  postgres::PgBackendProvider product_;
};

// Real Redshift-profile session over the scripted wire, through the ODBC API.
class RedshiftScriptedSessionTest : public ::testing::Test {
 protected:
  std::shared_ptr<WireScript> script = std::make_shared<WireScript>();
  SQLHENV env{}; SQLHDBC dbc{}; SQLHSTMT stmt{};

  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &env));
    ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
        reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    auto connection = std::make_unique<rs::odbc::ODBCConnection>(
        nullptr, std::make_shared<ScriptedRedshiftProvider>(script));
    dbc = reinterpret_cast<SQLHDBC>(connection.get());
    rs::odbc::HandleRegistry::instance().register_handle(dbc, std::move(connection), env);
    SQLCHAR input[] = "SERVER=fake;DATABASE=dev;UID=test;SSL=0";
    ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr, input, SQL_NTS, nullptr, 0,
                                            nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  }
  void TearDown() override {
    if (stmt) SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (dbc) {
      SQLDisconnect(dbc);
      SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    }
    if (env) SQLFreeHandle(SQL_HANDLE_ENV, env);
  }
  SQLRETURN execute(const std::string& sql) {
    return SQLExecDirect(stmt, reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), SQL_NTS);
  }
  std::string state() {
    SQLCHAR text[6]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, text, nullptr, nullptr, 0, nullptr));
    return reinterpret_cast<char*>(text);
  }
  std::string column_text(SQLUSMALLINT column, SQLUSMALLINT field) {
    char text[64]{};
    SQLSMALLINT length = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLColAttribute(stmt, column, field, text, sizeof(text), &length, nullptr));
    return std::string(text, length < 0 ? 0 : static_cast<std::size_t>(length));
  }
  SQLLEN column_number(SQLUSMALLINT column, SQLUSMALLINT field) {
    SQLLEN value = -99;
    EXPECT_EQ(SQL_SUCCESS, SQLColAttribute(stmt, column, field, nullptr, 0, nullptr, &value));
    return value;
  }
  // Complete SQLGetData in fixed chunks; returns the concatenated bytes.
  std::string chunked(SQLUSMALLINT column, SQLSMALLINT target, std::size_t chunk) {
    std::string result;
    std::vector<unsigned char> buffer(chunk);
    const std::size_t terminator = target == SQL_C_CHAR ? 1 : target == SQL_C_WCHAR ? sizeof(SQLWCHAR) : 0;
    for (int guard = 0; guard < 100000; ++guard) {
      SQLLEN length = -7;
      const auto rc = SQLGetData(stmt, column, target, buffer.data(), static_cast<SQLLEN>(buffer.size()), &length);
      if (rc == SQL_NO_DATA) {
        return result;
      }
      EXPECT_TRUE(rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) << rc;
      if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
        return result;
      }
      if (rc == SQL_SUCCESS_WITH_INFO) {
        EXPECT_EQ("01004", state());
        EXPECT_GE(length, static_cast<SQLLEN>(buffer.size() - terminator));
        result.append(reinterpret_cast<const char*>(buffer.data()), buffer.size() - terminator);
        continue;
      }
      EXPECT_GE(length, 0);
      result.append(reinterpret_cast<const char*>(buffer.data()), static_cast<std::size_t>(length));
    }
    ADD_FAILURE() << "SQLGetData did not finish";
    return result;
  }
};

} // namespace rs::tests::redshift_wire
