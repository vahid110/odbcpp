#include "odbcpp/auth/checked_response_stream.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>
namespace a=rs::core::auth;
using namespace std::chrono_literals;
namespace {
// Direct closed gates test helper ownership/confirmation only, not real phases,
// clocks, quality, cancellation sources or malicious injected implementations.
enum class Event { Check, Reject, Destroy };
enum class Reply { Normal, Success, Nonterminal, Unconfirmed, Mismatched, Throws, ConfirmationThrows };
struct Lifetime {unsigned owners{},gates{};};
struct Owner {
  Owner() {events.reserve(128);}
  ~Owner() {++life->owners;}
  std::shared_ptr<Lifetime> life=std::make_shared<Lifetime>();
  std::vector<Event> events;
  std::optional<a::BoundaryFailure> first,scheduled;
  unsigned checks{},rejections{},live{},fault_at{};
  Reply reply{Reply::Normal};
};
a::Request operation() {
  auto b=a::Binding::create({a::Service::Redshift,"stream.fixture.invalid",5439,"db","user","resource","stream.fixture.invalid","local-policy"},
      {a::SourceKind::TrustedTemporaryDbIssuer,"synthetic","one"},a::Method::TemporaryDatabasePassword);
  return std::move(a::Request::create(std::move(b).value(),rs::util::Deadline{10ms},100us)).value();
}
class Gate final:public a::detail::TransportBoundaryGate {
 public:
  explicit Gate(std::shared_ptr<Owner> owner):owner_(std::move(owner)),request_(operation()) {++owner_->live;}
  ~Gate() noexcept override {
    if(!owner_->live) {std::terminate();}
    --owner_->live;++owner_->life->gates;owner_->events.push_back(Event::Destroy);
  }
  const a::Request* request() const noexcept override {return &request_;}
  a::BoundaryStatus observe_checkpoint() override {
    ++owner_->checks;owner_->events.push_back(Event::Check);
    if(owner_->rejections && owner_->reply==Reply::ConfirmationThrows) {throw std::runtime_error("fixed synthetic confirmation failure");}
    if(owner_->rejections && owner_->reply==Reply::Mismatched) {return a::BoundaryFailure::ReadFailed;}
    if(owner_->fault_at && owner_->checks==owner_->fault_at && !owner_->first) {owner_->first=owner_->scheduled;}
    if(owner_->first) {return *owner_->first;}
    return std::monostate{};
  }
  a::BoundaryStatus reject_body_fault() override {
    ++owner_->rejections;owner_->events.push_back(Event::Reject);
    switch(owner_->reply) {
      case Reply::Success:return std::monostate{};
      case Reply::Nonterminal:return a::BoundaryFailure::InvalidState;
      case Reply::Unconfirmed:return a::BoundaryFailure::BodyRejected;
      case Reply::Throws:throw std::runtime_error("fixed synthetic refusal failure");
      case Reply::Mismatched:case Reply::ConfirmationThrows:case Reply::Normal:break;
    }
    if(!owner_->first) {owner_->first=a::BoundaryFailure::BodyRejected;}
    return *owner_->first;
  }
 private:
  const std::shared_ptr<Owner> owner_;const a::Request request_;
};
struct Fixture {
  std::shared_ptr<Owner> owner=std::make_shared<Owner>();
  a::TransportBoundaryLease lease() {
    auto bound=a::detail::ResponseLeaseFactory::bind_transport(std::make_unique<Gate>(owner));
    return std::get<a::TransportBoundaryLease>(std::move(bound));
  }
  a::CheckedStreamOwner stream(std::size_t cap=8) {
    return std::get<a::CheckedStreamOwner>(a::CheckedStreamOwner::create(cap,lease()));
  }
};
template<class T> void error(const a::CheckedStreamResult<T>& r,std::optional<a::StreamFailure> local,
    std::optional<a::BoundaryFailure> boundary) {
  ASSERT_TRUE(std::holds_alternative<a::CheckedStreamError>(r));const auto& e=std::get<a::CheckedStreamError>(r);
  EXPECT_EQ(e.local(),local);EXPECT_EQ(e.boundary(),boundary);
}
void terminal(const Fixture& f,a::BoundaryFailure expected) {
  EXPECT_EQ(f.owner->first,expected);EXPECT_EQ(f.owner->live,0u);EXPECT_EQ(f.owner->life->gates,1u);
}
static_assert(!std::is_copy_constructible_v<a::CheckedStreamOwner>);
static_assert(!std::is_move_assignable_v<a::CheckedStreamOwner>);
static_assert(std::is_nothrow_move_constructible_v<a::CheckedStreamOwner>);
static_assert(!std::is_nothrow_move_constructible_v<a::ResponseBytes>); // Preserve legacy active-view refusal.
}
TEST(CheckedResponseStreamPortable, OwningChunksNulExactCapSeekAndTransfer) {
  for(const auto cap:{std::size_t{4},a::StreamOwner::max_bytes}) {
    Fixture f;auto stream=f.stream(cap);auto* io=stream.io();
    std::vector<char> input(cap,'s');input[1]='\0';
    io->write(input.data(),2);io->write(input.data()+2,static_cast<std::streamsize>(cap-2));
    EXPECT_EQ(io->tellp(),std::streampos{static_cast<std::streamoff>(cap)});std::fill(input.begin(),input.end(),'z');
    io->seekg(1);char c='x';io->get(c);EXPECT_TRUE(c=='\0');io->seekg(0);
    auto result=a::seal_checked_response(std::move(stream));ASSERT_TRUE(std::holds_alternative<a::ResponseBytes>(result));
    EXPECT_EQ(f.owner->live,0u);EXPECT_EQ(f.owner->life->gates,1u);EXPECT_EQ(f.owner->rejections,0u);EXPECT_FALSE(f.owner->first);
    auto& body=std::get<a::ResponseBytes>(result);EXPECT_EQ(body.size(),cap);
    std::uintptr_t address{};
    body.with_bytes([&](auto bytes) {address=reinterpret_cast<std::uintptr_t>(bytes.data());
      EXPECT_TRUE(bytes[1]==std::byte{0});for(std::size_t i=0;i<bytes.size();++i) {if(i!=1) {EXPECT_TRUE(bytes[i]==std::byte{'s'});}}});
    auto moved=std::move(body);EXPECT_EQ(body.size(),0u);EXPECT_THROW(body.with_bytes([](auto){}),std::logic_error);
    moved.with_bytes([&](auto bytes) {EXPECT_TRUE(reinterpret_cast<std::uintptr_t>(bytes.data())==address);});
    EXPECT_TRUE(moved.clear());EXPECT_TRUE(moved.empty());
  }
  Fixture empty;auto stream=empty.stream();auto sealed=a::seal_checked_response(std::move(stream));
  ASSERT_TRUE(std::holds_alternative<a::ResponseBytes>(sealed));EXPECT_TRUE(std::get<a::ResponseBytes>(sealed).empty());
  EXPECT_EQ(empty.owner->live,0u); // Empty raw body is not a service envelope.
}
TEST(CheckedResponseStreamPortable, LimitsAndAllConstructionSeamsRetainExternalRejectionGuard) {
  for(const auto cap:{std::size_t{0},a::StreamOwner::max_bytes+1}) {
    Fixture f;error(a::CheckedStreamOwner::create(cap,f.lease()),a::StreamFailure::InvalidLimit,a::BoundaryFailure::BodyRejected);
    terminal(f,a::BoundaryFailure::BodyRejected);EXPECT_GE(f.owner->rejections,1u);
    ASSERT_GE(f.owner->events.size(),3u);const auto n=f.owner->events.size();
    EXPECT_EQ(f.owner->events[n-1],Event::Destroy);EXPECT_EQ(f.owner->events[n-2],Event::Check);EXPECT_EQ(f.owner->events[n-3],Event::Reject);
  }
  for(const auto step:{a::detail::CheckedConstructionFailure::BeforeStorage,a::detail::CheckedConstructionFailure::BeforeState,a::detail::CheckedConstructionFailure::AfterState}) {
    Fixture f;error(a::detail::CheckedStreamTestAccess::refuse_construction(8,f.lease(),step),a::StreamFailure::AllocationFailed,a::BoundaryFailure::BodyRejected);
    terminal(f,a::BoundaryFailure::BodyRejected);
    Fixture prior;auto lease=prior.lease();prior.owner->first=a::BoundaryFailure::ClockRollback;
    error(a::detail::CheckedStreamTestAccess::refuse_construction(8,std::move(lease),step),{},a::BoundaryFailure::ClockRollback);
    terminal(prior,a::BoundaryFailure::ClockRollback);
  }
}
TEST(CheckedResponseStreamPortable, SignedLengthsCapacityAndNoHoleSeekFaultsCannotClear) {
  for(int scenario=0;scenario<7;++scenario) {
    Fixture f;auto stream=f.stream(4);auto* io=stream.io();auto* b=io->rdbuf();auto expected=a::StreamFailure::InvalidSeek;
    if(scenario==0) {EXPECT_EQ(b->sputn("abcd",4),4);EXPECT_EQ(b->sputn("e",1),0);expected=a::StreamFailure::LimitExceeded;}
    if(scenario==1) {EXPECT_EQ(b->sputn("a",-1),0);expected=a::StreamFailure::LimitExceeded;}
    if(scenario==2) {char c{};EXPECT_EQ(b->sgetn(&c,-1),0);expected=a::StreamFailure::StreamFailed;}
    if(scenario==3) {EXPECT_EQ(b->pubseekoff(1,std::ios::beg,std::ios::in),std::streampos{-1});}
    if(scenario==4) {EXPECT_EQ(b->sputn("ab",2),2);EXPECT_EQ(b->pubseekoff(std::numeric_limits<std::streamoff>::max(),std::ios::end,std::ios::in),std::streampos{-1});}
    if(scenario==5) {EXPECT_EQ(b->pubseekoff(0,std::ios::beg,std::ios::out),std::streampos{-1});}
    if(scenario==6) {EXPECT_EQ(b->pubseekoff(-1,std::ios::beg,std::ios::in),std::streampos{-1});}
    io->clear();const auto checks=f.owner->checks;EXPECT_EQ(b->sputn("x",1),0);EXPECT_EQ(f.owner->checks,checks);
    error(a::seal_checked_response(std::move(stream)),expected,a::BoundaryFailure::BodyRejected);terminal(f,a::BoundaryFailure::BodyRejected);
  }
}
TEST(CheckedResponseStreamPortable, CurrentEofAcceptsButStaleEvidenceAndUnexplainedFlagsRefuse) {
  for(int scenario=0;scenario<7;++scenario) {
    Fixture f;auto stream=f.stream();auto* io=stream.io();io->put('a');const bool succeeds=scenario==0 || scenario==2;
    if(scenario<3) {
      char c{};io->get(c);io->get(c);ASSERT_TRUE(io->eof());
      if(scenario>0) {
        io->clear();io->put('b');
        if(scenario==1) {io->setstate(std::ios::eofbit|std::ios::failbit);}
        else {io->get(c);io->get(c);ASSERT_TRUE(io->eof());}
      }
    }
    if(scenario==3) {io->setstate(std::ios::badbit);}
    if(scenario==4) {io->setstate(std::ios::failbit);}
    if(scenario==5 || scenario==6) {
      char c{};io->get(c);io->get(c);io->clear();io->seekg(0);
      if(scenario==6) {io->get(c);}
      io->setstate(std::ios::eofbit|std::ios::failbit);
    }
    const auto result=a::seal_checked_response(std::move(stream));
    if(succeeds) {EXPECT_TRUE(std::holds_alternative<a::ResponseBytes>(result));EXPECT_FALSE(f.owner->first);EXPECT_EQ(f.owner->rejections,0u);}
    else {error(result,a::StreamFailure::StreamFailed,a::BoundaryFailure::BodyRejected);terminal(f,a::BoundaryFailure::BodyRejected);}
  }
}
TEST(CheckedResponseStreamPortable, OneOwnerScheduledFaultWinsEveryOperationAndSeal) {
  constexpr std::array reasons{a::BoundaryFailure::ReadFailed,a::BoundaryFailure::ClockRollback,a::BoundaryFailure::DeadlineElapsed,
    a::BoundaryFailure::Cancelled,a::BoundaryFailure::SourceClosed,a::BoundaryFailure::SourceChanged,a::BoundaryFailure::StaleObservation};
  for(const auto reason:reasons) {
    for(int path=0;path<5;++path) {
      Fixture f;auto stream=f.stream();stream.io()->put('a');auto* b=stream.io()->rdbuf();
      f.owner->fault_at=f.owner->checks+1;f.owner->scheduled=reason;
      if(path==0) {EXPECT_EQ(b->sputn("b",1),0);}
      if(path==1) {EXPECT_EQ(b->sbumpc(),std::char_traits<char>::eof());}
      if(path==2) {EXPECT_EQ(b->pubseekoff(0,std::ios::beg,std::ios::in),std::streampos{-1});}
      if(path==3) {EXPECT_EQ(b->pubsync(),-1);}
      if(path==4) {error(a::seal_checked_response(std::move(stream)),{},reason);terminal(f,reason);continue;}
      f.owner->scheduled=a::BoundaryFailure::Cancelled;f.owner->fault_at=0;stream.io()->clear();const auto checks=f.owner->checks;
      EXPECT_EQ(b->sputn("b",1),0);EXPECT_EQ(f.owner->checks,checks);
      error(a::seal_checked_response(std::move(stream)),{},reason);terminal(f,reason);
    }
  }
  Fixture f;auto lease=f.lease();ASSERT_TRUE(lease.request());EXPECT_EQ(lease.request()->deadline(),rs::util::Deadline{10ms});
  EXPECT_EQ(lease.request()->headroom(),100us);auto stream=a::CheckedStreamOwner::create(8,std::move(lease));
  ASSERT_TRUE(std::holds_alternative<a::CheckedStreamOwner>(stream));EXPECT_EQ(lease.request(),nullptr);
  auto sealed=a::seal_checked_response(std::get<a::CheckedStreamOwner>(std::move(stream)));ASSERT_TRUE(std::holds_alternative<a::ResponseBytes>(sealed));
}
TEST(CheckedResponseStreamPortable, OwnerMovesScopedBytesAndFacadeLifetimesRemainExplicit) {
  Fixture f;auto stream=f.stream();auto* io=stream.io();auto* buffer=io->rdbuf();auto moved=std::move(stream);
  EXPECT_EQ(stream.io(),nullptr);EXPECT_TRUE(moved.io()==io);EXPECT_TRUE(moved.io()->rdbuf()==buffer);
  const auto checks=f.owner->checks;error(a::seal_checked_response(std::move(stream)),a::StreamFailure::AlreadyConsumed,{});EXPECT_EQ(f.owner->checks,checks);
  moved.io()->put('x');auto result=a::seal_checked_response(std::move(moved));ASSERT_TRUE(std::holds_alternative<a::ResponseBytes>(result));
  // io/buffer borrows ended before seal; never touch them again.
  auto& body=std::get<a::ResponseBytes>(result);
  body.with_bytes([&](auto bytes) {
    EXPECT_FALSE(body.clear());EXPECT_THROW({auto invalid=std::move(body);(void)invalid;},std::logic_error);
    EXPECT_TRUE(bytes.size()==1 && bytes[0]==std::byte{'x'});
  });
  EXPECT_TRUE(body.clear());
  Fixture facade;auto life=facade.owner->life;std::weak_ptr<Owner> weak=facade.owner;auto held=facade.stream();facade.owner.reset();
  EXPECT_FALSE(weak.expired());held.io()->put('x');auto sealed=a::seal_checked_response(std::move(held));
  ASSERT_TRUE(std::holds_alternative<a::ResponseBytes>(sealed));EXPECT_TRUE(weak.expired());EXPECT_EQ(life->gates,1u);EXPECT_EQ(life->owners,1u);
}
TEST(CheckedResponseStreamPortable, UnsealedAbandonmentRejectsBeforeReleaseAndPreservesPriorFault) {
  for(bool prior:{false,true}) {
    Fixture f;const auto expected=prior?a::BoundaryFailure::ClockRollback:a::BoundaryFailure::BodyRejected;
    {auto stream=f.stream();stream.io()->put('x');if(prior) {f.owner->first=expected;EXPECT_EQ(stream.io()->rdbuf()->pubsync(),-1);}}
    terminal(f,expected);ASSERT_GE(f.owner->events.size(),3u);const auto n=f.owner->events.size();
    EXPECT_EQ(f.owner->events[n-3],Event::Reject);EXPECT_EQ(f.owner->events[n-2],Event::Check);EXPECT_EQ(f.owner->events[n-1],Event::Destroy);
  }
}
TEST(CheckedResponseStreamPortable, MovedLeaseAndUnconfirmableNegativeGateNeverPublish) {
  Fixture f;auto original=f.lease();auto live=std::move(original);const auto checks=f.owner->checks;
  error(a::CheckedStreamOwner::create(8,std::move(original)),{},a::BoundaryFailure::InvalidBoundary);EXPECT_EQ(f.owner->checks,checks);
  EXPECT_EQ(f.owner->rejections,0u);EXPECT_FALSE(f.owner->first);EXPECT_EQ(f.owner->live,1u);
  auto stream=a::CheckedStreamOwner::create(8,std::move(live));ASSERT_TRUE(std::holds_alternative<a::CheckedStreamOwner>(stream));
  auto result=a::seal_checked_response(std::get<a::CheckedStreamOwner>(std::move(stream)));ASSERT_TRUE(std::holds_alternative<a::ResponseBytes>(result));
  for(const auto reply:{Reply::Success,Reply::Nonterminal,Reply::Unconfirmed,Reply::Mismatched,Reply::Throws,Reply::ConfirmationThrows}) {
    for(int path=0;path<3;++path) {
      EXPECT_DEATH({
        Fixture bad;auto lease=bad.lease();bad.owner->reply=reply;
        if(path==0) {(void)a::CheckedStreamOwner::create(0,std::move(lease));}
        else {auto held=std::get<a::CheckedStreamOwner>(a::CheckedStreamOwner::create(1,std::move(lease)));
          if(path==1) {held.io()->write("ab",2);} // path2 abandons unsealed healthy storage.
        }
      },"");
    }
  }
}
