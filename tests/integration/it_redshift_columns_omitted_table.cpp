#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include "tests/test_connection_config.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
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
  const std::vector<std::string> tables{"a_%\\quoted\"雪","z_table","v_view"};
  void SetUp() override {
    // This is only an execution guard. Protected checkpoint authority must
    // separately admit this exact bounded fixture and its Redshift principal.
    const auto* enabled=std::getenv("ODBCPP_REDSHIFT_COLUMNS_OMITTED_TABLE_NATIVE");
    if(!enabled || std::string(enabled)!="1")GTEST_SKIP()<<"Separately admitted real Redshift test";
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_ENV,SQL_NULL_HANDLE,&env));
    ASSERT_EQ(SQL_SUCCESS,SQLSetEnvAttr(env,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),0));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_DBC,env,&dbc));
    ASSERT_EQ(SQL_SUCCESS,SQLConnect(dbc,odbcpp::test::configured_connection_string_data(),SQL_NTS,nullptr,0,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,dbc,&stmt));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(5),0));
    std::array<char,1025> name{};SQLSMALLINT length=0;
    ASSERT_EQ(SQL_SUCCESS,SQLGetInfo(dbc,SQL_DATABASE_NAME,name.data(),name.size(),&length));
    ASSERT_GT(length,0);ASSERT_LT(static_cast<std::size_t>(length),name.size());database.assign(name.data(),length);
    schema="rs5_cols_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"_雪";
    const auto ddl="CREATE SCHEMA "+quoted_identifier(schema)+";";
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(stmt,chars(ddl),SQL_NTS));created=true;ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
    for(unsigned i=0;i<2;++i) {
      const auto table="CREATE TABLE "+quoted_identifier(schema)+"."+quoted_identifier(tables[i])+"(id integer NOT NULL, value varchar(32));";
      ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(stmt,chars(table),SQL_NTS));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
    }
    const auto view="CREATE VIEW "+quoted_identifier(schema)+"."+quoted_identifier(tables[2])+" AS SELECT id FROM "+quoted_identifier(schema)+"."+quoted_identifier(tables[0])+";";
    ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(stmt,chars(view),SQL_NTS));ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
  }
  void TearDown() override {
    if(stmt) {
      SQLFreeStmt(stmt,SQL_CLOSE);
      if(created) {const auto ddl="DROP SCHEMA "+quoted_identifier(schema)+" CASCADE;";EXPECT_EQ(SQL_SUCCESS,SQLExecDirect(stmt,chars(ddl),SQL_NTS));SQLFreeStmt(stmt,SQL_CLOSE);}
      SQLFreeHandle(SQL_HANDLE_STMT,stmt);
    }
    if(dbc){SQLDisconnect(dbc);SQLFreeHandle(SQL_HANDLE_DBC,dbc);}
    if(env)SQLFreeHandle(SQL_HANDLE_ENV,env);
  }
  SQLRETURN columns(const std::optional<std::string>& table,bool metadata=false) {
    SQLFreeStmt(stmt,SQL_CLOSE);
    const auto setting=SQLSetStmtAttr(stmt,SQL_ATTR_METADATA_ID,reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(metadata?SQL_TRUE:SQL_FALSE)),0);
    if(setting!=SQL_SUCCESS)return setting;
    const auto s=metadata?quoted_identifier(schema):literal_pattern(schema);
    // No column filter, so NULL and % equivalence is measured across all tables.
    if(!GetParam()) return SQLColumns(stmt,chars(database),database.size(),chars(s),s.size(),
        table?chars(*table):nullptr,table?static_cast<SQLSMALLINT>(table->size()):0,nullptr,0);
    auto db=rs::odbc::utf8_to_wide(database),sc=rs::odbc::utf8_to_wide(s),t=rs::odbc::utf8_to_wide(table.value_or(""));
    if(!db || !sc || !t)return SQL_ERROR;
    SQLWCHAR empty=0;auto* data=table?(t->empty()?&empty:t->data()):nullptr;
    return SQLColumnsW(stmt,db->data(),db->size(),sc->data(),sc->size(),data,
        table?static_cast<SQLSMALLINT>(t->size()):0,nullptr,0);
  }
  SQLRETURN schema_columns(const std::optional<std::string>& schema_pattern,
      const std::string& exact_table) {
    SQLFreeStmt(stmt,SQL_CLOSE);
    const auto setting=SQLSetStmtAttr(stmt,SQL_ATTR_METADATA_ID,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(SQL_FALSE)),0);
    if (setting!=SQL_SUCCESS) return setting;
    const auto table=literal_pattern(exact_table);
    if (!GetParam()) return SQLColumns(stmt,chars(database),database.size(),
        schema_pattern?chars(*schema_pattern):nullptr,
        schema_pattern?static_cast<SQLSMALLINT>(schema_pattern->size()):0,
        chars(table),table.size(),nullptr,0);
    auto db=rs::odbc::utf8_to_wide(database),sc=rs::odbc::utf8_to_wide(schema_pattern.value_or("")),
        tb=rs::odbc::utf8_to_wide(table);
    if (!db || !sc || !tb) return SQL_ERROR;
    SQLWCHAR empty=0;
    auto* schema_data=schema_pattern?(sc->empty()?&empty:sc->data()):nullptr;
    return SQLColumnsW(stmt,db->data(),db->size(),schema_data,
        schema_pattern?static_cast<SQLSMALLINT>(sc->size()):0,tb->data(),tb->size(),nullptr,0);
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
  ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(stmt,chars(ddl),SQL_NTS));
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
INSTANTIATE_TEST_SUITE_P(AnsiAndWide,ColumnsOmittedTableNative,testing::Values(false,true));
} // namespace
