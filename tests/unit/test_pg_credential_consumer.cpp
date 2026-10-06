#include <gtest/gtest.h>
#include "odbcpp/auth/pg_credential_consumer.h"
#include "core/database/postgres/pg_database_connection.h"
#include "odbcpp/transport/start_tls_transport.h"
#include "odbcpp/transport/tls_configurable_transport.h"
#include <algorithm>
#include <cstring>
namespace rs::core::database::detail {
struct ConnectionAuthenticationTestAccess {
  static const std::string& password(const GenericDatabaseConnection& c) { return c.settings_.password; }
};
}
namespace {
namespace a=rs::core::auth;namespace d=rs::core::database;namespace t=rs::core::transport;
using Pg=d::postgres::PgDatabaseConnection;
class Wire final:public t::ITransport,public t::IStartTlsTransport,public t::ITlsConfigurableTransport {
 public:
  Wire(){reset();}
  void reset(bool rejected=false){
    input.clear();at=0;ssl=true;deadlines.clear();sent.clear();
    if(rejected){static constexpr char error[]="SFATAL\0C28P01\0Mfixed refusal\0";frame('E',{error,sizeof(error)});}
    else{frame('R',std::string_view("\0\0\0\3",4));frame('R',std::string_view("\0\0\0\0",4));frame('Z',"I");}
  }
  rs::util::Result<void> connect(std::string_view,std::uint16_t,rs::util::Deadline x) override{deadlines.push_back(x);return {};}
  rs::util::Result<void> connect_plain(std::string_view h,std::uint16_t p,rs::util::Deadline x) override{return connect(h,p,x);}
  rs::util::Result<void> upgrade_to_tls(std::string_view,rs::util::Deadline x) override{
    deadlines.push_back(x);if(tls_error)return {rs::util::DbErrorCode::TLSError,"fixed TLS refusal"};return {};
  }
  bool peer_identity_verified() noexcept override{return true;}
  void set_ca_locations(const std::string& f,const std::string& dir) override{++configured;ca=f;EXPECT_TRUE(dir.empty());}
  rs::util::Result<t::IOResult> send(std::span<const std::byte> bytes,rs::util::Deadline x) override{
    deadlines.push_back(x);if(throw_send)throw std::runtime_error("SYNTHETIC_SECRET");
    const auto n=std::min<std::size_t>(3,bytes.size());sent.insert(sent.end(),bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(n));return t::IOResult{n,false};
  }
  rs::util::Result<t::IOResult> recv(std::span<std::byte> out,rs::util::Deadline x) override{
    deadlines.push_back(x);if(ssl){ssl=false;out[0]=std::byte{'S'};return t::IOResult{1,false};}
    if(at==input.size()){return {rs::util::DbErrorCode::NetworkError,"fixed EOF"};}
    out[0]=input[at++];return t::IOResult{1,false};
  }
  void close() noexcept override{++closed;}
  void frame(char c,std::string_view p){input.push_back(std::byte(static_cast<unsigned char>(c)));auto n=static_cast<unsigned>(p.size()+4);for(int shift:{24,16,8,0})input.push_back(std::byte((n>>shift)&255));for(unsigned char b:p)input.push_back(std::byte(b));}
  unsigned configured{},closed{};bool tls_error{},throw_send{};std::string ca;
  std::vector<rs::util::Deadline> deadlines;std::vector<std::byte> sent,input;std::size_t at{};bool ssl{};
};
d::ConnectionSettings settings(){d::ConnectionSettings s;s.host="fixed.invalid";s.port=5439;s.database="db";s.user="IAM:user";s.ssl_ca_file="/synthetic/ca";s.timeout=std::chrono::milliseconds{0};return s;}
a::Request request(rs::util::Deadline end=rs::util::make_deadline(std::chrono::seconds{10}),a::Service service=a::Service::Redshift){
 auto b=a::Binding::create({service,"fixed.invalid",5439,"db","IAM:user","fixed-resource","fixed.invalid","verify-full"},{a::SourceKind::TrustedTemporaryDbIssuer,"issuer","generation"},a::Method::TemporaryDatabasePassword);
 if(!b){throw std::logic_error("fixture binding");}
 auto r=a::Request::create(std::move(b).value(),end,std::chrono::seconds{1});if(!r){throw std::logic_error("fixture request");}
 return std::move(r).value();
}
a::ExtractedDbFields fields(std::string_view password="synthetic-password"){
 auto secret=a::SecretBytes::create(std::as_bytes(std::span{password.data(),password.size()}));if(!secret){throw std::logic_error("fixture secret");}
 return {"IAM:user",std::move(secret).value(),a::UtcInstant{1}};
}
struct Fixture{Wire* wire;Pg pg;Fixture():Fixture(std::make_unique<Wire>()){}explicit Fixture(std::unique_ptr<Wire> w):wire(w.get()),pg(std::move(w),std::nullopt,d::postgres::PgCatalogProfile::Redshift){}};
}
TEST(PgCredentialConsumer, RealParserReusableOwningSuccessAndOriginalDeadline){
 Fixture f;auto r=request();auto s=settings();auto value=fields();auto result=a::connect_bound_temporary_db_until(f.pg,s,r,value);ASSERT_TRUE(result);
 EXPECT_EQ(result.session_snapshot(),(d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));EXPECT_TRUE(f.pg.is_connected());
 ASSERT_GT(f.wire->deadlines.size(),20U);for(auto observed:f.wire->deadlines)EXPECT_EQ(observed,r.deadline());EXPECT_EQ(f.wire->ca,s.ssl_ca_file);
 EXPECT_TRUE(s.password.empty());EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(f.pg).empty());EXPECT_EQ(value.password.size(),18U);
 const auto secret=std::as_bytes(std::span{std::string_view("synthetic-password")});
 EXPECT_TRUE(std::search(f.wire->sent.begin(),f.wire->sent.end(),secret.begin(),secret.end())!=f.wire->sent.end());
 const auto retained=result.session_snapshot();f.pg.disconnect();EXPECT_EQ(result.session_snapshot(),retained);EXPECT_FALSE(f.pg.is_connected());
}
TEST(PgCredentialConsumer, EverySelectedTargetAndTlsMismatchHasZeroIo){
 for(unsigned which=0;which<10;++which){Fixture f;auto s=settings();auto value=fields();switch(which){case 0:s.host="other";break;case 1:++s.port;break;case 2:s.database="other";break;case 3:s.user="other";break;case 4:value.user="other";break;case 5:s.use_ssl=false;break;case 6:s.ssl_ca_file.clear();break;case 7:s.ssl_ca_dir="other";break;case 8:s.password="fallback";break;case 9:s.ssl_ca_file=std::string("a\0b",3);break;}auto result=a::connect_bound_temporary_db_until(f.pg,s,request(),value);EXPECT_FALSE(result);EXPECT_TRUE(f.wire->deadlines.empty());EXPECT_EQ(f.wire->configured,0U);}
}
TEST(PgCredentialConsumer, InvalidMethodIdentityAndTlsIntentHaveZeroIo){
 for(unsigned which=0;which<3;++which){Fixture f;auto target=a::TargetInput{a::Service::Redshift,"fixed.invalid",5439,"db","IAM:user","resource","fixed.invalid","verify-full"};if(which==0)target.tls_identity="other";if(which==1)target.trust_policy="insecure";
 auto b=a::Binding::create(std::move(target),{which==2?a::SourceKind::ExternalPassword:a::SourceKind::TrustedTemporaryDbIssuer,"source","generation"},which==2?a::Method::OrdinaryPassword:a::Method::TemporaryDatabasePassword);ASSERT_TRUE(b);auto r=a::Request::create(std::move(b).value(),rs::util::make_deadline(std::chrono::seconds{10}),std::chrono::seconds{1});ASSERT_TRUE(r);auto value=fields();EXPECT_FALSE(a::connect_bound_temporary_db_until(f.pg,settings(),r.value(),value));EXPECT_TRUE(f.wire->deadlines.empty());}
}
TEST(PgCredentialConsumer, MissingNulAndMovedSecretRefuseThenRecover){
 Fixture f;auto s=settings();auto r=request();auto empty=fields("");EXPECT_FALSE(a::connect_bound_temporary_db_until(f.pg,s,r,empty));auto nul=fields(std::string_view("a\0b",3));EXPECT_FALSE(a::connect_bound_temporary_db_until(f.pg,s,r,nul));auto moved=fields();auto owned=std::move(moved.password);EXPECT_FALSE(a::connect_bound_temporary_db_until(f.pg,s,r,moved));EXPECT_TRUE(f.wire->deadlines.empty());auto valid=fields();EXPECT_TRUE(a::connect_bound_temporary_db_until(f.pg,s,r,valid));EXPECT_FALSE(owned.empty());
 const std::string large(a::SecretBytes::max_bytes+1,'x');EXPECT_FALSE(a::SecretBytes::create(std::as_bytes(std::span{large.data(),large.size()})));
}
TEST(PgCredentialConsumer, ExpiredOriginalDeadlineHasZeroIo){Fixture f;auto value=fields();auto r=request(rs::util::Clock::now()-std::chrono::seconds{1});auto result=a::connect_bound_temporary_db_until(f.pg,settings(),r,value);ASSERT_FALSE(result);EXPECT_EQ(result.backend_error().error_class,d::BackendErrorClass::Timeout);EXPECT_TRUE(f.wire->deadlines.empty());EXPECT_EQ(f.wire->configured,0U);}
TEST(PgCredentialConsumer, PreconnectedRefusalPreservesSessionAndNoAdditionalIo){Fixture f;auto r=request();auto s=settings();auto value=fields();ASSERT_TRUE(a::connect_bound_temporary_db_until(f.pg,s,r,value));auto calls=f.wire->deadlines.size();auto closed=f.wire->closed;auto result=a::connect_bound_temporary_db_until(f.pg,s,r,value);EXPECT_FALSE(result);EXPECT_TRUE(f.pg.is_connected());EXPECT_EQ(result.session_snapshot(),(d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));EXPECT_EQ(f.wire->deadlines.size(),calls);EXPECT_EQ(f.wire->closed,closed);}
TEST(PgCredentialConsumer, NativeAuthAndTlsFailuresRetainOwningBackendError){
 for(bool tls:{false,true}){Fixture f;if(tls)f.wire->tls_error=true;else f.wire->reset(true);auto value=fields();auto result=a::connect_bound_temporary_db_until(f.pg,settings(),request(),value);ASSERT_FALSE(result);EXPECT_EQ(result.backend_error().error_class,tls?d::BackendErrorClass::Tls:d::BackendErrorClass::Authentication);if(!tls){EXPECT_EQ(result.backend_error().native_state,std::optional<std::string>{"28P01"});}EXPECT_EQ(result.session_snapshot(),(d::SessionSnapshot{d::SessionState::Disconnected,d::SessionDisposition::Retire}));EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(f.pg).empty());auto error=result.backend_error();f.pg.disconnect();EXPECT_EQ(result.backend_error().message,error.message);}
}
TEST(PgCredentialConsumer, ExceptionCleansInternalCopyAndBackendCanReconnect){Fixture f;f.wire->throw_send=true;auto value=fields();auto r=request();auto result=a::connect_bound_temporary_db_until(f.pg,settings(),r,value);ASSERT_FALSE(result);EXPECT_EQ(result.error_message().find("SYNTHETIC_SECRET"),std::string::npos);EXPECT_FALSE(f.pg.is_connected());EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(f.pg).empty());f.wire->throw_send=false;f.wire->reset();ASSERT_TRUE(a::connect_bound_temporary_db_until(f.pg,settings(),r,value));f.pg.disconnect();EXPECT_FALSE(f.pg.is_connected());}

