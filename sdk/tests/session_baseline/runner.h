#pragma once
#include "odbcpp/database/i_database_connection.h"
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace odbcpp::test {
// Internal test fixture, not an exported SDK or complete conformance kit.
struct SessionBaselineFixture {
  std::string direct_sql;
  std::string prepared_sql;
  std::string recoverable_error_sql;
  std::vector<rs::core::database::QueryParameter> parameters;
  rs::core::database::ResultRow expected_cells;
  std::vector<rs::core::database::ScalarType> expected_types;
  std::chrono::milliseconds operation_timeout{5000};
};
using SessionFactory = std::function<std::unique_ptr<rs::core::database::IDatabaseConnection>()>;
// Empty means passed. Failures contain fixed check IDs only, never fixture data.
std::string_view run_session_baseline(const SessionFactory& factory,
    const rs::core::database::ConnectionSettings& settings,
    const SessionBaselineFixture& fixture);
}
