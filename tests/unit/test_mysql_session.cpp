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
TEST(MySqlSessionTest, ErrorSqlstateValidationPreservesServerClassificationAndRetiresMalformedPackets) {
  const Bytes valid{std::byte{255},std::byte{40},std::byte{4},std::byte{'#'},
      std::byte{'4'},std::byte{'2'},std::byte{'S'},std::byte{'0'},std::byte{'2'}};
  for (std::size_t mode=0;mode<6;++mode) {
    Fixture f;auto reply=valid;
    if (mode) reply[mode+3]=std::byte{'a'};
    // Message bytes are opaque and cannot appear in the fixed public diagnostic.
    reply.push_back(std::byte{255});len_text(reply,"native-message-canary");
    append(*f.transport,reply,1);auto result=f.execute();ASSERT_FALSE(result);
    EXPECT_EQ(mode?DbErrorCode::ProtocolError:DbErrorCode::QueryFailed,result.error());
    EXPECT_EQ("MySQL session operation failed",result.error_message());
    EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
    EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
    EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
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
    auto column=column_packet("id",mode==0?7:8);
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
  number(first,0,1);number(first,0,2);append(t,first,1);std::uint8_t seq=2;
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
TEST(MySqlSessionTest, PreparedUnsignedIntMetadataPreservesMaximumNullAndOwnedRows) {
  Fixture f;prepared_metadata(*f.transport);
  append(*f.transport,{std::byte{1}},1);
  append(*f.transport,column_packet("value",3,63,32),2);
  append(*f.transport,eof_packet(),3);
  Bytes maximum{std::byte{0},std::byte{0}};number(maximum,0xffffffffu,4);
  append(*f.transport,maximum,4);append(*f.transport,{std::byte{0},std::byte{4}},5);
  append(*f.transport,eof_packet(),6);
  const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
  const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
  auto result=f.session->execute_prepared("SELECT ?",params,deadline);ASSERT_TRUE(result);
  ASSERT_EQ(1u,result->columns.size());ASSERT_TRUE(result->columns[0].normalized_type);
  EXPECT_TRUE(result->columns[0].normalized_type->known);
  EXPECT_EQ(rs::core::database::ScalarType::BigInt,result->columns[0].normalized_type->type);
  EXPECT_EQ(10u,result->columns[0].normalized_type->column_size);
  ASSERT_EQ(2u,result->rows.size());ASSERT_EQ(1u,result->rows[0].size());ASSERT_TRUE(result->rows[0][0]);
  EXPECT_EQ("4294967295",*result->rows[0][0]);ASSERT_EQ(1u,result->rows[1].size());EXPECT_FALSE(result->rows[1][0]);
  EXPECT_TRUE(result->cell_errors.empty());EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(0u,f.transport->closes);
  EXPECT_EQ(rs::core::database::SessionState::Idle,result.session_snapshot().state);
  EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
  EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));
  EXPECT_EQ(f.transport->input.size(),f.transport->offset);
  for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
  append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());
  f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
  EXPECT_EQ("4294967295",*result->rows[0][0]);EXPECT_FALSE(result->rows[1][0]);
  EXPECT_EQ(rs::core::database::ScalarType::BigInt,result->columns[0].normalized_type->type);
}

TEST(MySqlSessionTest, PreparedFreshMetadataPreservesUnsignedBigintBinaryNullAndDeferredTextError) {
  Fixture f;prepared_metadata(*f.transport,1,3);
  // Execution supplies fresh metadata instead of preparation's signed BIGINT columns.
  append(*f.transport,{std::byte{3}},1);
  append(*f.transport,column_packet("maximum",8,63,32),2);
  append(*f.transport,column_packet("bytes",252,63),3);
  append(*f.transport,column_packet("text",253,45),4);append(*f.transport,eof_packet(),5);
  Bytes maximum{std::byte{0},std::byte{0}};
  for (unsigned i=0;i<8;++i) maximum.push_back(std::byte{255});
  len_text(maximum,std::string_view("\0\xff",2));len_text(maximum,std::string_view("\xff",1));
  append(*f.transport,maximum,6);
  // Numeric NULL, followed by two non-NULL empty length-encoded values.
  append(*f.transport,{std::byte{0},std::byte{4},std::byte{0},std::byte{0}},7);
  Bytes zero{std::byte{0},std::byte{24}};number(zero,0,4);number(zero,0,4);
  append(*f.transport,zero,8);append(*f.transport,eof_packet(),9);
  const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
  const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
  auto result=f.session->execute_prepared("SELECT ?",params,deadline);ASSERT_TRUE(result);
  ASSERT_EQ(3u,result->columns.size());
  for (const auto& column:result->columns) { ASSERT_TRUE(column.normalized_type);EXPECT_TRUE(column.normalized_type->known); }
  const auto& numeric=*result->columns[0].normalized_type;
  EXPECT_EQ("maximum",result->columns[0].name);
  EXPECT_EQ(rs::core::database::ScalarType::Numeric,numeric.type);EXPECT_EQ(20u,numeric.column_size);EXPECT_EQ(0,numeric.decimal_digits);
  EXPECT_EQ(rs::core::database::ScalarType::Binary,result->columns[1].normalized_type->type);
  EXPECT_EQ(rs::core::database::ScalarType::VarChar,result->columns[2].normalized_type->type);
  ASSERT_EQ(3u,result->rows.size());for (const auto& row:result->rows) ASSERT_EQ(3u,row.size());
  ASSERT_TRUE(result->rows[0][0]);EXPECT_EQ("18446744073709551615",*result->rows[0][0]);
  ASSERT_TRUE(result->rows[0][1]);EXPECT_EQ(std::string("\0\xff",2),*result->rows[0][1]);
  ASSERT_TRUE(result->rows[0][2]);EXPECT_TRUE(result->rows[0][2]->empty());
  EXPECT_FALSE(result->rows[1][0]);
  for (unsigned i=1;i<3;++i) { ASSERT_TRUE(result->rows[1][i]);EXPECT_TRUE(result->rows[1][i]->empty()); }
  ASSERT_TRUE(result->rows[2][0]);EXPECT_EQ("0",*result->rows[2][0]);
  EXPECT_FALSE(result->rows[2][1]);EXPECT_FALSE(result->rows[2][2]);
  EXPECT_EQ((std::vector<rs::core::database::CellEncodingError>{{0,2}}),result->cell_errors);
  EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));
  EXPECT_EQ(f.transport->input.size(),f.transport->offset);
  for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
  EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(0u,f.transport->closes);
  EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
  append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
  EXPECT_EQ("18446744073709551615",*result->rows[0][0]);EXPECT_EQ(std::string("\0\xff",2),*result->rows[0][1]);
  EXPECT_FALSE(result->rows[1][0]);EXPECT_EQ(20u,result->columns[0].normalized_type->column_size);
  EXPECT_EQ((std::vector<rs::core::database::CellEncodingError>{{0,2}}),result->cell_errors);
}

