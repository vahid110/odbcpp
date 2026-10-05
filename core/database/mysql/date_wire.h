#pragma once

#include "odbcpp/util/result.h"
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace rs::core::database::mysql::date_detail {
// Private result-only DATE profile; native qualification is separate.
struct Cell {
  std::optional<std::string> value;
  bool encoding_error{};
};
inline bool valid_components(unsigned year,unsigned month,unsigned day) {
  constexpr std::array<unsigned,12> days{31,28,31,30,31,30,31,31,30,31,30,31};
  if (year<1000 || year>9999 || month<1 || month>12 || day<1) return false;
  const bool leap=year%4==0 && (year%100!=0 || year%400==0);
  return day<=days[month-1]+(month==2 && leap?1u:0u);
}
inline bool valid_cell(std::string_view value) {
  if (value.size()!=10 || value[4]!='-' || value[7]!='-') return false;
  unsigned year{},month{},day{};
  for (std::size_t i=0;i<value.size();++i) {
    if (i==4 || i==7) continue;
    if (value[i]<'0' || value[i]>'9') return false;
    auto& part=i<4?year:(i<7?month:day);
    part=part*10+static_cast<unsigned>(value[i]-'0');
  }
  return valid_components(year,month,day);
}
// Caller has already decoded text framing / binary NULL bitmap. Absent input
// alone is NULL; empty input and zero-date values are never coerced to NULL.
inline Cell text_cell(std::optional<std::string_view> value) {
  if (!value) return {std::nullopt,false};
  if (!valid_cell(*value)) return {std::string{},true};
  return {std::string(*value),false};
}
// Exactly one framed, non-NULL DATE value, including its raw length byte.
// Caller retains row framing and session retirement/deadline responsibility.
inline rs::util::Result<Cell> binary_cell(std::optional<std::span<const std::byte>> value) {
  using rs::util::DbErrorCode;
  if (!value) return Cell{std::nullopt,false};
  if (value->empty()) return {DbErrorCode::ProtocolError};
  const auto length=std::to_integer<unsigned>((*value)[0]);
  if (length!=0 && length!=4 && length!=7 && length!=11) return {DbErrorCode::ProtocolError};
  if (value->size()!=length+1) return {DbErrorCode::ProtocolError};
  if (length==7 || length==11) return {DbErrorCode::UnsupportedFeature};
  if (length==0) return Cell{std::string{},true};
  const auto year=std::to_integer<unsigned>((*value)[1])|
      (std::to_integer<unsigned>((*value)[2])<<8);
  const auto month=std::to_integer<unsigned>((*value)[3]);
  const auto day=std::to_integer<unsigned>((*value)[4]);
  if (!valid_components(year,month,day)) return Cell{std::string{},true};
  std::string owned="0000-00-00";
  unsigned remaining_year=year;
  for (std::size_t i=4;i>0;--i) { owned[i-1]=static_cast<char>('0'+remaining_year%10);remaining_year/=10; }
  owned[5]=static_cast<char>('0'+month/10);owned[6]=static_cast<char>('0'+month%10);
  owned[8]=static_cast<char>('0'+day/10);owned[9]=static_cast<char>('0'+day%10);
  return Cell{std::move(owned),false};
}
}
