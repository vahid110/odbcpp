#include "core/auth/aws_db_json_response.h"
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
namespace rs::core::auth {
std::string_view JsonError::safe_message() const noexcept {
  switch (failure) {
    case JsonFailure::UnsupportedOperation: return "JSON credential operation unsupported";
    case JsonFailure::InvalidShape: return "JSON credential shape invalid";
    case JsonFailure::InvalidBody: return "JSON credential body invalid";
    case JsonFailure::InvalidDeadline: return "JSON credential deadline invalid";
    case JsonFailure::ResourceLimit: return "JSON credential resource limit";
    case JsonFailure::InvalidSyntax: return "JSON credential syntax invalid";
    case JsonFailure::InvalidUtf8: return "JSON credential UTF8 invalid";
    case JsonFailure::InvalidEscape: return "JSON credential escape invalid";
    case JsonFailure::FieldRejected: return "JSON credential field rejected";
    case JsonFailure::Cancelled: return "JSON credential cancelled";
    case JsonFailure::ClockRollback: return "JSON credential clock rollback";
    case JsonFailure::DeadlineElapsed: return "JSON credential deadline elapsed";
    case JsonFailure::AllocationFailed: return "JSON credential allocation failed";
  }
  return "JSON credential failure invalid";
}
namespace {
struct Refusal { JsonError error; };
[[noreturn]] void refuse(JsonFailure f) { throw Refusal{JsonError{f,{}}}; }
void wipe(std::byte* p, std::size_t n) noexcept {
  volatile std::byte* out=p;
  for(std::size_t i=0;i<n;++i) out[i]=std::byte{0};
}
class Scratch {
 public:
  ~Scratch() { clear(); }
  void clear() noexcept { if(bytes) wipe(bytes.get(),SecretBytes::max_bytes); used=0; }
  void put(unsigned char c) {
    if(!bytes) bytes=std::make_unique<std::byte[]>(SecretBytes::max_bytes);
    bytes[used++]=static_cast<std::byte>(c);
  }
  std::unique_ptr<std::byte[]> bytes;
  std::size_t used{0};
};
class Parser {
 public:
  Parser(std::span<const std::byte> raw, rs::util::Deadline d,
         rs::util::Deadline checkpoint, MonotonicClock& clock, const Cancellation* stop)
      : raw_(raw), deadline_(d), highwater_(checkpoint), clock_(clock), stop_(stop) {}
  void gate() {
    if(stop_ && stop_->stop_requested()) refuse(JsonFailure::Cancelled);
    const auto now=clock_.now();
    if(now<highwater_) refuse(JsonFailure::ClockRollback);
    highwater_=now;
    if(now>=deadline_) refuse(JsonFailure::DeadlineElapsed);
  }
  ResponseSnapshot parse() {
    gate(); ws(); expect('{'); nodes_=1; ws();
    std::vector<FieldOccurrence> fields; fields.reserve(ResponseSnapshot::max_fields);
    if(peek()!='}') {
      for(;;) {
        if(fields.size()==ResponseSnapshot::max_fields) refuse(JsonFailure::ResourceLimit);
        auto key=key_string(); ws(); expect(':'); ws();
        auto atom=value(1,true);
        auto field=FieldOccurrence::create(std::move(key),std::move(atom));
        if(!field) throw Refusal{JsonError{JsonFailure::FieldRejected,field.error()}};
        fields.push_back(std::move(field).value()); ws();
        if(peek()!=',') break;
        advance(); ws();
      }
    }
    expect('}'); ws(); if(pos_!=raw_.size()) refuse(JsonFailure::InvalidSyntax);
    auto snapshot=ResponseSnapshot::create(ResponseShape::ServerlessObject,std::move(fields));
    if(!snapshot) throw Refusal{JsonError{JsonFailure::FieldRejected,snapshot.error()}};
    gate(); return std::move(snapshot).value();
  }
 private:
  int peek() const { return pos_==raw_.size() ? -1 : std::to_integer<unsigned char>(raw_[pos_]); }
  void advance() { if(pos_==raw_.size()) refuse(JsonFailure::InvalidSyntax); ++pos_; if(pos_%256==0) gate(); }
  int take() { const int c=peek(); advance(); return c; }
  void expect(int c) { if(peek()!=c) refuse(JsonFailure::InvalidSyntax); advance(); }
  void ws() { while(peek()==' ' || peek()=='\t' || peek()=='\n' || peek()=='\r') advance(); }
  static bool digit(int c) { return c>='0' && c<='9'; }
  void emit(unsigned char c,std::size_t limit) {
    if(scratch_.used==limit || decoded_==ResponseSnapshot::max_aggregate_bytes) refuse(JsonFailure::ResourceLimit);
    scratch_.put(c); ++decoded_;
  }
  void scalar(std::uint32_t cp,std::size_t limit) {
    if(cp<0x80) emit(static_cast<unsigned char>(cp),limit);
    else if(cp<0x800) { emit(static_cast<unsigned char>(0xc0|(cp>>6)),limit); emit(static_cast<unsigned char>(0x80|(cp&63)),limit); }
    else if(cp<0x10000) {
      emit(static_cast<unsigned char>(0xe0|(cp>>12)),limit); emit(static_cast<unsigned char>(0x80|((cp>>6)&63)),limit); emit(static_cast<unsigned char>(0x80|(cp&63)),limit);
    } else {
      emit(static_cast<unsigned char>(0xf0|(cp>>18)),limit); emit(static_cast<unsigned char>(0x80|((cp>>12)&63)),limit);
      emit(static_cast<unsigned char>(0x80|((cp>>6)&63)),limit); emit(static_cast<unsigned char>(0x80|(cp&63)),limit);
    }
  }
  std::uint32_t hex4() {
    std::uint32_t cp=0;
    for(int i=0;i<4;++i) {
      const int c=peek(); unsigned v;
      if(c>='0'&&c<='9') v=static_cast<unsigned>(c-'0');
      else if(c>='a'&&c<='f') v=static_cast<unsigned>(c-'a'+10);
      else if(c>='A'&&c<='F') v=static_cast<unsigned>(c-'A'+10);
      else refuse(JsonFailure::InvalidEscape);
      advance(); cp=(cp<<4)|v;
    }
    return cp;
  }
  void string(std::size_t limit) {
    gate(); scratch_.clear(); expect('"');
    while(peek()!='"') {
      const int c=peek(); if(c<0 || c<0x20) refuse(JsonFailure::InvalidSyntax);
      advance();
      if(c=='\\') {
        const int e=peek(); if(e<0) refuse(JsonFailure::InvalidEscape); advance();
        switch(e) {
          case '"': case '\\': case '/': emit(static_cast<unsigned char>(e),limit); break;
          case 'b': emit(8,limit); break; case 'f': emit(12,limit); break;
          case 'n': emit(10,limit); break; case 'r': emit(13,limit); break; case 't': emit(9,limit); break;
          case 'u': {
            auto cp=hex4();
            if(cp>=0xd800 && cp<=0xdbff) {
              if(peek()!='\\') { refuse(JsonFailure::InvalidEscape); }
              advance();
              if(peek()!='u') { refuse(JsonFailure::InvalidEscape); }
              advance();
              const auto low=hex4(); if(low<0xdc00||low>0xdfff) refuse(JsonFailure::InvalidEscape);
              cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00);
            } else if(cp>=0xdc00&&cp<=0xdfff) refuse(JsonFailure::InvalidEscape);
            scalar(cp,limit); break;
          }
          default: refuse(JsonFailure::InvalidEscape);
        }
      } else if(c<0x80) emit(static_cast<unsigned char>(c),limit);
      else {
        unsigned count; std::uint32_t cp,minimum;
        if(c>=0xc2&&c<=0xdf) {count=1;cp=static_cast<unsigned>(c&31);minimum=0x80;}
        else if(c>=0xe0&&c<=0xef) {count=2;cp=static_cast<unsigned>(c&15);minimum=0x800;}
        else if(c>=0xf0&&c<=0xf4) {count=3;cp=static_cast<unsigned>(c&7);minimum=0x10000;}
        else refuse(JsonFailure::InvalidUtf8);
        for(unsigned i=0;i<count;++i) {int t=peek();if(t<0x80||t>0xbf) refuse(JsonFailure::InvalidUtf8);advance();cp=(cp<<6)|static_cast<unsigned>(t&63);}
        if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff)) refuse(JsonFailure::InvalidUtf8);
        scalar(cp,limit);
      }
    }
    advance();
  }
  std::string key_string() {
    string(ResponseSnapshot::max_key_bytes);
    std::string key;
    if(scratch_.used) key.assign(reinterpret_cast<const char*>(scratch_.bytes.get()),scratch_.used);
    scratch_.clear(); return key;
  }
  FieldAtom number(bool retain) {
    gate(); const auto start=pos_;
    auto next=[&] {advance();if(pos_-start>ResponseSnapshot::max_lexeme_bytes) refuse(JsonFailure::ResourceLimit);};
    if(peek()=='-') next();
    if(peek()=='0') next();
    else {if(peek()<'1'||peek()>'9') refuse(JsonFailure::InvalidSyntax); do{next();}while(digit(peek()));}
    if(peek()=='.') {next();if(!digit(peek())) refuse(JsonFailure::InvalidSyntax);do{next();}while(digit(peek()));}
    if(peek()=='e'||peek()=='E') {next();if(peek()=='+'||peek()=='-') next();if(!digit(peek())) refuse(JsonFailure::InvalidSyntax);do{next();}while(digit(peek()));}
    if(retain) return NumberLexeme{std::string(reinterpret_cast<const char*>(raw_.data()+start),pos_-start)};
    return NumberLexeme{};
  }
  void literal(std::string_view word) { for(char c:word) expect(c); }
  FieldAtom value(unsigned depth,bool retain) {
    gate(); if(++nodes_>128) refuse(JsonFailure::ResourceLimit);
    const int c=peek();
    if(c=='"') {
      string(SecretBytes::max_bytes);
      if(!retain) {scratch_.clear();return NullAtom{};}
      auto text=SecretBytes::create({scratch_.bytes.get(),scratch_.used}); scratch_.clear();
      if(!text) refuse(JsonFailure::AllocationFailed);
      return TextBytes{std::move(text).value()};
    }
    if(c=='-'||digit(c)) return number(retain);
    if(c=='n') {literal("null");return NullAtom{};}
    if(c=='t') {literal("true");return BooleanAtom{};}
    if(c=='f') {literal("false");return BooleanAtom{};}
    if(c=='{'||c=='[') {
      if(depth>=8) refuse(JsonFailure::ResourceLimit);
      const bool object=c=='{';advance();ws();const int end=object?'}':']';
      if(peek()!=end) for(;;) {
        if(object) {key_string();ws();expect(':');ws();}
        value(depth+1,false);ws();if(peek()!=',') break;advance();ws();
      }
      expect(end);return object ? FieldAtom{ObjectAtom{}} : FieldAtom{ArrayAtom{}};
    }
    refuse(JsonFailure::InvalidSyntax);
  }
  std::span<const std::byte> raw_;
  rs::util::Deadline deadline_,highwater_;
  MonotonicClock& clock_; const Cancellation* stop_;
  std::size_t pos_{0},decoded_{0},nodes_{0}; Scratch scratch_;
};
}
JsonOutcome<ResponseSnapshot> parse_aws_db_json_response(DbCredentialOperation operation,
    ResponseShape shape,ResponseBytes&& supplied,rs::util::Deadline deadline,
    rs::util::Deadline checkpoint,MonotonicClock& clock,const Cancellation* stop) {
  try {
    ResponseBytes body(std::move(supplied));
    if(operation!=DbCredentialOperation::ServerlessGetCredentials) return JsonError{JsonFailure::UnsupportedOperation,{}};
    if(shape!=ResponseShape::ServerlessObject) return JsonError{JsonFailure::InvalidShape,{}};
    if(body.size()>StreamOwner::max_bytes) return JsonError{JsonFailure::ResourceLimit,{}};
    if(deadline==rs::util::Deadline::max()) return JsonError{JsonFailure::InvalidDeadline,{}};
    std::optional<ResponseSnapshot> result;
    body.with_bytes([&](auto bytes) {Parser parser(bytes,deadline,checkpoint,clock,stop);result.emplace(parser.parse());});
    return std::move(*result);
  } catch(const Refusal& r) {return r.error;}
  catch(const std::bad_alloc&) {return JsonError{JsonFailure::AllocationFailed,{}};}
  catch(...) {return JsonError{JsonFailure::InvalidBody,{}};}
}
} // namespace rs::core::auth
