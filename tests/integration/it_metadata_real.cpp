#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include "tests/test_connection_config.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <string>

namespace {

std::string diagnostic_state(SQLSMALLINT handle_type, SQLHANDLE handle) {
    SQLCHAR state[6]{};
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetDiagRec(handle_type, handle, 1, state, nullptr, nullptr,
                            0, nullptr));
    return reinterpret_cast<const char*>(state);
}

std::optional<SQLINTEGER> integer_cell(SQLHSTMT statement,
                                       SQLUSMALLINT column) {
    SQLINTEGER value = 0;
    SQLLEN indicator = 0;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetData(statement, column, SQL_C_SLONG, &value,
                         sizeof(value), &indicator));
    if (indicator == SQL_NULL_DATA) return std::nullopt;
    return value;
}

std::optional<std::string> text_cell(SQLHSTMT statement,
                                     SQLUSMALLINT column) {
    std::array<char, 64> value{};
    SQLLEN indicator = 0;
    EXPECT_EQ(SQL_SUCCESS,
              SQLGetData(statement, column, SQL_C_CHAR, value.data(),
                         value.size(), &indicator));
    if (indicator == SQL_NULL_DATA) return std::nullopt;
    return std::string(value.data());
}

}  // namespace

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
    EXPECT_EQ(SQL_ERROR, SQLGetTypeInfo(hstmt, 12345));
    EXPECT_EQ("HY004", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(hstmt, SQL_WVARCHAR));
    SQLSMALLINT columns = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLNumResultCols(hstmt, &columns));
    EXPECT_EQ(19, columns);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    EXPECT_EQ(SQL_ERROR, SQLGetTypeInfoW(hstmt, SQL_INTEGER));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    struct ExpectedType {
        const char* name;
        SQLSMALLINT data_type;
        SQLINTEGER column_size;
    };
    constexpr std::array<ExpectedType, 18> expected_types{{
        {"boolean", SQL_BIT, 1},
        {"bigint", SQL_BIGINT, 19},
        {"bytea", SQL_VARBINARY, 1073741824},
        {"text", SQL_LONGVARCHAR, 1073741824},
        {"char", SQL_CHAR, 10485760},
        {"numeric", SQL_NUMERIC, 1000},
        {"decimal", SQL_DECIMAL, 1000},
        {"integer", SQL_INTEGER, 10},
        {"smallint", SQL_SMALLINT, 5},
        {"real", SQL_REAL, 7},
        {"double precision", SQL_DOUBLE, 15},
        {"date", SQL_DATE, 10},
        {"time", SQL_TIME, 15},
        {"timestamp", SQL_TIMESTAMP, 26},
        {"varchar", SQL_VARCHAR, 10485760},
        {"date", SQL_TYPE_DATE, 10},
        {"time", SQL_TYPE_TIME, 15},
        {"timestamp", SQL_TYPE_TIMESTAMP, 26},
    }};

    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfo(hstmt, SQL_ALL_TYPES));
    SQLLEN row_count = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLRowCount(hstmt, &row_count));
    EXPECT_EQ(static_cast<SQLLEN>(expected_types.size()), row_count);

    for (std::size_t index = 0; index < expected_types.size(); ++index) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        if (index == 0) {
            EXPECT_EQ(SQL_ERROR, SQLGetTypeInfo(hstmt, SQL_INTEGER));
            EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
        }
        char type_name[32]{};
        SQLSMALLINT data_type = 0;
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(
            hstmt, 1, SQL_C_CHAR, type_name, sizeof(type_name), nullptr));
        ASSERT_EQ(SQL_SUCCESS, SQLGetData(
            hstmt, 2, SQL_C_SSHORT, &data_type, 0, nullptr));
        EXPECT_STREQ(expected_types[index].name, type_name);
        EXPECT_EQ(expected_types[index].data_type, data_type);

        const auto type = expected_types[index].data_type;
        const bool character_type = type == SQL_CHAR ||
            type == SQL_VARCHAR || type == SQL_LONGVARCHAR;
        const bool quoted_type = character_type || type == SQL_VARBINARY ||
            type == SQL_DATE || type == SQL_TIME || type == SQL_TIMESTAMP ||
            type == SQL_TYPE_DATE || type == SQL_TYPE_TIME ||
            type == SQL_TYPE_TIMESTAMP;
        const bool numeric_type = type == SQL_BIGINT || type == SQL_NUMERIC ||
            type == SQL_DECIMAL || type == SQL_INTEGER ||
            type == SQL_SMALLINT || type == SQL_REAL || type == SQL_DOUBLE;
        const bool exact_numeric_type = type == SQL_BIGINT ||
            type == SQL_NUMERIC || type == SQL_DECIMAL ||
            type == SQL_INTEGER || type == SQL_SMALLINT;
        const bool datetime_type = type == SQL_DATE || type == SQL_TIME ||
            type == SQL_TIMESTAMP || type == SQL_TYPE_DATE ||
            type == SQL_TYPE_TIME || type == SQL_TYPE_TIMESTAMP;
        const bool time_type = type == SQL_TIME || type == SQL_TIMESTAMP ||
            type == SQL_TYPE_TIME || type == SQL_TYPE_TIMESTAMP;

        EXPECT_EQ(std::optional<SQLINTEGER>(expected_types[index].column_size),
                  integer_cell(hstmt, 3));
        EXPECT_EQ(quoted_type ? std::optional<std::string>("'") : std::nullopt,
                  text_cell(hstmt, 4));
        EXPECT_EQ(quoted_type ? std::optional<std::string>("'") : std::nullopt,
                  text_cell(hstmt, 5));
        if (type == SQL_CHAR || type == SQL_VARCHAR) {
            EXPECT_EQ(std::optional<std::string>("length"), text_cell(hstmt, 6));
        } else if (type == SQL_NUMERIC || type == SQL_DECIMAL) {
            EXPECT_EQ(std::optional<std::string>("precision,scale"),
                      text_cell(hstmt, 6));
        } else if (time_type) {
            EXPECT_EQ(std::optional<std::string>("precision"),
                      text_cell(hstmt, 6));
        } else {
            EXPECT_EQ(std::nullopt, text_cell(hstmt, 6));
        }
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NULLABLE),
                  integer_cell(hstmt, 7));
        EXPECT_EQ(std::optional<SQLINTEGER>(
                      character_type ? SQL_TRUE : SQL_FALSE),
                  integer_cell(hstmt, 8));
        EXPECT_EQ(std::optional<SQLINTEGER>(
                      character_type ? SQL_SEARCHABLE : SQL_PRED_BASIC),
                  integer_cell(hstmt, 9));
        EXPECT_EQ(numeric_type ? std::optional<SQLINTEGER>(SQL_FALSE)
                               : std::nullopt,
                  integer_cell(hstmt, 10));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_FALSE),
                  integer_cell(hstmt, 11));
        EXPECT_EQ(numeric_type ? std::optional<SQLINTEGER>(SQL_FALSE)
                               : std::nullopt,
                  integer_cell(hstmt, 12));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 13));
        EXPECT_EQ((exact_numeric_type || time_type)
                      ? std::optional<SQLINTEGER>(0)
                      : std::nullopt,
                  integer_cell(hstmt, 14));
        const auto maximum_scale = type == SQL_NUMERIC || type == SQL_DECIMAL
            ? std::optional<SQLINTEGER>(1000)
            : (time_type ? std::optional<SQLINTEGER>(6)
                         : (exact_numeric_type
                                ? std::optional<SQLINTEGER>(0)
                                : std::nullopt));
        EXPECT_EQ(maximum_scale, integer_cell(hstmt, 15));
        EXPECT_EQ(std::optional<SQLINTEGER>(
                      datetime_type ? SQL_DATETIME : type),
                  integer_cell(hstmt, 16));
        const auto datetime_subtype =
            type == SQL_DATE || type == SQL_TYPE_DATE
                ? std::optional<SQLINTEGER>(SQL_CODE_DATE)
                : (type == SQL_TIME || type == SQL_TYPE_TIME
                       ? std::optional<SQLINTEGER>(SQL_CODE_TIME)
                       : (type == SQL_TIMESTAMP || type == SQL_TYPE_TIMESTAMP
                              ? std::optional<SQLINTEGER>(SQL_CODE_TIMESTAMP)
                              : std::nullopt));
        EXPECT_EQ(datetime_subtype, integer_cell(hstmt, 17));
        EXPECT_EQ(exact_numeric_type
                      ? std::optional<SQLINTEGER>(10)
                      : ((type == SQL_REAL || type == SQL_DOUBLE)
                             ? std::optional<SQLINTEGER>(2)
                             : std::nullopt),
                  integer_cell(hstmt, 18));
        EXPECT_EQ(std::nullopt, integer_cell(hstmt, 19));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLGetTypeInfoW(hstmt, SQL_INTEGER));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    char type_name[32]{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(
        hstmt, 1, SQL_C_CHAR, type_name, sizeof(type_name), nullptr));
    EXPECT_STREQ("integer", type_name);
}

