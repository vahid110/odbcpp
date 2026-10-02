#include <gtest/gtest.h>
#include "core/database/mysql/mysql_session.h"
#include "core/database/session_owner.h"

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
Bytes column_packet(std::string_view name,unsigned type=8,unsigned charset=63,unsigned flags=0) {
  Bytes bytes;for (const auto field:{std::string_view("def"),std::string_view{},std::string_view{},std::string_view{},name,std::string_view{}}) len_text(bytes,field);
  number(bytes,12,1);number(bytes,charset,2);number(bytes,type==8?20:40,4);number(bytes,type,1);
  number(bytes,flags,2);number(bytes,0,1);number(bytes,0,2);return bytes;
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
TEST(MySqlSessionTest, OwningResultsNullEmptyBinarySafeTextAndRepeatedSequenceReset) {
  Fixture f;f.rows();auto result=f.execute();ASSERT_TRUE(result);
  ASSERT_EQ(2u,result->rows.size());EXPECT_EQ("42",*result->rows[0][0]);
  EXPECT_EQ(std::string("a\0b",3),*result->rows[0][1]);EXPECT_FALSE(result->rows[1][0]);
  ASSERT_TRUE(result->rows[1][1]);EXPECT_TRUE(result->rows[1][1]->empty());
  EXPECT_EQ(rs::core::database::ScalarType::BigInt,result->columns[0].normalized_type->type);
  EXPECT_EQ(10u,result->columns[1].normalized_type->column_size);
  EXPECT_EQ(rs::core::database::SessionState::Idle,result.session_snapshot().state);
  EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
  EXPECT_EQ(std::byte{0},f.transport->output[3]);EXPECT_EQ(std::byte{3},f.transport->output[4]);
  for (const auto dl:f.transport->deadlines) EXPECT_EQ(f.transport->expected,dl);
  append(*f.transport,ok(),1);EXPECT_TRUE(f.execute("SET @x=1"));
  f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);EXPECT_EQ("42",*result->rows[0][0]);
  EXPECT_TRUE(f.session->server_version().empty());
}
TEST(MySqlSessionTest, InvalidCellsAreDeferredWithoutDiscardingOwningRows) {
  Fixture f;f.rows(true);auto result=f.execute();ASSERT_TRUE(result);
  ASSERT_EQ(1u,result->cell_errors.size());EXPECT_EQ(0u,result->cell_errors[0].row);
  EXPECT_TRUE(result->rows[0][0]->empty());EXPECT_TRUE(f.session->is_connected());
}
TEST(MySqlSessionTest, LocalFailuresPreserveConnectedSessionWithoutIo) {
  Fixture f;const auto calls=f.transport->calls;
  EXPECT_FALSE(f.execute(""));EXPECT_FALSE(f.execute(std::string_view("x\0y",3)));
  EXPECT_FALSE(f.execute(std::string(1,static_cast<char>(255))));
  const std::array unsupported{rs::core::database::QueryParameter{"1",rs::core::database::QueryParameterType::Numeric}};
  EXPECT_FALSE(f.session->execute_prepared("SELECT ?",unsupported,f.transport->expected));
  EXPECT_FALSE(f.session->connect(settings()));EXPECT_EQ(calls,f.transport->calls);
  EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(0u,f.transport->closes);
  EXPECT_TRUE(f.session->is_connected());
}
TEST(MySqlSessionTest, WireSequenceTruncationWarningsAndMoreResultsRetire) {
  for (unsigned mode=0;mode<6;++mode) {
    Fixture f;
    if (mode==0) append(*f.transport,ok(),2);
    if (mode==1) append(*f.transport,{std::byte{0}},1);
    if (mode==2) { auto response=ok();response[5]=std::byte{1};append(*f.transport,response,1); }
    if (mode==3) { auto response=ok();response[3]=std::byte{8};append(*f.transport,response,1); }
    if (mode==4) append(*f.transport,{std::byte{255},std::byte{1},std::byte{0},std::byte{'#'},std::byte{'4'},std::byte{'2'},std::byte{'0'},std::byte{'0'},std::byte{'0'},std::byte{'s'}},1);
    if (mode==5) f.transport->throw_io=true;
    auto result=f.execute();ASSERT_FALSE(result);EXPECT_FALSE(f.session->is_connected());
    EXPECT_EQ(1u,f.transport->closes);EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
    EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
    auto again=f.execute();EXPECT_FALSE(again);EXPECT_EQ(DbErrorCode::NotConnected,again.error());
  }
}
TEST(MySqlSessionTest, ResponseAndResultBudgetsApplyBeforeBodyOrRowAdmission) {
  for (unsigned mode=0;mode<5;++mode) {
    auto config=settings();
    if (mode==0) config.response_limits.max_wire_bytes=5;
    if (mode==1) config.response_limits.max_messages=1;
    if (mode==2) config.result_limits.max_rows=0;
    if (mode==3) config.result_limits.max_cells=1;
    if (mode==4) config.result_limits.max_metadata_name_bytes=3;
    Fixture f(config);f.rows();auto result=f.execute();ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::ResourceLimit,result.error());EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlSessionTest, UnsupportedColumnsMalformedRowsAndMetadataRetire) {
  for (unsigned mode=0;mode<4;++mode) {
    Fixture f;append(*f.transport,{std::byte{1}},1);
    auto column=column_packet("id",mode==0?12:8);
    if (mode==1) column.pop_back();
    append(*f.transport,column,2);append(*f.transport,eof_packet(),3);
    append(*f.transport,mode==2?Bytes{std::byte{4},std::byte{'1'}}:Bytes{std::byte{1},std::byte{'1'},std::byte{0}},4);
    auto result=f.execute();EXPECT_FALSE(result);EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlSessionTest, DeadlineAndPeerLossCannotAdmitSuccessOrSendQuery) {
  Fixture expired;auto result=expired.session->execute_query("SELECT 1",rs::util::Clock::now());
  ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::Timeout,result.error());EXPECT_TRUE(expired.transport->output.empty());
  Fixture lost;lost.transport->verified=false;result=lost.execute();ASSERT_FALSE(result);
  EXPECT_EQ(DbErrorCode::TLSError,result.error());EXPECT_TRUE(lost.transport->output.empty());
}
TEST(MySqlSessionTest, ConnectRejectsUnsupportedSecurityInvalidDatabaseAndTightStartupBudgets) {
  for (unsigned mode=0;mode<3;++mode) {
    auto t=std::make_unique<FakeTransport>();auto* observed=t.get();MySqlSession session(std::move(t));auto config=settings();
    if (mode==0) config.use_ssl=false;
    if (mode==1) config.database=std::string(1,static_cast<char>(255));
    if (mode==2) config.startup_response_limits.max_messages=1;
    EXPECT_FALSE(session.connect(config));EXPECT_TRUE(observed->output.empty());EXPECT_EQ(0u,observed->calls);
  }
}
TEST(MySqlSessionTest, LengthEncodedBoundariesRejectNullReservedAndTruncation) {
  for (const auto& bytes:{Bytes{std::byte{251}},Bytes{std::byte{255}},Bytes{std::byte{252},std::byte{1}},Bytes{std::byte{254},std::byte{0}}}) {
    query_detail::Cursor c(bytes);std::uint64_t n{};EXPECT_FALSE(c.length(n));
  }
  Bytes bytes{std::byte{252},std::byte{255},std::byte{255}};query_detail::Cursor c(bytes);std::uint64_t n{};
  ASSERT_TRUE(c.length(n));EXPECT_EQ(65535u,n);EXPECT_EQ(0u,c.remaining());
}
TEST(MySqlSessionTest, TrustPathsAreValidatedBeforeProviderConfiguration) {
  for (unsigned mode=0;mode<4;++mode) {
    auto t=std::make_unique<FakeTransport>();auto* observed=t.get();MySqlSession session(std::move(t));auto config=settings();
    if (mode==0) config.ssl_ca_file=std::string(config.input_limits.max_connection_field_bytes+1,'x');
    if (mode==1) config.ssl_ca_dir=std::string(config.input_limits.max_connection_field_bytes+1,'x');
    if (mode==2) config.ssl_ca_file=std::string("a\0b",3);
    if (mode==3) config.ssl_ca_dir=std::string("a\0b",3);
    auto result=session.connect(config);ASSERT_FALSE(result);EXPECT_EQ(mode<2?DbErrorCode::ResourceLimit:DbErrorCode::InvalidParameter,result.error());
    EXPECT_EQ(0u,observed->configuration_calls);EXPECT_EQ(0u,observed->calls);EXPECT_TRUE(observed->output.empty());
    EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
    EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
  }
}
TEST(MySqlSessionTest, LocalInputLimitRetainsUsableStateAndAllowsNextQuery) {
  auto config=settings();config.input_limits.max_sql_bytes=8;Fixture f(config);
  auto rejected=f.execute("SELECT too_long");ASSERT_FALSE(rejected);EXPECT_EQ(DbErrorCode::ResourceLimit,rejected.error());
  EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,rejected.session_snapshot().disposition);
  EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(0u,f.transport->closes);
  append(*f.transport,ok(),1);EXPECT_TRUE(f.execute("SET @x=1"));
}
TEST(MySqlSessionTest, UnsupportedProtocolStatusesRejectOkAndRowEof) {
  for (const unsigned status:{0x40u,0x80u,0x1000u,0x4000u}) {
    for (const bool row_result:{false,true}) {
      Fixture f;
      if (row_result) {
        append(*f.transport,{std::byte{1}},1);append(*f.transport,column_packet("id"),2);
        append(*f.transport,eof_packet(),3);append(*f.transport,eof_packet(status),4);
      } else { auto response=ok();response[3]=static_cast<std::byte>(status&255);response[4]=static_cast<std::byte>(status>>8);append(*f.transport,response,1); }
      auto result=f.execute();ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());EXPECT_EQ(1u,f.transport->closes);
    }
  }
}
TEST(MySqlSessionTest, PacketSequenceWrapAndTransactionSnapshotArePreserved) {
  Fixture f;append(*f.transport,{std::byte{1}},1);append(*f.transport,column_packet("id"),2);append(*f.transport,eof_packet(),3);
  std::uint8_t seq=4;
  for (unsigned i=0;i<260;++i) append(*f.transport,{std::byte{1},std::byte{'1'}},seq++);
  append(*f.transport,eof_packet(1),seq);auto result=f.execute();ASSERT_TRUE(result);EXPECT_EQ(260u,result->rows.size());
  EXPECT_EQ(rs::core::database::SessionState::Transaction,result.session_snapshot().state);
  EXPECT_EQ(rs::core::database::SessionDisposition::ResetRequired,result.session_snapshot().disposition);
}
TEST(MySqlSessionTest, AuthenticatedConnectCanBeAdoptedBySdkOwner) {
  auto transport=std::make_unique<FakeTransport>();append(*transport,ok(),3);
  auto session=std::make_unique<MySqlSession>(std::move(transport));
  auto owner=rs::core::database::SessionOwner::connect_authenticated(std::move(session),settings(),
      rs::core::database::SessionReusePolicy{rs::util::make_deadline(std::chrono::hours(1)),std::chrono::minutes(5)});
  EXPECT_TRUE(owner);
}
TEST(MySqlSessionTest, PartialWritesInvalidProgressAndRecvFailuresRetireOnce) {
  for (unsigned mode=0;mode<7;++mode) {
    Fixture f;f.rows();
    if (mode<3) f.transport->bad_send=static_cast<int>(mode);
    if (mode==3) f.transport->fail_at=1;
    if (mode==4) f.transport->zero=true;
    if (mode==5) f.transport->oversize=true;
    if (mode==6) f.transport->eof=true;
    auto result=f.execute();EXPECT_FALSE(result);EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlSessionTest, DiagnosticAndPacketBudgetsBoundServerFailure) {
  auto config=settings();config.result_limits.max_diagnostic_bytes=8;Fixture f(config);
  append(*f.transport,{std::byte{255},std::byte{1},std::byte{0},std::byte{'#'},std::byte{'4'},std::byte{'2'},std::byte{'0'},std::byte{'0'},std::byte{'0'}},1);
  auto result=f.execute();ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ResourceLimit,result.error());
  Fixture packet_limit;packet_limit.transport->input.insert(packet_limit.transport->input.end(),
      {std::byte{1},std::byte{0},std::byte{1},std::byte{1}});
  result=packet_limit.execute();ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ResourceLimit,result.error());
  EXPECT_EQ(1u,packet_limit.transport->closes);
}
TEST(MySqlSessionTest, OversizedConnectionFieldsReportResourceLimitsWithoutIo) {
  for (unsigned mode=0;mode<6;++mode) {
    auto t=std::make_unique<FakeTransport>();auto* observed=t.get();MySqlSession session(std::move(t));auto config=settings();
    const std::string large(config.input_limits.max_connection_field_bytes+1,'x');
    if (mode==0) config.host=large;
    if (mode==1) config.user=large;
    if (mode==2) config.password=large;
    if (mode==3) config.database=large;
    if (mode==4) config.ssl_ca_file=large;
    if (mode==5) config.ssl_ca_dir=large;
    auto result=session.connect(config);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ResourceLimit,result.error());
    EXPECT_EQ(0u,observed->configuration_calls);EXPECT_EQ(0u,observed->calls);EXPECT_TRUE(observed->output.empty());
  }
}
} // namespace

