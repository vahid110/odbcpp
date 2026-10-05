#include <gtest/gtest.h>
#include "core/database/postgres/pg_database_connection.h"
#include "core/transport/i_transport.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <thread>

namespace rs::core::database::postgres::detail {
struct StagedPreparedRefusalTestAccess {
  static BackendResult<QueryResult> observe(PgDatabaseConnection& c,std::string_view sql,
      std::span<const QueryParameter> parameters,rs::util::Deadline deadline) {
    return c.observe_and_decline_binary_prepared_for_test(sql,parameters,deadline);
  }
};
}
namespace {
using namespace rs::core::database;
using namespace rs::core::database::postgres;
using rs::util::Deadline;
using rs::util::DbErrorCode;
void u32(std::vector<std::byte>& out,std::uint32_t value) {
  for(int shift:{24,16,8,0})out.push_back(std::byte((value>>shift)&255));
}
std::vector<std::byte> frame(char tag,std::string_view payload={}) {
  std::vector<std::byte> out{std::byte(tag)};u32(out,static_cast<std::uint32_t>(payload.size()+4));
  for(unsigned char c:payload) {
    out.push_back(std::byte(c));
  }
  return out;
}
void add(std::vector<std::byte>& out,const std::vector<std::byte>& value){out.insert(out.end(),value.begin(),value.end());}
std::vector<std::byte> description(std::uint32_t oid,char state='I') {
  std::vector<std::byte> out;add(out,frame('1'));std::vector<std::byte> payload{std::byte{0},std::byte{1}};u32(payload,oid);
  std::string raw(reinterpret_cast<const char*>(payload.data()),payload.size());add(out,frame('t',raw));add(out,frame('n'));add(out,frame('Z',std::string(1,state)));return out;
}
std::string tags(const std::vector<std::byte>& wire) {
  std::string out;std::size_t p=0;
  while(p+5<=wire.size()) {out.push_back(static_cast<char>(wire[p]));std::uint32_t length=0;for(unsigned i=1;i<5;++i)length=(length<<8)|std::to_integer<unsigned>(wire[p+i]);if(length<4||length>wire.size()-p-1)return out+"!";p+=1+length;}
  return p==wire.size()?out:out+"!";
}
struct Observed {
  std::vector<std::vector<std::byte>> writes;
  std::vector<Deadline> sends,reads;
  std::size_t close_count{};
};
class ScriptTransport final:public rs::core::transport::ITransport {
public:
  ScriptTransport(std::shared_ptr<Observed> seen,std::vector<std::byte> responses,char startup='I'):seen_(std::move(seen)) {
    add(input_,frame('R',std::string(4,'\0')));add(input_,frame('Z',std::string(1,startup)));add(input_,responses);
  }
  bool partial_timeout{},read_timeout{},read_allocation{};std::function<void()> on_query, on_final_query_recv;
  rs::util::Result<void> connect(std::string_view,std::uint16_t,Deadline)override{return {};}
  rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte> data,Deadline d)override {
    seen_->sends.push_back(d);seen_->writes.emplace_back(data.begin(),data.end());
    if(seen_->writes.size()==2&&on_query)on_query();
    if(partial_timeout&&seen_->writes.size()>1){if(seen_->writes.size()==2)return rs::core::transport::IOResult{2,false};return {DbErrorCode::Timeout,"synthetic partial timeout"};}
    return rs::core::transport::IOResult{data.size(),false};
  }
  rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte> data,Deadline d)override {
    seen_->reads.push_back(d);if(read_allocation&&seen_->writes.size()>1)throw std::bad_alloc{};if(read_timeout&&seen_->writes.size()>1)return {DbErrorCode::Timeout,"synthetic read timeout"};
    const auto count=std::min(data.size(),input_.size()-offset_);std::copy_n(input_.begin()+offset_,count,data.begin());offset_+=count;
    if(count>0&&offset_==input_.size()&&seen_->writes.size()>1&&on_final_query_recv)on_final_query_recv();
    return rs::core::transport::IOResult{count,count==0};
  }
  void close()noexcept override{++seen_->close_count;}
