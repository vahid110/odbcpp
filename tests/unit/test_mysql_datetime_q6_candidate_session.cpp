#include "core/database/mysql/mysql_session.h"
#include <gtest/gtest.h>
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
  std::optional<std::size_t> expire_after_output,lose_peer_after_input,expire_after_input;
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
    if (lose_peer_after_input && offset>=*lose_peer_after_input) { verified=false; }
    if (expire_after_input && offset>=*expire_after_input) { std::this_thread::sleep_until(dl); }
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
Bytes output_frame(const Bytes& output,std::size_t frame) {
  std::size_t offset{};
  for (std::size_t i=0;i<=frame;++i) {
    if (output.size()-offset<4) { return {}; }
    const auto size=std::to_integer<std::size_t>(output[offset])|(std::to_integer<std::size_t>(output[offset+1])<<8)|(std::to_integer<std::size_t>(output[offset+2])<<16);
    if (size+4>output.size()-offset) { return {}; }
    if (i==frame) { return Bytes(output.begin()+offset,output.begin()+offset+size+4); }
    offset+=size+4;
  }
  return {};
}
std::vector<unsigned> commands(const Bytes& output) {
  std::vector<unsigned> result;for (std::size_t i=0;;++i) { const auto frame=output_frame(output,i);if (frame.empty()) { return result; }result.push_back(std::to_integer<unsigned>(frame[4])); }
}
using namespace rs::core::database;
constexpr auto policy=prepared_detail::DatetimeWriterPolicy::DateDatetimeQ6Candidate;
const QueryParameter stamp{"2024-01-02 00:00:00",QueryParameterType::Timestamp};
Bytes literal(std::initializer_list<unsigned> values) {
  Bytes result;for (const auto value:values) { result.push_back(static_cast<std::byte>(value)); }return result;
}
Bytes q6_column(unsigned width=104,unsigned q=6,unsigned charset=45) {
  return column_packet("?",12,charset,0,width,q);
}
Bytes date_column(unsigned charset=45) { return column_packet("?",10,charset,0,charset==63?10:40); }
void prepare(FakeTransport& t,const std::vector<Bytes>& parameters={q6_column()},
    const std::vector<Bytes>& results={column_packet("value",3,63,0,11)},unsigned status=2) {
  append(t,literal({0,4,3,2,1,static_cast<unsigned>(results.size()),0,static_cast<unsigned>(parameters.size()),0,0,0,0}),1);
  std::uint8_t sequence=2;
  for (const auto& column:parameters) { append(t,column,sequence++); }
  if (!parameters.empty()) { append(t,eof_packet(status),sequence++); }
  for (const auto& column:results) { append(t,column,sequence++); }
  if (!results.empty()) { append(t,eof_packet(status),sequence); }
}
void result(FakeTransport& t,unsigned status=2) {
  append(t,literal({1}),1);append(t,column_packet("value",3,63,0,11),2);
  append(t,eof_packet(),3);append(t,literal({0,0,7,0,0,0}),4);append(t,eof_packet(status),5);
}
auto execute(Fixture& f,std::span<const QueryParameter> parameters,rs::util::Deadline deadline,
    prepared_detail::DatetimeWriterPolicy selected=policy) {
  return f.session->execute_datetime_q6_candidate("SELECT q6",parameters,deadline,selected);
}
void deadlines(const FakeTransport& t,rs::util::Deadline deadline) {
  ASSERT_FALSE(t.deadlines.empty());for (const auto actual:t.deadlines) { EXPECT_EQ(deadline,actual); }
}
void error(const BackendResult<QueryResult>& outcome,DbErrorCode expected,bool reusable) {
  ASSERT_FALSE(outcome);EXPECT_EQ(expected,outcome.error());
  EXPECT_EQ(BackendOperation::ExecutePrepared,outcome.backend_error().operation);
  EXPECT_EQ(reusable?SessionState::Idle:SessionState::Disconnected,outcome.session_snapshot().state);
  EXPECT_EQ(reusable?SessionDisposition::Reusable:SessionDisposition::Retire,outcome.session_snapshot().disposition);
  EXPECT_EQ("MySQL session operation failed",outcome.backend_error().message);
}
void recovery(Fixture& f) {
  prepare(*f.transport);result(*f.transport);
  auto recovered=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_TRUE(recovered);
  ASSERT_EQ(1u,recovered->rows.size());ASSERT_EQ(1u,recovered->rows[0].size());ASSERT_TRUE(recovered->rows[0][0]);EXPECT_EQ("7",*recovered->rows[0][0]);
}
}
TEST(MySqlDatetimeQ6CandidateSessionTest, EveryCallerPrecisionAndNullUsesLiteralFramesActualReceiptsAndOwningResults) {
  const std::array<const char*,7> fractions{"",".1",".12",".123",".1234",".12345",".123456"};
  const std::array<Bytes,6> micros{literal({0xa0,0x86,1,0}),literal({0xc0,0xd4,1,0}),literal({0x78,0xe0,1,0}),
    literal({8,0xe2,1,0}),literal({0x3a,0xe2,1,0}),literal({0x40,0xe2,1,0})};
  for (unsigned p=0;p<=7;++p) {
    Fixture f;prepare(*f.transport);result(*f.transport);const auto d=rs::util::make_deadline(std::chrono::seconds(1));
    QueryParameter caller{p==7?std::optional<std::string>{}:std::optional<std::string>{std::string("2024-01-02 00:00:00")+fractions[p]},QueryParameterType::Timestamp};
    auto outcome=execute(f,std::span(&caller,1),d);ASSERT_TRUE(outcome);
    Bytes expected;
    if (p==7) { expected=literal({14,0,0,0,23,4,3,2,1,0,1,0,0,0,1,1,12,0}); }
    else if (p==0) { expected=literal({19,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,4,0xe8,7,1,2}); }
    else {
      expected=literal({26,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,11,0xe8,7,1,2,0,0,0});
      expected.insert(expected.end(),micros[p-1].begin(),micros[p-1].end());
    }
    EXPECT_EQ(expected,output_frame(f.transport->output,1));
    EXPECT_EQ(literal({5,0,0,0,25,4,3,2,1}),output_frame(f.transport->output,2));
    EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));EXPECT_EQ(f.transport->input.size(),f.transport->offset);
    ASSERT_EQ(1u,outcome->normalized_parameter_types.size());const auto info=outcome->normalized_parameter_types[0];
    EXPECT_TRUE(info.known);EXPECT_EQ(ScalarType::Timestamp,info.type);EXPECT_EQ(26u,info.column_size);EXPECT_EQ(6,info.decimal_digits);
    ASSERT_EQ(1u,outcome->rows.size());ASSERT_EQ(1u,outcome->rows[0].size());ASSERT_TRUE(outcome->rows[0][0]);EXPECT_EQ("7",*outcome->rows[0][0]);
    deadlines(*f.transport,d);caller.value="changed";f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
    EXPECT_EQ("7",*outcome->rows[0][0]);EXPECT_EQ(26u,outcome->normalized_parameter_types[0].column_size);
  }
}
TEST(MySqlDatetimeQ6CandidateSessionTest, DateDatetimeBothOrdersAndNineBitmapUseSameParserFamilyReceipts) {
  for (bool reverse:{false,true}) {
    Fixture f;std::array<QueryParameter,2> callers{{{"2024-02-29",QueryParameterType::Date},stamp}};
    std::vector<Bytes> columns{date_column(reverse?63:45),q6_column()};
    if (reverse) { std::swap(callers[0],callers[1]);std::swap(columns[0],columns[1]); }
    prepare(*f.transport,columns);result(*f.transport);const auto d=rs::util::make_deadline(std::chrono::seconds(1));
    auto outcome=execute(f,callers,d);ASSERT_TRUE(outcome);
    const auto expected=reverse?literal({26,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,10,0,4,0xe8,7,1,2,4,0xe8,7,2,29}):
      literal({26,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,10,0,12,0,4,0xe8,7,2,29,4,0xe8,7,1,2});
    EXPECT_EQ(expected,output_frame(f.transport->output,1));ASSERT_EQ(2u,outcome->normalized_parameter_types.size());
    EXPECT_EQ(reverse?ScalarType::Timestamp:ScalarType::Date,outcome->normalized_parameter_types[0].type);deadlines(*f.transport,d);
  }
  Fixture f;std::array<QueryParameter,9> callers{{{std::nullopt,QueryParameterType::Timestamp},{"2024-02-29",QueryParameterType::Date},
    {"-2",QueryParameterType::Int16},stamp,{"x",QueryParameterType::Text},{"1",QueryParameterType::Boolean},
    {std::string("\0\xff",2),QueryParameterType::Binary,true},{"7",QueryParameterType::Int32},{std::nullopt,QueryParameterType::Timestamp}}};
  prepare(*f.transport,{q6_column(),date_column(),column_packet("?",2,63,0,6),q6_column(),column_packet("?",253,45,0,40),
    column_packet("?",1,63,0,1),column_packet("?",252,63,0,2),column_packet("?",3,63,0,11),q6_column()});result(*f.transport);
  auto outcome=execute(f,callers,rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_TRUE(outcome);
  EXPECT_EQ(literal({53,0,0,0,23,4,3,2,1,0,1,0,0,0,1,1,1,12,0,10,0,2,0,12,0,253,0,1,0,252,0,3,0,12,0,
    4,0xe8,7,2,29,0xfe,0xff,4,0xe8,7,1,2,1,'x',1,2,0,0xff,7,0,0,0}),output_frame(f.transport->output,1));
  EXPECT_EQ(9u,outcome->normalized_parameter_types.size());EXPECT_EQ(f.transport->input.size(),f.transport->offset);
}
TEST(MySqlDatetimeQ6CandidateSessionTest, InvalidCallerAndInputBudgetsPrecedeIoAndClosedPolicyWithoutRetiring) {
  for (const auto& caller:std::array<QueryParameter,6>{{{"1900-02-29 00:00:00",QueryParameterType::Timestamp},
      {"2024-01-01 24:00:00",QueryParameterType::Timestamp},{"2024-01-01 00:00:00.1234567",QueryParameterType::Timestamp},
      {"2024-01-01 00:00:00",QueryParameterType::Timestamp,true},{std::nullopt,QueryParameterType::Timestamp,true},{"32768",QueryParameterType::Int16}}}) {
    Fixture f;const auto before=f.transport->calls;auto outcome=execute(f,std::span(&caller,1),rs::util::make_deadline(std::chrono::seconds(1)));
    error(outcome,DbErrorCode::InvalidParameter,true);EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(before,f.transport->calls);EXPECT_EQ(0u,f.transport->closes);recovery(f);
  }
  for (const auto hint:{QueryParameterType::Numeric,QueryParameterType::Time,QueryParameterType::Float32,QueryParameterType::Float64}) {
    Fixture f;const QueryParameter caller{std::nullopt,hint};auto outcome=execute(f,std::span(&caller,1),rs::util::make_deadline(std::chrono::seconds(1)));
    error(outcome,DbErrorCode::UnsupportedFeature,true);EXPECT_TRUE(f.transport->output.empty());recovery(f);
  }
  for (unsigned fault=0;fault<5;++fault) {
    auto config=settings();
    if (fault==0) { config.input_limits.max_parameters=0; }
    if (fault==1) { config.input_limits.max_parameter_bytes=18; }
    if (fault==2) { config.input_limits.max_parameter_total_bytes=18; }
    if (fault==3) { config.input_limits.max_request_wire_bytes=45; }
    Fixture f(config);auto outcome=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)),
      fault==4?static_cast<prepared_detail::DatetimeWriterPolicy>(99):policy);
    error(outcome,fault==4?DbErrorCode::UnsupportedFeature:DbErrorCode::ResourceLimit,true);EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(0u,f.transport->closes);
  }
  Fixture exact;prepare(*exact.transport);result(*exact.transport);auto okay=execute(exact,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_TRUE(okay);
}
TEST(MySqlDatetimeQ6CandidateSessionTest, SupportedWrongFamiliesFullyDrainBothEofsCloseAndRecoverInInputOrder) {
  const std::array<Bytes,5> actual{date_column(),q6_column(),q6_column(),column_packet("?",253,45,0,40),column_packet("?",3,63,0,11)};
  const std::array<QueryParameter,5> callers{{stamp,{"2024-02-29",QueryParameterType::Date},{"text",QueryParameterType::Text},stamp,stamp}};
  for (std::size_t i=0;i<5;++i) {
    Fixture f;prepare(*f.transport,{actual[i]});const auto end=f.transport->input.size();const auto d=rs::util::make_deadline(std::chrono::seconds(1));
    auto outcome=execute(f,std::span(&callers[i],1),d);error(outcome,DbErrorCode::UnsupportedFeature,true);
    EXPECT_EQ(end,f.transport->offset);EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output));
    EXPECT_EQ(literal({5,0,0,0,25,4,3,2,1}),output_frame(f.transport->output,1));deadlines(*f.transport,d);recovery(f);
  }
  for (bool reverse:{false,true}) {
    Fixture f;std::array<QueryParameter,2> inputs{{stamp,{"2024-02-29",QueryParameterType::Date}}};
    std::vector<Bytes> receipts{date_column(),q6_column()};
    if (reverse) { std::swap(inputs[0],inputs[1]);std::swap(receipts[0],receipts[1]); }
    prepare(*f.transport,receipts);auto outcome=execute(f,inputs,rs::util::make_deadline(std::chrono::seconds(1)));
    error(outcome,DbErrorCode::UnsupportedFeature,true);EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output));recovery(f);
  }
}
TEST(MySqlDatetimeQ6CandidateSessionTest, StructuralAndEofFailuresWinBeforeCountAndAffinityWithoutInventedClose) {
  for (unsigned fault=0;fault<7;++fault) {
    Fixture f;const auto start=f.transport->offset;
    std::vector<Bytes> receipts{date_column(),q6_column()};DbErrorCode expected=DbErrorCode::ProtocolError;
    if (fault==0) { receipts[1]=q6_column(103); }
    if (fault==1) { receipts[1]=q6_column(104,3);expected=DbErrorCode::UnsupportedFeature; }
    if (fault==2) { receipts[1]=q6_column(104,6,63);expected=DbErrorCode::UnsupportedFeature; }
    if (fault==3) { receipts[1]=column_packet("?",7,45,0,104,6);expected=DbErrorCode::UnsupportedFeature; }
    if (fault==4) { receipts[1]=column_packet("?",246,63,128,67,30);expected=DbErrorCode::UnsupportedFeature; }
    if (fault==5) { receipts[1].pop_back(); }
    prepare(*f.transport,receipts);
    if (fault==6) { f.transport->input[f.transport->input.size()-5]=std::byte{0}; }
    auto outcome=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));
    error(outcome,expected,false);EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));EXPECT_EQ(1u,f.transport->closes);
    EXPECT_GT(f.transport->offset,start);f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
    if (fault!=6) { EXPECT_LT(f.transport->offset,f.transport->input.size()); }
  }
  Fixture complete;prepare(*complete.transport,{date_column(),q6_column()});
  auto count=execute(complete,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_FALSE(count);
  EXPECT_EQ(DbErrorCode::InvalidParameter,count.error());EXPECT_EQ(SessionDisposition::Reusable,count.session_snapshot().disposition);
  EXPECT_EQ((std::vector<unsigned>{22,25}),commands(complete.transport->output));EXPECT_EQ(complete.transport->input.size(),complete.transport->offset);recovery(complete);
}
TEST(MySqlDatetimeQ6CandidateSessionTest, CloseFailurePrecedesPendingMismatchAndNeverReceivesImaginaryAck) {
  for (unsigned fault=0;fault<3;++fault) {
    Fixture f;prepare(*f.transport,{date_column()});const auto end=f.transport->input.size();
    const auto d=rs::util::make_deadline(std::chrono::milliseconds(30));
    if (fault==0) { f.transport->fail_send_offset=14; }
    if (fault==1) { f.transport->lose_peer_after_output=23; }
    if (fault==2) { f.transport->expire_after_output=23; }
    auto outcome=execute(f,std::span(&stamp,1),d);
    error(outcome,fault==0?DbErrorCode::NetworkError:(fault==1?DbErrorCode::TLSError:DbErrorCode::Timeout),false);
    EXPECT_EQ(end,f.transport->offset);EXPECT_EQ(1u,f.transport->closes);deadlines(*f.transport,d);
    EXPECT_EQ(fault==0?(std::vector<unsigned>{22}):(std::vector<unsigned>{22,25}),commands(f.transport->output));
  }
}
TEST(MySqlDatetimeQ6CandidateSessionTest, ResultContextsAndFreshExecutionBudgetRemainStrictAndCumulative) {
  for (bool fresh:{false,true}) {
    Fixture f;
    if (fresh) {
      prepare(*f.transport);append(*f.transport,literal({1}),1);append(*f.transport,q6_column(),2);
      append(*f.transport,eof_packet(),3);
    } else { prepare(*f.transport,{q6_column()},{q6_column()}); }
    auto outcome=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));
    error(outcome,DbErrorCode::ProtocolError,false);EXPECT_EQ(fresh?(std::vector<unsigned>{22,23}):(std::vector<unsigned>{22}),commands(f.transport->output));
    EXPECT_LT(f.transport->offset,f.transport->input.size());EXPECT_EQ(1u,f.transport->closes);
  }
  auto config=settings();config.response_limits.max_messages=9;Fixture bounded(config);prepare(*bounded.transport);result(*bounded.transport);
  auto rejected=execute(bounded,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));
  error(rejected,DbErrorCode::ResourceLimit,false);EXPECT_EQ((std::vector<unsigned>{22,23}),commands(bounded.transport->output));
  EXPECT_LT(bounded.transport->offset,bounded.transport->input.size());
  config.response_limits.max_messages=10;Fixture exact(config);prepare(*exact.transport);result(*exact.transport);
  auto accepted=execute(exact,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_TRUE(accepted);
  config.result_limits.max_metadata_entries=12;Fixture metadata(config);prepare(*metadata.transport);result(*metadata.transport);
  auto names=execute(metadata,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));
  error(names,DbErrorCode::ResourceLimit,false);EXPECT_EQ((std::vector<unsigned>{22,23}),commands(metadata.transport->output));
}
TEST(MySqlDatetimeQ6CandidateSessionTest, ActualStatusRemainsTransactionAndIdleOnlyEntryRefusesLocally) {
  Fixture f;prepare(*f.transport);result(*f.transport,1);
  auto outcome=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_TRUE(outcome);
  EXPECT_EQ(SessionState::Transaction,outcome.session_snapshot().state);EXPECT_EQ(SessionDisposition::ResetRequired,outcome.session_snapshot().disposition);
  const auto before=f.transport->output;const auto calls=f.transport->calls;
  auto next=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_FALSE(next);
  EXPECT_EQ(DbErrorCode::InvalidParameter,next.error());EXPECT_EQ(SessionState::Transaction,next.session_snapshot().state);
  EXPECT_EQ(SessionDisposition::ResetRequired,next.session_snapshot().disposition);EXPECT_EQ(before,f.transport->output);EXPECT_EQ(calls,f.transport->calls);
  f.session->disconnect();auto disconnected=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));
  error(disconnected,DbErrorCode::NotConnected,false);EXPECT_EQ(1u,f.transport->closes);
}
TEST(MySqlDatetimeQ6CandidateSessionTest, OriginalDeadlineAndPeerIdentityAreCheckedAfterLastReadAndSuccessfulClose) {
  for (unsigned fault=0;fault<4;++fault) {
    Fixture f;prepare(*f.transport);result(*f.transport);const auto d=rs::util::make_deadline(std::chrono::milliseconds(30));
    if (fault==0) { f.transport->expire_after_input=f.transport->input.size(); }
    if (fault==1) { f.transport->lose_peer_after_input=f.transport->input.size(); }
    if (fault==2) { f.transport->expire_after_output=46; }
    if (fault==3) { f.transport->lose_peer_after_output=46; }
    auto outcome=execute(f,std::span(&stamp,1),d);
    error(outcome,(fault==0 || fault==2)?DbErrorCode::Timeout:DbErrorCode::TLSError,false);
    EXPECT_EQ(f.transport->input.size(),f.transport->offset);EXPECT_EQ(1u,f.transport->closes);deadlines(*f.transport,d);
    EXPECT_EQ(fault<2?(std::vector<unsigned>{22,23}):(std::vector<unsigned>{22,23,25}),commands(f.transport->output));
  }
}
TEST(MySqlDatetimeQ6CandidateSessionTest, CompleteInvalidResultCellsDrainButFramingAndErrNeverPublishPartialRows) {
  {
    Fixture f;const auto date_result=column_packet("day",10,63,0,10);prepare(*f.transport,{q6_column()},{date_result});
    append(*f.transport,literal({1}),1);append(*f.transport,date_result,2);append(*f.transport,eof_packet(),3);
    append(*f.transport,literal({0,0,0}),4);append(*f.transport,literal({0,0,4,0xe8,7,2,29}),5);append(*f.transport,eof_packet(),6);
    auto outcome=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_TRUE(outcome);
    ASSERT_EQ(2u,outcome->rows.size());ASSERT_EQ(1u,outcome->rows[0].size());ASSERT_EQ(1u,outcome->rows[1].size());
    EXPECT_EQ(std::optional<std::string>{""},outcome->rows[0][0]);EXPECT_EQ(std::optional<std::string>{"2024-02-29"},outcome->rows[1][0]);
    EXPECT_EQ((std::vector<CellEncodingError>{{0,0}}),outcome->cell_errors);EXPECT_EQ(f.transport->input.size(),f.transport->offset);
    EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));recovery(f);
  }
  for (bool native_err:{false,true}) {
    Fixture f;prepare(*f.transport);append(*f.transport,literal({1}),1);append(*f.transport,column_packet("value",3,63,0,11),2);
    append(*f.transport,eof_packet(),3);append(*f.transport,literal({0,0,7,0,0,0}),4);
    append(*f.transport,native_err?literal({255,0x15,4,'#','4','2','S','0','2','s','e','c','r','e','t'}):literal({0,0,7}),5);
    append(*f.transport,eof_packet(),6);auto outcome=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));
    error(outcome,native_err?DbErrorCode::QueryFailed:DbErrorCode::ProtocolError,false);
    EXPECT_EQ((std::vector<unsigned>{22,23}),commands(f.transport->output));EXPECT_LT(f.transport->offset,f.transport->input.size());EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlDatetimeQ6CandidateSessionTest, NativeErrAndEarlyHeaderCapsRetireAtTheirActualStageWithoutDetailLeakage) {
  for (unsigned stage=0;stage<3;++stage) {
    for (bool malformed:{false,true}) {
      Fixture f;auto err=literal({255,0x15,4,'#','4','2','S','0','2','p','r','i','v','a','t','e'});
      if (malformed) { err[5]=std::byte{'!'}; }
      if (stage==0) { append(*f.transport,err,1); }
      else {
        append(*f.transport,literal({0,4,3,2,1,1,0,1,0,0,0,0}),1);
        if (stage==1) { append(*f.transport,err,2); }
        else { append(*f.transport,q6_column(),2);append(*f.transport,eof_packet(),3);append(*f.transport,err,4); }
      }
      append(*f.transport,eof_packet(),5);auto outcome=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));
      error(outcome,malformed?DbErrorCode::ProtocolError:DbErrorCode::QueryFailed,false);EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
      EXPECT_LT(f.transport->offset,f.transport->input.size());EXPECT_EQ(1u,f.transport->closes);
    }
  }
  auto config=settings();config.input_limits.max_parameters=1;Fixture cap(config);prepare(*cap.transport,{q6_column(103),q6_column()});
  auto outcome=execute(cap,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));error(outcome,DbErrorCode::ResourceLimit,false);
  EXPECT_EQ((std::vector<unsigned>{22}),commands(cap.transport->output));EXPECT_LT(cap.transport->offset,cap.transport->input.size());
}
TEST(MySqlDatetimeQ6CandidateSessionTest, NarrowDescriptorSharesShapeAndDefensivelyChecksObservationCoherence) {
  const query_detail::NativeParameterDescriptorObservation raw{12,104,6,45,std::uint16_t{0}};
  const NativeTypeInfo normalized{ScalarType::Timestamp,26,6,true};
  auto valid=prepared_detail::validate_datetime_parameter(stamp);ASSERT_TRUE(valid);
  ASSERT_TRUE(datetime_parameter_detail::descriptor(QueryParameterType::Timestamp,raw,normalized,*valid));
  auto null=prepared_detail::validate_datetime_parameter({std::nullopt,QueryParameterType::Timestamp});ASSERT_TRUE(null);
  ASSERT_TRUE(datetime_parameter_detail::descriptor(QueryParameterType::Timestamp,raw,normalized,*null));
  for (unsigned fault=0;fault<7;++fault) {
    auto caller=fault<2?*null:*valid;
    if (fault==0) { caller.caller_precision=0; }
    if (fault==1) { caller.micros=0; }
    if (fault==2) { caller.caller_precision.reset(); }
    if (fault==3) { caller.micros.reset(); }
    if (fault==4) { caller.caller_precision=7; }
    if (fault==5) { caller.micros=1000000; }
    if (fault==6) { caller.caller_precision=3;caller.micros=1; }
    auto refused=datetime_parameter_detail::descriptor(QueryParameterType::Timestamp,raw,normalized,caller);ASSERT_FALSE(refused);
    EXPECT_EQ(DbErrorCode::InvalidParameter,refused.error());
    auto bad_raw=raw;bad_raw.width=103;
    auto shape_first=datetime_parameter_detail::descriptor(QueryParameterType::Text,bad_raw,normalized,caller);ASSERT_FALSE(shape_first);
    EXPECT_EQ(DbErrorCode::ProtocolError,shape_first.error());
    auto hint_first=datetime_parameter_detail::descriptor(QueryParameterType::Text,raw,normalized,caller);ASSERT_FALSE(hint_first);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature,hint_first.error());
  }
}
TEST(MySqlDatetimeQ6CandidateSessionTest, OrdinaryVirtualAndMetadataObservationDefaultsRemainIndependent) {
  for (bool nullable:{false,true}) {
    Fixture f;const QueryParameter caller{nullable?std::optional<std::string>{}:stamp.value,QueryParameterType::Timestamp};
    auto ordinary=f.session->execute_prepared("SELECT q6",std::span(&caller,1),rs::util::make_deadline(std::chrono::seconds(1)));
    error(ordinary,DbErrorCode::UnsupportedFeature,true);EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(0u,f.transport->closes);
  }
  Fixture text;prepare(*text.transport);const QueryParameter ordinary{"text",QueryParameterType::Text};
  auto refused=text.session->execute_prepared("SELECT q6",std::span(&ordinary,1),rs::util::make_deadline(std::chrono::seconds(1)));
  error(refused,DbErrorCode::UnsupportedFeature,false);EXPECT_EQ((std::vector<unsigned>{22}),commands(text.transport->output));
  EXPECT_LT(text.transport->offset,text.transport->input.size());
}
TEST(MySqlDatetimeQ6CandidateSessionTest, RawSevenCalendarBoundsAndNullMandatoryAffinityHaveLiteralFrames) {
  struct Case { const char* input;Bytes frame; };
  for (const auto& item:std::array{
      Case{"1000-01-01 00:00:00",literal({19,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,4,0xe8,3,1,1})},
      Case{"2024-02-29 12:34:56",literal({22,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,7,0xe8,7,2,29,12,34,56})},
      Case{"9999-12-31 23:59:59.999999",literal({26,0,0,0,23,4,3,2,1,0,1,0,0,0,0,1,12,0,11,15,39,12,31,23,59,59,63,66,15,0})}}) {
    Fixture f;prepare(*f.transport);result(*f.transport);const QueryParameter caller{item.input,QueryParameterType::Timestamp};
    auto outcome=execute(f,std::span(&caller,1),rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_TRUE(outcome);
    EXPECT_EQ(item.frame,output_frame(f.transport->output,1));EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));
  }
  Fixture mismatch;prepare(*mismatch.transport,{date_column()});const QueryParameter nullable{std::nullopt,QueryParameterType::Timestamp};
  auto refused=execute(mismatch,std::span(&nullable,1),rs::util::make_deadline(std::chrono::seconds(1)));error(refused,DbErrorCode::UnsupportedFeature,true);
  EXPECT_EQ((std::vector<unsigned>{22,25}),commands(mismatch.transport->output));recovery(mismatch);
  Fixture pair;const std::array<QueryParameter,2> callers{{{std::nullopt,QueryParameterType::Date},{std::nullopt,QueryParameterType::Timestamp}}};
  prepare(*pair.transport,{date_column(),q6_column()});result(*pair.transport);
  auto outcome=execute(pair,callers,rs::util::make_deadline(std::chrono::seconds(1)));ASSERT_TRUE(outcome);
  EXPECT_EQ(literal({16,0,0,0,23,4,3,2,1,0,1,0,0,0,3,1,10,0,12,0}),output_frame(pair.transport->output,1));
}
TEST(MySqlDatetimeQ6CandidateSessionTest, ExactCrossStageWireNameAndCombinedRequestBudgetsHaveIndependentBoundaries) {
  for (unsigned cap=0;cap<3;++cap) {
    for (bool short_budget:{false,true}) {
      auto config=settings();
      if (cap==0) { config.response_limits.max_wire_bytes=short_budget?155:156; }
      if (cap==1) { config.result_limits.max_metadata_name_bytes=short_budget?19:20; }
      if (cap==2) { config.input_limits.max_request_wire_bytes=short_budget?45:46; }
      Fixture f(config);prepare(*f.transport);result(*f.transport);
      auto outcome=execute(f,std::span(&stamp,1),rs::util::make_deadline(std::chrono::seconds(1)));
      if (!short_budget) {
        ASSERT_TRUE(outcome);EXPECT_EQ(f.transport->input.size(),f.transport->offset);
        EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));
      } else {
        error(outcome,DbErrorCode::ResourceLimit,cap==2);
        EXPECT_EQ(cap==2?(std::vector<unsigned>{}):(std::vector<unsigned>{22,23}),commands(f.transport->output));
        EXPECT_EQ(cap==2?0u:1u,f.transport->closes);
      }
    }
  }
}
