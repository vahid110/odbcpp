#include <gtest/gtest.h>
#include <sql.h>
#include <sqlext.h>

class MetadataIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        
        // Connect to test database
        SQLRETURN ret = SQLConnect(hdbc, (SQLCHAR*)"DSN=RedshiftProd", SQL_NTS, nullptr, 0, nullptr, 0);
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
    EXPECT_EQ(SQL_VARCHAR, data_type);  // Currently all columns are VARCHAR
    
    // Test column attribute
    SQLLEN numeric_attr;
    ret = SQLColAttribute(hstmt, 1, SQL_DESC_TYPE, nullptr, 0, nullptr, &numeric_attr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_EQ(SQL_VARCHAR, numeric_attr);
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
    EXPECT_STREQ("HY000", (char*)sqlstate);
    
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