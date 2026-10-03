#include "redshift_primary_key_contract.h"
#include "core/database/result_validation.h"
#include <array>
#include <charconv>
#include <set>

namespace rs::core::database::postgres {
namespace {
bool identifier(const std::string& value) {
  return !value.empty() && value.find('\0') == std::string::npos &&
      rs::util::utf8_code_point_count(value).has_value();
}
bool valid_plan(const RedshiftPrimaryKeyCommandPlan& plan) {
  return identifier(plan.database) && identifier(plan.schema) && identifier(plan.table);
}
BackendError invalid_metadata() {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      "Invalid Redshift primary-key metadata"};
  error.error_class = BackendErrorClass::InvalidMetadata;
  // A pure normalizer cannot assert session health or retry authority.
  return error;
}
bool text_family(ScalarType type) {
  return type == ScalarType::Char || type == ScalarType::VarChar ||
      type == ScalarType::LongVarChar;
}
}  // namespace

BackendResult<RedshiftPrimaryKeyCommandPlan> redshift_primary_key_plan(
    std::optional<std::string> database, std::optional<std::string> schema,
    std::optional<std::string> table) {
  if (!database || !schema || !table || !identifier(*database) ||
      !identifier(*schema) || !identifier(*table))
    return {rs::util::DbErrorCode::InvalidParameter,
        "Exact database, schema and table identifiers are required"};
  return RedshiftPrimaryKeyCommandPlan{std::move(*database), std::move(*schema), std::move(*table)};
}

BackendResult<QueryResult> normalize_redshift_primary_keys(
    const RedshiftPrimaryKeyCommandPlan& plan, BackendResult<QueryResult> input) {
  if (!input) return input.backend_error();
  const auto& source = *input;
  if (source.error) return *source.error;
  for (const auto& extra : source.additional_results)
    if (extra.error) return *extra.error;
  if (!valid_plan(plan) || !valid_result_structure(source) ||
      !source.additional_results.empty() || !source.cell_errors.empty() ||
      !source.normalized_parameter_types.empty() || source.affected_rows != 0 ||
      source.columns.size() != 6 || source.rows.size() > 32767)
    return invalid_metadata();

  constexpr std::array<const char*, 6> names{
      "database_name", "schema_name", "table_name", "column_name", "key_seq", "pk_name"};
  std::array<std::size_t, 6> indexes{};
  for (std::size_t expected = 0; expected < names.size(); ++expected) {
    std::size_t matches = 0;
    for (std::size_t actual = 0; actual < source.columns.size(); ++actual) {
      const auto& column = source.columns[actual];
      if (column.name != names[expected]) continue;
      ++matches;
      indexes[expected] = actual;
      if (!column.normalized_type || !column.normalized_type->known)
        return invalid_metadata();
      const auto type = column.normalized_type->type;
      if (expected == 4 ? (type != ScalarType::SmallInt && type != ScalarType::Integer)
                        : !text_family(type)) return invalid_metadata();
    }
    if (matches != 1) return invalid_metadata();
  }

  QueryResult output;
  constexpr std::array<const char*, 6> output_names{
      "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"};
  for (std::size_t i = 0; i < output_names.size(); ++i)
    output.columns.push_back({output_names[i], NativeTypeInfo{
        i == 4 ? ScalarType::SmallInt : ScalarType::VarChar, i == 4 ? 5u : 0u, 0, true}});
  std::set<int> sequences;
  std::set<std::string> key_columns;
  ResultCell pk_name;
  bool first = true;
  for (const auto& row : source.rows) {
    ResultRow normalized;
    for (const auto index : indexes) normalized.push_back(row[index]);
    for (std::size_t i = 0; i < 4; ++i)
      if (!normalized[i] || !identifier(*normalized[i])) return invalid_metadata();
    if (*normalized[0] != plan.database || *normalized[1] != plan.schema ||
        *normalized[2] != plan.table || !normalized[4]) return invalid_metadata();
    if (normalized[5] && !identifier(*normalized[5])) return invalid_metadata();
    if (!first && normalized[5] != pk_name) return invalid_metadata();
    first = false;
    pk_name = normalized[5];
    const auto& sequence = *normalized[4];
    if (sequence.empty() || sequence.size() > 5 || sequence.find_first_not_of("0123456789") != std::string::npos)
      return invalid_metadata();
    int number = 0;
    const auto parsed = std::from_chars(sequence.data(), sequence.data() + sequence.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != sequence.data() + sequence.size() ||
        number < 1 || number > 32767 || !sequences.insert(number).second ||
        !key_columns.insert(*normalized[3]).second) return invalid_metadata();
    normalized[4] = std::to_string(number);
    output.rows.push_back(std::move(normalized));
  }
  if (!sequences.empty() && (*sequences.begin() != 1 ||
      *sequences.rbegin() != static_cast<int>(sequences.size()))) return invalid_metadata();
  return BackendResult<QueryResult>{std::move(output), input.session_snapshot()};
}
}  // namespace rs::core::database::postgres