namespace {
a::Binding ordinary_binding(a::TargetInput target = {a::Service::PostgreSql,
    "fixed.invalid",5432,"db","ordinary_user","fixed-resource","fixed.invalid","verify-full"},
    a::SourceInput source = {a::SourceKind::ExternalPassword,"external-source","generation"}) {
  auto result=a::Binding::create(std::move(target),std::move(source),a::Method::OrdinaryPassword);
  if(!result) { throw std::logic_error("ordinary fixture binding"); }
  return std::move(result).value();
}
a::Request ordinary_request(const a::Binding& binding,
    rs::util::Deadline deadline=rs::util::make_deadline(std::chrono::seconds{10})) {
  auto result=a::Request::create(binding,deadline,std::chrono::seconds{1});
  if(!result) { throw std::logic_error("ordinary fixture request"); }
  return std::move(result).value();
}
a::SecretBytes ordinary_secret(std::string_view password="synthetic-password") {
  auto result=a::SecretBytes::create(std::as_bytes(std::span{password.data(),password.size()}));
  if(!result) { throw std::logic_error("ordinary fixture secret"); }
  return std::move(result).value();
}
a::Material ordinary_material(const a::Binding& binding,std::string principal="ordinary_user",
    std::string_view password="synthetic-password") {
  auto result=a::Material::create(binding,a::MaterialKind::OrdinaryPassword,
      std::move(principal),ordinary_secret(password),a::Validity::ordinary());
  if(!result) { throw std::logic_error("ordinary fixture material"); }
  return std::move(result).value();
}
d::ConnectionSettings ordinary_settings() {
  auto selected=settings();selected.port=5432;selected.user="ordinary_user";return selected;
}
class OrdinaryIssuer final:public a::TrustedIssuer {
public:
  a::Outcome<a::Material> acquire(const a::Request& requested,const a::Cancellation*) override {
    ++calls;return ordinary_material(requested.binding());
  }
  unsigned calls{};
};
class OrdinaryClock final:public a::MonotonicClock {
public:
  rs::util::Deadline now() noexcept override { return rs::util::Clock::now(); }
};
class OrdinaryNetworkFailure final:public t::ITransport,public t::IStartTlsTransport,
    public t::ITlsConfigurableTransport {
public:
  rs::util::Result<void> connect(std::string_view,std::uint16_t,rs::util::Deadline) override {
    return {rs::util::DbErrorCode::NetworkError,"fixed ordinary transport failure"};
  }
  rs::util::Result<t::IOResult> send(std::span<const std::byte>,rs::util::Deadline) override {
    return {rs::util::DbErrorCode::NetworkError,"unexpected write"};
  }
  rs::util::Result<t::IOResult> recv(std::span<std::byte>,rs::util::Deadline) override {
    return {rs::util::DbErrorCode::NetworkError,"unexpected read"};
  }
  rs::util::Result<void> connect_plain(std::string_view h,std::uint16_t p,rs::util::Deadline d) override {
    return connect(h,p,d);
  }
  rs::util::Result<void> upgrade_to_tls(std::string_view,rs::util::Deadline) override {
    return {rs::util::DbErrorCode::TLSError,"unexpected upgrade"};
  }
  bool peer_identity_verified() noexcept override { return true; }
  void set_ca_locations(const std::string&,const std::string&) override {}
  void close() noexcept override { ++closes; }
  unsigned closes{};
};
}

