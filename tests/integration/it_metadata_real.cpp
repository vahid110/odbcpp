#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include "tests/test_connection_config.h"

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

class MetadataIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
        SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
        SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
        
        // Connect to test database
        SQLRETURN ret = SQLConnect(
            hdbc, odbcpp::test::configured_connection_string_data(), SQL_NTS,
            nullptr, 0, nullptr, 0);
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

TEST_F(MetadataIntegrationTest, ReportsCurrentRowNumberOnlyWhilePositioned) {
    SQLCHAR query[] =
        "SELECT value FROM (VALUES (10), (20)) AS rows(value) ORDER BY value";
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(hstmt, query, SQL_NTS));

    SQLULEN row_number = 99;
    EXPECT_EQ(SQL_ERROR, SQLGetStmtAttr(
        hstmt, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number), nullptr));
    EXPECT_EQ(99u, row_number);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number), nullptr));
    EXPECT_EQ(1u, row_number);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number), nullptr));
    EXPECT_EQ(2u, row_number);

    ASSERT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    row_number = 99;
    EXPECT_EQ(SQL_ERROR, SQLGetStmtAttr(
        hstmt, SQL_ATTR_ROW_NUMBER, &row_number, sizeof(row_number), nullptr));
    EXPECT_EQ(99u, row_number);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));

    SQLINTEGER value = 0;
    EXPECT_EQ(SQL_ERROR, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &value, sizeof(value), nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));

    EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
        hstmt, SQL_ATTR_CURSOR_TYPE,
        reinterpret_cast<SQLPOINTER>(SQL_CURSOR_FORWARD_ONLY), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    SQLCHAR prepared_query[] = "SELECT 1";
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(hstmt, prepared_query, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLSetStmtAttr(
        hstmt, SQL_ATTR_CURSOR_TYPE,
        reinterpret_cast<SQLPOINTER>(SQL_CURSOR_FORWARD_ONLY), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY011", reinterpret_cast<char*>(state));
}

TEST_F(MetadataIntegrationTest, ReportsAnsiColumnNameTruncation) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 AS long_column_name", SQL_NTS));

    char name[5]{};
    SQLSMALLINT length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name), &length,
        nullptr, nullptr, nullptr, nullptr));
    EXPECT_STREQ("long", name);
    EXPECT_EQ(16, length);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));

    name[0] = '\0';
    length = 0;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLColAttribute(
        hstmt, 1, SQL_DESC_NAME, name, sizeof(name), &length, nullptr));
    EXPECT_STREQ("long", name);
    EXPECT_EQ(16, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(MetadataIntegrationTest, ImplementationDescriptorReportsResultMetadata) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 'hello'::varchar(12) AS label, "
                  "12.34::numeric(8,2) AS amount",
        SQL_NTS));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_ROW_DESC, &descriptor, 0, nullptr));
    SQLSMALLINT count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 0, SQL_DESC_COUNT, &count, 0, nullptr));
    EXPECT_EQ(2, count);

    char name[16]{};
    SQLINTEGER name_length = 0;
    SQLSMALLINT type = 0;
    SQLULEN length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 1, SQL_DESC_NAME, name, sizeof(name), &name_length));
    EXPECT_STREQ("label", name);
    EXPECT_EQ(5, name_length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 1, SQL_DESC_CONCISE_TYPE, &type, 0, nullptr));
    EXPECT_EQ(SQL_VARCHAR, type);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 1, SQL_DESC_LENGTH, &length, 0, nullptr));
    EXPECT_EQ(12u, length);

    SQLSMALLINT precision = 0;
    SQLSMALLINT scale = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 2, SQL_DESC_PRECISION, &precision, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 2, SQL_DESC_SCALE, &scale, 0, nullptr));
    EXPECT_EQ(8, precision);
    EXPECT_EQ(2, scale);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 0, SQL_DESC_COUNT, &count, 0, nullptr));
    EXPECT_EQ(0, count);
}

