#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "odbc/unicode.h"
#include "core/database/database_factory.h"
#include "tests/test_handle_helpers.h"

#include <cstdint>

using namespace rs::odbc;

class DescriptorAPITest : public ::testing::Test {
protected:
    void SetUp() override {
        conn = std::make_shared<ODBCConnection>(nullptr);
        stmt = std::make_unique<ODBCStatement>(conn);
    }
    
    std::shared_ptr<ODBCConnection> conn;
    std::unique_ptr<ODBCStatement> stmt;
};

// Test SQLBindCol parameter validation
TEST_F(DescriptorAPITest, BindColValidation) {
    char buffer[256];
    SQLLEN indicator;
    
    // Invalid column numbers
    EXPECT_EQ(SQL_ERROR, stmt->bind_col(0, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
    
    // Valid binding
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(2, SQL_C_SLONG, buffer, sizeof(SQLINTEGER), &indicator));
}

TEST_F(DescriptorAPITest, BindColRejectsMalformedBufferDescriptions) {
    char buffer[16]{};
    SQLLEN indicator = 0;

    EXPECT_EQ(SQL_ERROR, stmt->bind_col(
        1, SQL_C_CHAR, buffer, -1, &indicator));
    EXPECT_EQ("HY090", stmt->get_sqlstate());
    stmt->clear_diagnostics();

    EXPECT_EQ(SQL_ERROR, stmt->bind_col(
        1, 12345, buffer, sizeof(buffer), &indicator));
    EXPECT_EQ("HY003", stmt->get_sqlstate());
    stmt->clear_diagnostics();

    EXPECT_EQ(SQL_ERROR, stmt->bind_col(
        1, SQL_C_NUMERIC, buffer, sizeof(buffer), &indicator));
    EXPECT_EQ("HYC00", stmt->get_sqlstate());
}

// Test column binding storage in ARD
TEST_F(DescriptorAPITest, ARDStorage) {
    char str_buffer[256];
    SQLINTEGER int_buffer;
    SQLLEN str_len, int_len;
    
    // Bind multiple columns
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(1, SQL_C_CHAR, str_buffer, sizeof(str_buffer), &str_len));
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(3, SQL_C_SLONG, &int_buffer, 0, &int_len));
    
    // Verify bindings are stored (would need access to internal state for full verification)
    // This tests that the binding calls succeed and don't interfere with each other
    
    // Rebind column 1
    char new_buffer[512];
    SQLLEN new_len;
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(1, SQL_C_CHAR, new_buffer, sizeof(new_buffer), &new_len));
}

// Test SQLDescribeParam functionality
TEST_F(DescriptorAPITest, DescribeParamBasic) {
    SQLSMALLINT data_type;
    SQLULEN param_size;
    SQLSMALLINT decimal_digits;
    SQLSMALLINT nullable;
    
    // Without prepared statement or parameters, should return error
    EXPECT_EQ(SQL_ERROR, stmt->describe_param(1, &data_type, &param_size, &decimal_digits, &nullable));
    
    // Invalid parameter number
    EXPECT_EQ(SQL_ERROR, stmt->describe_param(0, &data_type, &param_size, &decimal_digits, &nullable));
}

// Test column binding with different data types
TEST_F(DescriptorAPITest, MultipleDataTypeBinding) {
    char char_buf[256];
    SQLINTEGER int_buf;
    SQLBIGINT bigint_buf;
    SQLDOUBLE double_buf;
    SQLLEN char_len, int_len, bigint_len, double_len;
    
    // Bind different data types
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(1, SQL_C_CHAR, char_buf, sizeof(char_buf), &char_len));
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(2, SQL_C_SLONG, &int_buf, 0, &int_len));
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(3, SQL_C_SBIGINT, &bigint_buf, 0, &bigint_len));
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(4, SQL_C_DOUBLE, &double_buf, 0, &double_len));
    
    // All bindings should succeed without interference
}

