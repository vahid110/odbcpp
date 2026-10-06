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
