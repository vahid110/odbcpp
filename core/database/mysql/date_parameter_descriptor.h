#pragma once

#include "core/database/mysql/query_wire.h"
#include "core/database/query_parameter.h"

namespace rs::core::database::mysql::date_parameter_detail {
// Private admission candidate, not selected by any session or packet writer.
// Both inputs must come from the same successfully validated column receipt.
// A NULL value does not change that receipt; caller-value validation is separate.
inline rs::util::Result<void> descriptor(
    QueryParameterType hint,const query_detail::NativeParameterDescriptorObservation& raw,
    const NativeTypeInfo& normalized) {
  const bool native_date=raw.type==10;
  const bool normalized_date=normalized.type==ScalarType::Date;
  if (native_date || normalized_date) {
    if (!native_date || !normalized_date || raw.width!=10 || raw.decimals!=0 ||
        !normalized.known || normalized.column_size!=10 || normalized.decimal_digits!=0)
      return {rs::util::DbErrorCode::ProtocolError};
    if (hint==QueryParameterType::Date) return {};
  }
  // A validated supported non-DATE receipt or a non-DATE hint is a local
  // candidate-policy mismatch, not permission to infer a DATE descriptor.
  return {rs::util::DbErrorCode::UnsupportedFeature};
}
}
