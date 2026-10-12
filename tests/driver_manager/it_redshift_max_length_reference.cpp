#include <gtest/gtest.h>
#include <sql.h>
#include <sqlext.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
// This executable dispatches exclusively through the linked driver manager.
class RedshiftMaxLengthReferenceTest : public ::testing::Test {
protected:
  using Clock=std::chrono::steady_clock;
  Clock::time_point end_{};
  bool admitted_=false,connected_=false;
  SQLHENV env_=SQL_NULL_HENV;
  SQLHDBC dbc_=SQL_NULL_HDBC;
  SQLHSTMT stmt_=SQL_NULL_HSTMT;
  std::string connection_,principal_,database_,driver_,loaded_,disconnect_fault_;
  std::array<char,32> input_{};
  SQLLEN input_length_=0,length_=73;
  SQLULEN processed_=99,fetched_=99;
  SQLUSMALLINT parameter_status_=SQL_PARAM_UNUSED,row_status_=SQL_ROW_NOROW;
  struct Text { std::array<unsigned char,8> before;std::array<SQLCHAR,128> value;std::array<unsigned char,8> after; } text_{};
  struct Wide { std::array<unsigned char,8> before;std::array<SQLWCHAR,12> value;std::array<unsigned char,8> after; } wide_{};
  struct Short { std::array<unsigned char,8> before;SQLSMALLINT value;std::array<unsigned char,8> after; } short_{};
  struct Integer { std::array<unsigned char,8> before;SQLINTEGER value;std::array<unsigned char,8> after; } integer_{};

