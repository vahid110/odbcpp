#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include <cstdlib>
#include <cstring>

class RedshiftRealTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Check if Redshift credentials are available
    host_ = getenv("REDSHIFT_HOST");
    port_ = getenv("REDSHIFT_PORT");
    database_ = getenv("REDSHIFT_DATABASE");
    user_ = getenv("REDSHIFT_USER");
    password_ = getenv("REDSHIFT_PASSWORD");
    
    if (!host_ || !database_ || !user_ || !password_) {
      GTEST_SKIP() << "Redshift credentials not provided. Set REDSHIFT_* environment variables.";
    }
    
    // Allocate handles
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &henv_), SQL_SUCCESS);
    ASSERT_EQ(SQLSetEnvAttr(henv_, SQL_ATTR_ODBC_VERSION, reinterpret_cast<void*>(SQL_OV_ODBC3), 0), SQL_SUCCESS);
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC, henv_, &hdbc_), SQL_SUCCESS);
  }
  
  void TearDown() override {
    if (hstmt_) SQLFreeHandle(SQL_HANDLE_STMT, hstmt_);
    if (hdbc_) {
      SQLDisconnect(hdbc_);
      SQLFreeHandle(SQL_HANDLE_DBC, hdbc_);
    }
    if (henv_) SQLFreeHandle(SQL_HANDLE_ENV, henv_);
  }
  
  bool connect() {
    std::string conn_str = "SERVER=" + std::string(host_) + 
                          ";PORT=" + (port_ ? port_ : "5439") +
                          ";DATABASE=" + database_ +
                          ";UID=" + user_ +
                          ";PWD=" + password_ +
                          ";SSL=0";
    
    SQLRETURN ret = SQLConnect(hdbc_,
                              reinterpret_cast<SQLCHAR*>(const_cast<char*>(conn_str.c_str())), SQL_NTS,
                              nullptr, 0,
                              nullptr, 0);
    
    if (ret == SQL_SUCCESS || ret == SQL_SUCCESS_WITH_INFO) {
      EXPECT_EQ(SQLAllocHandle(SQL_HANDLE_STMT, hdbc_, &hstmt_), SQL_SUCCESS);
      return true;
    }
    return false;
  }
  
  std::string get_error(SQLSMALLINT handle_type, SQLHANDLE handle) {
    SQLCHAR sqlstate[6];
    SQLCHAR message[256];
    SQLINTEGER native_error;
    SQLSMALLINT text_length;
    
    if (SQLGetDiagRec(handle_type, handle, 1, sqlstate, &native_error, 
                     message, sizeof(message), &text_length) == SQL_SUCCESS) {
      return std::string(reinterpret_cast<char*>(sqlstate)) + " - " + 
             std::string(reinterpret_cast<char*>(message));
    }
    return "Unknown error";
  }
  
  SQLHENV henv_ = nullptr;
  SQLHDBC hdbc_ = nullptr;
  SQLHSTMT hstmt_ = nullptr;
  
  const char* host_ = nullptr;
  const char* port_ = nullptr;
  const char* database_ = nullptr;
  const char* user_ = nullptr;
  const char* password_ = nullptr;
};

TEST_F(RedshiftRealTest, ConnectionTest) {
  ASSERT_TRUE(connect()) << "Failed to connect: " << get_error(SQL_HANDLE_DBC, hdbc_);
}

TEST_F(RedshiftRealTest, VersionQuery) {
  ASSERT_TRUE(connect());
  
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT version()")), 
                               SQL_NTS);
  ASSERT_EQ(ret, SQL_SUCCESS) << "Query failed: " << get_error(SQL_HANDLE_STMT, hstmt_);
  
  ret = SQLFetch(hstmt_);
  ASSERT_EQ(ret, SQL_SUCCESS) << "Fetch failed: " << get_error(SQL_HANDLE_STMT, hstmt_);
  
  char version[512];
  SQLLEN indicator;
  ret = SQLGetData(hstmt_, 1, SQL_C_CHAR, version, sizeof(version), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS) << "GetData failed: " << get_error(SQL_HANDLE_STMT, hstmt_);
  
  EXPECT_TRUE(strstr(version, "PostgreSQL") != nullptr || strstr(version, "Redshift") != nullptr)
    << "Unexpected version string: " << version;
}

TEST_F(RedshiftRealTest, CurrentUserQuery) {
  ASSERT_TRUE(connect());
  
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT current_user")), 
                               SQL_NTS);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  ret = SQLFetch(hstmt_);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  char current_user[256];
  SQLLEN indicator;
  ret = SQLGetData(hstmt_, 1, SQL_C_CHAR, current_user, sizeof(current_user), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  EXPECT_STRNE(current_user, "");
  std::cout << "Current user: " << current_user << std::endl;
}

TEST_F(RedshiftRealTest, MultipleRowQuery) {
  ASSERT_TRUE(connect());
  
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT 1 as num UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5")), 
                               SQL_NTS);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  int row_count = 0;
  while ((ret = SQLFetch(hstmt_)) == SQL_SUCCESS) {
    row_count++;
    
    char num_str[32];
    SQLLEN indicator;
    ret = SQLGetData(hstmt_, 1, SQL_C_CHAR, num_str, sizeof(num_str), &indicator);
    ASSERT_EQ(ret, SQL_SUCCESS);
    
    int num = std::stoi(num_str);
    EXPECT_EQ(num, row_count);
  }
  
  EXPECT_EQ(row_count, 5);
}

TEST_F(RedshiftRealTest, ErrorHandling) {
  ASSERT_TRUE(connect());
  
  // Execute invalid SQL
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>("SELECT * FROM nonexistent_table_12345")), 
                               SQL_NTS);
  EXPECT_EQ(ret, SQL_ERROR);
  
  std::string error = get_error(SQL_HANDLE_STMT, hstmt_);
  EXPECT_FALSE(error.empty());
  std::cout << "Expected error: " << error << std::endl;
}

TEST_F(RedshiftRealTest, DataTypes) {
  ASSERT_TRUE(connect());
  
  SQLRETURN ret = SQLExecDirect(hstmt_, 
                               reinterpret_cast<SQLCHAR*>(const_cast<char*>(
                                 "SELECT 'text_value' as text_col, "
                                 "42 as int_col, "
                                 "3.14159 as float_col, "
                                 "NOW() as timestamp_col")), 
                               SQL_NTS);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  ret = SQLFetch(hstmt_);
  ASSERT_EQ(ret, SQL_SUCCESS);
  
  // Test string column
  char text_val[256];
  SQLLEN indicator;
  ret = SQLGetData(hstmt_, 1, SQL_C_CHAR, text_val, sizeof(text_val), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS);
  EXPECT_STREQ(text_val, "text_value");
  
  // Test integer column
  char int_val[32];
  ret = SQLGetData(hstmt_, 2, SQL_C_CHAR, int_val, sizeof(int_val), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS);
  EXPECT_STREQ(int_val, "42");
  
  // Test float column
  char float_val[32];
  ret = SQLGetData(hstmt_, 3, SQL_C_CHAR, float_val, sizeof(float_val), &indicator);
  ASSERT_EQ(ret, SQL_SUCCESS);
  EXPECT_TRUE(strstr(float_val, "3.14") != nullptr);
}