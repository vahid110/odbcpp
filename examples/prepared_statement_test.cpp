#include <sql.h>
#include <sqlext.h>
#include <iostream>

int main() {
    std::cout << "🔧 ODBC Prepared Statement Test\n";
    std::cout << "===============================\n";
    
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
    
    // Test 1: Prepare statement with parameters
    std::cout << "\n📋 Test 1: Prepare statement with parameters\n";
    ret = SQLPrepare(hstmt, (SQLCHAR*)"SELECT ? as message, ? as number", SQL_NTS);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Statement prepared successfully\n";
    } else {
        std::cout << "❌ SQLPrepare failed\n";
        return 1;
    }
    
    // Bind parameters
    char message[50] = "Hello Prepared!";
    SQLINTEGER number = 123;
    
    ret = SQLBindParameter(hstmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 50, 0, message, sizeof(message), nullptr);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Parameter 1 bound (string)\n";
    } else {
        std::cout << "❌ SQLBindParameter 1 failed\n";
    }
    
    ret = SQLBindParameter(hstmt, 2, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, &number, 0, nullptr);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Parameter 2 bound (integer)\n";
    } else {
        std::cout << "❌ SQLBindParameter 2 failed\n";
    }
    
    // Execute prepared statement
    ret = SQLExecute(hstmt);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Prepared statement executed\n";
        
        // Fetch results
        std::cout << "📊 Results:\n";
        while (SQLFetch(hstmt) == SQL_SUCCESS) {
            char col1[256], col2[256];
            SQLGetData(hstmt, 1, SQL_C_CHAR, col1, sizeof(col1), nullptr);
            SQLGetData(hstmt, 2, SQL_C_CHAR, col2, sizeof(col2), nullptr);
            std::cout << "   " << col1 << " | " << col2 << "\n";
        }
    } else {
        std::cout << "❌ SQLExecute failed\n";
    }
    
    // Test 2: Execute with different parameters
    std::cout << "\n📋 Test 2: Execute with different parameters\n";
    strcpy(message, "Second execution");
    number = 456;
    
    ret = SQLExecute(hstmt);
    if (ret == SQL_SUCCESS) {
        std::cout << "✅ Second execution successful\n";
        
        std::cout << "📊 Results:\n";
        while (SQLFetch(hstmt) == SQL_SUCCESS) {
            char col1[256], col2[256];
            SQLGetData(hstmt, 1, SQL_C_CHAR, col1, sizeof(col1), nullptr);
            SQLGetData(hstmt, 2, SQL_C_CHAR, col2, sizeof(col2), nullptr);
            std::cout << "   " << col1 << " | " << col2 << "\n";
        }
    } else {
        std::cout << "❌ Second execution failed\n";
    }
    
    // Cleanup
    SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
    SQLDisconnect(hdbc);
    SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
    SQLFreeHandle(SQL_HANDLE_ENV, henv);
    
    std::cout << "\n🎉 Prepared statement test completed!\n";
    return 0;
}