  static std::string setting(const char* key) { const char* value=std::getenv(key);return value?std::string(value):std::string{}; }
  static bool canonical_file(const std::string& value) {
    std::error_code error;const auto path=std::filesystem::path(value);
    return path.is_absolute()&&std::filesystem::canonical(path,error)==path&&!error&&std::filesystem::is_regular_file(path,error)&&!error;
  }
  bool private_connection(const std::string& path) {
#if defined(__APPLE__)
    if(!canonical_file(path)) { return false; }
    const int fd=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd<0) { return false; }
    struct FileOwner { int value;~FileOwner() { if(value>=0) { ::close(value); } } } file{fd};
    struct stat before{},after{};
    bool valid=::fstat(fd,&before)==0&&S_ISREG(before.st_mode)&&before.st_uid==::geteuid()&&
        (before.st_mode&07777)==0600&&before.st_nlink==1&&before.st_size>0&&before.st_size<=4096;
    std::string bytes;
    if(valid) {
      bytes.resize(static_cast<std::size_t>(before.st_size));std::size_t offset=0;
      while(offset<bytes.size()) {
        const auto count=::read(fd,bytes.data()+offset,bytes.size()-offset);
        if(count<=0) { valid=false;break; }
        offset+=static_cast<std::size_t>(count);
      }
      char extra=0;if(valid&&::read(fd,&extra,1)!=0) { valid=false; }
    }
    valid=valid&&::fstat(fd,&after)==0&&before.st_dev==after.st_dev&&before.st_ino==after.st_ino&&
        before.st_size==after.st_size&&before.st_uid==after.st_uid&&before.st_mode==after.st_mode&&after.st_nlink==1&&
        before.st_mtimespec.tv_sec==after.st_mtimespec.tv_sec&&before.st_mtimespec.tv_nsec==after.st_mtimespec.tv_nsec&&
        before.st_ctimespec.tv_sec==after.st_ctimespec.tv_sec&&before.st_ctimespec.tv_nsec==after.st_ctimespec.tv_nsec;
    struct stat named{};valid=valid&&::lstat(path.c_str(),&named)==0&&S_ISREG(named.st_mode)&&named.st_dev==before.st_dev&&named.st_ino==before.st_ino;
    if(::close(fd)!=0) { valid=false; }file.value=-1;
    if(!valid||bytes.find('\0')!=std::string::npos||bytes.find('\n')!=std::string::npos||bytes.find('\r')!=std::string::npos) { return false; }
    // Closed root-prepared format; braces permit semicolons in the password.
    std::map<std::string,std::string> fields;std::size_t cursor=0;
    while(cursor<bytes.size()) {
      const auto equals=bytes.find('=',cursor);if(equals==std::string::npos) { return false; }
      const auto key=bytes.substr(cursor,equals-cursor);cursor=equals+1;std::string value;
      if(cursor<bytes.size()&&bytes[cursor]=='{') {
        ++cursor;bool closed=false;
        while(cursor<bytes.size()) {
          const char ch=bytes[cursor++];
          if(ch!='}') { value.push_back(ch); }
          else if(cursor<bytes.size()&&bytes[cursor]=='}') { value.push_back('}');++cursor; }
          else { closed=true;break; }
        }
        if(!closed||(cursor<bytes.size()&&bytes[cursor]!=';')) { return false; }
      } else {
        const auto separator=bytes.find(';',cursor);const auto finish=separator==std::string::npos?bytes.size():separator;
        value=bytes.substr(cursor,finish-cursor);cursor=finish;
      }
      if(!fields.emplace(key,std::move(value)).second) { return false; }
      if(cursor<bytes.size()) { ++cursor; }
    }
    constexpr std::array<std::string_view,12> keys{"DRIVER","SERVER","PORT","DATABASE","UID","PWD","SSLMODE","TRUSTSTORE","LOGLEVEL","CONNECTIONRETRYCOUNT","READONLY","USEDECLAREFETCH"};
    if(fields.size()!=keys.size()) { return false; }
    for(const auto key:keys) { if(!fields.contains(std::string(key))||fields.at(std::string(key)).empty()) { return false; } }
    const auto host=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_HOST");const auto ca=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_CA_FILE");
    if(fields.at("DRIVER")!=driver_||fields.at("UID")!=principal_||fields.at("DATABASE")!=database_||
        host.empty()||fields.at("SERVER")!=host||!canonical_file(ca)||fields.at("TRUSTSTORE")!=ca||fields.at("SSLMODE")!="verify-full"||fields.at("PORT")!="5439") { return false; }
    if(fields.at("LOGLEVEL")!="0"||fields.at("CONNECTIONRETRYCOUNT")!="0"||fields.at("READONLY")!="0"||fields.at("USEDECLAREFETCH")!="0") { return false; }
    connection_=std::move(bytes);return true;
#else
    (void)path;return false;