namespace {
Bytes decimal_column_packet(unsigned width=7,unsigned scale=2,bool unsigned_value=false) {
  auto bytes=column_packet("decimal",246,63,unsigned_value?32:0);
  for (unsigned i=0;i<4;++i) bytes[bytes.size()-10+i]=static_cast<std::byte>((width>>(8*i))&255);
  bytes[bytes.size()-3]=static_cast<std::byte>(scale);return bytes;
}
void decimal_preparation(FakeTransport& t) {
  Bytes first{std::byte{0}};number(first,17,4);number(first,1,2);number(first,1,2);
  number(first,0,1);number(first,0,2);append(t,first,1);
  append(t,column_packet("?",253,45),2);append(t,eof_packet(),3);
  append(t,decimal_column_packet(),4);append(t,eof_packet(),5);
}
}
TEST(MySqlSessionTest, DecimalResultsShareDirectAndPreparedErrorsDrainRecoveryAndOwnership) {
  for (bool binary:{false,true}) {
    SCOPED_TRACE(binary);
    Fixture f;if (binary) decimal_preparation(*f.transport);
    append(*f.transport,{std::byte{1}},1);append(*f.transport,decimal_column_packet(),2);
    append(*f.transport,eof_packet(),3);
    std::uint8_t sequence=4;
    for (const auto& value:{std::optional<std::string>{"-999.99"},std::optional<std::string>{},
        std::optional<std::string>{""},std::optional<std::string>{"1.20"}}) {
      Bytes row;
      if (binary) row={std::byte{0},value?std::byte{0}:std::byte{4}};
      if (value) len_text(row,*value);else if (!binary) row.push_back(std::byte{251});
      append(*f.transport,row,sequence++);
    }
    append(*f.transport,eof_packet(),sequence);
    const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=binary?f.session->execute_prepared("SELECT ?",params,deadline):f.session->execute_query("SELECT decimal",deadline);
    ASSERT_TRUE(result);ASSERT_EQ(1u,result->columns.size());ASSERT_TRUE(result->columns[0].normalized_type);
    const auto& info=*result->columns[0].normalized_type;
    EXPECT_EQ(rs::core::database::ScalarType::Decimal,info.type);EXPECT_EQ(5u,info.column_size);EXPECT_EQ(2,info.decimal_digits);EXPECT_TRUE(info.known);
    ASSERT_EQ(4u,result->rows.size());for (const auto& row:result->rows) ASSERT_EQ(1u,row.size());
    EXPECT_EQ(std::optional<std::string>{"-999.99"},result->rows[0][0]);EXPECT_FALSE(result->rows[1][0]);
    EXPECT_EQ(std::optional<std::string>{""},result->rows[2][0]);EXPECT_EQ(std::optional<std::string>{"1.20"},result->rows[3][0]);
    EXPECT_EQ((std::vector<rs::core::database::CellEncodingError>{{2,0}}),result->cell_errors);
    EXPECT_EQ(binary?(std::vector<unsigned>{22,23,25}):(std::vector<unsigned>{3}),commands(f.transport->output));
    EXPECT_EQ(f.transport->input.size(),f.transport->offset);for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
    EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(0u,f.transport->closes);
    EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
    append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
    f.session.reset();
    EXPECT_EQ(std::optional<std::string>{"-999.99"},result->rows[0][0]);EXPECT_EQ(5u,result->columns[0].normalized_type->column_size);
    EXPECT_EQ((std::vector<rs::core::database::CellEncodingError>{{2,0}}),result->cell_errors);
  }
}
TEST(MySqlSessionTest, DecimalMalformedMetadataAndLengthFramingRetireWithoutPartialSuccess) {
  for (bool binary:{false,true}) {
    for (bool bad_metadata:{false,true}) {
      SCOPED_TRACE(binary);
      SCOPED_TRACE(bad_metadata);
      Fixture f;if (binary) decimal_preparation(*f.transport);
      append(*f.transport,{std::byte{1}},1);
      append(*f.transport,bad_metadata?decimal_column_packet(1,0):decimal_column_packet(),2);
      append(*f.transport,eof_packet(),3);
      Bytes malformed;if (binary) malformed={std::byte{0},std::byte{0}};
      malformed.push_back(std::byte{5});malformed.push_back(std::byte{'1'});append(*f.transport,malformed,4);
      const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
      const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
      auto result=binary?f.session->execute_prepared("SELECT ?",params,deadline):f.session->execute_query("SELECT decimal",deadline);
      ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::ProtocolError,result.error());
      EXPECT_EQ(binary?rs::core::database::BackendOperation::ExecutePrepared:rs::core::database::BackendOperation::ExecuteDirect,result.backend_error().operation);
      EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
      EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);EXPECT_EQ(1u,f.transport->closes);
      EXPECT_EQ(binary?(std::vector<unsigned>{22,23}):(std::vector<unsigned>{3}),commands(f.transport->output));
      for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
      EXPECT_FALSE(f.session->is_connected());f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
    }
  }
}
TEST(MySqlSessionTest, DecimalParameterHintsAndNativeDescriptorsRemainUnsupported) {
  for (unsigned mode=0;mode<3;++mode) {
    Fixture f;const auto calls=f.transport->calls;
    if (mode==2) {
      Bytes first{std::byte{0}};number(first,17,4);number(first,0,2);number(first,1,2);
      number(first,0,1);number(first,0,2);append(*f.transport,first,1);append(*f.transport,decimal_column_packet(),2);
    }
    const std::array params{rs::core::database::QueryParameter{mode==1?std::optional<std::string>{}:std::optional<std::string>{"1.20"},
        mode==2?rs::core::database::QueryParameterType::Text:rs::core::database::QueryParameterType::Numeric}};
    auto result=f.session->execute_prepared("SELECT ?",params,rs::util::make_deadline(std::chrono::seconds(10)));
    ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
    EXPECT_EQ(rs::core::database::BackendOperation::ExecutePrepared,result.backend_error().operation);
    EXPECT_EQ(mode==2?rs::core::database::SessionDisposition::Retire:rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
    EXPECT_EQ(mode!=2,f.session->is_connected());EXPECT_EQ(mode==2?1u:0u,f.transport->closes);
    if (mode!=2) { EXPECT_EQ(calls,f.transport->calls);EXPECT_TRUE(f.transport->output.empty()); }
    else EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
  }
}

namespace {
Bytes date_column_packet() { return column_packet("date",10,63); }
void date_preparation(FakeTransport& t) {
  Bytes first{std::byte{0}};number(first,17,4);number(first,2,2);number(first,0,2);
  number(first,0,1);number(first,0,2);append(t,first,1);
  append(t,date_column_packet(),2);append(t,column_packet("neighbor"),3);append(t,eof_packet(),4);
}
}
TEST(MySqlSessionTest, DateResultsDecodeActualMetadataDrainInvalidCellsAndOwnRows) {
  for (bool binary:{false,true}) {
    SCOPED_TRACE(binary);Fixture f;if (binary) date_preparation(*f.transport);
    append(*f.transport,{std::byte{2}},1);append(*f.transport,date_column_packet(),2);
    append(*f.transport,column_packet("neighbor"),3);append(*f.transport,eof_packet(),4);
    std::uint8_t sequence=5;
    const std::array texts{"2000-02-29","0000-00-00","2024-00-01","1900-02-29","9999-12-31"};
    const std::array<unsigned,5> years{2000,0,2024,1900,9999},months{2,0,0,2,12},days{29,0,1,29,31};
    for (std::size_t i=0;i<texts.size();++i) {
      Bytes row;
      if (binary) {
        row={std::byte{0},std::byte{0}};
        if (i==1) row.push_back(std::byte{0});
        else { row.push_back(std::byte{4});number(row,years[i],2);number(row,months[i],1);number(row,days[i],1); }
        number(row,42,4);number(row,0,4);
      } else { len_text(row,texts[i]);len_text(row,"42"); }
      append(*f.transport,row,sequence++);
    }
    Bytes nullrow;if (binary) { nullrow={std::byte{0},std::byte{4}};number(nullrow,42,4);number(nullrow,0,4); }
    else { nullrow.push_back(std::byte{251});len_text(nullrow,"42"); }
    append(*f.transport,nullrow,sequence++);append(*f.transport,eof_packet(),sequence);
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=binary?f.session->execute_prepared("SELECT date,neighbor",{},deadline):f.session->execute_query("SELECT date,neighbor",deadline);
    ASSERT_TRUE(result);ASSERT_EQ(2u,result->columns.size());ASSERT_TRUE(result->columns[0].normalized_type);
    const auto& info=*result->columns[0].normalized_type;
    EXPECT_TRUE(info.known);EXPECT_EQ(rs::core::database::ScalarType::Date,info.type);EXPECT_EQ(10u,info.column_size);EXPECT_EQ(0,info.decimal_digits);
    ASSERT_EQ(6u,result->rows.size());for (const auto& row:result->rows) { ASSERT_EQ(2u,row.size());EXPECT_EQ(std::optional<std::string>{"42"},row[1]); }
    EXPECT_EQ(std::optional<std::string>{"2000-02-29"},result->rows[0][0]);
    for (unsigned i=1;i<4;++i) EXPECT_EQ(std::optional<std::string>{""},result->rows[i][0]);
    EXPECT_EQ(std::optional<std::string>{"9999-12-31"},result->rows[4][0]);EXPECT_FALSE(result->rows[5][0]);
    const std::vector<rs::core::database::CellEncodingError> errors{{1,0},{2,0},{3,0}};
    EXPECT_EQ(errors,result->cell_errors);EXPECT_FALSE(result->error);EXPECT_TRUE(result->additional_results.empty());
    EXPECT_EQ(binary?(std::vector<unsigned>{22,23,25}):(std::vector<unsigned>{3}),commands(f.transport->output));
    EXPECT_EQ(f.transport->input.size(),f.transport->offset);for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
    EXPECT_EQ(rs::core::database::SessionState::Idle,result.session_snapshot().state);
    EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
    EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(0u,f.transport->closes);
    append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);f.session.reset();
    EXPECT_EQ(std::optional<std::string>{"2000-02-29"},result->rows[0][0]);EXPECT_FALSE(result->rows[5][0]);
    EXPECT_EQ(errors,result->cell_errors);EXPECT_EQ(10u,result->columns[0].normalized_type->column_size);
  }
}
TEST(MySqlSessionTest, DateStructuralMetadataAndRowFaultsRetireWithoutPartialSuccessOrReplay) {
  // stage0 direct metadata,1 preparation metadata,2 execution metadata,
  // stage3 direct row,4 binary row. Queued remaining rows must stay unread.
  for (unsigned stage=0;stage<5;++stage) {
    for (unsigned fault=0;fault<(stage==4?5u:1u);++fault) {
      SCOPED_TRACE(stage);
      SCOPED_TRACE(fault);Fixture f;
      const bool binary=stage==1 || stage==2 || stage==4;
      if (binary && stage!=1) date_preparation(*f.transport);
      if (stage==1) {
        Bytes first{std::byte{0}};number(first,17,4);number(first,1,2);number(first,0,2);
        number(first,0,1);number(first,0,2);append(*f.transport,first,1);
        auto metadata=date_column_packet();metadata.pop_back();append(*f.transport,metadata,2);
      } else {
        append(*f.transport,{std::byte{2}},1);auto metadata=date_column_packet();
        if (stage<3) { metadata.pop_back(); }
        append(*f.transport,metadata,2);
        if (stage>=3) { append(*f.transport,column_packet("neighbor"),3);append(*f.transport,eof_packet(),4); }
      }
      if (stage>=3) {
        Bytes row;
        if (stage==3) row={std::byte{10},std::byte{'2'}};
        else {
          row={std::byte{0},std::byte{0}};
          const unsigned length=fault==0?4:(fault==1?7:(fault==2?11:(fault==3?7:1)));
          row.push_back(static_cast<std::byte>(length));
          if (fault==1 || fault==2) for (unsigned i=0;i<length;++i) row.push_back(std::byte{0});
          // fault0/3 are truncated4/7; fault4 invalidlength1.
        }
        append(*f.transport,row,5);
      }
      const auto rejected_at=f.transport->input.size();append(*f.transport,eof_packet(),6);
      append(*f.transport,{std::byte{0},std::byte{12}},7);
      const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
      auto result=binary?f.session->execute_prepared("SELECT date,neighbor",{},deadline):f.session->execute_query("SELECT date,neighbor",deadline);
      ASSERT_FALSE(result);EXPECT_EQ(stage==4 && (fault==1 || fault==2)?DbErrorCode::UnsupportedFeature:DbErrorCode::ProtocolError,result.error());
      EXPECT_EQ(binary?rs::core::database::BackendOperation::ExecutePrepared:rs::core::database::BackendOperation::ExecuteDirect,result.backend_error().operation);
      EXPECT_EQ("MySQL session operation failed",result.error_message());
      EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
      EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
      EXPECT_EQ(1u,f.transport->closes);EXPECT_EQ(rejected_at,f.transport->offset);EXPECT_LT(f.transport->offset,f.transport->input.size());
      EXPECT_EQ(binary?(stage==1?std::vector<unsigned>{22}:std::vector<unsigned>{22,23}):std::vector<unsigned>{3},commands(f.transport->output));
      for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
      const auto output=f.transport->output;const auto calls=f.transport->calls;
      auto again=f.execute();ASSERT_FALSE(again);EXPECT_EQ(DbErrorCode::NotConnected,again.error());
      EXPECT_EQ(output,f.transport->output);EXPECT_EQ(calls,f.transport->calls);f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
    }
  }
}
TEST(MySqlSessionTest, OtherTemporalHintsStayUnsupported) {
  for (auto hint:{rs::core::database::QueryParameterType::Time,rs::core::database::QueryParameterType::Timestamp})
    for (bool null:{false,true}) {
      Fixture f;const auto calls=f.transport->calls;
      const std::array params{rs::core::database::QueryParameter{null?std::nullopt:std::optional<std::string>{"2000-02-29"},hint}};
      const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
      auto result=f.session->execute_prepared("SELECT ?",params,deadline);ASSERT_FALSE(result);
      EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());EXPECT_EQ("MySQL session operation failed",result.error_message());
      EXPECT_EQ(rs::core::database::BackendOperation::ExecutePrepared,result.backend_error().operation);
      EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
      EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(0u,f.transport->closes);EXPECT_EQ(calls,f.transport->calls);
      EXPECT_TRUE(f.transport->output.empty());
    }
}