TEST_F(MetadataIntegrationTest, ImplementationDescriptorCompletesFieldMatrix) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric(8,2) AS amount, "
                  "'hello'::varchar(12) AS label, CURRENT_DATE AS day",
        SQL_NTS));

    SQLHDESC descriptor = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_ROW_DESC, &descriptor, 0, nullptr));

    SQLSMALLINT allocation = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        descriptor, 0, SQL_DESC_ALLOC_TYPE, &allocation, 0, nullptr));
    EXPECT_EQ(SQL_DESC_ALLOC_AUTO, allocation);

    struct NumericField {
        SQLSMALLINT identifier;
        SQLLEN expected;
    };
    const NumericField numeric_fields[] = {
        {SQL_DESC_AUTO_UNIQUE_VALUE, SQL_FALSE},
        {SQL_DESC_CASE_SENSITIVE, SQL_FALSE},
        {SQL_DESC_DISPLAY_SIZE, 10},
        {SQL_DESC_FIXED_PREC_SCALE, SQL_TRUE},
        {SQL_DESC_NUM_PREC_RADIX, 10},
        {SQL_DESC_OCTET_LENGTH, 10},
        {SQL_DESC_ROWVER, SQL_FALSE},
        {SQL_DESC_SEARCHABLE, SQL_PRED_SEARCHABLE},
        {SQL_DESC_UNNAMED, SQL_NAMED},
        {SQL_DESC_UNSIGNED, SQL_FALSE},
        {SQL_DESC_UPDATABLE, SQL_ATTR_READONLY},
    };
    for (const auto& field : numeric_fields) {
        SQLLEN value = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
            descriptor, 1, field.identifier, &value, 0, nullptr));
        EXPECT_EQ(field.expected, value) << field.identifier;
    }

    struct TextField {
        SQLSMALLINT identifier;
        const char* expected;
    };
    const TextField text_fields[] = {
        {SQL_DESC_BASE_COLUMN_NAME, ""},
        {SQL_DESC_BASE_TABLE_NAME, ""},
        {SQL_DESC_CATALOG_NAME, ""},
        {SQL_DESC_LABEL, "amount"},
        {SQL_DESC_LITERAL_PREFIX, ""},
        {SQL_DESC_LITERAL_SUFFIX, ""},
        {SQL_DESC_LOCAL_TYPE_NAME, "numeric"},
        {SQL_DESC_NAME, "amount"},
        {SQL_DESC_SCHEMA_NAME, ""},
        {SQL_DESC_TABLE_NAME, ""},
        {SQL_DESC_TYPE_NAME, "numeric"},
    };
    for (const auto& field : text_fields) {
        char value[32]{'x'};
        SQLINTEGER length = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
            descriptor, 1, field.identifier, value, sizeof(value),
            &length));
        EXPECT_STREQ(field.expected, value) << field.identifier;
        EXPECT_EQ(static_cast<SQLINTEGER>(std::strlen(field.expected)),
                  length) << field.identifier;
    }

    char name[16]{};
    SQLSMALLINT name_length = -1;
    SQLSMALLINT type = 0;
    SQLSMALLINT subtype = -1;
    SQLLEN length = -1;
    SQLSMALLINT precision = -1;
    SQLSMALLINT scale = -1;
    SQLSMALLINT nullable = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescRec(
        descriptor, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &type, &subtype, &length, &precision, &scale,
        &nullable));
    EXPECT_STREQ("amount", name);
    EXPECT_EQ(6, name_length);
    EXPECT_EQ(SQL_NUMERIC, type);
    EXPECT_EQ(0, subtype);
    EXPECT_EQ(10, length);
    EXPECT_EQ(8, precision);
    EXPECT_EQ(2, scale);
    EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);

    SQLWCHAR wide_name[16]{};
    name_length = -1;
    type = 0;
    subtype = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescRecW(
        descriptor, 3, wide_name,
        static_cast<SQLSMALLINT>(std::size(wide_name)), &name_length,
        &type, &subtype, nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(3, name_length);
    EXPECT_EQ(SQL_DATETIME, type);
    EXPECT_EQ(SQL_CODE_DATE, subtype);
    const auto wide_name_utf8 = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(wide_name, 3));
    ASSERT_TRUE(wide_name_utf8.has_value());
    EXPECT_EQ("day", *wide_name_utf8);

    SQLWCHAR short_type_name[4]{};
    SQLINTEGER required_bytes = -1;
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetDescFieldW(
        descriptor, 1, SQL_DESC_TYPE_NAME, short_type_name,
        sizeof(short_type_name), &required_bytes));
    EXPECT_EQ(static_cast<SQLINTEGER>(7 * sizeof(SQLWCHAR)), required_bytes);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_DESC, descriptor, 1, state, nullptr, nullptr, 0,
        nullptr));
    EXPECT_STREQ("01004", reinterpret_cast<char*>(state));
}

