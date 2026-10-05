#pragma once

#include "odbcpp/util/result.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace rs::core::database::mysql::time_detail {
// Private duration codec only: no ScalarType::Time or session admission.
struct Profile { unsigned precision{}; };
struct Cell {
  std::optional<std::string> value;
  bool encoding_error{};
  bool time_of_day{};
};
inline rs::util::Result<Profile> metadata(std::uint64_t length,std::uint64_t precision) {
  using rs::util::DbErrorCode;
  if (precision>6) return {DbErrorCode::UnsupportedFeature};
  if (length!=10+(precision?1+precision:0)) return {DbErrorCode::ProtocolError};
  return Profile{static_cast<unsigned>(precision)};
}
inline constexpr std::array<unsigned,7> powers{1,10,100,1000,10000,100000,1000000};
inline Cell invalid_cell() { return {std::string{},true,false}; }
inline bool valid_components(bool negative,std::uint64_t hours,unsigned minute,
    unsigned second,unsigned micros) {
  if (hours>838 || minute>59 || second>59 || micros>999999) return false;
  if (hours==838 && minute==59 && second==59 && micros) return false;
  return !(negative && hours==0 && minute==0 && second==0 && micros==0);
}
// Caller already decoded text framing/NULL. Exact canonical duration bytes
// remain owned; a complete invalid value is never confused with NULL.
inline rs::util::Result<Cell> text_cell(std::optional<std::string_view> input,Profile profile) {
  using rs::util::DbErrorCode;
  if (profile.precision>6) return {DbErrorCode::UnsupportedFeature};
  if (!input) return Cell{std::nullopt,false,false};
  auto value=*input;bool negative{};
  if (!value.empty() && value.front()=='-') { negative=true;value.remove_prefix(1); }
  const auto colon=value.find(':');
  if (colon!=2 && colon!=3) return invalid_cell();
  if (value.size()!=colon+6+(profile.precision?1+profile.precision:0) || value[colon+3]!=':')
    return invalid_cell();
  if (colon==3 && value[0]=='0') return invalid_cell();
  unsigned hours{},minute{},second{},fraction{};
  for (std::size_t i=0;i<value.size();++i) {
    if (i==colon || i==colon+3) continue;
    if (profile.precision && i==colon+6) { if (value[i]!='.') return invalid_cell();continue; }
    if (value[i]<'0' || value[i]>'9') return invalid_cell();
    auto& part=i<colon?hours:(i<colon+3?minute:(i<colon+6?second:fraction));
    part=part*10+static_cast<unsigned>(value[i]-'0');
  }
  const auto micros=fraction*powers[6-profile.precision];
  if (!valid_components(negative,hours,minute,second,micros)) return invalid_cell();
  return Cell{std::string(*input),false,!negative && hours<24};
}
inline std::uint32_t little4(std::span<const std::byte> value) {
  std::uint32_t result{};
  for (unsigned i=0;i<4;++i) result|=std::uint32_t(std::to_integer<unsigned>(value[i]))<<(8*i);
  return result;
}
inline std::string format(bool negative,std::uint64_t hours,unsigned minute,
    unsigned second,unsigned micros,Profile profile) {
  std::string owned=negative?"-":"";
  if (hours<10) owned+='0';
  owned+=std::to_string(hours);
  owned+=':';owned+=static_cast<char>('0'+minute/10);owned+=static_cast<char>('0'+minute%10);
  owned+=':';owned+=static_cast<char>('0'+second/10);owned+=static_cast<char>('0'+second%10);
  if (profile.precision) {
    owned+='.';
    for (unsigned i=0;i<profile.precision;++i) owned+=static_cast<char>('0'+(micros/powers[5-i])%10);
  }
  return owned;
}
// Exactly one binary non-NULL field including its raw length byte. Length is
// not lenenc; the caller retains row framing and retirement responsibility.
inline rs::util::Result<Cell> binary_cell(std::optional<std::span<const std::byte>> input,Profile profile) {
  using rs::util::DbErrorCode;
  if (profile.precision>6) return {DbErrorCode::UnsupportedFeature};
  if (!input) return Cell{std::nullopt,false,false};
  if (input->empty()) return {DbErrorCode::ProtocolError};
  const auto length=std::to_integer<unsigned>((*input)[0]);
  if ((length!=0 && length!=8 && length!=12) || input->size()!=length+1)
    return {DbErrorCode::ProtocolError};
  if (!length) return Cell{format(false,0,0,0,0,profile),false,true};
  const auto sign=std::to_integer<unsigned>((*input)[1]);
  const auto days=little4(input->subspan(2,4));
  const auto hour=std::to_integer<unsigned>((*input)[6]);
  const auto minute=std::to_integer<unsigned>((*input)[7]);
  const auto second=std::to_integer<unsigned>((*input)[8]);
  const auto micros=length==12?little4(input->subspan(9,4)):0u;
  // Widen before multiplying even a malformed maximum uint32 day count.
  const auto hours=std::uint64_t(days)*24+hour;
  if (sign>1 || hour>23 || !valid_components(sign!=0,hours,minute,second,micros) ||
      micros%powers[6-profile.precision]) return invalid_cell();
  return Cell{format(sign!=0,hours,minute,second,micros,profile),false,sign==0 && hours<24};
}
}
