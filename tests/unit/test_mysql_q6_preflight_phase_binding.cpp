#include <gtest/gtest.h>
#include "core/database/mysql/mysql_session.h"
#include <thread>
#include "tests/integration/mysql_datetime_q6_preflight_deadlines.h"

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
  void set_ca_locations(const std::string&,const std::string&) override {
    ++configuration_calls;
    if (throw_ca) { throw std::bad_alloc(); }
    if (expire_ca) { std::this_thread::sleep_until(expected); }
    if (delay_ca) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    ca_finished=rs::util::Clock::now();
  }
  Bytes input=packet(greeting()), output;
  std::size_t configuration_calls{}, offset{}, chunk{1}, calls{}, closes{}, upgrades{};
  std::optional<std::size_t> throw_recv_offset,fail_send_offset,lose_peer_after_output;
  std::optional<std::size_t> expire_after_output,lose_peer_after_input,expire_after_input;
  std::size_t peer_after_input{}; std::optional<std::size_t> late_peer, lost_peer;
  bool throw_ca{},expire_ca{},delay_ca{}; rs::util::Deadline ca_finished{};
  int fail_at{-1}; bool verified{true}, throw_io{}, zero{}, oversize{}, eof{}; int bad_send{-1};
  rs::util::Deadline expected{};
  std::vector<rs::util::Deadline> deadlines;
  std::optional<rs::util::Deadline> bound;
  std::size_t deadline_violations{};
  void bind(rs::util::Deadline dl) { bound=dl; expected=dl; deadlines.clear(); }
  void record(rs::util::Deadline dl) {
    deadlines.push_back(dl);
    if (bound && dl!=*bound) { ++deadline_violations; }
  }
  rs::util::Result<void> connect(std::string_view, std::uint16_t, rs::util::Deadline) override {
    ADD_FAILURE() << "Must connect plaintext explicitly"; return {DbErrorCode::ProtocolError};
  }
  rs::util::Result<void> connect_plain(std::string_view host, std::uint16_t port, rs::util::Deadline dl) override {
    EXPECT_EQ("localhost",host); EXPECT_EQ(3306,port); record(dl);
    if (fail_at==0) return {DbErrorCode::NetworkError,"unsafe native detail"};
    return {};
  }
  rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte> out, rs::util::Deadline dl) override {
    record(dl); ++calls;
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
    record(dl);
    if (fail_send_offset && output.size()>=*fail_send_offset) return {DbErrorCode::NetworkError};
    if (fail_at==2) return {DbErrorCode::NetworkError,"unsafe native detail"};
    if (bad_send>=0 && !output.empty()) return rs::core::transport::IOResult{bad_send==1?in.size()+1:0,bad_send==2};
    const auto n=std::min(chunk,in.size()); output.insert(output.end(),in.begin(),in.begin()+n);
    if (lose_peer_after_output && output.size()>=*lose_peer_after_output) verified=false;
    if (expire_after_output && output.size()>=*expire_after_output) std::this_thread::sleep_until(dl);
    return rs::core::transport::IOResult{n,false};
  }
  rs::util::Result<void> upgrade_to_tls(std::string_view host, rs::util::Deadline dl) override {
    EXPECT_EQ("localhost",host); record(dl); ++upgrades;
    if (fail_at==3) return {DbErrorCode::TLSError,"unsafe native detail"};
    return {};
  }
  bool peer_identity_verified() noexcept override {
    if (offset==input.size()) {
      ++peer_after_input;
      if (late_peer && peer_after_input==*late_peer) { std::this_thread::sleep_until(expected); }
      if (lost_peer && peer_after_input==*lost_peer) { verified=false; }
    }
    return verified;
  }
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

