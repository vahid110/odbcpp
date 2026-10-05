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
}
TEST(MySqlConnectUntilTest, ExpiredAndUnboundedInputsRefuseBeforeCaOrIo) {
  for (const auto d:{rs::util::Deadline::min(),rs::util::Clock::now()-std::chrono::seconds(1),rs::util::Deadline::max()}) {
    Fixture f; auto r=f.session.connect_until(settings(),d);
    rejected(f,r,d==rs::util::Deadline::max()?DbErrorCode::InvalidParameter:DbErrorCode::Timeout,BackendOperation::Connect,0);
    EXPECT_EQ(0u,f.transport->configuration_calls); EXPECT_TRUE(f.transport->deadlines.empty()); EXPECT_TRUE(f.transport->output.empty());
  }
  Fixture f; auto s=settings(); s.host.clear();
  rejected(f,f.session.connect_until(s,rs::util::Deadline::min()),DbErrorCode::InvalidParameter,BackendOperation::Connect,0);
  EXPECT_EQ(0u,f.transport->configuration_calls);
}
TEST(MySqlConnectUntilTest, MissingTlsConfigurationCapabilityPrecedesExpiredDeadline) {
  struct Unconfigured final : rs::core::transport::ITransport {
    std::size_t calls{},closes{};
    rs::util::Result<void> connect(std::string_view,std::uint16_t,rs::util::Deadline) override { ++calls;return {DbErrorCode::ProtocolError}; }
    rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte>,rs::util::Deadline) override { ++calls;return {DbErrorCode::ProtocolError}; }
    rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte>,rs::util::Deadline) override { ++calls;return {DbErrorCode::ProtocolError}; }
    void close() noexcept override { ++closes; }
  };
  auto t=std::make_unique<Unconfigured>();auto* raw=t.get();MySqlSession session(std::move(t));
  auto r=session.connect_until(settings(),rs::util::Deadline::min());
  ASSERT_FALSE(r);EXPECT_EQ(DbErrorCode::UnsupportedFeature,r.error());EXPECT_EQ(BackendOperation::Connect,r.backend_error().operation);
  EXPECT_EQ(0u,raw->calls);EXPECT_EQ(0u,raw->closes);session.disconnect();EXPECT_EQ(0u,raw->closes);
}
TEST(MySqlConnectUntilTest, FragmentedTlsCachedAndFullAuthenticationAndInitDbUseOriginalDeadline) {
  for (unsigned mode:{0u,1u,2u}) {
    Fixture f; f.authenticate(mode); append(*f.transport,ok(),1);
    auto s=settings(); s.database="db"; s.timeout=std::chrono::milliseconds(1); const auto d=f.deadline();
    auto r=f.session.connect_until(s,d); ASSERT_TRUE(r); EXPECT_TRUE(f.session.is_connected()); EXPECT_EQ("8.4.11",f.session.server_version());
    EXPECT_EQ(SessionState::Idle,r.session_snapshot().state); EXPECT_EQ(1u,f.transport->upgrades); EXPECT_EQ(0u,f.transport->closes);
    EXPECT_EQ(f.transport->input.size(),f.transport->offset); same_deadline(*f.transport,d);
    const auto ssl=frame(f.transport->output,0); ASSERT_EQ(36u,ssl.size());
    const Bytes prefix{std::byte{32},std::byte{0},std::byte{0},std::byte{1},std::byte{0},std::byte{138},std::byte{8},std::byte{0},std::byte{0},std::byte{0},std::byte{1},std::byte{0},std::byte{45}};
    EXPECT_TRUE(std::equal(prefix.begin(),prefix.end(),ssl.begin()));
    EXPECT_EQ((Bytes{std::byte{3},std::byte{0},std::byte{0},std::byte{0},std::byte{2},std::byte{'d'},std::byte{'b'}}),frame(f.transport->output,mode==2?3:2));
    if (mode==2) { EXPECT_EQ((Bytes{std::byte{7},std::byte{0},std::byte{0},std::byte{4},std::byte{'s'},std::byte{'e'},std::byte{'c'},std::byte{'r'},std::byte{'e'},std::byte{'t'},std::byte{0}}),frame(f.transport->output,2)); }
  }
}
TEST(MySqlConnectUntilTest, CaLateAndThrowOwnExactlyOneCloseBeforeAuthentication) {
  for (bool allocation:{false,true}) {
    Fixture f; f.transport->expected=rs::util::make_deadline(std::chrono::milliseconds(5));
    f.transport->expire_ca=!allocation; f.transport->throw_ca=allocation;
    rejected(f,f.session.connect_until(settings(),f.transport->expected),allocation?DbErrorCode::AllocationFailure:DbErrorCode::Timeout,BackendOperation::Connect,1);
    EXPECT_EQ(1u,f.transport->configuration_calls); EXPECT_TRUE(f.transport->deadlines.empty()); EXPECT_TRUE(f.transport->output.empty());
  }
}
TEST(MySqlConnectUntilTest, FragmentExpiryAndLateSuccessNeverPublishIdle) {
  for (bool complete:{false,true}) {
    Fixture f; f.authenticate(); f.transport->expire_after_input=complete?f.transport->input.size():5;
    const auto d=rs::util::make_deadline(std::chrono::milliseconds(5));
    rejected(f,f.session.connect_until(settings(),d),DbErrorCode::Timeout,BackendOperation::Authenticate,1); same_deadline(*f.transport,d);
    EXPECT_EQ(complete?f.transport->input.size():5u,f.transport->offset);
  }
}
TEST(MySqlConnectUntilTest, LateAuthenticationErrorsKeepNativeOrStructuralClassification) {
  for (unsigned mode:{0u,1u,2u}) {
    for (bool malformed:{false,true}) {
      Fixture f;
      if (mode) { append(*f.transport,{std::byte{1},static_cast<std::byte>(mode==1?3:4)},3); }
      append(*f.transport,error(malformed),static_cast<std::uint8_t>(mode==0?3:mode==1?4:5));
      f.transport->expire_after_input=f.transport->input.size(); const auto d=rs::util::make_deadline(std::chrono::milliseconds(5));
      auto r=f.session.connect_until(settings(),d);
      rejected(f,r,malformed?DbErrorCode::ProtocolError:DbErrorCode::AuthenticationFailed,BackendOperation::Authenticate,1); same_deadline(*f.transport,d);
      EXPECT_EQ(f.transport->input.size(),f.transport->offset);
      EXPECT_EQ(std::string::npos,r.backend_error().message.find("private server detail"));
    }
  }
}
TEST(MySqlConnectUntilTest, StartupLateErrorsAndThrowRetainOperationAndOneClose) {
  for (unsigned kind:{0u,1u,2u}) {
    Fixture f; f.authenticate(); const auto auth_end=f.transport->input.size(); append(*f.transport,error(kind==1),1);
    auto s=settings(); s.database="db"; const auto d=rs::util::make_deadline(std::chrono::milliseconds(5));
    if (kind==2) { f.transport->throw_recv_offset=auth_end; } else { f.transport->expire_after_input=f.transport->input.size(); }
    rejected(f,f.session.connect_until(s,d),kind==0?DbErrorCode::QueryFailed:DbErrorCode::ProtocolError,kind==2?BackendOperation::Connect:BackendOperation::Startup,1); same_deadline(*f.transport,d);
  }
}
TEST(MySqlConnectUntilTest, FinalPeerAndClockGatePrecedesOwningVersionPublication) {
  for (bool late:{false,true}) {
    Fixture f; f.authenticate(); f.transport->expected=rs::util::make_deadline(std::chrono::milliseconds(5));
    if (late) { f.transport->late_peer=2; } else { f.transport->lost_peer=2; }
    rejected(f,f.session.connect_until(settings(),f.transport->expected),late?DbErrorCode::Timeout:DbErrorCode::TLSError,BackendOperation::Connect,1);
    EXPECT_EQ(2u,f.transport->peer_after_input); same_deadline(*f.transport,f.transport->expected);
  }
}
TEST(MySqlConnectUntilTest, LegacyDeadlineStillStartsAfterCaAndAlreadyConnectedRefusesLocally) {
  Fixture f; f.authenticate(); f.transport->delay_ca=true; auto s=settings(); s.timeout=std::chrono::seconds(1);
  ASSERT_TRUE(f.session.connect(s)); ASSERT_FALSE(f.transport->deadlines.empty());
  const auto d=f.transport->deadlines.front(); EXPECT_GE(d,f.transport->ca_finished+s.timeout); same_deadline(*f.transport,d);
  const auto calls=f.transport->deadlines.size(); auto r=f.session.connect_until(s,rs::util::Deadline::min());
  ASSERT_FALSE(r); EXPECT_EQ(DbErrorCode::InvalidParameter,r.error()); EXPECT_TRUE(f.session.is_connected());
  EXPECT_EQ(SessionState::Idle,r.session_snapshot().state); EXPECT_EQ(calls,f.transport->deadlines.size()); EXPECT_EQ(1u,f.transport->configuration_calls); EXPECT_EQ(0u,f.transport->closes);
}
TEST(MySqlConnectUntilTest, OriginalDeadlineOwnsPrepareBothMetadataStagesAndCloseWithoutExecute) {
  Fixture f; f.authenticate(); const auto d=f.deadline(); ASSERT_TRUE(f.session.connect_until(settings(),d)); f.transport->output.clear();
  Bytes first{std::byte{0}};number(first,17,4);number(first,4,2);number(first,3,2);number(first,0,1);number(first,0,2);append(*f.transport,first,1);
  append(*f.transport,column_packet("?",10,45,0,40),2);append(*f.transport,column_packet("?",12,45,0,104,6),3);
  append(*f.transport,column_packet("?",246,63,128,67,30),4);append(*f.transport,eof_packet(),5);
  append(*f.transport,column_packet("d",10,45,0,10),6);append(*f.transport,column_packet("t",12,45,0,26,6),7);
  append(*f.transport,column_packet("n",246,63,0,7,2),8);append(*f.transport,column_packet("i",8,63,0,19),9);append(*f.transport,eof_packet(),10);
  auto r=f.session.observe_combined_prepare("SELECT owning",d,MySqlSession::PrepareObservationPolicy::DateDatetimeQ6Decimal65Q30ThreeByFour);
  ASSERT_TRUE(r.result); ASSERT_EQ(3u,r.result->parameters.size()); ASSERT_EQ(4u,r.result->result_types.size());
  EXPECT_TRUE(r.result->parameter_eof); EXPECT_TRUE(r.result->result_eof); EXPECT_TRUE(r.result->close_sent); EXPECT_TRUE(r.result->verified_completion);
  EXPECT_EQ(42u,r.result->metadata_entries); EXPECT_EQ(10u,r.result->response_messages); EXPECT_EQ(SessionState::Idle,r.result.session_snapshot().state);
  ASSERT_TRUE(r.result->parameters[2].raw.flags); EXPECT_EQ(128u,*r.result->parameters[2].raw.flags); EXPECT_EQ(65u,r.result->parameters[2].normalized.column_size);
  EXPECT_EQ(26u,r.result->parameters[1].normalized.column_size); EXPECT_EQ(ScalarType::Date,r.result->result_types[0].type);
  const auto prep=frame(f.transport->output,0); ASSERT_GE(prep.size(),5u); EXPECT_EQ(std::byte{22},prep[4]);
  EXPECT_EQ((Bytes{std::byte{5},std::byte{0},std::byte{0},std::byte{0},std::byte{25},std::byte{17},std::byte{0},std::byte{0},std::byte{0}}),frame(f.transport->output,1)); EXPECT_TRUE(frame(f.transport->output,2).empty()); same_deadline(*f.transport,d);
  f.session.disconnect(); EXPECT_EQ(1u,f.transport->closes); f.transport->input.clear();
  EXPECT_EQ(104u,r.result->parameters[1].raw.width); EXPECT_EQ(ScalarType::Decimal,r.result->result_types[2].type);
}

TEST(MySqlConnectUntilTest, AuthenticationTransportFailuresRetainCleanupAndOperation) {
  for (int kind:{0,1,2}) {
    Fixture f; f.authenticate(); const auto d=f.deadline();
    if (kind==2) { f.transport->throw_recv_offset=0; } else { f.transport->fail_at=kind==0?0:3; }
    rejected(f,f.session.connect_until(settings(),d),kind==0?DbErrorCode::NetworkError:kind==1?DbErrorCode::TLSError:DbErrorCode::ProtocolError,kind==2?BackendOperation::Connect:BackendOperation::Authenticate,1);
    same_deadline(*f.transport,d);
  }
}