TEST_F(MetadataIntegrationTest, ExecutesAndPreparesUnicodeSql) {
    const std::string expected =
        "Gr\xc3\xbc\xc3\x9f" "e \xe4\xb8\x96\xe7\x95\x8c "
        "\xf0\x9f\x99\x82";
    auto direct_sql = rs::odbc::utf8_to_wide(
        "SELECT 'Gr\xc3\xbc\xc3\x9f" "e \xe4\xb8\x96\xe7\x95\x8c "
        "\xf0\x9f\x99\x82'::text");
    ASSERT_TRUE(direct_sql.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirectW(hstmt, direct_sql->data(),
                             static_cast<SQLINTEGER>(direct_sql->size())));
    SQLWCHAR bound_value[64]{};
    SQLLEN bound_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLBindCol(hstmt, 1, SQL_C_WCHAR, bound_value,
                         sizeof(bound_value), &bound_length));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    const auto converted_bound = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            bound_value, static_cast<std::size_t>(bound_length) /
                             sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_bound.has_value());
    EXPECT_EQ(expected, *converted_bound);
    SQLWCHAR value[64]{};
    SQLLEN value_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 1, SQL_C_WCHAR, value, sizeof(value),
                         &value_length));
    const auto converted_value = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            value, static_cast<std::size_t>(value_length) /
                       sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_value.has_value());
    EXPECT_EQ(expected, *converted_value);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    auto prepared_sql = rs::odbc::utf8_to_wide("SELECT ?::text");
    ASSERT_TRUE(prepared_sql.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLPrepareW(hstmt, prepared_sql->data(),
                          static_cast<SQLINTEGER>(prepared_sql->size())));
    const std::string prepared_expected = "\xf0\x9f\x9a\x80 prepared";
    auto parameter = rs::odbc::utf8_to_wide(prepared_expected);
    ASSERT_TRUE(parameter.has_value());
    parameter->push_back(0);
    SQLLEN parameter_length = SQL_NTS;
    ASSERT_EQ(SQL_SUCCESS,
              SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
                               SQL_WVARCHAR, parameter->size() - 1, 0,
                               parameter->data(),
                               static_cast<SQLLEN>(parameter->size() *
                                                   sizeof(SQLWCHAR)),
                               &parameter_length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLWCHAR prepared_value[64]{};
    SQLLEN prepared_length = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 1, SQL_C_WCHAR, prepared_value,
                         sizeof(prepared_value), &prepared_length));
    const auto converted_prepared = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            prepared_value, static_cast<std::size_t>(prepared_length) /
                                sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_prepared.has_value());
    EXPECT_EQ(prepared_expected, *converted_prepared);
}

TEST_F(MetadataIntegrationTest, FindsUnicodeCatalogIdentifiers) {
    const std::string table_name =
        "odbcpp\xe4\xb8\x96\xe7\x95\x8c" "table";
    const std::string column_name =
        "gr\xc3\xbc\xc3\x9f" "ecolumn";
    auto create_sql = rs::odbc::utf8_to_wide(
        "CREATE TEMP TABLE \"odbcpp\xe4\xb8\x96\xe7\x95\x8c"
        "table\" (\"gr\xc3\xbc\xc3\x9f" "ecolumn\" text)");
    ASSERT_TRUE(create_sql.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirectW(hstmt, create_sql->data(),
                             static_cast<SQLINTEGER>(create_sql->size())));

    auto wide_table = rs::odbc::utf8_to_wide(table_name);
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLTablesW(hstmt, nullptr, 0, nullptr, 0,
                         wide_table->data(),
                         static_cast<SQLSMALLINT>(wide_table->size()),
                         nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLWCHAR returned_table[128]{};
    SQLLEN returned_table_bytes = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 3, SQL_C_WCHAR, returned_table,
                         sizeof(returned_table), &returned_table_bytes));
    const auto converted_table = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            returned_table,
            static_cast<std::size_t>(returned_table_bytes) /
                sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_table.has_value());
    EXPECT_EQ(table_name, *converted_table);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    auto wide_column = rs::odbc::utf8_to_wide(column_name);
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLColumnsW(hstmt, nullptr, 0, nullptr, 0,
                          wide_table->data(),
                          static_cast<SQLSMALLINT>(wide_table->size()),
                          wide_column->data(),
                          static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLWCHAR returned_column[128]{};
    SQLLEN returned_column_bytes = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLGetData(hstmt, 4, SQL_C_WCHAR, returned_column,
                         sizeof(returned_column), &returned_column_bytes));
    const auto converted_column = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            returned_column,
            static_cast<std::size_t>(returned_column_bytes) /
                sizeof(SQLWCHAR)));
    ASSERT_TRUE(converted_column.has_value());
    EXPECT_EQ(column_name, *converted_column);
}

