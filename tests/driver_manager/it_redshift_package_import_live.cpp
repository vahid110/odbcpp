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
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef ODBCPP_OFFICIAL_REDSHIFT_PROFILE
#define ODBCPP_OFFICIAL_REDSHIFT_PROFILE 0
#endif
static_assert(ODBCPP_OFFICIAL_REDSHIFT_PROFILE == 0 || ODBCPP_OFFICIAL_REDSHIFT_PROFILE == 1);
#if ODBCPP_OFFICIAL_REDSHIFT_PROFILE
static_assert(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == 4 && ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE == 4);
#endif
namespace {
static_assert(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == 2 || ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == 4);
static_assert(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE);
static_assert(sizeof(SQLWCHAR) == ODBCPP_EXPECT_DM_SQLWCHAR_SIZE);
// This executable dispatches exclusively through the linked driver manager.
class RedshiftPackageImportLiveTest : public ::testing::Test {
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
#if ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == 4
  SQLLEN numeric_indicator_=73; // Borrowed by SQLBindCol until reset/TearDown.
#endif
  SQLULEN processed_=99,fetched_=99;
  SQLUSMALLINT parameter_status_=SQL_PARAM_UNUSED,row_status_=SQL_ROW_NOROW;
  struct Text { std::array<unsigned char,8> before;std::array<SQLCHAR,128> value;std::array<unsigned char,8> after; } text_{};
  struct Wide { std::array<unsigned char,8> before;std::array<SQLWCHAR,12> value;std::array<unsigned char,8> after; } wide_{};
  struct Short { std::array<unsigned char,8> before;SQLSMALLINT value;std::array<unsigned char,8> after; } short_{};
  struct Integer { std::array<unsigned char,8> before;SQLINTEGER value;std::array<unsigned char,8> after; } integer_{};

