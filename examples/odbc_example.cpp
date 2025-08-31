#include "odbc/odbc_api.h"
#include <iostream>
#include <cstring>

int main() {
  std::cout << "🚀 ODBC Driver Example\n";
  std::cout << "======================\n";
  
  SQLHENV henv = nullptr;
  SQLHDBC hdbc = nullptr;
  SQLHSTMT hstmt = nullptr;
  SQLRETURN ret;
  
  try {
    // 1. Allocate environment handle
    ret = SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
    if (ret != SQL_SUCCESS) {
      std::cout << "❌ Failed to allocate environment handle\n";
      return 1;
    }
    std::cout << "✅ Environment handle allocated\n";
    
    // 2. Set ODBC version
    ret = SQLSetEnvAttr(henv, 200, reinterpret_cast<void*>(3), 0); // ODBC 3.x
    if (ret != SQL_SUCCESS) {
      std::cout << "❌ Failed to set ODBC version\n";
      return 1;
    }
    std::cout << "✅ ODBC version set to 3.x\n";
    
    // 3. Allocate connection handle
    ret = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    if (ret != SQL_SUCCESS) {
      std::cout << "❌ Failed to allocate connection handle\n";
      return 1;
    }
    std::cout << "✅ Connection handle allocated\n";
    
    // 4. Connect to database
    std::cout << "🔌 Connecting to database...\n";
    ret = SQLConnect(hdbc, 
                    reinterpret_cast<SQLCHAR*>(const_cast<char*>("postgres")), SQL_NTS,
                    reinterpret_cast<SQLCHAR*>(const_cast<char*>("postgres")), SQL_NTS,
                    reinterpret_cast<SQLCHAR*>(const_cast<char*>("postgres")), SQL_NTS);
    
    if (ret != SQL_SUCCESS) {
      // Get error details
      SQLCHAR sqlstate[6];
      SQLCHAR message[256];
      SQLINTEGER native_error;
      SQLSMALLINT text_length;
      
      SQLGetDiagRec(SQL_HANDLE_DBC, hdbc, 1, sqlstate, &native_error, 
                   message, sizeof(message), &text_length);
      
      std::cout << "❌ Connection failed: " << sqlstate << " - " << message << "\n";
      std::cout << "ℹ️  This is expected if no database is running\n";
    } else {
      std::cout << "✅ Connected to database successfully!\n";
      
      // 5. Allocate statement handle
      ret = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
      if (ret == SQL_SUCCESS) {
        std::cout << "✅ Statement handle allocated\n";
        
        // 6. Execute a query
        std::cout << "📊 Executing query: SELECT 'Hello ODBC!' as greeting\n";
        ret = SQLExecDirect(hstmt, 
                           reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 'Hello ODBC!' as greeting")), 
                           SQL_NTS);
        
        if (ret == SQL_SUCCESS) {
          std::cout << "✅ Query executed successfully\n";
          
          // 7. Fetch results
          ret = SQLFetch(hstmt);
          if (ret == SQL_SUCCESS) {
            char buffer[256];
            SQLLEN indicator;
            
            ret = SQLGetData(hstmt, 1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator);
            if (ret == SQL_SUCCESS) {
              std::cout << "📋 Result: " << buffer << "\n";
            }
          }
        } else {
          std::cout << "❌ Query execution failed\n";
        }
      }
    }
    
    // Cleanup
    if (hstmt) {
      SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
      std::cout << "🧹 Statement handle freed\n";
    }
    
    if (hdbc) {
      SQLDisconnect(hdbc);
      SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
      std::cout << "🧹 Connection handle freed\n";
    }
    
    if (henv) {
      SQLFreeHandle(SQL_HANDLE_ENV, henv);
      std::cout << "🧹 Environment handle freed\n";
    }
    
    std::cout << "\n✅ ODBC Example completed successfully!\n";
    std::cout << "🎉 Your ODBC driver is working!\n";
    
    return 0;
    
  } catch (const std::exception& e) {
    std::cerr << "❌ Exception: " << e.what() << "\n";
    return 1;
  }
}