TEST_F(MetadataIntegrationTest, ListsPostgreSQLTables) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_catalog_test(value int)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_catalog_test";
    SQLCHAR table_type[] = "LOCAL TEMPORARY";
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
    EXPECT_STREQ("LOCAL TEMPORARY", returned_type);
}

TEST_F(MetadataIntegrationTest, TablePatternsTypesAndEnumerationsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_table_a_b(value int)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"CREATE TEMP TABLE odbcpp_table_axb(value int)",
        SQL_NTS));

    SQLCHAR wildcard_pattern[] = "odbcpp_table_a_b";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, wildcard_pattern, SQL_NTS,
        nullptr, 0));
    for (const char* expected : {"odbcpp_table_a_b", "odbcpp_table_axb"}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>(expected), text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<std::string>("LOCAL TEMPORARY"),
                  text_cell(hstmt, 4));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR escaped_pattern[] = "odbcpp\\_table\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, escaped_pattern, SQL_NTS,
        nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_table_a_b"),
              text_cell(hstmt, 3));
    EXPECT_EQ(SQL_ERROR, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, escaped_pattern, SQL_NTS,
        nullptr, 0));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR local_temporary[] = "'local temporary'";
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, nullptr, 0, nullptr, 0, wildcard_pattern, SQL_NTS,
        local_temporary, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("LOCAL TEMPORARY"),
              text_cell(hstmt, 4));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR all[]{'%', 0};
    constexpr std::array<const char*, 5> expected_types{
        "FOREIGN TABLE", "LOCAL TEMPORARY", "SYSTEM TABLE", "TABLE", "VIEW"};
    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, empty, SQL_NTS, empty, SQL_NTS, empty, SQL_NTS,
        nullptr, 0));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, all, SQL_NTS, empty, SQL_NTS, empty, SQL_NTS,
        empty, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_TRUE(text_cell(hstmt, 1).has_value());
    EXPECT_EQ(std::nullopt, text_cell(hstmt, 2));
    EXPECT_EQ(std::nullopt, text_cell(hstmt, 3));
    EXPECT_EQ(std::nullopt, text_cell(hstmt, 4));
    EXPECT_EQ(std::nullopt, text_cell(hstmt, 5));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLWCHAR wide_empty[]{0};
    SQLWCHAR wide_all[]{'%', 0};
    ASSERT_EQ(SQL_SUCCESS, SQLTablesW(
        hstmt, wide_empty, SQL_NTS, wide_empty, SQL_NTS,
        wide_empty, SQL_NTS, wide_all, SQL_NTS));
    for (const auto* expected : expected_types) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>(expected), text_cell(hstmt, 4));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, empty, SQL_NTS, all, SQL_NTS, empty, SQL_NTS,
        empty, SQL_NTS));
    bool found_information_schema = false;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 1));
        const auto schema = text_cell(hstmt, 2);
        found_information_schema = found_information_schema ||
            schema == std::optional<std::string>("information_schema");
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 3));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 4));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 5));
    }
    EXPECT_TRUE(found_information_schema);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, empty, SQL_NTS, empty, SQL_NTS, empty, SQL_NTS,
        all, SQL_NTS));
    for (const auto* expected : expected_types) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 1));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 2));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<std::string>(expected), text_cell(hstmt, 4));
        EXPECT_EQ(std::nullopt, text_cell(hstmt, 5));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLTables(
        hstmt, all, SQL_NTS, nullptr, 0, nullptr, 0,
        nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_TRUE(text_cell(hstmt, 3).has_value());
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

TEST_F(MetadataIntegrationTest, ColumnPatternsAndCatalogArgumentsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_columns_a_b("
                  "column_a_b integer, column_axb integer)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_columns_axb(marker integer)",
        SQL_NTS));

    SQLCHAR table_pattern[] = "odbcpp_columns_a_b";
    SQLCHAR column_pattern[] = "column_a_b";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_pattern, SQL_NTS,
        column_pattern, SQL_NTS));
    for (const char* expected : {"column_a_b", "column_axb"}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>("odbcpp_columns_a_b"),
                  text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<std::string>(expected), text_cell(hstmt, 4));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR escaped_table[] = "odbcpp\\_columns\\_a\\_b";
    SQLCHAR escaped_column[] = "column\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, escaped_table, SQL_NTS,
        escaped_column, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_columns_a_b"),
              text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("column_a_b"),
              text_cell(hstmt, 4));
    EXPECT_EQ(SQL_ERROR, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, escaped_table, SQL_NTS,
        escaped_column, SQL_NTS));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR exact_table[] = "odbcpp_columns_a_b";
    SQLCHAR exact_column[] = "column_a_b";
    auto expect_no_columns = [&](SQLCHAR* catalog, SQLSMALLINT catalog_length,
                                 SQLCHAR* schema, SQLSMALLINT schema_length,
                                 SQLCHAR* table, SQLSMALLINT table_length,
                                 SQLCHAR* column, SQLSMALLINT column_length) {
        ASSERT_EQ(SQL_SUCCESS, SQLColumns(
            hstmt, catalog, catalog_length, schema, schema_length,
            table, table_length, column, column_length));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_columns(empty, SQL_NTS, nullptr, 0, exact_table, SQL_NTS,
                      exact_column, SQL_NTS);
    expect_no_columns(nullptr, 0, empty, SQL_NTS, exact_table, SQL_NTS,
                      exact_column, SQL_NTS);
    expect_no_columns(nullptr, 0, nullptr, 0, empty, SQL_NTS,
                      exact_column, SQL_NTS);
    expect_no_columns(nullptr, 0, nullptr, 0, exact_table, SQL_NTS,
                      empty, SQL_NTS);

    SQLCHAR percent[] = "%";
    expect_no_columns(percent, SQL_NTS, nullptr, 0, exact_table, SQL_NTS,
                      exact_column, SQL_NTS);

    std::array<SQLCHAR, 128> catalog{};
    SQLSMALLINT catalog_length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetInfo(
        hdbc, SQL_DATABASE_NAME, catalog.data(),
        static_cast<SQLSMALLINT>(catalog.size()),
        &catalog_length));
    ASSERT_GT(catalog_length, 0);
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, catalog.data(), catalog_length, nullptr, 0,
        escaped_table, SQL_NTS, escaped_column, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>(
                  reinterpret_cast<const char*>(catalog.data())),
              text_cell(hstmt, 1));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp\\_columns\\_a\\_b");
    auto wide_column = rs::odbc::utf8_to_wide("column\\_a\\_b");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_columns_a_b"),
              text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("column_a_b"),
              text_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, CatalogTypeNamesPreservePostgreSQLTypes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_domain AS integer",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_catalog_type_test("
                  "uuid_key uuid NOT NULL, document jsonb, tags integer[], "
                  "domain_key pg_temp.odbcpp_catalog_domain NOT NULL, "
                  "numeric_key numeric(12,3), memo text, "
                  "PRIMARY KEY(uuid_key, domain_key))",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_catalog_type_test";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));

    struct ExpectedColumn {
        const char* name;
        SQLINTEGER type;
        const char* type_name;
    };
    constexpr std::array<ExpectedColumn, 6> expected_columns{{
        {"uuid_key", SQL_VARCHAR, "uuid"},
        {"document", SQL_VARCHAR, "jsonb"},
        {"tags", SQL_VARCHAR, "integer[]"},
        {"domain_key", SQL_INTEGER, "odbcpp_catalog_domain"},
        {"numeric_key", SQL_NUMERIC, "numeric"},
        {"memo", SQL_LONGVARCHAR, "text"},
    }};
    for (const auto& expected : expected_columns) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>(expected.name), text_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected.type), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<std::string>(expected.type_name),
                  text_cell(hstmt, 6));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected.type),
                  integer_cell(hstmt, 14));
        if (std::strcmp(expected.name, "uuid_key") == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 7));
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 8));
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 16));
        } else if (std::strcmp(expected.name, "domain_key") == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 7));
            EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 8));
        } else if (std::strcmp(expected.name, "numeric_key") == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(12), integer_cell(hstmt, 7));
            EXPECT_EQ(std::optional<SQLINTEGER>(3), integer_cell(hstmt, 9));
        } else if (std::strcmp(expected.name, "memo") == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(1073741824),
                      integer_cell(hstmt, 16));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp_catalog_type_test");
    auto wide_column = rs::odbc::utf8_to_wide("domain_key");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<std::string>("odbcpp_catalog_domain"),
              text_cell(hstmt, 6));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("uuid_key"), text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_VARCHAR), integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("uuid"), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 6));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("domain_key"), text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("odbcpp_catalog_domain"),
              text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 6));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumnsW(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("uuid"), text_cell(hstmt, 4));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_catalog_domain"),
              text_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_catalog_type_function("
                  "id uuid, key pg_temp.odbcpp_catalog_domain, "
                  "tags integer[]) RETURNS jsonb LANGUAGE SQL "
                  "AS 'SELECT ''{}''::jsonb'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp\\_catalog\\_type\\_function";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    constexpr std::array<SQLINTEGER, 4> expected_routine_types{
        SQL_VARCHAR, SQL_INTEGER, SQL_VARCHAR, SQL_VARCHAR};
    for (std::size_t i = 0; i < expected_routine_types.size(); ++i) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_routine_types[i]),
                  integer_cell(hstmt, 6));
        if (i == 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 8));
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 9));
            EXPECT_EQ(std::optional<SQLINTEGER>(36), integer_cell(hstmt, 17));
        } else if (i == 1) {
            const auto type_name = text_cell(hstmt, 7);
            ASSERT_TRUE(type_name.has_value());
            EXPECT_NE(std::string::npos,
                      type_name->find("odbcpp_catalog_domain"));
            EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 8));
            EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 9));
            EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER),
                      integer_cell(hstmt, 15));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT uuid_key FROM odbcpp_catalog_type_test",
        SQL_NTS));
    SQLSMALLINT result_type = 0;
    SQLULEN result_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &result_type, &result_size,
        nullptr, nullptr));
    EXPECT_EQ(SQL_VARCHAR, result_type);
    EXPECT_EQ(36u, result_size);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_procedure = rs::odbc::utf8_to_wide(
        "odbcpp\\_catalog\\_type\\_function");
    ASSERT_TRUE(wide_procedure.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_procedure->data(),
        static_cast<SQLSMALLINT>(wide_procedure->size()),
        nullptr, 0));
    for (const auto expected_type : expected_routine_types) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_type),
                  integer_cell(hstmt, 6));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, NestedDomainsKeepBaseCatalogMetadata) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_inner_domain AS integer",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_outer_domain "
                  "AS pg_temp.odbcpp_inner_domain",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_nested_domain_test("
                  "id pg_temp.odbcpp_outer_domain NOT NULL PRIMARY KEY)",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_nested_domain_test";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<std::string>("odbcpp_outer_domain"),
              text_cell(hstmt, 6));
    EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 7));
    EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 8));
    EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 9));
    EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 10));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 14));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp_nested_domain_test");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<std::string>("odbcpp_outer_domain"),
              text_cell(hstmt, 6));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("odbcpp_outer_domain"),
              text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 6));
    EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 7));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_nested_domain_func("
                  "value pg_temp.odbcpp_outer_domain) "
                  "RETURNS pg_temp.odbcpp_outer_domain "
                  "LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp\\_nested\\_domain\\_func";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    for (SQLINTEGER column_type : {1, 5}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(column_type), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 6));
        EXPECT_EQ(std::optional<std::string>("odbcpp_outer_domain"),
                  text_cell(hstmt, 7));
        EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 8));
        EXPECT_EQ(std::optional<SQLINTEGER>(4), integer_cell(hstmt, 9));
        EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 10));
        EXPECT_EQ(std::optional<SQLINTEGER>(10), integer_cell(hstmt, 11));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 15));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt, (SQLCHAR*)"SELECT id FROM odbcpp_nested_domain_test",
        SQL_NTS));
    SQLSMALLINT result_type = 0;
    SQLULEN result_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &result_type, &result_size,
        nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, result_type);
    EXPECT_EQ(10u, result_size);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt, (SQLCHAR*)"SELECT ?::pg_temp.odbcpp_outer_domain",
        SQL_NTS));
    SQLSMALLINT parameter_type = 0;
    SQLULEN parameter_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &parameter_type, &parameter_size, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, parameter_type);
    EXPECT_EQ(10u, parameter_size);
    result_type = 0;
    result_size = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &result_type, &result_size,
        nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, result_type);
    EXPECT_EQ(10u, result_size);

    SQLINTEGER input = 37;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(
        hstmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER,
        0, 0, &input, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
        hstmt, 1, &parameter_type, &parameter_size, nullptr, nullptr));
    EXPECT_EQ(SQL_INTEGER, parameter_type);
    EXPECT_EQ(10u, parameter_size);
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(input), integer_cell(hstmt, 1));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, NestedDomainParameterMetadataKeepsTypmods) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_length_domain "
                  "AS varchar(13)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_nested_length_domain "
                  "AS pg_temp.odbcpp_length_domain",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_scale_domain "
                  "AS numeric(8,3)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(
        hstmt,
        (SQLCHAR*)"SELECT ?::pg_temp.odbcpp_nested_length_domain, "
                  "?::pg_temp.odbcpp_nested_length_domain, "
                  "?::pg_temp.odbcpp_scale_domain",
        SQL_NTS));

    for (SQLUSMALLINT parameter :
         std::array<SQLUSMALLINT, 3>{1, 2, 3}) {
        SQLSMALLINT data_type = 0;
        SQLULEN size = 0;
        SQLSMALLINT scale = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeParam(
            hstmt, parameter, &data_type, &size, &scale, nullptr));
        if (parameter < 3) {
            EXPECT_EQ(SQL_VARCHAR, data_type);
            EXPECT_EQ(13u, size);
            EXPECT_EQ(0, scale);
        } else {
            EXPECT_EQ(SQL_NUMERIC, data_type);
            EXPECT_EQ(8u, size);
            EXPECT_EQ(3, scale);
        }
    }

    SQLHDESC implementation = SQL_NULL_HDESC;
    ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(
        hstmt, SQL_ATTR_IMP_PARAM_DESC, &implementation, 0, nullptr));
    SQLULEN length = 0;
    SQLSMALLINT precision = 0;
    SQLSMALLINT scale = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 1, SQL_DESC_LENGTH, &length, 0, nullptr));
    EXPECT_EQ(13u, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 3, SQL_DESC_PRECISION, &precision, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(
        implementation, 3, SQL_DESC_SCALE, &scale, 0, nullptr));
    EXPECT_EQ(8, precision);
    EXPECT_EQ(3, scale);

    SQLSMALLINT result_type = 0;
    SQLULEN result_size = 0;
    SQLSMALLINT result_scale = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 1, nullptr, 0, nullptr, &result_type, &result_size,
        &result_scale, nullptr));
    EXPECT_EQ(SQL_VARCHAR, result_type);
    EXPECT_EQ(13u, result_size);
    ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
        hstmt, 3, nullptr, 0, nullptr, &result_type, &result_size,
        &result_scale, nullptr));
    EXPECT_EQ(SQL_NUMERIC, result_type);
    EXPECT_EQ(8u, result_size);
    EXPECT_EQ(3, result_scale);
}

