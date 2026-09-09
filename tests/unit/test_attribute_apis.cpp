#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "core/util/deadline.h"

#include <cstdint>
#include <string>

namespace {

class AttributeApisTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &environment_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment_, &connection_));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_STMT, connection_, &statement_));
  }

  void TearDown() override {
    if (statement_) SQLFreeHandle(SQL_HANDLE_STMT, statement_);
    if (connection_) SQLFreeHandle(SQL_HANDLE_DBC, connection_);
    if (environment_) SQLFreeHandle(SQL_HANDLE_ENV, environment_);
  }

  static SQLPOINTER integer_value(SQLULEN value) {
    return reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(value));
  }

  static std::string diagnostic_state(SQLSMALLINT handle_type,
                                      SQLHANDLE handle) {
    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        handle_type, handle, 1, state, nullptr, nullptr, 0, nullptr));
    return reinterpret_cast<const char*>(state);
  }

  SQLHENV environment_{nullptr};
  SQLHDBC connection_{nullptr};
  SQLHSTMT statement_{nullptr};
};

TEST_F(AttributeApisTest, StoresLoginTimeout) {
  SQLUINTEGER value = 0;
  SQLINTEGER length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, &value, sizeof(value), &length));
  EXPECT_EQ(30u, value);
  EXPECT_EQ(sizeof(SQLUINTEGER), static_cast<std::size_t>(length));

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, integer_value(7), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_LOGIN_TIMEOUT, &value, sizeof(value), nullptr));
  EXPECT_EQ(7u, value);
}

TEST_F(AttributeApisTest, StoresEnvironmentVersion) {
  SQLINTEGER version = 0;
  SQLINTEGER length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetEnvAttr(
      environment_, SQL_ATTR_ODBC_VERSION, &version, sizeof(version), &length));
  EXPECT_EQ(SQL_OV_ODBC3, version);
  EXPECT_EQ(sizeof(SQLINTEGER), static_cast<std::size_t>(length));

#ifdef SQL_OV_ODBC3_80
  EXPECT_EQ(SQL_SUCCESS, SQLSetEnvAttr(
      environment_, SQL_ATTR_ODBC_VERSION, integer_value(SQL_OV_ODBC3_80), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetEnvAttr(
      environment_, SQL_ATTR_ODBC_VERSION, &version, sizeof(version), nullptr));
  EXPECT_EQ(SQL_OV_ODBC3_80, version);
#endif

  EXPECT_EQ(SQL_ERROR, SQLSetEnvAttr(
      environment_, SQL_ATTR_ODBC_VERSION, integer_value(999), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_ENV, environment_));
}

TEST_F(AttributeApisTest, NegotiatesNativeSqlwcharEncodingWithIodbc) {
  SQLINTEGER driver_encoding = 0;
  SQLINTEGER length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetEnvAttr(
      environment_, rs::odbc::IODBC_ATTR_DRIVER_UNICODE_TYPE,
      &driver_encoding, sizeof(driver_encoding), &length));
  EXPECT_EQ(static_cast<SQLINTEGER>(rs::odbc::NATIVE_SQLWCHAR_ENCODING),
            driver_encoding);
  EXPECT_EQ(sizeof(SQLINTEGER), static_cast<std::size_t>(length));

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, rs::odbc::IODBC_ATTR_APP_WCHAR_TYPE,
      integer_value(rs::odbc::NATIVE_SQLWCHAR_ENCODING), 0));

  const auto other_encoding = sizeof(SQLWCHAR) == 2
      ? rs::odbc::IODBC_CP_UCS4 : rs::odbc::IODBC_CP_UTF16;
  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(
      connection_, rs::odbc::IODBC_ATTR_APP_WCHAR_TYPE,
      integer_value(other_encoding), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, StoresAutocommitMode) {
  SQLUINTEGER value = 99;
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_AUTOCOMMIT, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_AUTOCOMMIT_ON, value);

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, SQL_ATTR_AUTOCOMMIT,
      integer_value(SQL_AUTOCOMMIT_OFF), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_AUTOCOMMIT, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_AUTOCOMMIT_OFF, value);

  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(
      connection_, SQL_ATTR_AUTOCOMMIT, integer_value(99), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, StoresTransactionIsolation) {
  SQLUINTEGER value = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_TXN_ISOLATION, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_TXN_READ_COMMITTED, value);

  EXPECT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
      connection_, SQL_ATTR_TXN_ISOLATION,
      integer_value(SQL_TXN_REPEATABLE_READ), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
      connection_, SQL_ATTR_TXN_ISOLATION, &value, sizeof(value), nullptr));
  EXPECT_EQ(SQL_TXN_REPEATABLE_READ, value);

  EXPECT_EQ(SQL_ERROR, SQLSetConnectAttr(
      connection_, SQL_ATTR_TXN_ISOLATION, integer_value(0), 0));
  EXPECT_EQ("HY024", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, EndTransactionValidatesState) {
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_DBC, connection_, SQL_COMMIT));
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_DBC, connection_, 99));
  EXPECT_EQ("HY012", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR, SQLEndTran(SQL_HANDLE_ENV, environment_, SQL_COMMIT));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_ENV, environment_));
}

