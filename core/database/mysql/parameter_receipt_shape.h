#pragma once

#include "core/database/mysql/query_wire.h"

namespace rs::core::database::mysql::parameter_receipt_detail {
// Caller-independent shape only. Inputs must be one owning successful parser
// receipt; these pure checks cannot attest provenance or permit execution.
struct DecimalDescriptorShape { unsigned precision; unsigned scale; };
inline rs::util::Result<unsigned> datetime_q6_shape(
    const query_detail::NativeParameterDescriptorObservation& raw,
    const NativeTypeInfo& normalized) {
  using rs::util::DbErrorCode;
  // Native7 is not a supported receipt through today's column reader. This
  // defensive pure rejection does not imply it was drained or may be reused.
  if (raw.type==7) return {DbErrorCode::UnsupportedFeature};
  const bool native_datetime=raw.type==12;
  const bool normalized_datetime=normalized.type==ScalarType::Timestamp;
  if (!native_datetime && !normalized_datetime) return {DbErrorCode::UnsupportedFeature};
  if (!native_datetime || !normalized_datetime || !normalized.known)
    return {DbErrorCode::ProtocolError};
  // Only the explicitly qualified parameter context may produce this pair.
  // Raw byte width remains104; normalized calendar width remains26.
  if (raw.charset!=45 || raw.decimals!=6) return {DbErrorCode::UnsupportedFeature};
  if (raw.width!=104 || normalized.column_size!=26 || normalized.decimal_digits!=6)
    return {DbErrorCode::ProtocolError};
  return static_cast<unsigned>(raw.decimals);
}
inline rs::util::Result<DecimalDescriptorShape> decimal_65q30_shape(
    const query_detail::NativeParameterDescriptorObservation& raw,
    std::uint16_t explicit_flags,const NativeTypeInfo& normalized) {
  using rs::util::DbErrorCode;
  if (!raw.flags || *raw.flags!=explicit_flags)
    return {DbErrorCode::ProtocolError};
  if (raw.type==0) return {DbErrorCode::UnsupportedFeature};
  if (!normalized.known) return {DbErrorCode::ProtocolError};
  const bool native_decimal=raw.type==246;
  const bool normalized_decimal=normalized.type==ScalarType::Decimal;
  if (!native_decimal && !normalized_decimal) return {DbErrorCode::UnsupportedFeature};
  if (!native_decimal || !normalized_decimal)
    return {DbErrorCode::ProtocolError};
  // Only captured signed flags128 and binary charset63/q30 are selected.
  // Other valid flag profiles are unadmitted, not implicitly sign-compatible.
  if (explicit_flags!=128 || raw.charset!=63 || raw.decimals!=30)
    return {DbErrorCode::UnsupportedFeature};
  if (raw.width!=67 || normalized.column_size!=65 ||
      normalized.decimal_digits!=30)
    return {DbErrorCode::ProtocolError};
  return DecimalDescriptorShape{65,30};
}
}
