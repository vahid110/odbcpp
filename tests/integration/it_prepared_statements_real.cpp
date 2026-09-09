#include <gtest/gtest.h>
#include "odbc/odbc_types.h"
#include <chrono>
#include <cstdint>
#include <cstring>
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

TEST_F(PreparedStatementIntegrationTest, BinaryParameterRoundTripsAsBytea) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    unsigned char input[]{0x00, 0x01, 0x7f, 0xff};
    SQLLEN input_length = sizeof(input);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_VARBINARY, sizeof(input),
        0, input, sizeof(input), &input_length));
    SQLUSMALLINT param_status = SQL_PARAM_UNUSED;
    SQLULEN params_processed = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAM_STATUS_PTR, &param_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &params_processed, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    EXPECT_EQ(1u, params_processed);
    EXPECT_EQ(SQL_PARAM_SUCCESS, param_status);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    unsigned char output[sizeof(input)]{};
    SQLLEN output_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_BINARY, output, sizeof(output), &output_length));
    EXPECT_EQ(sizeof(input), static_cast<std::size_t>(output_length));
    EXPECT_EQ(0, std::memcmp(input, output, sizeof(input)));
}

TEST_F(PreparedStatementIntegrationTest,
       ApplicationDescriptorsDrivePreparedExecution) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer + 1", SQL_NTS));

    SQLHDESC application = SQL_NULL_HDESC;
    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &application));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLINTEGER input = 41;
    const auto number = [](SQLLEN value) {
        return reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(value));
    };
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        application, 1, SQL_DESC_CONCISE_TYPE,
        number(SQL_C_SLONG), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        application, 1, SQL_DESC_DATA_PTR, &input, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_CONCISE_TYPE,
        number(SQL_INTEGER), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_PARAMETER_TYPE,
        number(SQL_PARAM_INPUT), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_PARAM_DESC, application, 0));

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &output, 0, nullptr));
    EXPECT_EQ(42, output);

    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, application));
}

TEST_F(PreparedStatementIntegrationTest, PreparePreservesParameterBindings) {
    SQLINTEGER input = 7;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &output, 0, nullptr));
    EXPECT_EQ(input, output);
}

TEST_F(PreparedStatementIntegrationTest,
       RejectsUnsupportedAttachedParameterArraysBeforeExecution) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLINTEGER input = 7;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DESC, hdbc, &descriptor));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        descriptor, 0, SQL_DESC_ARRAY_SIZE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{2}), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_APP_PARAM_DESC, descriptor, 0));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));

    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HYC00", reinterpret_cast<char*>(state));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
}

TEST_F(PreparedStatementIntegrationTest, ReportsPreparedParameterCount) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?, '?'::text, ? /* ? */, $$?$$, ?", SQL_NTS));
    SQLSMALLINT parameter_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(3, parameter_count);
}

TEST_F(PreparedStatementIntegrationTest,
       ReplacesPreparedStatementsAndProtectsOpenCursors) {
    SQLCHAR state[6]{};
    const auto expect_state = [&](SQLRETURN result, const char* expected) {
        EXPECT_EQ(SQL_ERROR, result);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(state));
    };

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 11", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    expect_state(SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 12", SQL_NTS), "24000");
    expect_state(SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 13", SQL_NTS), "24000");
    expect_state(SQLExecute(hstmt), "24000");

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 21", SQL_NTS));
    SQLSMALLINT parameter_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumParams(hstmt, &parameter_count));
    EXPECT_EQ(0, parameter_count);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    expect_state(SQLExecute(hstmt), "HY010");
}

TEST_F(PreparedStatementIntegrationTest,
       MissingParameterFailsAndExtraBindingIsIgnored) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::integer", SQL_NTS));
    SQLUSMALLINT param_status = SQL_PARAM_UNUSED;
    SQLULEN params_processed = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAM_STATUS_PTR, &param_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &params_processed, 0));

    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ(1u, params_processed);
    EXPECT_EQ(SQL_PARAM_ERROR, param_status);
    SQLCHAR sqlstate[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, sqlstate, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("07009", reinterpret_cast<char*>(sqlstate));

    SQLINTEGER ignored = 99;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &ignored, 0, nullptr));
    SQLINTEGER used = 41;
    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &used, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &output, 0, nullptr));
    EXPECT_EQ(used, output);
}

