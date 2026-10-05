#pragma once

#include "odbcpp/database/sql_translation.h"
#include <cstddef>
#include <string_view>

namespace rs::core::database {

// Immutable provider-owned service. No I/O, credentials or mutable session state.
// Input is borrowed only during the call; translation results own their strings.
// The service reference remains valid for the provider lifetime.
class ISqlDialect {
 public:
  virtual ~ISqlDialect() = default;
  virtual std::size_t count_parameter_markers(std::string_view sql) const = 0;
  virtual SqlTranslationResult translate_sql(std::string_view sql) const = 0;
};

} // namespace rs::core::database
