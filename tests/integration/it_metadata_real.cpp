#include <gtest/gtest.h>
#include "odbc/odbc_types.h"

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
    EXPECT_STREQ("greeting", (char*)column_name);
    EXPECT_EQ(8, name_length);
    EXPECT_EQ(SQL_VARCHAR, data_type);
    EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);

    ret = SQLDescribeCol(hstmt, 2, column_name, sizeof(column_name), &name_length,
                        &data_type, &column_size, &decimal_digits, &nullable);
    EXPECT_EQ(SQL_SUCCESS, ret);
    EXPECT_STREQ("answer", (char*)column_name);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(10u, column_size);
    
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

TEST_F(MetadataIntegrationTest, ReportsAffectedRows) {
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(hstmt,
                            (SQLCHAR*)"CREATE TEMP TABLE row_count_test(value int)",
                            SQL_NTS));

    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(hstmt,
                            (SQLCHAR*)"INSERT INTO row_count_test VALUES (1), (2), (3)",
                            SQL_NTS));
    SQLLEN row_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);

    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirect(hstmt,
                            (SQLCHAR*)"UPDATE row_count_test SET value = value + 1 WHERE value >= 2",
                            SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(2, row_count);
}

TEST_F(MetadataIntegrationTest, ReportsSupportedTypeInformation) {
    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(hstmt, SQL_INTEGER));
    SQLSMALLINT columns = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
    EXPECT_EQ(19, columns);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char type_name[32]{};
    SQLSMALLINT data_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, type_name, sizeof(type_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_SSHORT, &data_type, 0, nullptr));
    EXPECT_STREQ("integer", type_name);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLTables) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_catalog_test(value int)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_catalog_test";
    SQLCHAR table_type[] = "TABLE";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        table_type, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));

    char returned_name[64]{};
    char returned_type[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_CHAR, returned_name, sizeof(returned_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, returned_type, sizeof(returned_type), nullptr));
    EXPECT_STREQ("odbcpp_catalog_test", returned_name);
    EXPECT_STREQ("TABLE", returned_type);
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLColumns) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_columns_test("
                  "id integer NOT NULL, name varchar(32))",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_columns_test";
    SQLCHAR column_pattern[] = "%";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        column_pattern, SQL_NTS));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char column_name[64]{};
    SQLSMALLINT data_type = 0;
    SQLINTEGER ordinal_position = 0;
    SQLSMALLINT nullable = SQL_NULLABLE_UNKNOWN;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &data_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 11, SQL_C_SSHORT, &nullable, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 17, SQL_C_SLONG, &ordinal_position, 0, nullptr));
    EXPECT_STREQ("id", column_name);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(SQL_NO_NULLS, nullable);
    EXPECT_EQ(1, ordinal_position);

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER column_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &data_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 7, SQL_C_SLONG, &column_size, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 11, SQL_C_SSHORT, &nullable, 0, nullptr));
    EXPECT_STREQ("name", column_name);
    EXPECT_EQ(SQL_VARCHAR, data_type);
    EXPECT_EQ(32, column_size);
    EXPECT_EQ(SQL_NULLABLE, nullable);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLPrimaryKeys) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_primary_key_test("
                  "tenant_id integer, item_id integer, "
                  "PRIMARY KEY(tenant_id, item_id))",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_primary_key_test";
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS));

    char column_name[64]{};
    char key_name[128]{};
    SQLSMALLINT key_sequence = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &key_sequence, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 6, SQL_C_CHAR, key_name, sizeof(key_name), nullptr));
    EXPECT_STREQ("tenant_id", column_name);
    EXPECT_EQ(1, key_sequence);
    EXPECT_STREQ("odbcpp_primary_key_test_pkey", key_name);

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &key_sequence, 0, nullptr));
    EXPECT_STREQ("item_id", column_name);
    EXPECT_EQ(2, key_sequence);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLForeignKeys) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_foreign_parent("
                  "tenant_id integer, item_id integer, "
                  "PRIMARY KEY(tenant_id, item_id))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_foreign_child("
                  "tenant_id integer, item_id integer, "
                  "CONSTRAINT odbcpp_child_parent_fk "
                  "FOREIGN KEY(tenant_id, item_id) REFERENCES "
                  "odbcpp_foreign_parent(tenant_id, item_id) "
                  "ON UPDATE CASCADE ON DELETE SET NULL)",
        SQL_NTS));
    SQLCHAR foreign_table[] = "odbcpp_foreign_child";
    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0,
        nullptr, 0, nullptr, 0, foreign_table, SQL_NTS));

    char primary_column[64]{};
    char foreign_column[64]{};
    char foreign_key_name[128]{};
    SQLSMALLINT key_sequence = 0;
    SQLSMALLINT update_rule = -1;
    SQLSMALLINT delete_rule = -1;
    SQLSMALLINT deferrability = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, primary_column, sizeof(primary_column), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_CHAR, foreign_column, sizeof(foreign_column), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 9, SQL_C_SSHORT, &key_sequence, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 10, SQL_C_SSHORT, &update_rule, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 11, SQL_C_SSHORT, &delete_rule, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 12, SQL_C_CHAR, foreign_key_name,
        sizeof(foreign_key_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 14, SQL_C_SSHORT, &deferrability, 0, nullptr));
    EXPECT_STREQ("tenant_id", primary_column);
    EXPECT_STREQ("tenant_id", foreign_column);
    EXPECT_EQ(1, key_sequence);
    EXPECT_EQ(SQL_CASCADE, update_rule);
    EXPECT_EQ(SQL_SET_NULL, delete_rule);
    EXPECT_STREQ("odbcpp_child_parent_fk", foreign_key_name);
    EXPECT_EQ(SQL_NOT_DEFERRABLE, deferrability);

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, primary_column, sizeof(primary_column), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_CHAR, foreign_column, sizeof(foreign_column), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 9, SQL_C_SSHORT, &key_sequence, 0, nullptr));
    EXPECT_STREQ("item_id", primary_column);
    EXPECT_STREQ("item_id", foreign_column);
    EXPECT_EQ(2, key_sequence);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLIndexes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_index_test("
                  "id integer, value text)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_index_test_id_idx "
                  "ON odbcpp_index_test(id DESC)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_index_test";
    ASSERT_EQ(SQL_SUCCESS, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_UNIQUE, SQL_QUICK));

    char index_name[128]{};
    char column_name[64]{};
    char ordering[2]{};
    SQLSMALLINT non_unique = SQL_TRUE;
    SQLSMALLINT type = 0;
    SQLSMALLINT ordinal_position = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_SSHORT, &non_unique, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 6, SQL_C_CHAR, index_name, sizeof(index_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 7, SQL_C_SSHORT, &type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_SSHORT, &ordinal_position, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 9, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 10, SQL_C_CHAR, ordering, sizeof(ordering), nullptr));
    EXPECT_EQ(SQL_FALSE, non_unique);
    EXPECT_STREQ("odbcpp_index_test_id_idx", index_name);
    EXPECT_EQ(SQL_INDEX_OTHER, type);
    EXPECT_EQ(1, ordinal_position);
    EXPECT_STREQ("id", column_name);
    EXPECT_STREQ("D", ordering);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLRoutines) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_routine_anchor(value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_catalog_function("
                  "first integer) RETURNS integer LANGUAGE SQL "
                  "AS 'SELECT first'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp_catalog_function";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS));

    char returned_name[128]{};
    SQLSMALLINT input_count = -1;
    SQLSMALLINT output_count = -1;
    SQLSMALLINT procedure_type = SQL_PT_UNKNOWN;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_CHAR, returned_name, sizeof(returned_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_SSHORT, &input_count, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &output_count, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_SSHORT, &procedure_type, 0, nullptr));
    EXPECT_STREQ("odbcpp_catalog_function", returned_name);
    EXPECT_EQ(1, input_count);
    EXPECT_EQ(0, output_count);
    EXPECT_EQ(SQL_PT_FUNCTION, procedure_type);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLRoutineColumns) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_routine_column_anchor("
                  "value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_column_function("
                  "first integer) RETURNS integer LANGUAGE SQL "
                  "AS 'SELECT first'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp_column_function";
    SQLCHAR column_name_pattern[] = "first";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        column_name_pattern, SQL_NTS));

    char returned_name[128]{};
    SQLSMALLINT column_type = SQL_PARAM_TYPE_UNKNOWN;
    SQLSMALLINT data_type = 0;
    SQLINTEGER ordinal_position = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 4, SQL_C_CHAR, returned_name, sizeof(returned_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 5, SQL_C_SSHORT, &column_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 6, SQL_C_SSHORT, &data_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 18, SQL_C_SLONG, &ordinal_position, 0, nullptr));
    EXPECT_STREQ("first", returned_name);
    EXPECT_EQ(SQL_PARAM_INPUT, column_type);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(1, ordinal_position);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, ReportsBestRowIdentifier) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_special_column_test("
                  "id integer PRIMARY KEY, value text)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_special_column_test";
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_SESSION, SQL_NO_NULLS));

    SQLSMALLINT scope = -1;
    char column_name[64]{};
    SQLSMALLINT data_type = 0;
    SQLSMALLINT pseudo_column = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SSHORT, &scope, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 2, SQL_C_CHAR, column_name, sizeof(column_name), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 3, SQL_C_SSHORT, &data_type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 8, SQL_C_SSHORT, &pseudo_column, 0, nullptr));
    EXPECT_EQ(SQL_SCOPE_SESSION, scope);
    EXPECT_STREQ("id", column_name);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(SQL_PC_NOT_PSEUDO, pseudo_column);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
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
