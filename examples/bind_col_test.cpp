#include <sql.h>
#include <sqlext.h>
#include <iostream>

int main() {
    std::cout << "🔗 ODBC SQLBindCol Test\n";
    std::cout << "========================\n";
    
    SQLHENV henv;
    SQLHDBC hdbc;
    SQLHSTMT hstmt;
    
    // Allocate handles
    SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
    SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    
    // Connect
    SQLRETURN ret = SQLConnect(hdbc, (SQLCHAR*)"DSN=RedshiftProd", SQL_NTS, nullptr, 0, nullptr, 0);
    if (ret != SQL_SUCCESS) {
        std::cout << "❌ Connection failed\n";
        return 1;
    }
    std::cout << "✅ Connected to Redshift\n";
    
    SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    
    // Execute query
    ret = SQLExecDirect(hstmt, (SQLCHAR*)"SELECT 'Hello' as message, 123 as number, 45.67 as decimal", SQL_NTS);
    if (ret != SQL_SUCCESS) {
        std::cout << "❌ Query execution failed\n";
        return 1;
    }
    std::cout << "✅ Query executed\n";
    
    // Bind columns to variables
    char message[256];
    SQLINTEGER number;
    SQLDOUBLE decimal;
    SQLLEN message_len, number_len, decimal_len;
    
    std::cout << "\n📋 Binding columns...\n";
    ret = SQLBindCol(hstmt, 1, SQL_C_CHAR, message, sizeof(message), &message_len);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Column 1 bound (string)\n";
    } else {
        std::cout << "❌ Column 1 binding failed\n";
    }
    
    ret = SQLBindCol(hstmt, 2, SQL_C_SLONG, &number, 0, &number_len);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Column 2 bound (integer)\n";
    } else {
        std::cout << "❌ Column 2 binding failed\n";
    }
    
    ret = SQLBindCol(hstmt, 3, SQL_C_DOUBLE, &decimal, 0, &decimal_len);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Column 3 bound (double)\n";
    } else {
        std::cout << "❌ Column 3 binding failed\n";
    }
    
    // Fetch with bound columns
    std::cout << "\n📊 Fetching with bound columns...\n";
    ret = SQLFetch(hstmt);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Fetch successful\n";
        std::cout << "📋 Bound Results:\n";
        std::cout << "   Message: " << message << "\n";
        std::cout << "   Number: " << number << "\n";
        std::cout << "   Decimal: " << decimal << "\n";
    } else {
        std::cout << "❌ Fetch failed\n";
    }
    
    // Cleanup
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    SQLDisconnect(hdbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    SQLFreeHandle(SQL_HANDLE_ENV, henv);
    
    std::cout << "\n🎉 SQLBindCol test completed!\n";
    return 0;
}