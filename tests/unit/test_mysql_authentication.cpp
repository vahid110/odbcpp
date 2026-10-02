#include <gtest/gtest.h>
#include "core/database/mysql/authentication.h"

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


Bytes ok() { return Bytes{std::byte{0},std::byte{0},std::byte{0},std::byte{2},std::byte{0},std::byte{0},std::byte{0}}; }
void append(FakeTransport& t, const Bytes& payload, std::uint8_t sequence) {
  auto next=packet(payload,sequence);t.input.insert(t.input.end(),next.begin(),next.end());
}
auto authenticate(FakeTransport& t,std::string_view password="secret") {
  t.expected=rs::util::make_deadline(std::chrono::seconds(10));
  return authenticate_verified_tls(t,"localhost",3306,"sdk",password,t.expected);
}
TEST(MySqlAuthenticationTest, FullAndCachedPathsRequireFinalOkAndExactSequences) {
  for(const auto path:{AuthenticationPath::Immediate,AuthenticationPath::Cached,AuthenticationPath::FullOverTls}) {
    FakeTransport t;
    if(path!=AuthenticationPath::Immediate) append(t,{std::byte{1},path==AuthenticationPath::Cached?std::byte{3}:std::byte{4}},3);
    append(t,ok(),path==AuthenticationPath::Immediate?3:path==AuthenticationPath::Cached?4:5);
    auto result=authenticate(t);ASSERT_TRUE(result);EXPECT_EQ(path,result->path);EXPECT_EQ(0u,t.closes);
    EXPECT_EQ(t.input.size(),t.offset);for(const auto dl:t.deadlines) EXPECT_EQ(t.expected,dl);
    ASSERT_GE(t.output.size(),36+4+32+4+1+32+21);
    EXPECT_EQ(std::byte{2},t.output[39]);
    EXPECT_TRUE(std::equal(t.output.begin()+4,t.output.begin()+36,t.output.begin()+40));
    // Username plus one-byte scramble length, with no plaintext in response41.
    EXPECT_EQ(std::byte{'s'},t.output[72]);EXPECT_EQ(std::byte{32},t.output[76]);
    const auto response_size=std::to_integer<std::size_t>(t.output[36]) | (std::to_integer<std::size_t>(t.output[37])<<8);
    const auto end=36+4+response_size;
    EXPECT_EQ(path==AuthenticationPath::FullOverTls?end+11:end,t.output.size());
    if(path==AuthenticationPath::FullOverTls) {
      EXPECT_EQ(std::byte{4},t.output[end+3]);
      EXPECT_EQ(std::byte{0},t.output.back());
      EXPECT_EQ(std::string("secret"),std::string(reinterpret_cast<const char*>(t.output.data()+end+4),6));
    }
    t.close();
  }
}
TEST(MySqlAuthenticationTest, EmptyPasswordAndBoundedInputs) {
  FakeTransport empty;append(empty,ok(),3);EXPECT_TRUE(authenticate(empty,""));
  for(const auto& name:{std::string{},std::string(257,'x'),std::string("a\0b",3)}) {
    FakeTransport t;EXPECT_FALSE(authenticate_verified_tls(t,"localhost",3306,name,"secret",rs::util::Deadline::max()));
    EXPECT_TRUE(t.deadlines.empty());EXPECT_EQ(1u,t.closes);
  }
  for(const auto& password:{std::string(65536,'x'),std::string("a\0b",3)}) {
    FakeTransport t;EXPECT_FALSE(authenticate(t,password));EXPECT_TRUE(t.deadlines.empty());EXPECT_EQ(1u,t.closes);
  }
}
TEST(MySqlAuthenticationTest, FailedOrMalformedExchangesNeverPublishAuthenticatedState) {
  for(const auto& reply:{Bytes{std::byte{255},std::byte{21},std::byte{4}},Bytes{std::byte{1},std::byte{3}},
      Bytes{std::byte{1},std::byte{4}},Bytes{std::byte{1},std::byte{5}},Bytes{std::byte{254}},
      Bytes{std::byte{0}},Bytes{std::byte{0},std::byte{0},std::byte{0},std::byte{0}},
      Bytes{std::byte{0},std::byte{1},std::byte{0},std::byte{2},std::byte{0},std::byte{0},std::byte{0}}}) {
    FakeTransport t;append(t,reply,3);auto rejected=authenticate(t);EXPECT_FALSE(rejected);EXPECT_EQ(1u,t.closes);
    EXPECT_EQ("MySQL authentication failed",rejected.error_message());
  }
  FakeTransport wrong_sequence;append(wrong_sequence,ok(),4);EXPECT_FALSE(authenticate(wrong_sequence));EXPECT_EQ(1u,wrong_sequence.closes);
  const auto valid=packet(ok(),3);
  for(std::size_t n=0;n<valid.size();++n) {
    FakeTransport t;t.input.insert(t.input.end(),valid.begin(),valid.begin()+n);
    EXPECT_FALSE(authenticate(t));EXPECT_EQ(1u,t.closes);
  }
  FakeTransport repeat;append(repeat,{std::byte{1},std::byte{3}},3);append(repeat,{std::byte{1},std::byte{4}},4);
  EXPECT_FALSE(authenticate(repeat));EXPECT_EQ(1u,repeat.closes);
}
TEST(MySqlAuthenticationTest, SecretPacketIsMoveAndCopyRestricted) {
  static_assert(!std::is_copy_constructible_v<authentication_detail::SecretPacket>);
  static_assert(!std::is_move_constructible_v<authentication_detail::SecretPacket>);
  FakeTransport t;t.verified=false;EXPECT_FALSE(authenticate(t));
  EXPECT_EQ(36u,t.output.size());EXPECT_EQ(1u,t.closes); // SSLRequest only.
}
class FaultTransport : public FakeTransport {
 public:
  std::size_t threshold{36}; int fault{-1}; bool lose_peer{}, receive_error{};
  bool peer_identity_verified() noexcept override { return verified && !(lose_peer && output.size()>=threshold); }
  rs::util::Result<rs::core::transport::IOResult> send(std::span<const std::byte> bytes,rs::util::Deadline dl) override {
    if(output.size()>=threshold) {
      if(fault==0) return rs::core::transport::IOResult{0,false};
      if(fault==1) return rs::core::transport::IOResult{bytes.size()+1,false};
      if(fault==2) return rs::core::transport::IOResult{0,true};
      if(fault==3) throw std::runtime_error("unsafe native detail");
      if(fault==4) return {DbErrorCode::Timeout,"unsafe native detail"};
    }
    return FakeTransport::send(bytes,dl);
  }
  rs::util::Result<rs::core::transport::IOResult> recv(std::span<std::byte> bytes,rs::util::Deadline dl) override {
    if(receive_error && offset>=packet(greeting()).size()) throw std::runtime_error("unsafe native detail");
    return FakeTransport::recv(bytes,dl);
  }
};
TEST(MySqlAuthenticationTest, PeerLossStopsEveryCredentialWriteIncludingPartialProgress) {
  for(const auto threshold:{std::size_t(36),std::size_t(37),std::size_t(130),std::size_t(131)}) {
    FaultTransport t;t.threshold=threshold;t.lose_peer=true;
    append(t,{std::byte{1},std::byte{4}},3);append(t,ok(),5);
    auto result=authenticate(t);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::TLSError,result.error());
    EXPECT_EQ(threshold,t.output.size());EXPECT_EQ(1u,t.closes);
  }
}
TEST(MySqlAuthenticationTest, AuthStageBadIoAndExceptionsRetireExactlyOnce) {
  for(const auto threshold:{std::size_t(37),std::size_t(131)}) {
    for(int fault=0;fault<5;++fault) {
      FaultTransport t;t.threshold=threshold;t.fault=fault;
      append(t,{std::byte{1},std::byte{4}},3);append(t,ok(),5);
      if(fault==3) { EXPECT_THROW(authenticate(t),std::runtime_error); }
      else {
        auto rejected=authenticate(t);ASSERT_FALSE(rejected);
        EXPECT_EQ(fault==1?DbErrorCode::ProtocolError:fault==4?DbErrorCode::Timeout:DbErrorCode::NetworkError,rejected.error());
        EXPECT_EQ("MySQL authentication failed",rejected.error_message());
      }
      EXPECT_EQ(threshold,t.output.size());EXPECT_EQ(1u,t.closes);
    }
  }
  FakeTransport negotiation;negotiation.throw_io=true;
  EXPECT_THROW(authenticate(negotiation),std::runtime_error);EXPECT_EQ(1u,negotiation.closes);
  FaultTransport reading;reading.receive_error=true;
  EXPECT_THROW(authenticate(reading),std::runtime_error);EXPECT_EQ(1u,reading.closes);
}
TEST(MySqlAuthenticationTest, RejectionClassificationDoesNotEchoServerBytes) {
  const std::vector<std::pair<Bytes,DbErrorCode>> cases{
    {{std::byte{255},std::byte{21},std::byte{4},std::byte{'#'},std::byte{'2'},std::byte{'8'},std::byte{'0'},std::byte{'0'},std::byte{'0'},std::byte{'!'}},DbErrorCode::AuthenticationFailed},
    {{std::byte{255}},DbErrorCode::ProtocolError},
    {{std::byte{1},std::byte{5}},DbErrorCode::UnsupportedFeature},
    {{std::byte{254}},DbErrorCode::UnsupportedFeature},
    {{std::byte{0}},DbErrorCode::ProtocolError},
  };
  for(const auto& [reply,code]:cases) {
    FakeTransport t;append(t,reply,3);auto result=authenticate(t);ASSERT_FALSE(result);
    EXPECT_EQ(code,result.error());EXPECT_EQ("MySQL authentication failed",result.error_message());EXPECT_EQ(1u,t.closes);
  }
}

TEST(MySqlAuthenticationTest, ProtocolActiveStatusAndWarningsCannotPublishSession) {
  for (const unsigned status:{1u,8u,0x40u,0x80u,0x1000u,0x4000u}) {
    FakeTransport t;auto reply=ok();reply[3]=static_cast<std::byte>(status&255);reply[4]=static_cast<std::byte>(status>>8);
    append(t,reply,3);auto result=authenticate(t);ASSERT_FALSE(result);EXPECT_EQ(DbErrorCode::UnsupportedFeature,result.error());EXPECT_EQ(1u,t.closes);
  }
  FakeTransport t;auto reply=ok();reply[5]=std::byte{1};append(t,reply,3);EXPECT_FALSE(authenticate(t));EXPECT_EQ(1u,t.closes);
}
}
