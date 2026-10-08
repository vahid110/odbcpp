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
class RedshiftPackageLiveTest : public ::testing::Test {
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
    constexpr std::array<std::string_view,8> keys{"DRIVER","SERVER","PORT","DATABASE","UID","PWD","SSL","SSLCAFILE"};
    if(fields.size()!=keys.size()) { return false; }
    for(const auto key:keys) { if(!fields.contains(std::string(key))||fields.at(std::string(key)).empty()) { return false; } }
    const auto host=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_HOST");const auto ca=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_CA_FILE");
    if(fields.at("DRIVER")!=driver_||fields.at("UID")!=principal_||fields.at("DATABASE")!=database_||
        host.empty()||fields.at("SERVER")!=host||!canonical_file(ca)||fields.at("SSLCAFILE")!=ca||fields.at("SSL")!="1"||fields.at("PORT")!="5439") { return false; }
    connection_=std::move(bytes);return true;
#else
    (void)path;return false;
#endif
  }
  std::string state(SQLSMALLINT type,SQLHANDLE handle) {
    std::array<SQLCHAR,6> code{};const auto result=SQLGetDiagRecA(type,handle,1,code.data(),nullptr,nullptr,0,nullptr);
    return result==SQL_SUCCESS||result==SQL_SUCCESS_WITH_INFO?std::string(reinterpret_cast<const char*>(code.data()),5):"unavailable";
  }
  template<class Cell> void poison(Cell& cell) { cell.before.fill(0x5a);cell.after.fill(0x5a);std::memset(&cell.value,0x5a,sizeof(cell.value)); }
  template<class Cell> void guards(const Cell& cell) {
    for(const auto byte:cell.before) { EXPECT_EQ(0x5a,byte); }
    for(const auto byte:cell.after) { EXPECT_EQ(0x5a,byte); }
  }
  bool cap(Clock::time_point cutoff) {
    const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(cutoff-Clock::now()).count();
    if(remaining<=0) { ADD_FAILURE()<<"Package case window expired";return false; }
    const auto seconds=static_cast<std::uintptr_t>(std::min<std::int64_t>(remaining,5));
    if(SQLSetConnectAttr(dbc_,SQL_ATTR_CONNECTION_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) { ADD_FAILURE()<<state(SQL_HANDLE_DBC,dbc_);return false; }
    if(stmt_&&SQLSetStmtAttr(stmt_,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(seconds),0)!=SQL_SUCCESS) { ADD_FAILURE()<<state(SQL_HANDLE_STMT,stmt_);return false; }
    return true;
  }
  void SetUp() override {
    const char* marker=std::getenv("ODBCPP_REDSHIFT_PACKAGE_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP()<<"Package scope not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="installed-driver-smoke-v1")<<"Wrong package marker";
    ASSERT_TRUE(sizeof(SQLWCHAR)==2)<<"Package requires matched SQLWCHAR2";
#if !defined(__APPLE__)
    FAIL()<<"Package platform not qualified";
#else
    end_=Clock::now()+std::chrono::seconds{40};admitted_=true;
    principal_=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_PRINCIPAL");database_=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_DATABASE");
    driver_=setting("ODBCPP_REDSHIFT_PACKAGE_INSTALLED_DRIVER");
    ASSERT_TRUE(principal_=="odbcpp_pilot_test"&&database_=="odbcpp_pilot"&&canonical_file(driver_))<<"Package identity/path preflight refused";
    ASSERT_TRUE(private_connection(setting("ODBCPP_REDSHIFT_PACKAGE_CONNECTION_FILE")))<<"Private connection preflight refused";
    ASSERT_TRUE(Clock::now()<end_)<<"Package setup expired";
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_ENV,SQL_NULL_HANDLE,&env_));
    ASSERT_EQ(SQL_SUCCESS,SQLSetEnvAttr(env_,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),0));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_DBC,env_,&dbc_));
    ASSERT_TRUE(cap(end_));ASSERT_EQ(SQL_SUCCESS,SQLSetConnectAttr(dbc_,SQL_ATTR_LOGIN_TIMEOUT,reinterpret_cast<SQLPOINTER>(std::uintptr_t{5}),0));
    const auto connect_result=SQLDriverConnectA(dbc_,nullptr,reinterpret_cast<SQLCHAR*>(connection_.data()),static_cast<SQLSMALLINT>(connection_.size()),nullptr,0,nullptr,SQL_DRIVER_NOPROMPT);
    connected_=connect_result==SQL_SUCCESS||connect_result==SQL_SUCCESS_WITH_INFO;
    ASSERT_EQ(SQL_SUCCESS,connect_result)<<state(SQL_HANDLE_DBC,dbc_);
    // Observe the image after the manager's real connection, never dlopen it for dispatch.
    std::size_t matches=0;
    for(std::uint32_t index=0;index<_dyld_image_count();++index) {
      const char* image=_dyld_get_image_name(index);if(!image) { continue; }
      const auto path=std::filesystem::path(image);
      const auto filename=path.filename().string();
      if(filename.starts_with("libodbcpp")||filename.find("redshiftodbc")!=std::string::npos||filename.find("psqlodbc")!=std::string::npos) {
        std::error_code error;const auto actual=std::filesystem::canonical(path,error);
        ASSERT_TRUE(!error&&actual==std::filesystem::path(driver_))<<"Mixed or unexpected driver image";
        ++matches;loaded_=actual.string();
      }
    }
    ASSERT_EQ(std::size_t{1},matches)<<"Installed driver image not uniquely observed";
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,dbc_,&stmt_));ASSERT_TRUE(cap(end_));
#endif
  }
  void direct(const char* sql) { ASSERT_TRUE(cap(end_));ASSERT_EQ(SQL_SUCCESS,SQLExecDirectA(stmt_,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS))<<state(SQL_HANDLE_STMT,stmt_); }
  void describe(SQLUSMALLINT column,const char* expected,SQLSMALLINT type,SQLULEN width) {
    std::array<SQLCHAR,64> name{};SQLSMALLINT length=-1,actual=-1,scale=-1;SQLULEN size=0;
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeColA(stmt_,column,name.data(),static_cast<SQLSMALLINT>(name.size()),&length,&actual,&size,&scale,nullptr));
    ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length),name.size());
    EXPECT_TRUE(std::string_view(reinterpret_cast<const char*>(name.data()),static_cast<std::size_t>(length))==expected);EXPECT_EQ(type,actual);EXPECT_EQ(width,size);EXPECT_EQ(0,scale);
  }
  std::string text(SQLUSMALLINT column) {
    poison(text_);length_=73;const auto result=SQLGetData(stmt_,column,SQL_C_CHAR,text_.value.data(),static_cast<SQLLEN>(text_.value.size()),&length_);
    EXPECT_EQ(SQL_SUCCESS,result)<<state(SQL_HANDLE_STMT,stmt_);guards(text_);
    if(result!=SQL_SUCCESS||length_<0||static_cast<std::size_t>(length_)>=text_.value.size()) { ADD_FAILURE()<<"Package text bounds refused";return {}; }
    EXPECT_EQ(0,text_.value[static_cast<std::size_t>(length_)]);
    return std::string(reinterpret_cast<const char*>(text_.value.data()),static_cast<std::size_t>(length_));
  }
  void integer(SQLINTEGER expected,SQLUSMALLINT column=1) {
    poison(integer_);length_=73;ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt_,column,SQL_C_SLONG,&integer_.value,sizeof(integer_.value),&length_))<<state(SQL_HANDLE_STMT,stmt_);
    EXPECT_EQ(expected,integer_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer_.value)),length_);guards(integer_);
  }
  void reset() {
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt_,SQL_UNBIND));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt_,SQL_RESET_PARAMS));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_ROW_STATUS_PTR,nullptr,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_ROWS_FETCHED_PTR,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_PARAM_STATUS_PTR,nullptr,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,nullptr,0));
  }
  void disconnect() {
    ASSERT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,stmt_));stmt_=SQL_NULL_HSTMT;
    ASSERT_TRUE(cap(end_));const auto result=SQLDisconnect(dbc_);
    if(result==SQL_SUCCESS||result==SQL_SUCCESS_WITH_INFO) { connected_=false; }
    else { disconnect_fault_=state(SQL_HANDLE_DBC,dbc_); }
    ASSERT_EQ(SQL_SUCCESS,result)<<disconnect_fault_;
  }
  void publish() { ASSERT_TRUE(Clock::now()<end_)<<"Package publication expired";ASSERT_FALSE(HasFailure());RecordProperty("package_loaded_driver_image",loaded_); }
  void TearDown() override {
    const auto entry=Clock::now();if(admitted_) { EXPECT_TRUE(entry<end_)<<"Package case exceeded original window"; }
    const auto cleanup=std::min(end_,entry+std::chrono::seconds{5});
    if(stmt_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,stmt_));stmt_=SQL_NULL_HSTMT; }
    if(dbc_) {
      if(connected_&&cap(cleanup)) {
        SQLRETURN result=SQL_ERROR;
        if(disconnect_fault_.empty()) {
          result=SQLDisconnect(dbc_);
          if(result==SQL_ERROR) { disconnect_fault_=state(SQL_HANDLE_DBC,dbc_); }
        }
        // A previous unknown disconnect outcome is never retried.
        if(result==SQL_ERROR&&disconnect_fault_=="25000") {
          ADD_FAILURE()<<"Package cleanup found active transaction";
          if(cap(cleanup)) {
            const auto rollback=SQLEndTran(SQL_HANDLE_DBC,dbc_,SQL_ROLLBACK);EXPECT_EQ(SQL_SUCCESS,rollback)<<state(SQL_HANDLE_DBC,dbc_);
            if(rollback==SQL_SUCCESS&&cap(cleanup)) { EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(dbc_)); }
          }
        } else { EXPECT_EQ(SQL_SUCCESS,result)<<disconnect_fault_; }
      }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,dbc_));dbc_=SQL_NULL_HDBC;
    }
    if(env_) { EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,env_));env_=SQL_NULL_HENV; }
  }
};

