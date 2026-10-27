#pragma once

// The SDK's default result and input limits, which suites use as expected
// values. Driver Manager builds cannot include the SDK's backend interface
// headers, so these mirror the defaults; direct builds check every field
// against the SDK so the copies cannot drift.
#include <cstddef>

namespace odbcpp::test {

struct ResultLimits {
  std::size_t max_rows{1000000};
  std::size_t max_cells{4000000};
  std::size_t max_columns_per_description{4096};
  std::size_t max_results{1024};
  std::size_t max_metadata_entries{65536};
  std::size_t max_column_name_bytes{1024};
  std::size_t max_metadata_name_bytes{1024 * 1024};
  std::size_t max_diagnostic_bytes{16 * 1024};
};

struct InputLimits {
  std::size_t max_sql_bytes{1024 * 1024};
  std::size_t max_parameters{65535};
  std::size_t max_parameter_bytes{16 * 1024 * 1024};
  std::size_t max_parameter_total_bytes{64 * 1024 * 1024};
  std::size_t max_connection_field_bytes{64 * 1024};
  std::size_t max_request_wire_bytes{128 * 1024 * 1024};
  std::size_t max_startup_wire_bytes{1024 * 1024};
  std::size_t max_auth_wire_bytes{1024 * 1024};
};

}  // namespace odbcpp::test

#ifndef ODBCPP_TEST_VIA_DRIVER_MANAGER
#include "odbcpp/database/i_database_connection.h"

namespace odbcpp::test::detail {
constexpr ResultLimits kResult{};
constexpr rs::core::database::ResultLimits kSdkResult{};
static_assert(kResult.max_rows == kSdkResult.max_rows);
static_assert(kResult.max_cells == kSdkResult.max_cells);
static_assert(kResult.max_columns_per_description == kSdkResult.max_columns_per_description);
static_assert(kResult.max_results == kSdkResult.max_results);
static_assert(kResult.max_metadata_entries == kSdkResult.max_metadata_entries);
static_assert(kResult.max_column_name_bytes == kSdkResult.max_column_name_bytes);
static_assert(kResult.max_metadata_name_bytes == kSdkResult.max_metadata_name_bytes);
static_assert(kResult.max_diagnostic_bytes == kSdkResult.max_diagnostic_bytes);
static_assert(sizeof(ResultLimits) == sizeof(rs::core::database::ResultLimits),
              "ResultLimits gained or lost a field");
constexpr InputLimits kInput{};
constexpr rs::core::database::InputLimits kSdkInput{};
static_assert(kInput.max_sql_bytes == kSdkInput.max_sql_bytes);
static_assert(kInput.max_parameters == kSdkInput.max_parameters);
static_assert(kInput.max_parameter_bytes == kSdkInput.max_parameter_bytes);
static_assert(kInput.max_parameter_total_bytes == kSdkInput.max_parameter_total_bytes);
static_assert(kInput.max_connection_field_bytes == kSdkInput.max_connection_field_bytes);
static_assert(kInput.max_request_wire_bytes == kSdkInput.max_request_wire_bytes);
static_assert(kInput.max_startup_wire_bytes == kSdkInput.max_startup_wire_bytes);
static_assert(kInput.max_auth_wire_bytes == kSdkInput.max_auth_wire_bytes);
static_assert(sizeof(InputLimits) == sizeof(rs::core::database::InputLimits),
              "InputLimits gained or lost a field");
}  // namespace odbcpp::test::detail
#endif
