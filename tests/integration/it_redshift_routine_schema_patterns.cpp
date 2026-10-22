#include "../test_live_package_inputs.h"
#include <gtest/gtest.h>
#include "odbc/odbc_api.h"
#include "odbc/odbc_types.h"
#include "odbc/unicode.h"
#include "odbc/connection_string.h"
#include "odbcpp/database/backend_provider.h"
#include "tests/test_connection_config.h"
#include <array>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using odbcpp::test::live_package_inputs::fixture_smallint_length;
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


// This fixture expects no SQL_ERROR. A live handle never proves completion.
constexpr bool known_completion(SQLRETURN result, std::string_view = {}, bool = false) {
  return result==SQL_SUCCESS || result==SQL_SUCCESS_WITH_INFO;
}
static_assert(known_completion(SQL_SUCCESS));
static_assert(known_completion(SQL_SUCCESS_WITH_INFO));
static_assert(!known_completion(SQL_ERROR,"HY000",true));
static_assert(!known_completion(SQL_ERROR,"40003",true));
static_assert(!known_completion(SQL_ERROR,"ZZ999",true));
static_assert(!known_completion(SQL_ERROR,"HY009",true));
static_assert(!known_completion(SQL_STILL_EXECUTING));

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
  std::string prefix,procedure,database;std::vector<std::string> schemas,created,created_procedures;
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
  SQLRETURN observe(SQLRETURN result) {
    // Sticky unknown completion prevents every subsequent cap()/DROP dispatch.
    completion_known_=completion_known_ && known_completion(result);
    return result;
  }
  SQLRETURN execute_fixture(const std::string& sql) {
    if (!cap()) {
      return SQL_ERROR;
    }
    return observe(SQLExecDirect(stmt,chars(sql),SQL_NTS));
  }
  void SetUp() override {
    // This runtime switch is an execution guard, never live/paid authority.
    // The protected checkpoint must separately admit this exact fixture batch.
    const auto enabled_environment = odbcpp::test::live_package_inputs::fixture_environment("ODBCPP_REDSHIFT_ROUTINE_SCHEMA_NATIVE");
    const char* enabled = enabled_environment ? enabled_environment->c_str() : nullptr;
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
    std::array<char,1025> db{};SQLSMALLINT length=0;
    ASSERT_EQ(SQL_SUCCESS,SQLGetInfo(dbc,SQL_DATABASE_NAME,db.data(),fixture_smallint_length(db.size()),&length));
    ASSERT_GT(length,0);ASSERT_LT(static_cast<std::size_t>(length),db.size());
    database.assign(db.data(),static_cast<std::size_t>(length));
    ASSERT_TRUE(database=="odbcpp_pilot");
    prefix="rs5_routine_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"_";
    procedure="sp_"+prefix+"same";schemas={prefix+"a_雪",prefix+"literal_%"};
    for(const auto& schema:schemas) {
      const auto ddl="CREATE SCHEMA "+quoted_identifier(schema)+";";
      const auto schema_result=execute_fixture(ddl);
      if (SQL_SUCCEEDED(schema_result)) {
        created.push_back(schema);
        RecordProperty("owned_schema_"+std::to_string(created.size()),schema);
      }
      ASSERT_EQ(SQL_SUCCESS,schema_result);
      EXPECT_EQ(schema,created.back());
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
      // Native DDL syntax: docs.aws.amazon.com/redshift/latest/dg/r_CREATE_PROCEDURE.html
      const auto proc="CREATE PROCEDURE "+quoted_identifier(schema)+"."+quoted_identifier(procedure)+
          "(in_x IN integer, out_y OUT integer) AS $$ BEGIN out_y := in_x; END; $$ LANGUAGE plpgsql;";
      const auto procedure_result=execute_fixture(proc);
      if (SQL_SUCCEEDED(procedure_result)) {
        created_procedures.push_back(schema);
        RecordProperty("owned_procedure_"+std::to_string(created_procedures.size()),
            quoted_identifier(schema)+"."+quoted_identifier(procedure)+"(integer)");
      }
      ASSERT_EQ(SQL_SUCCESS,procedure_result);
      EXPECT_EQ(schema,created_procedures.back());
      ASSERT_EQ(SQL_SUCCESS,SQLFreeStmt(stmt,SQL_CLOSE));
    }
  }
  bool drop_owned(const std::string& sql,bool owned) {
    if (!can_drop_owned(owned,completion_known_,seconds_left())) {
      ADD_FAILURE()<<"Owned routine fixture retained: no safe remaining cleanup budget";
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
      // Only exact successful CREATEs enter these ledgers. Never CASCADE into
      // untracked objects. OUT integer is not part of the procedure signature:
      // docs.aws.amazon.com/redshift/latest/dg/r_DROP_PROCEDURE.html
      while (!created_procedures.empty()) {
        const auto& owned_schema=created_procedures.back();
        const auto ddl="DROP PROCEDURE "+quoted_identifier(owned_schema)+"."+
            quoted_identifier(procedure)+"(integer);";
        if (!drop_owned(ddl,true)) {
          break;
        }
        created_procedures.pop_back();
      }
      if (created_procedures.empty()) {
        while (!created.empty()) {
          const auto ddl="DROP SCHEMA "+quoted_identifier(created.back())+" RESTRICT;";
          if (!drop_owned(ddl,true)) {
            break;
          }
          created.pop_back();
        }
      }
      EXPECT_TRUE(created.empty());EXPECT_TRUE(created_procedures.empty());
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
  SQLRETURN catalog(bool columns,const std::optional<std::string>& schema,bool metadata_id=false) {
    if (!cap()) {
      return SQL_ERROR;
    }
    SQLFreeStmt(stmt,SQL_CLOSE);
    const auto setting=SQLSetStmtAttr(stmt,SQL_ATTR_METADATA_ID,
        reinterpret_cast<SQLPOINTER>(static_cast<std::uintptr_t>(metadata_id?SQL_TRUE:SQL_FALSE)),0);
    if(setting!=SQL_SUCCESS)return setting;
    const auto proc=metadata_id?procedure:pattern_literal(procedure);
    const std::string column=metadata_id?"out_y":"out\\_y";
    if(!GetParam()) {
      return observe(columns?SQLProcedureColumns(stmt,chars(database),fixture_smallint_length(database.size()),schema?chars(*schema):nullptr,
          schema?static_cast<SQLSMALLINT>(schema->size()):0,chars(proc),fixture_smallint_length(proc.size()),chars(column),fixture_smallint_length(column.size())):
          SQLProcedures(stmt,chars(database),fixture_smallint_length(database.size()),schema?chars(*schema):nullptr,
          schema?static_cast<SQLSMALLINT>(schema->size()):0,chars(proc),fixture_smallint_length(proc.size())));
    }
    auto db=rs::odbc::utf8_to_wide(database),p=rs::odbc::utf8_to_wide(proc),c=rs::odbc::utf8_to_wide(column);
    auto s=rs::odbc::utf8_to_wide(schema.value_or(""));
    if(!db || !p || !c || !s)return SQL_ERROR;
    SQLWCHAR empty_schema=0;
    auto* schema_data=schema?(s->empty()?&empty_schema:s->data()):nullptr;
    return observe(columns?SQLProcedureColumnsW(stmt,db->data(),fixture_smallint_length(db->size()),schema_data,
        schema?static_cast<SQLSMALLINT>(s->size()):0,p->data(),fixture_smallint_length(p->size()),c->data(),fixture_smallint_length(c->size())):
        SQLProceduresW(stmt,db->data(),fixture_smallint_length(db->size()),schema_data,
        schema?static_cast<SQLSMALLINT>(s->size()):0,p->data(),fixture_smallint_length(p->size())));
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
