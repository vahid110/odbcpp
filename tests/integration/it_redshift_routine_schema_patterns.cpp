#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include "tests/test_connection_config.h"
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {
std::string quoted_identifier(const std::string& value) {
  std::string out{"\""};for(char ch:value){if(ch=='\"')out+='\"';out+=ch;}return out+'\"';
}
std::string pattern_literal(const std::string& value) {
  std::string out;for(char ch:value){if(ch=='\\' || ch=='%' || ch=='_')out+='\\';out+=ch;}return out;
}
SQLCHAR* chars(const std::string& text) {return reinterpret_cast<SQLCHAR*>(const_cast<char*>(text.data()));}
class RoutineSchemaNative : public testing::TestWithParam<bool> {
 protected:
  SQLHENV env{SQL_NULL_HENV};SQLHDBC dbc{SQL_NULL_HDBC};SQLHSTMT stmt{SQL_NULL_HSTMT};
  std::string prefix,procedure,database;std::vector<std::string> schemas,created;
  void SetUp() override {
    // This runtime switch is an execution guard, never live/paid authority.
    // The protected checkpoint must separately admit this exact fixture batch.
    const auto* enabled=std::getenv("ODBCPP_REDSHIFT_ROUTINE_SCHEMA_NATIVE");
    if(!enabled || std::string(enabled)!="1")GTEST_SKIP()<<"Separately admitted real Redshift test";
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_ENV,SQL_NULL_HANDLE,&env));
    ASSERT_EQ(SQL_SUCCESS,SQLSetEnvAttr(env,SQL_ATTR_ODBC_VERSION,reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3),0));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_DBC,env,&dbc));
    ASSERT_EQ(SQL_SUCCESS,SQLConnect(dbc,odbcpp::test::configured_connection_string_data(),SQL_NTS,nullptr,0,nullptr,0));
    ASSERT_EQ(SQL_SUCCESS,SQLAllocHandle(SQL_HANDLE_STMT,dbc,&stmt));
    ASSERT_EQ(SQL_SUCCESS,SQLSetStmtAttr(stmt,SQL_ATTR_QUERY_TIMEOUT,reinterpret_cast<SQLPOINTER>(5),0));
    std::array<char,1025> db{};SQLSMALLINT length=0;
    ASSERT_EQ(SQL_SUCCESS,SQLGetInfo(dbc,SQL_DATABASE_NAME,db.data(),db.size(),&length));
    ASSERT_GT(length,0);database.assign(db.data(),static_cast<std::size_t>(length));
    prefix="rs5_routine_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"_";
    procedure="sp_"+prefix+"same";schemas={prefix+"a_雪",prefix+"literal_%"};
    for(const auto& schema:schemas) {
      const auto ddl="CREATE SCHEMA "+quoted_identifier(schema)+";";
      ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(stmt,chars(ddl),SQL_NTS));created.push_back(schema);
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
      // Native DDL syntax: docs.aws.amazon.com/redshift/latest/dg/r_CREATE_PROCEDURE.html
      const auto proc="CREATE PROCEDURE "+quoted_identifier(schema)+"."+quoted_identifier(procedure)+
          "(in_x IN integer, out_y OUT integer) AS $$ BEGIN out_y := in_x; END; $$ LANGUAGE plpgsql;";
      ASSERT_EQ(SQL_SUCCESS,SQLExecDirect(stmt,chars(proc),SQL_NTS));
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
    }
  }
  void TearDown() override {
    if(stmt) {
      SQLFreeStmt(stmt,SQL_CLOSE);
      for(auto it=created.rbegin();it!=created.rend();++it) {
        const auto ddl="DROP SCHEMA "+quoted_identifier(*it)+" CASCADE;";
        EXPECT_EQ(SQL_SUCCESS,SQLExecDirect(stmt,chars(ddl),SQL_NTS));SQLFreeStmt(stmt,SQL_CLOSE);
      }
      SQLFreeHandle(SQL_HANDLE_STMT,stmt);
    }
    if(dbc){SQLDisconnect(dbc);SQLFreeHandle(SQL_HANDLE_DBC,dbc);}
    if(env)SQLFreeHandle(SQL_HANDLE_ENV,env);
  }
  SQLRETURN catalog(bool columns,const std::optional<std::string>& schema,bool metadata_id=false) {
    SQLFreeStmt(stmt,SQL_CLOSE);
    const auto setting=SQLSetStmtAttr(stmt,SQL_ATTR_METADATA_ID,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(metadata_id?SQL_TRUE:SQL_FALSE)),0);
    if(setting!=SQL_SUCCESS)return setting;
    const auto proc=metadata_id?procedure:pattern_literal(procedure);
    const std::string column=metadata_id?"out_y":"out\\_y";
    if(!GetParam()) {
      return columns?SQLProcedureColumns(stmt,chars(database),database.size(),schema?chars(*schema):nullptr,
          schema?static_cast<SQLSMALLINT>(schema->size()):0,chars(proc),proc.size(),chars(column),column.size()):
          SQLProcedures(stmt,chars(database),database.size(),schema?chars(*schema):nullptr,
          schema?static_cast<SQLSMALLINT>(schema->size()):0,chars(proc),proc.size());
    }
    auto db=rs::odbc::utf8_to_wide(database),p=rs::odbc::utf8_to_wide(proc),c=rs::odbc::utf8_to_wide(column);
    auto s=rs::odbc::utf8_to_wide(schema.value_or(""));
    if(!db || !p || !c || !s)return SQL_ERROR;
    SQLWCHAR empty_schema=0;
    auto* schema_data=schema?(s->empty()?&empty_schema:s->data()):nullptr;
    return columns?SQLProcedureColumnsW(stmt,db->data(),db->size(),schema_data,
        schema?static_cast<SQLSMALLINT>(s->size()):0,p->data(),p->size(),c->data(),c->size()):
        SQLProceduresW(stmt,db->data(),db->size(),schema_data,
        schema?static_cast<SQLSMALLINT>(s->size()):0,p->data(),p->size());
  }
  std::string text(SQLUSMALLINT column) {
    std::array<char,4096> data{};SQLLEN indicator=0;
    EXPECT_EQ(SQL_SUCCESS,SQLGetData(stmt,column,SQL_C_CHAR,data.data(),data.size(),&indicator));
    EXPECT_NE(SQL_NULL_DATA,indicator);return data.data();
  }
  std::vector<std::string> rows(bool columns) {
    std::vector<std::string> names;SQLRETURN fetched=SQL_NO_DATA;
    for(unsigned bounded=0;bounded<16;++bounded) {
      fetched=SQLFetch(stmt);if(fetched==SQL_NO_DATA)break;
      EXPECT_EQ(SQL_SUCCESS,fetched);if(fetched!=SQL_SUCCESS)break;
      EXPECT_EQ(database,text(1));names.push_back(text(2));EXPECT_EQ(procedure,text(3));
      if(columns) {
        EXPECT_EQ("out_y",text(4));EXPECT_EQ(std::to_string(SQL_PARAM_OUTPUT),text(5));
        EXPECT_EQ(std::to_string(SQL_INTEGER),text(6));EXPECT_EQ("2",text(18));
      }
    }
    EXPECT_EQ(SQL_NO_DATA,fetched);return names;
  }
};
TEST_P(RoutineSchemaNative, OmittedWildcardUnicodeEscapedAndMetadataIdentifiers) {
  ASSERT_EQ(SQL_SUCCESS,catalog(false,pattern_literal(prefix)+"%"));EXPECT_EQ(schemas,rows(false));
  ASSERT_EQ(SQL_SUCCESS,catalog(true,pattern_literal(prefix)+"%"));EXPECT_EQ(schemas,rows(true));
  ASSERT_EQ(SQL_SUCCESS,catalog(false,std::nullopt));EXPECT_EQ(schemas,rows(false));
  ASSERT_EQ(SQL_SUCCESS,catalog(false,pattern_literal(schemas[1])));EXPECT_EQ((std::vector<std::string>{schemas[1]}),rows(false));
  ASSERT_EQ(SQL_SUCCESS,catalog(true,schemas[0],true));EXPECT_EQ((std::vector<std::string>{schemas[0]}),rows(true));
  ASSERT_EQ(SQL_SUCCESS,catalog(false,std::string{}));EXPECT_TRUE(rows(false).empty());
  // Refilling a statement cannot mutate the previously collected owning names.
  const auto saved=schemas;ASSERT_EQ(SQL_SUCCESS,catalog(false,pattern_literal(prefix)+"missing%"));
  EXPECT_TRUE(rows(false).empty());EXPECT_EQ(saved,schemas);
}
INSTANTIATE_TEST_SUITE_P(AnsiAndWide,RoutineSchemaNative,testing::Values(false,true));
} // namespace
