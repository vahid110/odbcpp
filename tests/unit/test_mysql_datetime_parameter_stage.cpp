#include <gtest/gtest.h>
#include "core/database/mysql/mysql_session.h"
#include "core/database/session_owner.h"
#include <thread>

namespace {
using namespace rs::core::database::mysql;
using rs::util::DbErrorCode;
using Bytes = std::vector<std::byte>;
void number(Bytes& bytes, std::uint32_t value, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 255));
}
void text(Bytes& bytes, std::string_view value) {
  for (const auto c : value) bytes.push_back(static_cast<std::byte>(c));
  bytes.push_back(std::byte{0});
}
Bytes greeting(std::uint32_t capabilities = client_protocol_41 | client_ssl | client_secure_connection | client_plugin_auth) {
  Bytes bytes{std::byte{10}};
  text(bytes, "8.4.11"); number(bytes, 0x12345678, 4);
  for (unsigned i = 0; i < 8; ++i) number(bytes, i, 1);
  number(bytes, 0, 1); number(bytes, capabilities & 65535, 2);
  number(bytes, 45, 1); number(bytes, 2, 2); number(bytes, capabilities >> 16, 2);
  number(bytes, 21, 1);
  for (unsigned i = 0; i < 10; ++i) number(bytes, 0, 1);
  for (unsigned i = 8; i < 20; ++i) number(bytes, i, 1);
  number(bytes, 0, 1); text(bytes, "caching_sha2_password");
  return bytes;
}
Bytes packet(const Bytes& payload, std::uint8_t sequence = 0) {
  Bytes bytes(payload.size() + 4);
  for (std::size_t i = 0; i < 3; ++i)
    bytes[i] = static_cast<std::byte>((payload.size() >> (8 * i)) & 255);
  bytes[3] = static_cast<std::byte>(sequence);
  std::copy(payload.begin(), payload.end(), bytes.begin() + 4);
  return bytes;
}

class FakeTransport : public rs::core::transport::ITransport,
                      public rs::core::transport::IStartTlsTransport,
                      public rs::core::transport::ITlsConfigurableTransport {
 public:
  void set_ca_locations(const std::string&,const std::string&) override { ++configuration_calls; }
  Bytes input=packet(greeting()), output;
  std::size_t configuration_calls{}, offset{}, chunk{1}, calls{}, closes{}, upgrades{};
  std::optional<std::size_t> throw_recv_offset,fail_send_offset,lose_peer_after_output;
  std::optional<std::size_t> expire_after_output;
  int fail_at{-1}; bool verified{true}, throw_io{}, zero{}, oversize{}, eof{}; int bad_send{-1};
  rs::util::Deadline expected{};
  std::vector<rs::util::Deadline> deadlines;
  rs::util::Result<void> connect(std::string_view, std::uint16_t, rs::util::Deadline) override {
    ADD_FAILURE() << "Must connect plaintext explicitly"; return {DbErrorCode::ProtocolError};
  }
  rs::util::Result<void> connect_plain(std::string_view host, std::uint16_t port, rs::util::Deadline dl) override {
    EXPECT_EQ("localhost",host); EXPECT_EQ(3306,port); deadlines.push_back(dl);
    if (fail_at==0) return {DbErrorCode::NetworkError,"unsafe native detail"};
    return {};
  }
  rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte> out, rs::util::Deadline dl) override {
    deadlines.push_back(dl); ++calls;
    if (throw_io || (throw_recv_offset && offset>=*throw_recv_offset)) throw std::runtime_error("test failure");
    if (fail_at==1) return {DbErrorCode::Timeout,"unsafe native detail"};
    if (zero || oversize || eof) return rs::core::transport::IOResult{oversize?out.size()+1:0,eof};
    const auto n=std::min({chunk,out.size(),input.size()-offset});
    std::copy_n(input.begin()+offset,n,out.begin()); offset+=n;
    return rs::core::transport::IOResult{n,false};
  }
  rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte> in, rs::util::Deadline dl) override {
    deadlines.push_back(dl);
    if (fail_send_offset && output.size()>=*fail_send_offset) return {DbErrorCode::NetworkError};
    if (fail_at==2) return {DbErrorCode::NetworkError,"unsafe native detail"};
    if (bad_send>=0 && !output.empty()) return rs::core::transport::IOResult{bad_send==1?in.size()+1:0,bad_send==2};
    const auto n=std::min(chunk,in.size()); output.insert(output.end(),in.begin(),in.begin()+n);
    if (lose_peer_after_output && output.size()>=*lose_peer_after_output) verified=false;
    if (expire_after_output && output.size()>=*expire_after_output) std::this_thread::sleep_until(dl);
    return rs::core::transport::IOResult{n,false};
  }
  rs::util::Result<void> upgrade_to_tls(std::string_view host, rs::util::Deadline dl) override {
    EXPECT_EQ("localhost",host); deadlines.push_back(dl); ++upgrades;
    if (fail_at==3) return {DbErrorCode::TLSError,"unsafe native detail"};
    return {};
  }
  bool peer_identity_verified() noexcept override { return verified; }
  void close() noexcept override { ++closes; }
};


