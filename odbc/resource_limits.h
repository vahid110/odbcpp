#pragma once

#include "core/database/backend_provider.h"
#include <charconv>
#include <map>
#include <stdexcept>
#include <string>

namespace rs::odbc {

// Configuration is unavailable before capture; the input cannot raise its own
// parsing ceiling. This is independent of the resolved per-field SDK limit.
inline constexpr std::size_t connection_capture_max_bytes = 1024 * 1024;

// Apply the resolver's already merged, uppercase configuration keys.
inline void parse_resource_limits(const std::map<std::string, std::string>& parameters,
                                 rs::core::database::ConnectionOptions& options) {
  struct Option { const char* name; std::size_t* value; };
  const Option limits[]{
      {"MAXRESPONSEBYTES", &options.response_limits.max_wire_bytes},
      {"MAXRESPONSEMESSAGES", &options.response_limits.max_messages},
      {"MAXSTARTUPRESPONSEBYTES", &options.startup_response_limits.max_wire_bytes},
      {"MAXSTARTUPRESPONSEMESSAGES", &options.startup_response_limits.max_messages},
      {"MAXROWS", &options.result_limits.max_rows},
      {"MAXCELLS", &options.result_limits.max_cells},
      {"MAXCOLUMNS", &options.result_limits.max_columns_per_description},
      {"MAXRESULTS", &options.result_limits.max_results},
      {"MAXMETADATAENTRIES", &options.result_limits.max_metadata_entries},
      {"MAXCOLUMNNAMEBYTES", &options.result_limits.max_column_name_bytes},
      {"MAXMETADATANAMEBYTES", &options.result_limits.max_metadata_name_bytes},
      {"MAXDIAGNOSTICBYTES", &options.result_limits.max_diagnostic_bytes},
      {"MAXSQLBYTES", &options.input_limits.max_sql_bytes},
      {"MAXPARAMETERS", &options.input_limits.max_parameters},
      {"MAXPARAMETERBYTES", &options.input_limits.max_parameter_bytes},
      {"MAXPARAMETERTOTALBYTES", &options.input_limits.max_parameter_total_bytes},
      {"MAXCONNECTIONFIELDBYTES", &options.input_limits.max_connection_field_bytes},
      {"MAXREQUESTWIREBYTES", &options.input_limits.max_request_wire_bytes},
      {"MAXSTARTUPWIREBYTES", &options.input_limits.max_startup_wire_bytes},
      {"MAXAUTHWIREBYTES", &options.input_limits.max_auth_wire_bytes}};
  for (const auto& limit : limits) {
    const auto found = parameters.find(limit.name);
    if (found == parameters.end()) continue;
    const auto& text = found->second;
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
      throw std::invalid_argument(std::string(limit.name) + " must be an unsigned decimal integer");
    }
    *limit.value = value;
  }
}

} // namespace rs::odbc
