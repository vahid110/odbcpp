#pragma once
#include "core/auth/aws_db_xml_response.h"
#include <cstdint>
#include <memory>
#include <new>
namespace rs::core::auth::detail::credential_xml {
constexpr std::string_view ns="http://redshift.amazonaws.com/doc/2012-12-01/";
struct Refusal { XmlError error; };
[[noreturn]] inline void fail(XmlFailure f) { throw Refusal{XmlError{f,{}}}; }
class Scratch {
 public:
  ~Scratch(){clear();}
  void clear() noexcept {
    if(bytes){volatile std::byte* p=bytes.get();for(std::size_t i=0;i<SecretBytes::max_bytes;++i)p[i]=std::byte{0};}
    size=0;
  }
  void put(unsigned char c){if(!bytes)bytes=std::make_unique<std::byte[]>(SecretBytes::max_bytes);bytes[size++]=static_cast<std::byte>(c);}
  bool equals(std::string_view value)const {
    if(size!=value.size())return false;
    for(std::size_t i=0;i<size;++i)if(std::to_integer<unsigned char>(bytes[i])!=static_cast<unsigned char>(value[i]))return false;
    return true;
  }
  std::unique_ptr<std::byte[]> bytes;std::size_t size{0};
};
template<class Checkpoint>
class Parser {
 public:
  Parser(std::span<const std::byte> raw,ResponseShape shape,Checkpoint& checkpoint)
      :raw_(raw),shape_(shape),checkpoint_(checkpoint){}
  void gate(){checkpoint_.check();}
  ResponseSnapshot parse(){
    gate();if(starts("\xef\xbb\xbf")){advance();advance();advance();}
    if(starts("<?xml")) { declaration(); }
    ws();
    const auto response=shape_==ResponseShape::ClusterResult?"GetClusterCredentialsResponse":"GetClusterCredentialsWithIAMResponse";
    const auto result=shape_==ResponseShape::ClusterResult?"GetClusterCredentialsResult":"GetClusterCredentialsWithIAMResult";
    auto root=open();if(root!=response) {fail(XmlFailure::InvalidEnvelope);}
    if(!space(peek())) {fail(XmlFailure::InvalidNamespace);}ws();
    if(name()!="xmlns") {fail(XmlFailure::InvalidNamespace);}ws();expect('=');ws();attribute(128);
    if(!scratch_.equals(ns)) {fail(XmlFailure::InvalidNamespace);}scratch_.clear();ws();
    if(peek()!='>') {fail(XmlFailure::InvalidNamespace);}advance();
    std::vector<FieldOccurrence> fields;fields.reserve(ResponseSnapshot::max_fields);
    bool found=false,metadata_seen=false;
    for(;;){
      ws();if(starts("</")){close(response);break;}
      auto child=open();
      if(child==result){if(found) {fail(XmlFailure::DuplicateEnvelope);}found=true;result_body(child,fields);}
      else if(child=="ResponseMetadata"){
        if(metadata_seen) {fail(XmlFailure::DuplicateEnvelope);}metadata_seen=true;metadata(child);
      }else fail(XmlFailure::InvalidEnvelope);
    }
    if(!found) {fail(XmlFailure::InvalidEnvelope);}ws();if(pos_!=raw_.size()) {fail(XmlFailure::InvalidSyntax);}
    auto snapshot=ResponseSnapshot::create(shape_,std::move(fields));
    if(!snapshot)throw Refusal{XmlError{XmlFailure::FieldRejected,snapshot.error()}};
    gate();return std::move(snapshot).value();
  }
 private:
  int peek()const{return pos_==raw_.size()?-1:std::to_integer<unsigned char>(raw_[pos_]);}
  bool starts(std::string_view value)const {
    if(value.size()>raw_.size()-pos_)return false;
    for(std::size_t i=0;i<value.size();++i)if(std::to_integer<unsigned char>(raw_[pos_+i])!=static_cast<unsigned char>(value[i]))return false;
    return true;
  }
  void advance(){if(pos_==raw_.size()) {fail(XmlFailure::InvalidSyntax);}++pos_;if(pos_%256==0)gate();}
  void expect(int c){if(peek()!=c) {fail(XmlFailure::InvalidSyntax);}advance();}
  static bool space(int c){return c==' '||c=='\t'||c=='\n'||c=='\r';}
  void ws(){while(space(peek()))advance();}
  static bool first(int c){return(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_';}
  static bool rest(int c){return first(c)||(c>='0'&&c<='9')||c=='-'||c=='.';}
  std::string name(){
    gate();if(!first(peek())) {fail(XmlFailure::InvalidName);}std::string out;
    while(rest(peek())){if(out.size()==64) {fail(XmlFailure::ResourceLimit);}out.push_back(static_cast<char>(peek()));advance();}
    if(peek()==':'||peek()>=128) {fail(XmlFailure::InvalidName);}
    if(peek()>=0 && !space(peek()) && peek()!='>' && peek()!='/' && peek()!='?' && peek()!='=') { fail(XmlFailure::InvalidName); }
    return out;
  }
  std::string open(){
    gate();expect('<');if(peek()=='!'||peek()=='?') {fail(XmlFailure::UnsupportedMarkup);}
    if(++elements_>32) {fail(XmlFailure::ResourceLimit);}return name();
  }
  void close(std::string_view expected){expect('<');expect('/');if(name()!=expected) {fail(XmlFailure::InvalidSyntax);}ws();expect('>');}
  bool finish_open(){
    ws();if(peek()=='/'){advance();expect('>');return true;}
    if(peek()!='>') {fail(XmlFailure::UnsupportedMarkup);}advance();return false;
  }
  static bool xmlchar(std::uint32_t cp){return cp==9||cp==10||cp==13||(cp>=0x20&&cp<=0xd7ff)||(cp>=0xe000&&cp<=0xfffd)||(cp>=0x10000&&cp<=0x10ffff);}
  void emit(unsigned char c,std::size_t limit){
    if(scratch_.size==limit||decoded_==ResponseSnapshot::max_aggregate_bytes) {fail(XmlFailure::ResourceLimit);}
    scratch_.put(c);++decoded_;
  }
  void scalar(std::uint32_t cp,std::size_t limit){
    if(!xmlchar(cp)) {fail(XmlFailure::InvalidEntity);}
    if(cp<0x80)emit(static_cast<unsigned char>(cp),limit);
    else if(cp<0x800){emit(static_cast<unsigned char>(0xc0|(cp>>6)),limit);emit(static_cast<unsigned char>(0x80|(cp&63)),limit);}
    else if(cp<0x10000){emit(static_cast<unsigned char>(0xe0|(cp>>12)),limit);emit(static_cast<unsigned char>(0x80|((cp>>6)&63)),limit);emit(static_cast<unsigned char>(0x80|(cp&63)),limit);}
    else{emit(static_cast<unsigned char>(0xf0|(cp>>18)),limit);emit(static_cast<unsigned char>(0x80|((cp>>12)&63)),limit);emit(static_cast<unsigned char>(0x80|((cp>>6)&63)),limit);emit(static_cast<unsigned char>(0x80|(cp&63)),limit);}
  }
  void entity(std::size_t limit){
    expect('&');std::string token;
    while(peek()!=';'){
      if(peek()<0||peek()=='<'||space(peek())) {fail(XmlFailure::InvalidEntity);}
      if(token.size()==10) {fail(XmlFailure::ResourceLimit);}token.push_back(static_cast<char>(peek()));advance();
    }advance();
    if(token=="amp")scalar('&',limit);else if(token=="lt")scalar('<',limit);else if(token=="gt")scalar('>',limit);
    else if(token=="apos")scalar(39,limit);else if(token=="quot")scalar('"',limit);
    else if(!token.empty()&&token[0]=='#'){
      std::size_t p=1;unsigned base=10;if(p<token.size()&&token[p]=='x'){base=16;++p;}
      if(p==token.size()) {fail(XmlFailure::InvalidEntity);}std::uint32_t cp=0;
      for(;p<token.size();++p){const char c=token[p];unsigned d;
        if(c>='0'&&c<='9')d=static_cast<unsigned>(c-'0');else if(base==16&&c>='a'&&c<='f')d=static_cast<unsigned>(c-'a'+10);
        else if(base==16&&c>='A'&&c<='F')d=static_cast<unsigned>(c-'A'+10);else fail(XmlFailure::InvalidEntity);
        if(d>=base||cp>(0x10ffff-d)/base) {fail(XmlFailure::InvalidEntity);}cp=cp*base+d;
      }scalar(cp,limit);
    }else fail(XmlFailure::InvalidEntity);
  }
  void character(std::size_t limit){
    int c=peek();if(c<0) {fail(XmlFailure::InvalidSyntax);}
    if(c=='&'){entity(limit);return;}
    if(c=='<') {fail(XmlFailure::InvalidSyntax);}
    if(starts("]]>")) {fail(XmlFailure::InvalidSyntax);}
    advance();
    if(c=='\r'){if(peek()=='\n')advance();scalar(10,limit);return;}
    if(c<128){if(!xmlchar(static_cast<std::uint32_t>(c))) {fail(XmlFailure::InvalidSyntax);}scalar(static_cast<std::uint32_t>(c),limit);return;}
    unsigned count;std::uint32_t cp,minimum;
    if(c>=0xc2&&c<=0xdf){count=1;cp=static_cast<unsigned>(c&31);minimum=0x80;}
    else if(c>=0xe0&&c<=0xef){count=2;cp=static_cast<unsigned>(c&15);minimum=0x800;}
    else if(c>=0xf0&&c<=0xf4){count=3;cp=static_cast<unsigned>(c&7);minimum=0x10000;}else fail(XmlFailure::InvalidUtf8);
    for(unsigned i=0;i<count;++i){int t=peek();if(t<0x80||t>0xbf) {fail(XmlFailure::InvalidUtf8);}advance();cp=(cp<<6)|static_cast<unsigned>(t&63);}
    if(cp<minimum||!xmlchar(cp)) {fail(XmlFailure::InvalidUtf8);}scalar(cp,limit);
  }
  void attribute(std::size_t limit,bool references=true){
    scratch_.clear();const int quote=peek();if(quote!=39&&quote!='"') {fail(XmlFailure::InvalidSyntax);}advance();
    while(peek()!=quote) {
      if(!references && peek()=='&') { fail(XmlFailure::UnsupportedMarkup); }
      character(limit);
    }
    advance();
  }
  void declaration(){
    for(char c:std::string_view("<?xml"))expect(c);
    if(!space(peek())) {fail(XmlFailure::UnsupportedMarkup);}ws();
    if(name()!="version") {fail(XmlFailure::UnsupportedMarkup);}ws();expect('=');ws();attribute(16,false);
    if(!scratch_.equals("1.0")) {fail(XmlFailure::UnsupportedMarkup);}scratch_.clear();
    if(space(peek())){ws();if(!starts("?>")){
      if(name()!="encoding") {fail(XmlFailure::UnsupportedMarkup);}ws();expect('=');ws();attribute(16,false);
      if(!scratch_.equals("UTF-8")) {fail(XmlFailure::UnsupportedMarkup);}scratch_.clear();ws();
    }}expect('?');expect('>');
  }
  SecretBytes leaf(const std::string& tag,std::size_t limit){
    scratch_.clear();if(!finish_open()){
      while(peek()!='<')character(limit);
      if(!starts("</")) {fail(XmlFailure::UnsupportedMarkup);}close(tag);
    }
    auto bytes=SecretBytes::create({scratch_.bytes.get(),scratch_.size});scratch_.clear();
    if(!bytes) {fail(XmlFailure::AllocationFailed);}return std::move(bytes).value();
  }
  void result_body(const std::string& tag,std::vector<FieldOccurrence>& fields){
    if(finish_open())return;
    for(;;){ws();if(starts("</")){close(tag);return;}
      if(fields.size()==ResponseSnapshot::max_fields) {fail(XmlFailure::ResourceLimit);}
      auto key=open();auto bytes=leaf(key,SecretBytes::max_bytes);
      auto field=FieldOccurrence::create(std::move(key),TextBytes{std::move(bytes)});
      if(!field)throw Refusal{XmlError{XmlFailure::FieldRejected,field.error()}};
      fields.push_back(std::move(field).value());
    }
  }
  void metadata(const std::string& tag){
    if(finish_open()) { return; }
    bool seen=false;
    for(;;){ws();if(starts("</")){close(tag);return;}
      auto key=open();if(key!="RequestId") {fail(XmlFailure::InvalidEnvelope);}if(seen) {fail(XmlFailure::DuplicateEnvelope);}seen=true;
      auto discarded=leaf(key,128);
    }
  }
  std::span<const std::byte> raw_;ResponseShape shape_;Checkpoint& checkpoint_;
  std::size_t pos_{0},elements_{0},decoded_{0};Scratch scratch_;
};
}