TEST_F(MetadataIntegrationTest, NestedDomainCatalogDimensionsMatchBaseColumns) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_length "
                  "AS varchar(13)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_nested_length "
                  "AS pg_temp.odbcpp_catalog_length",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_scale "
                  "AS numeric(8,3)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE DOMAIN pg_temp.odbcpp_catalog_nested_scale "
                  "AS pg_temp.odbcpp_catalog_scale",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_catalog_domain_dimensions("
                  "base_v varchar(13), nested_v "
                  "pg_temp.odbcpp_catalog_nested_length, "
                  "base_n numeric(8,3), domain_n "
                  "pg_temp.odbcpp_catalog_nested_scale, "
                  "PRIMARY KEY(nested_v, domain_n))",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp_catalog_domain_dimensions";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));
    constexpr std::array<SQLUSMALLINT, 6> fields{5, 7, 8, 9, 10, 16};
    std::array<std::optional<SQLINTEGER>, fields.size()> base{};
    std::array<std::optional<SQLINTEGER>, fields.size()> nested_character{};
    std::array<std::optional<SQLINTEGER>, fields.size()> nested_numeric{};
    for (int pair = 0; pair < 2; ++pair) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < fields.size(); ++index) {
            base[index] = integer_cell(hstmt, fields[index]);
        }
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < fields.size(); ++index) {
            const auto nested = integer_cell(hstmt, fields[index]);
            EXPECT_EQ(base[index], nested)
                << "pair=" << pair << " field=" << fields[index];
            if (pair == 0) nested_character[index] = nested;
            else nested_numeric[index] = nested;
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_table = rs::odbc::utf8_to_wide("odbcpp_catalog_domain_dimensions");
    auto wide_column = rs::odbc::utf8_to_wide("nested_v");
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    for (std::size_t index = 0; index < fields.size(); ++index) {
        EXPECT_EQ(nested_character[index], integer_cell(hstmt, fields[index]));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    constexpr std::array<SQLUSMALLINT, 4> special_fields{3, 5, 6, 7};
    for (const auto& expected : {nested_character, nested_numeric}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < special_fields.size(); ++index) {
            EXPECT_EQ(expected[index],
                      integer_cell(hstmt, special_fields[index]));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_catalog_domain_func("
                  "v pg_temp.odbcpp_catalog_nested_length, "
                  "n pg_temp.odbcpp_catalog_nested_scale) "
                  "RETURNS pg_temp.odbcpp_catalog_nested_scale "
                  "LANGUAGE SQL AS 'SELECT $2'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp\\_catalog\\_domain\\_func";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    constexpr std::array<SQLUSMALLINT, 6> procedure_fields{
        6, 8, 9, 10, 11, 17};
    for (const auto& expected :
         {nested_character, nested_numeric, nested_numeric}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < procedure_fields.size(); ++index) {
            EXPECT_EQ(expected[index],
                      integer_cell(hstmt, procedure_fields[index]));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumnsW(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        SQL_SCOPE_CURROW, SQL_NO_NULLS));
    for (const auto& expected : {nested_character, nested_numeric}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < special_fields.size(); ++index) {
            EXPECT_EQ(expected[index],
                      integer_cell(hstmt, special_fields[index]));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_procedure = rs::odbc::utf8_to_wide(
        "odbcpp\\_catalog\\_domain\\_func");
    ASSERT_TRUE(wide_procedure.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumnsW(
        hstmt, nullptr, 0, nullptr, 0,
        wide_procedure->data(),
        static_cast<SQLSMALLINT>(wide_procedure->size()),
        nullptr, 0));
    for (const auto& expected :
         {nested_character, nested_numeric, nested_numeric}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        for (std::size_t index = 0; index < procedure_fields.size(); ++index) {
            EXPECT_EQ(expected[index],
                      integer_cell(hstmt, procedure_fields[index]));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, TemporalMetadataUsesDeclaredPrecision) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_temporal_metadata("
                  "d date, t0 time(0), t2 time(2), ts0 timestamp(0), "
                  "ts3 timestamp(3) NOT NULL, tz0 timetz(0), "
                  "ttz3 timestamptz(3), PRIMARY KEY(ts3))",
        SQL_NTS));
    SQLCHAR table_name[] = "odbcpp_temporal_metadata";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        nullptr, 0));
    struct ExpectedColumn {
        const char* name;
        SQLSMALLINT type;
        SQLINTEGER size;
        SQLINTEGER transfer_size;
        SQLINTEGER scale;
    };
    constexpr std::array<ExpectedColumn, 7> expected{{
        {"d", SQL_TYPE_DATE, 10, 6, -1},
        {"t0", SQL_TYPE_TIME, 8, 6, 0},
        {"t2", SQL_TYPE_TIME, 11, 6, 2},
        {"ts0", SQL_TYPE_TIMESTAMP, 19, 16, 0},
        {"ts3", SQL_TYPE_TIMESTAMP, 23, 16, 3},
        {"tz0", SQL_TYPE_TIME, 14, 6, 0},
        {"ttz3", SQL_TYPE_TIMESTAMP, 29, 16, 3},
    }};
    for (const auto& column : expected) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<std::string>(column.name), text_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(column.type), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(column.size), integer_cell(hstmt, 7));
        EXPECT_EQ(std::optional<SQLINTEGER>(column.transfer_size),
                  integer_cell(hstmt, 8));
        if (column.scale >= 0) {
            EXPECT_EQ(std::optional<SQLINTEGER>(column.scale),
                      integer_cell(hstmt, 9));
        }
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("ts3"), text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_TYPE_TIMESTAMP),
              integer_cell(hstmt, 3));
    EXPECT_EQ(std::optional<SQLINTEGER>(23), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(16), integer_cell(hstmt, 6));
    EXPECT_EQ(std::optional<SQLINTEGER>(3), integer_cell(hstmt, 7));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"SELECT d, t0, t2, ts0, ts3, tz0, ttz3 "
                  "FROM odbcpp_temporal_metadata",
        SQL_NTS));
    for (SQLUSMALLINT i = 1; i <= expected.size(); ++i) {
        SQLSMALLINT type = 0;
        SQLULEN size = 0;
        SQLSMALLINT scale = -1;
        ASSERT_EQ(SQL_SUCCESS, SQLDescribeCol(
            hstmt, i, nullptr, 0, nullptr, &type, &size, &scale, nullptr));
        EXPECT_EQ(expected[i - 1].type, type);
        EXPECT_EQ(static_cast<SQLULEN>(expected[i - 1].size), size);
        if (expected[i - 1].scale >= 0) {
            EXPECT_EQ(expected[i - 1].scale, scale);
        }
    }
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_temporal_metadata_func("
                  "d date, t time, ts timestamp) RETURNS integer "
                  "LANGUAGE SQL AS 'SELECT 1'",
        SQL_NTS));
    SQLCHAR procedure_name[] = "odbcpp\\_temporal\\_metadata\\_func";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, procedure_name, SQL_NTS,
        nullptr, 0));
    for (const SQLINTEGER transfer_size : {6, 6, 16}) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(transfer_size), integer_cell(hstmt, 9));
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
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