TEST(MySqlSessionTest, PreparedOtherTemporalMetadataRefusalRetiresBeforeQueuedRows) {
  for (unsigned native_type:{7u,11u,14u,17u,18u,19u}) {
    for (bool execution_metadata:{false,true}) {
      SCOPED_TRACE(native_type);
      SCOPED_TRACE(execution_metadata);
      Fixture f;
      if (execution_metadata) {
        prepared_metadata(*f.transport);append(*f.transport,{std::byte{1}},1);
      } else {
        Bytes first{std::byte{0}};number(first,17,4);number(first,1,2);number(first,1,2);
        number(first,0,1);number(first,0,2);append(*f.transport,first,1);
        append(*f.transport,column_packet("?",253,45),2);append(*f.transport,eof_packet(),3);
      }
      auto unsupported=column_packet("unsupported",native_type);
      append(*f.transport,unsupported,execution_metadata?2:4);
      const auto rejected_at=f.transport->input.size();
      append(*f.transport,eof_packet(),execution_metadata?3:5);
      if (execution_metadata) {
        // Even an all-NULL row must not turn unsupported metadata into a success.
        append(*f.transport,{std::byte{0},std::byte{4}},4);append(*f.transport,eof_packet(),5);
      }
      const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
      const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
      auto result=f.session->execute_prepared("SELECT ?",params,deadline);ASSERT_FALSE(result);
      EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());
      EXPECT_EQ(rs::core::database::BackendOperation::ExecutePrepared,result.backend_error().operation);
      EXPECT_EQ("MySQL session operation failed",result.error_message());
      EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
      EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
      EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
      EXPECT_EQ(rejected_at,f.transport->offset);EXPECT_LT(f.transport->offset,f.transport->input.size());
      const auto expected_commands=execution_metadata?(std::vector<unsigned>{22,23}):(std::vector<unsigned>{22});
      EXPECT_EQ(expected_commands,commands(f.transport->output));
      for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
      const auto output=f.transport->output;const auto calls=f.transport->calls;
      auto again=f.execute();ASSERT_FALSE(again);EXPECT_EQ(DbErrorCode::NotConnected,again.error());
      EXPECT_EQ(output,f.transport->output);EXPECT_EQ(calls,f.transport->calls);
      f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
    }
  }
}