TEST_F(MetadataIntegrationTest, DescribesUnicodeColumnNames) {
    const std::string expected =
        "r\xc3\xa9sultat\xe4\xb8\x96\xe7\x95\x8c";
    auto query = rs::odbc::utf8_to_wide(
        "SELECT 1 AS \"r\xc3\xa9sultat\xe4\xb8\x96\xe7\x95\x8c\"");
    ASSERT_TRUE(query.has_value());
    ASSERT_EQ(SQL_SUCCESS,
              SQLExecDirectW(hstmt, query->data(),
                             static_cast<SQLINTEGER>(query->size())));

    SQLWCHAR described_name[64]{};
    SQLSMALLINT described_units = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLDescribeColW(hstmt, 1, described_name, 64,
                              &described_units, nullptr, nullptr, nullptr,
                              nullptr));
    const auto described = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            described_name, static_cast<std::size_t>(described_units)));
    ASSERT_TRUE(described.has_value());
    EXPECT_EQ(expected, *described);

    SQLWCHAR attribute_name[64]{};
    SQLSMALLINT attribute_bytes = 0;
    ASSERT_EQ(SQL_SUCCESS,
              SQLColAttributeW(hstmt, 1, SQL_DESC_NAME, attribute_name,
                               sizeof(attribute_name), &attribute_bytes,
                               nullptr));
    const auto attribute = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            attribute_name,
            static_cast<std::size_t>(attribute_bytes) /
                sizeof(SQLWCHAR)));
    ASSERT_TRUE(attribute.has_value());
    EXPECT_EQ(expected, *attribute);
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

    EXPECT_EQ(SQL_ERROR, SQLFetch(hstmt));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("24000", reinterpret_cast<char*>(state));
}

TEST_F(MetadataIntegrationTest, DescribesPreparedResultsBeforeExecution) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE prepared_metadata_state(value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt,
        (SQLCHAR*)"INSERT INTO prepared_metadata_state VALUES (?) "
                  "RETURNING value AS inserted_value",
        SQL_NTS));

    SQLINTEGER input = 42;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));

    SQLLEN row_count = 91;
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(91, row_count);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY010", reinterpret_cast<char*>(state));

    SQLSMALLINT column_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    char name[32]{};
    SQLSMALLINT data_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name), nullptr,
        &data_type, nullptr, nullptr, nullptr));
    EXPECT_STREQ("inserted_value", name);
    EXPECT_EQ(SQL_INTEGER, data_type);

    SQLHSTMT verification = SQL_NULL_HSTMT;
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(
        SQL_HANDLE_STMT, hdbc, &verification));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        verification,
        (SQLCHAR*)"SELECT count(*) FROM prepared_metadata_state", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(verification));
    SQLINTEGER rows_before_execution = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        verification, 1, SQL_C_SLONG, &rows_before_execution, 0, nullptr));
    EXPECT_EQ(0, rows_before_execution);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, verification));

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(1, row_count);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER returned = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &returned, 0, nullptr));
    EXPECT_EQ(input, returned);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    column_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    row_count = 92;
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(92, row_count);
}

