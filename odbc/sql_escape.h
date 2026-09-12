#pragma once

#include <string>
#include <string_view>

namespace rs::odbc {

enum class SqlEscapeError {
  None,
  InvalidSyntax,
  InvalidDatetime,
  Unsupported,
};

struct SqlEscapeResult {
  std::string sql;
  SqlEscapeError error{SqlEscapeError::None};
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == SqlEscapeError::None;
  }
};

// Converts ODBC escape clauses to PostgreSQL syntax while leaving braces in
// string literals, quoted identifiers, dollar strings, and comments untouched.
SqlEscapeResult translate_odbc_sql(std::string_view sql);

} // namespace rs::odbc