// Test column binding array resizing
TEST_F(DescriptorAPITest, ColumnBindingResize) {
    char buffer[256];
    SQLLEN indicator;
    
    // Bind to high column number to test array resizing
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(10, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
    
    // Bind to lower column number
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(5, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
    
    // Bind to even higher column number
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(20, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
}

// Test binding with NULL indicators
TEST_F(DescriptorAPITest, NullIndicatorBinding) {
    char buffer[256];
    
    // Binding without indicator should work
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(1, SQL_C_CHAR, buffer, sizeof(buffer), nullptr));
    
    // Binding with indicator should work
    SQLLEN indicator;
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(2, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
}

// Test parameter metadata with empty IPD
TEST_F(DescriptorAPITest, EmptyIPDAccess) {
    SQLSMALLINT data_type;
    SQLULEN param_size;
    SQLSMALLINT decimal_digits;
    SQLSMALLINT nullable;
    
    // Access to non-existent parameter should fail
    EXPECT_EQ(SQL_ERROR, stmt->describe_param(1, &data_type, &param_size, &decimal_digits, &nullable));
    EXPECT_EQ(SQL_ERROR, stmt->describe_param(5, &data_type, &param_size, &decimal_digits, &nullable));
}

// Test descriptor consistency
TEST_F(DescriptorAPITest, DescriptorConsistency) {
    // Test that ARD and IRD don't interfere with each other
    char buffer[256];
    SQLLEN indicator;
    
    // Bind column (ARD operation)
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator));
    
    // Access column metadata (IRD operation) - should not be affected by binding
    SQLSMALLINT col_count;
    EXPECT_EQ(SQL_ERROR, stmt->get_num_result_cols(&col_count)); // No results yet, should fail
    
    // Binding should still work after metadata access
    EXPECT_EQ(SQL_SUCCESS, stmt->bind_col(2, SQL_C_SLONG, buffer, sizeof(SQLINTEGER), &indicator));
}

TEST(ExplicitDescriptorApiTest, AllocatesAndStoresHeaderAndRecordFields) {
    SQLHENV environment = nullptr;
    SQLHDBC connection = nullptr;
    SQLHDESC descriptor = nullptr;
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &environment));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
    descriptor = odbcpp::test::make_descriptor(connection);
    ASSERT_NE(nullptr, descriptor);

    const auto number = [](SQLULEN value) {
        return reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(value));
    };
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescField(descriptor, 0, SQL_DESC_ARRAY_SIZE,
                              number(4), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescField(descriptor, 1, SQL_DESC_CONCISE_TYPE,
                              number(SQL_C_CHAR), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescField(descriptor, 1, SQL_DESC_LENGTH,
                              number(128), 0));
    char data[128]{};
    SQLLEN indicator = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescField(descriptor, 1, SQL_DESC_DATA_PTR, data, 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescField(descriptor, 1, SQL_DESC_INDICATOR_PTR,
                              &indicator, 0));
    char name[] = "value";
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescField(descriptor, 1, SQL_DESC_NAME, name, SQL_NTS));

    SQLSMALLINT count = 0;
    SQLULEN array_size = 0;
    SQLSMALLINT type = 0;
    SQLULEN length = 0;
    SQLPOINTER data_ptr = nullptr;
    SQLLEN* indicator_ptr = nullptr;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(descriptor, 0, SQL_DESC_COUNT, &count, 0,
                              nullptr));
    EXPECT_EQ(1, count);
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(descriptor, 0, SQL_DESC_ARRAY_SIZE,
                              &array_size, 0, nullptr));
    EXPECT_EQ(4u, array_size);
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(descriptor, 1, SQL_DESC_CONCISE_TYPE, &type,
                              0, nullptr));
    EXPECT_EQ(SQL_C_CHAR, type);
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(descriptor, 1, SQL_DESC_LENGTH, &length, 0,
                              nullptr));
    EXPECT_EQ(128u, length);
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(descriptor, 1, SQL_DESC_DATA_PTR, &data_ptr,
                              0, nullptr));
    EXPECT_EQ(data, data_ptr);
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(descriptor, 1, SQL_DESC_INDICATOR_PTR,
                              &indicator_ptr, 0, nullptr));
    EXPECT_EQ(&indicator, indicator_ptr);
    char returned_name[16]{};
    SQLINTEGER name_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(descriptor, 1, SQL_DESC_NAME, returned_name,
                              sizeof(returned_name), &name_length));
    EXPECT_STREQ("value", returned_name);
    EXPECT_EQ(5, name_length);

    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DESC, descriptor));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST(ExplicitDescriptorApiTest, CopiesDescriptorState) {
    SQLHENV environment = nullptr;
    SQLHDBC connection = nullptr;
    SQLHDESC source = nullptr;
    SQLHDESC target = nullptr;
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &environment));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
    source = odbcpp::test::make_descriptor(connection);
    target = odbcpp::test::make_descriptor(connection);
    ASSERT_NE(nullptr, source);
    ASSERT_NE(nullptr, target);
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescField(source, 1, SQL_DESC_CONCISE_TYPE,
                              reinterpret_cast<SQLPOINTER>(SQL_C_WCHAR), 0));
    char name[] = "copied";
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescField(source, 1, SQL_DESC_NAME, name, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLCopyDesc(source, target));

    SQLSMALLINT type = 0;
    char returned_name[16]{};
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(target, 1, SQL_DESC_CONCISE_TYPE, &type, 0,
                              nullptr));
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescField(target, 1, SQL_DESC_NAME, returned_name,
                              sizeof(returned_name), nullptr));
    EXPECT_EQ(SQL_C_WCHAR, type);
    EXPECT_STREQ("copied", returned_name);

    SQLFreeHandle(SQL_HANDLE_DESC, target);
    SQLFreeHandle(SQL_HANDLE_DESC, source);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
}

