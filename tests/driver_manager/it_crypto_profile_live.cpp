#include <gtest/gtest.h>

#include <sql.h>
#include <sqlext.h>

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <string>

namespace {

std::string required_connection(const char* variable) {
  const char* value = std::getenv(variable);
  if (!value || !*value) {
    throw std::runtime_error(std::string(variable) + " is required");
  }
  return value;
}

class Handles {
 public:
  Handles() {
    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_) !=
            SQL_SUCCESS ||
        SQLSetEnvAttr(environment_, SQL_ATTR_ODBC_VERSION,
                      reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) !=
            SQL_SUCCESS ||
        SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_) !=
            SQL_SUCCESS) {
      throw std::runtime_error("failed to allocate ODBC handles");
    }
  }

  ~Handles() {
    if (connection_ != SQL_NULL_HDBC) {
      SQLDisconnect(connection_);
      SQLFreeHandle(SQL_HANDLE_DBC, connection_);
    }
    if (environment_ != SQL_NULL_HENV) {
      SQLFreeHandle(SQL_HANDLE_ENV, environment_);
    }
  }

  SQLHDBC connection() const { return connection_; }

 private:
  SQLHENV environment_{SQL_NULL_HENV};
  SQLHDBC connection_{SQL_NULL_HDBC};
};

SQLRETURN connect(SQLHDBC connection, const std::string& connection_string) {
  SQLCHAR output[1024]{};
  SQLSMALLINT output_length = 0;
  return SQLDriverConnect(
      connection, nullptr,
      reinterpret_cast<SQLCHAR*>(const_cast<char*>(connection_string.data())),
      SQL_NTS, output, static_cast<SQLSMALLINT>(sizeof(output)), &output_length,
      SQL_DRIVER_NOPROMPT);
}

std::string diagnostics(SQLHDBC connection) {
  std::string output;
  for (SQLSMALLINT record = 1;; ++record) {
    SQLCHAR state[6]{};
    SQLCHAR message[1024]{};
    SQLINTEGER native_error = 0;
    SQLSMALLINT message_length = 0;
    const auto result = SQLGetDiagRec(
        SQL_HANDLE_DBC, connection, record, state, &native_error, message,
        static_cast<SQLSMALLINT>(sizeof(message)), &message_length);
    if (result == SQL_NO_DATA) break;
    if (!SQL_SUCCEEDED(result)) break;
    output.append(reinterpret_cast<const char*>(state));
    output.push_back(' ');
    const auto available = std::min<std::size_t>(
        static_cast<std::size_t>(std::max<SQLSMALLINT>(message_length, 0)),
        sizeof(message) - 1);
    output.append(reinterpret_cast<const char*>(message),
                  available);
    output.push_back('\n');
  }
  return output;
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

TEST(CryptoProfileLiveTest, CompletesVerifiedTlsScramQuery) {
  Handles handles;
  ASSERT_TRUE(SQL_SUCCEEDED(connect(
      handles.connection(),
      required_connection("ODBCPP_CRYPTO_PROFILE_TEST_CONNECTION"))));

  SQLHSTMT statement = SQL_NULL_HSTMT;
  ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_STMT, handles.connection(), &statement),
            SQL_SUCCESS);
  SQLCHAR query[] = "SELECT 1";
  ASSERT_TRUE(SQL_SUCCEEDED(SQLExecDirect(statement, query, SQL_NTS)));
  ASSERT_TRUE(SQL_SUCCEEDED(SQLFetch(statement)));
  SQLINTEGER value = 0;
  ASSERT_TRUE(SQL_SUCCEEDED(SQLGetData(statement, 1, SQL_C_SLONG, &value,
                                       sizeof(value), nullptr)));
  EXPECT_EQ(value, 1);

  ASSERT_TRUE(SQL_SUCCEEDED(SQLCloseCursor(statement)));
  SQLCHAR tls_query[] =
      "SELECT ssl FROM pg_stat_ssl WHERE pid = pg_backend_pid()";
  ASSERT_TRUE(SQL_SUCCEEDED(SQLExecDirect(statement, tls_query, SQL_NTS)));
  ASSERT_TRUE(SQL_SUCCEEDED(SQLFetch(statement)));
  SQLCHAR tls_active[8]{};
  ASSERT_TRUE(SQL_SUCCEEDED(SQLGetData(statement, 1, SQL_C_CHAR, tls_active,
                                       sizeof(tls_active), nullptr)));
  EXPECT_STREQ(reinterpret_cast<const char*>(tls_active), "t");
  EXPECT_EQ(SQLFreeHandle(SQL_HANDLE_STMT, statement), SQL_SUCCESS);
}

TEST(CryptoProfileLiveTest, RejectsUntrustedCertificate) {
  Handles handles;
  ASSERT_EQ(connect(
                handles.connection(),
                required_connection("ODBCPP_CRYPTO_PROFILE_WRONG_CA_CONNECTION")),
            SQL_ERROR);
  const auto records = diagnostics(handles.connection());
  const auto normalized = lowercase(records);
  EXPECT_NE(normalized.find("certificate verify failed"), std::string::npos)
      << records;
  EXPECT_TRUE(normalized.find("ssl_connect") != std::string::npos ||
              normalized.find("tls") != std::string::npos)
      << records;
}

TEST(CryptoProfileLiveTest, RejectsHostnameMismatch) {
  Handles handles;
  ASSERT_EQ(connect(
                handles.connection(),
                required_connection("ODBCPP_CRYPTO_PROFILE_WRONG_HOST_CONNECTION")),
            SQL_ERROR);
  const auto records = diagnostics(handles.connection());
  EXPECT_NE(records.find("TLS hostname verification failed"),
            std::string::npos) << records;
}

}  // namespace