TEST_F(MetadataIntegrationTest, PrimaryKeyArgumentsAreLiteralAndKeepEmpty) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp.primary_'_keys\"("
                  "second_key integer, first_key integer, "
                  "CONSTRAINT odbcpp_literal_pk "
                  "PRIMARY KEY(first_key, second_key))",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp.primary_'_keys";
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    const auto catalog = text_cell(hstmt, 1);
    const auto schema = text_cell(hstmt, 2);
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    EXPECT_EQ(std::optional<std::string>("odbcpp.primary_'_keys"),
              text_cell(hstmt, 3));
    EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<std::string>("odbcpp_literal_pk"),
              text_cell(hstmt, 6));

    EXPECT_EQ(SQL_ERROR, SQLPrimaryKeys(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("second_key"), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(2), integer_cell(hstmt, 5));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR wildcard[] = "%";
    auto expect_no_keys = [&](SQLCHAR* catalog_name,
                              SQLSMALLINT catalog_length,
                              SQLCHAR* schema_name,
                              SQLSMALLINT schema_length,
                              SQLCHAR* requested_table,
                              SQLSMALLINT table_length) {
        ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(
            hstmt, catalog_name, catalog_length, schema_name, schema_length,
            requested_table, table_length));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_keys(empty, SQL_NTS, nullptr, 0, table_name, SQL_NTS);
    expect_no_keys(nullptr, 0, empty, SQL_NTS, table_name, SQL_NTS);
    expect_no_keys(nullptr, 0, nullptr, 0, empty, SQL_NTS);
    expect_no_keys(wildcard, SQL_NTS, nullptr, 0, table_name, SQL_NTS);
    expect_no_keys(nullptr, 0, wildcard, SQL_NTS, table_name, SQL_NTS);
    expect_no_keys(nullptr, 0, nullptr, 0, wildcard, SQL_NTS);

    std::string catalog_name = *catalog;
    std::string schema_name = *schema;
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeys(
        hstmt, reinterpret_cast<SQLCHAR*>(catalog_name.data()), SQL_NTS,
        reinterpret_cast<SQLCHAR*>(schema_name.data()), SQL_NTS,
        table_name, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    auto wide_catalog = rs::odbc::utf8_to_wide(catalog_name);
    auto wide_schema = rs::odbc::utf8_to_wide(schema_name);
    auto wide_table = rs::odbc::utf8_to_wide("odbcpp.primary_'_keys");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLPrimaryKeysW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 4));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
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

