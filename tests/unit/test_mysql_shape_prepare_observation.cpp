#include <gtest/gtest.h>
#include "core/database/mysql/mysql_session.h"
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
using Policy=MySqlSession::PrepareObservationPolicy;
using Stage=MySqlSession::PrepareObservationStage;
using Outcome=MySqlSession::PrepareObservationOutcome;
using Evidence=MySqlSession::CompletePrepareObservation;
const std::vector<Bytes> parameter_columns{
  column_packet("?",10,45,0,40),column_packet("?",12,45,0,104,6),column_packet("?",246,63,128,67,30)};
const std::vector<Bytes> result_columns{
  column_packet("day",10,45,0,10),column_packet("stamp",12,45,0,26,6),
  column_packet("amount",246,63,0,7,2),column_packet("neighbor",8,63,0,19)};
void prepare(FakeTransport& t,const std::vector<Bytes>& parameters=parameter_columns,
    const std::vector<Bytes>& results=result_columns,std::uint32_t id=17,unsigned status=2) {
  Bytes first{std::byte{0}};number(first,id,4);number(first,static_cast<std::uint32_t>(results.size()),2);
  number(first,static_cast<std::uint32_t>(parameters.size()),2);number(first,0,1);number(first,0,2);append(t,first,1);
  std::uint8_t sequence=2;
  for (const auto& column:parameters) { append(t,column,sequence++); }
  if (!parameters.empty()) { append(t,eof_packet(status),sequence++); }
  for (const auto& column:results) { append(t,column,sequence++); }
  if (!results.empty()) { append(t,eof_packet(status),sequence); }
}
Outcome observe(Fixture& f,rs::util::Deadline d,std::string_view sql="SELECT observation",
    Policy policy=Policy::DateDatetimeQ6Decimal65Q30ThreeByFour) {
  return f.session->observe_combined_prepare(sql,d,policy);
}
void deadlines(const FakeTransport& t,rs::util::Deadline d) {
  ASSERT_FALSE(t.deadlines.empty());for (const auto actual:t.deadlines) { EXPECT_EQ(d,actual); }
}
void exact(const Outcome& outcome,std::uint32_t id=17) {
  ASSERT_TRUE(outcome.result);const auto& r=*outcome.result;
  EXPECT_EQ(Stage::ClosedVerified,outcome.progress.stage);EXPECT_TRUE(outcome.progress.prepare_sent);
  EXPECT_TRUE(outcome.progress.count_verified);EXPECT_EQ(3,outcome.progress.parameter_count);EXPECT_EQ(4,outcome.progress.result_count);
  EXPECT_EQ(id,r.statement_id);EXPECT_EQ(3u,r.parameter_count);EXPECT_EQ(4u,r.result_count);
  EXPECT_TRUE(r.parameter_eof);EXPECT_TRUE(r.result_eof);EXPECT_TRUE(r.count_verified);EXPECT_TRUE(r.close_sent);EXPECT_TRUE(r.verified_completion);
  EXPECT_EQ(42u,r.metadata_entries);EXPECT_EQ(10u,r.response_messages);
  const std::array types{ScalarType::Date,ScalarType::Timestamp,ScalarType::Decimal};
  const std::array<std::uint64_t,3> sizes{10,26,65};const std::array<std::int16_t,3> q{0,6,30};
  const std::array<unsigned,3> rawtypes{10,12,246},widths{40,104,67},charsets{45,45,63};
  for (std::size_t i=0;i<3;++i) {
    EXPECT_TRUE(r.parameters[i].normalized.known);EXPECT_EQ(types[i],r.parameters[i].normalized.type);
    EXPECT_EQ(sizes[i],r.parameters[i].normalized.column_size);EXPECT_EQ(q[i],r.parameters[i].normalized.decimal_digits);
    EXPECT_EQ(rawtypes[i],r.parameters[i].raw.type);EXPECT_EQ(widths[i],r.parameters[i].raw.width);
    EXPECT_EQ(charsets[i],r.parameters[i].raw.charset);EXPECT_EQ(q[i],r.parameters[i].raw.decimals);
    ASSERT_TRUE(r.parameters[i].raw.flags);EXPECT_EQ(i==2?128u:0u,*r.parameters[i].raw.flags);
  }
  const std::array rt{ScalarType::Date,ScalarType::Timestamp,ScalarType::Decimal,ScalarType::BigInt};
  const std::array<std::uint64_t,4> rw{10,26,5,19};const std::array<std::int16_t,4> rq{0,6,2,0};
  for (std::size_t i=0;i<4;++i) {
    EXPECT_TRUE(r.result_types[i].known);EXPECT_EQ(rt[i],r.result_types[i].type);
    EXPECT_EQ(rw[i],r.result_types[i].column_size);EXPECT_EQ(rq[i],r.result_types[i].decimal_digits);
  }
  EXPECT_EQ(SessionState::Idle,outcome.result.session_snapshot().state);
  EXPECT_EQ(SessionDisposition::Reusable,outcome.result.session_snapshot().disposition);
}
void retired(Fixture& f,const Outcome& outcome,DbErrorCode error,rs::util::Deadline d) {
  ASSERT_FALSE(outcome.result);EXPECT_EQ(error,outcome.result.error());
  EXPECT_EQ(BackendOperation::ExecutePrepared,outcome.result.backend_error().operation);
  EXPECT_EQ(SessionState::Disconnected,outcome.result.session_snapshot().state);
  EXPECT_EQ(SessionDisposition::Retire,outcome.result.session_snapshot().disposition);
  EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);deadlines(*f.transport,d);
  f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
}
}
TEST(MySqlShapePrepareObservationTest, ExactSameParserTransactionClosesWithoutExecutionOrAck) {
  for (std::uint32_t id:{0u,17u,0xffffffffu}) {
    Fixture f;const auto start=f.transport->offset;const auto prior_calls=f.transport->calls;prepare(*f.transport,parameter_columns,result_columns,id);
    const auto end=f.transport->input.size();const auto d=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=observe(f,d);exact(result,id);ASSERT_TRUE(result.result);
    EXPECT_EQ(end-start,result.result->response_wire_bytes);EXPECT_EQ(3u*(3u+1u)+(3u+3u)+(3u+5u)+(3u+6u)+(3u+8u),result.result->metadata_name_bytes);
    EXPECT_EQ(end,f.transport->offset);EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output));
    const auto close=output_frame(f.transport->output,1);ASSERT_EQ(9u,close.size());
    for (std::size_t i=0;i<4;++i) { EXPECT_EQ(static_cast<std::byte>((id>>(8*i))&255),close[5+i]); }
    EXPECT_EQ(0u,f.transport->closes);EXPECT_EQ(f.transport->calls-prior_calls,end-start);deadlines(*f.transport,d);
  }
  // Cardinality is the policy: supported ordinary receipts are not replaced
  // by a fictional DATE/DATETIME/DECIMAL slot inventory.
  Fixture ordinary;
  const std::vector<Bytes> scalar_parameters(3,column_packet("?",8,63,0,19));
  const std::vector<Bytes> scalar_results(4,column_packet("n",8,63,0,19));
  prepare(*ordinary.transport,scalar_parameters,scalar_results);
  const auto d=rs::util::make_deadline(std::chrono::seconds(10));
  auto actual=observe(ordinary,d);ASSERT_TRUE(actual.result);
  for (const auto& receipt:actual.result->parameters) {
    EXPECT_EQ(8u,receipt.raw.type);EXPECT_EQ(63u,receipt.raw.charset);EXPECT_EQ(19u,receipt.raw.width);
    ASSERT_TRUE(receipt.raw.flags);EXPECT_EQ(0u,*receipt.raw.flags);
    EXPECT_TRUE(receipt.normalized.known);EXPECT_EQ(ScalarType::BigInt,receipt.normalized.type);
    EXPECT_EQ(19u,receipt.normalized.column_size);EXPECT_EQ(0,receipt.normalized.decimal_digits);
  }
  for (const auto& info:actual.result->result_types) {
    EXPECT_TRUE(info.known);EXPECT_EQ(ScalarType::BigInt,info.type);EXPECT_EQ(19u,info.column_size);
  }
  EXPECT_EQ((std::vector<unsigned>{22,25}),commands(ordinary.transport->output));
  EXPECT_EQ(ordinary.transport->input.size(),ordinary.transport->offset);deadlines(*ordinary.transport,d);
}
TEST(MySqlShapePrepareObservationTest, OwnsActualFlagsAcrossResetInputMutationAndDestruction) {
  std::optional<Evidence> saved;
  {
    Fixture f;prepare(*f.transport);const auto d=rs::util::make_deadline(std::chrono::seconds(10));
    auto first=observe(f,d);ASSERT_TRUE(first.result);saved=*first.result;
    auto changed=parameter_columns;changed[0]=column_packet("?",10,63,0x8000,10);
    changed[1]=column_packet("?",12,45,0x1234,104,6);prepare(*f.transport,changed,result_columns,29);
    auto second=observe(f,d);ASSERT_TRUE(second.result);EXPECT_EQ(29u,second.result->statement_id);
    ASSERT_TRUE(second.result->parameters[0].raw.flags);EXPECT_EQ(0x8000u,*second.result->parameters[0].raw.flags);
    ASSERT_TRUE(second.result->parameters[1].raw.flags);EXPECT_EQ(0x1234u,*second.result->parameters[1].raw.flags);
    EXPECT_EQ(63u,second.result->parameters[0].raw.charset);EXPECT_EQ(10u,second.result->parameters[0].raw.width);
    std::fill(f.transport->input.begin(),f.transport->input.end(),std::byte{255});f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
  }
  ASSERT_TRUE(saved);EXPECT_EQ(17u,saved->statement_id);EXPECT_EQ(40u,saved->parameters[0].raw.width);
  ASSERT_TRUE(saved->parameters[0].raw.flags);EXPECT_EQ(0u,*saved->parameters[0].raw.flags);
  EXPECT_EQ(65u,saved->parameters[2].normalized.column_size);EXPECT_EQ(5u,saved->result_types[2].column_size);
  // Observer scope is per-call: the next virtual call retains the default
  // immediate result-only family refusal, with queued EOF/results unread.
  for (const auto& receipt:{parameter_columns[1],parameter_columns[2]}) {
    Fixture f;prepare(*f.transport);const auto d=rs::util::make_deadline(std::chrono::seconds(10));
    ASSERT_TRUE(observe(f,d).result);
    prepare(*f.transport,{receipt});const auto end=f.transport->input.size();
    const std::array params{QueryParameter{"x",QueryParameterType::Text}};
    auto rejected=f.session->execute_prepared("SELECT ?",params,d);ASSERT_FALSE(rejected);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature,rejected.error());
    EXPECT_EQ(SessionDisposition::Retire,rejected.session_snapshot().disposition);
    EXPECT_EQ((std::vector<unsigned>{22,25,22}),commands(f.transport->output));
    EXPECT_LT(f.transport->offset,end);EXPECT_EQ(1u,f.transport->closes);deadlines(*f.transport,d);
  }
}
TEST(MySqlShapePrepareObservationTest, CountMismatchDrainsBothStagesIncludingZeroAndExtraThenRecovers) {
  for (unsigned pc:{0u,1u,2u,4u}) { for (unsigned rc:{0u,3u,5u}) {
    Fixture f;std::vector<Bytes> p(pc,parameter_columns[1]),r(rc,result_columns[1]);prepare(*f.transport,p,r);
    const auto end=f.transport->input.size();append(*f.transport,ok(),1);
    const auto d=rs::util::make_deadline(std::chrono::seconds(10));auto outcome=observe(f,d);
    ASSERT_FALSE(outcome.result);EXPECT_EQ(DbErrorCode::InvalidParameter,outcome.result.error());
    EXPECT_EQ(Stage::ClosedVerified,outcome.progress.stage);EXPECT_FALSE(outcome.progress.count_verified);
    EXPECT_EQ(pc,outcome.progress.parameter_count);EXPECT_EQ(rc,outcome.progress.result_count);
    EXPECT_EQ(end,f.transport->offset);EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output));
    EXPECT_EQ(SessionDisposition::Reusable,outcome.result.session_snapshot().disposition);
    EXPECT_EQ(0u,f.transport->closes);ASSERT_TRUE(f.session->execute_query("SELECT 1",d));deadlines(*f.transport,d);
  } }
  auto config=settings();config.input_limits.max_parameters=3;Fixture f(config);
  Bytes header{std::byte{0}};number(header,17,4);number(header,4,2);number(header,65535,2);number(header,0,3);append(*f.transport,header,1);
  const auto d=rs::util::make_deadline(std::chrono::seconds(10));auto outcome=observe(f,d);retired(f,outcome,DbErrorCode::ResourceLimit,d);
  EXPECT_EQ(Stage::PrepareHeaderValidated,outcome.progress.stage);EXPECT_EQ(65535u,outcome.progress.parameter_count);
  EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
  Fixture ordinary;
  prepare(*ordinary.transport,std::vector<Bytes>(2,column_packet("?",8,63,0,19)));
  const auto ordinary_end=ordinary.transport->input.size();append(*ordinary.transport,ok(),1);
  const auto ordinary_d=rs::util::make_deadline(std::chrono::seconds(10));
  auto scalar_mismatch=observe(ordinary,ordinary_d);ASSERT_FALSE(scalar_mismatch.result);
  EXPECT_EQ(DbErrorCode::InvalidParameter,scalar_mismatch.result.error());
  EXPECT_EQ(Stage::ClosedVerified,scalar_mismatch.progress.stage);EXPECT_FALSE(scalar_mismatch.progress.count_verified);
  EXPECT_EQ(2u,scalar_mismatch.progress.parameter_count);EXPECT_EQ(4u,scalar_mismatch.progress.result_count);
  EXPECT_EQ(ordinary_end,ordinary.transport->offset);EXPECT_EQ(0u,ordinary.transport->closes);
  EXPECT_EQ((std::vector<unsigned>{22,25}),commands(ordinary.transport->output));
  ASSERT_TRUE(ordinary.session->execute_query("SELECT 1",ordinary_d));deadlines(*ordinary.transport,ordinary_d);
}
TEST(MySqlShapePrepareObservationTest, LateStructuralAndUnadmittedProfilesPrecedeCountWithoutPublication) {
  const std::vector<std::pair<Bytes,DbErrorCode>> bad{
    {column_packet("?",10,45,0,39),DbErrorCode::ProtocolError},
    {column_packet("?",12,45,0,103,6),DbErrorCode::ProtocolError},
    {column_packet("?",246,63,128,66,30),DbErrorCode::ProtocolError},
    {column_packet("?",12,45,0,76,0),DbErrorCode::UnsupportedFeature},
    {column_packet("?",12,45,0,92,3),DbErrorCode::UnsupportedFeature},
    {column_packet("?",12,63,0,26,6),DbErrorCode::UnsupportedFeature},
    {column_packet("?",7,63,0,26,6),DbErrorCode::UnsupportedFeature},
    {column_packet("?",246,63,160,67,30),DbErrorCode::UnsupportedFeature}};
  for (const auto& [column,error]:bad) {
    Fixture f;auto p=parameter_columns;p.push_back(column);prepare(*f.transport,p);const auto end=f.transport->input.size();
    const auto d=rs::util::make_deadline(std::chrono::seconds(10));auto outcome=observe(f,d);retired(f,outcome,error,d);
    EXPECT_EQ(Stage::PrepareHeaderValidated,outcome.progress.stage);EXPECT_TRUE(outcome.progress.prepare_sent);
    EXPECT_FALSE(outcome.progress.count_verified);EXPECT_LT(f.transport->offset,end);EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
  }
}
TEST(MySqlShapePrepareObservationTest, EofSequenceTailNameAndResultContextErrorsAreAtomic) {
  for (unsigned mode=0;mode<8;++mode) {
    Fixture f;auto p=parameter_columns,r=result_columns;
    if (mode==0 || mode==1) { p.push_back(parameter_columns[0]); }
    if (mode==4) { p[2].pop_back(); }
    if (mode==5) { r[0]=column_packet(std::string_view("\xff",1),10,45,0,10); }
    if (mode==6) { r[1]=column_packet("stamp",12,45,0,104,6); }
    if (mode==7) { p[0][0]=std::byte{255}; }
    const auto start=f.transport->input.size();prepare(*f.transport,p,r);
    std::vector<std::size_t> offsets;for (std::size_t at=start;at<f.transport->input.size();) {
      offsets.push_back(at);const auto size=std::to_integer<std::size_t>(f.transport->input[at])|(std::to_integer<std::size_t>(f.transport->input[at+1])<<8)|(std::to_integer<std::size_t>(f.transport->input[at+2])<<16);at+=size+4;
    }
    ASSERT_EQ(p.size()+r.size()+3,offsets.size());
    if (mode==0) { f.transport->input[offsets[1+p.size()]+4]=std::byte{0}; }
    if (mode==1) { f.transport->input[offsets.back()+3]=std::byte{42}; }
    if (mode==2) { f.transport->input.resize(offsets[4]); }
    if (mode==3) { f.transport->input.resize(offsets[9]+6); }
    const auto d=rs::util::make_deadline(std::chrono::seconds(10));auto outcome=observe(f,d);
    retired(f,outcome,mode==2 || mode==3?DbErrorCode::NetworkError:DbErrorCode::ProtocolError,d);
    EXPECT_FALSE(outcome.progress.count_verified);EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
    EXPECT_EQ(mode==1 || mode==3 || mode==5 || mode==6?Stage::ParameterMetadataValidated:Stage::PrepareHeaderValidated,outcome.progress.stage);
  }
}
TEST(MySqlShapePrepareObservationTest, LocalValidationIdleAndRequestCapsHaveNoIo) {
  for (unsigned mode=0;mode<6;++mode) {
    auto config=settings();if (mode==4) { config.input_limits.max_sql_bytes=1; }
    if (mode==5) { config.input_limits.max_request_wire_bytes=31; }
    Fixture f(config);const auto calls=f.transport->calls;const auto offset=f.transport->offset;
    const auto d=rs::util::make_deadline(std::chrono::seconds(10));
    auto outcome=observe(f,d,mode==0?std::string_view{}:mode==1?std::string_view("bad\0sql",7):mode==2?std::string_view("\xff",1):std::string_view("SELECT observation"),
        mode==3?static_cast<Policy>(99):Policy::DateDatetimeQ6Decimal65Q30ThreeByFour);
    ASSERT_FALSE(outcome.result);EXPECT_EQ(Stage::Unavailable,outcome.progress.stage);EXPECT_FALSE(outcome.progress.prepare_sent);
    EXPECT_FALSE(outcome.progress.parameter_count);EXPECT_FALSE(outcome.progress.result_count);
    EXPECT_EQ(mode==3?DbErrorCode::UnsupportedFeature:mode>=4?DbErrorCode::ResourceLimit:DbErrorCode::InvalidParameter,outcome.result.error());
    EXPECT_EQ(calls,f.transport->calls);EXPECT_EQ(offset,f.transport->offset);EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(0u,f.transport->closes);
  }
  auto config=settings();config.input_limits.max_request_wire_bytes=32;Fixture f(config);prepare(*f.transport);
  const auto d=rs::util::make_deadline(std::chrono::seconds(10));ASSERT_TRUE(observe(f,d).result);
  f.session->disconnect();const auto calls=f.transport->calls;auto disconnected=observe(f,d);
  ASSERT_FALSE(disconnected.result);EXPECT_EQ(DbErrorCode::NotConnected,disconnected.result.error());EXPECT_EQ(calls,f.transport->calls);
  Fixture tx;append(*tx.transport,{std::byte{0},std::byte{0},std::byte{0},std::byte{3},std::byte{0},std::byte{0},std::byte{0}},1);
  ASSERT_TRUE(tx.session->execute_query("START TRANSACTION",d));tx.transport->output.clear();const auto io=tx.transport->calls;
  auto blocked=observe(tx,d);ASSERT_FALSE(blocked.result);EXPECT_EQ(DbErrorCode::InvalidParameter,blocked.result.error());
  EXPECT_EQ(SessionState::Transaction,blocked.result.session_snapshot().state);EXPECT_EQ(SessionDisposition::ResetRequired,blocked.result.session_snapshot().disposition);
  EXPECT_TRUE(tx.transport->output.empty());EXPECT_EQ(io,tx.transport->calls);EXPECT_EQ(0u,tx.transport->closes);
}
TEST(MySqlShapePrepareObservationTest, CrossStageNamesEntriesResponseAndMessageBudgetsDoNotReset) {
  for (unsigned mode=0;mode<4;++mode) {
    auto config=settings();if (mode==0) { config.result_limits.max_metadata_name_bytes=20; }
    if (mode==1) { config.result_limits.max_metadata_entries=41; }
    if (mode==2) { config.response_limits.max_messages=9; }
    if (mode==3) { config.response_limits.max_wire_bytes=80; }
    Fixture f(config);prepare(*f.transport);const auto end=f.transport->input.size();
    const auto d=rs::util::make_deadline(std::chrono::seconds(10));auto outcome=observe(f,d);retired(f,outcome,DbErrorCode::ResourceLimit,d);
    EXPECT_LT(f.transport->offset,end);EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
    if (mode==0 || mode==2) { EXPECT_EQ(Stage::ParameterMetadataValidated,outcome.progress.stage); }
    if (mode==1) { EXPECT_EQ(Stage::PrepareHeaderValidated,outcome.progress.stage); }
  }
  auto exact_config=settings();exact_config.result_limits.max_metadata_name_bytes=46;
  exact_config.result_limits.max_metadata_entries=42;exact_config.response_limits.max_messages=10;
  exact_config.response_limits.max_wire_bytes=241;
  Fixture exact_fixture(exact_config);prepare(*exact_fixture.transport);
  const auto d=rs::util::make_deadline(std::chrono::seconds(10));auto complete=observe(exact_fixture,d);
  exact(complete);ASSERT_TRUE(complete.result);EXPECT_EQ(241u,complete.result->response_wire_bytes);
  EXPECT_EQ(46u,complete.result->metadata_name_bytes);deadlines(*exact_fixture.transport,d);
  for (unsigned cap=0;cap<4;++cap) {
    auto config=exact_config;
    if (cap==0) { config.result_limits.max_metadata_name_bytes=45; }
    if (cap==1) { config.result_limits.max_metadata_entries=41; }
    if (cap==2) { config.response_limits.max_messages=9; }
    if (cap==3) { config.response_limits.max_wire_bytes=240; }
    Fixture f(config);prepare(*f.transport);const auto end=f.transport->input.size();
    auto outcome=observe(f,d);retired(f,outcome,DbErrorCode::ResourceLimit,d);
    EXPECT_LT(f.transport->offset,end);EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
    EXPECT_EQ(cap==1?Stage::PrepareHeaderValidated:Stage::ParameterMetadataValidated,outcome.progress.stage);
  }
}
TEST(MySqlShapePrepareObservationTest, CloseFailureDeadlineAndPeerOverrideMismatchWithoutAckOrReplay) {
  for (bool matching:{false,true}) { for (unsigned mode=0;mode<3;++mode) {
    Fixture f;prepare(*f.transport,matching?parameter_columns:std::vector<Bytes>{parameter_columns[0]});
    const auto end=f.transport->input.size();const std::size_t close_start=23;
    if (mode==0) { f.transport->fail_send_offset=close_start+2; }
    if (mode==1) { f.transport->expire_after_output=close_start+9; }
    if (mode==2) { f.transport->lose_peer_after_output=close_start+9; }
    const auto d=rs::util::make_deadline(std::chrono::milliseconds(100));auto outcome=observe(f,d);
    retired(f,outcome,mode==0?DbErrorCode::NetworkError:mode==1?DbErrorCode::Timeout:DbErrorCode::TLSError,d);
    EXPECT_EQ(end,f.transport->offset);EXPECT_EQ(matching,outcome.progress.count_verified);
    EXPECT_EQ(mode==0?(matching?Stage::CountValidated:Stage::AllMetadataValidated):Stage::CloseSent,outcome.progress.stage);
    EXPECT_EQ(mode==0?close_start+2:close_start+9,f.transport->output.size());
  } }
}
TEST(MySqlShapePrepareObservationTest, ServerErrorsMalformedHeaderAndPartialPrepareAreNotComplete) {
  for (bool malformed:{false,true}) {
    Fixture f;Bytes err{std::byte{255},std::byte{40},std::byte{4},std::byte{'#'},std::byte{'4'},std::byte{'2'},std::byte{'S'},std::byte{'0'},std::byte{'2'}};
    if (malformed) { err[4]=std::byte{'a'}; }text(err,"unsafe-native-canary");append(*f.transport,err,1);
    const auto d=rs::util::make_deadline(std::chrono::seconds(10));auto outcome=observe(f,d);
    retired(f,outcome,malformed?DbErrorCode::ProtocolError:DbErrorCode::QueryFailed,d);
    EXPECT_EQ("MySQL session operation failed",outcome.result.error_message());EXPECT_EQ(Stage::PrepareAttempted,outcome.progress.stage);
    EXPECT_TRUE(outcome.progress.prepare_sent);EXPECT_FALSE(outcome.progress.parameter_count);
  }
  Fixture partial;partial.transport->fail_send_offset=2;const auto d=rs::util::make_deadline(std::chrono::seconds(10));
  auto outcome=observe(partial,d);retired(partial,outcome,DbErrorCode::NetworkError,d);
  EXPECT_EQ(Stage::PrepareAttempted,outcome.progress.stage);EXPECT_FALSE(outcome.progress.prepare_sent);EXPECT_EQ(2u,partial.transport->output.size());
  Fixture malformed;append(*malformed.transport,{std::byte{0}},1);auto header=observe(malformed,d);retired(malformed,header,DbErrorCode::ProtocolError,d);
  EXPECT_FALSE(header.progress.parameter_count);EXPECT_EQ(Stage::PrepareAttempted,header.progress.stage);
}
TEST(MySqlShapePrepareObservationTest, FinalTransactionStateIsTruthfulAndCountsDoNotImplySuccess) {
  for (bool matching:{false,true}) {
    Fixture f;prepare(*f.transport,matching?parameter_columns:std::vector<Bytes>{parameter_columns[0]},result_columns,17,3);
    const auto d=rs::util::make_deadline(std::chrono::seconds(10));auto outcome=observe(f,d);ASSERT_FALSE(outcome.result);
    EXPECT_EQ(Stage::ClosedVerified,outcome.progress.stage);EXPECT_EQ(matching,outcome.progress.count_verified);
    EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output));EXPECT_EQ(f.transport->input.size(),f.transport->offset);
    if (matching) { retired(f,outcome,DbErrorCode::ProtocolError,d); }
    else {
      EXPECT_EQ(DbErrorCode::InvalidParameter,outcome.result.error());EXPECT_EQ(SessionState::Transaction,outcome.result.session_snapshot().state);
      EXPECT_EQ(SessionDisposition::ResetRequired,outcome.result.session_snapshot().disposition);EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(0u,f.transport->closes);
    }
    deadlines(*f.transport,d);
  }
  for (bool timeout:{false,true}) {
    Fixture f;const auto header_end=f.transport->offset+16;prepare(*f.transport);
    if (timeout) { f.transport->expire_after_input=header_end; }
    else { f.transport->lose_peer_after_input=header_end; }
    const auto d=rs::util::make_deadline(std::chrono::milliseconds(100));auto outcome=observe(f,d);
    retired(f,outcome,timeout?DbErrorCode::Timeout:DbErrorCode::TLSError,d);
    EXPECT_EQ(Stage::PrepareHeaderValidated,outcome.progress.stage);EXPECT_EQ(header_end,f.transport->offset);
    EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
  }
}
