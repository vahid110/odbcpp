#include "redshift_foreign_key_contract.h"
#include "redshift_catalog_query.h"
#include <limits>
#include "core/database/result_validation.h"
#include <array>
#include <algorithm>
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
      ( !source.normalized_parameter_types.empty() &&
        (source.normalized_parameter_types.size() != 3 ||
         !std::all_of(source.normalized_parameter_types.begin(), source.normalized_parameter_types.end(),
             [](const NativeTypeInfo& type) { return type.known && text(type.type); }))) ||
      source.affected_rows != 0) return malformed();
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
      if (numeric(i)) {
        if (type != ScalarType::SmallInt && type != ScalarType::Integer) {
          // SHOW may describe its entirely NULL optional rules as known text.
          // Required key_seq and every non-NULL rule keep the numeric contract.
          const bool null_rule = (i == 9 || i == 10 || i == 13) && text(type) &&
              std::all_of(source.rows.begin(), source.rows.end(),
                  [j](const ResultRow& row) { return !row[j]; });
          if (!null_rule) return malformed();
        }
      } else if (!text(type)) return malformed();
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


namespace rs::core::database::postgres {
namespace {
BackendError current_fk_invalid(const char* reason, SessionSnapshot snapshot) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      std::string("Invalid current-database foreign-key metadata: ") + reason};
  error.error_class = BackendErrorClass::InvalidMetadata;
  error.operation = BackendOperation::ExecuteCatalog;
  error.session_state = snapshot.state;
  error.disposition = SessionDisposition::Retire;
  return error;
}
BackendError current_fk_timeout() {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),
      "Foreign-key catalog deadline expired"};
  error.operation = BackendOperation::ExecuteCatalog;
  return error;
}
BackendError current_fk_limit() {
  auto error = redshift_routine_limit();
  error.message = "Foreign-key catalog budget exceeded";
  return error;
}
// Closed native allocation equation components, not a provider-neutral allocator.
bool current_fk_add(std::size_t& total, std::size_t count, std::size_t size = 1) {
  const auto maximum = (std::numeric_limits<std::size_t>::max)();
  if (count > maximum / size || count * size > maximum - total) return false;
  total += count * size;
  return true;
}
std::optional<RedshiftRoutineUsage> current_fk_storage(const QueryResult& source) {
  auto used = redshift_routine_schema_usage(source);
  if (!current_fk_add(used.bytes, source.normalized_parameter_types.capacity(), sizeof(NativeTypeInfo)) ||
      !current_fk_add(used.metadata, source.normalized_parameter_types.size())) return {};
  for (const auto& column : source.columns) {
    if (!current_fk_add(used.bytes, column.name.capacity() + 1 - column.name.size())) return {};
  }
  for (const auto& row : source.rows) {
    for (const auto& cell : row) {
      if (cell && (!current_fk_add(used.bytes, cell->capacity() + 1 - cell->size()) ||
                   !current_fk_add(used.name_bytes, cell->size()))) return {};
    }
  }
  return used;
}
constexpr std::array<std::string_view, 14> current_fk_fields{
    "pk_database_name", "pk_schema_name", "pk_table_name", "pk_column_name",
    "fk_database_name", "fk_schema_name", "fk_table_name", "fk_column_name",
    "key_seq", "update_rule", "delete_rule", "fk_name", "pk_name", "deferrability"};
constexpr std::array<std::string_view, 14> current_fk_output{
    "PKTABLE_CAT", "PKTABLE_SCHEM", "PKTABLE_NAME", "PKCOLUMN_NAME",
    "FKTABLE_CAT", "FKTABLE_SCHEM", "FKTABLE_NAME", "FKCOLUMN_NAME",
    "KEY_SEQ", "UPDATE_RULE", "DELETE_RULE", "FK_NAME", "PK_NAME", "DEFERRABILITY"};
}

BackendResult<std::string> redshift_foreign_key_database(BackendResult<QueryResult> input,
    RedshiftRoutineBudget& budget, rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra : input->additional_results) {
    if (extra.error) return *extra.error;
  }
  const auto snapshot = input.session_snapshot();
  const auto& raw = *input;
  if (snapshot != SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable} ||
      !valid_result_structure(raw) || !raw.additional_results.empty() ||
      !raw.cell_errors.empty() || !raw.normalized_parameter_types.empty() ||
      raw.columns.size() != 1 || raw.columns[0].name != "database_name" ||
      !raw.columns[0].normalized_type || !raw.columns[0].normalized_type->known ||
      !text(raw.columns[0].normalized_type->type) || raw.rows.size() != 1 ||
      raw.statement_kind != StatementKind::SelectCursor || raw.affected_rows > 1 ||
      !raw.rows[0][0] || !name(*raw.rows[0][0])) {
    return current_fk_invalid("identity", snapshot);
  }
  if (rs::util::Clock::now() >= deadline) return current_fk_timeout();
  auto storage = current_fk_storage(raw);
  RedshiftRoutineUsage owning{0, 0, raw.rows[0][0]->size() + 1 + sizeof(std::string),
      1, raw.rows[0][0]->size()};
  if (!storage || !budget.description(1) || !budget.name(raw.columns[0].name) ||
      !budget.name(*raw.rows[0][0]) || !budget.retain(*storage) || !budget.retain(owning)) {
    return current_fk_limit();
  }
  auto database = redshift_schema_database(std::move(input));
  budget.release(*storage); // Consumed native owning storage has been destroyed.
  if (rs::util::Clock::now() >= deadline) return current_fk_timeout();
  return database;
}