TEST_F(MetadataIntegrationTest, ForeignKeyFiltersRulesAndPrimaryTargetsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_parent("
                  "id integer PRIMARY KEY, alternate integer UNIQUE)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_child("
                  "cascade_id integer, restrict_id integer, null_id integer, "
                  "default_id integer DEFAULT 1, no_action_id integer, "
                  "unique_id integer, "
                  "CONSTRAINT fk_rule_cascade FOREIGN KEY(cascade_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id) "
                  "ON UPDATE CASCADE ON DELETE CASCADE, "
                  "CONSTRAINT fk_rule_restrict FOREIGN KEY(restrict_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id) "
                  "ON UPDATE RESTRICT ON DELETE RESTRICT "
                  "DEFERRABLE INITIALLY IMMEDIATE, "
                  "CONSTRAINT fk_rule_set_null FOREIGN KEY(null_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id) "
                  "ON UPDATE SET NULL ON DELETE SET NULL "
                  "DEFERRABLE INITIALLY DEFERRED, "
                  "CONSTRAINT fk_rule_set_default FOREIGN KEY(default_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id) "
                  "ON UPDATE SET DEFAULT ON DELETE SET DEFAULT, "
                  "CONSTRAINT fk_rule_no_action FOREIGN KEY(no_action_id) "
                  "REFERENCES odbcpp_fk_rules_parent(id), "
                  "CONSTRAINT fk_rule_unique FOREIGN KEY(unique_id) "
                  "REFERENCES odbcpp_fk_rules_parent(alternate))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_fk_rules_other("
                  "id integer, CONSTRAINT fk_rule_cascade FOREIGN KEY(id) "
                  "REFERENCES odbcpp_fk_rules_parent(id))",
        SQL_NTS));

    SQLCHAR parent[] = "odbcpp_fk_rules_parent";
    SQLCHAR child[] = "odbcpp_fk_rules_child";
    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, nullptr, 0,
        nullptr, 0, nullptr, 0, child, SQL_NTS));

    std::optional<std::string> catalog;
    std::optional<std::string> schema;
    std::map<std::string, std::array<SQLINTEGER, 3>> rules;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        if (!catalog) catalog = text_cell(hstmt, 1);
        if (!schema) schema = text_cell(hstmt, 2);
        const auto update_rule = integer_cell(hstmt, 10);
        const auto delete_rule = integer_cell(hstmt, 11);
        const auto name = text_cell(hstmt, 12);
        const auto deferrability = integer_cell(hstmt, 14);
        ASSERT_TRUE(update_rule.has_value());
        ASSERT_TRUE(delete_rule.has_value());
        ASSERT_TRUE(name.has_value());
        ASSERT_TRUE(deferrability.has_value());
        rules.emplace(*name, std::array<SQLINTEGER, 3>{
            *update_rule, *delete_rule, *deferrability});
    }
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    const std::map<std::string, std::array<SQLINTEGER, 3>> expected_rules{
        {"fk_rule_cascade", {SQL_CASCADE, SQL_CASCADE, SQL_NOT_DEFERRABLE}},
        {"fk_rule_restrict", {SQL_RESTRICT, SQL_RESTRICT,
                               SQL_INITIALLY_IMMEDIATE}},
        {"fk_rule_set_null", {SQL_SET_NULL, SQL_SET_NULL,
                               SQL_INITIALLY_DEFERRED}},
        {"fk_rule_set_default", {SQL_SET_DEFAULT, SQL_SET_DEFAULT,
                                  SQL_NOT_DEFERRABLE}},
        {"fk_rule_no_action", {SQL_NO_ACTION, SQL_NO_ACTION,
                                SQL_NOT_DEFERRABLE}},
    };
    EXPECT_EQ(expected_rules, rules);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, parent, SQL_NTS,
        nullptr, 0, nullptr, 0, nullptr, 0));
    int referring_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        ++referring_rows;
        EXPECT_EQ(std::optional<std::string>("odbcpp_fk_rules_parent"),
                  text_cell(hstmt, 3));
        const auto foreign_table = text_cell(hstmt, 7);
        ASSERT_TRUE(foreign_table.has_value());
        if (referring_rows <= 5) {
            EXPECT_EQ("odbcpp_fk_rules_child", *foreign_table);
        } else {
            EXPECT_EQ("odbcpp_fk_rules_other", *foreign_table);
        }
    }
    EXPECT_EQ(6, referring_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
        hstmt, nullptr, 0, nullptr, 0, parent, SQL_NTS,
        nullptr, 0, nullptr, 0, child, SQL_NTS));
    int matching_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++matching_rows;
    EXPECT_EQ(5, matching_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR wildcard[] = "%";
    auto expect_no_foreign_keys = [&](SQLCHAR* pk_catalog,
                                      SQLCHAR* pk_schema,
                                      SQLCHAR* pk_table,
                                      SQLCHAR* fk_catalog,
                                      SQLCHAR* fk_schema,
                                      SQLCHAR* fk_table) {
        ASSERT_EQ(SQL_SUCCESS, SQLForeignKeys(
            hstmt, pk_catalog, pk_catalog ? SQL_NTS : 0,
            pk_schema, pk_schema ? SQL_NTS : 0,
            pk_table, pk_table ? SQL_NTS : 0,
            fk_catalog, fk_catalog ? SQL_NTS : 0,
            fk_schema, fk_schema ? SQL_NTS : 0,
            fk_table, fk_table ? SQL_NTS : 0));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_foreign_keys(empty, nullptr, parent,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(nullptr, empty, parent,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(wildcard, nullptr, parent,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(nullptr, wildcard, parent,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(nullptr, nullptr, wildcard,
                           nullptr, nullptr, nullptr);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           empty, nullptr, child);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           nullptr, empty, child);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           wildcard, nullptr, child);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           nullptr, wildcard, child);
    expect_no_foreign_keys(nullptr, nullptr, nullptr,
                           nullptr, nullptr, wildcard);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_parent = rs::odbc::utf8_to_wide("odbcpp_fk_rules_parent");
    auto wide_child = rs::odbc::utf8_to_wide("odbcpp_fk_rules_child");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_parent.has_value());
    ASSERT_TRUE(wide_child.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLForeignKeysW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_parent->data(), static_cast<SQLSMALLINT>(wide_parent->size()),
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_child->data(), static_cast<SQLSMALLINT>(wide_child->size())));
    matching_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++matching_rows;
    EXPECT_EQ(5, matching_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
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

TEST_F(MetadataIntegrationTest, StatisticsReportPostgreSQLIndexSemantics) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp.stats_'_table\"("
                  "id integer, value text)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_stats_unique "
                  "ON \"odbcpp.stats_'_table\"(id DESC) "
                  "WHERE value IS NOT NULL",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE INDEX odbcpp_stats_expression "
                  "ON \"odbcpp.stats_'_table\"((lower(value)))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE INDEX odbcpp_stats_hash "
                  "ON \"odbcpp.stats_'_table\" USING hash(value)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE INDEX odbcpp_stats_covering "
                  "ON \"odbcpp.stats_'_table\"(id) INCLUDE(value)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE INDEX odbcpp_stats_clustered "
                  "ON \"odbcpp.stats_'_table\"(id)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CLUSTER \"odbcpp.stats_'_table\" "
                  "USING odbcpp_stats_clustered",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp.stats_'_table";
    ASSERT_EQ(SQL_SUCCESS, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_ALL, SQL_QUICK));
    EXPECT_EQ(SQL_ERROR, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_ALL, SQL_QUICK));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    std::optional<std::string> catalog;
    std::optional<std::string> schema;
    bool found_unique = false;
    bool found_expression = false;
    bool found_hash = false;
    bool found_covering_key = false;
    bool found_covering_include = false;
    bool found_clustered = false;
    int index_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        ++index_rows;
        if (!catalog) catalog = text_cell(hstmt, 1);
        if (!schema) schema = text_cell(hstmt, 2);
        const auto non_unique = integer_cell(hstmt, 4);
        const auto index_name = text_cell(hstmt, 6);
        const auto type = integer_cell(hstmt, 7);
        const auto ordinal = integer_cell(hstmt, 8);
        const auto column = text_cell(hstmt, 9);
        const auto direction = text_cell(hstmt, 10);
        const auto cardinality = integer_cell(hstmt, 11);
        const auto pages = integer_cell(hstmt, 12);
        const auto filter = text_cell(hstmt, 13);
        ASSERT_TRUE(non_unique.has_value());
        ASSERT_TRUE(index_name.has_value());
        ASSERT_TRUE(type.has_value());
        ASSERT_TRUE(ordinal.has_value());
        ASSERT_TRUE(column.has_value());
        EXPECT_EQ(std::nullopt, cardinality);
        EXPECT_TRUE(pages.has_value());

        if (*index_name == "odbcpp_stats_unique") {
            found_unique = true;
            EXPECT_EQ(SQL_FALSE, *non_unique);
            EXPECT_EQ(SQL_INDEX_OTHER, *type);
            EXPECT_EQ(1, *ordinal);
            EXPECT_EQ("id", *column);
            EXPECT_EQ(std::optional<std::string>("D"), direction);
            ASSERT_TRUE(filter.has_value());
            EXPECT_NE(std::string::npos, filter->find("value IS NOT NULL"));
        } else if (*index_name == "odbcpp_stats_expression") {
            found_expression = true;
            EXPECT_EQ(SQL_TRUE, *non_unique);
            EXPECT_EQ(SQL_INDEX_OTHER, *type);
            EXPECT_EQ(1, *ordinal);
            EXPECT_EQ("lower(value)", *column);
            EXPECT_EQ(std::optional<std::string>("A"), direction);
            EXPECT_EQ(std::nullopt, filter);
        } else if (*index_name == "odbcpp_stats_hash") {
            found_hash = true;
            EXPECT_EQ(SQL_TRUE, *non_unique);
            EXPECT_EQ(SQL_INDEX_HASHED, *type);
            EXPECT_EQ(1, *ordinal);
            EXPECT_EQ("value", *column);
            EXPECT_EQ(std::nullopt, direction);
            EXPECT_EQ(std::nullopt, filter);
        } else if (*index_name == "odbcpp_stats_covering") {
            EXPECT_EQ(SQL_TRUE, *non_unique);
            EXPECT_EQ(SQL_INDEX_OTHER, *type);
            EXPECT_EQ(std::nullopt, filter);
            if (*ordinal == 1) {
                found_covering_key = true;
                EXPECT_EQ("id", *column);
                EXPECT_EQ(std::optional<std::string>("A"), direction);
            } else if (*ordinal == 2) {
                found_covering_include = true;
                EXPECT_EQ("value", *column);
                EXPECT_EQ(std::nullopt, direction);
            } else {
                ADD_FAILURE() << "Unexpected covering-index ordinal "
                              << *ordinal;
            }
        } else if (*index_name == "odbcpp_stats_clustered") {
            found_clustered = true;
            EXPECT_EQ(SQL_TRUE, *non_unique);
            EXPECT_EQ(SQL_INDEX_CLUSTERED, *type);
            EXPECT_EQ(1, *ordinal);
            EXPECT_EQ("id", *column);
            EXPECT_EQ(std::optional<std::string>("A"), direction);
            EXPECT_EQ(std::nullopt, filter);
        } else {
            ADD_FAILURE() << "Unexpected index " << *index_name;
        }
    }
    EXPECT_EQ(6, index_rows);
    EXPECT_TRUE(found_unique);
    EXPECT_TRUE(found_expression);
    EXPECT_TRUE(found_hash);
    EXPECT_TRUE(found_covering_key);
    EXPECT_TRUE(found_covering_include);
    EXPECT_TRUE(found_clustered);
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_UNIQUE, SQL_QUICK));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("odbcpp_stats_unique"),
              text_cell(hstmt, 6));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    EXPECT_EQ(SQL_ERROR, SQLStatistics(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        SQL_INDEX_ALL, SQL_ENSURE));
    EXPECT_EQ("HYC00", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR wildcard[] = "%";
    auto expect_no_statistics = [&](SQLCHAR* requested_catalog,
                                    SQLCHAR* requested_schema,
                                    SQLCHAR* requested_table) {
        ASSERT_EQ(SQL_SUCCESS, SQLStatistics(
            hstmt, requested_catalog, requested_catalog ? SQL_NTS : 0,
            requested_schema, requested_schema ? SQL_NTS : 0,
            requested_table, SQL_NTS, SQL_INDEX_ALL, SQL_QUICK));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_statistics(empty, nullptr, table_name);
    expect_no_statistics(nullptr, empty, table_name);
    expect_no_statistics(nullptr, nullptr, empty);
    expect_no_statistics(wildcard, nullptr, table_name);
    expect_no_statistics(nullptr, wildcard, table_name);
    expect_no_statistics(nullptr, nullptr, wildcard);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_table = rs::odbc::utf8_to_wide("odbcpp.stats_'_table");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLStatisticsW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        SQL_INDEX_ALL, SQL_QUICK));
    index_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++index_rows;
    EXPECT_EQ(6, index_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
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

TEST_F(MetadataIntegrationTest, RoutinePatternsTypesAndOverloadsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_routine_a_b(integer) "
                  "RETURNS integer LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_routine_a_b(text) "
                  "RETURNS text LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_routine_axb(integer) "
                  "RETURNS integer LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE PROCEDURE pg_temp.odbcpp_procedure_a_b(integer) "
                  "LANGUAGE SQL AS 'SELECT 1'",
        SQL_NTS));

    SQLCHAR wildcard_pattern[] = "odbcpp_routine_a_b";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, wildcard_pattern, SQL_NTS));
    int wildcard_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++wildcard_rows;
    EXPECT_EQ(3, wildcard_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR escaped_pattern[] = "odbcpp\\_routine\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, escaped_pattern, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, escaped_pattern, SQL_NTS));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    std::optional<std::string> catalog;
    std::optional<std::string> schema;
    int overload_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        ++overload_rows;
        if (!catalog) catalog = text_cell(hstmt, 1);
        if (!schema) schema = text_cell(hstmt, 2);
        EXPECT_EQ(std::optional<std::string>("odbcpp_routine_a_b"),
                  text_cell(hstmt, 3));
        EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(-1), integer_cell(hstmt, 6));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PT_FUNCTION),
                  integer_cell(hstmt, 8));
    }
    EXPECT_EQ(2, overload_rows);
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR procedure_pattern[] = "odbcpp\\_procedure\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
        hstmt, nullptr, 0, nullptr, 0, procedure_pattern, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PT_PROCEDURE),
              integer_cell(hstmt, 8));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR percent[] = "%";
    auto expect_no_routines = [&](SQLCHAR* requested_catalog,
                                  SQLCHAR* requested_schema,
                                  SQLCHAR* requested_procedure) {
        ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
            hstmt, requested_catalog, requested_catalog ? SQL_NTS : 0,
            requested_schema, requested_schema ? SQL_NTS : 0,
            requested_procedure,
            requested_procedure ? SQL_NTS : 0));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_routines(empty, nullptr, escaped_pattern);
    expect_no_routines(percent, nullptr, escaped_pattern);
    expect_no_routines(nullptr, empty, escaped_pattern);
    expect_no_routines(nullptr, nullptr, empty);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_pattern = rs::odbc::utf8_to_wide("odbcpp\\_routine\\_a\\_b");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_pattern.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLProceduresW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_pattern->data(), static_cast<SQLSMALLINT>(wide_pattern->size())));
    overload_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++overload_rows;
    EXPECT_EQ(2, overload_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, ProcedureCountsFollowPostgreSQLArgumentModes) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_modes_inout("
                  "IN first integer, INOUT shared integer, OUT final text) "
                  "LANGUAGE SQL AS "
                  "'SELECT shared + first, first::text'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_modes_variadic("
                  "VARIADIC items integer[]) RETURNS integer "
                  "LANGUAGE SQL AS 'SELECT cardinality(items)'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_modes_table(first integer) "
                  "RETURNS TABLE(total integer, label text) LANGUAGE SQL "
                  "AS 'SELECT first, first::text'",
        SQL_NTS));

    auto expect_counts = [&](SQLCHAR* routine_pattern,
                             SQLINTEGER expected_inputs,
                             SQLINTEGER expected_outputs) {
        ASSERT_EQ(SQL_SUCCESS, SQLProcedures(
            hstmt, nullptr, 0, nullptr, 0,
            routine_pattern, SQL_NTS));
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_inputs),
                  integer_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_outputs),
                  integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(-1), integer_cell(hstmt, 6));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PT_FUNCTION),
                  integer_cell(hstmt, 8));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };

    SQLCHAR inout_name[] = "odbcpp\\_modes\\_inout";
    SQLCHAR variadic_name[] = "odbcpp\\_modes\\_variadic";
    SQLCHAR table_name[] = "odbcpp\\_modes\\_table";
    expect_counts(inout_name, 2, 2);
    expect_counts(variadic_name, 1, 0);
    expect_counts(table_name, 1, 2);
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

