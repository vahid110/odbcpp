#include <gtest/gtest.h>
#include "tests/support/pg_staged_refusal_observer.h"
#include "core/transport/async_transport.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <thread>
#include <vector>

namespace {
using rs::tests::support::PgStagedRefusalObserver;
using Observer=PgStagedRefusalObserver;
using rs::core::transport::ITransport;
using rs::core::transport::IStartTlsTransport;
using rs::core::transport::ITlsConfigurableTransport;
using rs::core::transport::IOResult;
using rs::util::Deadline;
using rs::util::DbErrorCode;
struct Calls {
  unsigned connect{},plain{},upgrade{},ca{},send{},recv{},close{},destroy{};
  std::string host,file,dir;std::uint16_t port{};Deadline deadline{};
};
class Bare final:public ITransport {
public:
 explicit Bare(std::shared_ptr<Calls> c):c_(std::move(c)){}~Bare()override{++c_->destroy;}
 rs::util::Result<void> connect(std::string_view,std::uint16_t,Deadline)override{++c_->connect;return {};}
 rs::util::Result<IOResult> send(std::span<const std::byte>,Deadline)override{++c_->send;return IOResult{};}
 rs::util::Result<IOResult> recv(std::span<std::byte>,Deadline)override{++c_->recv;return IOResult{};}
 void close()noexcept override{++c_->close;}
private:std::shared_ptr<Calls>c_;
};
class MissingCa final:public ITransport,public IStartTlsTransport {
public:
 explicit MissingCa(std::shared_ptr<Calls>c):c_(std::move(c)){}~MissingCa()override{++c_->destroy;}
 rs::util::Result<void>connect(std::string_view,std::uint16_t,Deadline)override{++c_->connect;return {};}
 rs::util::Result<void>connect_plain(std::string_view,std::uint16_t,Deadline)override{++c_->plain;return {};}
 rs::util::Result<void>upgrade_to_tls(std::string_view,Deadline)override{++c_->upgrade;return {};}
 rs::util::Result<IOResult>send(std::span<const std::byte>,Deadline)override{++c_->send;return IOResult{};}
 rs::util::Result<IOResult>recv(std::span<std::byte>,Deadline)override{++c_->recv;return IOResult{};}
 void close()noexcept override{++c_->close;}
private:std::shared_ptr<Calls>c_;
};
class MissingTls final:public ITransport,public ITlsConfigurableTransport {
public:
 explicit MissingTls(std::shared_ptr<Calls>c):c_(std::move(c)){}~MissingTls()override{++c_->destroy;}
 rs::util::Result<void>connect(std::string_view,std::uint16_t,Deadline)override{++c_->connect;return {};}
 rs::util::Result<IOResult>send(std::span<const std::byte>,Deadline)override{++c_->send;return IOResult{};}
 rs::util::Result<IOResult>recv(std::span<std::byte>,Deadline)override{++c_->recv;return IOResult{};}
 void set_ca_locations(const std::string&,const std::string&)override{++c_->ca;}
 void close()noexcept override{++c_->close;}
private:std::shared_ptr<Calls>c_;
};
class Mock final:public ITransport,public IStartTlsTransport,public ITlsConfigurableTransport {
public:
 explicit Mock(std::shared_ptr<Calls>c):calls(std::move(c)){}~Mock()override{++calls->destroy;}
 std::shared_ptr<Calls>calls;bool peer=true;int mode{};std::size_t chunk=std::numeric_limits<std::size_t>::max();
 std::vector<std::byte> response;std::size_t offset{};std::function<void()>callback;
 rs::util::Result<void>connect(std::string_view h,std::uint16_t p,Deadline d)override{++calls->connect;args(h,p,d);return {};}
 rs::util::Result<void>connect_plain(std::string_view h,std::uint16_t p,Deadline d)override{++calls->plain;args(h,p,d);return {};}
 rs::util::Result<void>upgrade_to_tls(std::string_view h,Deadline d)override{++calls->upgrade;args(h,0,d);return {};}
 void set_ca_locations(const std::string&f,const std::string&d)override{++calls->ca;calls->file=f;calls->dir=d;}
 bool peer_identity_verified()noexcept override{return peer;}
 rs::util::Result<IOResult>send(std::span<const std::byte>b,Deadline d)override{++calls->send;calls->deadline=d;return outcome(b.size());}
 rs::util::Result<IOResult>recv(std::span<std::byte>b,Deadline d)override{
  ++calls->recv;calls->deadline=d;
  if(mode)return outcome(b.size());
  const auto n=std::min({b.size(),chunk,response.size()-offset});std::copy_n(response.begin()+offset,n,b.begin());offset+=n;
  if(callback)callback();return IOResult{n,false};
 }
 void close()noexcept override{++calls->close;}
private:
 void args(std::string_view h,std::uint16_t p,Deadline d){calls->host=h;calls->port=p;calls->deadline=d;}
 rs::util::Result<IOResult>outcome(std::size_t n){
  if(callback)callback();
  if(mode==1)return {std::make_error_code(std::errc::permission_denied),"literal delegate error"};
  if(mode==2)throw 73;
  if(mode==3)return IOResult{0,false};
  if(mode==4)return IOResult{n+1,false};
  if(mode==5)return IOResult{std::min(n,chunk),true};
  return IOResult{std::min(n,chunk),false};
 }
};
struct Fixture {
 std::shared_ptr<Calls>calls=std::make_shared<Calls>();Mock*mock{};std::unique_ptr<Observer>observer;
 Fixture(){auto m=std::make_unique<Mock>(calls);mock=m.get();auto made=Observer::create(std::move(m));observer=std::move(*made);}
};
void append(std::vector<std::byte>&a,const std::vector<std::byte>&b){a.insert(a.end(),b.begin(),b.end());}
std::vector<std::byte>frame(char c,std::span<const std::byte>body={}) {
 std::vector<std::byte>out{std::byte(c)};auto n=static_cast<std::uint32_t>(body.size()+4);
 for(int shift:{24,16,8,0})out.push_back(std::byte((n>>shift)&255));append(out,{body.begin(),body.end()});return out;
}
std::vector<std::byte>request(){
 const unsigned char bytes[]{'P',0,0,0,21,0,'S','E','L','E','C','T',' ','$','1',0,0,1,0,0,0,17,'D',0,0,0,6,'S',0,'S',0,0,0,4};
 std::vector<std::byte>v;for(auto b:bytes)v.push_back(std::byte(b));return v;
}
std::vector<std::byte>description(bool row=false){
 auto v=frame('1');const std::byte count[]{std::byte{0},std::byte{1},std::byte{0},std::byte{0},std::byte{0x19},std::byte{0x97}};
 append(v,frame('t',count));const std::byte empty[]{std::byte{0},std::byte{0}};append(v,row?frame('T',empty):frame('n'));
 const std::byte idle[]{std::byte{'I'}};append(v,frame('Z',idle));return v;
}
Deadline future(){return rs::util::Clock::now()+std::chrono::seconds(3);}
void send_all(Fixture&f,const std::vector<std::byte>&v,Deadline d){
 std::size_t off=0;while(off<v.size()){const auto r=f.observer->send(std::span(v).subspan(off),d);ASSERT_TRUE(r);ASSERT_GT(r->n,0u);ASSERT_LE(r->n,v.size()-off);off+=r->n;}
}
void recv_all(Fixture&f,Deadline d){
 std::array<std::byte,37>b{};while(f.mock->offset<f.mock->response.size()){const auto r=f.observer->recv(b,d);ASSERT_TRUE(r);ASSERT_GT(r->n,0u);ASSERT_LE(r->n,b.size());}
}
}

