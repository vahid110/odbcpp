#include <sql.h>
#include <sqlext.h>
#include <iostream>
#include <iomanip>

int main() {
    std::cout << "🔍 ODBC Metadata Test\n";
    std::cout << "=====================\n";
    
    SQLHENV henv;
    SQLHDBC hdbc;
    SQLHSTMT hstmt;
    
    // Allocate handles
    SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    
    // Connect
    SQLRETURN ret = SQLConnect(hdbc, (SQLCHAR*)"DSN=RedshiftTest", SQL_NTS, nullptr, 0, nullptr, 0);
    if (ret != SQL_SUCCESS) {
        std::cout << "❌ Connection failed\n";
        return 1;
    }
    std::cout << "✅ Connected to Redshift\n";
    
    // Execute query
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    ret = SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Hello' as greeting, 42 as answer, NOW() as current_time", SQL_NTS);
    if (ret != SQL_SUCCESS) {
        std::cout << "❌ Query failed\n";
        return 1;
    }
    std::cout << "✅ Query executed\n";
    
    // Test SQLNumResultCols
    SQLSMALLINT num_cols;
    ret = SQLNumResultCols(hstmt, &num_cols);
    if (ret == SQL_SUCCESS) {
        std::cout << "📊 Number of columns: " << num_cols << "\n";
    } else {
        std::cout << "❌ SQLNumResultCols failed\n";
    }
    
    // Test SQLDescribeCol for each column
    for (SQLUSMALLINT i = 1; i <= num_cols; i++) {
        SQLCHAR column_name[256];
        SQLSMALLINT name_length;
        SQLSMALLINT data_type;
        SQLULEN column_size;
        SQLSMALLINT decimal_digits;
        SQLSMALLINT nullable;
        
        ret = SQLDescribeCol(hstmt, i, column_name, sizeof(column_name), &name_length,
                            &data_type, &column_size, &decimal_digits, &nullable);
        
        if (ret == SQL_SUCCESS) {
            std::cout << "📋 Column " << i << ":\n";
            std::cout << "   Name: " << column_name << "\n";
            std::cout << "   Type: " << data_type << "\n";
            std::cout << "   Size: " << column_size << "\n";
            std::cout << "   Nullable: " << (nullable == SQL_NULLABLE ? "Yes" : "No") << "\n";
        } else {
            std::cout << "❌ SQLDescribeCol failed for column " << i << "\n";
        }
    }
    
    // Test SQLColAttribute
    SQLLEN numeric_attr;
    ret = SQLColAttribute(hstmt, 1, SQL_DESC_TYPE, nullptr, 0, nullptr, &numeric_attr);
    if (ret == SQL_SUCCESS) {
        std::cout << "🔍 Column 1 type (via SQLColAttribute): " << numeric_attr << "\n";
    }
    
    // Fetch and display data
    std::cout << "\n📊 Query Results:\n";
    while (SQLFetch(hstmt) == SQL_SUCCESS) {
        for (SQLUSMALLINT i = 1; i <= num_cols; i++) {
            char buffer[512];
            SQLGetData(hstmt, i, SQL_C_CHAR, buffer, sizeof(buffer), nullptr);
            std::cout << buffer;
            if (i < num_cols) std::cout << " | ";
        }
        std::cout << "\n";
    }
    
    // Cleanup
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    SQLDisconnect(hdbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    SQLFreeHandle(SQL_HANDLE_ENV, henv);
    
    std::cout << "\n🎉 Metadata test completed!\n";
    return 0;
}