TEST(PgOrdinaryCredentialConsumer, ExplicitAuthorityMaterialToRealParserKeepsOriginalDeadline) {
  const auto binding=ordinary_binding();const auto requested=ordinary_request(binding);
  OrdinaryIssuer issuer;OrdinaryClock clock;
  auto authority=a::Authority::create(binding,issuer,clock);ASSERT_TRUE(authority);
  auto receipt=authority.value()->acquire(requested);ASSERT_TRUE(receipt);
  auto value=authority.value()->take(std::move(receipt).value());ASSERT_TRUE(value);
  EXPECT_EQ(1U,issuer.calls);
  auto transport=std::make_unique<Wire>();auto* wire=transport.get();Pg pg(std::move(transport));
  auto selected=ordinary_settings();
  const auto result=a::connect_bound_ordinary_password_until(pg,selected,requested,value.value());ASSERT_TRUE(result);
  EXPECT_EQ(result.session_snapshot(),(d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));
  ASSERT_GT(wire->deadlines.size(),20U);
  for(auto observed:wire->deadlines) { EXPECT_EQ(requested.deadline(),observed); }
  const auto bytes=std::as_bytes(std::span{std::string_view("synthetic-password")});
  EXPECT_TRUE(std::search(wire->sent.begin(),wire->sent.end(),bytes.begin(),bytes.end())!=wire->sent.end());
  EXPECT_TRUE(selected.password.empty());EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(pg).empty());
  value.value().with_secret([&](auto retained) { EXPECT_TRUE(std::equal(retained.begin(),retained.end(),bytes.begin(),bytes.end())); });
  pg.disconnect();EXPECT_FALSE(pg.is_connected());
  EXPECT_EQ(result.session_snapshot(),(d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));
}

