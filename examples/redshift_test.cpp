#include "odbc/odbc_api.h"
#include <iostream>
#include <cstring>
#include <cstdlib>

void print_error(SQLSMALLINT handle_type, SQLHANDLE handle) {
  SQLCHAR sqlstate[6];
  SQLCHAR message[256];
  SQLINTEGER native_error;
  SQLSMALLINT text_length;
  
  SQLRETURN ret = SQLGetDiagRec(handle_type, handle, 1, sqlstate, &native_error, 
                               message, sizeof(message), &text_length);
  if (ret == SQL_SUCCESS) {
    std::cout << "❌ Error: " << sqlstate << " - " << message << "\n";
  }
}

int main(int argc, char** argv) {
  std::cout << "🔴 Redshift ODBC Driver Test\n";
  std::cout << "============================\n";
  
  std::string connection_info;
  std::string user_override;
  std::string password_override;
  
  if (argc >= 2) {
    // Command line: ./redshift_test "DSN=RedshiftTest" [user] [password]
    // or: ./redshift_test "SERVER=host;PORT=5439;DATABASE=dev;UID=user;PWD=pass"
    connection_info = argv[1];
    if (argc >= 3) user_override = argv[2];
    if (argc >= 4) password_override = argv[3];
    
    std::cout << "📋 Using connection: " << connection_info << "\n";
    if (!user_override.empty()) std::cout << "👤 User override: " << user_override << "\n";
    
  } else {
    // Fallback to environment variables
    const char* host = getenv("REDSHIFT_HOST");
    const char* port = getenv("REDSHIFT_PORT");
    const char* database = getenv("REDSHIFT_DATABASE");
    const char* user = getenv("REDSHIFT_USER");
    const char* password = getenv("REDSHIFT_PASSWORD");
    
    if (host && database && user && password) {
      // Build connection string from environment
      connection_info = "SERVER=" + std::string(host) + 
                       ";PORT=" + (port ? port : "5439") +
                       ";DATABASE=" + database +
                       ";UID=" + user +
                       ";PWD=" + password;
      std::cout << "🌍 Using environment variables\n";
      std::cout << "🔗 Connecting to: " << host << ":" << (port ? port : "5439") << "/" << database << "\n";
    } else {
      std::cout << "❌ No connection specified. Usage:\n";
      std::cout << "\n📋 DSN Method:\n";
      std::cout << "   ./redshift_test \"DSN=RedshiftTest\"\n";
      std::cout << "   (Edit odbcpp.dsn file with your credentials)\n";
      std::cout << "\n🔗 Connection String Method:\n";
      std::cout << "   ./redshift_test \"SERVER=host;PORT=5439;DATABASE=dev;UID=user;PWD=pass\"\n";
      std::cout << "\n🌍 Environment Variables Method:\n";
      std::cout << "   export REDSHIFT_HOST=your-cluster.redshift.amazonaws.com\n";
      std::cout << "   export REDSHIFT_PORT=5439\n";
      std::cout << "   export REDSHIFT_DATABASE=dev\n";
      std::cout << "   export REDSHIFT_USER=your-username\n";
      std::cout << "   export REDSHIFT_PASSWORD=your-password\n";
      return 1;
    }
  }
  
  std::cout << "\n";
  
  SQLHENV henv = nullptr;
  SQLHDBC hdbc = nullptr;
  SQLHSTMT hstmt = nullptr;
  SQLRETURN ret;
  
  try {
    // 1. Allocate environment
    ret = SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv);
    if (ret != SQL_SUCCESS) {
      std::cout << "❌ Failed to allocate environment handle\n";
      return 1;
    }
    std::cout << "✅ Environment handle allocated\n";
    
    // 2. Set ODBC version
    ret = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, reinterpret_cast<void*>(SQL_OV_ODBC3), 0);
    if (ret != SQL_SUCCESS) {
      std::cout << "❌ Failed to set ODBC version\n";
      print_error(SQL_HANDLE_ENV, henv);
      return 1;
    }
    std::cout << "✅ ODBC version set\n";
    
    // 3. Allocate connection
    ret = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
    if (ret != SQL_SUCCESS) {
      std::cout << "❌ Failed to allocate connection handle\n";
      print_error(SQL_HANDLE_ENV, henv);
      return 1;
    }
    std::cout << "✅ Connection handle allocated\n";
    
    // 4. Connect to Redshift
    std::cout << "🔌 Connecting to Redshift...\n";
    
    ret = SQLConnect(hdbc,
                    reinterpret_cast<SQLCHAR*>(const_cast<char*>(connection_info.c_str())), SQL_NTS,
                    reinterpret_cast<SQLCHAR*>(const_cast<char*>(user_override.c_str())), SQL_NTS,
                    reinterpret_cast<SQLCHAR*>(const_cast<char*>(password_override.c_str())), SQL_NTS);
    
    if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
      std::cout << "❌ Connection failed\n";
      print_error(SQL_HANDLE_DBC, hdbc);
      return 1;
    }
    
    std::cout << "✅ Connected to Redshift successfully!\n\n";
    
    // 5. Allocate statement
    ret = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);
    if (ret != SQL_SUCCESS) {
      std::cout << "❌ Failed to allocate statement handle\n";
      print_error(SQL_HANDLE_DBC, hdbc);
      return 1;
    }
    std::cout << "✅ Statement handle allocated\n";
    
    // 6. Test queries
    std::vector<std::string> test_queries = {
      "SELECT version()",
      "SELECT current_database(), current_user",
      "SELECT COUNT(*) as table_count FROM information_schema.tables WHERE table_schema = 'public'",
      "SELECT 'Hello Redshift!' as greeting, 42 as answer, NOW() as current_time"
    };
    
    for (size_t i = 0; i < test_queries.size(); ++i) {
      const auto& sql = test_queries[i];
      std::cout << "\n📊 Test " << (i+1) << ": " << sql << "\n";
      
      ret = SQLExecDirect(hstmt, 
                         reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())), 
                         SQL_NTS);
      
      if (ret != SQL_SUCCESS && ret != SQL_SUCCESS_WITH_INFO) {
        std::cout << "❌ Query failed\n";
        print_error(SQL_HANDLE_STMT, hstmt);
        continue;
      }
      
      std::cout << "✅ Query executed successfully\n";
      
      // Fetch results
      int row_count = 0;
      while ((ret = SQLFetch(hstmt)) == SQL_SUCCESS) {
        row_count++;
        std::cout << "📋 Row " << row_count << ": ";
        
        // Get data from first few columns
        for (int col = 1; col <= 3; ++col) {
          char buffer[256];
          SQLLEN indicator;
          
          ret = SQLGetData(hstmt, col, SQL_C_CHAR, buffer, sizeof(buffer), &indicator);
          if (ret == SQL_SUCCESS) {
            std::cout << buffer;
            if (col < 3) std::cout << " | ";
          } else if (ret == SQL_NO_DATA) {
            break;
          }
        }
        std::cout << "\n";
        
        if (row_count >= 5) { // Limit output
          std::cout << "   ... (limiting output to 5 rows)\n";
          break;
        }
      }
      
      if (row_count == 0) {
        std::cout << "📋 No results returned\n";
      }
    }
    
    std::cout << "\n🎉 All Redshift tests completed successfully!\n";
    
  } catch (const std::exception& e) {
    std::cout << "❌ Exception: " << e.what() << "\n";
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
  
  return 0;
}