TEST(MySqlSessionTest, NativeDatabaseSelectionUsesOriginalDeadlineAndLiteralUtf8Name) {
  auto owned=std::make_unique<FakeTransport>();auto* t=owned.get();
  append(*t,ok(),3);append(*t,ok(),1);
  MySqlSession session(std::move(owned));auto config=settings();config.database="db`'; SELECT secret;--é";
  auto result=session.connect(config);ASSERT_TRUE(result);EXPECT_TRUE(session.is_connected());
  ASSERT_GT(t->output.size(),config.database.size()+5);
  const auto start=t->output.size()-config.database.size()-5;
  EXPECT_EQ(std::byte{0},t->output[start+3]);EXPECT_EQ(std::byte{2},t->output[start+4]);
  const std::string actual(reinterpret_cast<const char*>(t->output.data()+start+5),config.database.size());
  EXPECT_EQ(config.database,actual);
  for (const auto dl:t->deadlines) EXPECT_EQ(t->deadlines.front(),dl);
  append(*t,ok(),1);EXPECT_TRUE(session.execute_query("SELECT 1",rs::util::make_deadline(std::chrono::seconds(10))));
  EXPECT_EQ(0u,t->closes);
}
TEST(MySqlSessionTest, DatabaseSelectionRejectionNeverPublishesSessionAndReconnectsCleanly) {
  for (unsigned mode=0;mode<8;++mode) {
    auto owned=std::make_unique<FakeTransport>();auto* t=owned.get();append(*t,ok(),3);
    if (mode==0) append(*t,{std::byte{255},std::byte{1},std::byte{0},std::byte{'#'},std::byte{'4'},std::byte{'2'},std::byte{'0'},std::byte{'0'},std::byte{'0'}},1);
    if (mode==1) append(*t,ok(),2);
    if (mode==2) append(*t,{std::byte{0}},1);
    if (mode>=3 && mode<=5) { auto bytes=ok();bytes[mode==3?3:mode==4?5:1]=std::byte{1};append(*t,bytes,1); }
    if (mode==7) { auto bytes=ok();bytes[2]=std::byte{1};append(*t,bytes,1); }
    // mode 6: EOF after authentication, without a selection response.
    MySqlSession session(std::move(owned));auto config=settings();config.database="odbcpp";
    auto result=session.connect(config);ASSERT_FALSE(result);EXPECT_FALSE(session.is_connected());
    EXPECT_TRUE(session.server_version().empty());EXPECT_EQ(1u,t->closes);
    EXPECT_EQ(rs::core::database::BackendOperation::Startup,result.backend_error().operation);
    EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
    EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
    t->input=packet(greeting());t->offset=0;append(*t,ok(),3);append(*t,ok(),1);
    EXPECT_TRUE(session.connect(config));EXPECT_EQ(1u,t->closes);
  }
}
TEST(MySqlSessionTest, DatabaseStartupBudgetsAreCheckedBeforeCredentialsAndBodyAdmission) {
  constexpr std::size_t auth_bound=5*(connection_packet_limit+4);
  for (unsigned mode=0;mode<5;++mode) {
    auto owned=std::make_unique<FakeTransport>();auto* t=owned.get();MySqlSession session(std::move(owned));
    auto config=settings();config.database="odbcpp";
    if (mode==0) config.input_limits.max_request_wire_bytes=10;
    if (mode==1) config.input_limits.max_startup_wire_bytes=auth_bound+10;
    if (mode==2) config.startup_response_limits.max_wire_bytes=auth_bound+10;
    if (mode==3) config.startup_response_limits.max_messages=5;
    if (mode==4) config.database=std::string(connection_packet_limit,'a');
    auto result=session.connect(config);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ResourceLimit,result.error());
    EXPECT_TRUE(t->output.empty());EXPECT_EQ(0u,t->calls);EXPECT_EQ(0u,t->configuration_calls);
  }
  auto owned=std::make_unique<FakeTransport>();auto* t=owned.get();append(*t,ok(),3);
  auto oversized=ok();oversized.resize(20);append(*t,oversized,1);
  MySqlSession session(std::move(owned));auto config=settings();config.database="odbcpp";
  config.startup_response_limits.max_wire_bytes=auth_bound+11;
  auto result=session.connect(config);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ResourceLimit,result.error());
  EXPECT_EQ(t->input.size()-20,t->offset);EXPECT_EQ(1u,t->closes);
}

