#pragma once

#include "core/database/mysql/parameter_receipt_shape.h"
#include "core/database/mysql/prepared_wire.h"

namespace rs::core::database::mysql::datetime_parameter_detail {
// Caller-affinity only; structural receipt policy has one shared owner.
// Session inputs come from its owning packet validator, never caller reparsing.
inline rs::util::Result<unsigned> hint_affinity(QueryParameterType hint,
    const query_detail::NativeParameterDescriptorObservation& raw,const NativeTypeInfo& normalized) {
  auto precision=parameter_receipt_detail::datetime_q6_shape(raw,normalized);
  if (!precision) return {precision.error()};
  if (hint!=QueryParameterType::Timestamp) return {rs::util::DbErrorCode::UnsupportedFeature};
  return *precision;
}
inline rs::util::Result<void> descriptor(QueryParameterType hint,
    const query_detail::NativeParameterDescriptorObservation& raw,const NativeTypeInfo& normalized,
    const prepared_detail::DatetimeParameterObservation& caller) {
  using rs::util::DbErrorCode;
  auto precision=hint_affinity(hint,raw,normalized);
  if (!precision) return {precision.error()};
  if (!caller.encoded.value) {
    if (caller.caller_precision || caller.micros) return {DbErrorCode::InvalidParameter};
    return {};
  }
  if (!caller.caller_precision || !caller.micros || *caller.caller_precision>6 ||
      *caller.micros>999999 || *caller.micros%datetime_detail::powers[6-*caller.caller_precision]!=0 ||
      *caller.caller_precision>*precision) return {DbErrorCode::InvalidParameter};
  return {};
}
}