TEST_F(MetadataIntegrationTest, ProcedureColumnModesAndPatternsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_param_a_b("
                  "IN input_arg integer, INOUT inout_arg text, "
                  "OUT output_arg bigint) LANGUAGE SQL "
                  "AS 'SELECT inout_arg, input_arg::bigint'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_param_axb(input_arg integer) "
                  "RETURNS integer LANGUAGE SQL AS 'SELECT input_arg'",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE FUNCTION pg_temp.odbcpp_unnamed(integer) "
                  "RETURNS integer LANGUAGE SQL AS 'SELECT $1'",
        SQL_NTS));

    SQLCHAR wildcard_procedure[] = "odbcpp_param_a_b";
    SQLCHAR all_columns[] = "%";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, wildcard_procedure, SQL_NTS,
        all_columns, SQL_NTS));
    int wildcard_rows = 0;
    while (SQLFetch(hstmt) == SQL_SUCCESS) ++wildcard_rows;
    EXPECT_EQ(5, wildcard_rows);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR escaped_procedure[] = "odbcpp\\_param\\_a\\_b";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, escaped_procedure, SQL_NTS,
        all_columns, SQL_NTS));
    EXPECT_EQ(SQL_ERROR, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, escaped_procedure, SQL_NTS,
        all_columns, SQL_NTS));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    const std::array<const char*, 3> expected_names{
        "input_arg", "inout_arg", "output_arg"};
    const std::array<SQLINTEGER, 3> expected_modes{
        SQL_PARAM_INPUT, SQL_PARAM_INPUT_OUTPUT, SQL_PARAM_OUTPUT};
    const std::array<SQLINTEGER, 3> expected_types{
        SQL_INTEGER, SQL_LONGVARCHAR, SQL_BIGINT};
    std::optional<std::string> catalog;
    std::optional<std::string> schema;
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        if (!catalog) catalog = text_cell(hstmt, 1);
        if (!schema) schema = text_cell(hstmt, 2);
        EXPECT_EQ(std::optional<std::string>(expected_names[i]),
                  text_cell(hstmt, 4));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_modes[i]),
                  integer_cell(hstmt, 5));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_types[i]),
                  integer_cell(hstmt, 6));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_NULLABLE_UNKNOWN),
                  integer_cell(hstmt, 12));
        EXPECT_EQ(std::optional<SQLINTEGER>(static_cast<SQLINTEGER>(i + 1)),
                  integer_cell(hstmt, 18));
        EXPECT_EQ(std::optional<std::string>(""), text_cell(hstmt, 19));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR unnamed_procedure[] = "odbcpp\\_unnamed";
    SQLCHAR empty[] = "";
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
        hstmt, nullptr, 0, nullptr, 0, unnamed_procedure, SQL_NTS,
        empty, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>(""), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PARAM_INPUT),
              integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(1), integer_cell(hstmt, 18));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>(""), text_cell(hstmt, 4));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_RETURN_VALUE),
              integer_cell(hstmt, 5));
    EXPECT_EQ(std::optional<SQLINTEGER>(0), integer_cell(hstmt, 18));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR percent[] = "%";
    auto expect_no_procedure_columns = [&](SQLCHAR* requested_catalog,
                                           SQLCHAR* requested_schema,
                                           SQLCHAR* requested_procedure,
                                           SQLCHAR* requested_column) {
        ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumns(
            hstmt, requested_catalog, requested_catalog ? SQL_NTS : 0,
            requested_schema, requested_schema ? SQL_NTS : 0,
            requested_procedure, requested_procedure ? SQL_NTS : 0,
            requested_column, requested_column ? SQL_NTS : 0));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_procedure_columns(empty, nullptr, escaped_procedure,
                                all_columns);
    expect_no_procedure_columns(percent, nullptr, escaped_procedure,
                                all_columns);
    expect_no_procedure_columns(nullptr, empty, escaped_procedure,
                                all_columns);
    expect_no_procedure_columns(nullptr, nullptr, empty, all_columns);
    expect_no_procedure_columns(nullptr, nullptr, escaped_procedure, empty);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_procedure = rs::odbc::utf8_to_wide("odbcpp\\_param\\_a\\_b");
    auto wide_column = rs::odbc::utf8_to_wide("input%");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_procedure.has_value());
    ASSERT_TRUE(wide_column.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLProcedureColumnsW(
        hstmt,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_procedure->data(),
        static_cast<SQLSMALLINT>(wide_procedure->size()),
        wide_column->data(), static_cast<SQLSMALLINT>(wide_column->size())));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("input_arg"), text_cell(hstmt, 4));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
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
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));

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
    EXPECT_EQ(SQL_SCOPE_CURROW, scope);
    EXPECT_STREQ("id", column_name);
    EXPECT_EQ(SQL_INTEGER, data_type);
    EXPECT_EQ(SQL_PC_NOT_PSEUDO, pseudo_column);
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
}

