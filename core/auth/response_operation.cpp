#include "core/auth/response_operation.h"
#include <exception>
#include <new>
#include <type_traits>
namespace rs::core::auth {
namespace {
bool finite(rs::util::Deadline value) noexcept { return value!=rs::util::Deadline::min() && value!=rs::util::Deadline::max(); }
bool matches(const Request& a,const Request& b) noexcept {
  return a.binding().target()==b.binding().target() && a.binding().source()==b.binding().source() &&
      a.binding().method()==b.binding().method() && a.deadline()==b.deadline() && a.headroom()==b.headroom();
}
}
BoundaryResult<std::shared_ptr<ResponseSourceGeneration>> ResponseSourceGeneration::create(const Binding& binding) noexcept {
  if(binding.invariant_error()) { return BoundaryFailure::InvalidInput; }
  try { return std::shared_ptr<ResponseSourceGeneration>{new ResponseSourceGeneration{binding.source()}}; }
  catch(...) { return BoundaryFailure::AllocationFailed; }
}
ResponseSourceGeneration::~ResponseSourceGeneration() { if(thread_!=std::this_thread::get_id()) { std::terminate(); } }
BoundaryStatus ResponseSourceGeneration::checkpoint() const noexcept {
  if(thread_!=std::this_thread::get_id()) { return BoundaryFailure::WrongThread; }
  if(retired_) { return *retired_; }
  return std::monostate{};
}
BoundaryStatus ResponseSourceGeneration::retire(SourceRetirement reason) noexcept {
  if(thread_!=std::this_thread::get_id()) { return BoundaryFailure::WrongThread; }
  if(retired_) { return *retired_; }
  switch(reason) {
    case SourceRetirement::Closed:retired_=BoundaryFailure::SourceClosed;break;
    case SourceRetirement::Changed:retired_=BoundaryFailure::SourceChanged;break;
    default:return BoundaryFailure::InvalidInput;
  }
  return *retired_;
}
namespace detail {
enum class ResponseStage { Prepared,Transport,Processing,Finalized };
struct ResponseOperationState final {
  ResponseOperationState(Request r,std::shared_ptr<ResponseObservationSource> observer,std::shared_ptr<ResponseSourceGeneration> source)
      :request(std::move(r)),observer(std::move(observer)),source(std::move(source)),thread(std::this_thread::get_id()) {}
  ~ResponseOperationState() { if(!same_thread() || reading || transport || processing) { std::terminate(); } }
  bool same_thread() const noexcept { return thread==std::this_thread::get_id(); }
  std::optional<BoundaryFailure> guard() const noexcept {
    if(!same_thread()) { return BoundaryFailure::WrongThread; }
    if(reading) { std::terminate(); }
    return first;
  }
  BoundaryFailure latch(BoundaryFailure fault) noexcept { if(!first) { first=fault; }return *first; }
  BoundaryStatus read() noexcept {
    if(auto fault=guard()) { return *fault; }
    const auto source_status=source->checkpoint();
    if(const auto* fault=std::get_if<BoundaryFailure>(&source_status)) { return latch(*fault); }
    struct Reading {
      explicit Reading(bool& value) noexcept:value(value) { value=true; }
      ~Reading() { value=false; }
      bool& value;
    } active(reading);
    try {
      if(observer->cancellation_requested()) { return latch(BoundaryFailure::Cancelled); }
      // Cancellation is injected code too: a false return may retire the source.
      const auto cancellation_source=source->checkpoint();
      if(const auto* fault=std::get_if<BoundaryFailure>(&cancellation_source)) { return latch(*fault); }
      const auto sample=observer->read_monotonic();
      const auto* actual=std::get_if<rs::util::Deadline>(&sample);
      // Every enum/malformed/exceptional read is permanently safe ReadFailed.
      if(!actual || !finite(*actual)) { return latch(BoundaryFailure::ReadFailed); }
      const auto after=source->checkpoint();
      if(const auto* fault=std::get_if<BoundaryFailure>(&after)) { return latch(*fault); }
      if(observer->cancellation_requested()) { return latch(BoundaryFailure::Cancelled); }
      const auto final_source=source->checkpoint();
      if(const auto* fault=std::get_if<BoundaryFailure>(&final_source)) { return latch(*fault); }
      if(highwater && *actual<*highwater) { return latch(BoundaryFailure::ClockRollback); }
      highwater=*actual;
      if(*actual>=request.deadline()) { return latch(BoundaryFailure::DeadlineElapsed); }
      return std::monostate{};
    } catch(...) { return latch(BoundaryFailure::ReadFailed); }
  }
  BoundaryStatus reject(ResponseStage phase) noexcept {
    if(auto fault=guard()) { return *fault; }
    if(stage!=phase) { return BoundaryFailure::InvalidState; }
    return latch(BoundaryFailure::BodyRejected);
  }
  const Request request;
  const std::shared_ptr<ResponseObservationSource> observer;
  const std::shared_ptr<ResponseSourceGeneration> source;
  const std::thread::id thread;
  ResponseStage stage{ResponseStage::Prepared};
  std::optional<BoundaryFailure> first;
  std::optional<rs::util::Deadline> highwater,initial,completed;
  bool reading{};
  unsigned transport{},processing{};
};
template<class Interface,ResponseStage Phase> class OperationGate final:public Interface {
 public:
  OperationGate(std::shared_ptr<ResponseOperationState> state,const Request& expected):state_(std::move(state)),request_(expected) {}
  void attach() noexcept {
    if(attached_) { std::terminate(); }
    auto& count=Phase==ResponseStage::Transport?state_->transport:state_->processing;
    if(count>=64) { std::terminate(); }
    ++count;attached_=true;
  }
  ~OperationGate() noexcept override {
    if(!state_->same_thread() || state_->reading) { std::terminate(); }
    if(attached_) {
      auto& count=Phase==ResponseStage::Transport?state_->transport:state_->processing;
      if(!count || state_->stage!=Phase) { std::terminate(); }
      --count;
    }
  }
  const Request* request() const noexcept override { return &request_; }
  BoundaryStatus observe_checkpoint() override {
    if(auto fault=state_->guard()) { return *fault; }
    if(!attached_ || state_->stage!=Phase) { return BoundaryFailure::InvalidState; }
    return state_->read();
  }
  BoundaryStatus reject_body_fault() override {
    if(!attached_) { return BoundaryFailure::InvalidState; }
    return state_->reject(Phase);
  }
 private:
  const std::shared_ptr<ResponseOperationState> state_;
  const Request request_;
  bool attached_{};
};
template<class Interface,ResponseStage Phase> BoundaryResult<std::unique_ptr<Interface>> acquire(
    const std::shared_ptr<ResponseOperationState>& state,const Request& expected,std::optional<ResponsePreparationFailure> failure) noexcept {
  if(auto fault=state->guard()) { return *fault; }
  if(expected.binding().invariant_error()) { return BoundaryFailure::InvalidInput; }
  if(!matches(state->request,expected)) { return BoundaryFailure::WrongOperation; }
  if(state->stage!=Phase) { return BoundaryFailure::InvalidState; }
  const auto count=Phase==ResponseStage::Transport?state->transport:state->processing;
  if(count>=64) { return BoundaryFailure::Borrowed; }
  if(failure && *failure!=ResponsePreparationFailure::BeforeGate && *failure!=ResponsePreparationFailure::BeforePermit) { return BoundaryFailure::InvalidInput; }
  try {
    if(failure==ResponsePreparationFailure::BeforeGate) { throw std::bad_alloc{}; }
    // Immutable Request copy and storage allocation precede permit admission.
    auto gate=std::make_unique<OperationGate<Interface,Phase>>(state,expected);
    if(failure==ResponsePreparationFailure::BeforePermit) { throw std::bad_alloc{}; }
    const auto checked=state->read();
    if(const auto* fault=std::get_if<BoundaryFailure>(&checked)) { return *fault; }
    gate->attach(); // Nothing potentially throwing after sole permit admission.
    return std::unique_ptr<Interface>(std::move(gate));
  } catch(...) { return BoundaryFailure::AllocationFailed; }
}
}
BoundaryResult<std::unique_ptr<ResponseOperation>> ResponseOperation::begin(Request request,
    std::shared_ptr<ResponseObservationSource> observer,std::shared_ptr<ResponseSourceGeneration> source) noexcept {
  if(!observer || !source) { return BoundaryFailure::InvalidInput; }
  if(request.binding().invariant_error()) { return BoundaryFailure::InvalidInput; }
  if(request.binding().source()!=source->identity()) { return BoundaryFailure::WrongOperation; }
  const auto source_status=source->checkpoint();
  if(const auto* fault=std::get_if<BoundaryFailure>(&source_status)) { return *fault; }
  try {
    auto checked=Request::create(request.binding(),request.deadline(),request.headroom());
    if(!checked) { return BoundaryFailure::InvalidInput; }
    auto state=std::make_shared<detail::ResponseOperationState>(std::move(checked).value(),std::move(observer),std::move(source));
    auto operation=std::unique_ptr<ResponseOperation>{new ResponseOperation{state}};
    const auto status=state->read();
    if(const auto* fault=std::get_if<BoundaryFailure>(&status)) { return *fault; }
    state->initial=state->highwater;return operation;
  } catch(...) { return BoundaryFailure::AllocationFailed; }
}
ResponseOperation::~ResponseOperation() { if(state_ && (!state_->same_thread() || state_->reading)) { std::terminate(); } }
BoundaryStatus ResponseOperation::begin_transport() noexcept {
  if(auto fault=state_->guard()) { return *fault; }
  if(state_->stage!=detail::ResponseStage::Prepared) { return BoundaryFailure::InvalidState; }
  const auto checked=state_->read();if(std::holds_alternative<BoundaryFailure>(checked)) { return checked; }
  state_->stage=detail::ResponseStage::Transport;return std::monostate{};
}
BoundaryResult<TransportBoundaryLease> ResponseOperation::transport_impl(const Request& expected,std::optional<detail::ResponsePreparationFailure> failure) noexcept {
  auto gate=detail::acquire<detail::TransportBoundaryGate,detail::ResponseStage::Transport>(state_,expected,failure);
  if(const auto* fault=std::get_if<BoundaryFailure>(&gate)) { return *fault; }
  return detail::ResponseLeaseFactory::bind_transport(std::move(std::get<std::unique_ptr<detail::TransportBoundaryGate>>(gate)));
}
BoundaryResult<ProcessingBoundaryLease> ResponseOperation::processing_impl(const Request& expected,std::optional<detail::ResponsePreparationFailure> failure) noexcept {
  auto gate=detail::acquire<detail::ProcessingBoundaryGate,detail::ResponseStage::Processing>(state_,expected,failure);
  if(const auto* fault=std::get_if<BoundaryFailure>(&gate)) { return *fault; }
  return detail::ResponseLeaseFactory::bind_processing(std::move(std::get<std::unique_ptr<detail::ProcessingBoundaryGate>>(gate)));
}
BoundaryResult<TransportBoundaryLease> ResponseOperation::transport_lease(const Request& request) noexcept { return transport_impl(request,{}); }
BoundaryResult<ProcessingBoundaryLease> ResponseOperation::processing_lease(const Request& request) noexcept { return processing_impl(request,{}); }
BoundaryStatus ResponseOperation::complete_transport() noexcept {
  if(auto fault=state_->guard()) { return *fault; }
  if(state_->stage!=detail::ResponseStage::Transport) { return BoundaryFailure::InvalidState; }
  if(state_->transport) { return BoundaryFailure::Borrowed; }
  const auto checked=state_->read();if(std::holds_alternative<BoundaryFailure>(checked)) { return checked; }
  state_->completed=state_->highwater;state_->stage=detail::ResponseStage::Processing;return std::monostate{};
}
BoundaryResult<ResponseCompletion> ResponseOperation::finish_processing() noexcept {
  if(auto fault=state_->guard()) { return *fault; }
  if(state_->stage!=detail::ResponseStage::Processing) { return BoundaryFailure::InvalidState; }
  if(state_->processing) { return BoundaryFailure::Borrowed; }
  try {
    Request request=state_->request; // Potential copy failure BEFORE final sample/stage transition.
    const auto checked=state_->read();if(const auto* fault=std::get_if<BoundaryFailure>(&checked)) { return *fault; }
    ResponseCompletion result{std::move(request),*state_->initial,*state_->completed,*state_->highwater};
    state_->stage=detail::ResponseStage::Finalized;return result;
  } catch(...) { return BoundaryFailure::AllocationFailed; }
}
std::optional<BoundaryFailure> ResponseOperation::fault() const noexcept { return state_->guard(); }
BoundaryResult<TransportBoundaryLease> detail::ResponseOperationTestAccess::refuse_transport(ResponseOperation& operation,const Request& request,ResponsePreparationFailure failure) noexcept { return operation.transport_impl(request,failure); }
BoundaryResult<ProcessingBoundaryLease> detail::ResponseOperationTestAccess::refuse_processing(ResponseOperation& operation,const Request& request,ResponsePreparationFailure failure) noexcept { return operation.processing_impl(request,failure); }
static_assert(std::is_nothrow_move_constructible_v<Request>);
static_assert(std::is_nothrow_move_constructible_v<ResponseCompletion>);
static_assert(std::is_nothrow_move_constructible_v<BoundaryResult<ResponseCompletion>>);
static_assert(std::is_nothrow_move_constructible_v<BoundaryResult<std::unique_ptr<detail::TransportBoundaryGate>>>);
} // namespace rs::core::auth