private:std::shared_ptr<Observed> seen_;std::vector<std::byte> input_;std::size_t offset_{};
};
ConnectionSettings settings(){ConnectionSettings s;s.host="fake";s.port=5439;s.user="synthetic";s.database="synthetic";s.use_ssl=false;return s;}
BackendResult<QueryResult> observe(PgDatabaseConnection& c,std::string_view sql,std::span<const QueryParameter> params,Deadline d){return rs::core::database::postgres::detail::StagedPreparedRefusalTestAccess::observe(c,sql,params,d);}
}

TEST(PgStagedPreparedRefusalTest, DescriptionOnlyRawIdsDeclineWithoutResolverAndRecover) {
  for(std::uint32_t oid:{6551u,17u,999999u}) {
    SCOPED_TRACE(oid);auto seen=std::make_shared<Observed>();auto replies=description(oid);add(replies,frame('C',std::string("SELECT 0\0",9)));add(replies,frame('Z',"I"));auto transport=std::make_unique<ScriptTransport>(seen,replies);auto* raw=transport.get();
    PgDatabaseConnection backend(std::move(transport),std::nullopt,PgCatalogProfile::Redshift);ASSERT_TRUE(backend.connect(settings()));
    std::string sql="SELECT ?";std::vector<QueryParameter> params{{std::string("\0\xa1\xff",3),QueryParameterType::Binary,true}};
    raw->on_query=[&]{sql="changed";params[0].value="changed";params[0].type=QueryParameterType::Text;};
    const auto read_before=seen->reads.size();const auto deadline=rs::util::Clock::now()+std::chrono::seconds(3);const auto result=observe(backend,sql,params,deadline);ASSERT_TRUE(result.has_error());EXPECT_EQ(BackendErrorClass::Unsupported,result.backend_error().error_class);EXPECT_EQ(BackendOperation::ExecutePrepared,result.backend_error().operation);EXPECT_EQ(SessionState::Idle,result.backend_error().session_state);EXPECT_EQ(SessionDisposition::Reusable,result.backend_error().disposition);EXPECT_TRUE(backend.is_connected());EXPECT_EQ(0u,seen->close_count);
    ASSERT_EQ(2u,seen->writes.size());EXPECT_EQ("PDS",tags(seen->writes[1]));const auto& request=seen->writes[1];
    // Literal unnamed Parse of SELECT $1 with a single intended OID17.
    const unsigned char expected[]{'P',0,0,0,21,0,'S','E','L','E','C','T',' ','$','1',0,0,1,0,0,0,17,'D',0,0,0,6,'S',0,'S',0,0,0,4};
    ASSERT_EQ(sizeof(expected),request.size());for(std::size_t i=0;i<request.size();++i){EXPECT_EQ(expected[i],std::to_integer<unsigned>(request[i]))<<i;}
    EXPECT_EQ(deadline,seen->sends[1]);for(std::size_t i=read_before;i<seen->reads.size();++i){EXPECT_EQ(deadline,seen->reads[i]);}
    ASSERT_TRUE(backend.execute_query("SELECT 0",deadline));ASSERT_EQ(3u,seen->writes.size());EXPECT_EQ("Q",tags(seen->writes[2]));EXPECT_TRUE(backend.is_connected());EXPECT_EQ(0u,seen->close_count);
  }
}