TEST_F(AttributeApisTest, ReportsTransactionCapabilities) {
  SQLUSMALLINT small_value = 0;
  SQLSMALLINT length = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_TXN_CAPABLE, &small_value, sizeof(small_value), &length));
  EXPECT_EQ(SQL_TC_ALL, small_value);
  EXPECT_EQ(sizeof(SQLUSMALLINT), static_cast<std::size_t>(length));

  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_CURSOR_COMMIT_BEHAVIOR, &small_value,
      sizeof(small_value), nullptr));
  EXPECT_EQ(SQL_CB_PRESERVE, small_value);
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_CURSOR_ROLLBACK_BEHAVIOR, &small_value,
      sizeof(small_value), nullptr));
  EXPECT_EQ(SQL_CB_PRESERVE, small_value);

  SQLUINTEGER integer_value = 0;
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_DEFAULT_TXN_ISOLATION, &integer_value,
      sizeof(integer_value), &length));
  EXPECT_EQ(SQL_TXN_READ_COMMITTED, integer_value);
  EXPECT_EQ(sizeof(SQLUINTEGER), static_cast<std::size_t>(length));
  EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_TXN_ISOLATION_OPTION, &integer_value,
      sizeof(integer_value), nullptr));
  EXPECT_EQ(SQL_TXN_READ_UNCOMMITTED | SQL_TXN_READ_COMMITTED |
                SQL_TXN_REPEATABLE_READ | SQL_TXN_SERIALIZABLE,
            integer_value);
}

TEST_F(AttributeApisTest, ReportsApplicationStartupCapabilities) {
  const auto text_info = [&](SQLUSMALLINT type) {
    SQLCHAR value[64]{};
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS, SQLGetInfo(
        connection_, type, value, sizeof(value), &length));
    return std::string(reinterpret_cast<char*>(value),
                       static_cast<std::size_t>(length));
  };
  EXPECT_EQ("ODBCPP Driver", text_info(SQL_DRIVER_NAME));
  EXPECT_EQ("01.00.0000", text_info(SQL_DRIVER_VER));
  EXPECT_EQ("03.80", text_info(SQL_DRIVER_ODBC_VER));
#ifdef ODBCPP_ENABLE_REDSHIFT
  EXPECT_EQ("Amazon Redshift", text_info(SQL_DBMS_NAME));
#else
  EXPECT_EQ("PostgreSQL", text_info(SQL_DBMS_NAME));
#endif
  EXPECT_EQ("\"", text_info(SQL_IDENTIFIER_QUOTE_CHAR));
  EXPECT_EQ("database", text_info(SQL_CATALOG_TERM));
  EXPECT_EQ("schema", text_info(SQL_SCHEMA_TERM));
  EXPECT_EQ("Y", text_info(SQL_CATALOG_NAME));

  SQLUSMALLINT small_value = 0;
  SQLSMALLINT length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_MAX_IDENTIFIER_LEN, &small_value,
      sizeof(small_value), &length));
  EXPECT_EQ(63, small_value);
  EXPECT_EQ(sizeof(SQLUSMALLINT), static_cast<std::size_t>(length));
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_IDENTIFIER_CASE, &small_value,
      sizeof(small_value), nullptr));
  EXPECT_EQ(SQL_IC_LOWER, small_value);

  SQLUINTEGER capabilities = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_SCROLL_OPTIONS, &capabilities,
      sizeof(capabilities), nullptr));
  EXPECT_EQ(SQL_SO_FORWARD_ONLY, capabilities);
  ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
      connection_, SQL_GETDATA_EXTENSIONS, &capabilities,
      sizeof(capabilities), nullptr));
  EXPECT_EQ(SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER, capabilities);
}