TEST_F(RedshiftPackageLiveTest, VerifiedTlsIdentityIntegerAndOwningDisconnect) {
  direct("SELECT TRIM(current_user) AS user_name,current_database() AS database_name,CAST(1 AS INTEGER) AS integer_value");ASSERT_FALSE(HasFailure());
  SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(stmt_,&count));ASSERT_EQ(3,count);describe(3,"integer_value",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt_));
  const auto user=text(1);const auto database=text(2);ASSERT_FALSE(HasFailure());EXPECT_TRUE(user==principal_);EXPECT_TRUE(database==database_);
  poison(integer_);length_=73;ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt_,3,SQL_C_SLONG,&integer_.value,sizeof(integer_.value),&length_));EXPECT_EQ(1,integer_.value);EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer_.value)),length_);guards(integer_);const auto owned=integer_.value;
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(stmt_));disconnect();ASSERT_FALSE(HasFailure());EXPECT_TRUE(user==principal_&&database==database_);EXPECT_EQ(1,owned);publish();
}

TEST_F(RedshiftPackageLiveTest, PreparedUnicodeNullAndStatementReuse) {
  ASSERT_TRUE(cap(end_));SQLCHAR query[]="SELECT CAST(? AS VARCHAR(32)) AS text_value";
  ASSERT_EQ(SQL_SUCCESS,SQLPrepareA(stmt_,query,SQL_NTS))<<state(SQL_HANDLE_STMT,stmt_);
  SQLSMALLINT parameters=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(stmt_,&parameters));ASSERT_EQ(1,parameters);
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed_,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_PARAM_STATUS_PTR,&parameter_status_,0));
  constexpr char literal[]="Grüße😀";std::vector<SQLWCHAR> owned;
  constexpr std::array<SQLWCHAR,8> expected{'G','r',0xfc,0xdf,'e',0xd83d,0xde00,0};
  for(unsigned trial=0;trial<2;++trial) {
    input_.fill('\x5a');std::memcpy(input_.data(),literal,sizeof(literal));input_length_=trial==0?static_cast<SQLLEN>(sizeof(literal)-1):SQL_NULL_DATA;
    if(trial==1) { input_.fill('\xff'); }
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt_,1,SQL_PARAM_INPUT,SQL_C_CHAR,SQL_VARCHAR,32,0,input_.data(),static_cast<SQLLEN>(input_.size()),&input_length_));
    ASSERT_TRUE(cap(end_));ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt_))<<state(SQL_HANDLE_STMT,stmt_);EXPECT_EQ(static_cast<SQLULEN>(1),processed_);EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status_);
    input_.fill('Z');input_length_=97;SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(stmt_,&columns));ASSERT_EQ(1,columns);describe(1,"text_value",SQL_VARCHAR,32);ASSERT_FALSE(HasFailure());poison(wide_);length_=73;
    ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt_,1,SQL_C_WCHAR,wide_.value.data(),static_cast<SQLLEN>(sizeof(wide_.value)),&length_));ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt_));guards(wide_);
    if(trial==0) { EXPECT_EQ(static_cast<SQLLEN>(14),length_);EXPECT_TRUE(std::equal(expected.begin(),expected.end(),wide_.value.begin()));owned.assign(wide_.value.begin(),wide_.value.begin()+7); }
    else { EXPECT_EQ(SQL_NULL_DATA,length_);for(const auto unit:wide_.value) { EXPECT_EQ(static_cast<SQLWCHAR>(0x5a5a),unit); } }
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(stmt_));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt_,SQL_UNBIND));
  }
  reset();ASSERT_FALSE(HasFailure());poison(wide_);EXPECT_TRUE(owned.size()==7&&std::equal(owned.begin(),owned.end(),expected.begin()));disconnect();ASSERT_FALSE(HasFailure());publish();
}

