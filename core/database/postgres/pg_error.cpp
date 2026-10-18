#include "pg_backend_provider.h"
#include "odbcpp/util/utf8.h"

#include <algorithm>

namespace rs::core::database::postgres {
namespace {
bool redshift_null_violation_column(std::string_view column) {
  if (column.empty() || column.size() > 127) return false;
  bool nonblank = false;
  for (std::size_t offset = 0; offset < column.size();) {
    const auto point = rs::util::next_utf8_code_point(column, offset);
    if (!point || *point < 0x20 || (*point >= 0x7f && *point <= 0x9f) ||
        *point == 0x2028 || *point == 0x2029) return false;
    nonblank = nonblank || *point != ' ';
  }
  return nonblank;
}
}  // namespace

std::optional<std::string> PgBackendProvider::normalize_error_sqlstate(
    const BackendError& error, ErrorContext context) const {
  // Redshift emits XX000 for this enforced NOT NULL violation. XX000 alone
  // is ambiguous: require the observed server wording, including the parser's
  // owning diagnostic prefix, and one bounded printable UTF-8 column name.
  constexpr std::string_view prefix{
      "Query error: Cannot insert a NULL value into column "};
  if (catalog_profile_ == PgCatalogProfile::Redshift &&
      context == ErrorContext::Unknown &&
      error.code == rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed) &&
      error.error_class == BackendErrorClass::Server &&
      error.native_state == "XX000" && error.message.starts_with(prefix)) {
    const std::string_view column = std::string_view{error.message}.substr(prefix.size());
    if (redshift_null_violation_column(column)) {
      return std::string{"23000"};
    }
  }
  return normalize_error_sqlstate(error.native_state.value_or(""), context);
}

std::optional<std::string> PgBackendProvider::normalize_error_sqlstate(
    std::string_view server_state, ErrorContext context) const {
  if (server_state.size() != 5 ||
      !std::all_of(server_state.begin(), server_state.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
      })) {
    return std::nullopt;
  }
  if (server_state == "22P02") return std::string{"22018"};
  if (server_state.substr(0, 2) == "22") return std::string(server_state);
  if (server_state.substr(0, 2) == "23") return std::string{"23000"};
  // Preserve the failed-transaction diagnostic so callers can identify the
  // rollback requirement; unrelated vendor states still use caller fallback.
  if (server_state == "25P02") return std::string{"25P02"};
  if (server_state == "3F000") return std::string{"3F000"};
  if (server_state == "42P07") {
    if (context == ErrorContext::CreateIndex) return std::string{"42S11"};
    if (context == ErrorContext::CreateTable ||
        context == ErrorContext::CreateView) return std::string{"42S01"};
    return std::nullopt;
  }
  if (server_state == "42P01") return std::string{"42S02"};
  if (server_state == "42704" && context == ErrorContext::DropIndex) {
    return std::string{"42S12"};
  }
  if (server_state == "42701") return std::string{"42S21"};
  if (server_state == "42703") return std::string{"42S22"};
  return std::nullopt;
}

} // namespace rs::core::database::postgres