TEST(PgOrdinaryCredentialConsumer, FullTargetSourceAndReturnedPrincipalMismatchHasZeroIo) {
  const auto binding=ordinary_binding();const auto requested=ordinary_request(binding);
  for(unsigned which=0;which<9;++which) {
    SCOPED_TRACE(which);auto target=binding.target();auto source=binding.source();std::string principal="ordinary_user";
    switch(which) {
      case 0:target.resource_id="other-resource";break;
      case 1:source.identity="other-source";break;
      case 2:source.generation="other-generation";break;
      case 3:target.service=a::Service::Redshift;break;
      case 4:target.database="other-db";break;
      case 5:target.endpoint="other.invalid";break;
      case 6:principal="other-returned-user";break;
      case 7:target.tls_identity="other.invalid";break;
      case 8:target.trust_policy="other-policy";break;
    }
    const auto foreign=ordinary_binding(std::move(target),std::move(source));auto value=ordinary_material(foreign,principal);
    Fixture f;auto result=a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,value);
    ASSERT_FALSE(result);EXPECT_EQ(d::BackendErrorClass::InvalidInput,result.backend_error().error_class);
    EXPECT_TRUE(f.wire->deadlines.empty());EXPECT_EQ(0U,f.wire->configured);
  }
}

TEST(PgOrdinaryCredentialConsumer, OrdinaryAndTemporaryApiMethodsNeverFallback) {
  Fixture f;const auto binding=ordinary_binding();auto requested=ordinary_request(binding);auto ordinary=ordinary_material(binding);
  auto temporary_request=request();auto secret=fields();
  auto temporary=a::Material::create(temporary_request.binding(),a::MaterialKind::TemporaryDatabasePassword,
      "IAM:user",std::move(secret.password),a::Validity::monotonic(rs::util::Clock::now(),temporary_request.deadline()+std::chrono::seconds{2}));
  ASSERT_TRUE(temporary);
  EXPECT_FALSE(a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,temporary.value()));
  EXPECT_FALSE(a::connect_bound_ordinary_password_until(f.pg,settings(),temporary_request,ordinary));
  auto raw=fields();EXPECT_FALSE(a::connect_bound_temporary_db_until(f.pg,settings(),requested,raw));
  EXPECT_TRUE(f.wire->deadlines.empty());EXPECT_EQ(0U,f.wire->configured);
}