TEST(MySqlSessionTest, PreparedMetadataAndLateRowErrorsValidateSqlstateAndRetire) {
  for (bool late_row:{false,true}) {
    for (unsigned fault=0;fault<3;++fault) {
      SCOPED_TRACE(late_row);
      SCOPED_TRACE(fault);
      Fixture f;
      if (late_row) {
        prepared_metadata(*f.transport);
        append(*f.transport,{std::byte{1}},1);append(*f.transport,column_packet("id"),2);
        append(*f.transport,eof_packet(),3);
        Bytes row{std::byte{0},std::byte{0}};number(row,42,4);number(row,0,4);append(*f.transport,row,4);
      } else {
        Bytes first{std::byte{0}};number(first,17,4);number(first,1,2);number(first,1,2);
        number(first,0,1);number(first,0,2);append(*f.transport,first,1);
      }
      Bytes reply{std::byte{255},std::byte{40},std::byte{4},std::byte{'#'},
          std::byte{'4'},std::byte{'2'},std::byte{'S'},std::byte{'0'},std::byte{'2'}};
      if (fault) reply[6]=fault==1?std::byte{'s'}:std::byte{0};
      for (const auto ch:std::string_view("native-message-canary")) reply.push_back(static_cast<std::byte>(ch));
      reply.push_back(std::byte{255});append(*f.transport,reply,late_row?5:2);
      const std::array params{rs::core::database::QueryParameter{"42",rs::core::database::QueryParameterType::Int32}};
      const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
      auto result=f.session->execute_prepared("SELECT ?",params,deadline);ASSERT_FALSE(result);
      // Failed BackendResult carries only an error, so the previously decoded row cannot escape.
      EXPECT_EQ(fault?DbErrorCode::ProtocolError:DbErrorCode::QueryFailed,result.error());
      EXPECT_EQ("MySQL session operation failed",result.error_message());
      EXPECT_EQ(rs::core::database::BackendOperation::ExecutePrepared,result.backend_error().operation);
      EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
      EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
      EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
      EXPECT_EQ(f.transport->input.size(),f.transport->offset);
      EXPECT_EQ(late_row?(std::vector<unsigned>{22,23}):(std::vector<unsigned>{22}),commands(f.transport->output));
      for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
      f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
    }
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
    if (mode>=3) { EXPECT_EQ(DbErrorCode::ResourceLimit,result.error()); }
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

TEST(MySqlSessionTest, TransactionFacetDeclaresInnoDbSemanticsAndBorrowsStablePointer) {
  Fixture f;auto* facet=f.session->transaction_session();ASSERT_NE(nullptr,facet);
  auto caps=facet->transaction_capabilities();EXPECT_TRUE(caps.supported);EXPECT_FALSE(caps.transactional_ddl);
  EXPECT_EQ(rs::core::database::TransactionIsolation::RepeatableRead,caps.default_isolation);
  for (auto level:rs::core::database::transaction_isolations) { EXPECT_TRUE(caps.supports(level)); }
  auto begin=ok();begin[3]=std::byte{3};append(*f.transport,begin,1);
  const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
  auto result=facet->transaction(rs::core::database::TransactionAction::Begin,deadline);ASSERT_TRUE(result);
  EXPECT_EQ(rs::core::database::SessionState::Transaction,result.session_snapshot().state);
  EXPECT_EQ(rs::core::database::SessionDisposition::ResetRequired,result.session_snapshot().disposition);
  const std::string sent(reinterpret_cast<const char*>(f.transport->output.data()+5),f.transport->output.size()-5);
  EXPECT_EQ("START TRANSACTION",sent);
  const auto size=f.transport->output.size();
  auto nested=facet->transaction(rs::core::database::TransactionAction::Begin,deadline);ASSERT_FALSE(nested);
  EXPECT_EQ(rs::core::database::BackendOperation::BeginTransaction,nested.backend_error().operation);
  EXPECT_EQ(rs::core::database::SessionState::Transaction,nested.session_snapshot().state);
  EXPECT_EQ(rs::core::database::SessionDisposition::ResetRequired,nested.session_snapshot().disposition);
  auto isolation=facet->set_transaction_isolation(rs::core::database::TransactionIsolation::ReadCommitted,deadline);ASSERT_FALSE(isolation);
  EXPECT_EQ(rs::core::database::BackendOperation::SetTransactionIsolation,isolation.backend_error().operation);
  EXPECT_EQ(rs::core::database::SessionState::Transaction,isolation.session_snapshot().state);
  EXPECT_EQ(size,f.transport->output.size());EXPECT_EQ(0u,f.transport->closes);
  append(*f.transport,ok(),1);result=facet->transaction(rs::core::database::TransactionAction::Rollback,deadline);ASSERT_TRUE(result);
  EXPECT_EQ(rs::core::database::SessionState::Idle,result.session_snapshot().state);
  for (const auto dl:f.transport->deadlines) { EXPECT_EQ(deadline,dl); }
  f.session->disconnect();EXPECT_EQ(facet,f.session->transaction_session());
  result=facet->transaction(rs::core::database::TransactionAction::Commit,deadline);ASSERT_FALSE(result);
  EXPECT_EQ(DbErrorCode::NotConnected,result.error());
  EXPECT_EQ(rs::core::database::BackendOperation::CommitTransaction,result.backend_error().operation);
}
TEST(MySqlSessionTest, IsolationCommandsAreFixedAndInvalidEnumsDoNotMutateOwner) {
  Fixture f;auto* facet=f.session->transaction_session();const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
  const std::array names{"READ UNCOMMITTED","READ COMMITTED","REPEATABLE READ","SERIALIZABLE"};
  for (std::size_t i=0;i<names.size();++i) {
    f.transport->output.clear();append(*f.transport,ok(),1);
    EXPECT_TRUE(facet->set_transaction_isolation(rs::core::database::transaction_isolations[i],deadline));
    const std::string sent(reinterpret_cast<const char*>(f.transport->output.data()+5),f.transport->output.size()-5);
    EXPECT_EQ(std::string("SET SESSION TRANSACTION ISOLATION LEVEL ")+names[i],sent);
  }
  const auto size=f.transport->output.size();
  EXPECT_FALSE(facet->set_transaction_isolation(static_cast<rs::core::database::TransactionIsolation>(99),deadline));
  EXPECT_FALSE(facet->transaction(static_cast<rs::core::database::TransactionAction>(99),deadline));
  EXPECT_EQ(size,f.transport->output.size());EXPECT_TRUE(f.session->is_connected());EXPECT_EQ(0u,f.transport->closes);
}
TEST(MySqlSessionTest, TransactionCompletionStateWarningsAndServerFailuresRetire) {
  for (unsigned mode=0;mode<6;++mode) {
    Fixture f;
    if (mode==0) append(*f.transport,ok(),1); // BEGIN must report IN_TRANS.
    if (mode==1) { auto bytes=ok();bytes[1]=std::byte{1};bytes[3]=std::byte{3};append(*f.transport,bytes,1); }
    if (mode==2) { auto bytes=ok();bytes[5]=std::byte{1};append(*f.transport,bytes,1); }
    if (mode==3) append(*f.transport,{std::byte{255},std::byte{1},std::byte{0},std::byte{'#'},std::byte{'4'},std::byte{'2'},std::byte{'0'},std::byte{'0'},std::byte{'0'}},1);
    if (mode==4) f.rows();
    if (mode==5) { auto bytes=ok();bytes[2]=std::byte{1};bytes[3]=std::byte{3};append(*f.transport,bytes,1); }
    auto result=f.session->transaction_session()->transaction(rs::core::database::TransactionAction::Begin,
        rs::util::make_deadline(std::chrono::seconds(10)));
    ASSERT_FALSE(result);EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
    EXPECT_EQ(rs::core::database::BackendOperation::BeginTransaction,result.backend_error().operation);
    EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
  }
}

TEST(MySqlSessionTest, CommitAndRollbackOverrideChainAndReleaseAndRequireIdleCompletion) {
  for (auto action:{rs::core::database::TransactionAction::Commit,rs::core::database::TransactionAction::Rollback}) {
    for (bool in_transaction:{false,true}) {
      Fixture f;auto response=ok();if (in_transaction) response[3]=std::byte{3};append(*f.transport,response,1);
      auto result=f.session->transaction_session()->transaction(action,rs::util::make_deadline(std::chrono::seconds(10)));
      const std::string sent(reinterpret_cast<const char*>(f.transport->output.data()+5),f.transport->output.size()-5);
      EXPECT_EQ(action==rs::core::database::TransactionAction::Commit?"COMMIT AND NO CHAIN NO RELEASE":
          "ROLLBACK AND NO CHAIN NO RELEASE",sent);
      if (in_transaction) { EXPECT_FALSE(result);EXPECT_EQ(1u,f.transport->closes);EXPECT_FALSE(f.session->is_connected()); }
      else { ASSERT_TRUE(result);EXPECT_EQ(rs::core::database::SessionState::Idle,result.session_snapshot().state); }
    }
  }
}

TEST(MySqlSessionTest, RedshiftCatalogModeRejectsBeforeTransportConfigurationOrIo) {
  auto transport = std::make_unique<FakeTransport>(); auto* spy = transport.get();
  MySqlSession session(std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.redshift_catalog_mode = rs::core::database::RedshiftCatalogMode::Legacy;
  auto result = session.connect(settings);
  ASSERT_FALSE(result); EXPECT_EQ(rs::util::make_error_code(DbErrorCode::InvalidParameter), result.error());
  EXPECT_TRUE(spy->deadlines.empty()); EXPECT_EQ(0u, spy->configuration_calls);
  EXPECT_EQ(0u, spy->calls); EXPECT_EQ(0u, spy->upgrades); EXPECT_TRUE(spy->output.empty());
  EXPECT_FALSE(session.is_connected());
}

namespace {
Bytes datetime_column(unsigned p=6,std::optional<unsigned> width=std::nullopt) {
  return column_packet("datetime",12,63,0,width.value_or(19+(p?1+p:0)),p);
}
void datetime_preparation(FakeTransport& t,unsigned p) {
  Bytes first{std::byte{0}};number(first,17,4);number(first,2,2);number(first,0,2);
  number(first,0,1);number(first,0,2);append(t,first,1);
  append(t,datetime_column(p),2);append(t,column_packet("neighbor",3),3);append(t,eof_packet(),4);
}
Bytes datetime_field(unsigned year,unsigned month,unsigned day,unsigned length=4,
    unsigned hour=0,unsigned minute=0,unsigned second=0,unsigned micros=0) {
  Bytes value{static_cast<std::byte>(length)};
  if (length) { number(value,year,2);number(value,month,1);number(value,day,1); }
  if (length>=7) { number(value,hour,1);number(value,minute,1);number(value,second,1); }
  if (length==11) number(value,micros,4);
  return value;
}
TEST(MySqlSessionTest, DatetimeResultsUseActualMetadataDrainCellErrorsRecoverAndOwnCalendarRows) {
  for (bool binary:{false,true}) for (unsigned p:{0u,3u,6u}) {
    SCOPED_TRACE(binary);
    SCOPED_TRACE(p);Fixture f;
    // Preparation uses p0; execution supplies fresh p3/p6 descriptors.
    if (binary) datetime_preparation(*f.transport,0);
    append(*f.transport,{std::byte{2}},1);append(*f.transport,datetime_column(p),2);
    append(*f.transport,column_packet("neighbor",3),3);append(*f.transport,eof_packet(),4);
    const auto zero=p?"."+std::string(p,'0'):"";
    const std::vector<std::optional<std::string>> expected{
      "2000-02-29 12:34:56"+(p?"."+std::string("123456").substr(0,p):""),
      "1000-01-01 00:00:00"+zero,"2024-02-29 01:02:03"+zero,std::nullopt,
      "","","","","","9999-12-31 23:59:59"+zero};
    const std::vector<Bytes> fields{datetime_field(2000,2,29,p?11:7,12,34,56,p==3?123000:123456),
      datetime_field(1000,1,1),datetime_field(2024,2,29,7,1,2,3),{},datetime_field(0,0,0,0),
      datetime_field(2024,0,1),datetime_field(1900,2,29),datetime_field(2024,1,1,7,24,0,0),
      datetime_field(2024,1,1,11,0,0,0,p==6?1000000:123456),datetime_field(9999,12,31,7,23,59,59)};
    std::uint8_t sequence=5;
    for (std::size_t i=0;i<expected.size();++i) {
      Bytes row;
      if (binary) {
        row={std::byte{0},static_cast<std::byte>(i==3?4:0)};
        if (i!=3) row.insert(row.end(),fields[i].begin(),fields[i].end());
        number(row,42,4);
      } else {
        if (i==3) row.push_back(std::byte{251});
        else {
          const std::array<std::string,5> invalid{"0000-00-00 00:00:00"+zero,"2024-00-01 00:00:00"+zero,
              "1900-02-29 00:00:00"+zero,"2024-01-01 24:00:00"+zero,"2024-01-01 00:00:00.12345"};
          len_text(row,i>=4 && i<=8?invalid[i-4]:*expected[i]);
        }
        len_text(row,"42");
      }
      append(*f.transport,row,sequence++);
    }
    append(*f.transport,eof_packet(),sequence);
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=binary?f.session->execute_prepared("SELECT datetime,neighbor",{},deadline):
        f.session->execute_query("SELECT datetime,neighbor",deadline);
    ASSERT_TRUE(result);ASSERT_EQ(2u,result->columns.size());const auto& info=*result->columns[0].normalized_type;
    EXPECT_TRUE(info.known);EXPECT_EQ(rs::core::database::ScalarType::Timestamp,info.type);
    EXPECT_EQ(19+(p?1+p:0),info.column_size);EXPECT_EQ(p,info.decimal_digits);
    ASSERT_EQ(expected.size(),result->rows.size());
    for (std::size_t i=0;i<expected.size();++i) {
      ASSERT_EQ(2u,result->rows[i].size());EXPECT_EQ(expected[i],result->rows[i][0]);
      EXPECT_EQ(std::optional<std::string>{"42"},result->rows[i][1]);
    }
    const std::vector<rs::core::database::CellEncodingError> errors{{4,0},{5,0},{6,0},{7,0},{8,0}};
    EXPECT_EQ(errors,result->cell_errors);EXPECT_FALSE(result->error);EXPECT_TRUE(result->additional_results.empty());
    EXPECT_TRUE(result->normalized_parameter_types.empty());EXPECT_EQ(f.transport->input.size(),f.transport->offset);
    EXPECT_EQ(binary?(std::vector<unsigned>{22,23,25}):(std::vector<unsigned>{3}),commands(f.transport->output));
    for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
    EXPECT_EQ(rs::core::database::SessionState::Idle,result.session_snapshot().state);
    EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
    EXPECT_EQ(0u,f.transport->closes);append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());
    f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);f.session.reset();
    EXPECT_EQ(expected[0],result->rows[0][0]);EXPECT_FALSE(result->rows[3][0]);EXPECT_EQ(errors,result->cell_errors);
    EXPECT_EQ(p,result->columns[0].normalized_type->decimal_digits);
  }
}
TEST(MySqlSessionTest, DatetimeMetadataAndFramingFaultsRetireAtExactPacketWithoutPartialSuccess) {
  // direct metadata, preparation result metadata, fresh execution metadata,
  // direct lenenc row and prepared binary row; queued remainder stays unread.
  for (unsigned stage=0;stage<5;++stage) for (unsigned fault=0;fault<(stage<3?2u:(stage==3?2u:5u));++fault) {
    SCOPED_TRACE(stage);
    SCOPED_TRACE(fault);Fixture f;const bool binary=stage==1 || stage==2 || stage==4;
    if (binary && stage!=1) datetime_preparation(*f.transport,6);
    if (stage==1) {
      Bytes first{std::byte{0}};number(first,17,4);number(first,1,2);number(first,0,2);
      number(first,0,1);number(first,0,2);append(*f.transport,first,1);
    } else append(*f.transport,{std::byte{2}},1);
    append(*f.transport,stage<3?(fault?datetime_column(7):datetime_column(6,25)):datetime_column(),2);
    if (stage>=3) {
      append(*f.transport,column_packet("neighbor",3),3);append(*f.transport,eof_packet(),4);
      Bytes row;
      if (stage==3) {
        if (!fault) row={std::byte{26},std::byte{'2'}};
        else { len_text(row,"2024-02-29 00:00:00.000000");len_text(row,"42");row.push_back(std::byte{0}); }
      } else {
        row={std::byte{0},std::byte{0}};
        if (fault==0) row.push_back(std::byte{1});
        else if (fault<4) row.push_back(static_cast<std::byte>(fault==1?4:(fault==2?7:11)));
        else { const auto field=datetime_field(2024,2,29);row.insert(row.end(),field.begin(),field.end());number(row,42,4);row.push_back(std::byte{0}); }
      }
      append(*f.transport,row,5);
    }
    const auto rejected_at=f.transport->input.size();append(*f.transport,eof_packet(),static_cast<std::uint8_t>(stage>=3?6:3));
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=binary?f.session->execute_prepared("SELECT datetime,neighbor",{},deadline):f.session->execute_query("SELECT datetime,neighbor",deadline);
    ASSERT_FALSE(result);EXPECT_EQ(stage<3 && fault?DbErrorCode::UnsupportedFeature:DbErrorCode::ProtocolError,result.error());
    EXPECT_EQ(binary?rs::core::database::BackendOperation::ExecutePrepared:rs::core::database::BackendOperation::ExecuteDirect,result.backend_error().operation);
    EXPECT_EQ("MySQL session operation failed",result.error_message());EXPECT_EQ(rejected_at,f.transport->offset);
    EXPECT_LT(f.transport->offset,f.transport->input.size());EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
    EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
    EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
    const auto expected_commands=stage==1?std::vector<unsigned>{22}:(binary?std::vector<unsigned>{22,23}:std::vector<unsigned>{3});
    EXPECT_EQ(expected_commands,commands(f.transport->output));for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
    const auto output=f.transport->output;const auto calls=f.transport->calls;
    auto again=f.execute();ASSERT_FALSE(again);EXPECT_EQ(DbErrorCode::NotConnected,again.error());
    EXPECT_EQ(output,f.transport->output);EXPECT_EQ(calls,f.transport->calls);f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
  }
}
TEST(MySqlSessionTest, DatetimeParameterDescriptorsRemainRefusedIncludingNullInputs) {
  for (unsigned p:{0u,3u,6u}) for (bool null:{false,true}) {
    Fixture f;Bytes first{std::byte{0}};number(first,17,4);number(first,0,2);number(first,1,2);
    number(first,0,1);number(first,0,2);append(*f.transport,first,1);append(*f.transport,datetime_column(p),2);
    const auto rejected_at=f.transport->input.size();append(*f.transport,eof_packet(),3);
    const std::array params{rs::core::database::QueryParameter{null?std::optional<std::string>{}:std::optional<std::string>{"2024-01-01 00:00:00"},rs::core::database::QueryParameterType::Text}};
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT ?",params,deadline);ASSERT_FALSE(result);
    EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());EXPECT_EQ("MySQL session operation failed",result.error_message());
    EXPECT_EQ(rs::core::database::BackendOperation::ExecutePrepared,result.backend_error().operation);
    EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
    EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
    EXPECT_EQ(rejected_at,f.transport->offset);EXPECT_LT(f.transport->offset,f.transport->input.size());
    EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);
    for (const auto dl:f.transport->deadlines) EXPECT_EQ(deadline,dl);
    const auto output=f.transport->output;const auto calls=f.transport->calls;
    auto again=f.execute();ASSERT_FALSE(again);EXPECT_EQ(DbErrorCode::NotConnected,again.error());
    EXPECT_EQ(output,f.transport->output);EXPECT_EQ(calls,f.transport->calls);f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
  }
}
}