TEST_F(MetadataIntegrationTest, DescribesPreparedColumnsDirectly) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt,
        (SQLCHAR*)"SELECT 12.34::numeric(8,2) AS prepared_amount, "
                  "'x'::varchar(12) AS prepared_label",
        SQL_NTS));

    char name[32]{"unchanged"};
    SQLSMALLINT name_length = -1;
    SQLSMALLINT data_type = 0;
    SQLULEN column_size = 0;
    SQLSMALLINT scale = -1;
    SQLSMALLINT nullable = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    EXPECT_STREQ("prepared_amount", name);
    EXPECT_EQ(15, name_length);
    EXPECT_EQ(SQL_NUMERIC, data_type);
    EXPECT_EQ(8u, column_size);
    EXPECT_EQ(2, scale);
    EXPECT_EQ(SQL_NULLABLE_UNKNOWN, nullable);

    char label[32]{"unchanged"};
    SQLSMALLINT label_length = -1;
    SQLLEN ignored_numeric = 81;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 2, SQL_DESC_LABEL, label, sizeof(label), &label_length,
        &ignored_numeric));
    EXPECT_STREQ("prepared_label", label);
    EXPECT_EQ(14, label_length);
    EXPECT_EQ(81, ignored_numeric);

    struct NumericAttribute {
        SQLUSMALLINT field;
        SQLLEN expected;
    };
    const NumericAttribute attributes[] = {
        {SQL_DESC_TYPE, SQL_NUMERIC},
        {SQL_DESC_CONCISE_TYPE, SQL_NUMERIC},
        {SQL_DESC_LENGTH, 8},
        {SQL_DESC_PRECISION, 8},
        {SQL_DESC_SCALE, 2},
        {SQL_DESC_NULLABLE, SQL_NULLABLE_UNKNOWN},
        {SQL_DESC_UNNAMED, SQL_NAMED},
    };
    for (const auto& attribute : attributes) {
        char ignored_text[]{"keep"};
        SQLSMALLINT ignored_length = 82;
        SQLLEN value = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
            hstmt, 1, attribute.field, ignored_text, -7, &ignored_length,
            &value));
        EXPECT_EQ(attribute.expected, value);
        EXPECT_STREQ("keep", ignored_text);
        EXPECT_EQ(82, ignored_length);
    }

    char ignored_text[]{"keep"};
    SQLSMALLINT ignored_length = 83;
    SQLLEN count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 999, SQL_DESC_COUNT, ignored_text, -7, &ignored_length,
        &count));
    EXPECT_EQ(2, count);
    EXPECT_STREQ("keep", ignored_text);
    EXPECT_EQ(83, ignored_length);

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    data_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 2, nullptr, 0, nullptr, &data_type, nullptr, nullptr,
        nullptr));
    EXPECT_EQ(SQL_VARCHAR, data_type);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    name[0] = '\0';
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name), nullptr,
        nullptr, nullptr, nullptr, nullptr));
    EXPECT_STREQ("prepared_amount", name);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT CURRENT_DATE AS prepared_date", SQL_NTS));
    SQLLEN descriptor_type = 0;
    SQLLEN concise_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_TYPE, nullptr, 0, nullptr, &descriptor_type));
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 1, SQL_DESC_CONCISE_TYPE, nullptr, 0, nullptr,
        &concise_type));
    EXPECT_EQ(SQL_DATETIME, descriptor_type);
    EXPECT_EQ(SQL_TYPE_DATE, concise_type);
}