TEST(PgOrdinaryCredentialConsumer, MovedOwnersRefuseAndClosedFactoryRejectsBadSecret) {
  const auto binding=ordinary_binding();auto requested=ordinary_request(binding);auto value=ordinary_material(binding);
  auto retained=std::move(value);Fixture f;
  EXPECT_FALSE(a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,value));
  auto consumed_request=std::move(requested);
  EXPECT_FALSE(a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,retained));
  EXPECT_TRUE(f.wire->deadlines.empty());EXPECT_EQ(0U,f.wire->configured);
  for(auto password:{std::string_view{},std::string_view{"a\0b",3}}) {
    EXPECT_FALSE(a::Material::create(binding,a::MaterialKind::OrdinaryPassword,"ordinary_user",
        ordinary_secret(password),a::Validity::ordinary()));
  }
  const std::string large(a::SecretBytes::max_bytes+1,'x');
  EXPECT_FALSE(a::SecretBytes::create(std::as_bytes(std::span{large.data(),large.size()})));
  ASSERT_TRUE(a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),consumed_request,retained));
}

TEST(PgOrdinaryCredentialConsumer, SelectedPolicyAndAlreadyOpenSessionArePreserved) {
  const auto binding=ordinary_binding();const auto requested=ordinary_request(binding);auto value=ordinary_material(binding);
  for(unsigned which=0;which<8;++which) {
    SCOPED_TRACE(which);Fixture f;auto selected=ordinary_settings();
    switch(which) {
      case 0:selected.use_ssl=false;break;
      case 1:selected.ssl_ca_file.clear();break;
      case 2:selected.ssl_ca_dir="other-ca";break;
      case 3:selected.ssl_ca_file=std::string("a\0b",3);break;
      case 4:selected.password="fallback";break;
      case 5:selected.user="other-user";break;
      case 6:++selected.port;break;
      case 7:selected.host="other.invalid";break;
    }
    EXPECT_FALSE(a::connect_bound_ordinary_password_until(f.pg,selected,requested,value));
    EXPECT_TRUE(f.wire->deadlines.empty());EXPECT_EQ(0U,f.wire->configured);
  }
  Fixture f;auto selected=ordinary_settings();ASSERT_TRUE(a::connect_bound_ordinary_password_until(f.pg,selected,requested,value));
  const auto calls=f.wire->deadlines.size();const auto closes=f.wire->closed;
  auto rejected=a::connect_bound_ordinary_password_until(f.pg,selected,requested,value);ASSERT_FALSE(rejected);
  EXPECT_TRUE(f.pg.is_connected());EXPECT_EQ(calls,f.wire->deadlines.size());EXPECT_EQ(closes,f.wire->closed);
  EXPECT_EQ(rejected.session_snapshot(),(d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}));
}

TEST(PgOrdinaryCredentialConsumer, OriginalExpiredAndShortValidDeadlinesDoNotReset) {
  const auto binding=ordinary_binding();auto value=ordinary_material(binding);Fixture f;
  const auto expired=ordinary_request(binding,rs::util::Clock::now()-std::chrono::seconds{1});
  auto result=a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),expired,value);ASSERT_FALSE(result);
  EXPECT_EQ(d::BackendErrorClass::Timeout,result.backend_error().error_class);EXPECT_TRUE(f.wire->deadlines.empty());EXPECT_EQ(0U,f.wire->configured);
  const auto short_request=ordinary_request(binding,rs::util::make_deadline(std::chrono::seconds{2}));
  ASSERT_TRUE(a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),short_request,value));
  for(auto observed:f.wire->deadlines) { EXPECT_EQ(short_request.deadline(),observed); }
}

TEST(PgOrdinaryCredentialConsumer, NativeTlsAndNetworkFailuresOwnErrorsAndRecoverFresh) {
  const auto binding=ordinary_binding();const auto requested=ordinary_request(binding);auto value=ordinary_material(binding);
  for(bool tls:{false,true}) {
    Fixture f;if(tls) { f.wire->tls_error=true; } else { f.wire->reset(true); }
    auto result=a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,value);ASSERT_FALSE(result);
    EXPECT_EQ(tls?d::BackendErrorClass::Tls:d::BackendErrorClass::Authentication,result.backend_error().error_class);
    if(!tls) { EXPECT_EQ(std::optional<std::string>{"28P01"},result.backend_error().native_state); }
    EXPECT_EQ(result.session_snapshot(),(d::SessionSnapshot{d::SessionState::Disconnected,d::SessionDisposition::Retire}));
    const auto retained=result.backend_error();EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(f.pg).empty());
    f.wire->tls_error=false;f.wire->reset();ASSERT_TRUE(a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,value));
    EXPECT_EQ(retained.message,result.backend_error().message);
  }
  auto transport=std::make_unique<OrdinaryNetworkFailure>();auto* wire=transport.get();Pg pg(std::move(transport));
  auto selected=ordinary_settings();
  auto network=a::connect_bound_ordinary_password_until(pg,selected,requested,value);ASSERT_FALSE(network);
  EXPECT_EQ(d::BackendErrorClass::Connection,network.backend_error().error_class);
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ConnectionFailed),network.error());
  EXPECT_EQ("fixed ordinary transport failure",network.error_message());
  EXPECT_EQ(network.session_snapshot(),(d::SessionSnapshot{d::SessionState::Disconnected,d::SessionDisposition::Retire}));EXPECT_GT(wire->closes,0U);
}