TEST_F(AttributeApisTest, ReportsInformationStringTruncation) {
  SQLCHAR value[5]{};
  SQLSMALLINT required = 0;
  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLGetInfo(connection_, SQL_DRIVER_NAME, value, sizeof(value),
                       &required));
  EXPECT_STREQ("ODBC", reinterpret_cast<char*>(value));
  EXPECT_EQ(13, required);
  EXPECT_EQ("01004", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, ReportsImplementedFunctions) {
  SQLUSMALLINT supported = SQL_FALSE;
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLDRIVERCONNECT, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLMORERESULTS, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLFETCHSCROLL, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLCOLUMNS, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLPRIMARYKEYS, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLFOREIGNKEYS, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLSTATISTICS, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLPROCEDURES, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLPROCEDURECOLUMNS, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLSPECIALCOLUMNS, &supported));
  EXPECT_EQ(SQL_TRUE, supported);
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_SQLBROWSECONNECT, &supported));
  EXPECT_EQ(SQL_FALSE, supported);

  SQLUSMALLINT odbc2_functions[100]{};
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_ALL_FUNCTIONS, odbc2_functions));
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLCONNECT]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLDRIVERCONNECT]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLMORERESULTS]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLCOLUMNS]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLPRIMARYKEYS]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLFOREIGNKEYS]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLSTATISTICS]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLPROCEDURES]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLPROCEDURECOLUMNS]);
  EXPECT_EQ(SQL_TRUE, odbc2_functions[SQL_API_SQLSPECIALCOLUMNS]);
  EXPECT_EQ(SQL_FALSE, odbc2_functions[SQL_API_SQLBROWSECONNECT]);

  SQLUSMALLINT odbc3_functions[SQL_API_ODBC3_ALL_FUNCTIONS_SIZE]{};
  EXPECT_EQ(SQL_SUCCESS, SQLGetFunctions(
      connection_, SQL_API_ODBC3_ALL_FUNCTIONS, odbc3_functions));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLALLOCHANDLE));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLDRIVERCONNECT));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLMORERESULTS));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLFETCHSCROLL));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLCOLUMNS));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLPRIMARYKEYS));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLFOREIGNKEYS));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLSTATISTICS));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLPROCEDURES));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLPROCEDURECOLUMNS));
  EXPECT_EQ(SQL_TRUE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLSPECIALCOLUMNS));
  EXPECT_EQ(SQL_FALSE,
            SQL_FUNC_EXISTS(odbc3_functions, SQL_API_SQLBROWSECONNECT));

  EXPECT_EQ(SQL_ERROR, SQLGetFunctions(
      connection_, SQL_API_SQLCONNECT, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, StoresQueryTimeoutAndAllowsZero) {
  SQLULEN value = 99;
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, &value, sizeof(value), nullptr));
  EXPECT_EQ(0u, value);

  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, integer_value(3), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, &value, sizeof(value), nullptr));
  EXPECT_EQ(3u, value);
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, integer_value(0), 0));
}

TEST_F(AttributeApisTest, ReportsAllImplicitDescriptorHandles) {
  const SQLINTEGER attributes[] = {
      SQL_ATTR_APP_ROW_DESC, SQL_ATTR_APP_PARAM_DESC,
      SQL_ATTR_IMP_ROW_DESC, SQL_ATTR_IMP_PARAM_DESC};
  for (const auto attribute : attributes) {
    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        statement_, attribute, &descriptor, sizeof(descriptor), nullptr));
    ASSERT_NE(nullptr, descriptor);

    SQLSMALLINT count = -1;
    EXPECT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 0, SQL_DESC_COUNT, &count, 0, nullptr));
    EXPECT_EQ(0, count);
    EXPECT_EQ(SQL_ERROR, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
    EXPECT_EQ("HY017", diagnostic_state(SQL_HANDLE_DESC, descriptor));
  }
}

TEST_F(AttributeApisTest, ReportsForwardOnlyStatementDefaults) {
  struct AttributeExpectation {
    SQLINTEGER attribute;
    SQLULEN value;
  };
  const AttributeExpectation expectations[] = {
      {SQL_ATTR_CURSOR_TYPE, SQL_CURSOR_FORWARD_ONLY},
      {SQL_ATTR_CONCURRENCY, SQL_CONCUR_READ_ONLY},
      {SQL_ATTR_ROW_ARRAY_SIZE, 1},
      {SQL_ATTR_ROW_BIND_TYPE, SQL_BIND_BY_COLUMN},
      {SQL_ATTR_RETRIEVE_DATA, SQL_RD_ON},
      {SQL_ATTR_USE_BOOKMARKS, SQL_UB_OFF},
      {SQL_ATTR_ASYNC_ENABLE, SQL_ASYNC_ENABLE_OFF},
  };
  for (const auto& expectation : expectations) {
    SQLULEN value = 99;
    EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        statement_, expectation.attribute, &value, sizeof(value), nullptr));
    EXPECT_EQ(expectation.value, value);
    EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        statement_, expectation.attribute,
        integer_value(expectation.value), 0));
  }

  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_CURSOR_TYPE,
      integer_value(SQL_CURSOR_STATIC), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, SQL_ATTR_ROW_ARRAY_SIZE, integer_value(2), 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(AttributeApisTest, StoresMaximumRowsAndFinishesResultSequence) {
  SQLULEN value = 99;
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_MAX_ROWS, &value, sizeof(value), nullptr));
  EXPECT_EQ(0u, value);
  EXPECT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
      statement_, SQL_ATTR_MAX_ROWS, integer_value(25), 0));
  EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
      statement_, SQL_ATTR_MAX_ROWS, &value, sizeof(value), nullptr));
  EXPECT_EQ(25u, value);

  EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(statement_));
  EXPECT_EQ(SQL_INVALID_HANDLE, SQLMoreResults(nullptr));
}

