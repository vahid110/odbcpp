#include "redshift_foreign_key_contract.h"
#include "core/database/result_validation.h"
#include <array>
#include <charconv>
#include <map>
#include <set>
#include <tuple>

namespace rs::core::database::postgres {
namespace {
bool name(const std::string& value) {
  return !value.empty() && value.find('\0') == std::string::npos &&
      rs::util::utf8_code_point_count(value).has_value();
}
bool table(const RedshiftForeignKeyTable& value) {
  return name(value.database) && name(value.schema) && name(value.table);
}
bool valid(const RedshiftForeignKeyCommandPlan& plan) {
  if ((plan.primary && !table(*plan.primary)) || (plan.foreign && !table(*plan.foreign))) return false;
  switch (plan.direction) {
    case RedshiftForeignKeyDirection::Imported: return !plan.primary && plan.foreign.has_value();
    case RedshiftForeignKeyDirection::Exported: return plan.primary.has_value() && !plan.foreign;
    case RedshiftForeignKeyDirection::Both: return plan.primary.has_value() && plan.foreign.has_value();
  }
  return false;
}
BackendError malformed() {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      "Invalid Redshift foreign-key metadata"};
  error.error_class = BackendErrorClass::InvalidMetadata;
  return error;
}
bool text(ScalarType type) {
  return type == ScalarType::Char || type == ScalarType::VarChar || type == ScalarType::LongVarChar;
}
bool numeric(std::size_t index) { return index == 8 || index == 9 || index == 10 || index == 13; }
std::optional<int> integer(const ResultCell& value, int low, int high) {
  if (!value || value->empty() || value->size() > 5 ||
      value->find_first_not_of("0123456789") != std::string::npos) return {};
  int result = 0;
  const auto parsed = std::from_chars(value->data(), value->data() + value->size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size() || result < low || result > high) return {};
  return result;
}
bool matches(const ResultRow& row, std::size_t offset, const RedshiftForeignKeyTable& target) {
  return *row[offset] == target.database && *row[offset + 1] == target.schema && *row[offset + 2] == target.table;
}
using GroupKey = std::tuple<std::string, std::string, std::string, std::string>;
using Reference = std::tuple<std::string, std::string, std::string, ResultCell, int, int, int>;
struct Group {
  Reference reference;
  std::set<int> sequences;
  std::set<std::pair<std::string, std::string>> pairs;
};
}  // namespace

BackendResult<RedshiftForeignKeyCommandPlan> redshift_foreign_key_plan(
    RedshiftForeignKeyDirection direction, std::optional<RedshiftForeignKeyTable> primary,
    std::optional<RedshiftForeignKeyTable> foreign) {
  RedshiftForeignKeyCommandPlan plan{direction, std::move(primary), std::move(foreign)};
  if (!valid(plan)) return {rs::util::DbErrorCode::InvalidParameter, "Exact foreign-key direction and table identities are required"};
  return plan;
}

BackendResult<QueryResult> normalize_redshift_foreign_keys(
    const RedshiftForeignKeyCommandPlan& plan, BackendResult<QueryResult> input) {
  if (!input) return input.backend_error();
  const auto& source = *input;
  if (source.error) return *source.error;
  for (const auto& extra : source.additional_results) if (extra.error) return *extra.error;
  if (!valid(plan) || !valid_result_structure(source) || source.columns.size() != 14 ||
      !source.additional_results.empty() || !source.cell_errors.empty() ||
      !source.normalized_parameter_types.empty() || source.affected_rows != 0) return malformed();
  constexpr std::array<const char*, 14> names{"pk_database_name", "pk_schema_name", "pk_table_name", "pk_column_name",
      "fk_database_name", "fk_schema_name", "fk_table_name", "fk_column_name", "key_seq", "update_rule",
      "delete_rule", "fk_name", "pk_name", "deferrability"};
  std::array<std::size_t, 14> indexes{};
  for (std::size_t i = 0; i < names.size(); ++i) {
    std::size_t count = 0;
    for (std::size_t j = 0; j < source.columns.size(); ++j) {
      const auto& column = source.columns[j];
      if (column.name != names[i]) continue;
      ++count; indexes[i] = j;
      if (!column.normalized_type || !column.normalized_type->known) return malformed();
      const auto type = column.normalized_type->type;
      if (numeric(i) ? (type != ScalarType::SmallInt && type != ScalarType::Integer) : !text(type)) return malformed();
    }
    if (count != 1) return malformed();
  }
  QueryResult output;
  constexpr std::array<const char*, 14> output_names{"PKTABLE_CAT", "PKTABLE_SCHEM", "PKTABLE_NAME", "PKCOLUMN_NAME",
      "FKTABLE_CAT", "FKTABLE_SCHEM", "FKTABLE_NAME", "FKCOLUMN_NAME", "KEY_SEQ", "UPDATE_RULE", "DELETE_RULE",
      "FK_NAME", "PK_NAME", "DEFERRABILITY"};
  for (std::size_t i = 0; i < names.size(); ++i)
    output.columns.push_back({output_names[i], NativeTypeInfo{numeric(i) ? ScalarType::SmallInt : ScalarType::VarChar,
        numeric(i) ? 5u : 0u, 0, true}});
  std::map<GroupKey, Group> groups;
  ResultRows normalized;
  for (const auto& source_row : source.rows) {
    ResultRow row;
    for (const auto index : indexes) row.push_back(source_row[index]);
    for (std::size_t i = 0; i < 8; ++i) if (!row[i] || !name(*row[i])) return malformed();
    if (!row[11] || !name(*row[11]) || (row[12] && !name(*row[12]))) return malformed();
    const auto seq = integer(row[8], 1, 32767);
    const auto update = row[9] ? integer(row[9], 0, 4) : std::optional<int>{3};
    const auto remove = row[10] ? integer(row[10], 0, 4) : std::optional<int>{3};
    const auto defer = row[13] ? integer(row[13], 5, 7) : std::optional<int>{7};
    if (!seq || !update || !remove || !defer) return malformed();
    if (plan.foreign && !matches(row, 4, *plan.foreign)) return malformed();
    if (plan.direction == RedshiftForeignKeyDirection::Exported && !matches(row, 0, *plan.primary)) return malformed();
    const GroupKey key{*row[4], *row[5], *row[6], *row[11]};
    const Reference reference{*row[0], *row[1], *row[2], row[12], *update, *remove, *defer};
    auto [group, inserted] = groups.try_emplace(key, Group{reference, {}, {}});
    if ((!inserted && group->second.reference != reference) || !group->second.sequences.insert(*seq).second ||
        !group->second.pairs.emplace(*row[3], *row[7]).second) return malformed();
    row[8] = std::to_string(*seq); row[9] = std::to_string(*update);
    row[10] = std::to_string(*remove); row[13] = std::to_string(*defer);
    normalized.push_back(std::move(row));
  }
  for (const auto& [key, group] : groups)
    if (*group.sequences.begin() != 1 || *group.sequences.rbegin() != static_cast<int>(group.sequences.size())) return malformed();
  for (auto& row : normalized)
    if (plan.direction != RedshiftForeignKeyDirection::Both || matches(row, 0, *plan.primary)) output.rows.push_back(std::move(row));
  return BackendResult<QueryResult>{std::move(output), input.session_snapshot()};
}
}  // namespace rs::core::database::postgres
