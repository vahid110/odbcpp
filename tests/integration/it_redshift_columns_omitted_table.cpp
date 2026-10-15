#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include "odbc/connection_string.h"
#include "odbcpp/database/backend_provider.h"
#include "tests/test_connection_config.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
// Compile-time fixture admission witnesses; none dispatch SQL or create objects.
constexpr std::uintptr_t remaining_seconds(std::chrono::milliseconds remaining) {
  const auto seconds=std::chrono::duration_cast<std::chrono::seconds>(remaining).count();
  return seconds>0?static_cast<std::uintptr_t>(seconds<5?seconds:5):0;
}
constexpr bool can_drop_owned(bool created,bool completion_known,std::uintptr_t seconds) {
  return created && completion_known && seconds>0;
}
static_assert(remaining_seconds(std::chrono::milliseconds{-1})==0);
static_assert(remaining_seconds(std::chrono::milliseconds{999})==0);
static_assert(remaining_seconds(std::chrono::milliseconds{1000})==1);
static_assert(remaining_seconds(std::chrono::milliseconds{4999})==4);
static_assert(remaining_seconds(std::chrono::milliseconds{40000})==5);
static_assert(!can_drop_owned(false,true,5));
static_assert(!can_drop_owned(true,false,5));
static_assert(!can_drop_owned(true,true,0));
static_assert(can_drop_owned(true,true,1));


enum class LocalRefusal { None, MetadataNull, ColumnPattern, TablePattern };
// Only the deliberate, source-proven pre-dispatch refusals used below recover.
constexpr bool known_completion(SQLRETURN result, std::string_view state = {},
    std::string_view message = {}, bool live = false,
    LocalRefusal expected = LocalRefusal::None) {
  if (result==SQL_SUCCESS || result==SQL_SUCCESS_WITH_INFO) {
    return true;
  }
  if (result!=SQL_ERROR || !live) {
    return false;
  }
  return (expected==LocalRefusal::MetadataNull && state=="HY009" &&
          message=="Metadata identifier argument is required") ||
      (expected==LocalRefusal::ColumnPattern && state=="HY000" &&
          message=="Invalid schema/table/column patterns") ||
      (expected==LocalRefusal::TablePattern && state=="HY000" &&
          message=="Invalid table catalog patterns");
}
static_assert(known_completion(SQL_SUCCESS));
static_assert(known_completion(SQL_SUCCESS_WITH_INFO));
static_assert(!known_completion(SQL_ERROR,"HY000","generic error",true));
static_assert(!known_completion(SQL_ERROR,"40003","completion unknown",true));
static_assert(!known_completion(SQL_ERROR,"ZZ999","unclassified",true));
static_assert(!known_completion(SQL_ERROR,"HY000","generic error",true,LocalRefusal::ColumnPattern));
static_assert(!known_completion(SQL_ERROR,"40003","Invalid table catalog patterns",true,LocalRefusal::TablePattern));
static_assert(!known_completion(SQL_ERROR,"HY009","Metadata identifier argument is required",true));
static_assert(!known_completion(SQL_ERROR,"HY009","Metadata identifier argument is required",false,LocalRefusal::MetadataNull));
static_assert(!known_completion(SQL_ERROR,"HYT00","Invalid schema/table/column patterns",true,LocalRefusal::ColumnPattern));
static_assert(!known_completion(SQL_ERROR,"08S01","Invalid table catalog patterns",true,LocalRefusal::TablePattern));
static_assert(known_completion(SQL_ERROR,"HY009","Metadata identifier argument is required",true,LocalRefusal::MetadataNull));
static_assert(known_completion(SQL_ERROR,"HY000","Invalid schema/table/column patterns",true,LocalRefusal::ColumnPattern));
static_assert(known_completion(SQL_ERROR,"HY000","Invalid table catalog patterns",true,LocalRefusal::TablePattern));
static_assert(!known_completion(SQL_STILL_EXECUTING));