using namespace rs::core::database;
struct Fixture {
  FakeTransport* transport{};
  MySqlSession session;
  Fixture():Fixture(std::make_unique<FakeTransport>()) {}
  explicit Fixture(std::unique_ptr<FakeTransport> owned):transport(owned.get()),session(std::move(owned)) {}
  rs::util::Deadline deadline() {
    transport->expected=rs::util::make_deadline(std::chrono::seconds(2));
    return transport->expected;
  }
  void authenticate(unsigned mode=0) {
    if (mode) { append(*transport,{std::byte{1},static_cast<std::byte>(mode==1?3:4)},3); }
    append(*transport,ok(),static_cast<std::uint8_t>(mode==0?3:mode==1?4:5));
  }
};
void same_deadline(const FakeTransport& t,rs::util::Deadline d) {
  ASSERT_FALSE(t.deadlines.empty());
  for (const auto actual:t.deadlines) { EXPECT_EQ(d,actual); }
}
void rejected(Fixture& f,const BackendResult<void>& result,DbErrorCode code,BackendOperation op,std::size_t closes) {
  ASSERT_FALSE(result); EXPECT_EQ(code,result.error()); EXPECT_EQ(op,result.backend_error().operation);
  EXPECT_EQ(SessionState::Disconnected,result.session_snapshot().state);
  EXPECT_EQ(SessionDisposition::Retire,result.session_snapshot().disposition);
  EXPECT_FALSE(f.session.is_connected()); EXPECT_TRUE(f.session.server_version().empty());
  EXPECT_EQ(closes,f.transport->closes); f.session.disconnect(); EXPECT_EQ(closes,f.transport->closes);
}
Bytes error(bool malformed=false) {
  Bytes b{std::byte{255},std::byte{21},std::byte{4},std::byte{'#'},std::byte{'2'},std::byte{'8'},std::byte{'0'},std::byte{'0'},std::byte{'0'}};
  if (malformed) { b[4]=std::byte{'a'}; }
  for (const char c:std::string_view("private server detail")) { b.push_back(static_cast<std::byte>(c)); }
  return b;
}
Bytes frame(const Bytes& output,std::size_t index) {
  std::size_t pos{};
  for (std::size_t i=0;i<=index;++i) {
    if (output.size()-pos<4) { return {}; }
    const auto n=std::to_integer<std::size_t>(output[pos])|(std::to_integer<std::size_t>(output[pos+1])<<8)|(std::to_integer<std::size_t>(output[pos+2])<<16);
    if (n+4>output.size()-pos) { return {}; }
    if (i==index) { return Bytes(output.begin()+pos,output.begin()+pos+n+4); }
    pos+=n+4;
  }
  return {};
}
using Phase=rs::tests::mysql::q6::PhaseDeadlines;
using PhasePolicy=rs::tests::mysql::q6::Policy;
using Fault=rs::tests::mysql::q6::Fault;
using ObservePolicy=MySqlSession::PrepareObservationPolicy;
using Stage=MySqlSession::PrepareObservationStage;
const std::vector<Bytes> parameters{
  column_packet("?",10,45,0,40),column_packet("?",12,45,0,104,6),column_packet("?",8,63,0,19)};
const std::vector<Bytes> results{
  column_packet("d",10,45,0,10),column_packet("t",12,45,0,26,6),
  column_packet("n",8,63,0,19),column_packet("i",3,63,0,11)};
