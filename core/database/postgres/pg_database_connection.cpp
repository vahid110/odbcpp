#include "pg_database_connection.h"
#include "pg_backend_provider.h"
#include "pg_protocol_parser.h"

#include <algorithm>
#include <charconv>
#include <unordered_set>

namespace rs::core::database::postgres {

PgDatabaseConnection::PgDatabaseConnection(
    std::unique_ptr<rs::core::transport::ITransport> transport,
    std::optional<SessionResetProfile> reset_profile, PgCatalogProfile catalog_profile)
    : GenericDatabaseConnection(std::make_unique<PgProtocolParser>(),
                                std::move(transport)), reset_profile_(reset_profile), catalog_profile_(catalog_profile) {}

BackendResult<void> PgDatabaseConnection::check_health(rs::util::Deadline deadline) {
  auto result = execute_query("SELECT 1", deadline);
  if (result.has_error()) {
    auto error = std::move(result.backend_error());
    error.operation = BackendOperation::CheckHealth;
    return error;
  }
  const auto snapshot = result.session_snapshot();
  // Validate the fixed probe, not merely receipt of ReadyForQuery. Unexpected
  // successful payload/state is a protocol failure and cannot authorize reuse.
  if (result->columns.size() != 1 || result->rows.size() != 1 ||
      result->rows[0].size() != 1 || result->rows[0][0] != "1" ||
      !result->cell_errors.empty() || !result->additional_results.empty() ||
      result->error || (snapshot.state != SessionState::Idle &&
                        snapshot.state != SessionState::Transaction)) {
    disconnect();
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
                       "Unexpected health probe response"};
    error.operation = BackendOperation::CheckHealth;
    error.session_state = SessionState::Disconnected;
    error.disposition = SessionDisposition::Retire;
    return error;
  }
  return BackendResult<void>{snapshot};
}

BackendResult<ResolvedTypeMap> PgDatabaseConnection::resolve_types(
    std::span<const std::uint32_t> ids, rs::util::Deadline deadline) {
  if (catalog_profile_ == PgCatalogProfile::Redshift &&
      std::find(ids.begin(), ids.end(), 6551u) != ids.end()) {
    // This rejects unqualified parameter metadata, not execution before I/O:
    // the combined prepared exchange may already have sent Execute.
    return local_backend_error(LocalFailure::Unsupported,
        "Redshift VARBYTE parameters are not qualified", BackendOperation::ResolveTypes,
        session_state());
  }
  ResolvedTypeMap resolved;
  std::vector<std::uint32_t> unresolved;
  for (const auto id : ids) {
    const auto type = describe_type(id, -1, -1);
    const auto inserted = resolved.emplace(id, type).second;
    if (inserted && id != 0 && !type.known) unresolved.push_back(id);
  }
  if (unresolved.empty()) return resolved;
  std::sort(unresolved.begin(), unresolved.end());

  std::string query =
      "WITH RECURSIVE type_chain(original_oid, type_oid, base_oid, "
      "type_modifier) AS ("
      "SELECT oid, oid, typbasetype, typtypmod "
      "FROM pg_catalog.pg_type WHERE oid IN (";
  for (std::size_t index = 0; index < unresolved.size(); ++index) {
    if (index != 0) query += ',';
    query += std::to_string(unresolved[index]);
  }
  query +=
      ") UNION ALL SELECT chain.original_oid, base.oid, base.typbasetype, "
      "CASE WHEN chain.type_modifier >= 0 THEN chain.type_modifier "
      "ELSE base.typtypmod END "
      "FROM type_chain AS chain JOIN pg_catalog.pg_type AS base "
      "ON base.oid = chain.base_oid) "
      "SELECT original_oid::text, type_oid::text, type_modifier::text "
      "FROM type_chain "
      "WHERE base_oid = 0";

  auto types = execute_query(query, deadline);
  if (types.has_error()) {
    auto error = std::move(types.backend_error());
    error.operation = BackendOperation::ResolveTypes;
    return error;
  }

  const auto parse_number = [](const std::string& value, auto& number) {
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), number);
    return error == std::errc{} && end == value.data() + value.size();
  };
  std::unordered_set<std::uint32_t> seen;
  for (const auto& row : types->rows) {
    std::uint32_t original = 0, base = 0;
    std::int32_t modifier = -1;
    if (row.size() != 3 || !row[0] || !row[1] || !row[2] ||
        !parse_number(*row[0], original) || !parse_number(*row[1], base) ||
        !parse_number(*row[2], modifier) || base == 0 ||
        !std::binary_search(unresolved.begin(), unresolved.end(), original) ||
        !seen.insert(original).second) {
      // The query has completed and drained. Invalid metadata does not itself
      // imply an ambiguous transport or justify retiring an idle session.
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),
                        "Data source returned invalid parameter type metadata"};
      error.error_class = BackendErrorClass::InvalidMetadata;
      error.operation = BackendOperation::ResolveTypes;
      error.session_state = session_state();
      error.disposition = error.session_state == SessionState::Disconnected ? SessionDisposition::Retire :
          error.session_state == SessionState::Idle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired;
      return error;
    }
    resolved.at(original) = describe_type(base, -1, modifier);
  }
  return resolved;
}

} // namespace rs::core::database::postgres
