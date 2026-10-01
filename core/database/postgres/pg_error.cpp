#include "pg_backend_provider.h"

#include <algorithm>

namespace rs::core::database::postgres {

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