void prepare(FakeTransport& t,const std::vector<Bytes>& params=parameters,
    const std::vector<Bytes>& columns=results,bool bad_eof=false) {
  Bytes first{std::byte{0}};number(first,17,4);number(first,static_cast<std::uint32_t>(columns.size()),2);
  number(first,static_cast<std::uint32_t>(params.size()),2);number(first,0,1);number(first,0,2);append(t,first,1);
  std::uint8_t seq=2;
  for (const auto& c:params) { append(t,c,seq++); }
  if (!params.empty()) { append(t,eof_packet(),seq++); }
  for (const auto& c:columns) { append(t,c,seq++); }
  if (!columns.empty()) { append(t,bad_eof?Bytes{std::byte{254}}:eof_packet(),seq); }
}
auto observe(Fixture& f,rs::util::Deadline d) {
  return f.session.observe_combined_prepare("SELECT preflight",d,
      ObservePolicy::DateDatetimeQ6Decimal65Q30ThreeByFour);
}
void command_frames(const FakeTransport& t,bool cleanup=false) {
  EXPECT_EQ((Bytes{std::byte{17},std::byte{0},std::byte{0},std::byte{0},std::byte{22},
    std::byte{'S'},std::byte{'E'},std::byte{'L'},std::byte{'E'},std::byte{'C'},std::byte{'T'},std::byte{' '},
    std::byte{'p'},std::byte{'r'},std::byte{'e'},std::byte{'f'},std::byte{'l'},std::byte{'i'},std::byte{'g'},std::byte{'h'},std::byte{'t'}}),frame(t.output,0));
  EXPECT_EQ((Bytes{std::byte{5},std::byte{0},std::byte{0},std::byte{0},std::byte{25},
    std::byte{17},std::byte{0},std::byte{0},std::byte{0}}),frame(t.output,1));
  if (cleanup) {
    EXPECT_EQ((Bytes{std::byte{5},std::byte{0},std::byte{0},std::byte{0},std::byte{3},
      std::byte{'D'},std::byte{'O'},std::byte{' '},std::byte{'0'}}),frame(t.output,2));
  }
  EXPECT_TRUE(frame(t.output,cleanup?3:2).empty());
}
void complete(const MySqlSession::PrepareObservationOutcome& o) {
  ASSERT_TRUE(o.result);const auto& e=*o.result;
  ASSERT_EQ(3u,e.parameters.size());ASSERT_EQ(4u,e.result_types.size());
  EXPECT_EQ(17u,e.statement_id);EXPECT_EQ(3u,e.parameter_count);EXPECT_EQ(4u,e.result_count);
  EXPECT_TRUE(e.parameter_eof);EXPECT_TRUE(e.result_eof);EXPECT_TRUE(e.count_verified);
  EXPECT_TRUE(e.close_sent);EXPECT_TRUE(e.verified_completion);
  EXPECT_EQ(42u,e.metadata_entries);EXPECT_EQ(10u,e.response_messages);
  EXPECT_EQ(Stage::ClosedVerified,o.progress.stage);
  const std::array<unsigned,3> types{10,12,8},charsets{45,45,63},widths{40,104,19};
  const std::array<std::int16_t,3> q{0,6,0};
  const std::array<ScalarType,3> normalized{ScalarType::Date,ScalarType::Timestamp,ScalarType::BigInt};
  const std::array<std::uint64_t,3> dimensions{10,26,19};
  for (std::size_t i=0;i<3;++i) {
    const auto& p=e.parameters[i];EXPECT_EQ(types[i],p.raw.type);EXPECT_EQ(charsets[i],p.raw.charset);
    EXPECT_EQ(widths[i],p.raw.width);EXPECT_EQ(q[i],p.raw.decimals);
    ASSERT_TRUE(p.raw.flags);EXPECT_EQ(0u,*p.raw.flags);EXPECT_TRUE(p.normalized.known);
    EXPECT_EQ(normalized[i],p.normalized.type);EXPECT_EQ(dimensions[i],p.normalized.column_size);
    EXPECT_EQ(q[i],p.normalized.decimal_digits);
  }
  const std::array<ScalarType,4> rt{ScalarType::Date,ScalarType::Timestamp,ScalarType::BigInt,ScalarType::Integer};
  const std::array<std::uint64_t,4> size{10,26,19,10};
  const std::array<std::int16_t,4> precision{0,6,0,0};
  for (std::size_t i=0;i<4;++i) {
    EXPECT_TRUE(e.result_types[i].known);EXPECT_EQ(rt[i],e.result_types[i].type);
    EXPECT_EQ(size[i],e.result_types[i].column_size);EXPECT_EQ(precision[i],e.result_types[i].decimal_digits);
  }
  EXPECT_EQ(SessionState::Idle,o.result.session_snapshot().state);
  EXPECT_EQ(SessionDisposition::Reusable,o.result.session_snapshot().disposition);
}
// Small real-clock windows shorten only the synthetic caller timeline. No OS timeout or TLS proof.
rs::util::Deadline short_origin() {
  return rs::util::Clock::now()-std::chrono::seconds(10)+std::chrono::milliseconds(20);
}
}

