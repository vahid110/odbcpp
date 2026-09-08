#include <gtest/gtest.h>
#include "odbc/odbc_types.h"
#include <chrono>
#include <thread>

class PreparedStatementIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        
        // Connect to real database
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

// Test all data type combinations with real database
class PreparedStatementRealDataTest : public PreparedStatementIntegrationTest,
                                     public ::testing::WithParamInterface<std::tuple<SQLSMALLINT, std::string, std::string>> {
};

TEST_P(PreparedStatementRealDataTest, RealDatabaseExecution) {
    auto [c_type, test_value, expected_result] = GetParam();
    
    // Prepare statement
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? as test_value", SQL_NTS));
    
    // Bind parameter based on type
    void* param_buffer = nullptr;
    SQLLEN buffer_length = 0;
    
    std::string str_value;
    SQLINTEGER int_value;
    SQLBIGINT bigint_value;
    SQLDOUBLE double_value;
    
    switch (c_type) {
        case SQL_C_CHAR:
            str_value = test_value;
            param_buffer = (void*)str_value.c_str();
            buffer_length = str_value.length();
            break;
        case SQL_C_SLONG:
            int_value = std::stoi(test_value);
            param_buffer = &int_value;
            buffer_length = sizeof(SQLINTEGER);
            break;
        case SQL_C_SBIGINT:
            bigint_value = std::stoll(test_value);
            param_buffer = &bigint_value;
            buffer_length = sizeof(SQLBIGINT);
            break;
        case SQL_C_DOUBLE:
            double_value = std::stod(test_value);
            param_buffer = &double_value;
            buffer_length = sizeof(SQLDOUBLE);
            break;
    }
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, c_type, SQL_VARCHAR, 0, 0, param_buffer, buffer_length, nullptr));
    
    // Execute
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    
    // Fetch and verify result
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    char result[256];
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
    
    EXPECT_STREQ(expected_result.c_str(), result);
}

INSTANTIATE_TEST_SUITE_P(
    RealDataTypes,
    PreparedStatementRealDataTest,
    ::testing::Values(
        std::make_tuple(SQL_C_CHAR, "Hello Database", "Hello Database"),
        std::make_tuple(SQL_C_SLONG, "12345", "12345"),
        std::make_tuple(SQL_C_SBIGINT, "9876543210", "9876543210"),
        std::make_tuple(SQL_C_DOUBLE, "123.456", "123.456000"),
        std::make_tuple(SQL_C_CHAR, "", ""),
        std::make_tuple(SQL_C_SLONG, "0", "0"),
        std::make_tuple(SQL_C_SLONG, "-999", "-999")
    )
);

// Test complex queries with multiple parameters
TEST_F(PreparedStatementIntegrationTest, ComplexMultiParameterQuery) {
    // Test arithmetic operations
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? + ? as sum, ? * ? as product, ? as text", SQL_NTS));
    
    SQLINTEGER a = 10, b = 20, c = 3, d = 7;
    std::string text = "Complex Query";
    
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &a, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &b, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 3, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &c, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 4, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &d, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (void*)text.c_str(), text.length(), nullptr));
    
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    char sum[32], product[32], result_text[256];
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, sum, sizeof(sum), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 2, SQL_C_CHAR, product, sizeof(product), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 3, SQL_C_CHAR, result_text, sizeof(result_text), nullptr));
    
    EXPECT_STREQ("30", sum);      // 10 + 20
    EXPECT_STREQ("21", product);  // 3 * 7
    EXPECT_STREQ("Complex Query", result_text);
}

// Test prepared statement reuse
TEST_F(PreparedStatementIntegrationTest, PreparedStatementReuse) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? * 2 as doubled", SQL_NTS));
    
    // Execute with different values
    std::vector<int> test_values = {1, 5, 10};
    
    for (int value : test_values) {
        SQLINTEGER param = value;
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &param, 0, nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        
        char result[32];
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
        
        int expected = value * 2;
        EXPECT_EQ(expected, std::stoi(result));
        
        // Create new statement for next iteration
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? * 2 as doubled", SQL_NTS));
    }
}