TEST_F(PreparedStatementIntegrationTest,
       BindingValidationUsesSpecificDiagnostics) {
    SQLINTEGER value = 7;
    SQLCHAR state[6]{};
    const auto expect_state = [&](SQLRETURN result, const char* expected) {
        EXPECT_EQ(SQL_ERROR, result);
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(state));
    };

    expect_state(SQLBindParameter(
        hstmt, 1, 12345, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &value, 0, nullptr), "HY105");
    expect_state(SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, 12345, SQL_INTEGER,
        0, 0, &value, 0, nullptr), "HY003");
    expect_state(SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, 12345,
        0, 0, &value, 0, nullptr), "HY004");
    expect_state(SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        0, 0, &value, -1, nullptr), "HY090");
    expect_state(SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR,
        0, 0, nullptr, 0, nullptr), "HY009");

    EXPECT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &value, 0, nullptr));
}

TEST_F(PreparedStatementIntegrationTest, DefaultCTypeUsesSqlTypeMapping) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::smallint", SQL_NTS));
    SQLSMALLINT input = 123;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT, SQL_SMALLINT,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER output = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &output, 0, nullptr));
    EXPECT_EQ(input, output);
}

TEST_F(PreparedStatementIntegrationTest, AutocommitOffSupportsCommitAndRollback) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_tx_test(value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
        hdbc, SQL_ATTR_AUTOCOMMIT,
        reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(SQL_AUTOCOMMIT_OFF)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
        hdbc, SQL_ATTR_TXN_ISOLATION,
        reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(SQL_TXN_REPEATABLE_READ)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SHOW transaction_isolation", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char isolation[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, isolation, sizeof(isolation), nullptr));
    EXPECT_STREQ("repeatable read", isolation);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_tx_test VALUES (1)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT count(*) FROM odbcpp_tx_test", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &count, 0, nullptr));
    EXPECT_EQ(0, count);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_tx_test VALUES (2)", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_COMMIT));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT count(*) FROM odbcpp_tx_test", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &count, 0, nullptr));
    EXPECT_EQ(1, count);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSetConnectAttr(
        hdbc, SQL_ATTR_AUTOCOMMIT,
        reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(SQL_AUTOCOMMIT_ON)), 0));
    SQLUINTEGER autocommit = SQL_AUTOCOMMIT_OFF;
    ASSERT_EQ(SQL_SUCCESS, SQLGetConnectAttr(
        hdbc, SQL_ATTR_AUTOCOMMIT, &autocommit, sizeof(autocommit), nullptr));
    EXPECT_EQ(SQL_AUTOCOMMIT_ON, autocommit);
}

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

    SQLSMALLINT parameter_type = 0;
    SQLULEN parameter_size = 0;
    SQLSMALLINT decimal_digits = -1;
    SQLSMALLINT nullable = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLDescribeParam(hstmt, 1, &parameter_type, &parameter_size,
                               &decimal_digits, &nullable));
    EXPECT_EQ(SQL_INTEGER, parameter_type);
    EXPECT_EQ(10u, parameter_size);
    EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);

    ASSERT_EQ(SQL_SUCCESS,
              SQLDescribeParam(hstmt, 5, &parameter_type, &parameter_size,
                               &decimal_digits, &nullable));
    EXPECT_EQ(SQL_VARCHAR, parameter_type);
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
                               sizeof(invalid) - 1, nullptr));
    SQLUSMALLINT param_status = SQL_PARAM_UNUSED;
    SQLULEN params_processed = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAM_STATUS_PTR, &param_status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &params_processed, 0));
    EXPECT_EQ(SQL_ERROR, SQLExecute(hstmt));
    EXPECT_EQ(1u, params_processed);
    EXPECT_EQ(SQL_PARAM_ERROR, param_status);

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
    SQLHSTMT disconnected_stmt = reinterpret_cast<SQLHSTMT>(
        static_cast<std::uintptr_t>(1));
    SQLHDBC disconnected_conn;
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &disconnected_conn);
    SQLRETURN ret = SQLAllocHandle(
        SQL_HANDLE_STMT, disconnected_conn, &disconnected_stmt);
    EXPECT_EQ(SQL_ERROR, ret);
    EXPECT_EQ(nullptr, disconnected_stmt);
    
    // Verify diagnostic
    SQLCHAR sqlstate[6], message[256];
    ret = SQLGetDiagRec(SQL_HANDLE_DBC, disconnected_conn, 1, sqlstate,
                        nullptr, message, sizeof(message), nullptr);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("08003", (char*)sqlstate);
    
    SQLFreeHandle(SQL_HANDLE_DBC, disconnected_conn);
    
    // Test SQLExecute without prepare
    ret = SQLExecute(hstmt);
    EXPECT_EQ(SQL_ERROR, ret);
    ret = SQLGetDiagRec(SQL_HANDLE_STMT, hstmt, 1, sqlstate,
                        nullptr, message, sizeof(message), nullptr);
    ASSERT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("HY010", reinterpret_cast<char*>(sqlstate));
    
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
