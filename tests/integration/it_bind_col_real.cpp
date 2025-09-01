#include <gtest/gtest.h>
#include <sql.h>
#include <sqlext.h>

class BindColIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        
        SQLRETURN ret = SQLConnect(hdbc, (SQLCHAR*)"DSN=RedshiftProd", SQL_NTS, nullptr, 0, nullptr, 0);
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
    
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    // Verify conversions
    EXPECT_STREQ("123", int_as_str);
    EXPECT_EQ(456, str_as_int);
    EXPECT_EQ(78, double_as_int);  // Truncated
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
    EXPECT_STREQ("HY000", (char*)sqlstate);
    EXPECT_TRUE(strstr((char*)message, "column number") != nullptr);
    
    // Test SQLFetch without execution
    SQLHSTMT new_stmt;
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &new_stmt);
    ret = SQLFetch(new_stmt);
    EXPECT_EQ(SQL_NO_DATA, ret);
    SQLFreeHandle(SQL_HANDLE_STMT, new_stmt);
    
    // Test SQLGetData without execution
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &new_stmt);
    ret = SQLGetData(new_stmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Verify diagnostic
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, new_stmt, 1, sqlstate, nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("HY000", (char*)sqlstate);
    SQLFreeHandle(SQL_HANDLE_STMT, new_stmt);
    
    // Test SQLGetData with invalid column
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 1", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    ret = SQLGetData(hstmt, 0, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
    
    ret = SQLGetData(hstmt, 999, SQL_C_CHAR, buffer, sizeof(buffer), &len);
    EXPECT_EQ(SQL_ERROR, ret);
}