TEST(PgOrdinaryCredentialConsumer, ExceptionsActiveBorrowAndFailedOuterConsumptionDisconnect) {
  const auto binding=ordinary_binding();const auto requested=ordinary_request(binding);auto value=ordinary_material(binding);Fixture f;
  f.wire->throw_send=true;auto result=a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,value);ASSERT_FALSE(result);
  EXPECT_EQ(std::string::npos,result.error_message().find("SYNTHETIC_SECRET"));EXPECT_FALSE(f.pg.is_connected());
  EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(f.pg).empty());
  f.wire->throw_send=false;f.wire->reset();
  value.with_secret([&](auto){
    const auto calls=f.wire->deadlines.size();auto nested=a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,value);
    EXPECT_FALSE(nested);EXPECT_EQ(calls,f.wire->deadlines.size());EXPECT_FALSE(f.pg.is_connected());
  });
  ASSERT_TRUE(a::connect_bound_ordinary_password_until(f.pg,ordinary_settings(),requested,value));
  // Embedding duty, not a callback framework: a later outer refusal requires
  // the caller to retire this seemingly successful session without replay.
  const bool enclosing_observation_passed=false;
  if(!enclosing_observation_passed) { f.pg.disconnect(); }
  EXPECT_FALSE(f.pg.is_connected());EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(f.pg).empty());
  value.with_secret([&](auto bytes){EXPECT_EQ(18U,bytes.size());});
}

namespace {
d::ConnectionSettings driver_settings() {
  auto selected=ordinary_settings();selected.password="synthetic-password";
  selected.timeout=std::chrono::seconds{10};return selected;
}
// Minimal no-challenge wire for compatibility profiles, including plaintext.
// It never treats an unverified password exchange as success.
class DriverCompatibilityWire final:public t::ITransport,public t::IStartTlsTransport,
    public t::ITlsConfigurableTransport {
public:
  DriverCompatibilityWire() {
    const unsigned char literal[]{'R',0,0,0,8,0,0,0,0,'Z',0,0,0,5,'I'};
    for(auto byte:literal) { input.push_back(std::byte{byte}); }
  }
  rs::util::Result<void> connect(std::string_view,std::uint16_t,rs::util::Deadline end) override {
    deadlines.push_back(end);return {};
  }
  rs::util::Result<void> connect_plain(std::string_view host,std::uint16_t port,rs::util::Deadline end) override {
    return connect(host,port,end);
  }
  rs::util::Result<void> upgrade_to_tls(std::string_view,rs::util::Deadline end) override {
    deadlines.push_back(end);return {};
  }
  bool peer_identity_verified() noexcept override { return true; }
  void set_ca_locations(const std::string& file,const std::string& directory) override {
    ca=file;dir=directory;++configured;
  }
  rs::util::Result<t::IOResult> send(std::span<const std::byte> bytes,rs::util::Deadline end) override {
    deadlines.push_back(end);sent.insert(sent.end(),bytes.begin(),bytes.end());
    const unsigned char ssl[]{0,0,0,8,4,210,22,47};
    if(sent.size()==8) {
      ssl_pending=true;
      for(std::size_t i=0;i<8;++i) { ssl_pending=ssl_pending&&sent[i]==std::byte{ssl[i]}; }
    }
    return t::IOResult{bytes.size(),false};
  }
  rs::util::Result<t::IOResult> recv(std::span<std::byte> output,rs::util::Deadline end) override {
    deadlines.push_back(end);
    if(ssl_pending) { ssl_pending=false;output[0]=std::byte{'S'};return t::IOResult{1,false}; }
    if(offset==input.size()) { return {rs::util::DbErrorCode::NetworkError,"fixed EOF"}; }
    output[0]=input[offset++];return t::IOResult{1,false};
  }
  void close() noexcept override { ++closed; }
  std::vector<rs::util::Deadline> deadlines;std::vector<std::byte> sent,input;
  std::string ca,dir;unsigned configured{},closed{};std::size_t offset{};bool ssl_pending{};
};
}

TEST(PgDriverPasswordComposition, RealParserSelectedRouteKeepsOneDeadlineAndCallerPassword) {
  for(auto profile:{d::postgres::PgCatalogProfile::PostgreSQL,d::postgres::PgCatalogProfile::Redshift}) {
    auto wire=std::make_unique<Wire>();auto* observed=wire.get();Pg pg(std::move(wire),std::nullopt,profile);
    const auto selected=driver_settings();const auto before=rs::util::Clock::now()+selected.timeout;
    const auto result=pg.connect(selected);const auto after=rs::util::Clock::now()+selected.timeout;ASSERT_TRUE(result);
    EXPECT_EQ((d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}),result.session_snapshot());
    ASSERT_GT(observed->deadlines.size(),20U);const auto original=observed->deadlines.front();
    EXPECT_GE(original,before);EXPECT_LE(original,after);
    for(auto value:observed->deadlines) { EXPECT_EQ(original,value); }
    EXPECT_TRUE(selected.password=="synthetic-password");
    EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(pg).empty());
    EXPECT_EQ(profile==d::postgres::PgCatalogProfile::Redshift,pg.catalog_execution()!=nullptr);
    pg.disconnect();EXPECT_FALSE(pg.is_connected());
  }
}

