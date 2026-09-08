#include <gtest/gtest.h>
#include "odbc/odbc_types.h"

// Simple unit test using ODBC API directly
class MetadataAPITest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
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