namespace {
using Parameter=rs::core::database::QueryParameter;
using Hint=rs::core::database::QueryParameterType;
Bytes exact_date_receipt(unsigned width=10,unsigned decimals=0) {
  return column_packet("?",10,63,0,width,decimals);
}
void date_parameter_prepare(FakeTransport& t,const std::vector<Bytes>& receipts,unsigned status=2) {
  Bytes first{std::byte{0}};number(first,17,4);number(first,2,2);number(first,static_cast<unsigned>(receipts.size()),2);
  number(first,0,1);number(first,0,2);append(t,first,1);std::uint8_t seq=2;
  for (const auto& receipt:receipts) append(t,receipt,seq++);
  if (!receipts.empty()) append(t,eof_packet(status),seq++);
  append(t,date_column_packet(),seq++);append(t,column_packet("neighbor"),seq++);append(t,eof_packet(status),seq);
}
void date_parameter_result(FakeTransport& t,bool null=false,bool invalid=false,unsigned status=2) {
  append(t,{std::byte{2}},1);append(t,date_column_packet(),2);append(t,column_packet("neighbor"),3);append(t,eof_packet(status),4);
  Bytes row{std::byte{0},static_cast<std::byte>(null?4:0)};
  if (!null) { number(row,4,1);number(row,invalid?1900:2000,2);number(row,2,1);number(row,29,1); }
  number(row,42,4);number(row,0,4);append(t,row,5);append(t,eof_packet(status),6);
}
Bytes literal(std::initializer_list<unsigned> values) {
  Bytes bytes;for (auto value:values) bytes.push_back(static_cast<std::byte>(value));return bytes;
}
Bytes output_frame(const Bytes& output,std::size_t frame) {
  std::size_t offset=0;
  for (std::size_t i=0;i<=frame;++i) {
    if (output.size()-offset<4) return {};
    const auto size=std::to_integer<std::size_t>(output[offset])|(std::to_integer<std::size_t>(output[offset+1])<<8)|
        (std::to_integer<std::size_t>(output[offset+2])<<16);
    if (size+4>output.size()-offset) return {};
    if (i==frame) return Bytes(output.begin()+offset,output.begin()+offset+size+4);
    offset+=size+4;
  }
  return {};
}
void date_deadlines(const FakeTransport& t,rs::util::Deadline deadline) {
  for (const auto dl:t.deadlines) EXPECT_EQ(deadline,dl);
}
void date_retired(Fixture& f,const rs::core::database::BackendResult<rs::core::database::QueryResult>& result,
                  DbErrorCode error,rs::util::Deadline deadline) {
  ASSERT_FALSE(result);EXPECT_EQ(error,result.error());EXPECT_EQ("MySQL session operation failed",result.error_message());
  EXPECT_EQ(rs::core::database::BackendOperation::ExecutePrepared,result.backend_error().operation);
  EXPECT_EQ(rs::core::database::SessionState::Disconnected,result.session_snapshot().state);
  EXPECT_EQ(rs::core::database::SessionDisposition::Retire,result.session_snapshot().disposition);
  EXPECT_FALSE(f.session->is_connected());EXPECT_EQ(1u,f.transport->closes);date_deadlines(*f.transport,deadline);
  const auto output=f.transport->output;const auto calls=f.transport->calls;
  EXPECT_FALSE(f.execute());EXPECT_EQ(output,f.transport->output);EXPECT_EQ(calls,f.transport->calls);
  f.session->disconnect();EXPECT_EQ(1u,f.transport->closes);
}
}
TEST(MySqlSessionTest, DateParametersUseActualReceiptTypedNullFramesAndOwnResults) {
  for (unsigned charset:{63u,45u}) for (bool null:{false,true}) for (unsigned status:{2u,3u}) {
    Fixture f;date_parameter_prepare(*f.transport,{column_packet("?",10,charset,0,charset==63?10u:40u)},status);date_parameter_result(*f.transport,null,false,status);
    std::array params{Parameter{null?std::nullopt:std::optional<std::string>{"2000-02-29"},Hint::Date}};
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT ?",params,deadline);ASSERT_TRUE(result);
    ASSERT_EQ(1u,result->normalized_parameter_types.size());const auto receipt=result->normalized_parameter_types[0];
    EXPECT_TRUE(receipt.known);EXPECT_EQ(rs::core::database::ScalarType::Date,receipt.type);
    EXPECT_EQ(10u,receipt.column_size);EXPECT_EQ(0,receipt.decimal_digits);
    EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));
    EXPECT_EQ(null?literal({14,0,0,0,23,17,0,0,0,0,1,0,0,0,1,1,10,0}):
        literal({19,0,0,0,23,17,0,0,0,0,1,0,0,0,0,1,10,0,4,208,7,2,29}),output_frame(f.transport->output,1));
    EXPECT_EQ(literal({5,0,0,0,25,17,0,0,0}),output_frame(f.transport->output,2));
    ASSERT_EQ(1u,result->rows.size());EXPECT_EQ(params[0].value,result->rows[0][0]);EXPECT_EQ("42",*result->rows[0][1]);
    EXPECT_TRUE(result->cell_errors.empty());EXPECT_EQ(f.transport->input.size(),f.transport->offset);
    EXPECT_EQ(status==3?rs::core::database::SessionDisposition::ResetRequired:rs::core::database::SessionDisposition::Reusable,
        result.session_snapshot().disposition);date_deadlines(*f.transport,deadline);
    params[0].value="invalid";append(*f.transport,ok(),1);ASSERT_TRUE(f.execute());f.session->disconnect();f.session.reset();
    EXPECT_EQ(null?std::optional<std::string>{}:std::optional<std::string>{"2000-02-29"},result->rows[0][0]);
    EXPECT_EQ(10u,result->normalized_parameter_types[0].column_size);
  }
}
TEST(MySqlSessionTest, DateParametersNineMixedReceiptsCrossBitmapAndPreserveOrdinaryNeighbors) {
  Fixture f;date_parameter_prepare(*f.transport,{exact_date_receipt(),column_packet("?",2),column_packet("?",3),
      column_packet("?",8),column_packet("?",1),column_packet("?",253,45),column_packet("?",252),exact_date_receipt(),exact_date_receipt()});
  date_parameter_result(*f.transport);
  const std::array params{Parameter{"2000-02-29",Hint::Date},Parameter{"-32768",Hint::Int16},Parameter{"2147483647",Hint::Int32},
      Parameter{"-9223372036854775808",Hint::Int64},Parameter{"1",Hint::Boolean},Parameter{std::string("a\0b",3),Hint::Text},
      Parameter{std::string("\0\xff",2),Hint::Binary,true},Parameter{std::nullopt,Hint::Date},Parameter{std::nullopt,Hint::Date}};
  const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));auto result=f.session->execute_prepared("SELECT ?",params,deadline);ASSERT_TRUE(result);
  EXPECT_EQ(literal({58,0,0,0,23,17,0,0,0,0,1,0,0,0,128,1,1,10,0,2,0,3,0,8,0,1,0,253,0,252,0,10,0,10,0,
      4,208,7,2,29,0,128,255,255,255,127,0,0,0,0,0,0,0,128,1,3,97,0,98,2,0,255}),output_frame(f.transport->output,1));
  EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));ASSERT_EQ(9u,result->normalized_parameter_types.size());
  EXPECT_EQ(rs::core::database::ScalarType::Date,result->normalized_parameter_types[8].type);
  EXPECT_EQ(rs::core::database::ScalarType::Binary,result->normalized_parameter_types[6].type);
  EXPECT_EQ(f.transport->input.size(),f.transport->offset);date_deadlines(*f.transport,deadline);
}
TEST(MySqlSessionTest, DateCallerInvalidAndBudgetErrorsPrecedeAllIoAndAllowRecovery) {
  for (unsigned fault=0;fault<9;++fault) {
    auto config=settings();if (fault==8) config.input_limits.max_request_wire_bytes=44;
    Fixture f(config);const auto calls=f.transport->calls;const auto offset=f.transport->offset;
    const std::array invalid{"0000-00-00","1900-02-29","0999-12-31","10000-01-01","", "2000-02-29x"};
    std::string value=fault<6?invalid[fault]:(fault==6?std::string("2000-02-29\0",11):"2000-02-29");
    const std::array params{Parameter{value,Hint::Date,fault==7}};
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));auto result=f.session->execute_prepared("SELECT ?",params,deadline);
    ASSERT_FALSE(result);EXPECT_EQ(fault==8?DbErrorCode::ResourceLimit:DbErrorCode::InvalidParameter,result.error());
    EXPECT_EQ(rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
    EXPECT_TRUE(f.transport->output.empty());EXPECT_EQ(calls,f.transport->calls);EXPECT_EQ(offset,f.transport->offset);
    EXPECT_EQ(0u,f.transport->closes);EXPECT_TRUE(f.session->is_connected());
    append(*f.transport,ok(),1);EXPECT_TRUE(f.execute());
  }
}
TEST(MySqlSessionTest, DateSupportedAffinityMismatchesDrainClosePreserveStateAndCountPrecedence) {
  for (unsigned charset:{63u,45u}) for (unsigned mode=0;mode<8;++mode) for (bool null:{false,true}) for (unsigned status:{2u,3u}) {
    Fixture f;const bool reverse=mode>=4 && mode<7;
    date_parameter_prepare(*f.transport,{reverse?column_packet("?",mode==4?8:(mode==5?253:6),mode==5?45:63):column_packet("?",10,charset,0,charset==63?10u:40u)},status);
    const auto drained=f.transport->input.size();append(*f.transport,ok(),1); // unrelated next command must stay unread
    const auto hint=reverse?Hint::Date:(mode==0?Hint::Unspecified:(mode==1?Hint::Text:(mode==2?Hint::Int32:Hint::Binary)));
    const std::array params{Parameter{null?std::nullopt:std::optional<std::string>{reverse?"2000-02-29":(hint==Hint::Int32?"42":"x")},hint}};
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT ?",mode==7?std::span<const Parameter>{}:std::span<const Parameter>(params),deadline);
    ASSERT_FALSE(result);EXPECT_EQ(mode==7?DbErrorCode::InvalidParameter:DbErrorCode::UnsupportedFeature,result.error());
    EXPECT_EQ(rs::core::database::BackendOperation::ExecutePrepared,result.backend_error().operation);
    EXPECT_EQ(status==3?rs::core::database::SessionDisposition::ResetRequired:rs::core::database::SessionDisposition::Reusable,result.session_snapshot().disposition);
    if (mode!=7) { EXPECT_EQ("MySQL session operation failed",result.error_message()); }
    EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output));EXPECT_EQ(drained,f.transport->offset);
    EXPECT_EQ(literal({5,0,0,0,25,17,0,0,0}),output_frame(f.transport->output,1));
    EXPECT_EQ(0u,f.transport->closes);date_deadlines(*f.transport,deadline);ASSERT_TRUE(f.execute());
  }
}
TEST(MySqlSessionTest, DateReceiptMalformedUnsupportedAndLatePreparationFaultsRetireUnread) {
  for (unsigned fault=0;fault<39;++fault) {
    SCOPED_TRACE(fault);
    auto config=settings();if (fault==14) config.result_limits.max_metadata_entries=12;
    if (fault==38) config.result_limits.max_metadata_name_bytes=15;
    Fixture f(config);Bytes first{std::byte{0}};number(first,17,4);number(first,2,2);number(first,1,2);
    number(first,0,1);number(first,0,2);append(*f.transport,first,1);
    Bytes receipt=exact_date_receipt();
    if (fault<4) receipt=exact_date_receipt(fault==0?0:(fault==1?9:(fault==2?11:0xffffffffu)));
    if (fault==4 || fault==5) receipt=exact_date_receipt(10,fault==4?1:255);
    if (fault==6) { receipt.pop_back(); }
    if (fault==7) { receipt.push_back(std::byte{0}); }
    if (fault==8) receipt.back()=std::byte{1};
    if (fault==9) receipt=column_packet("?",12,63,0,19,0);
    if (fault==10) receipt=column_packet("?",246,63,0,7,2);
    if (fault==11) receipt=column_packet("?",7);
    if (fault>=15 && fault<38) receipt.resize(fault-15); // Every prefix of the 23-byte definition.
    append(*f.transport,receipt,2);auto rejected=f.transport->input.size();
    const auto eof_start=f.transport->input.size();
    append(*f.transport,eof_packet(),fault==12?4:3);
    if (fault==12) rejected=eof_start+4; // Sequence rejection reads only its header.
    auto result_column=date_column_packet();if (fault==13) result_column.pop_back();
    append(*f.transport,result_column,4);if (fault==13) rejected=f.transport->input.size();
    append(*f.transport,column_packet("neighbor"),5);if (fault==38) rejected=f.transport->input.size();
    append(*f.transport,eof_packet(),6);date_parameter_result(*f.transport);
    const std::array params{Parameter{"2000-02-29",Hint::Date}};const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT ?",params,deadline);
    date_retired(f,result,fault>=9 && fault<=11?DbErrorCode::UnsupportedFeature:
        (fault==14 || fault==38?DbErrorCode::ResourceLimit:DbErrorCode::ProtocolError),deadline);
    EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
    if (fault!=14) { EXPECT_EQ(rejected,f.transport->offset); }
    EXPECT_LT(f.transport->offset,f.transport->input.size());
  }
}
TEST(MySqlSessionTest, DateExecutionCellDrainFramingAndMismatchCloseFailuresKeepOriginalDeadline) {
  for (unsigned fault=0;fault<10;++fault) {
    SCOPED_TRACE(fault);
    Fixture f;date_parameter_prepare(*f.transport,{exact_date_receipt()});
    const bool mismatch=fault>=3 && fault<=6;
    if (!mismatch) {
      if (fault==0 || fault>=7) date_parameter_result(*f.transport,false,fault==0);
      else {
        append(*f.transport,{std::byte{2}},1);append(*f.transport,date_column_packet(),2);
        append(*f.transport,column_packet("neighbor"),3);append(*f.transport,eof_packet(),4);
        if (fault==1) append(*f.transport,{std::byte{0},std::byte{0},std::byte{4},std::byte{208}},5);
        else append(*f.transport,literal({255,21,4,35,72,89,48,48,48}),5);
        append(*f.transport,eof_packet(),6);
      }
    }
    const auto drained=f.transport->input.size();append(*f.transport,ok(),1);
    // SELECT ? prepare13; mismatch close9. Fail before or inside CLOSE,
    // or lose identity / expire original deadline after fully sending it.
    if (fault==3) f.transport->fail_send_offset=13;
    if (fault==4) f.transport->fail_send_offset=16;
    if (fault==5) f.transport->lose_peer_after_output=22;
    if (fault==6) f.transport->expire_after_output=22;
    if (fault==7) f.transport->fail_send_offset=36; // prepare13 + DATE execute23
    if (fault==8) f.transport->lose_peer_after_output=45;
    if (fault==9) f.transport->expire_after_output=45;
    const std::array params{Parameter{mismatch?"text":"2000-02-29",mismatch?Hint::Text:Hint::Date}};
    const auto deadline=rs::util::make_deadline(fault==6 || fault==9?std::chrono::milliseconds(20):std::chrono::milliseconds(10000));
    auto result=f.session->execute_prepared("SELECT ?",params,deadline);
    if (fault==0) {
      ASSERT_TRUE(result);ASSERT_EQ(1u,result->cell_errors.size());EXPECT_EQ(0u,result->cell_errors[0].row);EXPECT_EQ(0u,result->cell_errors[0].column);
      EXPECT_EQ("",*result->rows[0][0]);EXPECT_EQ("42",*result->rows[0][1]);
      EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output));EXPECT_EQ(drained,f.transport->offset);
      date_deadlines(*f.transport,deadline);EXPECT_TRUE(f.execute());
    } else {
      date_retired(f,result,fault==1?DbErrorCode::ProtocolError:(fault==2?DbErrorCode::QueryFailed:
          (fault<5 || fault==7?DbErrorCode::NetworkError:(fault==5 || fault==8?DbErrorCode::TLSError:DbErrorCode::Timeout))),deadline);
      if (fault<=2) { EXPECT_EQ((std::vector<unsigned>{22,23}),commands(f.transport->output)); }
      if (fault==5 || fault==6) { EXPECT_EQ((std::vector<unsigned>{22,25}),commands(f.transport->output)); }
      if (fault==7) { EXPECT_EQ((std::vector<unsigned>{22,23}),commands(f.transport->output)); }
      if (fault>=8) { EXPECT_EQ((std::vector<unsigned>{22,23,25}),commands(f.transport->output)); }
      if (mismatch || fault>=7) { EXPECT_EQ(drained,f.transport->offset); }
      EXPECT_LT(f.transport->offset,f.transport->input.size());
    }
  }
}

