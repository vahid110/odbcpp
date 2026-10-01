#pragma once

#include "native_type_info.h"
#include <unordered_map>

namespace rs::core::database {

// Private PostgreSQL-family resolution storage, not an SDK session contract.
using ResolvedTypeMap = std::unordered_map<std::uint32_t, NativeTypeInfo>;

} // namespace rs::core::database
