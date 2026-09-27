#include "pg_database_connection.h"
#include "pg_protocol_parser.h"

#include <algorithm>
#include <charconv>
#include <unordered_set>

namespace rs::core::database::postgres {

PgDatabaseConnection::PgDatabaseConnection(
    std::unique_ptr<rs::core::transport::ITransport> transport)
    : GenericDatabaseConnection(std::make_unique<PgProtocolParser>(),
                                std::move(transport)) {}

rs::util::Result<ResolvedTypeMap> PgDatabaseConnection::resolve_types(
    std::span<const std::uint32_t> ids, rs::util::Deadline deadline) {
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
  if (types.has_error()) return {types.error(), types.error_message()};

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
      return {rs::util::DbErrorCode::QueryFailed,
              "Data source returned invalid parameter type metadata"};
    }
    resolved.at(original) = describe_type(base, -1, modifier);
  }
  return resolved;
}

} // namespace rs::core::database::postgres