BackendResult<QueryResult> redshift_foreign_key_part(const RedshiftForeignKeyCommandPlan& plan,
    BackendResult<QueryResult> input, RedshiftRoutineBudget& budget, rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra : input->additional_results) {
    if (extra.error) return *extra.error;
  }
  const auto snapshot = input.session_snapshot();
  const auto bad = [&](const char* reason) { return current_fk_invalid(reason, snapshot); };
  const auto& source = *input;
  if (snapshot != SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable} ||
      !valid(plan) || !valid_result_structure(source) || source.columns.size() != 14 ||
      source.rows.size() > 10000 || !source.additional_results.empty() ||
      !source.cell_errors.empty() || source.affected_rows != 0 || !source.statement_kind ||
      (*source.statement_kind != StatementKind::Unknown &&
       *source.statement_kind != StatementKind::SelectCursor) ||
      (!source.normalized_parameter_types.empty() &&
       (source.normalized_parameter_types.size() != 3 ||
        !std::all_of(source.normalized_parameter_types.begin(), source.normalized_parameter_types.end(),
            [](const NativeTypeInfo& type) { return type.known && text(type.type); })))) {
    return bad("completion");
  }
  std::size_t groups = 0, selected = 0, characters = 0;
  {
    // Borrowed validation has no growing index/group allocation. This fixed index
    // dies before the pure normalizer's own fourteen-entry index becomes live.
    std::array<std::size_t, 14> index{};
    for (std::size_t field = 0; field < current_fk_fields.size(); ++field) {
      std::size_t matches = 0;
      for (std::size_t column = 0; column < source.columns.size(); ++column) {
        if (source.columns[column].name != current_fk_fields[field]) continue;
        ++matches;
        index[field] = column;
        const auto& type = source.columns[column].normalized_type;
        if (!type || !type->known) return bad("type");
        if (numeric(field)) {
          if (type->type != ScalarType::SmallInt && type->type != ScalarType::Integer &&
              !(field != 8 && text(type->type) &&
                std::all_of(source.rows.begin(), source.rows.end(),
                    [column](const ResultRow& row) { return !row[column]; }))) return bad("type");
        } else if (!text(type->type)) return bad("type");
      }
      if (matches != 1) return bad("layout");
    }
    const auto cell = [&](const ResultRow& row, std::size_t field) -> const ResultCell& {
      return row[index[field]];
    };
    const auto matches_table = [&](const ResultRow& row, std::size_t offset,
        const RedshiftForeignKeyTable& target) {
      return *cell(row, offset) == target.database && *cell(row, offset + 1) == target.schema &&
          *cell(row, offset + 2) == target.table;
    };
    const auto rule = [&](const ResultRow& row, std::size_t field) {
      return cell(row, field) ? integer(cell(row, field), field == 13 ? 5 : 0, field == 13 ? 7 : 4) :
          std::optional<int>{field == 13 ? 7 : 3};
    };
    // Validate every native row before Both filtering or budget admission.
    for (const auto& row : source.rows) {
      if (rs::util::Clock::now() >= deadline) return current_fk_timeout();
      for (std::size_t field = 0; field < 8; ++field) {
        if (!cell(row, field) || !name(*cell(row, field))) return bad("identity");
      }
      if (!cell(row, 11) || !name(*cell(row, 11)) ||
          (cell(row, 12) && !name(*cell(row, 12))) ||
          !integer(cell(row, 8), 1, 32767) || !rule(row, 9) ||
          !rule(row, 10) || !rule(row, 13)) return bad("constraint");
      if ((plan.foreign && !matches_table(row, 4, *plan.foreign)) ||
          (plan.direction == RedshiftForeignKeyDirection::Exported &&
           !matches_table(row, 0, *plan.primary))) return bad("target");
    }
    for (std::size_t r = 0; r < source.rows.size(); ++r) {
      if (rs::util::Clock::now() >= deadline) return current_fk_timeout();
      const auto& row = source.rows[r];
      std::size_t count = 0;
      int maximum = 0;
      bool first = true;
      for (std::size_t q = 0; q < source.rows.size(); ++q) {
        if (rs::util::Clock::now() >= deadline) return current_fk_timeout();
        const auto& other = source.rows[q];
        if (cell(row, 4) != cell(other, 4) || cell(row, 5) != cell(other, 5) ||
            cell(row, 6) != cell(other, 6) || cell(row, 11) != cell(other, 11)) continue;
        ++count;
        if (q < r) first = false;
        if (cell(row, 0) != cell(other, 0) || cell(row, 1) != cell(other, 1) ||
            cell(row, 2) != cell(other, 2) || cell(row, 12) != cell(other, 12) ||
            rule(row, 9) != rule(other, 9) || rule(row, 10) != rule(other, 10) ||
            rule(row, 13) != rule(other, 13)) return bad("reference");
        const auto sequence = *integer(cell(other, 8), 1, 32767);
        maximum = (std::max)(maximum, sequence);
        if (q != r && (*integer(cell(row, 8), 1, 32767) == sequence ||
            (cell(row, 3) == cell(other, 3) && cell(row, 7) == cell(other, 7)))) {
          return bad("duplicate");
        }
      }
      if (maximum != static_cast<int>(count)) return bad("sequence");
      if (first) ++groups;
      if (plan.direction != RedshiftForeignKeyDirection::Both || matches_table(row, 0, *plan.primary)) {
        ++selected;
      }
    }
    // Name/capacity admission follows full borrowed shape/group validation.
    for (const auto& column : source.columns) {
      if (!budget.name(column.name) || !current_fk_add(characters, column.name.capacity() + 1)) {
        return current_fk_limit();
      }
    }
    for (const auto& row : source.rows) {
      for (const auto& value : row) {
        if (value && (!budget.name(*value) || !current_fk_add(characters, value->capacity() + 1))) {
          return current_fk_limit();
        }
      }
    }
  }
  if (rs::util::Clock::now() >= deadline) return current_fk_timeout();
  if (!budget.description(14)) return current_fk_limit();
  auto raw = current_fk_storage(source);
  const auto rows = source.rows.size();
  RedshiftRoutineUsage peak{rows + selected, rows * 14, 8192, 28 + 2 * groups + 2 * rows};
  if (!raw || !current_fk_add(peak.bytes, characters, 6) ||
      !current_fk_add(peak.bytes, rows, 6 * sizeof(ResultRow)) ||
      !current_fk_add(peak.bytes, rows, 32 * sizeof(ResultCell)) ||
      !current_fk_add(peak.bytes, groups, sizeof(GroupKey) + sizeof(Group) + 256) ||
      !current_fk_add(peak.bytes, rows, sizeof(int) + sizeof(std::pair<std::string, std::string>) + 128) ||
      !current_fk_add(peak.bytes, 14, sizeof(std::size_t)) ||
      !current_fk_add(peak.name_bytes, characters, 6) ||
      !current_fk_add(peak.name_bytes, rows, 16)) return current_fk_limit();
  for (const auto field : current_fk_output) {
    if (!budget.name(field) || !current_fk_add(peak.name_bytes, field.size())) return current_fk_limit();
  }
  if (!budget.retain(*raw) || !budget.retain(peak)) return current_fk_limit();
  auto normalized = normalize_redshift_foreign_keys(plan, std::move(input));
  // Native raw/group storage is now destroyed. Keep its conservative reservation
  // through sorting, including the pure normalizer's moved-from row containers.
  if (!normalized) return normalized.backend_error();
  const std::size_t start = plan.direction == RedshiftForeignKeyDirection::Exported ? 4 : 0;
  std::stable_sort(normalized->rows.begin(), normalized->rows.end(),
      [start](const ResultRow& left, const ResultRow& right) {
        for (std::size_t field = start; field < start + 3; ++field) {
          if (*left[field] != *right[field]) return *left[field] < *right[field];
        }
        unsigned l = 0, r = 0;
        std::from_chars(left[8]->data(), left[8]->data() + left[8]->size(), l);
        std::from_chars(right[8]->data(), right[8]->data() + right[8]->size(), r);
        return l < r;
      });
  if (rs::util::Clock::now() >= deadline) return current_fk_timeout();
  auto owning = current_fk_storage(*normalized);
  if (!owning || owning->rows > peak.rows || owning->cells > peak.cells ||
      owning->bytes > peak.bytes || owning->metadata > peak.metadata ||
      owning->name_bytes > peak.name_bytes) return bad("allocation-model");
  budget.release(*raw);
  budget.release({peak.rows - owning->rows, peak.cells - owning->cells,
      peak.bytes - owning->bytes, peak.metadata - owning->metadata,
      peak.name_bytes - owning->name_bytes});
  return normalized;
}
}  // namespace rs::core::database::postgres