TEST_F(MetadataIntegrationTest, ColumnMetadataStateErrorsPreserveOutputs) {
    const auto expect_state = [this](const char* expected) {
        SQLCHAR state[6]{};
        ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
            SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0,
            nullptr));
        EXPECT_STREQ(expected, reinterpret_cast<char*>(state));
    };

    char name[16]{"unchanged"};
    SQLSMALLINT name_length = 71;
    SQLSMALLINT data_type = 72;
    SQLULEN column_size = 73;
    SQLSMALLINT scale = 74;
    SQLSMALLINT nullable = 75;
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    expect_state("HY010");
    EXPECT_STREQ("unchanged", name);
    EXPECT_EQ(71, name_length);
    EXPECT_EQ(72, data_type);
    EXPECT_EQ(73u, column_size);
    EXPECT_EQ(74, scale);
    EXPECT_EQ(75, nullable);

    SQLLEN numeric = 76;
    SQLSMALLINT string_length = 77;
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, SQL_DESC_TYPE, name, sizeof(name), &string_length,
        &numeric));
    expect_state("HY010");
    EXPECT_EQ(76, numeric);
    EXPECT_EQ(77, string_length);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"UPDATE pg_catalog.pg_class SET relname = relname "
                         "WHERE false",
        SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    expect_state("07005");
    EXPECT_STREQ("unchanged", name);
    EXPECT_EQ(71, name_length);

    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, SQL_DESC_NAME, name, sizeof(name), &string_length,
        &numeric));
    expect_state("07005");
    EXPECT_EQ(76, numeric);
    EXPECT_EQ(77, string_length);

    numeric = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttribute(
        hstmt, 999, SQL_DESC_COUNT, nullptr, 0, nullptr, &numeric));
    EXPECT_EQ(0, numeric);

    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    expect_state("07005");
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, SQL_DESC_NAME, name, sizeof(name), &string_length,
        &numeric));
    expect_state("07005");

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT 1 AS value", SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 0, reinterpret_cast<SQLCHAR*>(name), sizeof(name),
        &name_length, &data_type, &column_size, &scale, &nullable));
    expect_state("07009");
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 2, SQL_DESC_TYPE, nullptr, 0, nullptr, &numeric));
    expect_state("07009");

    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, 9999, nullptr, 0, nullptr, &numeric));
    expect_state("HY091");
    EXPECT_EQ(SQL_ERROR, SQLColAttribute(
        hstmt, 1, SQL_DESC_BASE_TABLE_NAME, name, sizeof(name),
        &string_length, nullptr));
    expect_state("HYC00");

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT 1 AS direct_value", SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    EXPECT_EQ(SQL_ERROR, SQLDescribeCol(
        hstmt, 1, reinterpret_cast<SQLCHAR*>(name), sizeof(name), nullptr,
        nullptr, nullptr, nullptr, nullptr));
    expect_state("HY010");
}

TEST_F(MetadataIntegrationTest, DescribesPreparedWideColumnNames) {
    const std::string expected =
        "pr\xc3\xa9par\xc3\xa9\xe4\xb8\x96\xe7\x95\x8c";
    auto query = rs::odbc::utf8_to_wide(
        "SELECT 1 AS \"pr\xc3\xa9par\xc3\xa9\xe4\xb8\x96\xe7\x95\x8c\"");
    ASSERT_TRUE(query.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLPrepareW(
        hstmt, query->data(), static_cast<SQLINTEGER>(query->size())));

    SQLWCHAR name[32]{};
    SQLSMALLINT name_units = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeColW(
        hstmt, 1, name, 32, &name_units, nullptr, nullptr, nullptr,
        nullptr));
    const auto described = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(name, static_cast<std::size_t>(name_units)));
    ASSERT_TRUE(described.has_value());
    EXPECT_EQ(expected, *described);

    SQLWCHAR label[32]{};
    SQLSMALLINT label_bytes = -1;
    SQLLEN ignored_numeric = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLColAttributeW(
        hstmt, 1, SQL_DESC_LABEL, label, sizeof(label), &label_bytes,
        &ignored_numeric));
    const auto described_label = rs::odbc::wide_to_utf8(
        std::span<const SQLWCHAR>(
            label, static_cast<std::size_t>(label_bytes) /
                       sizeof(SQLWCHAR)));
    ASSERT_TRUE(described_label.has_value());
    EXPECT_EQ(expected, *described_label);
    EXPECT_EQ(91, ignored_numeric);

    label[0] = static_cast<SQLWCHAR>('x');
    label_bytes = 92;
    EXPECT_EQ(SQL_ERROR, SQLColAttributeW(
        hstmt, 1, SQL_DESC_LABEL, label,
        static_cast<SQLSMALLINT>(sizeof(SQLWCHAR) + 1), &label_bytes,
        nullptr));
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("HY090", reinterpret_cast<char*>(state));
    EXPECT_EQ(static_cast<SQLWCHAR>('x'), label[0]);
    EXPECT_EQ(92, label_bytes);
}