TEST(MySqlQ6PreflightPhaseBindingTest, PreboundStartupOverallAndCleanupOwnActualParserTransaction) {
  Fixture f;f.authenticate();const auto origin=rs::util::Clock::now();
  Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);
  ASSERT_TRUE(phases.begin_startup(origin));const auto v=phases.view();
  ASSERT_TRUE(v.startup);ASSERT_TRUE(v.overall);
  EXPECT_EQ(origin+std::chrono::seconds(10),*v.startup);
  EXPECT_EQ(origin+std::chrono::seconds(30),*v.overall);
  f.transport->bind(*v.startup);auto connected=f.session.connect_until(settings(),*v.startup);
  ASSERT_TRUE(connected);same_deadline(*f.transport,*v.startup);
  ASSERT_TRUE(phases.finish_startup(rs::util::Clock::now()));
  f.transport->output.clear();prepare(*f.transport);const auto response_begin=f.transport->offset;
  f.transport->bind(*v.overall);auto observation=observe(f,*v.overall);complete(observation);
  ASSERT_TRUE(observation.result);command_frames(*f.transport);same_deadline(*f.transport,*v.overall);
  EXPECT_EQ(f.transport->input.size()-response_begin,observation.result->response_wire_bytes);
  EXPECT_EQ(f.transport->input.size(),f.transport->offset);
  const auto first_cleanup=rs::util::Clock::now();ASSERT_TRUE(phases.begin_cleanup(first_cleanup));
  const auto c=phases.view().cleanup;ASSERT_TRUE(c);EXPECT_EQ(first_cleanup+std::chrono::seconds(5),*c);
  append(*f.transport,ok(),1);f.transport->bind(*c);
  ASSERT_TRUE(f.session.execute_query("DO 0",*c));same_deadline(*f.transport,*c);command_frames(*f.transport,true);
  ASSERT_TRUE(phases.finish_cleanup(rs::util::Clock::now()));
  EXPECT_EQ(0u,f.transport->deadline_violations);EXPECT_EQ(0u,f.transport->closes);
  f.session.disconnect();EXPECT_EQ(1u,f.transport->closes);f.transport->input.assign(1,std::byte{255});
  complete(observation);EXPECT_EQ(*c,phases.view().cleanup);
}
TEST(MySqlQ6PreflightPhaseBindingTest, FragmentedTlsCachedFullAndInitDbKeepStartupNotOverall) {
  for (unsigned mode:{0u,1u,2u}) {
    Fixture f;f.authenticate(mode);append(*f.transport,ok(),1);
    const auto origin=rs::util::Clock::now();Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);
    ASSERT_TRUE(phases.begin_startup(origin));const auto v=phases.view();ASSERT_TRUE(v.startup);ASSERT_TRUE(v.overall);
    auto s=settings();s.database="db";s.timeout=std::chrono::milliseconds(1);
    f.transport->bind(*v.startup);ASSERT_TRUE(f.session.connect_until(s,*v.startup));
    ASSERT_TRUE(phases.finish_startup(rs::util::Clock::now()));
    same_deadline(*f.transport,*v.startup);EXPECT_NE(*v.startup,*v.overall);
    EXPECT_EQ(0u,f.transport->deadline_violations);EXPECT_EQ(1u,f.transport->upgrades);
    EXPECT_EQ(f.transport->input.size(),f.transport->offset);
    EXPECT_EQ((Bytes{std::byte{3},std::byte{0},std::byte{0},std::byte{0},std::byte{2},std::byte{'d'},std::byte{'b'}}),frame(f.transport->output,mode==2?3:2));
    if (mode==2) {
      EXPECT_EQ((Bytes{std::byte{7},std::byte{0},std::byte{0},std::byte{4},std::byte{'s'},std::byte{'e'},std::byte{'c'},std::byte{'r'},std::byte{'e'},std::byte{'t'},std::byte{0}}),frame(f.transport->output,2));
    }
    f.session.disconnect();EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlQ6PreflightPhaseBindingTest, LateAuthenticationAndStartupErrKeepActualClassification) {
  for (bool startup:{false,true}) {
    for (bool malformed:{false,true}) {
      Fixture f;if (startup) { f.authenticate(); }
      append(*f.transport,error(malformed),static_cast<std::uint8_t>(startup?1:3));
      f.transport->expire_after_input=f.transport->input.size();const auto origin=short_origin();
      Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);ASSERT_TRUE(phases.begin_startup(origin));
      const auto p=phases.view().startup;ASSERT_TRUE(p);f.transport->bind(*p);
      auto s=settings();if (startup) { s.database="db"; }
      const auto r=f.session.connect_until(s,*p);
      rejected(f,r,malformed?DbErrorCode::ProtocolError:startup?DbErrorCode::QueryFailed:DbErrorCode::AuthenticationFailed,
          startup?BackendOperation::Startup:BackendOperation::Authenticate,1);
      EXPECT_EQ(std::string::npos,r.backend_error().message.find("private server detail"));
      // A failed backend result is not fed to finish_startup to replace its native error.
      EXPECT_FALSE(phases.view().startup_complete);EXPECT_EQ(Fault::None,phases.view().first_fault);
      same_deadline(*f.transport,*p);EXPECT_EQ(0u,f.transport->deadline_violations);
      ASSERT_TRUE(phases.begin_cleanup(rs::util::Clock::now()));
      const auto c=phases.view().cleanup;ASSERT_TRUE(c);const auto calls=f.transport->deadlines.size();
      f.session.disconnect();EXPECT_EQ(calls,f.transport->deadlines.size());EXPECT_EQ(1u,f.transport->closes);
      ASSERT_TRUE(phases.finish_cleanup(rs::util::Clock::now()));EXPECT_EQ(c,phases.view().cleanup);
    }
  }
}
TEST(MySqlQ6PreflightPhaseBindingTest, LateCaAndFinalSuccessCannotPublishIdleOrRenewStartup) {
  for (unsigned kind:{0u,1u,2u}) {
    Fixture f;f.authenticate();const auto origin=short_origin();
    Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);ASSERT_TRUE(phases.begin_startup(origin));
    const auto p=phases.view().startup;ASSERT_TRUE(p);f.transport->bind(*p);
    if (kind==0) { f.transport->expire_ca=true; }
    if (kind==1) { f.transport->expire_after_input=f.transport->input.size(); }
    if (kind==2) { f.transport->late_peer=2; }
    auto r=f.session.connect_until(settings(),*p);
    rejected(f,r,DbErrorCode::Timeout,kind==1?BackendOperation::Authenticate:BackendOperation::Connect,1);
    EXPECT_FALSE(phases.finish_startup(rs::util::Clock::now()));EXPECT_EQ(Fault::Expired,phases.view().first_fault);
    EXPECT_FALSE(phases.begin_startup(rs::util::Clock::now()));EXPECT_EQ(p,phases.view().startup);
    EXPECT_FALSE(phases.view().startup_complete);EXPECT_EQ(0u,f.transport->deadline_violations);
    if (kind==0) { EXPECT_TRUE(f.transport->deadlines.empty());EXPECT_TRUE(f.transport->output.empty()); }
    else { same_deadline(*f.transport,*p); }
    ASSERT_TRUE(phases.begin_cleanup(rs::util::Clock::now()));ASSERT_TRUE(phases.finish_cleanup(rs::util::Clock::now()));
    EXPECT_EQ(Fault::Expired,phases.view().first_fault);EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlQ6PreflightPhaseBindingTest, AlreadyExpiredPreboundStartupDoesNoCaIoOrClose) {
  Fixture f;f.authenticate();const auto origin=rs::util::Clock::now()-std::chrono::seconds(11);
  Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);ASSERT_TRUE(phases.begin_startup(origin));
  const auto v=phases.view();ASSERT_TRUE(v.startup);ASSERT_TRUE(v.overall);
  EXPECT_LT(*v.startup,rs::util::Clock::now());EXPECT_GT(*v.overall,rs::util::Clock::now());
  f.transport->bind(*v.startup);rejected(f,f.session.connect_until(settings(),*v.startup),DbErrorCode::Timeout,BackendOperation::Connect,0);
  EXPECT_EQ(0u,f.transport->configuration_calls);EXPECT_TRUE(f.transport->deadlines.empty());EXPECT_TRUE(f.transport->output.empty());
  EXPECT_FALSE(phases.finish_startup(rs::util::Clock::now()));EXPECT_EQ(Fault::Expired,phases.view().first_fault);
  ASSERT_TRUE(phases.begin_cleanup(rs::util::Clock::now()));ASSERT_TRUE(phases.finish_cleanup(rs::util::Clock::now()));
  EXPECT_EQ(Fault::Expired,phases.view().first_fault);EXPECT_EQ(0u,f.transport->closes);
}
TEST(MySqlQ6PreflightPhaseBindingTest, MetadataStructureEofAndCountFailuresKeepOverallDeadline) {
  for (unsigned kind:{0u,1u,2u,3u}) {
    Fixture f;f.authenticate();const auto origin=rs::util::Clock::now();
    Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);ASSERT_TRUE(phases.begin_startup(origin));
    const auto v=phases.view();ASSERT_TRUE(v.startup);ASSERT_TRUE(v.overall);f.transport->bind(*v.startup);
    ASSERT_TRUE(f.session.connect_until(settings(),*v.startup));ASSERT_TRUE(phases.finish_startup(rs::util::Clock::now()));
    f.transport->output.clear();f.transport->bind(*v.overall);
    if (kind==0) { append(*f.transport,error(),1); }
    else {
      auto p=parameters;if (kind==1) { p[1]=column_packet("?",12,45,0,103,6); }
      if (kind==3) { p.pop_back(); }
      prepare(*f.transport,p,results,kind==2);
    }
    auto o=observe(f,*v.overall);ASSERT_FALSE(o.result);
    EXPECT_EQ(kind==0?DbErrorCode::QueryFailed:kind==3?DbErrorCode::InvalidParameter:DbErrorCode::ProtocolError,o.result.error());
    EXPECT_EQ(BackendOperation::ExecutePrepared,o.result.backend_error().operation);
    same_deadline(*f.transport,*v.overall);EXPECT_EQ(0u,f.transport->deadline_violations);
    if (kind==3) {
      EXPECT_EQ(Stage::ClosedVerified,o.progress.stage);EXPECT_EQ(SessionState::Idle,o.result.session_snapshot().state);
      EXPECT_EQ(SessionDisposition::Reusable,o.result.session_snapshot().disposition);EXPECT_EQ(0u,f.transport->closes);
      command_frames(*f.transport);EXPECT_EQ(f.transport->input.size(),f.transport->offset);
      f.transport->output.clear();prepare(*f.transport);auto recovered=observe(f,*v.overall);complete(recovered);
      ASSERT_TRUE(recovered.result);command_frames(*f.transport);same_deadline(*f.transport,*v.overall);
    } else {
      EXPECT_EQ(SessionState::Disconnected,o.result.session_snapshot().state);EXPECT_EQ(SessionDisposition::Retire,o.result.session_snapshot().disposition);
      EXPECT_EQ(1u,f.transport->closes);EXPECT_TRUE(frame(f.transport->output,1).empty());
    }
    ASSERT_TRUE(phases.begin_cleanup(rs::util::Clock::now()));f.session.disconnect();
    ASSERT_TRUE(phases.finish_cleanup(rs::util::Clock::now()));EXPECT_EQ(1u,f.transport->closes);
  }
  // A complete final metadata reply or completed CLOSE send at D is not an owning success.
  for (bool late_close:{false,true}) {
    Fixture f;f.authenticate();
    const auto origin=rs::util::Clock::now()-std::chrono::seconds(30)+std::chrono::milliseconds(20);
    Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);
    ASSERT_TRUE(phases.begin_startup(rs::util::Clock::now()));
    const auto v=phases.view();ASSERT_TRUE(v.startup);ASSERT_TRUE(v.overall);EXPECT_EQ(v.startup,v.overall);
    f.transport->bind(*v.startup);ASSERT_TRUE(f.session.connect_until(settings(),*v.startup));
    ASSERT_TRUE(phases.finish_startup(rs::util::Clock::now()));f.transport->output.clear();prepare(*f.transport);
    f.transport->bind(*v.overall);
    if (late_close) { f.transport->expire_after_output=30; }
    else { f.transport->expire_after_input=f.transport->input.size(); }
    auto o=observe(f,*v.overall);ASSERT_FALSE(o.result);EXPECT_EQ(DbErrorCode::Timeout,o.result.error());
    EXPECT_EQ(SessionState::Disconnected,o.result.session_snapshot().state);
    EXPECT_EQ(SessionDisposition::Retire,o.result.session_snapshot().disposition);
    EXPECT_EQ(1u,f.transport->closes);same_deadline(*f.transport,*v.overall);EXPECT_EQ(0u,f.transport->deadline_violations);
    EXPECT_EQ(f.transport->input.size(),f.transport->offset);
    if (late_close) { command_frames(*f.transport); }
    else { EXPECT_TRUE(frame(f.transport->output,1).empty()); }
    ASSERT_TRUE(phases.begin_cleanup(rs::util::Clock::now()));f.session.disconnect();
    ASSERT_TRUE(phases.finish_cleanup(rs::util::Clock::now()));EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlQ6PreflightPhaseBindingTest, CleanupCommandUsesFrozenCutoffAndFailureDoesNotRenewIt) {
  Fixture f;f.authenticate();const auto origin=rs::util::Clock::now();
  Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);ASSERT_TRUE(phases.begin_startup(origin));
  const auto v=phases.view();ASSERT_TRUE(v.startup);ASSERT_TRUE(v.overall);f.transport->bind(*v.startup);
  ASSERT_TRUE(f.session.connect_until(settings(),*v.startup));ASSERT_TRUE(phases.finish_startup(rs::util::Clock::now()));
  f.transport->output.clear();prepare(*f.transport);f.transport->bind(*v.overall);auto o=observe(f,*v.overall);complete(o);ASSERT_TRUE(o.result);
  const auto first=rs::util::Clock::now();ASSERT_TRUE(phases.begin_cleanup(first));const auto c=phases.view().cleanup;ASSERT_TRUE(c);
  EXPECT_EQ(first+std::chrono::seconds(5),*c);append(*f.transport,error(),1);f.transport->bind(*c);
  const auto r=f.session.execute_query("DO 0",*c);ASSERT_FALSE(r);EXPECT_EQ(DbErrorCode::QueryFailed,r.error());
  EXPECT_EQ(BackendOperation::ExecuteDirect,r.backend_error().operation);same_deadline(*f.transport,*c);command_frames(*f.transport,true);
  EXPECT_EQ(0u,f.transport->deadline_violations);EXPECT_FALSE(phases.begin_cleanup(rs::util::Clock::now()));
  EXPECT_FALSE(phases.finish_cleanup(rs::util::Clock::now()));EXPECT_EQ(c,phases.view().cleanup);
  EXPECT_EQ(Fault::RepeatedCleanup,phases.view().first_fault);EXPECT_TRUE(phases.view().cleanup_uncertain);
  f.session.disconnect();EXPECT_EQ(1u,f.transport->closes);complete(o);
}
TEST(MySqlQ6PreflightPhaseBindingTest, PrebindingDetectsWrongPhaseAndLegacyRelativeDeadlineIsSeparate) {
  Fixture f;f.authenticate();const auto origin=rs::util::Clock::now();
  Phase phases(PhasePolicy::Overall30Startup10Cleanup5,origin);ASSERT_TRUE(phases.begin_startup(origin));
  const auto v=phases.view();ASSERT_TRUE(v.startup);ASSERT_TRUE(v.overall);
  f.transport->bind(*v.overall);ASSERT_TRUE(f.session.connect_until(settings(),*v.startup));
  EXPECT_GT(f.transport->deadline_violations,0u);same_deadline(*f.transport,*v.startup);
  EXPECT_EQ(*v.overall,*f.transport->bound); // The fake never learns its expectation from callbacks.
  f.session.disconnect();EXPECT_EQ(1u,f.transport->closes);
  Fixture ordinary;ordinary.authenticate();ordinary.transport->delay_ca=true;
  auto s=settings();s.timeout=std::chrono::seconds(1);ASSERT_TRUE(ordinary.session.connect(s));
  ASSERT_FALSE(ordinary.transport->deadlines.empty());const auto actual=ordinary.transport->deadlines.front();
  EXPECT_GE(actual,ordinary.transport->ca_finished+std::chrono::seconds(1));same_deadline(*ordinary.transport,actual);
  EXPECT_FALSE(ordinary.transport->bound);EXPECT_EQ(0u,ordinary.transport->deadline_violations);
  const auto calls=ordinary.transport->deadlines.size();auto again=ordinary.session.connect_until(s,*v.startup);
  ASSERT_FALSE(again);EXPECT_EQ(DbErrorCode::InvalidParameter,again.error());EXPECT_EQ(calls,ordinary.transport->deadlines.size());
  EXPECT_TRUE(ordinary.session.is_connected());ordinary.session.disconnect();EXPECT_EQ(1u,ordinary.transport->closes);
}