TEST(ExplicitDescriptorApiTest, StoresAndReturnsWideDescriptorNames) {
    SQLHENV environment = nullptr;
    SQLHDBC connection = nullptr;
    SQLHDESC descriptor = nullptr;
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &environment));
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                            reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS,
              SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection));
    descriptor = odbcpp::test::make_descriptor(connection);
    ASSERT_NE(nullptr, descriptor);

    auto name = utf8_to_wide("Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x99\x82");
    ASSERT_TRUE(name.has_value());
    name->push_back(0);
    ASSERT_EQ(SQL_SUCCESS,
              SQLSetDescFieldW(descriptor, 1, SQL_DESC_NAME,
                               const_cast<SQLWCHAR*>(name->data()), SQL_NTS));

    SQLWCHAR returned_name[32]{};
    SQLINTEGER returned_bytes = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetDescFieldW(descriptor, 1, SQL_DESC_NAME, returned_name,
                               sizeof(returned_name), &returned_bytes));
    EXPECT_EQ(static_cast<SQLINTEGER>((name->size() - 1) * sizeof(SQLWCHAR)),
              returned_bytes);
    const auto returned_utf8 = wide_to_utf8(std::span<const SQLWCHAR>(
        returned_name, static_cast<std::size_t>(returned_bytes) /
                           sizeof(SQLWCHAR)));
    ASSERT_TRUE(returned_utf8.has_value());
    EXPECT_EQ("Gr\xc3\xbc\xc3\x9f" "e \xf0\x9f\x99\x82", *returned_utf8);

    SQLFreeHandle(SQL_HANDLE_DESC, descriptor);
    SQLFreeHandle(SQL_HANDLE_DBC, connection);
    SQLFreeHandle(SQL_HANDLE_ENV, environment);
}

