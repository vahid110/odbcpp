#include <gtest/gtest.h>
#include "odbcpp/auth/aws/provisioned_pg_connector.h"
#include "core/database/postgres/pg_database_connection.h"
#include "odbcpp/transport/start_tls_transport.h"
#include "odbcpp/transport/tls_configurable_transport.h"
#include <algorithm>
#include <functional>
#include <cstring>
namespace {
namespace a=rs::core::auth;namespace n=a::aws::provisioned_native;
namespace d=rs::core::database;namespace t=rs::core::transport;
using Pg=d::postgres::PgDatabaseConnection;
class Observer final:public a::ResponseObservationSource {
public:
  rs::util::Deadline now=rs::util::Clock::now(),end{};
  unsigned reads{},after_reads{},late_on{};bool ready{};
  a::ResponseClockRead read_monotonic()override {
    ++reads;if(ready && ++after_reads==late_on)return end;return now;
  }
  bool cancellation_requested()override{return false;}
};
struct Source {
  n::Context context=n::FixedFixture::context();
  std::shared_ptr<Observer> observer=std::make_shared<Observer>();
  std::shared_ptr<n::WorkerCancellation> cancel=std::make_shared<n::WorkerCancellation>();
  rs::util::Deadline deadline=observer->now+std::chrono::seconds{20};
  std::shared_ptr<a::ResponseSourceGeneration> generation;
  Source(){observer->end=deadline;auto made=a::ResponseSourceGeneration::create(n::FixedFixture::request(context).binding());
    if(!std::holds_alternative<std::shared_ptr<a::ResponseSourceGeneration>>(made))std::terminate();
    generation=std::get<std::shared_ptr<a::ResponseSourceGeneration>>(std::move(made));}
  n::CreateOutcome make(n::FixedCase scenario=n::FixedCase::Happy) {
    return n::FixedFixture::create_for_connector(scenario,deadline,observer,generation,cancel);
  }
  d::ConnectionSettings settings()const {
    d::ConnectionSettings s;s.host=context.db_endpoint;s.port=5439;s.database=context.database;
    s.user=context.expected_user;s.ssl_ca_file="/synthetic/ca";s.timeout=std::chrono::milliseconds{0};return s;
  }
};
class Wire final:public t::ITransport,public t::IStartTlsTransport,public t::ITlsConfigurableTransport {
public:
  explicit Wire(bool rejected=false){
    if(rejected){static constexpr char error[]="SFATAL\0C28P01\0Mfixed refusal\0";frame('E',{error,sizeof(error)});}
    else{frame('R',std::string_view("\0\0\0\3",4));frame('R',std::string_view("\0\0\0\0",4));frame('Z',"I");}
  }
  void frame(char tag,std::string_view bytes){input.push_back(std::byte(static_cast<unsigned char>(tag)));unsigned count=static_cast<unsigned>(bytes.size()+4);
    for(int shift:{24,16,8,0})input.push_back(std::byte((count>>shift)&255));for(unsigned char b:bytes)input.push_back(std::byte(b));}
  rs::util::Result<void> connect(std::string_view,std::uint16_t,rs::util::Deadline end)override{deadlines.push_back(end);return {};}
  rs::util::Result<void> connect_plain(std::string_view h,std::uint16_t p,rs::util::Deadline end)override{return connect(h,p,end);}
  rs::util::Result<void> upgrade_to_tls(std::string_view,rs::util::Deadline end)override{
    deadlines.push_back(end);if(tls_error)return {rs::util::DbErrorCode::TLSError,"fixed TLS refusal"};return {};}
  bool peer_identity_verified()noexcept override{return true;}
  void set_ca_locations(const std::string& file,const std::string& directory)override{++configured;ca=file;EXPECT_TRUE(directory.empty());}
  rs::util::Result<t::IOResult> send(std::span<const std::byte> bytes,rs::util::Deadline end)override{
    deadlines.push_back(end);if(throw_send)throw std::runtime_error("SYNTHETIC_SECRET");
    const auto count=std::min<std::size_t>(3,bytes.size());sent.insert(sent.end(),bytes.begin(),bytes.begin()+static_cast<std::ptrdiff_t>(count));return t::IOResult{count,false};}
  rs::util::Result<t::IOResult> recv(std::span<std::byte> bytes,rs::util::Deadline end)override{
    deadlines.push_back(end);if(ssl){ssl=false;bytes[0]=std::byte{'S'};return t::IOResult{1,false};}
    if(at==input.size())return {rs::util::DbErrorCode::NetworkError,"fixed EOF"};
    bytes[0]=input[at++];if(at==input.size()&&completed)completed();return t::IOResult{1,false};}
  void close()noexcept override{++closed;}
  std::vector<std::byte> input,sent;std::vector<rs::util::Deadline> deadlines;
  std::function<void()> completed;std::string ca;std::size_t at{};unsigned closed{},configured{};
  bool ssl{true},tls_error{},throw_send{};
};
struct Session {
  Wire* wire;Pg pg;
  explicit Session(bool rejected=false):Session(std::make_unique<Wire>(rejected)){}
  explicit Session(std::unique_ptr<Wire> w):wire(w.get()),pg(std::move(w),std::nullopt,d::postgres::PgCatalogProfile::Redshift){}
};
void quiescent(const n::ConnectorOutcome& out){EXPECT_TRUE(out.observation_closed);EXPECT_TRUE(out.counts.unknown_only);
  EXPECT_EQ(out.counts.selected_sends,1U);EXPECT_EQ(out.counts.shutdowns,1U);EXPECT_EQ(out.counts.active,0U);EXPECT_EQ(out.counts.wrappers,out.counts.destroyed_wrappers);}
}
TEST(ProvisionedPgConnector, ExactOriginalRequestOwningSuccessAndWireDeadline){
  Source source;auto created=source.make();ASSERT_TRUE(created.owner);auto acquired=created.owner->acquire_observation();ASSERT_TRUE(acquired.observation);
  const auto* original=acquired.observation->bound_request();ASSERT_NE(original,nullptr);EXPECT_EQ(original->deadline(),source.deadline);EXPECT_EQ(original->headroom(),std::chrono::seconds{1});
  Session s;const auto out=n::connect_provisioned_observation_until(s.pg,source.settings(),*acquired.observation);
  ASSERT_TRUE(out.succeeded());ASSERT_TRUE(out.connection);EXPECT_TRUE(out.entered);EXPECT_TRUE(out.wire_identity);EXPECT_TRUE(out.password_present);EXPECT_TRUE(out.raw_expiry_positive);quiescent(out);
  EXPECT_EQ(out.counts.n_samples,1U);EXPECT_EQ(acquired.observation->bound_request(),nullptr);EXPECT_TRUE(s.pg.is_connected());
  ASSERT_GT(s.wire->deadlines.size(),20U);for(auto end:s.wire->deadlines)EXPECT_EQ(end,source.deadline);
  const std::string expected="synthetic-only";const auto bytes=std::as_bytes(std::span{expected.data(),expected.size()});
  EXPECT_NE(std::search(s.wire->sent.begin(),s.wire->sent.end(),bytes.begin(),bytes.end()),s.wire->sent.end());
  const auto snapshot=out.connection->session_snapshot();EXPECT_TRUE(created.owner->close());s.pg.disconnect();EXPECT_EQ(out.connection->session_snapshot(),snapshot);
}
TEST(ProvisionedPgConnector, TargetTlsAndCompetingPasswordRefuseBeforePgIo){
  for(unsigned which=0;which<9;++which){SCOPED_TRACE(which);Source source;auto created=source.make();ASSERT_TRUE(created.owner);auto acquired=created.owner->acquire_observation();ASSERT_TRUE(acquired.observation);
    auto settings=source.settings();switch(which){case 0:settings.host="other.invalid";break;case 1:++settings.port;break;case 2:settings.database="other";break;case 3:settings.user="other";break;case 4:settings.use_ssl=false;break;case 5:settings.ssl_ca_file.clear();break;case 6:settings.ssl_ca_dir="other";break;case 7:settings.password="fallback";break;case 8:settings.ssl_ca_file=std::string("a\0b",3);break;}
    Session s;auto out=n::connect_provisioned_observation_until(s.pg,settings,*acquired.observation);EXPECT_FALSE(out.succeeded());EXPECT_EQ(out.failure,n::ConnectorFailure::ConnectionRejected);ASSERT_TRUE(out.connection);EXPECT_TRUE(out.connection->has_error());
    EXPECT_TRUE(s.wire->deadlines.empty());EXPECT_EQ(s.wire->configured,0U);quiescent(out);EXPECT_TRUE(created.owner->close());}
}
TEST(ProvisionedPgConnector, MoveClosedAndSingleConsumptionCannotReuseFields){
  Source source;auto created=source.make();ASSERT_TRUE(created.owner);auto acquired=created.owner->acquire_observation();ASSERT_TRUE(acquired.observation);
  n::Observation moved{std::move(*acquired.observation)};EXPECT_EQ(acquired.observation->bound_request(),nullptr);Session s;
  const auto reads=source.observer->reads;auto invalid=n::connect_provisioned_observation_until(s.pg,source.settings(),*acquired.observation);
  EXPECT_EQ(invalid.failure,n::ConnectorFailure::InvalidObservation);EXPECT_EQ(source.observer->reads,reads);EXPECT_TRUE(s.wire->deadlines.empty());EXPECT_FALSE(created.owner->close());
  auto out=n::connect_provisioned_observation_until(s.pg,source.settings(),moved);ASSERT_TRUE(out.succeeded());auto count=s.wire->deadlines.size();
  auto repeated=n::connect_provisioned_observation_until(s.pg,source.settings(),moved);EXPECT_EQ(repeated.failure,n::ConnectorFailure::InvalidObservation);EXPECT_EQ(s.wire->deadlines.size(),count);EXPECT_TRUE(s.pg.is_connected());
  EXPECT_EQ(created.owner->acquire_observation().failure,n::Failure::Consumed);EXPECT_TRUE(created.owner->close());s.pg.disconnect();
}
TEST(ProvisionedPgConnector, ProviderRefusalHasNoFieldsAndNoPgDispatch){
  Source source;auto created=source.make(n::FixedCase::ServiceDenied);ASSERT_TRUE(created.owner);Session s;auto acquired=created.owner->acquire_observation();
  EXPECT_FALSE(acquired.observation);EXPECT_TRUE(acquired.failure);EXPECT_EQ(acquired.counts.provider_failure,n::ProviderFailure::AccessDenied);
  EXPECT_EQ(acquired.counts.selected_sends,1U);EXPECT_EQ(acquired.counts.model_bytes,0U);EXPECT_TRUE(s.wire->deadlines.empty());EXPECT_TRUE(created.owner->close());
}
TEST(ProvisionedPgConnector, BeforeCallbackFaultsStayPermanentWithoutPgIo){
  for(unsigned kind=0;kind<4;++kind){SCOPED_TRACE(kind);Source source;auto created=source.make();ASSERT_TRUE(created.owner);auto acquired=created.owner->acquire_observation();ASSERT_TRUE(acquired.observation);
    auto expected=a::BoundaryFailure::DeadlineElapsed;
    if(kind==0)source.observer->now=source.deadline;
    if(kind==1){source.cancel->cancel();expected=a::BoundaryFailure::Cancelled;}
    if(kind==2){source.observer->now-=std::chrono::seconds{1};expected=a::BoundaryFailure::ClockRollback;}
    if(kind==3){(void)source.generation->retire(a::SourceRetirement::Closed);expected=a::BoundaryFailure::SourceClosed;}
    Session s;auto out=n::connect_provisioned_observation_until(s.pg,source.settings(),*acquired.observation);EXPECT_FALSE(out.succeeded());EXPECT_FALSE(out.entered);EXPECT_FALSE(out.connection);EXPECT_EQ(out.counts.boundary,expected);EXPECT_EQ(out.counts.n_samples,0U);EXPECT_TRUE(s.wire->deadlines.empty());quiescent(out);
    const auto reads=source.observer->reads;source.observer->now=rs::util::Clock::now();source.cancel->cancel();auto again=n::connect_provisioned_observation_until(s.pg,source.settings(),*acquired.observation);
    EXPECT_FALSE(again.succeeded());EXPECT_EQ(again.counts.boundary,expected);EXPECT_EQ(source.observer->reads,reads);EXPECT_TRUE(created.owner->close());}
}
TEST(ProvisionedPgConnector, BackendNativeAndTlsErrorsRemainOwningAndFreshRecoveryWorks){
  for(bool tls:{false,true}){Source source;auto created=source.make();ASSERT_TRUE(created.owner);auto acquired=created.owner->acquire_observation();ASSERT_TRUE(acquired.observation);
    Session s(!tls);s.wire->tls_error=tls;auto out=n::connect_provisioned_observation_until(s.pg,source.settings(),*acquired.observation);ASSERT_TRUE(out.connection);ASSERT_TRUE(out.connection->has_error());EXPECT_FALSE(out.succeeded());EXPECT_FALSE(s.pg.is_connected());EXPECT_GT(s.wire->closed,0U);
    if(tls){EXPECT_EQ(out.connection->backend_error().code,rs::util::make_error_code(rs::util::DbErrorCode::TLSError));}else{EXPECT_EQ(out.connection->backend_error().native_state,"28P01");}
    const auto error=out.connection->backend_error();EXPECT_TRUE(created.owner->close());EXPECT_EQ(out.connection->backend_error().code,error.code);EXPECT_EQ(out.connection->backend_error().native_state,error.native_state);}
  Source source;auto created=source.make();ASSERT_TRUE(created.owner);auto acquired=created.owner->acquire_observation();ASSERT_TRUE(acquired.observation);Session recovered;
  auto good=n::connect_provisioned_observation_until(recovered.pg,source.settings(),*acquired.observation);EXPECT_TRUE(good.succeeded());EXPECT_TRUE(created.owner->close());recovered.pg.disconnect();
}
TEST(ProvisionedPgConnector, PostConnectOrFinalCloseFailureDisconnectsSuccessfulBackend){
  for(unsigned late:{1U,2U}){SCOPED_TRACE(late);Source source;auto created=source.make();ASSERT_TRUE(created.owner);auto acquired=created.owner->acquire_observation();ASSERT_TRUE(acquired.observation);Session s;
    source.observer->late_on=late;s.wire->completed=[&]{source.observer->ready=true;};
    auto out=n::connect_provisioned_observation_until(s.pg,source.settings(),*acquired.observation);ASSERT_TRUE(out.connection);ASSERT_TRUE(out.connection->has_value());EXPECT_FALSE(out.succeeded());EXPECT_EQ(out.failure,n::ConnectorFailure::ObservationRejected);
    EXPECT_EQ(out.counts.boundary,a::BoundaryFailure::DeadlineElapsed);EXPECT_EQ(out.counts.n_samples,0U);EXPECT_FALSE(s.pg.is_connected());EXPECT_GT(s.wire->closed,0U);EXPECT_EQ(out.borrow_passed,late==2);quiescent(out);EXPECT_TRUE(created.owner->close());}
}
TEST(ProvisionedPgConnector, ExceptionRedactionAndPostErrorFaultPreservePrimaryBackendResult){
  Source source;auto created=source.make();ASSERT_TRUE(created.owner);auto acquired=created.owner->acquire_observation();ASSERT_TRUE(acquired.observation);Session s;s.wire->throw_send=true;
  auto out=n::connect_provisioned_observation_until(s.pg,source.settings(),*acquired.observation);ASSERT_TRUE(out.connection);ASSERT_TRUE(out.connection->has_error());EXPECT_FALSE(out.succeeded());EXPECT_EQ(out.connection->backend_error().message.find("SYNTHETIC_SECRET"),std::string::npos);EXPECT_FALSE(s.pg.is_connected());quiescent(out);EXPECT_TRUE(created.owner->close());
  Source second;auto owner=second.make();ASSERT_TRUE(owner.owner);auto result=owner.owner->acquire_observation();ASSERT_TRUE(result.observation);Session rejected(true);
  const auto error_read=[&]{second.cancel->cancel();};rejected.wire->completed=error_read;
  auto both=n::connect_provisioned_observation_until(rejected.pg,second.settings(),*result.observation);ASSERT_TRUE(both.connection);ASSERT_TRUE(both.connection->has_error());EXPECT_EQ(both.connection->backend_error().native_state,"28P01");EXPECT_EQ(both.counts.boundary,a::BoundaryFailure::Cancelled);EXPECT_EQ(both.failure,n::ConnectorFailure::ConnectionRejected);EXPECT_TRUE(owner.owner->close());
}
