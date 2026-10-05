#pragma once

#include <string>
#include <string_view>

namespace rs::core::database {

enum class SqlTranslationError {
  None,
  InvalidSyntax,
  InvalidDatetime,
  Unsupported,
};

// The backend returns translated SQL or a semantic error. The ODBC layer owns
// SQLSTATE selection and application-buffer handling; SQL is unusable on error.
struct SqlTranslationResult {
  std::string sql;
  SqlTranslationError error{SqlTranslationError::None};
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SqlTranslationError::None;
  }
};

} // namespace rs::core::database