  SQLINTEGER minimum_=1;
  SQLLEN minimum_length_=static_cast<SQLLEN>(sizeof(minimum_));
  SQLHDESC ard_=SQL_NULL_HDESC;
  template<class T> struct Scalar { std::array<unsigned char,8> before;T value;std::array<unsigned char,8> after; };
  Scalar<SQL_NUMERIC_STRUCT> numeric_{};
  Scalar<SQL_DATE_STRUCT> day_{};
  Scalar<SQL_TIMESTAMP_STRUCT> stamp_{};
  Scalar<std::array<SQLCHAR,64>> name_{};
  Scalar<std::array<SQLWCHAR,64>> wide_name_{};
  Scalar<std::array<SQLCHAR,65537>> opaque_{};
  struct Descriptor { std::string name;SQLSMALLINT type,scale,nullable;SQLULEN width;bool operator==(const Descriptor&) const = default; };
  std::vector<Descriptor> descriptors_;
  std::array<std::array<std::optional<std::string>,18>,2> columns_{};
  std::array<std::optional<std::string>,5> table_{};

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
#if ODBCPP_OFFICIAL_REDSHIFT_PROFILE
    constexpr std::array<std::string_view,8> keys{"DRIVER","SERVER","PORT","DATABASE","UID","PWD","SSLMODE","TRUSTSTORE"};
#else
    constexpr std::array<std::string_view,8> keys{"DRIVER","SERVER","PORT","DATABASE","UID","PWD","SSL","SSLCAFILE"};
#endif
    if(fields.size()!=keys.size()) { return false; }
    for(const auto key:keys) { if(!fields.contains(std::string(key))||fields.at(std::string(key)).empty()) { return false; } }
    const auto host=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_HOST");const auto ca=setting("ODBCPP_REDSHIFT_PACKAGE_EXPECTED_CA_FILE");
#if ODBCPP_OFFICIAL_REDSHIFT_PROFILE
    // Require explicit TrustStore and verify-full for the selected official profile.
    if(fields.at("DRIVER")!=driver_||fields.at("UID")!=principal_||fields.at("DATABASE")!=database_||
        host.empty()||fields.at("SERVER")!=host||!canonical_file(ca)||fields.at("TRUSTSTORE")!=ca||fields.at("SSLMODE")!="verify-full"||fields.at("PORT")!="5439") { return false; }
#else
    if(fields.at("DRIVER")!=driver_||fields.at("UID")!=principal_||fields.at("DATABASE")!=database_||
        host.empty()||fields.at("SERVER")!=host||!canonical_file(ca)||fields.at("SSLCAFILE")!=ca||fields.at("SSL")!="1"||fields.at("PORT")!="5439") { return false; }
#endif
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
#if ODBCPP_OFFICIAL_REDSHIFT_PROFILE
    const char* marker=std::getenv("ODBCPP_REDSHIFT_OFFICIAL_PACKAGE_IMPORT_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP()<<"Official package scope not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="official-driver-import-v1")<<"Wrong official package marker";
#else
    const char* marker=std::getenv("ODBCPP_REDSHIFT_PACKAGE_IMPORT_ADMISSION");
    if(marker==nullptr) { GTEST_SKIP()<<"Package scope not admitted"; }
    ASSERT_TRUE(std::string_view(marker)=="installed-driver-import-v1")<<"Wrong package marker";
#endif
    ASSERT_TRUE(sizeof(SQLWCHAR)==ODBCPP_EXPECT_DM_SQLWCHAR_SIZE &&
        sizeof(SQLWCHAR)==ODBCPP_EXPECT_DRIVER_SQLWCHAR_SIZE)<<"Package requires its explicit matched SQLWCHAR pair";
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
      if(filename=="librsodbc64.dylib"||filename.starts_with("libodbcpp")||filename.find("redshiftodbc")!=std::string::npos||filename.find("psqlodbc")!=std::string::npos) {
        std::error_code error;const auto actual=std::filesystem::canonical(path,error);
        ASSERT_TRUE(!error&&actual==std::filesystem::path(driver_))<<"Mixed or unexpected driver image";
        ++matches;loaded_=actual.string();
      }
    }
    ASSERT_EQ(std::size_t{1},matches)<<"Installed driver image not uniquely observed";
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,dbc_,&stmt_));ASSERT_TRUE(cap(end_));
#endif
  }
  void direct(const char* sql) {
    if(HasFailure()) { return; }
    ASSERT_TRUE(cap(end_));ASSERT_EQ(SQL_SUCCESS,SQLExecDirectA(stmt_,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql)),SQL_NTS))<<state(SQL_HANDLE_STMT,stmt_); }
  void describe(SQLUSMALLINT column,const char* expected,SQLSMALLINT type,SQLULEN width,SQLSMALLINT expected_scale=0) {
    poison(name_);SQLSMALLINT length=-1,actual=-1,scale=-1,nullable=-1;SQLULEN size=0;
    ASSERT_TRUE(cap(end_));
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeColA(stmt_,column,name_.value.data(),static_cast<SQLSMALLINT>(name_.value.size()),&length,&actual,&size,&scale,&nullable))<<state(SQL_HANDLE_STMT,stmt_);
    guards(name_);ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length),name_.value.size());
    const std::string owned(reinterpret_cast<const char*>(name_.value.data()),static_cast<std::size_t>(length));
    EXPECT_TRUE(owned==expected);EXPECT_EQ(type,actual);EXPECT_EQ(width,size);EXPECT_EQ(expected_scale,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
    EXPECT_EQ(0,name_.value[static_cast<std::size_t>(length)]);
    descriptors_.push_back({owned,actual,scale,nullable,size});
  }
  void show_text_descriptor(SQLUSMALLINT column,const char* expected) {
    poison(name_);SQLSMALLINT length=-1,type=-1,scale=-1,nullable=-1;SQLULEN capacity=99;
    ASSERT_TRUE(cap(end_));
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeColA(stmt_,column,name_.value.data(),static_cast<SQLSMALLINT>(name_.value.size()),&length,&type,&capacity,&scale,&nullable))<<state(SQL_HANDLE_STMT,stmt_);
    guards(name_);ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length),name_.value.size());
    const std::string owned(reinterpret_cast<const char*>(name_.value.data()),static_cast<std::size_t>(length));
    EXPECT_TRUE(owned==expected);EXPECT_EQ(SQL_VARCHAR,type);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
    // SHOW carries source capacities, including unknown0, rather than legacy casts.
    EXPECT_LE(capacity,static_cast<SQLULEN>(65535));EXPECT_EQ(0,name_.value[static_cast<std::size_t>(length)]);
    descriptors_.push_back({owned,type,scale,nullable,capacity});
  }
  static void fitting_ascii(const std::optional<std::string>& value,const Descriptor& descriptor) {
    if(value&&descriptor.width!=0&&std::all_of(value->begin(),value->end(),[](unsigned char ch){return ch<128;})) {
      EXPECT_LE(static_cast<SQLULEN>(value->size()),descriptor.width);
    }
  }
  std::optional<std::string> opaque_text(SQLUSMALLINT column) {
    poison(opaque_);length_=73;
    const auto result=SQLGetData(stmt_,column,SQL_C_CHAR,opaque_.value.data(),static_cast<SQLLEN>(opaque_.value.size()),&length_);
    EXPECT_EQ(SQL_SUCCESS,result)<<state(SQL_HANDLE_STMT,stmt_);guards(opaque_);
    if(result!=SQL_SUCCESS) { return std::nullopt; }
    if(length_==SQL_NULL_DATA) {
      EXPECT_TRUE(std::all_of(opaque_.value.begin(),opaque_.value.end(),[](SQLCHAR byte){return byte==0x5a;}));
      return std::nullopt;
    }
    if(length_<0||length_>65535) { ADD_FAILURE()<<"SHOW opaque text bounds refused";return std::nullopt; }
    const auto size=static_cast<std::size_t>(length_);EXPECT_EQ(0,opaque_.value[size]);EXPECT_EQ(0x5a,opaque_.value[size+1]);
    return std::string(reinterpret_cast<const char*>(opaque_.value.data()),size);
  }
  template<std::size_t N> void wide_descriptor(SQLUSMALLINT column,const std::array<SQLWCHAR,N>& expected,SQLSMALLINT type,SQLULEN width) {
    poison(wide_name_);SQLSMALLINT length=-1,actual=-1,scale=-1,nullable=-1;SQLULEN size=0;
    ASSERT_TRUE(cap(end_));
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeColW(stmt_,column,wide_name_.value.data(),static_cast<SQLSMALLINT>(wide_name_.value.size()),&length,&actual,&size,&scale,&nullable))<<state(SQL_HANDLE_STMT,stmt_);
    guards(wide_name_);ASSERT_EQ(static_cast<SQLSMALLINT>(N-1),length);ASSERT_LT(N,wide_name_.value.size());
    EXPECT_TRUE(std::equal(expected.begin(),expected.end(),wide_name_.value.begin()));
    EXPECT_EQ(type,actual);EXPECT_EQ(width,size);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
    for(std::size_t i=N;i<wide_name_.value.size();++i) { EXPECT_EQ(static_cast<SQLWCHAR>(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE==2?0x5a5aU:0x5a5a5a5aU),wide_name_.value[i]); }
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
    if(HasFailure()) { return; }
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt_,SQL_CLOSE));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt_,SQL_UNBIND));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt_,SQL_RESET_PARAMS));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_ROW_STATUS_PTR,nullptr,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_ROWS_FETCHED_PTR,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_PARAM_STATUS_PTR,nullptr,0));ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,nullptr,0));
  }
  void disconnect() {
    if(HasFailure()) { return; }
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
  std::optional<std::string> nullable_text(SQLUSMALLINT column) {
    poison(text_);length_=73;
    const auto result=SQLGetData(stmt_,column,SQL_C_CHAR,text_.value.data(),static_cast<SQLLEN>(text_.value.size()),&length_);
    EXPECT_EQ(SQL_SUCCESS,result)<<state(SQL_HANDLE_STMT,stmt_);guards(text_);
    if(result!=SQL_SUCCESS) { return std::nullopt; }
    if(length_==SQL_NULL_DATA) {
      for(const auto byte:text_.value) { EXPECT_EQ(0x5a,byte); }
      return std::nullopt;
    }
    if(length_<0||static_cast<std::size_t>(length_)>=text_.value.size()) { ADD_FAILURE()<<"Import text bounds refused";return std::nullopt; }
    EXPECT_EQ(0,text_.value[static_cast<std::size_t>(length_)]);
    return std::string(reinterpret_cast<const char*>(text_.value.data()),static_cast<std::size_t>(length_));
  }
  std::optional<std::string> nullable_number(SQLUSMALLINT column) {
    poison(integer_);length_=73;
    const auto result=SQLGetData(stmt_,column,SQL_C_SLONG,&integer_.value,sizeof(integer_.value),&length_);
    EXPECT_EQ(SQL_SUCCESS,result)<<state(SQL_HANDLE_STMT,stmt_);guards(integer_);
    if(result!=SQL_SUCCESS) { return std::nullopt; }
    if(length_==SQL_NULL_DATA) { EXPECT_EQ(static_cast<SQLINTEGER>(0x5a5a5a5a),integer_.value);return std::nullopt; }
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(integer_.value)),length_);
    return std::to_string(integer_.value);
  }
  void wide_column_name(bool value_column,std::optional<std::string>& owned) {
    poison(wide_);length_=73;
    ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt_,4,SQL_C_WCHAR,wide_.value.data(),static_cast<SQLLEN>(sizeof(wide_.value)),&length_));guards(wide_);
    constexpr std::array<SQLWCHAR,3> id_name{'i','d',0};
    constexpr std::array<SQLWCHAR,6> value_name{'v','a','l','u','e',0};
    const auto units=value_column?std::size_t{5}:std::size_t{2};
    // Indicators are bytes; DescribeColW lengths above remain character units.
    // Independent literals for ASCII "value" (5 units) and "id" (2 units).
    constexpr SQLLEN value_bytes=ODBCPP_EXPECT_DM_SQLWCHAR_SIZE==4?20:10;
    constexpr SQLLEN id_bytes=ODBCPP_EXPECT_DM_SQLWCHAR_SIZE==4?8:4;
    EXPECT_EQ(value_column?value_bytes:id_bytes,length_);
    if(value_column) { EXPECT_TRUE(std::equal(value_name.begin(),value_name.end(),wide_.value.begin())); }
    else { EXPECT_TRUE(std::equal(id_name.begin(),id_name.end(),wide_.value.begin())); }
    for(std::size_t i=units+1;i<wide_.value.size();++i) { EXPECT_EQ(static_cast<SQLWCHAR>(ODBCPP_EXPECT_DM_SQLWCHAR_SIZE==2?0x5a5aU:0x5a5a5a5aU),wide_.value[i]); }
    ASSERT_FALSE(HasFailure());std::string actual;
    for(std::size_t i=0;i<units;++i) { ASSERT_LE(wide_.value[i],static_cast<SQLWCHAR>(127));actual+=static_cast<char>(wide_.value[i]); }
    owned=std::move(actual);
  }
  void result_count(SQLSMALLINT expected) {
    if(HasFailure()) { return; }
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumResultCols(stmt_,&count));ASSERT_EQ(expected,count);
  }
  void fetch_row() {
    if(HasFailure()) { return; }
    ASSERT_TRUE(cap(end_));fetched_=99;row_status_=SQL_ROW_NOROW;
    ASSERT_EQ(SQL_SUCCESS,SQLFetch(stmt_))<<state(SQL_HANDLE_STMT,stmt_);
    EXPECT_EQ(static_cast<SQLULEN>(1),fetched_);EXPECT_EQ(SQL_ROW_SUCCESS,row_status_);
  }
  void fetch_end() {
    if(HasFailure()) { return; }
    ASSERT_TRUE(cap(end_));fetched_=99;row_status_=SQL_ROW_NOROW;
    ASSERT_EQ(SQL_NO_DATA,SQLFetch(stmt_));EXPECT_EQ(static_cast<SQLULEN>(0),fetched_);EXPECT_EQ(SQL_ROW_NOROW,row_status_);
  }
  void row_status() {
    if(HasFailure()) { return; }
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_ROW_STATUS_PTR,&row_status_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_ROWS_FETCHED_PTR,&fetched_,0));
  }
  void prepare(const std::string& sql) {
    if(HasFailure()) { return; }
    ASSERT_TRUE(cap(end_));
    ASSERT_EQ(SQL_SUCCESS,SQLPrepareA(stmt_,reinterpret_cast<SQLCHAR*>(const_cast<char*>(sql.c_str())),SQL_NTS))<<state(SQL_HANDLE_STMT,stmt_);
    SQLSMALLINT count=-1;ASSERT_EQ(SQL_SUCCESS,SQLNumParams(stmt_,&count));ASSERT_EQ(1,count);
  }
  void bind_minimum(SQLINTEGER value) {
    if(HasFailure()) { return; }
    minimum_=value;minimum_length_=static_cast<SQLLEN>(sizeof(minimum_));processed_=99;parameter_status_=SQL_PARAM_UNUSED;
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_PARAMS_PROCESSED_PTR,&processed_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt_,SQL_ATTR_PARAM_STATUS_PTR,&parameter_status_,0));
    ASSERT_EQ(SQL_SUCCESS,SQLBindParameter(stmt_,1,SQL_PARAM_INPUT,SQL_C_SLONG,SQL_INTEGER,10,0,&minimum_,sizeof(minimum_),&minimum_length_));
  }
  void execute() {
    if(HasFailure()) { return; }
    ASSERT_TRUE(cap(end_));ASSERT_EQ(SQL_SUCCESS,SQLExecute(stmt_))<<state(SQL_HANDLE_STMT,stmt_);
    EXPECT_EQ(static_cast<SQLULEN>(1),processed_);EXPECT_EQ(SQL_PARAM_SUCCESS,parameter_status_);
    // The SDK owns material after the synchronous execution returns.
    minimum_=97;minimum_length_=97;
  }
  void recovery() {
    if(HasFailure()) { return; }
    direct("SELECT 1 AS recovery");ASSERT_FALSE(HasFailure());result_count(1);describe(1,"recovery",SQL_INTEGER,10);row_status();ASSERT_FALSE(HasFailure());
    fetch_row();ASSERT_FALSE(HasFailure());integer(1);ASSERT_FALSE(HasFailure());fetch_end();reset();ASSERT_FALSE(HasFailure());
  }
  static std::string quoted(const std::string& name) {
    std::string result="\"";
    for(const auto byte:name) { if(byte=='\"') { result+='\"'; }result+=byte; }
    result+='\"';return result;
  }
  void numeric_profile() {
    if(HasFailure()) { return; }
#if ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == 4
    // DISTINCT iODBC bound-fetch route; the observed unbound SQL_ARD_TYPE
    // S1003 route remains unqualified.
    ASSERT_EQ(SQL_SUCCESS,SQLBindCol(stmt_,2,SQL_C_NUMERIC,&numeric_.value,sizeof(numeric_.value),&numeric_indicator_));
#endif
    ASSERT_EQ(SQL_SUCCESS,SQLGetStmtAttr(stmt_,SQL_ATTR_APP_ROW_DESC,&ard_,0,nullptr));
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,2,SQL_DESC_CONCISE_TYPE,reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(SQL_C_NUMERIC)),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,2,SQL_DESC_PRECISION,reinterpret_cast<SQLPOINTER>(std::intptr_t{5}),0));
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,2,SQL_DESC_SCALE,reinterpret_cast<SQLPOINTER>(std::intptr_t{2}),0));
#if ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == 4
    // Descriptor edits invalidate DATA_PTR; restore the persistent binding last.
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,2,SQL_DESC_DATA_PTR,&numeric_.value,0));
#else
    // Descriptor edits invalidate DATA_PTR; this is deliberately unbound GetData.
    ASSERT_EQ(SQL_SUCCESS,SQLSetDescField(ard_,2,SQL_DESC_DATA_PTR,nullptr,0));