TEST(PgDriverPasswordComposition, InvalidSelectedFactoriesAndNulHaveZeroIoThenRecover) {
  for(unsigned which=0;which<4;++which) {
    Fixture fixture;auto selected=driver_settings();
    if(which==0) { selected.user=std::string(1025,'x'); }
    if(which==1) { selected.host=std::string("h\0x",3); }
    if(which==2) { selected.password=std::string("p\0x",3); }
    if(which==3) { selected.ssl_ca_file=std::string("c\0x",3); }
    const auto failed=fixture.pg.connect(selected);ASSERT_FALSE(failed);
    EXPECT_EQ(d::BackendErrorClass::InvalidInput,failed.backend_error().error_class);
    if(which!=3) { EXPECT_TRUE(failed.error_message()=="Invalid ordinary driver password composition"); }
    EXPECT_TRUE(fixture.wire->deadlines.empty());EXPECT_EQ(0U,fixture.wire->configured);
    EXPECT_FALSE(fixture.pg.is_connected());
    EXPECT_TRUE(fixture.pg.connect(driver_settings()));fixture.pg.disconnect();
  }
}

TEST(PgDriverPasswordComposition, NativeFailuresRetireOwnErrorAndFreshConnectRecovers) {
  for(bool tls:{false,true}) {
    Fixture fixture;if(tls) { fixture.wire->tls_error=true; } else { fixture.wire->reset(true); }
    const auto failed=fixture.pg.connect(driver_settings());ASSERT_FALSE(failed);
    EXPECT_EQ(tls?d::BackendErrorClass::Tls:d::BackendErrorClass::Authentication,failed.backend_error().error_class);
    if(!tls) { EXPECT_EQ(std::optional<std::string>{"28P01"},failed.backend_error().native_state); }
    EXPECT_EQ((d::SessionSnapshot{d::SessionState::Disconnected,d::SessionDisposition::Retire}),failed.session_snapshot());
    auto retained=failed.backend_error();EXPECT_FALSE(fixture.pg.is_connected());
    EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(fixture.pg).empty());
    fixture.wire->tls_error=false;fixture.wire->reset();ASSERT_TRUE(fixture.pg.connect(driver_settings()));
    fixture.pg.disconnect();EXPECT_EQ(retained.code,failed.backend_error().code);
    EXPECT_EQ(retained.native_state,failed.backend_error().native_state);
    EXPECT_TRUE(retained.message==failed.backend_error().message);
  }
}

TEST(PgDriverPasswordComposition, AlreadyOpenRefusesWithoutIoOrRetirement) {
  Fixture fixture;ASSERT_TRUE(fixture.pg.connect(driver_settings()));
  const auto calls=fixture.wire->deadlines.size();const auto closes=fixture.wire->closed;
  const auto refused=fixture.pg.connect(driver_settings());ASSERT_FALSE(refused);
  EXPECT_EQ(d::BackendErrorClass::InvalidInput,refused.backend_error().error_class);
  EXPECT_EQ((d::SessionSnapshot{d::SessionState::Idle,d::SessionDisposition::Reusable}),refused.session_snapshot());
  EXPECT_TRUE(fixture.pg.is_connected());EXPECT_EQ(calls,fixture.wire->deadlines.size());EXPECT_EQ(closes,fixture.wire->closed);
  fixture.pg.disconnect();
}

TEST(PgDriverPasswordComposition, OutsideTlsAndPasswordProfilesRetainDirectRoute) {
  for(unsigned which=0;which<4;++which) {
    auto wire=std::make_unique<DriverCompatibilityWire>();auto* observed=wire.get();Pg pg(std::move(wire));auto selected=driver_settings();
    switch(which) {
      case 0:selected.ssl_ca_file.clear();break;
      case 1:selected.ssl_ca_file.clear();selected.ssl_ca_dir="/synthetic/directory";break;
      case 2:selected.use_ssl=false;selected.ssl_ca_file.clear();break;
      case 3:selected.password.clear();break;
    }
    const auto result=pg.connect(selected);ASSERT_TRUE(result);EXPECT_TRUE(pg.is_connected());
    EXPECT_EQ(selected.use_ssl?1U:0U,observed->configured);
    if(selected.use_ssl) { EXPECT_EQ(selected.ssl_ca_file,observed->ca);EXPECT_EQ(selected.ssl_ca_dir,observed->dir); }
    // Literal AuthenticationOk uses no password challenge on these profiles.
    EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(pg).empty());pg.disconnect();
  }
}

