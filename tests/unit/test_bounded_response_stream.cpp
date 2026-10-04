#include "core/auth/bounded_response_stream.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
using namespace rs::core::auth;
namespace {
using namespace std::chrono_literals;
struct Clock final : MonotonicClock {
  rs::util::Deadline value{1s};
  int calls{0}, change_at{0};
  rs::util::Deadline changed{};
  rs::util::Deadline now() noexcept override {
    if (++calls == change_at) value = changed;
    return value;
  }
};
struct Stop final : Cancellation {
  bool stopped{false};
  bool stop_requested() const noexcept override { return stopped; }
};
StreamOwner owner(Clock& clock, std::size_t cap = 16, Stop* stop = nullptr) {
  auto result = StreamOwner::create(cap, rs::util::Deadline(10s), clock, stop);
  if (!std::holds_alternative<StreamOwner>(result)) throw std::logic_error("Test owner creation failed");
  return std::move(std::get<StreamOwner>(result));
}
std::string text(const ResponseBytes& bytes) {
  std::string out;
  bytes.with_bytes([&](auto view) { out.assign(reinterpret_cast<const char*>(view.data()), view.size()); });
  return out;
}
TEST(BoundedResponseStream, IncrementalRawBytesAndReadCursorOwnExactBody) {
  const std::string input("a\0b\xffz",5);
  for (std::size_t chunk = 1; chunk <= input.size(); ++chunk) {
    Clock clock; auto body = owner(clock, input.size());
    auto* io = body.io();
    for (std::size_t p = 0; p < input.size(); p += chunk)
      io->write(input.data()+p, static_cast<std::streamsize>(std::min(chunk,input.size()-p)));
    EXPECT_EQ(io->tellp(), std::streampos(5));
    char first{}; io->get(first); EXPECT_EQ(first,'a');
    EXPECT_EQ(io->tellg(), std::streampos(1));
    io->seekg(0); EXPECT_EQ(io->tellg(),std::streampos(0));
    io->seekg(5); EXPECT_EQ(io->tellg(),std::streampos(5));
    auto result = seal_response(std::move(body));
    ASSERT_TRUE(std::holds_alternative<ResponseBytes>(result));
    EXPECT_EQ(text(std::get<ResponseBytes>(result)),input);
    EXPECT_EQ(body.io(),nullptr);
  }
}
TEST(BoundedResponseStream, ExactMaximumAndEmptySealAreDistinctFromConsumed) {
  Clock c; auto b = owner(c,StreamOwner::max_bytes);
  std::string input(StreamOwner::max_bytes,'x'); b.io()->write(input.data(),input.size());
  auto r=seal_response(std::move(b)); ASSERT_TRUE(std::holds_alternative<ResponseBytes>(r));
  EXPECT_EQ(std::get<ResponseBytes>(r).size(),input.size());
  auto empty=owner(c); auto e=seal_response(std::move(empty));
  ASSERT_TRUE(std::holds_alternative<ResponseBytes>(e)); EXPECT_EQ(text(std::get<ResponseBytes>(e)),"");
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(empty))),StreamFailure::AlreadyConsumed);
}
TEST(BoundedResponseStream, OversizedChunkFailsAtomicallyAndCannotBeCleared) {
  Clock c; auto b=owner(c,3); auto* io=b.io(); io->write("ab",2);
  EXPECT_EQ(io->rdbuf()->sputn("cd",2),0); EXPECT_EQ(b.failure(),StreamFailure::LimitExceeded);
  io->clear(); io->write("z",1); io->clear(); io->flush(); io->clear();
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(b))),StreamFailure::LimitExceeded);
}
TEST(BoundedResponseStream, SingleByteOverflowAndExtremeCountLatchWithoutCopy) {
  Clock c; auto b=owner(c,1); b.io()->put('a'); b.io()->put('b');
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(b))),StreamFailure::LimitExceeded);
  auto huge=owner(c); EXPECT_EQ(huge.io()->rdbuf()->sputn("a",std::numeric_limits<std::streamsize>::max()),0);
  EXPECT_EQ(huge.failure(),StreamFailure::LimitExceeded);
}
TEST(BoundedResponseStream, ZeroWriteAndSyncDoNotAddTerminator) {
  Clock c; auto b=owner(c); b.io()->write("",0); b.io()->flush();
  auto r=seal_response(std::move(b)); ASSERT_TRUE(std::holds_alternative<ResponseBytes>(r));
  EXPECT_TRUE(std::get<ResponseBytes>(r).empty());
}
TEST(BoundedResponseStream, ForbiddenSeekNoHolesAndCheckedArithmetic) {
  for (int which=0;which<5;++which) {
    Clock c; auto b=owner(c); b.io()->write("abc",3);
    auto* sb=b.io()->rdbuf();
    std::streampos position;
    if (which==0) position=sb->pubseekoff(1,std::ios::end,std::ios::in);
    else if (which==1) position=sb->pubseekoff(-1,std::ios::beg,std::ios::in);
    else if (which==2) position=sb->pubseekoff(std::numeric_limits<std::streamoff>::max(),std::ios::end,std::ios::in);
    else if (which==3) position=sb->pubseekoff(0,std::ios::beg,std::ios::out);
    else position=sb->pubseekoff(0,std::ios::cur,std::ios::in|std::ios::out);
    EXPECT_EQ(position,std::streampos(-1)); b.io()->clear();
    EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(b))),StreamFailure::InvalidSeek);
  }
}
TEST(BoundedResponseStream, OrdinaryReadEofAllowedButFabricatedEofAndBadStateRefused) {
  Clock c; auto b=owner(c); b.io()->write("a",1); char output[2]{}; b.io()->read(output,2);
  EXPECT_TRUE(b.io()->eof()); auto r=seal_response(std::move(b));
  ASSERT_TRUE(std::holds_alternative<ResponseBytes>(r)); EXPECT_EQ(text(std::get<ResponseBytes>(r)),"a");
  for(auto flags:{std::ios::failbit,std::ios::badbit,std::ios::failbit|std::ios::eofbit}) {
    auto failed=owner(c); failed.io()->setstate(flags);
    EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(failed))),StreamFailure::StreamFailed);
  }
}
TEST(BoundedResponseStream, OldEofCannotAuthorizeFlagsAfterAppendButNewEndCan) {
  for (bool fresh_read : {false,true}) {
    Clock c; auto b=owner(c); auto* io=b.io(); io->write("a",1);
    char out[2]{}; io->read(out,2); ASSERT_TRUE(io->eof());
    io->clear(); io->write("b",1);
    if (fresh_read) io->read(out,2); // Cursor is at old end: reads b, then new EOF.
    else io->setstate(std::ios::failbit|std::ios::eofbit);
    auto result=seal_response(std::move(b));
    if (fresh_read) {
      ASSERT_TRUE(std::holds_alternative<ResponseBytes>(result));
      EXPECT_EQ(text(std::get<ResponseBytes>(result)),"ab");
    } else {
      ASSERT_TRUE(std::holds_alternative<StreamFailure>(result));
      EXPECT_EQ(std::get<StreamFailure>(result),StreamFailure::StreamFailed);
    }
  }
}
TEST(BoundedResponseStream, NonEofReadInvalidatesOldEndEvidence) {
  Clock c; auto b=owner(c); auto* io=b.io(); io->write("a",1);
  char out[2]{}; io->read(out,2); ASSERT_TRUE(io->eof());
  io->clear(); // Exercise the buffer callback; istream may optimize read(out,0).
  EXPECT_EQ(io->rdbuf()->sgetn(out,0),0);
  io->setstate(std::ios::failbit|std::ios::eofbit);
  auto result=seal_response(std::move(b));
  ASSERT_TRUE(std::holds_alternative<StreamFailure>(result));
  EXPECT_EQ(std::get<StreamFailure>(result),StreamFailure::StreamFailed);
}
TEST(BoundedResponseStream, MovePreservesBorrowedAddressAndSealExpiresIt) {
  Clock c; auto b=owner(c); auto* io=b.io(); auto* buffer=io->rdbuf();
  auto moved=std::move(b); EXPECT_EQ(b.io(),nullptr); EXPECT_EQ(moved.io(),io); EXPECT_EQ(moved.io()->rdbuf(),buffer);
  io->write("secret",6); auto r=seal_response(std::move(moved));
  ASSERT_TRUE(std::holds_alternative<ResponseBytes>(r)); EXPECT_EQ(text(std::get<ResponseBytes>(r)),"secret");
  EXPECT_EQ(moved.io(),nullptr); // Never dereference expired io/buffer.
}
TEST(BoundedResponseStream, BytesMoveClearAndThrowingConsumerMaintainClosedOwnership) {
  Clock c; auto b=owner(c); b.io()->write("secret",6); auto r=seal_response(std::move(b));
  auto bytes=std::move(std::get<ResponseBytes>(r));
  EXPECT_THROW(std::get<ResponseBytes>(r).with_bytes([](auto){}),std::logic_error);
  EXPECT_THROW(bytes.with_bytes([](auto){throw std::runtime_error("consumer");}),std::runtime_error);
  EXPECT_EQ(text(bytes),"secret");
  bytes.with_bytes([&](auto){EXPECT_FALSE(bytes.clear()); EXPECT_THROW(ResponseBytes(std::move(bytes)),std::logic_error);});
  EXPECT_TRUE(bytes.clear()); EXPECT_THROW(bytes.with_bytes([](auto){}),std::logic_error);
}
TEST(BoundedResponseStream, CancelBeforeCreationWriteAndSealConsumesOwner) {
  Clock c; Stop stop; stop.stopped=true;
  EXPECT_EQ(std::get<StreamFailure>(StreamOwner::create(3,rs::util::Deadline(10s),c,&stop)),StreamFailure::Cancelled);
  stop.stopped=false; auto b=owner(c,16,&stop); b.io()->write("secret",6); stop.stopped=true;
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(b))),StreamFailure::Cancelled); EXPECT_EQ(b.io(),nullptr);
  stop.stopped=false; auto late=owner(c,16,&stop); stop.stopped=true; late.io()->write("s",1); late.io()->clear(); stop.stopped=false;
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(late))),StreamFailure::Cancelled);
}
TEST(BoundedResponseStream, ExclusiveDeadlineAndRollbackCannotExtendBound) {
  Clock c; auto b=owner(c); b.io()->write("a",1); c.value=rs::util::Deadline(10s);
  b.io()->write("b",1); b.io()->clear(); c.value=rs::util::Deadline(2s);
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(b))),StreamFailure::DeadlineElapsed);
  c.value=rs::util::Deadline(3s); auto rollback=owner(c); c.value=rs::util::Deadline(2s);
  rollback.io()->flush(); rollback.io()->clear(); c.value=rs::util::Deadline(4s);
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(rollback))),StreamFailure::ClockRollback);
}
TEST(BoundedResponseStream, SealDeadlineEqualityAndReadCancellationRefuse) {
  Clock c; auto body=owner(c); body.io()->write("secret",6); c.value=rs::util::Deadline(10s);
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(body))),StreamFailure::DeadlineElapsed);
  c.value=rs::util::Deadline(1s); Stop stop; auto read=owner(c,16,&stop); read.io()->write("secret",6);
  stop.stopped=true; char value{}; read.io()->get(value); read.io()->clear(); stop.stopped=false;
  EXPECT_EQ(std::get<StreamFailure>(seal_response(std::move(read))),StreamFailure::Cancelled);
}
TEST(BoundedResponseStream, PostAllocationPublicationSamplesOriginalDeadline) {
  Clock c; c.change_at=2; c.changed=rs::util::Deadline(10s);
  EXPECT_EQ(std::get<StreamFailure>(StreamOwner::create(16,rs::util::Deadline(10s),c,nullptr)),StreamFailure::DeadlineElapsed);
}
TEST(BoundedResponseStream, InvalidResourceBoundsAndInfiniteDeadlineRefused) {
  Clock c;
  for(auto cap:{std::size_t(0),StreamOwner::max_bytes+1,std::numeric_limits<std::size_t>::max()})
    EXPECT_EQ(std::get<StreamFailure>(StreamOwner::create(cap,rs::util::Deadline(10s),c,nullptr)),StreamFailure::InvalidLimit);
  EXPECT_EQ(std::get<StreamFailure>(StreamOwner::create(1,rs::util::Deadline::max(),c,nullptr)),StreamFailure::InvalidDeadline);
}
TEST(BoundedResponseStream, ExceptionMaskAndSafeErrorsPreserveFailureWithoutBody) {
  Clock c; auto b=owner(c,1); b.io()->exceptions(std::ios::badbit|std::ios::failbit);
  EXPECT_THROW(b.io()->write("SECRET_BODY",11),std::ios_base::failure);
  auto r=seal_response(std::move(b)); ASSERT_TRUE(std::holds_alternative<StreamFailure>(r));
  EXPECT_EQ(std::get<StreamFailure>(r),StreamFailure::LimitExceeded);
  EXPECT_EQ(stream_safe_message(std::get<StreamFailure>(r)).find("SECRET_BODY"),std::string_view::npos);
}
}