Bytes ok() { return Bytes{std::byte{0},std::byte{0},std::byte{0},std::byte{2},std::byte{0},std::byte{0},std::byte{0}}; }
void append(FakeTransport& t, const Bytes& payload, std::uint8_t sequence) {
  auto next=packet(payload,sequence);t.input.insert(t.input.end(),next.begin(),next.end());
}
Bytes eof_packet(unsigned status=2,unsigned warnings=0) {
  Bytes bytes{std::byte{254}};number(bytes,warnings,2);number(bytes,status,2);return bytes;
}
void len_text(Bytes& bytes,std::string_view value) {
  ASSERT_LT(value.size(),251u);number(bytes,static_cast<unsigned>(value.size()),1);
  for (const auto ch:value) bytes.push_back(static_cast<std::byte>(ch));
}
Bytes column_packet(std::string_view name,unsigned type=8,unsigned charset=63,unsigned flags=0,
    std::optional<unsigned> width=std::nullopt,unsigned precision=0) {
  Bytes bytes;for (const auto field:{std::string_view("def"),std::string_view{},std::string_view{},std::string_view{},name,std::string_view{}}) len_text(bytes,field);
  number(bytes,12,1);number(bytes,charset,2);number(bytes,width.value_or(type==8?20:40),4);number(bytes,type,1);
  number(bytes,flags,2);number(bytes,precision,1);number(bytes,0,2);return bytes;
}
rs::core::database::ConnectionSettings settings() {
  rs::core::database::ConnectionSettings s;s.host="localhost";s.port=3306;s.user="sdk";s.password="secret";return s;
}
struct Fixture {
  FakeTransport* transport{};std::unique_ptr<MySqlSession> session;
  explicit Fixture(rs::core::database::ConnectionSettings config=settings()) {
    auto owned=std::make_unique<FakeTransport>();transport=owned.get();append(*transport,ok(),3);
    session=std::make_unique<MySqlSession>(std::move(owned));EXPECT_TRUE(session->connect(config));
    transport->output.clear();transport->deadlines.clear();
  }
  auto execute(std::string_view sql="SELECT 1") {
    transport->expected=rs::util::make_deadline(std::chrono::seconds(10));
    return session->execute_query(sql,transport->expected);
  }
  void rows(bool invalid=false) {
    append(*transport,{std::byte{2}},1);append(*transport,column_packet("id"),2);
    append(*transport,column_packet("value",253,45),3);append(*transport,eof_packet(),4);
    Bytes row;len_text(row,invalid?"invalid":"42");len_text(row,std::string_view("a\0b",3));append(*transport,row,5);
    append(*transport,{std::byte{251},std::byte{0}},6);append(*transport,eof_packet(),7);
  }
};
using namespace rs::core::database;
using Parameter=QueryParameter;
using Hint=QueryParameterType;
Bytes head(unsigned parameters,unsigned results=0) {
  Bytes b{std::byte{0}};number(b,17,4);number(b,results,2);number(b,parameters,2);
  number(b,0,1);number(b,0,2);return b;
}
// Synthetic packet reconstruction from captured numeric12/45/104/q6 only.
// No names/flags/packet bytes were captured by the native diagnostic.
Bytes dt(unsigned width=104,unsigned q=6,unsigned charset=45) {
  return column_packet("datetime",12,charset,0,width,q);
}
std::vector<unsigned> commands(const Bytes& bytes) {
  std::vector<unsigned> result;std::size_t offset=0;
  while (offset<bytes.size()) {
    if (bytes.size()-offset<5) { ADD_FAILURE()<<"short command frame";return {}; }
    const auto n=std::to_integer<std::size_t>(bytes[offset])|(std::to_integer<std::size_t>(bytes[offset+1])<<8)|(std::to_integer<std::size_t>(bytes[offset+2])<<16);
    if (!n || n>bytes.size()-offset-4) { ADD_FAILURE()<<"bad command length";return {}; }
    EXPECT_EQ(std::byte{0},bytes[offset+3]);result.push_back(std::to_integer<unsigned>(bytes[offset+4]));offset+=n+4;
  }
  return result;
}
void retired(Fixture& f,const BackendResult<QueryResult>& result,DbErrorCode error,
    std::size_t boundary,rs::util::Deadline deadline,std::vector<unsigned> expected={22},
    BackendOperation operation=BackendOperation::ExecutePrepared) {
  ASSERT_FALSE(result);EXPECT_EQ(error,result.error());EXPECT_EQ("MySQL session operation failed",result.error_message());
  EXPECT_EQ(operation,result.backend_error().operation);EXPECT_EQ(SessionState::Disconnected,result.session_snapshot().state);
  EXPECT_EQ(SessionDisposition::Retire,result.session_snapshot().disposition);EXPECT_FALSE(f.session->is_connected());
  EXPECT_EQ(boundary,f.transport->offset);EXPECT_LT(boundary,f.transport->input.size());EXPECT_EQ(1u,f.transport->closes);
  EXPECT_EQ(expected,commands(f.transport->output));for (const auto dl:f.transport->deadlines) { EXPECT_EQ(deadline,dl); }
  const auto output=f.transport->output;const auto calls=f.transport->calls;
  auto again=f.session->execute_query("SELECT 1",deadline);ASSERT_FALSE(again);EXPECT_EQ(DbErrorCode::NotConnected,again.error());
  EXPECT_EQ(output,f.transport->output);EXPECT_EQ(calls,f.transport->calls);f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
}
void close_and_recover(Fixture& f,const BackendResult<QueryResult>& result,DbErrorCode error,
    std::size_t boundary,rs::util::Deadline deadline) {
  ASSERT_FALSE(result);EXPECT_EQ(error,result.error());EXPECT_EQ(boundary,f.transport->offset);
  EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(0u,f.transport->closes);
  EXPECT_EQ(SessionDisposition::Reusable,result.session_snapshot().disposition);
  EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output));
  ASSERT_GE(f.transport->output.size(),9u);
  const Bytes literal{std::byte{5},std::byte{0},std::byte{0},std::byte{0},std::byte{25},std::byte{17},std::byte{0},std::byte{0},std::byte{0}};
  EXPECT_EQ(literal,Bytes(f.transport->output.end()-9,f.transport->output.end()));
  auto recovery=f.session->execute_query("SELECT 1",deadline);ASSERT_TRUE(recovery);EXPECT_EQ(f.transport->input.size(),f.transport->offset);
  EXPECT_EQ((std::vector<unsigned>{22,25,3}),commands(f.transport->output));
  for (const auto dl:f.transport->deadlines) { EXPECT_EQ(deadline,dl); }
}
TEST(MySqlDatetimeParameterStageTest, QualifiedTextAndNullRefuseBeforeQueuedEofWithoutExecuteOrClose) {
  for (bool null:{false,true}) {
    Fixture f;append(*f.transport,head(1,2),1);append(*f.transport,dt(),2);const auto boundary=f.transport->input.size();
    append(*f.transport,eof_packet(),3);append(*f.transport,dt(26),4);append(*f.transport,column_packet("n"),5);
    append(*f.transport,eof_packet(),6);append(*f.transport,ok(),1);
    const std::array params{Parameter{null?std::nullopt:std::optional<std::string>{"2024-02-29 12:34:56.123456"},Hint::Text}};
    const auto dl=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT ?",params,dl);retired(f,result,DbErrorCode::UnsupportedFeature,boundary,dl);
  }
}
TEST(MySqlDatetimeParameterStageTest, ParameterShapeAndEveryMalformedPayloadPrefixHaveExactUnreadCategories) {
  const auto valid=dt();
  for (std::size_t fault=0;fault<valid.size()+7;++fault) {
    Fixture f;append(*f.transport,head(1),1);Bytes bad;auto expected=DbErrorCode::ProtocolError;
    if (fault<valid.size()) { bad=Bytes(valid.begin(),valid.begin()+fault); }
    else if (fault==valid.size()) { bad=dt(26); }
    else if (fault==valid.size()+1) { bad=dt(103); }
    else if (fault==valid.size()+2) { bad=dt(105); }
    else if (fault==valid.size()+3) { bad=dt(104,6,63);expected=DbErrorCode::UnsupportedFeature; }
    else if (fault==valid.size()+4) { bad=dt(76,0);expected=DbErrorCode::UnsupportedFeature; }
    else if (fault==valid.size()+5) { bad=dt(92,3);expected=DbErrorCode::UnsupportedFeature; }
    else { bad=valid;bad.back()=std::byte{1}; }
    append(*f.transport,bad,2);const auto boundary=f.transport->input.size();append(*f.transport,eof_packet(),3);append(*f.transport,ok(),1);
    const std::array params{Parameter{"1",Hint::Text}};const auto dl=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT ?",params,dl);retired(f,result,expected,boundary,dl);
  }
}
TEST(MySqlDatetimeParameterStageTest, PreparationResultContextStaysResultWithAndWithoutParameters) {
  for (bool parameter:{false,true}) {
    Fixture f;append(*f.transport,head(parameter?1:0,1),1);std::uint8_t seq=2;
    if (parameter) { append(*f.transport,column_packet("p"),seq++);append(*f.transport,eof_packet(),seq++); }
    append(*f.transport,dt(),seq++);const auto boundary=f.transport->input.size();append(*f.transport,eof_packet(),seq);append(*f.transport,ok(),1);
    std::vector<Parameter> params;if (parameter) { params.push_back(Parameter{"1",Hint::Text}); }
    const auto dl=rs::util::make_deadline(std::chrono::seconds(10));auto result=f.session->execute_prepared("SELECT 1",params,dl);
    retired(f,result,DbErrorCode::ProtocolError,boundary,dl);
  }
}
TEST(MySqlDatetimeParameterStageTest, DirectAndFreshExecutionResultsNeverSelectParameterContext) {
  for (bool prepared:{false,true}) {
    Fixture f;
    if (prepared) {
      append(*f.transport,head(1,1),1);append(*f.transport,column_packet("p"),2);append(*f.transport,eof_packet(),3);
      append(*f.transport,dt(26),4);append(*f.transport,eof_packet(),5);
    }
    append(*f.transport,{std::byte{1}},1);append(*f.transport,dt(),2);const auto boundary=f.transport->input.size();
    append(*f.transport,eof_packet(),3);append(*f.transport,{std::byte{0}},4);append(*f.transport,eof_packet(),5);
    const std::array params{Parameter{"1",Hint::Text}};const auto dl=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=prepared?f.session->execute_prepared("SELECT ?",params,dl):f.session->execute_query("SELECT 1",dl);
    retired(f,result,DbErrorCode::ProtocolError,boundary,dl,prepared?std::vector<unsigned>{22,23}:std::vector<unsigned>{3},
        prepared?BackendOperation::ExecutePrepared:BackendOperation::ExecuteDirect);
  }
}
TEST(MySqlDatetimeParameterStageTest, SupportedCountAndDateHintMismatchDrainAllMetadataCloseWithoutAckAndRecover) {
  for (bool count:{false,true}) {
    Fixture f;append(*f.transport,head(count?2:1,1),1);std::uint8_t seq=2;
    append(*f.transport,column_packet("p"),seq++);
    if (count) { append(*f.transport,column_packet("extra"),seq++); }
    append(*f.transport,eof_packet(),seq++);append(*f.transport,dt(26),seq++);append(*f.transport,eof_packet(),seq);
    const auto boundary=f.transport->input.size();append(*f.transport,ok(),1);
    const std::array params{Parameter{count?"1":"2000-02-29",count?Hint::Text:Hint::Date}};
    const auto dl=rs::util::make_deadline(std::chrono::seconds(10));auto result=f.session->execute_prepared("SELECT ?",params,dl);
    close_and_recover(f,result,count?DbErrorCode::InvalidParameter:DbErrorCode::UnsupportedFeature,boundary,dl);
  }
}
TEST(MySqlDatetimeParameterStageTest, UnsupportedExtraColumnAndLateEofFaultBeatCountMismatch) {
  for (bool timestamp:{false,true}) {
    Fixture f;append(*f.transport,head(2,1),1);append(*f.transport,column_packet("p"),2);
    append(*f.transport,timestamp?dt():column_packet("extra"),3);std::size_t boundary=f.transport->input.size();
    if (!timestamp) { boundary+=4;append(*f.transport,eof_packet(),99); } // Badsequence rejects at header, before EOFbody.
    append(*f.transport,eof_packet(),4);append(*f.transport,dt(26),5);append(*f.transport,eof_packet(),6);
    const std::array params{Parameter{"1",Hint::Text}};const auto dl=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT ?",params,dl);
    retired(f,result,timestamp?DbErrorCode::UnsupportedFeature:DbErrorCode::ProtocolError,boundary,dl);
  }
}
TEST(MySqlDatetimeParameterStageTest, PrepareCountLimitsAndExplicitTimestampWriterRefusalRemainEarlierThanMetadata) {
  auto config=settings();config.input_limits.max_parameters=1;Fixture limited(config);
  append(*limited.transport,head(2),1);const auto boundary=limited.transport->input.size();append(*limited.transport,dt(),2);
  const std::array text_params{Parameter{"1",Hint::Text}};const auto dl=rs::util::make_deadline(std::chrono::seconds(10));
  auto limit=limited.session->execute_prepared("SELECT ?",text_params,dl);retired(limited,limit,DbErrorCode::ResourceLimit,boundary,dl);
  for (bool null:{false,true}) {
    Fixture f;const auto offset=f.transport->offset;const auto calls=f.transport->calls;
    const std::array params{Parameter{null?std::nullopt:std::optional<std::string>{"2024-02-29 12:34:56.123456"},Hint::Timestamp}};
    auto result=f.session->execute_prepared("SELECT ?",params,dl);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
    EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(SessionDisposition::Reusable,result.session_snapshot().disposition);
    EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(offset,f.transport->offset);EXPECT_EQ(calls,f.transport->calls);EXPECT_EQ(0u,f.transport->closes);
  }
}
}
