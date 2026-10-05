#pragma once

#include "datetime_wire.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace rs::core::database::mysql::datetime_parameter_detail {
// Private native DATETIME candidate only; no runtime parameter admission.
// Precision is explicit: the shared Timestamp hint has no native-kind/scale
// receipt. NULL belongs in the parameter bitmap and carries no value bytes.
struct Parameter {
  static constexpr std::uint8_t native_type=12;
  static constexpr std::uint8_t unsigned_flag=0;
  std::optional<std::vector<std::byte>> value;
};
inline rs::util::Result<Parameter> encode(std::optional<std::string_view> input,
    unsigned precision,std::size_t max_value_bytes=12) {
  using rs::util::DbErrorCode;
  if (precision>6) return {DbErrorCode::UnsupportedFeature};
  if (!input) return Parameter{std::nullopt};
  const auto text=*input;
  if (text.size()!=19+(precision?1+precision:0) ||
      !date_detail::valid_cell(text.substr(0,10)) || text[10]!=' ' ||
      text[13]!=':' || text[16]!=':' || (precision && text[19]!='.'))
    return {DbErrorCode::InvalidParameter};
  unsigned year{},month{},day{},hour{},minute{},second{},micros{};
  for (std::size_t i=0;i<text.size();++i) {
    if (i==4 || i==7 || i==10 || i==13 || i==16 || i==19) continue;
    if (text[i]<'0' || text[i]>'9') return {DbErrorCode::InvalidParameter};
    auto& part=i<4?year:(i<7?month:(i<10?day:(i<13?hour:(i<16?minute:(i<19?second:micros)))));
    part=part*10+static_cast<unsigned>(text[i]-'0');
  }
  micros*=datetime_detail::powers[6-precision];
  if (!datetime_detail::valid_components(year,month,day,hour,minute,second,micros,
      {datetime_detail::Kind::Datetime,precision})) return {DbErrorCode::InvalidParameter};
  const unsigned length=micros?11u:((hour || minute || second)?7u:4u);
  if (length+1>max_value_bytes) return {DbErrorCode::ResourceLimit};
  std::vector<std::byte> bytes{static_cast<std::byte>(length),static_cast<std::byte>(year&255),
      static_cast<std::byte>(year>>8),static_cast<std::byte>(month),static_cast<std::byte>(day)};
  if (length>=7) for (auto part:{hour,minute,second}) bytes.push_back(static_cast<std::byte>(part));
  if (length==11) for (unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::byte>((micros>>(8*i))&255));
  return Parameter{std::move(bytes)};
}
}