TEST_F(MetadataIntegrationTest, SpecialColumnScopeAndArgumentsFollowOdbc) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE \"odbcpp.special_'_table\"("
                  "second_key bigint, first_key integer, value text, "
                  "PRIMARY KEY(first_key, second_key))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_special_shorter_unique "
                  "ON \"odbcpp.special_'_table\" (value)",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp.special_'_table";
    SQLCHAR column_pattern[] = "first_key";
    ASSERT_EQ(SQL_SUCCESS, SQLColumns(
        hstmt, nullptr, 0, nullptr, 0, table_name, SQL_NTS,
        column_pattern, SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    const auto catalog = text_cell(hstmt, 1);
    const auto schema = text_cell(hstmt, 2);
    ASSERT_TRUE(catalog.has_value());
    ASSERT_TRUE(schema.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
    EXPECT_EQ(SQL_ERROR, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
    EXPECT_EQ("24000", diagnostic_state(SQL_HANDLE_STMT, hstmt));

    const std::array<const char*, 2> expected_names{"first_key", "second_key"};
    const std::array<SQLINTEGER, 2> expected_types{SQL_INTEGER, SQL_BIGINT};
    for (std::size_t i = 0; i < expected_names.size(); ++i) {
        ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_SCOPE_CURROW),
                  integer_cell(hstmt, 1));
        EXPECT_EQ(std::optional<std::string>(expected_names[i]),
                  text_cell(hstmt, 2));
        EXPECT_EQ(std::optional<SQLINTEGER>(expected_types[i]),
                  integer_cell(hstmt, 3));
        EXPECT_EQ(std::optional<SQLINTEGER>(SQL_PC_NOT_PSEUDO),
                  integer_cell(hstmt, 8));
    }
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    constexpr std::array<SQLUSMALLINT, 2> broader_scopes{
        static_cast<SQLUSMALLINT>(SQL_SCOPE_TRANSACTION),
        static_cast<SQLUSMALLINT>(SQL_SCOPE_SESSION)};
    for (const auto scope : broader_scopes) {
        ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
            hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
            table_name, SQL_NTS, scope, SQL_NULLABLE));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    }
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_ROWVER, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    SQLCHAR empty[] = "";
    SQLCHAR wildcard[] = "%";
    auto expect_no_special_columns = [&](SQLCHAR* requested_catalog,
                                         SQLCHAR* requested_schema,
                                         SQLCHAR* requested_table) {
        ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
            hstmt, SQL_BEST_ROWID,
            requested_catalog, requested_catalog ? SQL_NTS : 0,
            requested_schema, requested_schema ? SQL_NTS : 0,
            requested_table, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
        EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
    };
    expect_no_special_columns(empty, nullptr, table_name);
    expect_no_special_columns(nullptr, empty, table_name);
    expect_no_special_columns(nullptr, nullptr, empty);
    expect_no_special_columns(wildcard, nullptr, table_name);
    expect_no_special_columns(nullptr, wildcard, table_name);
    expect_no_special_columns(nullptr, nullptr, wildcard);

    auto wide_catalog = rs::odbc::utf8_to_wide(*catalog);
    auto wide_schema = rs::odbc::utf8_to_wide(*schema);
    auto wide_table = rs::odbc::utf8_to_wide("odbcpp.special_'_table");
    ASSERT_TRUE(wide_catalog.has_value());
    ASSERT_TRUE(wide_schema.has_value());
    ASSERT_TRUE(wide_table.has_value());
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumnsW(
        hstmt, SQL_BEST_ROWID,
        wide_catalog->data(), static_cast<SQLSMALLINT>(wide_catalog->size()),
        wide_schema->data(), static_cast<SQLSMALLINT>(wide_schema->size()),
        wide_table->data(), static_cast<SQLSMALLINT>(wide_table->size()),
        SQL_SCOPE_CURROW, SQL_NULLABLE));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("first_key"), text_cell(hstmt, 2));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
}

