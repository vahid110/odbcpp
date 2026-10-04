#pragma once

#include "date_wire.h"
#include <array>
#include <cstdint>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace rs::core::database::mysql::datetime_detail {
// Private result codec only. TIMESTAMP fields are session-local calendar
// fields, not UTC instants; this codec does not validate storage epoch bounds.
enum class Kind { Datetime, Timestamp };
struct Profile { Kind kind{Kind::Datetime}; unsigned precision{}; };
using Cell = date_detail::Cell;
inline bool valid_profile(Profile profile) {
  return (profile.kind==Kind::Datetime || profile.kind==Kind::Timestamp) && profile.precision<=6;
}
inline rs::util::Result<Profile> metadata(Kind kind,std::uint64_t length,std::uint64_t precision) {
  using rs::util::DbErrorCode;
  if ((kind!=Kind::Datetime && kind!=Kind::Timestamp) || precision>6)
    return {DbErrorCode::UnsupportedFeature};
  if (length!=19+(precision?1+precision:0)) return {DbErrorCode::ProtocolError};
  return Profile{kind,static_cast<unsigned>(precision)};
}
inline constexpr std::array<unsigned,7> powers{1,10,100,1000,10000,100000,1000000};
inline Cell invalid_cell() { return {std::string{},true}; }
inline bool valid_components(unsigned year,unsigned month,unsigned day,unsigned hour,
    unsigned minute,unsigned second,unsigned micros,Profile profile) {
  return valid_profile(profile) && date_detail::valid_components(year,month,day) && hour<=23 && minute<=59 && second<=59 &&
      micros<=999999 && micros%powers[6-profile.precision]==0;
}
// Text framing and NULL decoding belong to the caller. Only exact canonical
// calendar bytes are accepted, with precisely the declared fractional width.
inline rs::util::Result<Cell> text_cell(std::optional<std::string_view> input,Profile profile) {
  using rs::util::DbErrorCode;
  if (!valid_profile(profile)) return {DbErrorCode::UnsupportedFeature};
  if (!input) return Cell{std::nullopt,false};
  const auto value=*input;
  if (value.size()!=19+(profile.precision?1+profile.precision:0)) return invalid_cell();
  if (!date_detail::valid_cell(value.substr(0,10)) || value[10]!=' ' || value[13]!=':' || value[16]!=':' ||
      (profile.precision && value[19]!='.')) return invalid_cell();
  unsigned hour{},minute{},second{},fraction{};
  for (std::size_t i=11;i<value.size();++i) {
    if (i==13 || i==16 || i==19) continue;
    if (value[i]<'0' || value[i]>'9') return invalid_cell();
    auto& part=i<13?hour:(i<16?minute:(i<19?second:fraction));
    part=part*10+static_cast<unsigned>(value[i]-'0');
  }
  if (hour>23 || minute>59 || second>59) return invalid_cell();
  return Cell{std::string(value),false};
}
inline std::string format(unsigned year,unsigned month,unsigned day,unsigned hour,
    unsigned minute,unsigned second,unsigned micros,Profile profile) {
  std::string owned="0000-00-00 00:00:00";
  for (std::size_t i=4;i>0;--i) { owned[i-1]=static_cast<char>('0'+year%10);year/=10; }
  for (const auto& part:{std::pair<std::size_t,unsigned>{5,month},{8,day},{11,hour},{14,minute},{17,second}}) {
    owned[part.first]=static_cast<char>('0'+part.second/10);
    owned[part.first+1]=static_cast<char>('0'+part.second%10);
  }
  if (profile.precision) {
    owned+='.';
    for (unsigned i=0;i<profile.precision;++i) owned+=static_cast<char>('0'+(micros/powers[5-i])%10);
  }
  return owned;
}
// Exactly one non-NULL field, including the raw length byte (not lenenc).
// Complete invalid values are deferred cell errors; malformed framing is fatal
// to the caller. No server data is retained in diagnostics.
inline rs::util::Result<Cell> binary_cell(std::optional<std::span<const std::byte>> input,Profile profile) {
  using rs::util::DbErrorCode;
  if (!valid_profile(profile)) return {DbErrorCode::UnsupportedFeature};
  if (!input) return Cell{std::nullopt,false};
  if (input->empty()) return {DbErrorCode::ProtocolError};
  const auto length=std::to_integer<unsigned>((*input)[0]);
  if ((length!=0 && length!=4 && length!=7 && length!=11) || input->size()!=length+1)
    return {DbErrorCode::ProtocolError};
  if (!length) return invalid_cell();
  const auto year=std::to_integer<unsigned>((*input)[1])|(std::to_integer<unsigned>((*input)[2])<<8);
  const auto month=std::to_integer<unsigned>((*input)[3]);
  const auto day=std::to_integer<unsigned>((*input)[4]);
  const auto hour=length>=7?std::to_integer<unsigned>((*input)[5]):0u;
  const auto minute=length>=7?std::to_integer<unsigned>((*input)[6]):0u;
  const auto second=length>=7?std::to_integer<unsigned>((*input)[7]):0u;
  unsigned micros{};
  if (length==11) for (unsigned i=0;i<4;++i) micros|=std::to_integer<unsigned>((*input)[8+i])<<(8*i);
  if (!valid_components(year,month,day,hour,minute,second,micros,profile)) return invalid_cell();
  return Cell{format(year,month,day,hour,minute,second,micros,profile),false};
}
}