TEST_F(AttributeApisTest, SupportsForwardOnlyFetchScroll) {
  EXPECT_EQ(SQL_NO_DATA, SQLFetchScroll(
      statement_, SQL_FETCH_NEXT, 0));
  EXPECT_EQ(SQL_ERROR, SQLFetchScroll(
      statement_, SQL_FETCH_FIRST, 0));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_ERROR, SQLFetchScroll(statement_, 999, 0));
  EXPECT_EQ("HY106", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_INVALID_HANDLE, SQLFetchScroll(nullptr, SQL_FETCH_NEXT, 0));
}

TEST_F(AttributeApisTest, ManagesStatementCursorAndBindings) {
  EXPECT_EQ(SQL_ERROR, SQLCloseCursor(statement_));
  EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, statement_));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_CLOSE));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_UNBIND));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeStmt(statement_, SQL_RESET_PARAMS));
  EXPECT_EQ(SQL_ERROR, SQLFreeStmt(statement_, 999));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(AttributeApisTest, ReportsUnsupportedAttributesAndNullOutputs) {
  SQLUINTEGER value = 0;
  EXPECT_EQ(SQL_ERROR, SQLGetConnectAttr(
      connection_, -12345, &value, sizeof(value), nullptr));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_DBC, connection_));

  EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
      statement_, -12345, integer_value(1), 0));
  EXPECT_EQ("HY092", diagnostic_state(SQL_HANDLE_STMT, statement_));

  EXPECT_EQ(SQL_ERROR, SQLGetStmtAttr(
      statement_, SQL_ATTR_QUERY_TIMEOUT, nullptr, 0, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, statement_));
}

TEST_F(AttributeApisTest, RejectsInvalidHandles) {
  EXPECT_EQ(SQL_INVALID_HANDLE, SQLSetConnectAttr(
      nullptr, SQL_ATTR_LOGIN_TIMEOUT, integer_value(1), 0));
  EXPECT_EQ(SQL_INVALID_HANDLE, SQLSetStmtAttr(
      nullptr, SQL_ATTR_QUERY_TIMEOUT, integer_value(1), 0));
}

TEST_F(AttributeApisTest, DriverConnectValidatesArgumentsBeforeConnecting) {
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(
      connection_, nullptr, nullptr, 0, nullptr, 0, nullptr,
      SQL_DRIVER_NOPROMPT));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_DBC, connection_));

  SQLCHAR connection_string[] = "SERVER=127.0.0.1";
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(
      connection_, nullptr, connection_string, SQL_NTS, nullptr, 0, nullptr,
      999));
  EXPECT_EQ("HY110", diagnostic_state(SQL_HANDLE_DBC, connection_));
  EXPECT_EQ(SQL_ERROR, SQLDriverConnect(
      connection_, nullptr, connection_string, SQL_NTS, nullptr, 0, nullptr,
      SQL_DRIVER_PROMPT));
  EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST_F(AttributeApisTest, NativeSqlValidatesInputAndConnectionState) {
  EXPECT_EQ(SQL_ERROR, SQLNativeSql(
      connection_, nullptr, 0, nullptr, 0, nullptr));
  EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_DBC, connection_));

  SQLCHAR query[] = "SELECT 1";
  EXPECT_EQ(SQL_ERROR, SQLNativeSql(
      connection_, query, SQL_NTS, nullptr, 0, nullptr));
  EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_DBC, connection_));
}

TEST(AttributeDeadlineTest, SaturatesUnlimitedTimeoutWithoutOverflow) {
  EXPECT_EQ(rs::util::Deadline::max(),
            rs::util::make_deadline(std::chrono::milliseconds::max()));
}

} // namespace