#endif
  }
};

TEST_F(RedshiftPackageImportLiveTest, CatalogDescriptorsDrivePreparedImport) {
  SQLCHAR database[]="odbcpp_pilot",schema_pattern[]="odbcpp\\_fixture",table_pattern[]="pilot\\_rows",kind[]="TABLE";
  ASSERT_TRUE(cap(end_));
  ASSERT_EQ(SQL_SUCCESS,SQLTablesA(stmt_,database,SQL_NTS,schema_pattern,SQL_NTS,table_pattern,SQL_NTS,kind,SQL_NTS))<<state(SQL_HANDLE_STMT,stmt_);
  result_count(5);ASSERT_FALSE(HasFailure());
  constexpr std::array<const char*,5> table_names{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","TABLE_TYPE","REMARKS"};
  descriptors_.clear();
  for(SQLUSMALLINT column=1;column<=5;++column) { show_text_descriptor(column,table_names[column-1]);ASSERT_FALSE(HasFailure()); }
  const auto table_descriptors=descriptors_;row_status();ASSERT_FALSE(HasFailure());fetch_row();ASSERT_FALSE(HasFailure());
  for(SQLUSMALLINT column=1;column<=5;++column) { table_[column-1]=column==5?opaque_text(column):nullable_text(column);ASSERT_FALSE(HasFailure());fitting_ascii(table_[column-1],table_descriptors[column-1]); }
  EXPECT_TRUE(table_[0]==std::optional<std::string>{"odbcpp_pilot"});EXPECT_TRUE(table_[1]==std::optional<std::string>{"odbcpp_fixture"});
  EXPECT_TRUE(table_[2]==std::optional<std::string>{"pilot_rows"});EXPECT_TRUE(table_[3]==std::optional<std::string>{"TABLE"}); // SHOW preserves actual nullable source remarks.
  ASSERT_FALSE(HasFailure());fetch_end();reset();ASSERT_FALSE(HasFailure());const auto owned_table=table_;
  // Literal wide SQLWCHAR ASCII arguments are independent of the implementation's converter.
  std::array<SQLWCHAR,13> database_w{'o','d','b','c','p','p','_','p','i','l','o','t',0};
  std::array<SQLWCHAR,16> schema_w{'o','d','b','c','p','p','\\','_','f','i','x','t','u','r','e',0};
  std::array<SQLWCHAR,12> table_w{'p','i','l','o','t','\\','_','r','o','w','s',0};
  ASSERT_TRUE(cap(end_));
  ASSERT_EQ(SQL_SUCCESS,SQLColumnsW(stmt_,database_w.data(),SQL_NTS,schema_w.data(),SQL_NTS,table_w.data(),SQL_NTS,nullptr,0))<<state(SQL_HANDLE_STMT,stmt_);
  result_count(18);ASSERT_FALSE(HasFailure());
  constexpr std::array<const char*,18> names{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","COLUMN_NAME","DATA_TYPE","TYPE_NAME","COLUMN_SIZE","BUFFER_LENGTH","DECIMAL_DIGITS","NUM_PREC_RADIX","NULLABLE","REMARKS","COLUMN_DEF","SQL_DATA_TYPE","SQL_DATETIME_SUB","CHAR_OCTET_LENGTH","ORDINAL_POSITION","IS_NULLABLE"};
  constexpr std::array<SQLSMALLINT,18> types{SQL_VARCHAR,SQL_VARCHAR,SQL_VARCHAR,SQL_VARCHAR,SQL_SMALLINT,SQL_VARCHAR,SQL_INTEGER,SQL_INTEGER,SQL_SMALLINT,SQL_SMALLINT,SQL_SMALLINT,SQL_VARCHAR,SQL_VARCHAR,SQL_SMALLINT,SQL_SMALLINT,SQL_INTEGER,SQL_INTEGER,SQL_VARCHAR};
  descriptors_.clear();
  for(SQLUSMALLINT column=1;column<=18;++column) {
    poison(name_);SQLSMALLINT length=-1,type=-1,scale=-1,nullable=-1;SQLULEN width=99;
    ASSERT_EQ(SQL_SUCCESS,SQLDescribeColA(stmt_,column,name_.value.data(),static_cast<SQLSMALLINT>(name_.value.size()),&length,&type,&width,&scale,&nullable));
    guards(name_);ASSERT_GE(length,0);ASSERT_LT(static_cast<std::size_t>(length),name_.value.size());
    const std::string name(reinterpret_cast<const char*>(name_.value.data()),static_cast<std::size_t>(length));
    EXPECT_TRUE(name==names[column-1]);EXPECT_EQ(0,name_.value[static_cast<std::size_t>(length)]);EXPECT_EQ(types[column-1],type);EXPECT_EQ(0,scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,nullable);
    if(type==SQL_SMALLINT) { EXPECT_EQ(static_cast<SQLULEN>(5),width); }
    else if(type==SQL_INTEGER) { EXPECT_EQ(static_cast<SQLULEN>(10),width); }
    else { EXPECT_LE(width,static_cast<SQLULEN>(65535)); }
    descriptors_.push_back({name,type,scale,nullable,width});ASSERT_FALSE(HasFailure());
  }
  const auto column_descriptors=descriptors_;
  row_status();ASSERT_FALSE(HasFailure());
  for(std::size_t row=0;row<2;++row) {
    fetch_row();ASSERT_FALSE(HasFailure());
    for(SQLUSMALLINT column=1;column<=18;++column) {
      if(column==4) { wide_column_name(row==1,columns_[row][column-1]); }
      else if(column==12||column==13) { columns_[row][column-1]=opaque_text(column); }
      else { columns_[row][column-1]=types[column-1]==SQL_VARCHAR?nullable_text(column):nullable_number(column); }
      if(types[column-1]==SQL_VARCHAR) { fitting_ascii(columns_[row][column-1],column_descriptors[column-1]); }
      ASSERT_FALSE(HasFailure());
    }
    const auto& values=columns_[row];const bool second=row==1;
    EXPECT_TRUE(values[0]==std::optional<std::string>{"odbcpp_pilot"});EXPECT_TRUE(values[1]==owned_table[1]);EXPECT_TRUE(values[2]==owned_table[2]);
    EXPECT_TRUE(values[3]==std::optional<std::string>{second?"value":"id"});EXPECT_TRUE(values[4]==std::optional<std::string>{second?"12":"4"});
    EXPECT_TRUE(values[5]==std::optional<std::string>{second?"character varying":"integer"});EXPECT_TRUE(values[6]==std::optional<std::string>{second?"32":"10"});EXPECT_TRUE(values[7]==std::optional<std::string>{second?"32":"4"});
    if(second) { EXPECT_FALSE(values[8].has_value());EXPECT_FALSE(values[9].has_value()); }
    else { EXPECT_TRUE(values[8]==std::optional<std::string>{"0"});EXPECT_TRUE(values[9]==std::optional<std::string>{"10"}); }
    EXPECT_TRUE(values[10]==std::optional<std::string>{"1"});
    // SHOW remarks/defaults retain their actual owning NULLness/text, not guessed NULL.
    EXPECT_TRUE(values[13]==values[4]);EXPECT_FALSE(values[14].has_value());
    if(second) { EXPECT_TRUE(values[15]==std::optional<std::string>{"32"}); } else { EXPECT_FALSE(values[15].has_value()); }
    EXPECT_TRUE(values[16]==std::optional<std::string>{second?"2":"1"});EXPECT_TRUE(values[17]==std::optional<std::string>{"YES"});ASSERT_FALSE(HasFailure());
  }
  fetch_end();reset();ASSERT_FALSE(HasFailure());const auto owned_columns=columns_;
  const auto saved_table=owned_table;const auto saved_columns=owned_columns;
  ASSERT_TRUE(owned_table[1].has_value()&&owned_table[2].has_value()&&owned_columns[0][3].has_value()&&owned_columns[1][3].has_value());
  const auto sql="SELECT "+quoted(*owned_columns[0][3])+","+quoted(*owned_columns[1][3])+" FROM "+quoted(*owned_table[1])+"."+quoted(*owned_table[2])+" WHERE "+quoted(*owned_columns[0][3])+">=? ORDER BY "+quoted(*owned_columns[0][3]);
  prepare(sql);bind_minimum(1);execute();ASSERT_FALSE(HasFailure());result_count(2);descriptors_.clear();describe(1,"id",SQL_INTEGER,10);describe(2,"value",SQL_VARCHAR,32);
  constexpr std::array<SQLWCHAR,6> value_name{'v','a','l','u','e',0};wide_descriptor(2,value_name,SQL_VARCHAR,32);row_status();ASSERT_FALSE(HasFailure());
  std::array<SQLINTEGER,2> ids{};std::array<std::string,2> values;
  for(std::size_t row=0;row<2;++row) {
    fetch_row();ASSERT_FALSE(HasFailure());integer(static_cast<SQLINTEGER>(row+1));ASSERT_FALSE(HasFailure());ids[row]=integer_.value;
    values[row]=text(2);EXPECT_TRUE(values[row]==(row==0?"one":"two"));ASSERT_FALSE(HasFailure());
  }
  fetch_end();reset();recovery();disconnect();ASSERT_FALSE(HasFailure());
  table_.fill(std::nullopt);for(auto& row:columns_) { row.fill(std::nullopt); }descriptors_.clear();poison(opaque_);
  EXPECT_EQ(static_cast<SQLINTEGER>(1),ids[0]);EXPECT_EQ(static_cast<SQLINTEGER>(2),ids[1]);EXPECT_TRUE(values[0]=="one"&&values[1]=="two");
  EXPECT_TRUE(owned_table[1]==std::optional<std::string>{"odbcpp_fixture"}&&owned_table[2]==std::optional<std::string>{"pilot_rows"});
  EXPECT_TRUE(owned_columns[0][3]==std::optional<std::string>{"id"}&&owned_columns[1][3]==std::optional<std::string>{"value"});
  const std::array<std::optional<std::string>,18> expected_id{"odbcpp_pilot","odbcpp_fixture","pilot_rows","id","4","integer","10","4","0","10","1",saved_columns[0][11],saved_columns[0][12],"4",std::nullopt,std::nullopt,"1","YES"};
  const std::array<std::optional<std::string>,18> expected_value{"odbcpp_pilot","odbcpp_fixture","pilot_rows","value","12","character varying","32","32",std::nullopt,std::nullopt,"1",saved_columns[1][11],saved_columns[1][12],"12",std::nullopt,"32","2","YES"};
  for(std::size_t field=0;field<18;++field) {
    EXPECT_TRUE(owned_columns[0][field]==expected_id[field]);EXPECT_TRUE(owned_columns[1][field]==expected_value[field]);
  }
  // Opaque remarks/defaults retain captured NULLness and bytes through disconnect.
  for(const auto& row:owned_columns) {
    for(const auto field:{std::size_t{11},std::size_t{12}}) { if(row[field]) { EXPECT_LE(row[field]->size(),std::size_t{65535}); } }
  }
  ASSERT_EQ(std::size_t{5},table_descriptors.size());ASSERT_EQ(std::size_t{18},column_descriptors.size());
  for(std::size_t field=0;field<18;++field) { EXPECT_TRUE(column_descriptors[field].name==names[field]);EXPECT_EQ(types[field],column_descriptors[field].type);EXPECT_EQ(0,column_descriptors[field].scale);EXPECT_EQ(SQL_NULLABLE_UNKNOWN,column_descriptors[field].nullable); }
  EXPECT_TRUE(owned_table==saved_table);EXPECT_TRUE(owned_columns==saved_columns);
  EXPECT_TRUE(table_descriptors[2].name=="TABLE_NAME");publish();
}

TEST_F(RedshiftPackageImportLiveTest, TypedImportedRowsPreserveDecimalTemporalNull) {
  direct("SELECT id,CAST(CASE WHEN id=1 THEN '123.45' ELSE '-123.45' END AS DECIMAL(5,2)) AS amount,CASE WHEN id=1 THEN CAST('2000-02-29' AS DATE) ELSE CAST(NULL AS DATE) END AS day,CASE WHEN id=1 THEN CAST('2000-02-29 12:34:56.123456' AS TIMESTAMP) ELSE CAST(NULL AS TIMESTAMP) END AS stamp,CASE WHEN id=1 THEN value ELSE CAST(NULL AS VARCHAR(32)) END AS optional_value FROM \"odbcpp_fixture\".\"pilot_rows\" ORDER BY id");ASSERT_FALSE(HasFailure());
  result_count(5);descriptors_.clear();describe(1,"id",SQL_INTEGER,10);describe(2,"amount",SQL_NUMERIC,5,2);describe(3,"day",SQL_TYPE_DATE,10);describe(4,"stamp",SQL_TYPE_TIMESTAMP,26,6);describe(5,"optional_value",SQL_VARCHAR,32);row_status();numeric_profile();ASSERT_FALSE(HasFailure());
  const auto owned_descriptors=descriptors_;std::array<SQL_NUMERIC_STRUCT,2> amounts{};SQL_DATE_STRUCT owned_day{};SQL_TIMESTAMP_STRUCT owned_stamp{};std::array<std::optional<std::string>,2> strings;
  constexpr std::array<SQLCHAR,16> magnitude{0x39,0x30,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  for(std::size_t row=0;row<2;++row) {
#if ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == 4
    poison(numeric_);numeric_indicator_=73; // Poison BEFORE bound SQLFetch.
#endif
    fetch_row();ASSERT_FALSE(HasFailure());integer(static_cast<SQLINTEGER>(row+1));ASSERT_FALSE(HasFailure());
#if ODBCPP_EXPECT_DM_SQLWCHAR_SIZE == 4
    // id GetData is unbound and supported by SQL_GD_ANY_COLUMN. No GetData on
    // the bound numeric column: no SQL_GD_BOUND claim or descriptor bypass.
    length_=numeric_indicator_;
#else
    poison(numeric_);length_=73;ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt_,2,SQL_ARD_TYPE,&numeric_.value,sizeof(numeric_.value),&length_))<<state(SQL_HANDLE_STMT,stmt_);
#endif
    guards(numeric_);EXPECT_EQ(static_cast<SQLLEN>(sizeof(numeric_.value)),length_);EXPECT_EQ(5,numeric_.value.precision);EXPECT_EQ(2,numeric_.value.scale);EXPECT_EQ(row==0?1:0,numeric_.value.sign);EXPECT_TRUE(std::equal(magnitude.begin(),magnitude.end(),numeric_.value.val));amounts[row]=numeric_.value;
    poison(day_);length_=73;ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt_,3,SQL_C_TYPE_DATE,&day_.value,sizeof(day_.value),&length_));guards(day_);
    if(row==0) { EXPECT_EQ(2000,day_.value.year);EXPECT_EQ(2,day_.value.month);EXPECT_EQ(29,day_.value.day);EXPECT_EQ(static_cast<SQLLEN>(sizeof(day_.value)),length_);owned_day=day_.value; }
    else { EXPECT_EQ(SQL_NULL_DATA,length_);const auto* bytes=reinterpret_cast<const unsigned char*>(&day_.value);EXPECT_TRUE(std::all_of(bytes,bytes+sizeof(day_.value),[](auto byte){return byte==0x5a;})); }
    poison(stamp_);length_=73;ASSERT_EQ(SQL_SUCCESS,SQLGetData(stmt_,4,SQL_C_TYPE_TIMESTAMP,&stamp_.value,sizeof(stamp_.value),&length_));guards(stamp_);
    if(row==0) { EXPECT_EQ(2000,stamp_.value.year);EXPECT_EQ(2,stamp_.value.month);EXPECT_EQ(29,stamp_.value.day);EXPECT_EQ(12,stamp_.value.hour);EXPECT_EQ(34,stamp_.value.minute);EXPECT_EQ(56,stamp_.value.second);EXPECT_EQ(static_cast<SQLUINTEGER>(123456000),stamp_.value.fraction);EXPECT_EQ(static_cast<SQLLEN>(sizeof(stamp_.value)),length_);owned_stamp=stamp_.value; }
    else { EXPECT_EQ(SQL_NULL_DATA,length_);const auto* bytes=reinterpret_cast<const unsigned char*>(&stamp_.value);EXPECT_TRUE(std::all_of(bytes,bytes+sizeof(stamp_.value),[](auto byte){return byte==0x5a;})); }
    strings[row]=nullable_text(5);if(row==0) { EXPECT_TRUE(strings[row]==std::optional<std::string>{"one"}); } else { EXPECT_FALSE(strings[row].has_value()); }ASSERT_FALSE(HasFailure());
  }
  fetch_end();reset();recovery();disconnect();ASSERT_FALSE(HasFailure());poison(numeric_);poison(day_);poison(stamp_);poison(text_);descriptors_.clear();
  EXPECT_EQ(1,amounts[0].sign);EXPECT_EQ(0,amounts[1].sign);for(const auto& amount:amounts) { EXPECT_TRUE(std::equal(magnitude.begin(),magnitude.end(),amount.val));EXPECT_EQ(5,amount.precision);EXPECT_EQ(2,amount.scale); }
  EXPECT_EQ(2000,owned_day.year);EXPECT_EQ(2,owned_day.month);EXPECT_EQ(29,owned_day.day);EXPECT_EQ(static_cast<SQLUINTEGER>(123456000),owned_stamp.fraction);EXPECT_TRUE(strings[0]==std::optional<std::string>{"one"}&&!strings[1].has_value());ASSERT_EQ(std::size_t{5},owned_descriptors.size());EXPECT_TRUE(owned_descriptors[1].name=="amount"&&owned_descriptors[3].name=="stamp");publish();
}

