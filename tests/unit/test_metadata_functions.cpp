#include <gtest/gtest.h>
#include "odbc/odbc_types.h"
#include "tests/test_handle_helpers.h"

#include <string>

namespace {

std::string diagnostic_state(SQLSMALLINT handle_type, SQLHANDLE handle) {
    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(handle_type, handle, 1, state, nullptr, nullptr,
                            0, nullptr));
    return reinterpret_cast<const char*>(state);
}

}  // namespace

// Simple unit test using ODBC API directly
class MetadataAPITest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        hstmt = odbcpp::test::make_statement(hdbc);
    }
    
    void TearDown() override {
        if (hstmt) SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        if (hdbc) SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
        if (henv) SQLFreeHandle(SQL_HANDLE_ENV, henv);
    }
    
    SQLHENV henv = nullptr;
    SQLHDBC hdbc = nullptr;
    SQLHSTMT hstmt = nullptr;
};

TEST_F(MetadataAPITest, InvalidHandles) {
    SQLSMALLINT column_count;
    
    // Test with invalid statement handle
    EXPECT_EQ(SQL_INVALID_HANDLE, SQLNumResultCols(nullptr, &column_count));
    EXPECT_EQ(SQL_INVALID_HANDLE, SQLNumResultCols((SQLHSTMT)0x12345, &column_count));
    
    // Test with null output parameter
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, nullptr));

    SQLLEN row_count = 0;
    EXPECT_EQ(SQL_INVALID_HANDLE, SQLRowCount(nullptr, &row_count));
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, nullptr));

    EXPECT_EQ(SQL_INVALID_HANDLE, SQLGetTypeInfo(nullptr, SQL_ALL_TYPES));
    EXPECT_EQ(SQL_INVALID_HANDLE, SQLGetTypeInfoW(nullptr, SQL_ALL_TYPES));
    EXPECT_EQ(SQL_ERROR, SQLGetTypeInfo(hstmt, SQL_ALL_TYPES));
    EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_ERROR, SQLGetTypeInfoW(hstmt, SQL_ALL_TYPES));
    EXPECT_EQ("08003", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    SQLCHAR pattern[] = "%";
    EXPECT_EQ(SQL_INVALID_HANDLE, SQLTables(
        nullptr, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_EQ(SQL_INVALID_HANDLE, SQLTablesW(
        nullptr, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_EQ(SQL_ERROR, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_EQ("08001", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_ERROR, SQLTablesW(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_EQ("08001", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_ERROR, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, pattern, -2, nullptr, 0));
    EXPECT_EQ(SQL_ERROR, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, pattern, -2, nullptr, 0));
    EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(
        hstmt, nullptr, 0, nullptr, 0, pattern, -2));
    EXPECT_EQ("HY090", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_INVALID_HANDLE, SQLPrimaryKeysW(
        nullptr, nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_EQ(SQL_ERROR, SQLPrimaryKeysW(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_EQ("HY009", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_ERROR, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0,
        nullptr, 0, nullptr, 0, nullptr, 0));
    EXPECT_EQ(SQL_ERROR, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0,
        nullptr, 0, nullptr, 0, pattern, -2));
    EXPECT_EQ(SQL_ERROR, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0,
        SQL_INDEX_ALL, SQL_QUICK));
    EXPECT_EQ(SQL_ERROR, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, pattern, SQL_NTS,
        99, SQL_QUICK));
    EXPECT_EQ(SQL_ERROR, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, pattern, SQL_NTS,
        SQL_INDEX_ALL, 99));
    EXPECT_EQ(SQL_ERROR, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, pattern, -2));
    EXPECT_EQ(SQL_ERROR, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0, pattern, -2));
    EXPECT_EQ(SQL_ERROR, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0, nullptr, 0,
        SQL_SCOPE_SESSION, SQL_NO_NULLS));
    EXPECT_EQ(SQL_ERROR, SQLSpecialColumns(
        hstmt, 99, nullptr, 0, nullptr, 0, pattern, SQL_NTS,
        SQL_SCOPE_SESSION, SQL_NO_NULLS));
    EXPECT_EQ(SQL_ERROR, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0, pattern, SQL_NTS,
        99, SQL_NO_NULLS));
    EXPECT_EQ(SQL_ERROR, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0, pattern, SQL_NTS,
        SQL_SCOPE_SESSION, 99));
}

TEST_F(MetadataAPITest, NoQueryExecuted) {
    SQLSMALLINT column_count;
    
    // Should fail if no query executed
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    
    // SQLDescribeCol should also fail
    SQLCHAR column_name[256];
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(hstmt, 1, column_name, sizeof(column_name), 
                                       nullptr, nullptr, nullptr, nullptr, nullptr));
}

TEST_F(MetadataAPITest, InvalidColumnNumbers) {
    // Even without a real query, test parameter validation
    SQLCHAR column_name[256];
    SQLSMALLINT data_type;
    
    // Invalid column numbers should be caught
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(hstmt, 0, column_name, sizeof(column_name), 
                                       nullptr, &data_type, nullptr, nullptr, nullptr));
}