TEST_F(RedshiftPackageLiveTest, BoundNarrowingErrorAndRecovery) {
  direct("SELECT CAST(32768 AS INTEGER) AS integer_value");ASSERT_FALSE(HasFailure());SQLSMALLINT columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(stmt_,&columns));ASSERT_EQ(1,columns);describe(1,"integer_value",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());
  poison(short_);length_=73;ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt_,1,SQL_C_SSHORT,&short_.value,sizeof(short_.value),&length_));
  ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_ROW_STATUS_PTR,&row_status_,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_ROWS_FETCHED_PTR,&fetched_,0));
  ASSERT_EQ(SQL_ERROR,SQLFetch(stmt_));EXPECT_TRUE(state(SQL_HANDLE_STMT,stmt_)=="22003");EXPECT_EQ(SQL_ROW_ERROR,row_status_);EXPECT_EQ(static_cast<SQLULEN>(1),fetched_);EXPECT_EQ(static_cast<SQLLEN>(73),length_);EXPECT_EQ(static_cast<SQLSMALLINT>(0x5a5a),short_.value);guards(short_);
  ASSERT_FALSE(HasFailure());ASSERT_TRUE(cap(end_));reset();ASSERT_FALSE(HasFailure());
  // SQL_GD_BOUND is not advertised: close the failed bound cursor before GetData.
  direct("SELECT CAST(32768 AS INTEGER) AS recovered_value,CAST(1 AS INTEGER) AS recovery");ASSERT_FALSE(HasFailure());
  columns=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(stmt_,&columns));ASSERT_EQ(2,columns);
  describe(1,"recovered_value",SQL_INTEGER,10);describe(2,"recovery",SQL_INTEGER,10);ASSERT_FALSE(HasFailure());
  ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt_));integer(32768,1);ASSERT_FALSE(HasFailure());const auto owned=integer_.value;
  integer(1,2);ASSERT_FALSE(HasFailure());const auto recovery=integer_.value;
  ASSERT_EQ(SQL_NO_DATA,SQLFetch(stmt_));reset();disconnect();ASSERT_FALSE(HasFailure());EXPECT_EQ(32768,owned);EXPECT_EQ(1,recovery);publish();
}
} // namespace