TEST_F(RedshiftPackageImportLiveTest, PreparedRefreshRebindAndEarlyCloseOwnRows) {
  prepare("SELECT id,value,CASE WHEN id=2 THEN CAST(NULL AS VARCHAR(32)) ELSE value END AS optional_value FROM \"odbcpp_fixture\".\"pilot_rows\" WHERE id>=? ORDER BY id");bind_minimum(1);execute();ASSERT_FALSE(HasFailure());
  result_count(3);descriptors_.clear();describe(1,"id",SQL_INTEGER,10);describe(2,"value",SQL_VARCHAR,32);describe(3,"optional_value",SQL_VARCHAR,32);row_status();ASSERT_FALSE(HasFailure());
  fetch_row();ASSERT_FALSE(HasFailure());integer(1);ASSERT_FALSE(HasFailure());const auto first_id=integer_.value;const auto first_value=text(2);const auto first_optional=nullable_text(3);EXPECT_TRUE(first_value=="one"&&first_optional==std::optional<std::string>{"one"});ASSERT_FALSE(HasFailure());
  // Close before row2, then reset only application bindings; retain this prepare.
  reset();ASSERT_FALSE(HasFailure());bind_minimum(2);execute();ASSERT_FALSE(HasFailure());
  result_count(3);describe(1,"id",SQL_INTEGER,10);describe(2,"value",SQL_VARCHAR,32);describe(3,"optional_value",SQL_VARCHAR,32);row_status();ASSERT_FALSE(HasFailure());
  fetch_row();ASSERT_FALSE(HasFailure());integer(2);ASSERT_FALSE(HasFailure());const auto refreshed_id=integer_.value;const auto refreshed_value=text(2);const auto refreshed_optional=nullable_text(3);EXPECT_TRUE(refreshed_value=="two");EXPECT_FALSE(refreshed_optional.has_value());ASSERT_FALSE(HasFailure());
  fetch_end();reset();recovery();disconnect();ASSERT_FALSE(HasFailure());poison(integer_);poison(text_);minimum_=99;
  EXPECT_EQ(static_cast<SQLINTEGER>(1),first_id);EXPECT_TRUE(first_value=="one"&&first_optional==std::optional<std::string>{"one"});EXPECT_EQ(static_cast<SQLINTEGER>(2),refreshed_id);EXPECT_TRUE(refreshed_value=="two"&&!refreshed_optional.has_value());publish();
}
} // namespace