TEST(PgStagedRefusalObserverTest, OwningExtensionsAndFaithfulForwarding) {
 EXPECT_FALSE(Observer::create(nullptr));
 for(int missing:{0,1,2}){auto calls=std::make_shared<Calls>();std::unique_ptr<ITransport>d=missing==0?std::unique_ptr<ITransport>(new Bare(calls)):missing==1?std::unique_ptr<ITransport>(new MissingCa(calls)):std::unique_ptr<ITransport>(new MissingTls(calls));const auto r=Observer::create(std::move(d));EXPECT_FALSE(r);EXPECT_EQ(1u,calls->destroy);EXPECT_EQ(0u,calls->connect);EXPECT_EQ(0u,calls->send);EXPECT_EQ(0u,calls->close);}
 Fixture f;const auto d=future();ASSERT_TRUE(f.observer->connect("literal-host",5439,d));EXPECT_EQ(1u,f.calls->connect);EXPECT_EQ("literal-host",f.calls->host);EXPECT_EQ(5439u,f.calls->port);EXPECT_EQ(d,f.calls->deadline);
 ASSERT_TRUE(f.observer->connect_plain("plain-host",1234,d));EXPECT_EQ(1u,f.calls->plain);EXPECT_EQ("plain-host",f.calls->host);EXPECT_EQ(1234u,f.calls->port);
 ASSERT_TRUE(f.observer->upgrade_to_tls("tls-host",d));EXPECT_EQ(1u,f.calls->upgrade);EXPECT_EQ("tls-host",f.calls->host);EXPECT_EQ(d,f.calls->deadline);
 f.observer->set_ca_locations("literal-ca","literal-dir");EXPECT_EQ(1u,f.calls->ca);EXPECT_EQ("literal-ca",f.calls->file);EXPECT_EQ("literal-dir",f.calls->dir);
 EXPECT_TRUE(f.observer->peer_identity_verified());f.mock->peer=false;EXPECT_FALSE(f.observer->peer_identity_verified());
 ITransport*base=f.observer.get();EXPECT_EQ(nullptr,dynamic_cast<rs::core::transport::IAsyncTransport*>(base));
 f.observer->close();f.observer->close();EXPECT_EQ(2u,f.calls->close);f.observer.reset();EXPECT_EQ(1u,f.calls->destroy);EXPECT_EQ(2u,f.calls->close);
}
TEST(PgStagedRefusalObserverTest, PrivacySingleLifecycleAndPermanentFault) {
 Fixture f;const std::byte auth[]{std::byte{'p'},std::byte{0},std::byte{'s'}};ASSERT_TRUE(f.observer->send(auth,future()));EXPECT_EQ(0u,f.observer->summary().sent_bytes);f.mock->response=frame('R');std::array<std::byte,8>private_buffer{};ASSERT_TRUE(f.observer->recv(private_buffer,future()));EXPECT_EQ(0u,f.observer->summary().received_bytes);f.mock->response.clear();f.mock->offset=0;
 const auto d=future();ASSERT_TRUE(f.observer->arm(d));send_all(f,request(),d);f.mock->response=description();recv_all(f,d);const auto summary=f.observer->seal();ASSERT_TRUE(summary.verified);EXPECT_EQ(Observer::Evidence::TestObservationOnly,summary.label);
 EXPECT_EQ(34u,summary.sent_bytes);EXPECT_EQ(27u,summary.received_bytes);ASSERT_TRUE(f.observer->send(auth,d));EXPECT_EQ(34u,f.observer->seal().sent_bytes);EXPECT_EQ(Observer::State::Sealed,f.observer->state());EXPECT_FALSE(f.observer->arm(d));EXPECT_FALSE(f.observer->seal().verified);EXPECT_TRUE(summary.verified);
 for(int mode:{0,1,2}){Fixture g;if(mode==0)g.mock->peer=false;const auto dl=mode==1?Deadline{}:future();if(mode==2){EXPECT_FALSE(g.observer->seal().verified);}EXPECT_FALSE(g.observer->arm(dl));g.mock->peer=true;EXPECT_FALSE(g.observer->arm(future()));EXPECT_EQ(Observer::State::Fault,g.observer->state());EXPECT_EQ(0u,g.calls->send);}
 Fixture peer;const auto pd=future();ASSERT_TRUE(peer.observer->arm(pd));peer.mock->peer=false;EXPECT_FALSE(peer.observer->peer_identity_verified());EXPECT_EQ(Observer::Fault::Peer,peer.observer->summary().fault);
}
TEST(PgStagedRefusalObserverTest, SuccessfulOutgoingPrefixesAndStrictSequence) {
 for(std::size_t n=1;n<=34;++n){SCOPED_TRACE(n);Fixture f;const auto d=future();ASSERT_TRUE(f.observer->arm(d));f.mock->chunk=n;send_all(f,request(),d);f.mock->response=description();recv_all(f,d);const auto s=f.observer->seal();ASSERT_TRUE(s.verified);EXPECT_EQ(34u,s.sent_bytes);EXPECT_EQ(3u,s.sent_frames);EXPECT_EQ(1u,s.parses);EXPECT_EQ(1u,s.describes);EXPECT_EQ(1u,s.syncs);}
 Fixture prefix;const auto d=future();ASSERT_TRUE(prefix.observer->arm(d));prefix.mock->chunk=1;auto noisy=request();noisy.push_back(std::byte{'B'});const auto sent=prefix.observer->send(noisy,d);ASSERT_TRUE(sent);ASSERT_EQ(1u,sent->n);auto rest=request();rest.erase(rest.begin());send_all(prefix,rest,d);prefix.mock->response=description();recv_all(prefix,d);EXPECT_TRUE(prefix.observer->seal().verified);EXPECT_EQ(34u,prefix.observer->summary().sent_bytes);
 for(char tag:{'B','E','Q','D','S'}){Fixture f;const auto dl=future();ASSERT_TRUE(f.observer->arm(dl));const auto bytes=frame(tag);ASSERT_TRUE(f.observer->send(bytes,dl));EXPECT_EQ(Observer::State::Fault,f.observer->state());EXPECT_EQ(1u,f.calls->send);}
 for(int invalid=0;invalid<3;++invalid){Fixture f;const auto dl=future();ASSERT_TRUE(f.observer->arm(dl));auto bytes=request();if(invalid==0)append(bytes,request());if(invalid==1)bytes[26]=std::byte{7};if(invalid==2)bytes[33]=std::byte{5};send_all(f,bytes,dl);EXPECT_FALSE(f.observer->seal().verified);}
 Fixture partial;const auto pd=future();ASSERT_TRUE(partial.observer->arm(pd));const auto bytes=request();ASSERT_TRUE(partial.observer->send(std::span(bytes).first(4),pd));EXPECT_FALSE(partial.observer->seal().verified);
}
TEST(PgStagedRefusalObserverTest, IncomingFragmentationDescriptionAndNativeErrorBranches) {
 for(bool row:{false,true})for(std::size_t n=1;n<=27;++n){SCOPED_TRACE(n);Fixture f;const auto d=future();ASSERT_TRUE(f.observer->arm(d));send_all(f,request(),d);f.mock->chunk=n;f.mock->response=description(row);recv_all(f,d);const auto s=f.observer->seal();ASSERT_TRUE(s.verified);EXPECT_EQ(Observer::Completion::DescriptionIdle,s.completion);EXPECT_EQ(1u,s.parameter_count);EXPECT_EQ(4u,s.received_frames);EXPECT_TRUE(s.ready_idle);}
 Fixture error;const auto d=future();ASSERT_TRUE(error.observer->arm(d));send_all(error,request(),d);const std::byte error_body[]{std::byte{0}};error.mock->response=frame('E',error_body);const std::byte idle[]{std::byte{'I'}};append(error.mock->response,frame('Z',idle));recv_all(error,d);const auto es=error.observer->seal();EXPECT_TRUE(es.verified);EXPECT_EQ(Observer::Completion::ErrorIdle,es.completion);EXPECT_EQ(0u,es.parameter_count);
 for(int invalid=0;invalid<8;++invalid){SCOPED_TRACE(invalid);Fixture f;const auto dl=future();ASSERT_TRUE(f.observer->arm(dl));send_all(f,request(),dl);auto r=description();if(invalid==0)r.erase(r.begin(),r.begin()+5);if(invalid==1)r[11]=std::byte{2};if(invalid==2)r[10]=std::byte{9};if(invalid==3)r.back()=std::byte{'T'};if(invalid==4)append(r,frame('Z',idle));if(invalid==5)r[0]=std::byte{'D'};if(invalid==6)r[0]=std::byte{'R'};if(invalid==7)r[0]=std::byte{'G'};f.mock->response=r;recv_all(f,dl);EXPECT_FALSE(f.observer->seal().verified);}
 for(unsigned count:{0u,8u,9u}){Fixture f;const auto dl=future();ASSERT_TRUE(f.observer->arm(dl));send_all(f,request(),dl);auto r=description();r[11]=std::byte(count);f.mock->response=r;recv_all(f,dl);EXPECT_FALSE(f.observer->seal().verified);}
}
TEST(PgStagedRefusalObserverTest, OriginalErrorsExceptionsAndHostileIoCounts) {
 for(bool receive:{false,true})for(int mode=1;mode<=5;++mode){SCOPED_TRACE(mode);Fixture f;const auto d=future();ASSERT_TRUE(f.observer->arm(d));f.mock->mode=mode;std::array<std::byte,8>buf{};
  if(mode==2){try{if(receive){(void)f.observer->recv(buf,d);}else{(void)f.observer->send(buf,d);}FAIL()<<"delegate exception missing";}catch(int n){EXPECT_EQ(73,n);}}
  else{const auto r=receive?f.observer->recv(buf,d):f.observer->send(buf,d);if(mode==1){ASSERT_TRUE(r.has_error());EXPECT_EQ(std::make_error_code(std::errc::permission_denied),r.error());EXPECT_EQ("literal delegate error",r.error_message());}else{ASSERT_TRUE(r);EXPECT_EQ(mode==3?0u:mode==4?9u:8u,r->n);EXPECT_EQ(mode==5,r->eof);}}
  EXPECT_EQ(Observer::State::Fault,f.observer->state());EXPECT_EQ(receive?1u:0u,f.calls->recv);EXPECT_EQ(receive?0u:1u,f.calls->send);EXPECT_EQ(0u,f.observer->summary().sent_bytes);EXPECT_EQ(0u,f.observer->summary().received_bytes);f.mock->mode=0;ASSERT_TRUE(f.observer->send(buf,d));EXPECT_FALSE(f.observer->seal().verified);f.observer->close();EXPECT_EQ(1u,f.calls->close);
 }
}
TEST(PgStagedRefusalObserverTest, OriginalDeadlineMismatchAndLateCompletionAreTerminal) {
 for(bool receive:{false,true}){Fixture f;const auto d=future();ASSERT_TRUE(f.observer->arm(d));std::array<std::byte,8>b{};const auto wrong=d+std::chrono::milliseconds(1);const auto r=receive?f.observer->recv(b,wrong):f.observer->send(b,wrong);ASSERT_TRUE(r);EXPECT_EQ(wrong,f.calls->deadline);EXPECT_EQ(Observer::Fault::Deadline,f.observer->summary().fault);EXPECT_FALSE(f.observer->arm(future()));}
 for(bool receive:{false,true}){Fixture f;const auto d=rs::util::Clock::now()+std::chrono::milliseconds(20);ASSERT_TRUE(f.observer->arm(d));f.mock->response=request();f.mock->callback=[&]{std::this_thread::sleep_until(d+std::chrono::milliseconds(1));};std::array<std::byte,34>b{};const auto r=receive?f.observer->recv(b,d):f.observer->send(b,d);ASSERT_TRUE(r);EXPECT_EQ(34u,r->n);EXPECT_EQ(d,f.calls->deadline);EXPECT_EQ(Observer::Fault::Deadline,f.observer->summary().fault);EXPECT_FALSE(f.observer->seal().verified);}
 Fixture expired;const auto ed=rs::util::Clock::now()+std::chrono::milliseconds(20);ASSERT_TRUE(expired.observer->arm(ed));std::this_thread::sleep_until(ed+std::chrono::milliseconds(1));const auto bytes=request();ASSERT_TRUE(expired.observer->send(bytes,ed));EXPECT_EQ(1u,expired.calls->send);EXPECT_EQ(ed,expired.calls->deadline);EXPECT_EQ(Observer::Fault::Deadline,expired.observer->summary().fault);
 Fixture seal;const auto sd=rs::util::Clock::now()+std::chrono::milliseconds(20);ASSERT_TRUE(seal.observer->arm(sd));send_all(seal,request(),sd);seal.mock->response=description();recv_all(seal,sd);std::this_thread::sleep_until(sd+std::chrono::milliseconds(1));EXPECT_FALSE(seal.observer->seal().verified);EXPECT_EQ(Observer::Fault::Deadline,seal.observer->summary().fault);
}
TEST(PgStagedRefusalObserverTest, FixedResourceLimitsRejectBeforePayloadWork) {
 for(std::size_t count:{25u,26u}){Fixture f;const auto d=future();ASSERT_TRUE(f.observer->arm(d));send_all(f,request(),d);for(std::size_t i=0;i<count;++i)append(f.mock->response,frame('N'));append(f.mock->response,description());recv_all(f,d);EXPECT_EQ(count==25,f.observer->seal().verified);if(count==25){EXPECT_EQ(32u,f.observer->summary().sent_frames+f.observer->summary().received_frames);}}
 for(std::size_t extra:{0u,1u}){Fixture f;const auto d=future();ASSERT_TRUE(f.observer->arm(d));std::vector<std::byte>body(16340+extra,std::byte{'x'});auto r=frame('P',body);const std::byte describe[]{std::byte{'S'},std::byte{0}};append(r,frame('D',describe));append(r,frame('S'));send_all(f,r,d);f.mock->response=description();recv_all(f,d);EXPECT_EQ(extra==0,f.observer->seal().verified);if(extra==0){EXPECT_EQ(16384u,f.observer->summary().sent_bytes+f.observer->summary().received_bytes);}}
 for(unsigned length:{0u,3u,16385u,0xffffffffu}){Fixture f;const auto d=future();ASSERT_TRUE(f.observer->arm(d));std::vector<std::byte>header{std::byte{'P'}};for(int shift:{24,16,8,0})header.push_back(std::byte((length>>shift)&255));ASSERT_TRUE(f.observer->send(header,d));EXPECT_EQ(Observer::Fault::Bounds,f.observer->summary().fault);EXPECT_EQ(1u,f.calls->send);}
}
TEST(PgStagedRefusalObserverTest, StructuralSummaryNeverClaimsMetadataOrAdmission) {
 Fixture f;const auto d=future();ASSERT_TRUE(f.observer->arm(d));send_all(f,request(),d);auto r=description(true);const auto insert=frame('S');r.insert(r.begin(),insert.begin(),insert.end());const auto notification=frame('A');r.insert(r.begin(),notification.begin(),notification.end());const auto notice=frame('N');r.insert(r.begin()+insert.size()+5,notice.begin(),notice.end());f.mock->response=r;recv_all(f,d);const auto s=f.observer->seal();EXPECT_TRUE(s.verified);EXPECT_EQ(Observer::Evidence::TestObservationOnly,s.label);EXPECT_EQ(Observer::Completion::DescriptionIdle,s.completion);EXPECT_EQ(7u,s.received_frames);
 Fixture g;const auto gd=future();ASSERT_TRUE(g.observer->arm(gd));send_all(g,request(),gd);g.mock->response=description();g.mock->response.pop_back();recv_all(g,gd);EXPECT_FALSE(g.observer->seal().verified);g.mock->response=description();g.mock->offset=0;recv_all(g,gd);EXPECT_FALSE(g.observer->seal().verified);EXPECT_EQ(Observer::State::Fault,g.observer->state());
 for(int operation=0;operation<4;++operation){Fixture f;const auto dl=future();ASSERT_TRUE(f.observer->arm(dl));if(operation==0){ASSERT_TRUE(f.observer->connect("h",1,dl));EXPECT_EQ(1u,f.calls->connect);}if(operation==1){ASSERT_TRUE(f.observer->connect_plain("h",1,dl));EXPECT_EQ(1u,f.calls->plain);}if(operation==2){ASSERT_TRUE(f.observer->upgrade_to_tls("h",dl));EXPECT_EQ(1u,f.calls->upgrade);}if(operation==3){f.observer->set_ca_locations("f","d");EXPECT_EQ(1u,f.calls->ca);}EXPECT_EQ(Observer::Fault::Lifecycle,f.observer->summary().fault);}
 Fixture closed;const auto cd=future();ASSERT_TRUE(closed.observer->arm(cd));closed.observer->close();EXPECT_EQ(Observer::Fault::Lifecycle,closed.observer->summary().fault);EXPECT_EQ(1u,closed.calls->close);
}