TEST(MySqlSessionTest, DatabaseResponseTruncationAndPostAuthenticationExceptionCloseExactlyOnce) {
  for (std::size_t size=0;size<ok().size()+4;++size) {
    auto owned=std::make_unique<FakeTransport>();auto* t=owned.get();append(*t,ok(),3);
    const auto start=t->input.size();const auto response=packet(ok(),1);
    t->input.insert(t->input.end(),response.begin(),response.begin()+size);
    MySqlSession session(std::move(owned));auto config=settings();config.database="odbcpp";
    auto result=session.connect(config);ASSERT_FALSE(result);EXPECT_EQ(1u,t->closes);
    EXPECT_FALSE(session.is_connected());EXPECT_TRUE(session.server_version().empty());EXPECT_GE(t->offset,start);
  }
  auto owned=std::make_unique<FakeTransport>();auto* t=owned.get();append(*t,ok(),3);
  t->throw_recv_offset=t->input.size();append(*t,ok(),1);
  MySqlSession session(std::move(owned));auto config=settings();config.database="odbcpp";
  auto result=session.connect(config);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
  EXPECT_FALSE(session.is_connected());EXPECT_EQ(1u,t->closes);session.disconnect();EXPECT_EQ(1u,t->closes);
}

namespace {
void prepared_metadata(FakeTransport& t,unsigned parameters=1,unsigned columns=1) {
  Bytes first{std::byte{0}};number(first,17,4);number(first,columns,2);number(first,parameters,2);
  number(first,0,1);number(first,0,2);append(t,first,1);unsigned seq=2;
  for (unsigned i=0;i<parameters;++i) append(t,column_packet("?",253,45),seq++);
  if (parameters) append(t,eof_packet(),seq++);
  for (unsigned i=0;i<columns;++i) append(t,column_packet("id"),seq++);
  if (columns) append(t,eof_packet(),seq++);
}
void binary_result(FakeTransport& t,bool null=false) {
  append(t,{std::byte{1}},1);append(t,column_packet("id"),2);append(t,eof_packet(),3);
  Bytes row{std::byte{0},static_cast<std::byte>(null?4:0)};
  if (!null) { number(row,42,4);number(row,0,4); }
  append(t,row,4);append(t,eof_packet(),5);
}
std::vector<unsigned> commands(const Bytes& bytes) {
  std::vector<unsigned> result;std::size_t offset{};
  while (offset<bytes.size()) {
    EXPECT_GE(bytes.size()-offset,5u);if (bytes.size()-offset<5) break;
    const auto size=std::to_integer<std::size_t>(bytes[offset])|
        (std::to_integer<std::size_t>(bytes[offset+1])<<8)|(std::to_integer<std::size_t>(bytes[offset+2])<<16);
    EXPECT_EQ(std::byte{0},bytes[offset+3]);result.push_back(std::to_integer<unsigned>(bytes[offset+4]));
    offset+=size+4;
  }
  EXPECT_EQ(bytes.size(),offset);return result;
}
}
TEST(MySqlSessionTest, PreparedExecuteBindsTypedParametersClosesAndPreservesOwnedRows) {
  for (bool null:{false,true}) {
    Fixture f;prepared_metadata(*f.transport);binary_result(*f.transport,null);
    const std::array params{rs::core::database::QueryParameter{null?std::nullopt:std::optional<std::string>{"42"},
        rs::core::database::QueryParameterType::Int32}};
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT CAST(? AS SIGNED) AS id",params,deadline);ASSERT_TRUE(result);
    ASSERT_EQ(1u,result->rows.size());EXPECT_EQ(params[0].value,result->rows[0][0]);
    ASSERT_EQ(1u,result->normalized_parameter_types.size());
    EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));
    EXPECT_EQ(std::byte{17},f.transport->output[f.transport->output.size()-4]);
    for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
    append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());f.session->disconnect();
    EXPECT_EQ(params[0].value,result->rows[0][0]);EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlSessionTest, PreparedParameterMismatchClosesStatementAndPreservesOwner) {
  Fixture f;prepared_metadata(*f.transport);
  auto result=f.session->execute_prepared("SELECT ?",{},rs::util::make_deadline(std::chrono::seconds(10)));
  ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());
  EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output));EXPECT_TRUE(f.session->is_connected());
  EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
  append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());
}
TEST(MySqlSessionTest, PreparedLocalValidationDoesNotSendOrRetire) {
  Fixture f;const std::array params{rs::core::database::QueryParameter{"2147483648",rs::core::database::QueryParameterType::Int32}};
  auto result=f.session->execute_prepared("SELECT ?",params,rs::util::make_deadline(std::chrono::seconds(10)));
  ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());
  EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(0u,f.transport->closes);EXPECT_TRUE(f.session->is_connected());
}
TEST(MySqlSessionTest, PreparedMalformedMetadataBinaryRowsAndAggregateBudgetsRetire) {
  for (unsigned mode=0;mode<5;++mode) {
    auto config=settings();if (mode==3) config.response_limits.max_messages=9;
    if (mode==4) config.result_limits.max_metadata_entries=12;
    Fixture f(config);
    if (mode==0) append(*f.transport,{std::byte{0}},1);
    else {
      prepared_metadata(*f.transport);
      if (mode==1) append(*f.transport,ok(),2);
      else if (mode==2) { append(*f.transport,{std::byte{1}},1);append(*f.transport,column_packet("id"),2);
        append(*f.transport,eof_packet(),3);append(*f.transport,{std::byte{0},std::byte{0},std::byte{42}},4); }
      else binary_result(*f.transport);
    }
    const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
    auto result=f.session->execute_prepared("SELECT ?",params,rs::util::make_deadline(std::chrono::seconds(10)));
    ASSERT_FALSE(result);EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
    EXPECT_EQ(rs::core::database::BackendOperation::ExecutePrepared,result.backend_error().operation);
    EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
    if (mode>=3) EXPECT_EQ(DbErrorCode::ResourceLimit,result.error());
  }
}
TEST(MySqlSessionTest, PreparedCloseFailureAndPeerLossRetireSuccessfulExecution) {
  for (bool peer_loss:{false,true}) {
    Fixture f;prepared_metadata(*f.transport);binary_result(*f.transport);
    // SELECT ? prepare = 13 bytes; one Int32 execute = 22 bytes; close = 9 bytes.
    if (peer_loss) f.transport->lose_peer_after_output=44;else f.transport->fail_send_offset=35;
    const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
    auto result=f.session->execute_prepared("SELECT ?",params,rs::util::make_deadline(std::chrono::seconds(10)));
    ASSERT_FALSE(result);EXPECT_EQ(peer_loss?DbErrorCode::TLSError:DbErrorCode::NetworkError,result.error());
    EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlSessionTest, PreparedNoParameterDmlClosesWithoutReadingCloseAcknowledgement) {
  Fixture f;prepared_metadata(*f.transport,0,0);auto response=ok();response[1]=std::byte{2};append(*f.transport,response,1);
  auto result=f.session->execute_prepared("UPDATE fixture SET id=1",{},rs::util::make_deadline(std::chrono::seconds(10)));
  ASSERT_TRUE(result);EXPECT_EQ(2u,result->affected_rows);EXPECT_TRUE(result->rows.empty());
  EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));EXPECT_EQ(f.transport->input.size(),f.transport->offset);
}

TEST(MySqlSessionTest, PreparedAggregateRequestLimitPreservesOwnerBeforeAnyCommand) {
  auto config=settings();config.input_limits.max_request_wire_bytes=43;Fixture f(config);
  const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
  auto result=f.session->execute_prepared("SELECT ?",params,rs::util::make_deadline(std::chrono::seconds(10)));
  ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ResourceLimit,result.error());EXPECT_TRUE(f.transport->output.empty());
  EXPECT_EQ(0u,f.transport->closes);EXPECT_TRUE(f.session->is_connected());
  append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());
}
TEST(MySqlSessionTest, PreparedExpiredDeadlineAndUnverifiedPeerNeverSendSql) {
  for (bool expired:{true,false}) {
    Fixture f;if (!expired) f.transport->verified=false;
    const auto deadline=expired?rs::util::Clock::now():rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT 1",{},deadline);ASSERT_FALSE(result);
    EXPECT_EQ(expired?DbErrorCode::Timeout:DbErrorCode::TLSError,result.error());
    EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(1u,f.transport->closes);EXPECT_FALSE(f.session->is_connected());
  }
}