TEST(MySqlSessionTest, DateCharsetUnknownOrKnownWidthContradictionRetiresAtReceiptBeforeQueuedEof) {
  for (const unsigned charset:{0u,8u,45u,46u,63u,255u,65535u}) for (bool null:{false,true}) {
    Fixture f;
    const bool known=charset==45 || charset==63;
    Bytes first{std::byte{0}};number(first,17,4);number(first,2,2);number(first,1,2);number(first,0,1);number(first,0,2);
    append(*f.transport,first,1);
    // Known charset with the other charset's width is structural, not a
    // supported mismatch; unknown charset cannot borrow either width policy.
    append(*f.transport,column_packet("?",10,charset,0,charset==63?40:10),2);
    const auto rejected=f.transport->input.size();
    append(*f.transport,eof_packet(),3);append(*f.transport,date_column_packet(),4);
    append(*f.transport,column_packet("neighbor"),5);append(*f.transport,eof_packet(),6);
    date_parameter_result(*f.transport);
    const std::array params{Parameter{null?std::nullopt:std::optional<std::string>{"2000-02-29"},Hint::Date}};
    const auto deadline=rs::util::make_deadline(std::chrono::seconds(10));
    auto result=f.session->execute_prepared("SELECT ?",params,deadline);
    date_retired(f,result,known?DbErrorCode::ProtocolError:DbErrorCode::UnsupportedFeature,deadline);
    EXPECT_EQ((std::vector<unsigned>{22}),commands(f.transport->output));
    EXPECT_EQ(rejected,f.transport->offset);EXPECT_LT(rejected,f.transport->input.size());
  }
}