TEST(PgDriverPasswordComposition, ExactSecretBoundAndLargerPasswordKeepCompatibility) {
  for(auto size:{a::SecretBytes::max_bytes,a::SecretBytes::max_bytes+1}) {
    auto wire=std::make_unique<DriverCompatibilityWire>();Pg pg(std::move(wire));auto selected=driver_settings();
    selected.password.assign(size,'x');selected.input_limits.max_connection_field_bytes=128*1024;
    ASSERT_TRUE(pg.connect(selected));EXPECT_EQ(size,selected.password.size());pg.disconnect();
  }
  Fixture fixture;auto selected=driver_settings();selected.password.assign(a::SecretBytes::max_bytes+1,'x');
  selected.password[0]='\0';selected.input_limits.max_connection_field_bytes=128*1024;
  const auto refused=fixture.pg.connect(selected);ASSERT_FALSE(refused);
  EXPECT_TRUE(refused.error_message()=="PostgreSQL authentication credential contains an embedded NUL byte");
  EXPECT_TRUE(fixture.wire->deadlines.empty()); // Legacy validation, no secret cap/truncation or retry.
}

TEST(PgDriverPasswordComposition, FiniteExpiredAndSentinelDeadlineRoutesPreserved) {
  Fixture fixture;auto selected=driver_settings();selected.timeout=std::chrono::milliseconds{0};
  const auto expired=fixture.pg.connect(selected);ASSERT_FALSE(expired);
  EXPECT_EQ(d::BackendErrorClass::Timeout,expired.backend_error().error_class);EXPECT_TRUE(fixture.wire->deadlines.empty());
  EXPECT_EQ(0U,fixture.wire->configured);EXPECT_TRUE(expired.error_message()=="Ordinary password handoff deadline elapsed");
  Fixture negative;selected.timeout=std::chrono::milliseconds{-1};const auto refused=negative.pg.connect(selected);ASSERT_FALSE(refused);
  EXPECT_EQ(d::BackendErrorClass::Timeout,refused.backend_error().error_class);EXPECT_TRUE(negative.wire->deadlines.empty());
  EXPECT_TRUE(refused.error_message()!="Ordinary password handoff deadline elapsed");
  auto wire=std::make_unique<DriverCompatibilityWire>();auto* observed=wire.get();Pg pg(std::move(wire));
  selected.timeout=std::chrono::milliseconds::max();ASSERT_TRUE(pg.connect(selected));
  ASSERT_FALSE(observed->deadlines.empty());for(auto value:observed->deadlines) { EXPECT_EQ(rs::util::Deadline::max(),value); }
  pg.disconnect();
}

TEST(PgDriverPasswordComposition, ExplicitConnectUntilAndExceptionCleanupStayOwning) {
  Fixture direct;auto selected=driver_settings();selected.password=std::string("p\0x",3);
  const auto legacy=direct.pg.connect_until(selected,rs::util::make_deadline(std::chrono::seconds{10}));ASSERT_FALSE(legacy);
  EXPECT_TRUE(legacy.error_message()=="PostgreSQL authentication credential contains an embedded NUL byte");
  EXPECT_TRUE(direct.wire->deadlines.empty());
  Fixture limited;auto bounded=driver_settings();bounded.input_limits.max_startup_wire_bytes=1;
  const auto limited_result=limited.pg.connect(bounded);ASSERT_FALSE(limited_result);
  EXPECT_EQ(d::BackendErrorClass::ResourceLimit,limited_result.backend_error().error_class);
  EXPECT_TRUE(limited.wire->deadlines.empty());
  auto mode_wire=std::make_unique<Wire>();auto* mode_observed=mode_wire.get();Pg foreign_mode(std::move(mode_wire));
  auto mode=driver_settings();mode.redshift_catalog_mode=d::RedshiftCatalogMode::Legacy;
  const auto foreign_result=foreign_mode.connect(mode);ASSERT_FALSE(foreign_result);
  EXPECT_EQ(d::BackendErrorClass::InvalidInput,foreign_result.backend_error().error_class);
  EXPECT_TRUE(foreign_result.error_message()=="Invalid Redshift catalog mode for this provider");
  EXPECT_TRUE(mode_observed->deadlines.empty());
  Fixture fixture;fixture.wire->throw_send=true;const auto failed=fixture.pg.connect(driver_settings());ASSERT_FALSE(failed);
  EXPECT_EQ(std::string::npos,failed.error_message().find("SYNTHETIC_SECRET"));EXPECT_FALSE(fixture.pg.is_connected());
  EXPECT_TRUE(d::detail::ConnectionAuthenticationTestAccess::password(fixture.pg).empty());
  fixture.wire->throw_send=false;fixture.wire->reset();ASSERT_TRUE(fixture.pg.connect(driver_settings()));fixture.pg.disconnect();
}