TEST(PgStagedPreparedRefusalTest, ScopePhaseStateAndDeadlineFailuresNeverBindOrFallback) {
  const QueryParameter binary[]{ {std::string("\0\xa1\xff",3),QueryParameterType::Binary,true} };
  // Entry rejections perform no statement send, including valid-looking nonbinary requests.
  for(int mode=0;mode<8;++mode) {
    SCOPED_TRACE(mode);auto seen=std::make_shared<Observed>();auto transport=std::make_unique<ScriptTransport>(seen,description(6551),mode==1?'T':'I');PgDatabaseConnection backend(std::move(transport),std::nullopt,mode==0?PgCatalogProfile::PostgreSQL:PgCatalogProfile::Redshift);auto configured=settings();if(mode==6)configured.input_limits.max_sql_bytes=4;if(mode==7)configured.input_limits.max_request_wire_bytes=16;ASSERT_TRUE(backend.connect(configured));
    const QueryParameter text[]{{"x",QueryParameterType::Text}};std::string sql=mode==3?"SELECT ?, ?":mode==4?std::string("SELECT ?\0",9):"SELECT ?";
    const auto result=observe(backend,sql,mode==2?std::span<const QueryParameter>(text):std::span<const QueryParameter>(binary),mode==5?Deadline{}:Deadline::max());ASSERT_TRUE(result.has_error());EXPECT_EQ(1u,seen->writes.size());EXPECT_EQ(mode==5?1u:0u,seen->close_count);EXPECT_EQ(mode<2?BackendErrorClass::Unsupported:mode<5?BackendErrorClass::InvalidInput:mode==5?BackendErrorClass::Timeout:BackendErrorClass::ResourceLimit,result.backend_error().error_class);
  }
  // Literal malformed phase/count transcripts require retirement; no B/E or resolver query.
  std::vector<std::vector<std::byte>> failures;
  auto missing_parse=description(6551);missing_parse.erase(missing_parse.begin(),missing_parse.begin()+5);failures.push_back(missing_parse);
  auto duplicate=description(6551);const auto extra_parse=frame('1');duplicate.insert(duplicate.begin()+5,extra_parse.begin(),extra_parse.end());failures.push_back(duplicate);
  std::vector<std::byte> wrong_count;add(wrong_count,frame('1'));add(wrong_count,frame('t',std::string(2,'\0')));add(wrong_count,frame('n'));add(wrong_count,frame('Z',"I"));failures.push_back(wrong_count);
  for(char tag:{'2','D','C'}){auto value=description(6551);const auto extra=frame(tag,tag=='C'?std::string_view("SELECT 0\0",9):std::string_view{});value.insert(value.begin()+5,extra.begin(),extra.end());failures.push_back(value);}
  auto incomplete=description(6551);incomplete.resize(incomplete.size()-11);failures.push_back(incomplete);
  for(std::size_t failure=0;failure<failures.size();++failure) {
    const auto& transcript=failures[failure];
    auto seen=std::make_shared<Observed>();PgDatabaseConnection backend(std::make_unique<ScriptTransport>(seen,transcript),std::nullopt,PgCatalogProfile::Redshift);ASSERT_TRUE(backend.connect(settings()));const auto d=rs::util::Clock::now()+std::chrono::seconds(3);const auto result=observe(backend,"SELECT ?",binary,d);ASSERT_TRUE(result.has_error());EXPECT_EQ(SessionDisposition::Retire,result.backend_error().disposition);EXPECT_EQ(failure+1==failures.size()?BackendErrorClass::Transport:BackendErrorClass::Protocol,result.backend_error().error_class);EXPECT_FALSE(backend.is_connected());EXPECT_EQ(1u,seen->close_count);ASSERT_EQ(2u,seen->writes.size());EXPECT_EQ("PDS",tags(seen->writes[1]));
  }
  // Inherited binary-format refusal occurs before the staged branch: a
  // complete drained Idle is reusable; this is not raw staged admission.
  {
    auto response=description(6551);response.resize(response.size()-11);
    const unsigned char field[]{0,1,'v',0,0,0,0,0,0,0,0,0,0x19,0x97,0xff,0xff,0xff,0xff,0xff,0xff,0,1};
    add(response,frame('T',std::string_view(reinterpret_cast<const char*>(field),sizeof(field))));add(response,frame('Z',"I"));auto events=std::make_shared<Observed>();PgDatabaseConnection c(std::make_unique<ScriptTransport>(events,response),std::nullopt,PgCatalogProfile::Redshift);ASSERT_TRUE(c.connect(settings()));const auto result=observe(c,"SELECT ?",binary,Deadline::max());ASSERT_TRUE(result.has_error());EXPECT_EQ(BackendErrorClass::Unsupported,result.backend_error().error_class);EXPECT_EQ(SessionDisposition::Reusable,result.backend_error().disposition);EXPECT_EQ(SessionState::Idle,result.backend_error().session_state);EXPECT_TRUE(c.is_connected());EXPECT_EQ(0u,events->close_count);ASSERT_EQ(2u,events->writes.size());EXPECT_EQ("PDS",tags(events->writes[1]));
  }
  for(char state:{'T','E'}) {
    auto seen=std::make_shared<Observed>();PgDatabaseConnection backend(std::make_unique<ScriptTransport>(seen,description(6551,state)),std::nullopt,PgCatalogProfile::Redshift);ASSERT_TRUE(backend.connect(settings()));const auto result=observe(backend,"SELECT ?",binary,Deadline::max());ASSERT_TRUE(result.has_error());EXPECT_EQ(BackendErrorClass::Unsupported,result.backend_error().error_class);EXPECT_EQ(state=='T'?SessionState::Transaction:SessionState::FailedTransaction,result.backend_error().session_state);EXPECT_EQ(SessionDisposition::ResetRequired,result.backend_error().disposition);EXPECT_EQ(0u,seen->close_count);ASSERT_EQ(2u,seen->writes.size());EXPECT_EQ("PDS",tags(seen->writes[1]));
  }
  std::vector<std::byte> server_error;constexpr char error[]="SERROR\0C42501\0Msynthetic denied\0\0";add(server_error,frame('E',std::string_view(error,sizeof(error)-1)));add(server_error,frame('Z',"I"));auto seen=std::make_shared<Observed>();PgDatabaseConnection backend(std::make_unique<ScriptTransport>(seen,server_error),std::nullopt,PgCatalogProfile::Redshift);ASSERT_TRUE(backend.connect(settings()));const auto error_result=observe(backend,"SELECT ?",binary,Deadline::max());ASSERT_TRUE(error_result.has_error());EXPECT_EQ(BackendErrorClass::Server,error_result.backend_error().error_class);EXPECT_EQ(std::optional<std::string>{"42501"},error_result.backend_error().native_state);EXPECT_EQ(SessionDisposition::Reusable,error_result.backend_error().disposition);EXPECT_EQ(0u,seen->close_count);
  {
    auto events=std::make_shared<Observed>();auto t=std::make_unique<ScriptTransport>(events,description(6551));auto* raw=t.get();PgDatabaseConnection c(std::move(t),std::nullopt,PgCatalogProfile::Redshift);ASSERT_TRUE(c.connect(settings()));raw->read_allocation=true;const auto result=observe(c,"SELECT ?",binary,Deadline::max());ASSERT_TRUE(result.has_error());EXPECT_EQ(BackendErrorClass::AllocationFailure,result.backend_error().error_class);EXPECT_EQ(SessionDisposition::Retire,result.backend_error().disposition);EXPECT_EQ(1u,events->close_count);ASSERT_EQ(2u,events->writes.size());EXPECT_EQ("PDS",tags(events->writes[1]));
  }
  for(bool partial:{false,true}) {
    auto events=std::make_shared<Observed>();auto t=std::make_unique<ScriptTransport>(events,description(6551));auto* raw=t.get();PgDatabaseConnection c(std::move(t),std::nullopt,PgCatalogProfile::Redshift);ASSERT_TRUE(c.connect(settings()));raw->partial_timeout=partial;raw->read_timeout=!partial;const auto d=rs::util::Clock::now()+std::chrono::seconds(3);const auto result=observe(c,"SELECT ?",binary,d);ASSERT_TRUE(result.has_error());EXPECT_EQ(BackendErrorClass::Timeout,result.backend_error().error_class);EXPECT_EQ(SessionDisposition::Retire,result.backend_error().disposition);EXPECT_EQ(1u,events->close_count);EXPECT_FALSE(c.is_connected());ASSERT_GE(events->writes.size(),2u);EXPECT_EQ("PDS",tags(events->writes[1]));for(std::size_t i=1;i<events->sends.size();++i){EXPECT_EQ(d,events->sends[i]);}
  }
  // On-time native error remains drained and permits same-session recovery.
  {
    std::vector<std::byte> replies;constexpr char denied[]="SERROR\0C42501\0Msynthetic denied\0\0";
    add(replies,frame('E',std::string_view(denied,sizeof(denied)-1)));add(replies,frame('Z',"I"));
    add(replies,frame('C',std::string_view("SELECT 0\0",9)));add(replies,frame('Z',"I"));
    auto events=std::make_shared<Observed>();PgDatabaseConnection c(std::make_unique<ScriptTransport>(events,replies),std::nullopt,PgCatalogProfile::Redshift);
    ASSERT_TRUE(c.connect(settings()));const auto d=rs::util::Clock::now()+std::chrono::seconds(3);const auto refused=observe(c,"SELECT ?",binary,d);
    ASSERT_TRUE(refused.has_error());EXPECT_EQ(BackendErrorClass::Server,refused.backend_error().error_class);
    EXPECT_EQ(std::optional<std::string>{"42501"},refused.backend_error().native_state);EXPECT_EQ(SessionDisposition::Reusable,refused.backend_error().disposition);
    ASSERT_TRUE(c.execute_query("SELECT 0",d));ASSERT_EQ(3u,events->writes.size());EXPECT_EQ("PDS",tags(events->writes[1]));EXPECT_EQ("Q",tags(events->writes[2]));EXPECT_TRUE(c.is_connected());EXPECT_EQ(0u,events->close_count);
  }
  // Successful final transport reads can cross D before inherited early exits.
  // On-time native-error and binary-format assertions above remain unchanged.
  for(int final_kind=0;final_kind<3;++final_kind) {
    SCOPED_TRACE(final_kind);
    auto response=description(6551);
    if(final_kind==0) {
      response.resize(response.size()-11);
      const unsigned char field[]{0,1,'v',0,0,0,0,0,0,0,0,0,0x19,0x97,0xff,0xff,0xff,0xff,0xff,0xff,0,1};
      add(response,frame('T',std::string_view(reinterpret_cast<const char*>(field),sizeof(field))));add(response,frame('Z',"I"));
    } else if(final_kind==1) {
      response.clear();constexpr char denied[]="SERROR\0C42501\0Msynthetic denied\0\0";
      add(response,frame('E',std::string_view(denied,sizeof(denied)-1)));add(response,frame('Z',"I"));
    }
    auto events=std::make_shared<Observed>();auto t=std::make_unique<ScriptTransport>(events,response);auto* raw=t.get();
    PgDatabaseConnection c(std::move(t),std::nullopt,PgCatalogProfile::Redshift);ASSERT_TRUE(c.connect(settings()));
    const auto read_before=events->reads.size();const auto d=rs::util::Clock::now()+std::chrono::milliseconds(20);bool crossed=false;
    raw->on_final_query_recv=[&]{std::this_thread::sleep_until(d+std::chrono::milliseconds(1));crossed=true;};
    const auto result=observe(c,"SELECT ?",binary,d);ASSERT_TRUE(crossed);ASSERT_TRUE(result.has_error());
    EXPECT_EQ(BackendErrorClass::Timeout,result.backend_error().error_class);EXPECT_EQ(BackendOperation::ExecutePrepared,result.backend_error().operation);
    EXPECT_EQ(SessionDisposition::Retire,result.backend_error().disposition);EXPECT_FALSE(c.is_connected());EXPECT_EQ(1u,events->close_count);
    ASSERT_EQ(2u,events->writes.size());EXPECT_EQ("PDS",tags(events->writes[1]));
    EXPECT_EQ(d,events->sends[1]);for(std::size_t i=read_before;i<events->reads.size();++i){EXPECT_EQ(d,events->reads[i]);}
  }

}