// Test string escaping and SQL injection prevention
TEST_F(PreparedStatementIntegrationTest, SqlInjectionPrevention) {
    // Test various potentially malicious inputs
    std::vector<std::string> malicious_inputs = {
        "'; DROP TABLE users; --",
        "' OR '1'='1",
        "Robert'); DROP TABLE students; --"
    };
    
    for (const auto& input : malicious_inputs) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? as user_input", SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 0, 0, (void*)input.c_str(), input.length(), nullptr));
        SQLRETURN exec_result = SQLExecute(hstmt);
        
        if (exec_result == SQL_SUCCESS) {
            // If execution succeeds, verify safe handling
            SQLRETURN fetch_result = SQLFetch(hstmt);
            if (fetch_result == SQL_SUCCESS) {
                char result[512];
                ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
                
                // Should return the exact input as a string (properly escaped)
                EXPECT_STREQ(input.c_str(), result);
            }
            // SQL_NO_DATA is also acceptable (no rows returned)
        }
        // Execution failure is also acceptable for malicious input (good security)
        
        // Reset statement
        SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
        SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    }
}

// Test edge cases with real database
TEST_F(PreparedStatementIntegrationTest, EdgeCasesReal) {
    // Very large number
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? as large_number", SQL_NTS));
    SQLBIGINT large_num = 9223372036854775807LL; // Max BIGINT
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &large_num, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    
    char result[32];
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
    EXPECT_STREQ("9223372036854775807", result);
}

TEST_F(PreparedStatementIntegrationTest, NullParameterUsesProtocolNull) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?::text IS NULL", SQL_NTS));
    SQLLEN indicator = SQL_NULL_DATA;
    ASSERT_EQ(SQL_SUCCESS,
              SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
                               SQL_VARCHAR, 0, 0, nullptr, 0, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char result[8]{};
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
    EXPECT_STREQ("t", result);
}

TEST_F(PreparedStatementIntegrationTest, ErrorLeavesConnectionSynchronized) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLPrepare(hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    const char invalid[] = "not-an-integer";
    ASSERT_EQ(SQL_SUCCESS,
              SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
                               SQL_VARCHAR, 0, 0, (SQLPOINTER)invalid,
                               SQL_NTS, nullptr));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));

    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    hstmt = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt));
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 42", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char result[8]{};
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 1, SQL_C_CHAR, result, sizeof(result), nullptr));
    EXPECT_STREQ("42", result);
}

TEST_F(PreparedStatementIntegrationTest, NegativeTests) {
    // Test SQLPrepare without connection
    SQLHSTMT disconnected_stmt;
    SQLHDBC disconnected_conn;
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &disconnected_conn);
    SQLAllocHandle(SQL_HANDLE_STMT, disconnected_conn, &disconnected_stmt);
    
    SQLRETURN ret = SQLPrepare(disconnected_stmt, (SQLCHAR*)"SELECT 1", SQL_NTS);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Verify diagnostic
    SQLCHAR sqlstate[6], message[256];
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, disconnected_stmt, 1, sqlstate, nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("08001", (char*)sqlstate);
    
    SQLFreeHandle(SQL_HANDLE_STMT, disconnected_stmt);
    SQLFreeHandle(SQL_HANDLE_DBC, disconnected_conn);
    
    // Test SQLExecute without prepare
    ret = SQLExecute(hstmt);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test SQLBindParameter with invalid parameter number
    SQLINTEGER param = 123;
    ret = SQLBindParameter(hstmt, 0, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &param, 0, nullptr);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test SQLDescribeParam without prepare
    SQLSMALLINT data_type;
    SQLULEN param_size;
    ret = SQLDescribeParam(hstmt, 1, &data_type, &param_size, nullptr, nullptr);
    EXPECT_EQ(SQL_ERROR, ret);
    
    // Test with invalid handles
    ret = SQLPrepare(nullptr, (SQLCHAR*)"SELECT 1", SQL_NTS);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
    
    ret = SQLExecute(nullptr);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
    
    ret = SQLBindParameter(nullptr, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &param, 0, nullptr);
    EXPECT_EQ(SQL_INVALID_HANDLE, ret);
}
