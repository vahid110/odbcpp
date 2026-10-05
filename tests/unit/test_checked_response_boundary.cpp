#include "odbcpp/auth/checked_response_boundary.h"
#include <gtest/gtest.h>
#include <array>
#include <stdexcept>
#include <type_traits>
#include <vector>
using namespace std::chrono_literals;
namespace a=rs::core::auth;
namespace {
// Closed deterministic gates exercise only wrapper ownership/error policy.
// They are not real clock/phase/issuer authorities or malicious-gate protection.
enum class Event { Request, Check, Reject, Destroy };
enum class Reply { Normal, Success, Nonterminal, Unconfirmed, Mismatched, Throws };
struct Owner {
  Owner() {events.reserve(64);}
  std::vector<Event> events;
  std::optional<a::BoundaryFailure> fault,explicit_failure;
  unsigned created{},destroyed{},checks{},rejections{},live_transport{},live_processing{};
  bool throw_check{},throw_reject{},throw_confirmation{};
  Reply reply{Reply::Normal};
};
a::Request operation() {
  auto b=a::Binding::create({a::Service::Redshift,"boundary.fixture.invalid",5439,"db","user","resource",
      "boundary.fixture.invalid","local-policy"},
      {a::SourceKind::TrustedTemporaryDbIssuer,"synthetic-source","generation-one"},a::Method::TemporaryDatabasePassword);
  return std::move(a::Request::create(std::move(b).value(),rs::util::Deadline{10ms},100us)).value();
}
a::Request invalid_operation() {
  auto value=operation();auto moved=std::move(value);(void)moved;
  return value; // Closed moved-from Binding stays invalid.
}
template<class Interface> class Gate final:public Interface {
 public:
  Gate(std::shared_ptr<Owner> owner,a::Request request,bool null_request=false)
      :owner_(std::move(owner)),request_(std::move(request)),null_request_(null_request) {
    ++owner_->created;++live();
  }
  ~Gate() noexcept override {
    if(!live()) {std::terminate();}
    --live();++owner_->destroyed;owner_->events.push_back(Event::Destroy);
  }
  const a::Request* request() const noexcept override {
    owner_->events.push_back(Event::Request);return null_request_?nullptr:&request_;
  }
  a::BoundaryStatus observe_checkpoint() override {
    ++owner_->checks;owner_->events.push_back(Event::Check);
    if(owner_->throw_check) {owner_->throw_check=false;throw std::runtime_error("private checkpoint payload");}
    if(owner_->rejections && owner_->throw_confirmation) {throw std::runtime_error("private confirmation payload");}
    if(owner_->rejections && owner_->reply==Reply::Mismatched) {return a::BoundaryFailure::ReadFailed;}
    if(owner_->fault) {return *owner_->fault;}
    if(owner_->explicit_failure) {return *owner_->explicit_failure;}
    return std::monostate{};
  }
  a::BoundaryStatus reject_body_fault() override {
    ++owner_->rejections;owner_->events.push_back(Event::Reject);
    if(owner_->throw_reject) {owner_->throw_reject=false;throw std::runtime_error("private rejection payload");}
    switch(owner_->reply) {
      case Reply::Success:return std::monostate{};
      case Reply::Nonterminal:return a::BoundaryFailure::InvalidState;
      case Reply::Unconfirmed:return a::BoundaryFailure::BodyRejected;
      case Reply::Mismatched:break;
      case Reply::Throws:throw std::runtime_error("private negative payload");
      case Reply::Normal:break;
    }
    if(!owner_->fault) {owner_->fault=a::BoundaryFailure::BodyRejected;}
    return *owner_->fault;
  }
 private:
  unsigned& live() noexcept {
    if constexpr(std::is_same_v<Interface,a::detail::TransportBoundaryGate>) {return owner_->live_transport;}
    else {return owner_->live_processing;}
  }
  std::shared_ptr<Owner> owner_;const a::Request request_;const bool null_request_;
};
struct Transport {
  using Interface=a::detail::TransportBoundaryGate;using Lease=a::TransportBoundaryLease;
  static a::BoundaryResult<Lease> bind(std::unique_ptr<Interface> gate) {return a::detail::ResponseLeaseFactory::bind_transport(std::move(gate));}
};
struct Processing {
  using Interface=a::detail::ProcessingBoundaryGate;using Lease=a::ProcessingBoundaryLease;
  static a::BoundaryResult<Lease> bind(std::unique_ptr<Interface> gate) {return a::detail::ResponseLeaseFactory::bind_processing(std::move(gate));}
};
template<class Phase> auto acquire(const std::shared_ptr<Owner>& owner,a::Request request=operation(),bool null_request=false) {
  return Phase::bind(std::make_unique<Gate<typename Phase::Interface>>(owner,std::move(request),null_request));
}
template<class T> void failure(const a::BoundaryResult<T>& result,a::BoundaryFailure expected) {
  ASSERT_TRUE(std::holds_alternative<a::BoundaryFailure>(result));EXPECT_EQ(std::get<a::BoundaryFailure>(result),expected);
}
void failure(const a::BoundaryStatus& result,a::BoundaryFailure expected) {
  ASSERT_TRUE(std::holds_alternative<a::BoundaryFailure>(result));EXPECT_EQ(std::get<a::BoundaryFailure>(result),expected);
}
static_assert(!std::is_copy_constructible_v<a::TransportBoundaryLease>);
static_assert(!std::is_copy_constructible_v<a::ProcessingBoundaryLease>);
static_assert(!std::is_move_assignable_v<a::TransportBoundaryLease>);
static_assert(!std::is_move_assignable_v<a::ProcessingBoundaryLease>);
static_assert(std::is_nothrow_move_constructible_v<a::TransportBoundaryLease>);
static_assert(std::is_nothrow_move_constructible_v<a::ProcessingBoundaryLease>);
static_assert(!std::is_constructible_v<a::ProcessingBoundaryLease,a::TransportBoundaryLease&&>);
static_assert(!std::is_constructible_v<a::TransportBoundaryLease,a::ProcessingBoundaryLease&&>);
}
TEST(CheckedResponseBoundary, OwningImmutableRequestSurvivesApplicationMoveAndLifetime) {
  auto owner=std::make_shared<Owner>();
  {
    auto result=[&] {auto original=operation();auto acquired=acquire<Transport>(owner,original);
      auto moved=std::move(original);(void)moved;return acquired;}();
    ASSERT_TRUE(std::holds_alternative<Transport::Lease>(result));
    auto lease=std::get<Transport::Lease>(std::move(result));
    ASSERT_TRUE(lease.request());EXPECT_FALSE(lease.request()->binding().invariant_error());
    EXPECT_TRUE(lease.request()->binding().target().endpoint=="boundary.fixture.invalid");
    EXPECT_TRUE(lease.request()->binding().source().generation=="generation-one");
    EXPECT_EQ(lease.request()->binding().method(),a::Method::TemporaryDatabasePassword);
    EXPECT_EQ(lease.request()->deadline(),rs::util::Deadline{10ms});EXPECT_EQ(lease.request()->headroom(),100us);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(lease.check()));
    EXPECT_EQ(owner->created,1u);EXPECT_EQ(owner->destroyed,0u);EXPECT_EQ(owner->live_transport,1u);
  }
  EXPECT_EQ(owner->destroyed,1u);EXPECT_EQ(owner->live_transport,0u);EXPECT_EQ(owner->rejections,0u);
  EXPECT_EQ(owner->events.front(),Event::Request);EXPECT_EQ(owner->events[1],Event::Check);EXPECT_EQ(owner->events.back(),Event::Destroy);
}
TEST(CheckedResponseBoundary, NullAndInvalidOwningFactoryHaveDistinctTerminalDisposition) {
  failure(Transport::bind(nullptr),a::BoundaryFailure::InvalidBoundary);
  failure(Processing::bind(nullptr),a::BoundaryFailure::InvalidBoundary);
  for(bool processing:{false,true}) {
    for(bool null_request:{false,true}) {
      auto owner=std::make_shared<Owner>();
      if(processing) {failure(acquire<Processing>(owner,invalid_operation(),null_request),a::BoundaryFailure::BodyRejected);}
      else {failure(acquire<Transport>(owner,invalid_operation(),null_request),a::BoundaryFailure::BodyRejected);}
      EXPECT_EQ(owner->fault,a::BoundaryFailure::BodyRejected);EXPECT_EQ(owner->rejections,1u);EXPECT_EQ(owner->checks,1u);
      EXPECT_EQ(owner->destroyed,1u);EXPECT_EQ(owner->live_transport+owner->live_processing,0u);
      ASSERT_EQ(owner->events.size(),4u);EXPECT_EQ(owner->events[0],Event::Request);EXPECT_EQ(owner->events[1],Event::Reject);
      EXPECT_EQ(owner->events[2],Event::Check);EXPECT_EQ(owner->events[3],Event::Destroy);
    }
  }
}
TEST(CheckedResponseBoundary, InitialLocalStatusesForwardWithoutMutatingOwner) {
  constexpr std::array reasons{a::BoundaryFailure::InvalidInput,a::BoundaryFailure::InvalidState,a::BoundaryFailure::WrongOperation,
      a::BoundaryFailure::WrongThread,a::BoundaryFailure::UnknownQuality,a::BoundaryFailure::Borrowed,a::BoundaryFailure::AllocationFailed};
  for(const auto reason:reasons) {
    for(bool processing:{false,true}) {
      auto owner=std::make_shared<Owner>();owner->explicit_failure=reason;
      if(processing) {failure(acquire<Processing>(owner),reason);}else {failure(acquire<Transport>(owner),reason);}
      EXPECT_EQ(owner->checks,1u);EXPECT_EQ(owner->rejections,0u);EXPECT_FALSE(owner->fault);EXPECT_EQ(owner->destroyed,1u);
    }
  }
  auto owner=std::make_shared<Owner>();auto result=acquire<Transport>(owner);
  ASSERT_TRUE(std::holds_alternative<Transport::Lease>(result));auto& lease=std::get<Transport::Lease>(result);
  for(const auto reason:{a::BoundaryFailure::WrongThread,a::BoundaryFailure::InvalidState}) {
    owner->explicit_failure=reason;failure(lease.check(),reason);EXPECT_EQ(owner->rejections,0u);EXPECT_FALSE(owner->fault);
  }
}
TEST(CheckedResponseBoundary, UnexpectedVirtualFailuresConfirmSameGateTerminalError) {
  for(bool during_factory:{false,true}) {
    for(bool processing:{false,true}) {
      auto owner=std::make_shared<Owner>();owner->throw_check=during_factory;
      if(during_factory) {
        if(processing) {failure(acquire<Processing>(owner),a::BoundaryFailure::BodyRejected);}
        else {failure(acquire<Transport>(owner),a::BoundaryFailure::BodyRejected);}
      } else if(processing) {
        auto result=acquire<Processing>(owner);ASSERT_TRUE(std::holds_alternative<Processing::Lease>(result));
        owner->throw_check=true;failure(std::get<Processing::Lease>(result).check(),a::BoundaryFailure::BodyRejected);
      } else {
        auto result=acquire<Transport>(owner);ASSERT_TRUE(std::holds_alternative<Transport::Lease>(result));
        owner->throw_reject=true;failure(std::get<Transport::Lease>(result).reject_body(),a::BoundaryFailure::BodyRejected);
        EXPECT_EQ(owner->rejections,2u);
      }
      EXPECT_EQ(owner->fault,a::BoundaryFailure::BodyRejected);EXPECT_GE(owner->checks,2u);
      const auto last=owner->events.size();ASSERT_GE(last,3u);EXPECT_EQ(owner->events[last-1],Event::Destroy);
      EXPECT_EQ(owner->events[last-2],Event::Check);EXPECT_EQ(owner->events[last-3],Event::Reject);
    }
  }
}
TEST(CheckedResponseBoundary, UnconfirmableExceptionOrInvalidOwningFactoryIsFatal) {
  for(bool processing:{false,true}) {
    for(bool invalid_request:{false,true}) {
      for(const auto reply:{Reply::Success,Reply::Nonterminal,Reply::Unconfirmed,Reply::Mismatched,Reply::Throws}) {
        EXPECT_DEATH({
          auto owner=std::make_shared<Owner>();owner->reply=reply;owner->throw_check=!invalid_request;
          if(processing) {(void)acquire<Processing>(owner,invalid_request?invalid_operation():operation());}
          else {(void)acquire<Transport>(owner,invalid_request?invalid_operation():operation());}
        },"");
      }
    }
  }
  EXPECT_DEATH({auto owner=std::make_shared<Owner>();owner->throw_check=true;owner->throw_confirmation=true;
    (void)acquire<Transport>(owner);},"");
}
TEST(CheckedResponseBoundary, EarlierTerminalFaultRemainsPrimaryAfterRecoveryAndCancel) {
  constexpr std::array reasons{a::BoundaryFailure::ClockRollback,a::BoundaryFailure::DeadlineElapsed,a::BoundaryFailure::Cancelled,
      a::BoundaryFailure::SourceClosed,a::BoundaryFailure::SourceChanged,a::BoundaryFailure::ReadFailed,a::BoundaryFailure::Overflow,
      a::BoundaryFailure::StaleObservation,a::BoundaryFailure::UnsafeDiagnostic,a::BoundaryFailure::InvalidDiagnostic,
      a::BoundaryFailure::StaleDiagnostic,a::BoundaryFailure::ForeignDispatch,a::BoundaryFailure::BodyRejected};
  for(const auto reason:reasons) {
    auto owner=std::make_shared<Owner>();auto result=acquire<Transport>(owner);
    ASSERT_TRUE(std::holds_alternative<Transport::Lease>(result));auto& lease=std::get<Transport::Lease>(result);
    owner->fault=reason;owner->throw_check=true;failure(lease.check(),reason);
    owner->explicit_failure=a::BoundaryFailure::Cancelled;failure(lease.reject_body(),reason);failure(lease.check(),reason);
    EXPECT_EQ(owner->fault,reason);EXPECT_EQ(owner->rejections,2u);
    // Invalid Request factory must also preserve an earlier terminal owner.
    auto other=std::make_shared<Owner>();other->fault=reason;
    failure(acquire<Processing>(other,invalid_operation()),reason);EXPECT_EQ(other->fault,reason);
  }
}
TEST(CheckedResponseBoundary, MovedLeaseHasZeroGateInteractionsAndSingleDestruction) {
  auto owner=std::make_shared<Owner>();
  {
    auto result=acquire<Transport>(owner);ASSERT_TRUE(std::holds_alternative<Transport::Lease>(result));
    auto first=std::get<Transport::Lease>(std::move(result));auto transferred=std::move(first);
    const auto events=owner->events.size();const auto checks=owner->checks,rejects=owner->rejections;
    EXPECT_EQ(first.request(),nullptr);failure(first.check(),a::BoundaryFailure::InvalidBoundary);failure(first.reject_body(),a::BoundaryFailure::InvalidBoundary);
    EXPECT_EQ(owner->events.size(),events);EXPECT_EQ(owner->checks,checks);EXPECT_EQ(owner->rejections,rejects);
    ASSERT_TRUE(transferred.request());EXPECT_TRUE(std::holds_alternative<std::monostate>(transferred.check()));
    EXPECT_EQ(owner->destroyed,0u);EXPECT_EQ(owner->live_transport,1u);
  }
  EXPECT_EQ(owner->destroyed,1u);EXPECT_EQ(owner->live_transport,0u);
}
TEST(CheckedResponseBoundary, DistinctPhaseTypesShareOnlyClosedErrorAndOwnershipPolicy) {
  auto owner=std::make_shared<Owner>();
  {
    auto transport=acquire<Transport>(owner);auto processing=acquire<Processing>(owner);
    ASSERT_TRUE(std::holds_alternative<Transport::Lease>(transport));ASSERT_TRUE(std::holds_alternative<Processing::Lease>(processing));
    auto& t=std::get<Transport::Lease>(transport);auto& p=std::get<Processing::Lease>(processing);
    EXPECT_EQ(owner->live_transport,1u);EXPECT_EQ(owner->live_processing,1u);
    ASSERT_TRUE(t.request());ASSERT_TRUE(p.request());
    EXPECT_TRUE(t.request()->binding().target()==p.request()->binding().target());
    EXPECT_TRUE(t.request()->binding().source()==p.request()->binding().source());
    EXPECT_EQ(t.request()->deadline(),p.request()->deadline());EXPECT_EQ(t.request()->headroom(),p.request()->headroom());
    failure(t.reject_body(),a::BoundaryFailure::BodyRejected);failure(p.check(),a::BoundaryFailure::BodyRejected);
    failure(p.reject_body(),a::BoundaryFailure::BodyRejected);EXPECT_EQ(owner->rejections,2u);
  }
  EXPECT_EQ(owner->destroyed,2u);EXPECT_EQ(owner->live_transport+owner->live_processing,0u);
  // No assertion here implies real Transport/c/Processing/n stage enforcement.
}