#endif
  }
  std::string state(SQLSMALLINT type,SQLHANDLE handle) {
    std::array<SQLCHAR,6> code{};const auto result=SQLGetDiagRecA(type,handle,1,code.data(),nullptr,nullptr,0,nullptr);
    return result==SQL_SUCCESS||result==SQL_SUCCESS_WITH_INFO?std::string(reinterpret_cast<const char*>(code.data()),5):"unavailable";
  }

  void SetUp() override {
    if(setting("ODBCPP_MAX_LENGTH_REFERENCE_ADMISSION")!="aws-max-length-diagnostic-v1") {
      FAIL()<<"Diagnostic not admitted";
    }
    ASSERT_EQ(sizeof(SQLWCHAR),4u);
    end_=Clock::now()+std::chrono::seconds(40);
    principal_=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_PRINCIPAL");
    database_=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_DATABASE");
    driver_=setting("ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER");
    ASSERT_EQ(principal_,"odbcpp_pilot_test");ASSERT_EQ(database_,"odbcpp_pilot");
    ASSERT_TRUE(private_connection(setting("ODBCPP_REDSHIFT_PACKAGE_CONNECTION_FILE")));
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_ENV,SQL_NULL_HANDLE,&env_),SQL_SUCCESS);
    ASSERT_EQ(SQLSetEnvAttr(env_,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),0),SQL_SUCCESS);
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_DBC,env_,&dbc_),SQL_SUCCESS);
    ASSERT_EQ(SQLSetConnectAttr(dbc_,SQL_ATTR_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(5),0),SQL_SUCCESS);
    ASSERT_TRUE(Clock::now()+std::chrono::seconds(5)<end_);
    auto rc=SQLDriverConnectA(dbc_,nullptr,reinterpret_cast<SQLCHAR*>(connection_.data()),
      static_cast<SQLSMALLINT>(connection_.size()),nullptr,0,nullptr,SQL_DRIVER_NOPROMPT);
    connected_=SQL_SUCCEEDED(rc);ASSERT_TRUE(connected_)<<state(SQL_HANDLE_DBC,dbc_);
    std::size_t matches=0;
    for(std::uint32_t i=0;i<_dyld_image_count();++i) {
      auto p=std::filesystem::path(_dyld_get_image_name(i));
      if(p.filename()=="librsodbc64.dylib") {
        ASSERT_EQ(std::filesystem::canonical(p).string(),driver_);++matches;
      }
    }
    ASSERT_EQ(matches,1u);
    ASSERT_EQ(SQLAllocHandle(SQL_HANDLE_STMT,dbc_,&stmt_),SQL_SUCCESS);
    ASSERT_TRUE(SQL_SUCCEEDED(SQLSetStmtAttr(stmt_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(5),0)));
  }
  void TearDown() override {
    if(connected_) { EXPECT_LT(Clock::now(),end_); }
    if(stmt_) { SQLFreeHandle(SQL_HANDLE_STMT,stmt_); }
    if(connected_) { EXPECT_TRUE(SQL_SUCCEEDED(SQLDisconnect(dbc_)))<<state(SQL_HANDLE_DBC,dbc_); }
    if(dbc_) { SQLFreeHandle(SQL_HANDLE_DBC,dbc_); }
    if(env_) { SQLFreeHandle(SQL_HANDLE_ENV,env_); }
    if(connected_) { EXPECT_LT(Clock::now(),end_); }
  }
  void observe(SQLULEN requested,bool bound) {
    ASSERT_LT(Clock::now(),end_);
    const std::string key=std::string(bound?"bound_":"getdata_")+std::to_string(requested);
    auto rc=SQLSetStmtAttrA(stmt_,SQL_ATTR_MAX_LENGTH,reinterpret_cast<SQLPOINTER>(requested),0);
    RecordProperty(key+"_set_rc",std::to_string(rc));RecordProperty(key+"_set_state",state(SQL_HANDLE_STMT,stmt_));
    SQLULEN effective=static_cast<SQLULEN>(-1);
    ASSERT_TRUE(SQL_SUCCEEDED(SQLGetStmtAttrA(stmt_,SQL_ATTR_MAX_LENGTH,&effective,sizeof(effective),nullptr)));
    RecordProperty(key+"_effective",std::to_string(effective));
    if(requested==0) { ASSERT_TRUE(SQL_SUCCEEDED(rc));ASSERT_EQ(effective,0u); }
    // A diagnostic records refusal, substitution or actual effect; it does not force vendor parity.
    ASSERT_TRUE(SQL_SUCCEEDED(rc)||rc==SQL_ERROR);
    std::array<unsigned char,66> text{},bytes{},nulltext{};
    text.fill(0xa5);bytes.fill(0xa5);nulltext.fill(0xa5);
    SQLLEN tl=-99,bl=-99,nl=-99;
    if(bound) {
      ASSERT_EQ(SQLBindCol(stmt_,1,SQL_C_CHAR,text.data()+1,64,&tl),SQL_SUCCESS);
      ASSERT_EQ(SQLBindCol(stmt_,2,SQL_C_BINARY,bytes.data()+1,64,&bl),SQL_SUCCESS);
      ASSERT_EQ(SQLBindCol(stmt_,3,SQL_C_CHAR,nulltext.data()+1,64,&nl),SQL_SUCCESS);
    }
    constexpr char query[]="SELECT CAST('abcdefghijklmnop' AS VARCHAR(16)), FROM_HEX('000102030405060708090a0b0c0d0e0f'), CAST(NULL AS VARCHAR(16))";
    ASSERT_LT(Clock::now(),end_);
    rc=SQLExecDirectA(stmt_,reinterpret_cast<SQLCHAR*>(const_cast<char*>(query)),SQL_NTS);
    ASSERT_TRUE(SQL_SUCCEEDED(rc))<<state(SQL_HANDLE_STMT,stmt_);
    ASSERT_TRUE(Clock::now()+std::chrono::seconds(5)<end_);
    rc=SQLFetch(stmt_);ASSERT_TRUE(SQL_SUCCEEDED(rc))<<state(SQL_HANDLE_STMT,stmt_);
    RecordProperty(key+"_fetch_rc",std::to_string(rc));
    if(!bound) {
      ASSERT_TRUE(Clock::now()+std::chrono::seconds(5)<end_);
      rc=SQLGetData(stmt_,1,SQL_C_CHAR,text.data()+1,64,&tl);ASSERT_TRUE(SQL_SUCCEEDED(rc));
      RecordProperty(key+"_text_rc",std::to_string(rc));
      ASSERT_TRUE(Clock::now()+std::chrono::seconds(5)<end_);
      rc=SQLGetData(stmt_,2,SQL_C_BINARY,bytes.data()+1,64,&bl);ASSERT_TRUE(SQL_SUCCEEDED(rc));
      RecordProperty(key+"_binary_rc",std::to_string(rc));
      ASSERT_TRUE(Clock::now()+std::chrono::seconds(5)<end_);
      ASSERT_TRUE(SQL_SUCCEEDED(SQLGetData(stmt_,3,SQL_C_CHAR,nulltext.data()+1,64,&nl)));
    }
    RecordProperty(key+"_text_indicator",std::to_string(tl));RecordProperty(key+"_binary_indicator",std::to_string(bl));
    std::size_t textsize=0;while(textsize<64&&text[textsize+1]!=0) { ++textsize; }
    RecordProperty(key+"_text_returned",std::to_string(textsize));
    ASSERT_LE(textsize,16u);EXPECT_EQ(std::memcmp(text.data()+1,"abcdefghijklmnop",textsize),0);
    ASSERT_GE(bl,0);ASSERT_LE(bl,16);
    for(SQLLEN i=0;i<bl;++i) { EXPECT_EQ(bytes[static_cast<std::size_t>(i)+1],i); }
    EXPECT_EQ(nl,SQL_NULL_DATA);
    for(const auto* cell:{&text,&bytes,&nulltext}) { EXPECT_EQ(cell->front(),0xa5);EXPECT_EQ(cell->back(),0xa5); }
    if(requested==0) { EXPECT_EQ(textsize,16u);EXPECT_EQ(tl,16);EXPECT_EQ(bl,16); }
    ASSERT_TRUE(Clock::now()+std::chrono::seconds(5)<end_);
    EXPECT_EQ(SQLFetch(stmt_),SQL_NO_DATA);
    ASSERT_LT(Clock::now(),end_);
    ASSERT_EQ(SQLFreeStmt(stmt_,SQL_CLOSE),SQL_SUCCESS);
    ASSERT_EQ(SQLFreeStmt(stmt_,SQL_UNBIND),SQL_SUCCESS);
    ASSERT_LT(Clock::now(),end_);
  }
};
TEST_F(RedshiftMaxLengthReferenceTest,ObserveZeroAndEightBoundFetchAndGetData) {
  observe(0,true);if(HasFatalFailure()) { return; }
  observe(8,true);if(HasFatalFailure()) { return; }
  observe(0,false);if(HasFatalFailure()) { return; }
  observe(8,false);
}
} // namespace