TEST_F(MetadataIntegrationTest, RefreshesPreparedShapeAfterIpdTypeChange) {
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ? AS typed_value", SQL_NTS));
    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    const auto number = [](SQLSMALLINT value) {
        return reinterpret_cast<SQLPOINTER>(
            static_cast<std::uintptr_t>(value));
    };

    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_CONCISE_TYPE,
        number(SQL_INTEGER), 0));
    SQLSMALLINT column_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    SQLSMALLINT data_type = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &data_type,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, data_type);

    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
        implementation, 1, SQL_DESC_CONCISE_TYPE,
        number(SQL_VARCHAR), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &data_type,
        nullptr, nullptr, nullptr));
    EXPECT_EQ(SQL_VARCHAR, data_type);
}

TEST_F(MetadataIntegrationTest, ResultShapeAndRowCountFollowStatementState) {
    SQLSMALLINT column_count = 71;
    SQLLEN row_count = 72;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(71, column_count);
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(72, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE result_state(value integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(0, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"INSERT INTO result_state VALUES (1), (2), (3)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT value FROM result_state ORDER BY value",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    column_count = 73;
    row_count = 74;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(73, column_count);
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(74, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"UPDATE result_state SET value = value + 10",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    row_count = 75;
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(75, row_count);
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(3, row_count);

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT FROM missing syntax", SQL_NTS));
    column_count = 76;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(76, column_count);
    SQLCHAR state[6]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(
        SQL_HANDLE_STMT, hstmt, 1, state, nullptr, nullptr, 0, nullptr));
    EXPECT_STREQ("42000", reinterpret_cast<char*>(state));
    row_count = 77;
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(77, row_count);

    EXPECT_EQ(SQL_ERROR, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT * FROM definitely_missing_odbcpp_table",
        SQL_NTS));
    column_count = 78;
    row_count = 79;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(78, column_count);
    EXPECT_EQ(SQL_ERROR, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(79, row_count);
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

TEST_F(MetadataIntegrationTest, LimitsRowsAndCompletesResultSequence) {
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(
        hstmt, SQL_ATTR_MAX_ROWS,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(2)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT value FROM generate_series(1, 3) value",
        SQL_NTS));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));

    SQLSMALLINT column_count = -1;
    EXPECT_EQ(SQL_ERROR, SQLNumResultCols(hstmt, &column_count));
}

TEST_F(MetadataIntegrationTest, FetchScrollSupportsNextRows) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT value FROM generate_series(1, 2) value",
        SQL_NTS));
    EXPECT_EQ(SQL_SUCCESS, SQLFetchScroll(hstmt, SQL_FETCH_NEXT, 0));
    EXPECT_EQ(SQL_SUCCESS, SQLFetchScroll(hstmt, SQL_FETCH_NEXT, 0));
    EXPECT_EQ(SQL_NO_DATA, SQLFetchScroll(hstmt, SQL_FETCH_NEXT, 0));
    EXPECT_EQ(SQL_ERROR, SQLFetchScroll(hstmt, SQL_FETCH_FIRST, 0));
}

TEST_F(MetadataIntegrationTest, TraversesMultiplePostgreSQLResults) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_more_results(value int)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"INSERT INTO odbcpp_more_results VALUES (1)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT 11 AS first_value; "
                  "UPDATE odbcpp_more_results SET value = 2; "
                  "SELECT 'done'::text AS final_value",
        SQL_NTS));

    SQLSMALLINT column_count = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(1, column_count);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    SQLINTEGER first_value = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_SLONG, &first_value, 0, nullptr));
    EXPECT_EQ(11, first_value);

    ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &column_count));
    EXPECT_EQ(0, column_count);
    SQLLEN affected_rows = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &affected_rows));
    EXPECT_EQ(1, affected_rows);

    ASSERT_EQ(SQL_SUCCESS, SQLMoreResults(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char final_value[16]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, final_value, sizeof(final_value), nullptr));
    EXPECT_STREQ("done", final_value);
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));
}

TEST_F(MetadataIntegrationTest, ClosingCursorDiscardsPendingResults) {
    constexpr auto batch =
        "SELECT 1 AS first_value; SELECT 2 AS second_value";

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)batch, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)batch, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(hstmt, SQL_CLOSE));
    EXPECT_EQ(SQL_NO_DATA, SQLMoreResults(hstmt));
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
    EXPECT_STREQ("HY010", (char*)sqlstate);
    
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