TEST_F(MetadataIntegrationTest, SpecialColumnsFallsBackToUsableUniqueIndex) {
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_unique_rowid_test("
                  "nullable_code integer, stable_code bigint NOT NULL, "
                  "included_value text, spare integer NOT NULL)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_rowid_expression "
                  "ON odbcpp_unique_rowid_test ((lower(included_value)))",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_rowid_partial "
                  "ON odbcpp_unique_rowid_test (spare) WHERE spare > 0",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_rowid_nullable "
                  "ON odbcpp_unique_rowid_test (nullable_code)",
        SQL_NTS));
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE UNIQUE INDEX odbcpp_rowid_nonnull "
                  "ON odbcpp_unique_rowid_test (stable_code) "
                  "INCLUDE (included_value)",
        SQL_NTS));

    SQLCHAR table_name[] = "odbcpp_unique_rowid_test";
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NULLABLE));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("nullable_code"),
              text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_INTEGER), integer_cell(hstmt, 3));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        table_name, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(hstmt));
    EXPECT_EQ(std::optional<std::string>("stable_code"),
              text_cell(hstmt, 2));
    EXPECT_EQ(std::optional<SQLINTEGER>(SQL_BIGINT), integer_cell(hstmt, 3));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));

    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(
        hstmt,
        (SQLCHAR*)"CREATE TEMP TABLE odbcpp_nullable_rowid_test("
                  "only_key integer UNIQUE)",
        SQL_NTS));
    SQLCHAR nullable_table[] = "odbcpp_nullable_rowid_test";
    ASSERT_EQ(SQL_SUCCESS, SQLSpecialColumns(
        hstmt, SQL_BEST_ROWID, nullptr, 0, nullptr, 0,
        nullable_table, SQL_NTS, SQL_SCOPE_CURROW, SQL_NO_NULLS));
    EXPECT_EQ(SQL_NO_DATA, SQLFetch(hstmt));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(hstmt));
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
