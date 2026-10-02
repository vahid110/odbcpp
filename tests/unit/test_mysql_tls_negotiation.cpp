#include <gtest/gtest.h>
#include "core/database/mysql/tls_negotiation.h"

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
                      public rs::core::transport::IStartTlsTransport {
 public:
  Bytes input=packet(greeting()), output;
  std::size_t offset{}, chunk{1}, calls{}, closes{}, upgrades{};
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
    if (throw_io) throw std::runtime_error("test failure");
    if (fail_at==1) return {DbErrorCode::Timeout,"unsafe native detail"};
    if (zero || oversize || eof) return rs::core::transport::IOResult{oversize?out.size()+1:0,eof};
    const auto n=std::min({chunk,out.size(),input.size()-offset});
    std::copy_n(input.begin()+offset,n,out.begin()); offset+=n;
    return rs::core::transport::IOResult{n,false};
  }
  rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte> in, rs::util::Deadline dl) override {
    deadlines.push_back(dl);
    if (fail_at==2) return {DbErrorCode::NetworkError,"unsafe native detail"};
    if (bad_send>=0 && !output.empty()) return rs::core::transport::IOResult{bad_send==1?in.size()+1:0,bad_send==2};
    const auto n=std::min(chunk,in.size()); output.insert(output.end(),in.begin(),in.begin()+n);
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

class PlainOnlyTransport : public rs::core::transport::ITransport {
 public:
  std::size_t calls{}, closes{};
  rs::util::Result<void> connect(std::string_view,std::uint16_t,rs::util::Deadline) override {
    ++calls; return {};
  }
  rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte>,rs::util::Deadline) override {
    ++calls; return rs::core::transport::IOResult{};
  }
  rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte>,rs::util::Deadline) override {
    ++calls; return rs::core::transport::IOResult{};
  }
  void close() noexcept override { ++closes; }
};
TEST(MySqlTlsNegotiationTest, CustomTransportAndInvalidHostCannotBypassVerifiedTls) {
  PlainOnlyTransport plain;
  auto rejected=negotiate_verified_tls(plain,"localhost",3306,rs::util::Deadline::max());
  ASSERT_FALSE(rejected); EXPECT_EQ(DbErrorCode::UnsupportedFeature,rejected.error());
  EXPECT_EQ(0u,plain.calls); EXPECT_EQ(1u,plain.closes);
  for(const auto host:{std::string_view{},std::string_view("local\0host",10)}) {
    FakeTransport invalid;
    auto result=negotiate_verified_tls(invalid,host,3306,rs::util::Deadline::max());
    ASSERT_FALSE(result); EXPECT_EQ(DbErrorCode::InvalidParameter,result.error());
    EXPECT_TRUE(invalid.deadlines.empty()); EXPECT_EQ(1u,invalid.closes);
  }
}

auto run(FakeTransport& transport) {
  transport.expected=rs::util::make_deadline(std::chrono::seconds(10));
  return negotiate_verified_tls(transport,"localhost",3306,transport.expected);
}
TEST(MySqlTlsNegotiationTest, PartialIoKeepsOriginalDeadlineAndSendsOnlySslRequest) {
  for (const auto chunk : {std::size_t(1),std::size_t(3),std::size_t(65536)}) {
    FakeTransport t; t.chunk=chunk; t.input.push_back(std::byte{99});
    auto result=run(t); ASSERT_TRUE(result); EXPECT_EQ("8.4.11",result->greeting.server_version);
    EXPECT_EQ(0u,t.closes); EXPECT_EQ(1u,t.upgrades);
    EXPECT_EQ(t.input.size()-1,t.offset); // No speculative read across TLS boundary.
    auto expected=make_ssl_request(result->greeting); ASSERT_TRUE(expected);
    EXPECT_EQ(Bytes(expected->packet.begin(),expected->packet.end()),t.output);
    EXPECT_EQ(expected->capabilities,result->negotiated_capabilities);
    for (const auto dl:t.deadlines) EXPECT_EQ(t.expected,dl);
    t.close(); EXPECT_EQ(1u,t.closes); // Successful caller now owns cleanup.
  }
}
TEST(MySqlTlsNegotiationTest, AllTransportFailuresRetireWithoutLeakingNativeDetail) {
  for(int stage=0;stage<4;++stage) {
    FakeTransport t; t.fail_at=stage; auto result=run(t); ASSERT_FALSE(result);
    EXPECT_EQ("MySQL verified TLS negotiation failed",result.error_message()); EXPECT_EQ(1u,t.closes);
    if(stage<2) { EXPECT_TRUE(t.output.empty()); }
    if(stage<3) { EXPECT_EQ(0u,t.upgrades); }
  }
  FakeTransport unverified; unverified.verified=false;
  auto rejected=run(unverified); ASSERT_FALSE(rejected); EXPECT_EQ(DbErrorCode::TLSError,rejected.error());
  EXPECT_EQ(1u,unverified.closes);
  FakeTransport throwing; throwing.throw_io=true;
  EXPECT_THROW(run(throwing),std::runtime_error); EXPECT_EQ(1u,throwing.closes);
}
TEST(MySqlTlsNegotiationTest, BadProgressAndTruncatedGreetingFailClosed) {
  for(int mode=0;mode<3;++mode) {
    FakeTransport t; t.zero=mode==0;t.oversize=mode==1;t.eof=mode==2;
    EXPECT_FALSE(run(t)); EXPECT_EQ(1u,t.closes); EXPECT_TRUE(t.output.empty());
  }
  for(int mode=0;mode<3;++mode) {
    FakeTransport t; t.bad_send=mode;
    EXPECT_FALSE(run(t)); EXPECT_EQ(1u,t.closes); EXPECT_EQ(0u,t.upgrades);
    ASSERT_EQ(1u,t.output.size()); EXPECT_EQ(std::byte{32},t.output[0]);
  }
  const auto valid=packet(greeting());
  for(std::size_t size=0;size<valid.size();++size) {
    FakeTransport t; t.input.resize(size); EXPECT_FALSE(run(t));
    EXPECT_EQ(1u,t.closes); EXPECT_TRUE(t.output.empty()); EXPECT_EQ(0u,t.upgrades);
  }
}
TEST(MySqlTlsNegotiationTest, AdmissionAndHeaderChecksOccurBeforeTlsOrCredentialExchange) {
  for(int mode=0;mode<4;++mode) {
    FakeTransport t;
    if(mode==0) t.input[3]=std::byte{1};
    if(mode==1) {t.input[0]=std::byte{255};t.input[1]=std::byte{255};t.input[2]=std::byte{255};}
    if(mode==2) t.input[5]=std::byte{'9'}; // Not pinned server profile.
    if(mode==3) t.input=packet(greeting(client_protocol_41));
    EXPECT_FALSE(run(t)); EXPECT_TRUE(t.output.empty()); EXPECT_EQ(0u,t.upgrades); EXPECT_EQ(1u,t.closes);
  }
  FakeTransport expired;
  auto result=negotiate_verified_tls(expired,"localhost",3306,rs::util::Deadline::min());
  ASSERT_FALSE(result); EXPECT_EQ(DbErrorCode::Timeout,result.error()); EXPECT_TRUE(expired.deadlines.empty());
  EXPECT_EQ(1u,expired.closes);
  for (const auto port:{std::uint16_t(0)}) {
    FakeTransport invalid; EXPECT_FALSE(negotiate_verified_tls(invalid,"localhost",port,rs::util::Deadline::max()));
    EXPECT_TRUE(invalid.deadlines.empty()); EXPECT_EQ(1u,invalid.closes);
  }
}
}
