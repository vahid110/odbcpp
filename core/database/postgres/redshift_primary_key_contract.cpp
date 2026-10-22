#include "redshift_primary_key_contract.h"
#include <algorithm>
#include "core/database/result_validation.h"
#include <array>
#include <charconv>
#include <set>
#include <limits>
#include "redshift_catalog_query.h"

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
      (source.affected_rows != 0 && source.affected_rows != source.rows.size()) ||
      source.columns.size() != 6 || source.rows.size() > 32767)
    return invalid_metadata();
  // The real prepared exchange may retain descriptions for the three bound
  // database/schema/table values. They are input metadata, not catalog fields.
  // Accept only the fixed known text-family triple; never expose it as output
  // or erase malformed/unknown metadata to manufacture catalog success.
  if (!source.normalized_parameter_types.empty() &&
      (source.normalized_parameter_types.size() != 3 ||
       std::any_of(source.normalized_parameter_types.begin(), source.normalized_parameter_types.end(),
           [](const NativeTypeInfo& type) { return !type.known || !text_family(type.type); })))
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

namespace rs::core::database::postgres {
namespace {
BackendError current_pk_invalid(const char* reason,SessionSnapshot snapshot) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      std::string("Invalid current-database primary-key metadata: ")+reason};
  error.public_reason=std::string("primary-key.")+reason;
  error.error_class=BackendErrorClass::InvalidMetadata;error.operation=BackendOperation::ExecuteCatalog;
  error.session_state=snapshot.state;error.disposition=SessionDisposition::Retire;return error;
}
BackendError current_pk_timeout() {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),"Primary-key catalog deadline expired"};
  error.operation=BackendOperation::ExecuteCatalog;return error;
}
BackendError current_pk_limit() {
  auto error=redshift_routine_limit();error.message="Primary-key catalog budget exceeded";return error;
}
bool current_pk_ready(const BackendResult<QueryResult>& input) {
  return input.session_snapshot()==SessionSnapshot{SessionState::Idle,SessionDisposition::Reusable};
}
bool current_pk_add(std::size_t& value,std::size_t count,std::size_t size=1) {
  const auto max=std::numeric_limits<std::size_t>::max();
  if (count>max/size || count*size>max-value) return false;
  value+=count*size;return true;
}
RedshiftRoutineUsage current_pk_raw(const QueryResult& input) {
  auto raw=redshift_routine_schema_usage(input);
  if (!current_pk_add(raw.bytes,input.normalized_parameter_types.capacity(),sizeof(NativeTypeInfo)) ||
      !current_pk_add(raw.metadata,input.normalized_parameter_types.size())) raw.bytes=std::numeric_limits<std::size_t>::max();
  for (const auto& row:input.rows) {
    for (const auto& cell:row) {
      if (cell && !current_pk_add(raw.name_bytes,cell->size())) raw.name_bytes=std::numeric_limits<std::size_t>::max();
    }
  }
  return raw;
}
}
BackendResult<std::string> redshift_primary_key_database(BackendResult<QueryResult> input,
    RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra:input->additional_results) if (extra.error) return *extra.error;
  if (rs::util::Clock::now()>=deadline) return current_pk_timeout();
  const auto snapshot=input.session_snapshot();const auto& source=*input;
  if (!current_pk_ready(input) || !valid_result_structure(source) || !source.additional_results.empty() ||
      !source.cell_errors.empty() || !source.normalized_parameter_types.empty() || source.columns.size()!=1 ||
      source.columns[0].name!="database_name" || !source.columns[0].normalized_type ||
      !source.columns[0].normalized_type->known || !text_family(source.columns[0].normalized_type->type) ||
      source.rows.size()!=1 || source.statement_kind!=StatementKind::SelectCursor || source.affected_rows>1 ||
      !source.rows[0][0] || !identifier(*source.rows[0][0]))
    return current_pk_invalid("identity",snapshot);
  auto raw=current_pk_raw(source);
  const RedshiftRoutineUsage owning{0,0,source.rows[0][0]->size()+sizeof(std::string),1,source.rows[0][0]->size()};
  if (!budget.name(source.columns[0].name) || !budget.name(*source.rows[0][0]) || !budget.description(1) ||
      !budget.retain(raw) || !budget.retain(owning)) return current_pk_limit();
  auto database=redshift_schema_database(std::move(input)); // Consumed native response dies in the existing closed decoder.
  budget.release(raw);
  return database;
}
RedshiftRoutineUsage redshift_primary_key_output_usage(const QueryResult& result) {
  return current_pk_raw(result);
}
BackendResult<QueryResult> redshift_primary_key_part(const RedshiftPrimaryKeyCommandPlan& plan,
    BackendResult<QueryResult> input,RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra:input->additional_results) if (extra.error) return *extra.error;
  if (rs::util::Clock::now()>=deadline) return current_pk_timeout();
  const auto snapshot=input.session_snapshot();const auto& source=*input;
  const auto bad=[&](const char* reason) {return current_pk_invalid(reason,snapshot);};
  if (!current_pk_ready(input) || !valid_plan(plan) || !valid_result_structure(source) ||
      !source.additional_results.empty() || !source.cell_errors.empty() || source.columns.size()!=6 ||
      source.rows.size()>10000 ||
      (source.affected_rows!=0 && source.affected_rows!=source.rows.size()) || !source.statement_kind ||
      (*source.statement_kind!=StatementKind::Unknown && *source.statement_kind!=StatementKind::SelectCursor) ||
      (!source.normalized_parameter_types.empty() && (source.normalized_parameter_types.size()!=3 ||
       std::any_of(source.normalized_parameter_types.begin(),source.normalized_parameter_types.end(),
           [](const NativeTypeInfo& type){return !type.known || !text_family(type.type);}))))
    return bad("completion");
  {
    constexpr std::array<std::string_view,6> names{"database_name","schema_name","table_name","column_name","key_seq","pk_name"};
    std::array<std::size_t,6> index{};
    for (std::size_t n=0;n<names.size();++n) {
      std::size_t count=0;
      for (std::size_t c=0;c<source.columns.size();++c) {
        if (source.columns[c].name!=names[n]) continue;
        index[n]=c;++count;
        const auto& type=source.columns[c].normalized_type;
        if (!type || !type->known || (n==4?(type->type!=ScalarType::SmallInt && type->type!=ScalarType::Integer):
            !text_family(type->type))) return bad("type");
      }
      if (count!=1) return bad("layout");
    }
    unsigned maximum=0;
    for (std::size_t r=0;r<source.rows.size();++r) {
      if (rs::util::Clock::now()>=deadline) return current_pk_timeout();
      const auto& row=source.rows[r];
      for (const auto n:{0u,1u,2u,3u}) if (!row[index[n]] || !identifier(*row[index[n]])) return bad("identity");
      if (*row[index[0]]!=plan.database || *row[index[1]]!=plan.schema || *row[index[2]]!=plan.table ||
          (row[index[5]] && !identifier(*row[index[5]])) ||
          (r && row[index[5]]!=source.rows[0][index[5]]) || !row[index[4]]) return bad("foreign");
      const auto sequence=[&](const ResultRow& value)->unsigned {
        const auto& text=*value[index[4]];unsigned seq=0;
        if (text.empty() || text.size()>5 || text.find_first_not_of("0123456789")!=std::string::npos) return 0;
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),seq);
        return parsed.ec==std::errc{} && parsed.ptr==text.data()+text.size() && seq<=32767?seq:0;
      };
      const auto seq=sequence(row);if (!seq) return bad("sequence");maximum=std::max(maximum,seq);
      // Borrowed full validation admits no duplicate set before an overflow can hide malformed rows.
      for (std::size_t p=0;p<r;++p) {
        if (rs::util::Clock::now()>=deadline) return current_pk_timeout();
        if (seq==sequence(source.rows[p]) || row[index[3]]==source.rows[p][index[3]]) return bad("duplicate");
      }
    }
    if (maximum!=source.rows.size()) return bad("sequence-gap");
  } // Borrowed validation index dies before the pure normalizer creates its own.
  auto raw=current_pk_raw(source);const auto count=source.rows.size();
  // Six native index slots, six live projected descriptors, two duplicate-set
  // nodes per row, and the normalizer's owning PK-name comparison identity.
  // A normalized row is moved into output; its temporary adds no live row/cells.
  RedshiftRoutineUsage peak{count,count*6,0,12+count*2+(count?1:0),0};
  if (!current_pk_add(peak.bytes,raw.bytes,3) || !current_pk_add(peak.bytes,count+1,
      2*sizeof(ResultRow)+8*sizeof(ResultCell)+256) ||
      !current_pk_add(peak.bytes,8,sizeof(ResultColumnMetadata)) || !current_pk_add(peak.bytes,4096) ||
      !current_pk_add(peak.name_bytes,raw.name_bytes,3) || !current_pk_add(peak.name_bytes,55)) return current_pk_limit();
  constexpr std::array<std::string_view,6> headers{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","COLUMN_NAME","KEY_SEQ","PK_NAME"};
  for (const auto header:headers) if (!budget.name(header)) return current_pk_limit();
  for (const auto& column:source.columns) if (!budget.name(column.name)) return current_pk_limit();
  for (const auto& row:source.rows) for (const auto& cell:row) if (cell && !budget.name(*cell)) return current_pk_limit();
  if (!budget.description(6) || !budget.retain(raw) || !budget.retain(peak)) return current_pk_limit();
  auto output=normalize_redshift_primary_keys(plan,std::move(input)); // Raw input and normalizer sets are destroyed at return.
  budget.release(raw);
  if (!output) return output.backend_error();
  if (rs::util::Clock::now()>=deadline) return current_pk_timeout();
  std::sort(output->rows.begin(),output->rows.end(),[](const ResultRow& left,const ResultRow& right) {
    unsigned l=0,r=0;std::from_chars(left[4]->data(),left[4]->data()+left[4]->size(),l);
    std::from_chars(right[4]->data(),right[4]->data()+right[4]->size(),r);return l<r;
  });
  if (rs::util::Clock::now()>=deadline) return current_pk_timeout();
  const auto retained=current_pk_raw(*output);
  if (retained.rows>peak.rows || retained.cells>peak.cells || retained.bytes>peak.bytes ||
      retained.metadata>peak.metadata || retained.name_bytes>peak.name_bytes) return current_pk_limit();
  budget.release({peak.rows-retained.rows,peak.cells-retained.cells,peak.bytes-retained.bytes,
      peak.metadata-retained.metadata,peak.name_bytes-retained.name_bytes});
  return output;
}
} // namespace rs::core::database::postgres
