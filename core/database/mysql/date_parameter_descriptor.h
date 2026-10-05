#pragma once

#include "core/database/mysql/query_wire.h"
#include "core/database/query_parameter.h"

namespace rs::core::database::mysql::date_parameter_detail {
// Private DATE parameter admission policy used by the MySQL session.
// Both inputs must come from the same successfully validated column receipt.
// A NULL value does not change that receipt; caller-value validation is separate.
inline rs::util::Result<void> receipt_shape(
    const query_detail::NativeParameterDescriptorObservation& raw,
    const NativeTypeInfo& normalized) {
  const bool native_date=raw.type==10;
  const bool normalized_date=normalized.type==ScalarType::Date;
  if (native_date || normalized_date) {
    if (!native_date || !normalized_date || raw.decimals!=0 ||
        !normalized.known || normalized.column_size!=10 || normalized.decimal_digits!=0)
      return {rs::util::DbErrorCode::ProtocolError};
    // Column widths describe byte capacity for this receipt's charset. These
    // exact representations do not assert a negotiated session charset.
    if (raw.charset!=63 && raw.charset!=45)
      return {rs::util::DbErrorCode::UnsupportedFeature};
    if (raw.width!=(raw.charset==63 ? 10u : 40u))
      return {rs::util::DbErrorCode::ProtocolError};
    return {};
  }
  // A validated supported non-DATE receipt is a local
  // candidate-policy mismatch, not permission to infer a DATE descriptor.
  return {rs::util::DbErrorCode::UnsupportedFeature};
}
inline rs::util::Result<void> descriptor(
    QueryParameterType hint,const query_detail::NativeParameterDescriptorObservation& raw,
    const NativeTypeInfo& normalized) {
  auto shape=receipt_shape(raw,normalized);
  if (!shape) return {shape.error()};
  if (hint!=QueryParameterType::Date) return {rs::util::DbErrorCode::UnsupportedFeature};
  return {};
}

}
