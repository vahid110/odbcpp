#include "runner.h"

namespace odbcpp::test {
namespace {
using namespace rs::core::database;
constexpr SessionSnapshot ready{SessionState::Idle, SessionDisposition::Reusable};
bool passive_ready(const IDatabaseConnection& session) {
  return session.is_connected() && session.session_state() == SessionState::Idle;
}
bool matches(const QueryResult& result, const SessionBaselineFixture& fixture) {
  if (result.error || !result.additional_results.empty() || !result.cell_errors.empty() ||
      result.rows.size() != 1 || result.rows[0] != fixture.expected_cells ||
      result.columns.size() != fixture.expected_cells.size()) return false;
  for (std::size_t i = 0; i < result.columns.size(); ++i) {
    if (!result.columns[i].normalized_type || !result.columns[i].normalized_type->known ||
        result.columns[i].normalized_type->type != fixture.expected_types[i]) return false;
  }
  return true;
}
struct Cleanup {
  IDatabaseConnection* session;
  ~Cleanup() { if (session) { try { session->disconnect(); } catch (...) {} } }
};
}
std::string_view run_session_baseline(const SessionFactory& factory,
    const ConnectionSettings& settings, const SessionBaselineFixture& fixture) {
  if (fixture.operation_timeout <= std::chrono::milliseconds::zero() ||
      fixture.operation_timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(
          rs::util::Deadline::max() - rs::util::Clock::now()) ||
      fixture.expected_cells.size() != fixture.expected_types.size() ||
      fixture.expected_cells.size() != 3 || !fixture.expected_cells[0] ||
      fixture.expected_cells[1] || fixture.expected_cells[2] != std::string{}) return "fixture";
  try {
    auto session = factory();
    if (!session) return "factory";
    Cleanup cleanup{session.get()};
    auto connected = session->connect(settings);
    if (!connected || connected.session_snapshot() != ready || !passive_ready(*session)) return "connect.state";
    auto direct = session->execute_query(fixture.direct_sql, rs::util::make_deadline(fixture.operation_timeout));
    if (!direct || direct.session_snapshot() != ready || !passive_ready(*session)) return "direct.state";
    if (!matches(*direct, fixture)) return "direct.result";
    auto prepared = session->execute_prepared(fixture.prepared_sql, fixture.parameters,
        rs::util::make_deadline(fixture.operation_timeout));
    if (!prepared || prepared.session_snapshot() != ready || !passive_ready(*session)) return "prepared.state";
    if (!matches(*prepared, fixture)) return "prepared.result";
    auto rejected = session->execute_query(fixture.recoverable_error_sql,
        rs::util::make_deadline(fixture.operation_timeout));
    if (rejected || rejected.backend_error().error_class != BackendErrorClass::Server ||
        rejected.backend_error().operation != BackendOperation::ExecuteDirect ||
        rejected.session_snapshot() != ready || !passive_ready(*session)) return "error.recovery_state";
    const auto retained_message = rejected.backend_error().message;
    const auto retained_native_state = rejected.backend_error().native_state;
    auto recovery = session->execute_query(fixture.direct_sql, rs::util::make_deadline(fixture.operation_timeout));
    if (!recovery || recovery.session_snapshot() != ready || !passive_ready(*session) ||
        !matches(*recovery, fixture)) return "recovery.result";
    session->disconnect();
    if (session->is_connected() || session->session_state() != SessionState::Disconnected) return "disconnect.state";
    cleanup.session = nullptr;
    session.reset();
    if (!matches(*direct, fixture) || !matches(*prepared, fixture) ||
        rejected.backend_error().message != retained_message ||
        rejected.backend_error().native_state != retained_native_state) return "retained.ownership";
    return {};
  } catch (...) { return "exception"; }
}
}
