#pragma once

#include "core/database/native_type_info.h"
#include "core/util/result.h"
#include <string_view>

namespace rs::core::database::mysql::decimal_detail {
// Bounded NEWDECIMAL result profile. Live fixed-scale/p==s qualification is
// separate. MySQL wire decimals are strings even in binary result rows.
inline rs::util::Result<NativeTypeInfo> metadata(std::uint64_t length,
    std::uint64_t scale, bool unsigned_value) {
  using rs::util::DbErrorCode;
  const auto overhead=(scale?1u:0u)+(unsigned_value?0u:1u);
  if (length<=overhead) return {DbErrorCode::ProtocolError};
  const auto precision=length-overhead;
  if (scale>precision) return {DbErrorCode::ProtocolError};
  if (precision>65 || scale>30) return {DbErrorCode::UnsupportedFeature};
  return NativeTypeInfo{ScalarType::Decimal,precision,static_cast<std::int16_t>(scale),true};
}

// No integer/floating conversion, rounding or normalization: keep exact bytes.
// Complete invalid cells follow the deferred-cell contract; framing is separate.
inline bool valid_cell(std::string_view value,const NativeTypeInfo& info,bool unsigned_value) {
  if (info.type!=ScalarType::Decimal || !info.known || !info.column_size ||
      info.column_size>65 || info.decimal_digits<0 || info.decimal_digits>30 ||
      static_cast<std::uint64_t>(info.decimal_digits)>info.column_size || value.empty()) return false;
  if (value.front()=='-') {
    if (unsigned_value) return false;
    value.remove_prefix(1);
  }
  const auto point=value.find('.');
  const auto whole=value.substr(0,point);
  const auto fraction=point==std::string_view::npos?std::string_view{}:value.substr(point+1);
  const auto scale=static_cast<std::size_t>(info.decimal_digits);
  if (whole.empty() || fraction.size()!=scale || (scale==0 && point!=std::string_view::npos) ||
      (scale!=0 && point==std::string_view::npos)) return false;
  for (const auto digit:whole) if (digit<'0' || digit>'9') return false;
  for (const auto digit:fraction) if (digit<'0' || digit>'9') return false;
  const auto first=whole.find_first_not_of('0');
  const auto significant=first==std::string_view::npos?0:whole.size()-first;
  return significant<=info.column_size-scale;
}
}