std::string quoted_identifier(std::string_view value) {
  std::string out{"\""};for(char ch:value){if(ch=='"')out+='"';out+=ch;}return out+'"';
}
std::string literal_pattern(std::string_view value) {
  std::string out;for(char ch:value){if(ch=='\\' || ch=='%' || ch=='_')out+='\\';out+=ch;}return out;
}
SQLCHAR* chars(const std::string& text) {return reinterpret_cast<SQLCHAR*>(const_cast<char*>(text.data()));}
class ColumnsOmittedTableNative : public testing::TestWithParam<bool> {
 protected:
  SQLHENV env{SQL_NULL_HENV};SQLHDBC dbc{SQL_NULL_HDBC};SQLHSTMT stmt{SQL_NULL_HSTMT};
  std::string schema,database;bool created=false;
  std::vector<std::pair<std::string,bool>> created_children;
  const std::vector<std::string> tables{"a_%\\quoted\"雪","z_table","v_view"};
  const std::chrono::steady_clock::time_point window_end_=
      std::chrono::steady_clock::now()+std::chrono::seconds{40};
  bool connected_{false},completion_known_{true};
  std::uintptr_t seconds_left() const {
    return remaining_seconds(std::chrono::duration_cast<std::chrono::milliseconds>(
        window_end_-std::chrono::steady_clock::now()));
  }
  bool cap(bool statement=true,bool login=false) {
    const auto seconds=seconds_left();
    if (!completion_known_ || seconds==0) {
      ADD_FAILURE()<<"Opt-in fixture original window exhausted or completion unknown";
      return false;
    }
    const auto value=reinterpret_cast<SQLPOINTER>(seconds);
    if (SQLSetConnectAttr(dbc,SQL_ATTR_CONNECTION_TIMEOUT,value,0)!=SQL_SUCCESS ||
        (login && SQLSetConnectAttr(dbc,SQL_ATTR_LOGIN_TIMEOUT,value,0)!=SQL_SUCCESS) ||
        (statement && SQLSetStmtAttr(stmt,SQL_ATTR_QUERY_TIMEOUT,value,0)!=SQL_SUCCESS)) {
      completion_known_=false;
      ADD_FAILURE()<<"Opt-in fixture could not apply remaining operation budget";
      return false;
    }
    return true;
  }
  void require_tls_profile() {
    const auto resolved=rs::odbc::ConnectionString::resolve(
        odbcpp::test::configured_connection_string(),
        std::string(rs::core::database::configured_backend_provider().identity().driver_name));
    const auto& fields=resolved.effective_parameters;
    ASSERT_TRUE(fields.contains("SSL") && fields.contains("SSLCAFILE"));
    const auto ssl=rs::odbc::ConnectionString::to_upper(
        rs::odbc::ConnectionString::trim(fields.at("SSL")));
    ASSERT_TRUE(ssl=="1" || ssl=="TRUE" || ssl=="YES" || ssl=="ON");
    ASSERT_TRUE(!fields.at("SSLCAFILE").empty() &&
        fields.at("SSLCAFILE").find('\0')==std::string::npos);
    ASSERT_TRUE(fields.contains("DATABASE") && fields.at("DATABASE")=="odbcpp_pilot");
    ASSERT_TRUE(fields.contains("UID") && fields.at("UID")=="odbcpp_pilot_test");
    auto maximum=rs::core::database::ResultLimits{}.max_results;
    if (const auto entry=fields.find("MAXRESULTS");entry!=fields.end()) {
      const auto& text=entry->second;
      const auto parsed=std::from_chars(text.data(),text.data()+text.size(),maximum);
      ASSERT_TRUE(!text.empty() && parsed.ec==std::errc{} &&
          parsed.ptr==text.data()+text.size() && maximum>0);
    }
    RecordProperty("profile_max_results",std::to_string(maximum));
  }
  SQLRETURN observe(SQLRETURN result,LocalRefusal expected=LocalRefusal::None) {
    if (result==SQL_SUCCESS || result==SQL_SUCCESS_WITH_INFO) {
      return result;
    }
    const bool previously_known=completion_known_;
    completion_known_=false; // Fence first, before inspecting any error evidence.
    if (previously_known && result==SQL_ERROR && expected!=LocalRefusal::None) {
      std::array<SQLCHAR,6> state{};
      std::array<SQLCHAR,256> message{};
      SQLSMALLINT length=0;
      const auto diagnostic=SQLGetDiagRec(SQL_HANDLE_STMT,stmt,1,state.data(),nullptr,
          message.data(),message.size(),&length);
      // Truncated/missing diagnostics and unrelated errors remain unknown. Only
      // these exact calls fail local validation before any backend exchange.
      if (diagnostic==SQL_SUCCESS && state[5]==0 && length>=0 &&
          static_cast<std::size_t>(length)<message.size() &&
          known_completion(result,reinterpret_cast<const char*>(state.data()),
              std::string_view(reinterpret_cast<const char*>(message.data()),length),true,expected)) {
        SQLUINTEGER dead=SQL_CD_TRUE;
        if (SQLGetConnectAttr(dbc,SQL_ATTR_CONNECTION_DEAD,&dead,sizeof(dead),nullptr)==SQL_SUCCESS &&
            dead==SQL_CD_FALSE) {
          completion_known_=true;
        }
      }
    }
    return result;
  }
  SQLRETURN execute_fixture(const std::string& sql) {
    if (!cap()) {
      return SQL_ERROR;
    }
    return observe(SQLExecDirect(stmt,chars(sql),SQL_NTS));
  }
  SQLRETURN create_child(const std::string& ddl,const std::string& name,bool view=false) {
    if (!created) {
      ADD_FAILURE()<<"Child creation requires this fixture's successful schema CREATE";
      return SQL_ERROR;
    }
    const auto result=execute_fixture(ddl);
    if (SQL_SUCCEEDED(result)) {
      created_children.emplace_back(name,view);
      RecordProperty("owned_child_"+std::to_string(created_children.size()),
          quoted_identifier(schema)+"."+quoted_identifier(name));
      EXPECT_EQ(name,created_children.back().first);
    }
    return result;
  }
  void SetUp() override {
    // This is only an execution guard. Protected checkpoint authority must
    // separately admit this exact bounded fixture and its Redshift principal.
    const auto* enabled=std::getenv("ODBCPP_REDSHIFT_COLUMNS_OMITTED_TABLE_NATIVE");
    if (!enabled || std::string(enabled)!="1") {
      GTEST_SKIP()<<"Separately admitted real Redshift test";
    }
    ASSERT_NO_FATAL_FAILURE(require_tls_profile());
    ASSERT_GE(seconds_left(),1u);ASSERT_LE(seconds_left(),5u);
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_ENV,SQL_NULL_HANDLE,&env));
    ASSERT_EQ(SQL_SUCCESS,SQLSetEnvAttr(env,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),0));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_DBC,env,&dbc));
    ASSERT_TRUE(cap(false,true));
    const auto login_result=SQLConnect(dbc,odbcpp::test::configured_connection_string_data(),SQL_NTS,nullptr,0,nullptr,0);
    connected_=SQL_SUCCEEDED(login_result);
    ASSERT_EQ(SQL_SUCCESS,login_result);
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,dbc,&stmt));
    ASSERT_TRUE(cap());
    std::array<char,1025> name{};SQLSMALLINT length=0;
    ASSERT_EQ(SQL_SUCCESS,SQLGetInfo(dbc,SQL_DATABASE_NAME,name.data(),name.size(),&length));
    ASSERT_GT(length,0);ASSERT_LT(static_cast<std::size_t>(length),name.size());database.assign(name.data(),length);
    ASSERT_TRUE(database=="odbcpp_pilot");
    schema="rs5_cols_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"_雪";
    const auto ddl="CREATE SCHEMA "+quoted_identifier(schema)+";";
    const auto schema_result=execute_fixture(ddl);
    created=SQL_SUCCEEDED(schema_result);
    if (created) {
      RecordProperty("owned_schema",schema);
    }
    ASSERT_EQ(SQL_SUCCESS,schema_result);EXPECT_TRUE(created);
    ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
    for(unsigned i=0;i<2;++i) {
      const auto table="CREATE TABLE "+quoted_identifier(schema)+"."+quoted_identifier(tables[i])+"(id integer NOT NULL, value varchar(32));";
      ASSERT_EQ(SQL_SUCCESS,create_child(table,tables[i]));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
    }
    const auto view="CREATE VIEW "+quoted_identifier(schema)+"."+quoted_identifier(tables[2])+" AS SELECT id FROM "+quoted_identifier(schema)+"."+quoted_identifier(tables[0])+";";
    ASSERT_EQ(SQL_SUCCESS,create_child(view,tables[2],true));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
  }
  bool drop_owned(const std::string& sql,bool owned) {
    if (!can_drop_owned(owned,completion_known_,seconds_left())) {
      ADD_FAILURE()<<"Owned column fixture retained: no safe remaining cleanup budget";
      return false;
    }
    const auto result=execute_fixture(sql);
    EXPECT_EQ(SQL_SUCCESS,result);
    if (SQL_SUCCEEDED(result)) {
      const auto closed=SQLFreeStmt(stmt,SQL_CLOSE);
      EXPECT_EQ(SQL_SUCCESS,closed);
      if (closed!=SQL_SUCCESS) {
        completion_known_=false;
      }
      return true;
    }
    return false;
  }
  void TearDown() override {
    if (stmt) {
      EXPECT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
      // Reverse CREATE order removes our view before its input table. RESTRICT
      // refuses an untracked dependent instead of deleting it with CASCADE.
      while (!created_children.empty()) {
        const auto& child=created_children.back();
        const auto ddl=std::string(child.second?"DROP VIEW ":"DROP TABLE ")+
            quoted_identifier(schema)+"."+quoted_identifier(child.first)+" RESTRICT;";
        if (!drop_owned(ddl,created)) {
          break;
        }
        created_children.pop_back();
      }
      if (created && created_children.empty()) {
        const auto ddl="DROP SCHEMA "+quoted_identifier(schema)+" RESTRICT;";
        if (drop_owned(ddl,true)) {
          created=false;
        }
      }
      EXPECT_TRUE(created_children.empty());EXPECT_FALSE(created);
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_STMT,stmt));stmt=SQL_NULL_HSTMT;
    }
    if (dbc) {
      if (connected_) {
        EXPECT_EQ(SQL_SUCCESS,SQLDisconnect(dbc));
      }
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_DBC,dbc));dbc=SQL_NULL_HDBC;
    }
    if (env) {
      EXPECT_EQ(SQL_SUCCESS,SQLFreeHandle(SQL_HANDLE_ENV,env));env=SQL_NULL_HENV;
    }
    EXPECT_LT(std::chrono::steady_clock::now(),window_end_);
  }
  SQLRETURN columns(const std::optional<std::string>& table,bool metadata=false) {
    if (!cap()) {
      return SQL_ERROR;
    }
    SQLFreeStmt(stmt,SQL_CLOSE);
    const auto setting=SQLSetStmtAttr(stmt,SQL_ATTR_METADATA_ID,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(metadata?SQL_TRUE:SQL_FALSE)),0);
    if(setting!=SQL_SUCCESS)return setting;
    const auto expected=metadata && !table?LocalRefusal::MetadataNull:LocalRefusal::None;
    const auto s=metadata?quoted_identifier(schema):literal_pattern(schema);
    // No column filter, so NULL and % equivalence is measured across all tables.
    if (!GetParam()) {
      return observe(SQLColumns(stmt,chars(database),database.size(),chars(s),s.size(),
          table?chars(*table):nullptr,table?static_cast<SQLSMALLINT>(table->size()):0,nullptr,0),expected);
    }
    auto db=rs::odbc::utf8_to_wide(database),sc=rs::odbc::utf8_to_wide(s),t=rs::odbc::utf8_to_wide(table.value_or(""));
    if(!db || !sc || !t)return SQL_ERROR;
    SQLWCHAR empty=0;auto* data=table?(t->empty()?&empty:t->data()):nullptr;
    return observe(SQLColumnsW(stmt,db->data(),db->size(),sc->data(),sc->size(),data,
        table?static_cast<SQLSMALLINT>(t->size()):0,nullptr,0),expected);
  }
  SQLRETURN schema_columns(const std::optional<std::string>& schema_pattern,
      const std::string& exact_table) {
    if (!cap()) {
      return SQL_ERROR;
    }
    SQLFreeStmt(stmt,SQL_CLOSE);
    const auto setting=SQLSetStmtAttr(stmt,SQL_ATTR_METADATA_ID,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_FALSE)),0);
    if (setting!=SQL_SUCCESS) return setting;
    const auto expected=schema_pattern && *schema_pattern==literal_pattern(schema)+"\\"?
        LocalRefusal::ColumnPattern:LocalRefusal::None;
    const auto table=literal_pattern(exact_table);
    if (!GetParam()) {
      return observe(SQLColumns(stmt,chars(database),database.size(),
          schema_pattern?chars(*schema_pattern):nullptr,
          schema_pattern?static_cast<SQLSMALLINT>(schema_pattern->size()):0,
          chars(table),table.size(),nullptr,0),expected);
    }
    auto db=rs::odbc::utf8_to_wide(database),sc=rs::odbc::utf8_to_wide(schema_pattern.value_or("")),
        tb=rs::odbc::utf8_to_wide(table);
    if (!db || !sc || !tb) return SQL_ERROR;
    SQLWCHAR empty=0;
    auto* schema_data=schema_pattern?(sc->empty()?&empty:sc->data()):nullptr;
    return observe(SQLColumnsW(stmt,db->data(),db->size(),schema_data,
        schema_pattern?static_cast<SQLSMALLINT>(sc->size()):0,tb->data(),tb->size(),nullptr,0),expected);
  }
  SQLRETURN schema_tables(const std::optional<std::string>& pattern,const std::string& table) {
    if (!cap()) {
      return SQL_ERROR;
    }
    SQLFreeStmt(stmt,SQL_CLOSE);
    const auto setting=SQLSetStmtAttr(stmt,SQL_ATTR_METADATA_ID,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_FALSE)),0);
    if (setting!=SQL_SUCCESS) return setting;
    const auto expected=pattern && *pattern==literal_pattern(schema)+"\\"?
        LocalRefusal::TablePattern:LocalRefusal::None;
    const auto catalog=literal_pattern(database),object=literal_pattern(table);
    const std::string types="TABLE";
    if (!GetParam()) {
      return observe(SQLTables(stmt,chars(catalog),catalog.size(),
          pattern?chars(*pattern):nullptr,pattern?static_cast<SQLSMALLINT>(pattern->size()):0,
          chars(object),object.size(),chars(types),types.size()),expected);
    }
    auto c=rs::odbc::utf8_to_wide(catalog),sc=rs::odbc::utf8_to_wide(pattern.value_or("")),
        tb=rs::odbc::utf8_to_wide(object),ty=rs::odbc::utf8_to_wide(types);
    if (!c || !sc || !tb || !ty) return SQL_ERROR;
    SQLWCHAR empty=0;auto* schema_data=pattern?(sc->empty()?&empty:sc->data()):nullptr;
    return observe(SQLTablesW(stmt,c->data(),c->size(),schema_data,
        pattern?static_cast<SQLSMALLINT>(sc->size()):0,tb->data(),tb->size(),ty->data(),ty->size()),expected);
  }
  std::vector<std::string> table_rows() {
    SQLSMALLINT count=0;EXPECT_EQ(SQL_SUCCESS,SQLNumResultCols(stmt,&count));EXPECT_EQ(5,count);
    std::vector<std::string> result;SQLRETURN fetched=SQL_NO_DATA;
    for (unsigned bound=0;bound<8;++bound) {
      fetched=SQLFetch(stmt);
      if (fetched==SQL_NO_DATA) { break; }
      EXPECT_EQ(SQL_SUCCESS,fetched);
      if (fetched!=SQL_SUCCESS) { break; }
      EXPECT_EQ(database,text(1));EXPECT_EQ(schema,text(2));EXPECT_EQ("TABLE",text(4));
      result.push_back(text(3));
    }
    EXPECT_EQ(SQL_NO_DATA,fetched);return result;
  }
  std::string text(SQLUSMALLINT column) {
    std::array<char,4096> value{};SQLLEN indicator=0;
    EXPECT_EQ(SQL_SUCCESS,SQLGetData(stmt,column,SQL_C_CHAR,value.data(),value.size(),&indicator));
    EXPECT_NE(SQL_NULL_DATA,indicator);return value.data();
  }
  std::vector<std::pair<std::string,std::string>> rows() {
    SQLSMALLINT count=0;EXPECT_EQ(SQL_SUCCESS,SQLNumResultCols(stmt,&count));EXPECT_EQ(18,count);
    std::vector<std::pair<std::string,std::string>> result;SQLRETURN fetched=SQL_NO_DATA;
    for(unsigned bound=0;bound<16;++bound) {
      fetched=SQLFetch(stmt);if(fetched==SQL_NO_DATA)break;
      EXPECT_EQ(SQL_SUCCESS,fetched);if(fetched!=SQL_SUCCESS)break;
      EXPECT_EQ(database,text(1));EXPECT_EQ(schema,text(2));
      const auto table=text(3),column=text(4);const auto ordinal=text(17);
      EXPECT_EQ(column=="id"?"1":"2",ordinal);result.emplace_back(table,column);
    }
    EXPECT_EQ(SQL_NO_DATA,fetched);return result;
  }
};
TEST_P(ColumnsOmittedTableNative, NullUniversalExactIdentifiersEmptyAndRecovery) {
  ASSERT_EQ(SQL_SUCCESS,columns(std::nullopt));const auto omitted=rows();
  auto names=tables;std::sort(names.begin(),names.end());std::vector<std::pair<std::string,std::string>> expected;
  for(const auto& table:names){expected.emplace_back(table,"id");if(table!=tables[2])expected.emplace_back(table,"value");}
  EXPECT_EQ(expected,omitted);
  ASSERT_EQ(SQL_SUCCESS,columns(std::string{"%"}));EXPECT_EQ(omitted,rows());
  ASSERT_EQ(SQL_SUCCESS,columns(literal_pattern(tables[0])));EXPECT_EQ((std::vector<std::pair<std::string,std::string>>{{tables[0],"id"},{tables[0],"value"}}),rows());
  ASSERT_EQ(SQL_SUCCESS,columns(std::string{}));EXPECT_TRUE(rows().empty());
  ASSERT_EQ(SQL_ERROR,columns(std::nullopt,true));
  std::array<SQLCHAR,6> state{};std::array<SQLCHAR,1024> message{};SQLINTEGER native=0;SQLSMALLINT length=0;
  ASSERT_EQ(SQL_SUCCESS,SQLGetDiagRec(SQL_HANDLE_STMT,stmt,1,state.data(),&native,message.data(),message.size(),&length));
  EXPECT_STREQ("HY009",reinterpret_cast<char*>(state.data()));
  ASSERT_EQ(SQL_SUCCESS,columns(std::nullopt));EXPECT_EQ(omitted,rows());
}
TEST_P(ColumnsOmittedTableNative, TableLikePrefixEscapesUnicodeNoMatchAndRecovery) {
  // Existing opt-in setup/5s statement budget and exact-principal cleanup apply;
  // source authorship/compile-link is not execution or live admission.
  for (const auto& pattern:std::vector<std::string>{"a\\_%",
      literal_pattern(tables[0].substr(0,tables[0].size()-3))+"_"}) {
    ASSERT_EQ(SQL_SUCCESS,columns(pattern));
    EXPECT_EQ((std::vector<std::pair<std::string,std::string>>{{tables[0],"id"},{tables[0],"value"}}),rows());
  }
  ASSERT_EQ(SQL_SUCCESS,columns(std::string{"z\\_%"}));
  EXPECT_EQ((std::vector<std::pair<std::string,std::string>>{{tables[1],"id"},{tables[1],"value"}}),rows());
  ASSERT_EQ(SQL_SUCCESS,columns(std::string{"v\\_%"}));
  EXPECT_EQ((std::vector<std::pair<std::string,std::string>>{{tables[2],"id"}}),rows());
  ASSERT_EQ(SQL_SUCCESS,columns(std::string{"missing%"}));EXPECT_TRUE(rows().empty());
  ASSERT_EQ(SQL_SUCCESS,columns(std::string{}));EXPECT_TRUE(rows().empty());
  ASSERT_EQ(SQL_SUCCESS,columns(std::string{"z\\_%"}));
  EXPECT_EQ((std::vector<std::pair<std::string,std::string>>{{tables[1],"id"},{tables[1],"value"}}),rows());
}
TEST_P(ColumnsOmittedTableNative, SchemaNullUniversalUtf8PrefixEmptyAndFreshRecovery) {
  // A unique table inside the already-owned schema narrows NULL/% schema calls.
  // Five-second statement limit, bounded rows and existing DROP-schema cleanup
  // remain; compile/link does not admit any SQL or native body execution.
  const auto table=schema+"_schema_pattern_only";
  const auto ddl="CREATE TABLE "+quoted_identifier(schema)+"."+quoted_identifier(table)+"(id integer);";
  ASSERT_EQ(SQL_SUCCESS,create_child(ddl,table));
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
  const std::vector<std::pair<std::string,std::string>> expected{{table,"id"}};
  for (const auto& pattern:std::vector<std::optional<std::string>>{std::nullopt,"%",
      literal_pattern(schema)+"%",literal_pattern(schema.substr(0,schema.size()-3))+"_"}) {
    ASSERT_EQ(SQL_SUCCESS,schema_columns(pattern,table));
    EXPECT_EQ(expected,rows());
  }
  for (const auto* pattern:{"","missing_schema_pattern%"}) {
    ASSERT_EQ(SQL_SUCCESS,schema_columns(std::string{pattern},table));
    EXPECT_TRUE(rows().empty());
  }
  ASSERT_EQ(SQL_ERROR,schema_columns(literal_pattern(schema)+"\\",table));
  ASSERT_EQ(SQL_SUCCESS,schema_columns(literal_pattern(schema)+"%",table));
  EXPECT_EQ(expected,rows());
}
TEST_P(ColumnsOmittedTableNative, SqlTablesSchemaNullUniversalPatternEmptyAndRecovery) {
  // This newly authored SQLTables body requires its own protected exact-case
  // checkpoint admission; the existing fixture marker is only an execution guard.
  const auto table=schema+"_tables_schema_pattern_only";
  const auto ddl="CREATE TABLE "+quoted_identifier(schema)+"."+quoted_identifier(table)+"(id integer);";
  ASSERT_EQ(SQL_SUCCESS,create_child(ddl,table));
  ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
  const std::vector<std::string> expected{table};
  for (const auto& pattern:std::vector<std::optional<std::string>>{std::nullopt,"%",
      literal_pattern(schema)+"%",literal_pattern(schema.substr(0,schema.size()-3))+"_"}) {
    ASSERT_EQ(SQL_SUCCESS,schema_tables(pattern,table));EXPECT_EQ(expected,table_rows());
  }
  for (const auto* pattern:{"","missing_schema_pattern%"}) {
    ASSERT_EQ(SQL_SUCCESS,schema_tables(std::string{pattern},table));EXPECT_TRUE(table_rows().empty());
  }
  ASSERT_EQ(SQL_ERROR,schema_tables(literal_pattern(schema)+"\\",table));
  ASSERT_EQ(SQL_SUCCESS,schema_tables(literal_pattern(schema)+"%",table));EXPECT_EQ(expected,table_rows());
}
INSTANTIATE_TEST_SUITE_P(AnsiAndWide,ColumnsOmittedTableNative,testing::Values(false,true));
} // namespace