TEST(DescriptorBindingApiTest, BindCallsPopulateStatementDescriptors) {
    SQLHENV environment = SQL_NULL_HENV;
    SQLHDBC connection = SQL_NULL_HDBC;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
    ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(
        environment, SQL_ATTR_ODBC_VERSION,
        reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_DBC, environment, &connection));
    const auto statement = odbcpp::test::make_statement(connection);
    ASSERT_NE(nullptr, statement);

    char column[32]{};
    SQLLEN column_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindCol(
        statement, 1, SQL_C_CHAR, column, sizeof(column), &column_length));
    SQLINTEGER parameter = 7;
    SQLLEN parameter_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        statement, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        10, 0, &parameter, 0, &parameter_length));

    SQLHDESC row = SQL_NULL_HDESC;
    SQLHDESC app_param = SQL_NULL_HDESC;
    SQLHDESC imp_param = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        statement, SQL_ATTR_APP_ROW_DESC, &row, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        statement, SQL_ATTR_APP_PARAM_DESC, &app_param, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        statement, SQL_ATTR_IMP_PARAM_DESC, &imp_param, 0, nullptr));

    SQLSMALLINT concise_type = 0;
    SQLLEN octet_length = -1;
    SQLPOINTER data = nullptr;
    SQLLEN* indicator = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        row, 1, SQL_DESC_CONCISE_TYPE, &concise_type, 0, nullptr));
    EXPECT_EQ(SQL_C_CHAR, concise_type);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        row, 1, SQL_DESC_OCTET_LENGTH, &octet_length, 0, nullptr));
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(column)), octet_length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        row, 1, SQL_DESC_DATA_PTR, &data, 0, nullptr));
    EXPECT_EQ(column, data);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        row, 1, SQL_DESC_INDICATOR_PTR, &indicator, 0, nullptr));
    EXPECT_EQ(&column_length, indicator);

    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        app_param, 1, SQL_DESC_CONCISE_TYPE, &concise_type, 0, nullptr));
    EXPECT_EQ(SQL_C_SLONG, concise_type);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        app_param, 1, SQL_DESC_DATA_PTR, &data, 0, nullptr));
    EXPECT_EQ(&parameter, data);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        imp_param, 1, SQL_DESC_CONCISE_TYPE, &concise_type, 0, nullptr));
    EXPECT_EQ(SQL_INTEGER, concise_type);
    SQLULEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        imp_param, 1, SQL_DESC_LENGTH, &length, 0, nullptr));
    EXPECT_EQ(10u, length);
    SQLSMALLINT parameter_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        imp_param, 1, SQL_DESC_PARAMETER_TYPE, &parameter_type, 0, nullptr));
    EXPECT_EQ(SQL_PARAM_INPUT, parameter_type);

    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
    EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}

TEST_F(DescriptorAPITest, ImplementationRowDescriptorIsReadOnly) {
    SQLHDESC implementation_row = SQL_NULL_HDESC;
    SQLHDESC application_row = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, stmt->get_attribute(
        SQL_ATTR_IMP_ROW_DESC, &implementation_row));
    ASSERT_EQ(SQL_SUCCESS, stmt->get_attribute(
        SQL_ATTR_APP_ROW_DESC, &application_row));

    EXPECT_EQ(SQL_ERROR, SQLSetDescField(
        implementation_row, 1, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(std::uintptr_t{SQL_VARCHAR}), 0));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_DESC, implementation_row, 1, state, nullptr,
        nullptr, 0, nullptr));
    EXPECT_STREQ("HY016", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLCopyDesc(application_row, implementation_row));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_DESC, implementation_row, 1, state, nullptr,
        nullptr, 0, nullptr));
    EXPECT_STREQ("HY016", reinterpret_cast<char*>(state));
}

TEST_F(DescriptorAPITest, ImplementationRowStatusHeadersRemainWritable) {
    SQLHDESC implementation_row = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, stmt->get_attribute(
        SQL_ATTR_IMP_ROW_DESC, &implementation_row));
    SQLUSMALLINT status = SQL_ROW_NOROW;
    SQLULEN processed = 0;
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation_row, 0, SQL_DESC_ARRAY_STATUS_PTR, &status, 0));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation_row, 0, SQL_DESC_ROWS_PROCESSED_PTR, &processed, 0));

    SQLUSMALLINT* returned_status = nullptr;
    SQLULEN* returned_processed = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation_row, 0, SQL_DESC_ARRAY_STATUS_PTR,
        &returned_status, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation_row, 0, SQL_DESC_ROWS_PROCESSED_PTR,
        &returned_processed, 0, nullptr));
    EXPECT_EQ(&status, returned_status);
    EXPECT_EQ(&processed, returned_processed);
}
