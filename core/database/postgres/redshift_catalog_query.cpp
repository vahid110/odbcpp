#include "redshift_catalog_query.h"
#include "core/database/result_validation.h"
#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <type_traits>
#include <stdexcept>
#include <charconv>
#include <limits>
#include "pg_protocol_parser.h"

namespace rs::core::database::postgres {

rs::util::Result<CatalogDatabaseScope> redshift_catalog_database_scope(
    CatalogDatabaseScope requested, const std::optional<std::string>& datashare_enabled,
    const std::optional<std::string>& external_database) {
  if (requested != CatalogDatabaseScope::CurrentDatabase &&
      requested != CatalogDatabaseScope::VisibleDatabases)
    return {rs::util::DbErrorCode::InvalidParameter, "Invalid metadata database scope"};
  const auto valid = [](const std::optional<std::string>& value) {
    return !value || *value == "on" || *value == "off";
  };
  if (!valid(datashare_enabled) || !valid(external_database))
    return {rs::util::DbErrorCode::ProtocolError,
            "Invalid authenticated metadata scope parameter"};
  return (external_database && *external_database == "on") ||
          (requested == CatalogDatabaseScope::VisibleDatabases && datashare_enabled && *datashare_enabled == "on")
      ? CatalogDatabaseScope::VisibleDatabases : CatalogDatabaseScope::CurrentDatabase;
}

namespace {
std::string literal(const std::string& value) {
  std::string result{"'"};
  for (const char ch : value) {
    if (ch == '\'' || ch == '\\') result.push_back(ch);
    result.push_back(ch);
  }
  result.push_back('\'');
  return result;
}
}  // namespace

namespace {
BackendError invalid_schema_metadata(const char* fixed_reason) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      std::string("Invalid Redshift schema metadata: ") + fixed_reason};
  error.error_class = BackendErrorClass::InvalidMetadata;
  error.operation = BackendOperation::ExecuteCatalog;
  return error;
}
bool schema_identifier(std::string_view value) {
  return !value.empty() && value.find('\0') == std::string_view::npos &&
      rs::util::utf8_code_point_count(value).has_value();
}
bool schema_text(const ResultColumnMetadata& column) {
  if (!column.normalized_type || !column.normalized_type->known) return false;
  const auto type = column.normalized_type->type;
  return type == ScalarType::Char || type == ScalarType::VarChar ||
      type == ScalarType::LongVarChar;
}
// The seven-field native SHOW layout is shared by schema enumeration, routine
// schema discovery and SQLColumns. Caller-specific matching/admission stays local.
std::optional<std::array<std::size_t,7>> show_schema_indexes(const QueryResult& source) {
  if (source.columns.size()!=7) return std::nullopt;
  constexpr std::array<std::string_view,7> fields{"database_name","schema_name","schema_owner",
      "schema_type","schema_acl","source_database","schema_option"};
  std::array<std::size_t,7> index{};
  for (std::size_t n=0;n<fields.size();++n) {
    unsigned count=0;
    for (std::size_t c=0;c<source.columns.size();++c) {
      if (source.columns[c].name==fields[n]) {index[n]=c;++count;}
    }
    if (count!=1) return std::nullopt;
  }
  return index;
}
bool clean_schema_result(const QueryResult& source) {
  return valid_result_structure(source) && source.additional_results.empty() &&
      source.cell_errors.empty() && source.normalized_parameter_types.empty();
}
}  // namespace

BackendResult<std::string_view> redshift_schema_database_view(const BackendResult<QueryResult>& input) {
  if (!input) return input.backend_error();
  const auto& source = *input;
  if (source.error) return *source.error;
  for (const auto& extra : source.additional_results)
    if (extra.error) return *extra.error;
  // Keep the original short-circuit order; diagnostics carry fixed local labels.
  if (!clean_schema_result(source)) return invalid_schema_metadata("lookup-structure");
  if (source.columns.size() != 1) return invalid_schema_metadata("lookup-columns");
  if (source.columns[0].name != "database_name") return invalid_schema_metadata("lookup-name");
  if (!schema_text(source.columns[0])) return invalid_schema_metadata("lookup-type");
  if (source.rows.size() != 1) return invalid_schema_metadata("lookup-rows");
  // Returned cardinality is checked above; a SELECT completion can omit its
  // affected count. Require explicit completed SELECT metadata so an unfinished
  // result (also normalized to zero) cannot supply the database identity.
  if (source.statement_kind != StatementKind::SelectCursor || source.affected_rows > 1)
    return invalid_schema_metadata("lookup-completion");
  if (!source.rows[0][0] || !schema_identifier(*source.rows[0][0]))
    return invalid_schema_metadata("lookup-identifier");
  if (input.session_snapshot().disposition == SessionDisposition::Retire)
    return invalid_schema_metadata("lookup-snapshot");
  return BackendResult<std::string_view>{*source.rows[0][0], input.session_snapshot()};
}

BackendResult<std::string> redshift_schema_database(BackendResult<QueryResult> input) {
  auto identity = redshift_schema_database_view(input);
  if (!identity) return identity.backend_error();
  return BackendResult<std::string>{std::string(*identity), identity.session_snapshot()};
}

BackendResult<std::vector<std::string>> normalize_redshift_databases(
    BackendResult<QueryResult> input) {
  if (!input) return input.backend_error();
  const auto& source = *input;
  if (source.error) return *source.error;
  for (const auto& extra : source.additional_results)
    if (extra.error) return *extra.error;
  if (!clean_schema_result(source) ||
      (source.columns.size() != 6 && source.columns.size() != 7) ||
      source.rows.size() > 10000 ||
      !source.statement_kind || (*source.statement_kind != StatementKind::Unknown &&
                                *source.statement_kind != StatementKind::SelectCursor) ||
      (source.affected_rows != 0 && source.affected_rows != source.rows.size()))
    return invalid_schema_metadata("databases-structure");
  if (input.session_snapshot().state != SessionState::Idle ||
      input.session_snapshot().disposition != SessionDisposition::Reusable)
    return invalid_schema_metadata("databases-snapshot");
  constexpr std::array<std::string_view, 6> names{
      "database_name", "database_owner", "database_type", "database_acl",
      "parameters", "database_isolation_level"};
  std::size_t identity = 0;
  for (const auto expected : names) {
    unsigned matches = 0;
    for (std::size_t index = 0; index < source.columns.size(); ++index) {
      if (source.columns[index].name == expected) {
        ++matches;
        if (expected == "database_name") identity = index;
      }
    }
    if (matches != 1) return invalid_schema_metadata("databases-layout");
  }
  // This projection owns names only. The optional field is opaque server
  // metadata, just like the five existing nonidentity fields; never filter on it.
  if (source.columns.size() == 7 &&
      std::count_if(source.columns.begin(), source.columns.end(), [](const auto& column) {
        return column.name == "allow_connections";
      }) != 1)
    return invalid_schema_metadata("databases-layout");
  if (!schema_text(source.columns[identity])) return invalid_schema_metadata("databases-type");
  std::set<std::string> databases;
  for (const auto& row : source.rows) {
    if (!row[identity] || !schema_identifier(*row[identity]))
      return invalid_schema_metadata("databases-identifier");
    if (!databases.insert(*row[identity]).second)
      return invalid_schema_metadata("databases-duplicate");
  }
  return BackendResult<std::vector<std::string>>{
      std::vector<std::string>{databases.begin(), databases.end()}, input.session_snapshot()};
}

std::string redshift_show_schemas_command(std::string_view validated_database) {
  // Called only with an owning identifier from redshift_schema_database.
  // Identifiers are quoted, not SQL literals or prepared value placeholders.
  std::string command = "SHOW SCHEMAS FROM DATABASE \"";
  for (const char ch : validated_database) {
    if (ch == '\"') command.push_back('\"');
    command.push_back(ch);
  }
  command += "\";";
  return command;
}

BackendResult<QueryResult> normalize_redshift_schemas(
    std::string_view database, BackendResult<QueryResult> input) {
  if (!input) return input.backend_error();
  const auto& source = *input;
  if (source.error) return *source.error;
  for (const auto& extra : source.additional_results)
    if (extra.error) return *extra.error;
  if (!schema_identifier(database)) return invalid_schema_metadata("show-database");
  if (!clean_schema_result(source)) return invalid_schema_metadata("show-structure");
  // Preserve the strict seven-field normalization contract. Actual server layout
  // remains unqualified; these refusal labels measure it without accepting more.
  if (source.columns.size() != 7) return invalid_schema_metadata("show-columns");
  if (source.rows.size() > 10000) return invalid_schema_metadata("show-rows");
  if (source.affected_rows != 0 && source.affected_rows != source.rows.size())
    return invalid_schema_metadata("show-completion");
  const auto layout=show_schema_indexes(source);
  if (!layout) return invalid_schema_metadata("show-layout");
  const auto& indexes=*layout;
  if (!schema_text(source.columns[indexes[0]]) ||
      !schema_text(source.columns[indexes[1]])) return invalid_schema_metadata("show-type");
  std::set<std::string> schemas;
  for (const auto& row : source.rows) {
    const auto& db = row[indexes[0]];
    const auto& schema = row[indexes[1]];
    if (!db || !schema) return invalid_schema_metadata("show-null-identity");
    if (*db != database) return invalid_schema_metadata("show-foreign-database");
    if (!schema_identifier(*schema)) return invalid_schema_metadata("show-identifier");
    if (!schemas.insert(*schema).second) return invalid_schema_metadata("show-duplicate");
  }
  QueryResult output;
  for (const auto* name : {"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS"})
    output.columns.push_back({name, NativeTypeInfo{ScalarType::VarChar, 0, 0, true}});
  // Recognition is not a guessed capacity. Preserve the actual source's schema
  // dimension (including unknown zero), but normalize its published text family.
  output.columns[1].normalized_type->column_size =
      declared_character_metadata(*source.columns[indexes[1]].normalized_type).column_size;
  for (const auto& schema : schemas)
    output.rows.push_back({std::nullopt, schema, std::nullopt, std::nullopt, std::nullopt});
  return BackendResult<QueryResult>{std::move(output), input.session_snapshot()};
}


namespace {
// Closed current-schema admission arithmetic, not an allocator/RSS estimate.
bool current_schema_add(std::size_t& total, std::size_t count, std::size_t width = 1) {
  const auto max = std::numeric_limits<std::size_t>::max();
  if (width && count > max / width) return false;
  const auto bytes = count * width;
  if (bytes > max - total) return false;
  total += bytes;
  return true;
}
bool current_schema_string(std::size_t& bytes, std::size_t length) {
  return current_schema_add(bytes, length, 2) && current_schema_add(bytes, 2);
}
}  // namespace

BackendError redshift_current_schema_limit(SessionSnapshot snapshot) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),
      "Redshift schema enumeration storage budget exceeded"};
  error.operation = BackendOperation::ExecuteCatalog;
  error.session_state = snapshot.state;
  error.disposition = snapshot.disposition;
  return error;
}

std::optional<RedshiftRoutineUsage> redshift_current_schema_raw_storage(const QueryResult& source) {
  RedshiftRoutineUsage used{source.rows.size(), 0, sizeof(QueryResult), source.columns.size(), 0};
  if (!current_schema_add(used.bytes, source.columns.capacity(), sizeof(ResultColumnMetadata)) ||
      !current_schema_add(used.bytes, source.rows.capacity(), sizeof(ResultRow)) ||
      !current_schema_add(used.bytes, source.additional_results.capacity(), sizeof(QueryResult)) ||
      !current_schema_add(used.bytes, source.normalized_parameter_types.capacity(), sizeof(NativeTypeInfo)) ||
      !current_schema_add(used.bytes, source.cell_errors.capacity(), sizeof(CellEncodingError))) return std::nullopt;
  for (const auto& column : source.columns) {
    if (!current_schema_add(used.bytes, column.name.capacity()) || !current_schema_add(used.bytes, 1) ||
        !current_schema_add(used.name_bytes, column.name.size())) return std::nullopt;
  }
  for (const auto& row : source.rows) {
    if (!current_schema_add(used.cells, row.size()) ||
        !current_schema_add(used.bytes, row.capacity(), sizeof(ResultCell))) return std::nullopt;
    for (std::size_t c = 0; c < row.size(); ++c) {
      if (!row[c]) continue;
      if (!current_schema_add(used.bytes, row[c]->capacity()) || !current_schema_add(used.bytes, 1))
        return std::nullopt;
      if ((source.columns[c].name == "database_name" || source.columns[c].name == "schema_name") &&
          !current_schema_add(used.name_bytes, row[c]->size())) return std::nullopt;
    }
  }
  return used;
}

std::optional<RedshiftRoutineUsage> redshift_current_schema_database_storage(std::string_view database) {
  RedshiftRoutineUsage used{0, 0, sizeof(std::string), 0, database.size()};
  if (!current_schema_string(used.bytes, database.size())) return std::nullopt;
  return used;
}

BackendResult<RedshiftCurrentSchemaSource> admit_redshift_current_schemas(std::string_view database,
    const BackendResult<QueryResult>& input, RedshiftRoutineBudget& budget,
    rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  const auto& source = *input;
  if (source.error) return *source.error;
  for (const auto& extra : source.additional_results) {
    if (extra.error) return *extra.error;
  }
  if (!schema_identifier(database)) return invalid_schema_metadata("show-database");
  if (!clean_schema_result(source)) return invalid_schema_metadata("show-structure");
  if (source.columns.size() != 7) return invalid_schema_metadata("show-columns");
  if (source.rows.size() > 10000) return invalid_schema_metadata("show-rows");
  if (source.affected_rows != 0 && source.affected_rows != source.rows.size())
    return invalid_schema_metadata("show-completion");
  const auto layout = show_schema_indexes(source);
  if (!layout) return invalid_schema_metadata("show-layout");
  const auto& indexes = *layout;
  if (!schema_text(source.columns[indexes[0]]) || !schema_text(source.columns[indexes[1]]))
    return invalid_schema_metadata("show-type");
  const auto expired = [deadline] { return rs::util::Clock::now() >= deadline; };
  const auto timeout = [] {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),
        "Schema enumeration normalization deadline expired"};
    error.operation = BackendOperation::ExecuteCatalog;
    return error;
  };
  // Borrowed validation precedes both budget refusal and duplicate-index
  // allocation. No malformed hidden row becomes a prefix or an empty success.
  for (std::size_t r = 0; r < source.rows.size(); ++r) {
    if (expired()) return timeout();
    const auto& row = source.rows[r];
    if (!row[indexes[0]] || !row[indexes[1]]) return invalid_schema_metadata("show-null-identity");
    if (*row[indexes[0]] != database) return invalid_schema_metadata("show-foreign-database");
    if (!schema_identifier(*row[indexes[1]])) return invalid_schema_metadata("show-identifier");
    for (std::size_t previous = 0; previous < r; ++previous) {
      if (expired()) return timeout();
      if (source.rows[previous][indexes[1]] == row[indexes[1]]) return invalid_schema_metadata("show-duplicate");
    }
  }
  const auto limit = [&] { return redshift_current_schema_limit(input.session_snapshot()); };
  if (expired()) return timeout();
  if (!budget.description(source.columns.size()) || !budget.description(5)) return limit();
  for (const auto& column : source.columns) {
    if (!budget.name(column.name)) return limit();
  }
  for (const auto& row : source.rows) {
    if (!budget.name(*row[indexes[0]]) || !budget.name(*row[indexes[1]])) return limit();
  }
  const auto raw = redshift_current_schema_raw_storage(source);
  // The caller still holds the quoted-command reservation: this is the D+C+R
  // peak, before any destruction makes those bytes available to projection.
  if (!raw || !budget.retain(*raw)) return limit();
  return BackendResult<RedshiftCurrentSchemaSource>{{indexes, *raw}, input.session_snapshot()};
}

BackendResult<QueryResult> normalize_redshift_current_schemas(
    const BackendResult<QueryResult>& input, const RedshiftCurrentSchemaSource& admitted,
    RedshiftRoutineBudget& budget, rs::util::Deadline deadline) {
  const auto& source = *input;
  const auto& indexes = admitted.indexes;
  const auto expired = [deadline] { return rs::util::Clock::now() >= deadline; };
  const auto timeout = [] {
    BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),
        "Schema enumeration normalization deadline expired"};
    error.operation = BackendOperation::ExecuteCatalog;
    return error;
  };
  const auto limit = [&] { return redshift_current_schema_limit(input.session_snapshot()); };
  if (expired()) return timeout();
  RedshiftRoutineUsage scratch{0, 0, sizeof(std::set<std::string>), source.rows.size(), 0};
  RedshiftRoutineUsage output{source.rows.size(), 0, sizeof(QueryResult), 5, 0};
  constexpr std::array<std::string_view, 5> headers{
      "TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "TABLE_TYPE", "REMARKS"};
  if (!current_schema_add(scratch.bytes, source.rows.size(), sizeof(std::string) + 4 * sizeof(void*)) ||
      !current_schema_add(output.cells, source.rows.size(), 5) ||
      !current_schema_add(output.bytes, 5, sizeof(ResultColumnMetadata)) ||
      !current_schema_add(output.bytes, source.rows.size(), sizeof(ResultRow)) ||
      !current_schema_add(output.bytes, source.rows.size(), 5 * sizeof(ResultCell))) return limit();
  for (const auto name : headers) {
    if (!budget.name(name) || !current_schema_string(output.bytes, name.size()) ||
        !current_schema_add(output.name_bytes, name.size())) return limit();
  }
  for (const auto& row : source.rows) {
    const auto length = row[indexes[1]]->size();
    if (!current_schema_string(scratch.bytes, length) || !current_schema_add(scratch.name_bytes, length) ||
        !current_schema_string(output.bytes, length) || !current_schema_add(output.name_bytes, length)) return limit();
  }
  // Raw is already charged by admission. Command storage has actually died;
  // its released bytes can now hold set/output, without charging raw twice.
  if (!budget.retain(scratch) || !budget.retain(output)) return limit();
  QueryResult result;
  {
    std::set<std::string> schemas;
    for (const auto& row : source.rows) {
      if (expired()) return timeout();
      schemas.insert(*row[indexes[1]]);
    }
    result.columns.resize(headers.size());
    for (std::size_t n = 0; n < headers.size(); ++n) {
      result.columns[n].name = headers[n];
      result.columns[n].normalized_type = NativeTypeInfo{ScalarType::VarChar, 0, 0, true};
    }
    result.columns[1].normalized_type->column_size =
        declared_character_metadata(*source.columns[indexes[1]].normalized_type).column_size;
    result.rows.reserve(schemas.size());
    for (const auto& schema : schemas) {
      if (expired()) return timeout();
      result.rows.emplace_back();
      result.rows.back().resize(5);
      result.rows.back()[1] = schema;
    }
  }
  budget.release(scratch); // All set nodes/names are destroyed, owning output remains live and charged.
  if (expired()) return timeout();
  return BackendResult<QueryResult>{std::move(result), input.session_snapshot()};
}


std::optional<CatalogRequest> redshift_identifier_show_request(const CatalogRequest& validated_native) {
  CatalogRequest lowered = validated_native;
  const auto escape = [](std::optional<std::string>& name, CatalogNameMatch match) {
    if (match == CatalogNameMatch::Existing || !name) return;
    std::string pattern;
    for (const char ch : *name) {
      if (ch == '\\' || ch == '%' || ch == '_') pattern.push_back('\\');
      pattern.push_back(ch);
    }
    name = std::move(pattern);
  };
  if (auto* tables = std::get_if<TablesCatalogRequest>(&lowered)) {
    if (tables->mode != TablesCatalogRequest::Mode::Tables) return std::nullopt;
    escape(tables->catalog, tables->name_matches.catalog);
    escape(tables->schema, tables->name_matches.schema);
    escape(tables->table, tables->name_matches.object);
    tables->name_matches = {};
  } else if (auto* columns = std::get_if<ColumnsCatalogRequest>(&lowered)) {
    escape(columns->schema, columns->name_matches.schema);
    escape(columns->table, columns->name_matches.object);
    escape(columns->column, columns->name_matches.member);
    columns->name_matches = {};
  } else return std::nullopt;
  return lowered;
}

std::optional<CatalogRequest> native_catalog_request(const CatalogRequest& input) {
  auto output = input;
  const auto normalize = [](auto& value, CatalogNameMatch match) {
    if (match != CatalogNameMatch::Existing && match != CatalogNameMatch::IdentifierUnquoted &&
        match != CatalogNameMatch::IdentifierQuoted) return false;
    if (match == CatalogNameMatch::Existing) return true;
    std::string* text = nullptr;
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::optional<std::string>>) {
      if (!value) return false;
      text = &*value;
    } else text = &value;
    if (text->find('\0') != std::string::npos || !rs::util::utf8_code_point_count(*text)) return false;
    if (match == CatalogNameMatch::IdentifierUnquoted)
      for (char& ch : *text) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    return true;
  };
  const bool valid = std::visit([&](auto& names) {
    using T = std::decay_t<decltype(names)>;
    const auto& m = names.name_matches;
    if constexpr (std::is_same_v<T, ForeignKeysCatalogRequest>) {
      return m.member == CatalogNameMatch::Existing &&
          normalize(names.primary_catalog,m.catalog) && normalize(names.primary_schema,m.schema) &&
          normalize(names.primary_table,m.object) && normalize(names.foreign_catalog,m.foreign_catalog) &&
          normalize(names.foreign_schema,m.foreign_schema) && normalize(names.foreign_table,m.foreign_object);
    } else {
      if (m.foreign_catalog != CatalogNameMatch::Existing || m.foreign_schema != CatalogNameMatch::Existing ||
          m.foreign_object != CatalogNameMatch::Existing) return false;
      if constexpr (std::is_same_v<T, TablesCatalogRequest>)
        if (names.mode != TablesCatalogRequest::Mode::Tables) return m.existing();
      if (!normalize(names.catalog,m.catalog) || !normalize(names.schema,m.schema)) return false;
      if constexpr (std::is_same_v<T, ProceduresCatalogRequest> || std::is_same_v<T, ProcedureColumnsCatalogRequest>) {
        if (!normalize(names.procedure,m.object)) return false;
      } else if (!normalize(names.table,m.object)) return false;
      if constexpr (std::is_same_v<T, ColumnsCatalogRequest> || std::is_same_v<T, ProcedureColumnsCatalogRequest>) {
        return normalize(names.column,m.member);
      } else {
        return m.member == CatalogNameMatch::Existing;
      }
    }
  }, output);
  if (!valid) return std::nullopt;
  return output;
}

bool redshift_table_schema_is_literal(const TablesCatalogRequest& request) noexcept {
  if (request.mode != TablesCatalogRequest::Mode::Tables || !request.schema ||
      request.schema->empty()) return false;
  bool escaped = false;
  for (const char ch : *request.schema) {
    if (escaped) { escaped = false; continue; }
    if (ch == '\\') escaped = true;
    else if (ch == '%' || ch == '_') return false;
  }
  return !escaped;
}

namespace { bool table_pattern_valid(const std::optional<std::string>& pattern); }

std::optional<std::string> redshift_table_schema(const TablesCatalogRequest& request) {
  if (!table_pattern_valid(request.catalog) || !table_pattern_valid(request.table)) return std::nullopt;
  if (request.types) for (const auto& type : *request.types)
    if (type.find('\0') != std::string::npos || !rs::util::utf8_code_point_count(type)) return std::nullopt;
  if (!redshift_table_schema_is_literal(request)) return std::nullopt;
  std::string result;
  bool escaped = false;
  for (const char ch : *request.schema) {
    if (!escaped && ch == '\\') { escaped = true; continue; }
    result.push_back(ch); escaped = false;
  }
  if (!schema_identifier(result)) return std::nullopt;
  return result;
}

namespace {
BackendError invalid_table_metadata(const char* reason) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      std::string("Invalid Redshift table metadata: ") + reason};
  error.error_class = BackendErrorClass::InvalidMetadata;
  error.operation = BackendOperation::ExecuteCatalog;
  return error;
}
bool table_pattern_valid(const std::optional<std::string>& pattern) {
  if (!pattern) return true;
  if (pattern->find('\0') != std::string::npos ||
      !rs::util::utf8_code_point_count(*pattern)) return false;
  bool escaped = false;
  for (char ch : *pattern) { if (escaped) escaped = false; else if (ch == '\\') escaped = true; }
  return !escaped;
}
std::size_t codepoint_end(std::string_view text, std::size_t i) {
  ++i;
  while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) ++i;
  return i;
}
// Local LIKE filtering matches whole UTF8 code points for '_'. Both inputs are
// validated first. Greedy '%' backtracking is bounded by the owning value length.
bool table_like(std::string_view value, const std::optional<std::string>& pattern) {
  if (!pattern) return true;
  const std::string_view p = *pattern;
  std::size_t v = 0, i = 0, star = std::string_view::npos, retry = 0;
  while (v < value.size()) {
    if (i < p.size() && p[i] == '%') { star = ++i; retry = v; continue; }
    if (i < p.size() && p[i] == '_') { ++i; v = codepoint_end(value, v); continue; }
    auto literal_start = i;
    if (literal_start < p.size() && p[literal_start] == '\\') ++literal_start;
    if (literal_start < p.size()) {
      const auto end = codepoint_end(p, literal_start);
      const auto vend = codepoint_end(value, v);
      if (p.substr(literal_start, end-literal_start) == value.substr(v, vend-v)) {
        i = end; v = vend; continue;
      }
    }
    if (star == std::string_view::npos) return false;
    retry = codepoint_end(value, retry); v = retry; i = star;
  }
  while (i < p.size() && p[i] == '%') ++i;
  return i == p.size();
}
}

rs::util::Result<bool> redshift_database_matches(std::string_view database,
    const std::optional<std::string>& requested, bool pattern) {
  if (requested && (requested->find('\0') != std::string::npos ||
      !rs::util::utf8_code_point_count(*requested) || (pattern && !table_pattern_valid(requested))))
    return {rs::util::DbErrorCode::InvalidParameter, "Invalid metadata catalog argument"};
  return !requested || (pattern ? table_like(database, requested) : database == *requested);
}

std::string redshift_show_tables_command(std::string_view database, std::string_view schema) {
  const auto quote = [](std::string_view name) {
    std::string out{"\""};
    for (char ch : name) { if (ch == '"') out.push_back('"'); out.push_back(ch); }
    out.push_back('"'); return out;
  };
  // V4 per-schema grammar in pinned AWS helper182, not V5 database-wide grammar.
  return "SHOW TABLES FROM SCHEMA " + quote(database) + "." + quote(schema) + ";";
}

BackendResult<QueryResult> normalize_redshift_tables(std::string_view database,
    std::string_view schema, const TablesCatalogRequest& request, BackendResult<QueryResult> input,
    bool detailed_types) {
  if (!input) return input.backend_error();
  const auto& source = *input;
  if (source.error) return *source.error;
  for (const auto& extra : source.additional_results) if (extra.error) return *extra.error;
  if (!schema_identifier(database) || !schema_identifier(schema)) return invalid_table_metadata("identity");
  if (!table_pattern_valid(request.catalog) || !table_pattern_valid(request.table)) return invalid_table_metadata("pattern");
  if (!clean_schema_result(source)) return invalid_table_metadata("structure");
  if (source.rows.size() > 10000) return invalid_table_metadata("rows");
  // CommandComplete is distinct from its optional row count. SHOW maps to
  // Unknown; SELECT-form completion also occurs in metadata result protocols.
  if (!source.statement_kind || (*source.statement_kind != StatementKind::Unknown &&
      *source.statement_kind != StatementKind::SelectCursor) ||
      (source.affected_rows != 0 && source.affected_rows != source.rows.size()))
    return invalid_table_metadata("completion");
  if (input.session_snapshot().disposition == SessionDisposition::Retire) return invalid_table_metadata("snapshot");
  // Normalize the five named fields consumed by the pinned official SQLTables
  // processor. Only documented/pinned optional fields are allowed; no assumed
  // version-to-field-count mapping. Actual server layout still needs live proof.
  constexpr std::array<std::string_view,11> names{"database_name","schema_name","table_name",
      "table_type","remarks","table_acl","owner","last_altered_time","last_modified_time","dist_style","table_subtype"};
  std::array<std::size_t,5> indexes{};
  std::set<std::string_view> headers;
  if (source.columns.size() < 5 || source.columns.size() > names.size()) return invalid_table_metadata("columns");
  for (std::size_t i = 0; i < source.columns.size(); ++i) {
    const auto name = std::string_view(source.columns[i].name);
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end() || !headers.insert(name).second) return invalid_table_metadata("layout");
    const auto index = static_cast<std::size_t>(found-names.begin());
    if (index < indexes.size()) indexes[index] = i;
  }
  for (std::size_t i = 0; i < indexes.size(); ++i)
    if (!headers.contains(names[i]) || !schema_text(source.columns[indexes[i]])) return invalid_table_metadata("type");
  const auto types = catalog_table_types();
  std::set<std::string> identities;
  QueryResult output;
  constexpr std::array<const char*,5> outnames{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","TABLE_TYPE","REMARKS"};
  for (std::size_t i = 0; i < indexes.size(); ++i) {
    auto type = declared_character_metadata(*source.columns[indexes[i]].normalized_type);
    type.type = ScalarType::VarChar;
    output.columns.push_back({outnames[i], type});
  }
  // Validate every row before filtering: a nonmatching malformed row is still a
  // protocol refusal, never fabricated empty success or silently dropped error.
  for (const auto& row : source.rows) {
    for (std::size_t i = 0; i < 4; ++i)
      if (!row[indexes[i]] || !schema_identifier(*row[indexes[i]])) return invalid_table_metadata("row-identity");
    if (*row[indexes[0]] != database || *row[indexes[1]] != schema) return invalid_table_metadata("foreign-identity");
    if (!identities.insert(*row[indexes[2]]).second) return invalid_table_metadata("duplicate");
    if (std::find(types.begin(),types.end(),*row[indexes[3]]) == types.end()) return invalid_table_metadata("table-type");
    if (row[indexes[4]] && (row[indexes[4]]->find('\0') != std::string::npos ||
        !rs::util::utf8_code_point_count(*row[indexes[4]]))) return invalid_table_metadata("remarks");
    // Classify before filtering, so TABLE includes external/system/temp tables
    // when detailed labels are disabled. Validate native rows before this step.
    const auto presented = catalog_table_type(*row[indexes[3]], detailed_types);
    if (!presented) return invalid_table_metadata("table-type");
    if (!table_like(database,request.catalog) || !table_like(*row[indexes[2]],request.table) ||
        (request.types && std::find(request.types->begin(),request.types->end(),*presented)==request.types->end())) continue;
    output.rows.push_back({row[indexes[0]],row[indexes[1]],row[indexes[2]],std::string(*presented),row[indexes[4]]});
  }
  std::sort(output.rows.begin(),output.rows.end(),[](const auto& a,const auto& b) {
    return std::tie(a[3],a[0],a[1],a[2]) < std::tie(b[3],b[0],b[1],b[2]);
  });
  return BackendResult<QueryResult>{std::move(output),input.session_snapshot()};
}


bool redshift_tables_schema_wide(const TablesCatalogRequest& request) noexcept {
  return request.mode==TablesCatalogRequest::Mode::Tables && request.name_matches.existing() &&
      !redshift_table_schema_is_literal(request);
}
bool redshift_tables_patterns_valid(const TablesCatalogRequest& request) {
  if (!table_pattern_valid(request.catalog) || !table_pattern_valid(request.schema) ||
      !table_pattern_valid(request.table)) return false;
  if (request.types) for (const auto& type:*request.types)
    if (type.find('\0')!=std::string::npos || !rs::util::utf8_code_point_count(type)) return false;
  return true;
}

namespace {
bool column_literal_pattern(const std::optional<std::string>& pattern) noexcept {
  if (!pattern || pattern->empty())return false;
  bool escaped=false;
  for (char ch:*pattern) {
    if (escaped){escaped=false;continue;}
    if (ch=='\\')escaped=true;else if (ch=='%' || ch=='_')return false;
  }
  return !escaped;
}
std::string column_literal_name(std::string_view pattern) {
  std::string result;bool escaped=false;
  for(char ch:pattern){if(!escaped && ch=='\\'){escaped=true;continue;}result.push_back(ch);escaped=false;}
  return result;
}
BackendError invalid_column_metadata(const char* reason) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      std::string("Invalid Redshift column metadata: ")+reason};
  error.error_class=BackendErrorClass::InvalidMetadata;error.operation=BackendOperation::ExecuteCatalog;return error;
}
}
bool redshift_column_names_are_literal(const ColumnsCatalogRequest& request) noexcept {
  return column_literal_pattern(request.schema) && column_literal_pattern(request.table);
}
bool redshift_columns_schema_wide(const ColumnsCatalogRequest& request) noexcept {
  return request.name_matches.existing() && !redshift_column_names_are_literal(request);
}
bool redshift_columns_patterns_valid(const ColumnsCatalogRequest& request) {
  return table_pattern_valid(request.schema) && table_pattern_valid(request.table) &&
      table_pattern_valid(request.column);
}
std::optional<std::string> redshift_columns_schema(const ColumnsCatalogRequest& request) {
  if (!redshift_columns_schema_wide(request) || !column_literal_pattern(request.schema) ||
      !redshift_columns_patterns_valid(request)) return std::nullopt;
  auto schema = column_literal_name(*request.schema);
  return schema_identifier(schema) ? std::optional<std::string>{std::move(schema)} : std::nullopt;
}
std::optional<std::pair<std::string,std::string>> redshift_column_names(const ColumnsCatalogRequest& request) {
  if (!redshift_column_names_are_literal(request) || !table_pattern_valid(request.column))return std::nullopt;
  if (request.catalog && (request.catalog->find('\0')!=std::string::npos ||
      !rs::util::utf8_code_point_count(*request.catalog)))return std::nullopt;
  auto schema=column_literal_name(*request.schema),table=column_literal_name(*request.table);
  if (!schema_identifier(schema) || !schema_identifier(table))return std::nullopt;
  return std::pair{std::move(schema),std::move(table)};
}
std::string redshift_show_columns_command(std::string_view database,std::string_view schema,std::string_view table) {
  const auto quote=[](std::string_view value){std::string result(1,'"');for(char ch:value){if(ch=='"')result.push_back('"');result.push_back(ch);}result.push_back('"');return result;};
  // Pinned AWS helper184: V4 per-table command, no database-wide V5 discovery.
  return "SHOW COLUMNS FROM TABLE "+quote(database)+"."+quote(schema)+"."+quote(table)+";";
}


namespace {
bool column_number(const std::optional<std::string>& value,std::optional<std::int32_t>& result) {
  result.reset();if(!value)return true;
  std::int32_t number=0;const auto [end,error]=std::from_chars(value->data(),value->data()+value->size(),number);
  if(value->empty() || error!=std::errc{} || end!=value->data()+value->size())return false;
  result=number;return true;
}
using ColumnCell=std::optional<std::string>;
struct ColumnDimensions {
  int code{0},sql_type{0};ColumnCell size,buffer,scale,radix,sub,octets;
};
// Reuse the existing parser's scalar dimensions rather than a second temporal/
// numeric size policy. The SQLColumns values follow the existing SVV projection;
// they deliberately are not the official driver's internal-storage byte counts.
std::optional<ColumnDimensions> column_dimensions(std::string_view type,
    std::optional<std::int32_t> length,std::optional<std::int32_t> precision,std::optional<std::int32_t> scale) {
  std::string normalized(type);for(char& ch:normalized)if(ch>='A' && ch<='Z')ch=static_cast<char>(ch-'A'+'a');
  // Closed server catalog labels, not an OID/result codec or DDL-size claim.
  if (normalized == "super") {
    ColumnDimensions result;
    result.code = result.sql_type = -1; // SQL_LONGVARCHAR; SUPER is unsized catalog metadata.
    return result;
  }
  if (normalized == "geometry" || normalized == "geography") {
    ColumnDimensions result;
    result.code = result.sql_type = -4; // Closed unsized spatial catalog labels, not a payload codec.
    return result;
  }
  if (normalized == "varbyte" || normalized == "varbinary" || normalized == "binary varying") {
    ColumnDimensions result;
    result.code = result.sql_type = -4; // SQL_LONGVARBINARY; dimensions remain unknown.
    return result;
  }
  std::int32_t fraction=-1;
  const auto suffix=normalized.rfind(" (");
  const auto temporal_name=std::string_view(normalized).substr(0,suffix);
  const bool temporal=temporal_name=="time without time zone" || temporal_name=="time with time zone" ||
      temporal_name=="timestamp without time zone" || temporal_name=="timestamp with time zone";
  if(suffix!=std::string::npos && temporal && normalized.back()==')') {
    const auto digits=std::string_view(normalized).substr(suffix+2,normalized.size()-suffix-3);
    const auto [end,error]=std::from_chars(digits.data(),digits.data()+digits.size(),fraction);
    if(digits.empty() || error!=std::errc{} || end!=digits.data()+digits.size() || fraction<0 || fraction>6)return std::nullopt;
    normalized.resize(suffix);
  }
  std::uint32_t oid=0;std::int32_t modifier=-1;int code=0,bytes=0;
  if(normalized=="boolean"){oid=16;code=-7;bytes=1;}
  else if(normalized=="smallint"){oid=21;code=5;bytes=2;}
  else if(normalized=="integer"){oid=23;code=4;bytes=4;}
  else if(normalized=="bigint"){oid=20;code=-5;bytes=8;}
  else if(normalized=="real"){oid=700;code=7;bytes=4;}
  else if(normalized=="double precision"){oid=701;code=8;bytes=8;}
  else if(normalized=="numeric" || normalized=="decimal") {
    if(!precision || !scale || *precision<1 || *precision>38 || *scale<0 || *scale>*precision)return std::nullopt;
    oid=1700;code=normalized=="decimal"?3:2;modifier=4+(*precision<<16)+*scale;bytes=*precision+2;
  } else if(normalized=="character" || normalized=="character varying") {
    if(!length || *length<1 || *length>65535)return std::nullopt;
    oid=normalized=="character"?1042:1043;code=normalized=="character"?1:12;modifier=*length+4;bytes=*length;
  } else if(normalized=="date"){oid=1082;code=91;bytes=6;}
  else if(normalized=="time without time zone"){oid=1083;code=92;bytes=6;modifier=fraction;}
  else if(normalized=="time with time zone"){oid=1266;code=92;bytes=6;modifier=fraction;}
  else if(normalized=="timestamp without time zone"){oid=1114;code=93;bytes=16;modifier=fraction;}
  else if(normalized=="timestamp with time zone"){oid=1184;code=93;bytes=16;modifier=fraction;}
  ColumnDimensions result;
  if(!oid)return result; // Unsupported families retain TYPE_NAME, unknown code/NULL dimensions.
  if(fraction!=-1 && code!=92 && code!=93)return std::nullopt;
  const auto native=PgProtocolParser{}.describe_type(oid,-1,modifier);
  result.code=code;result.sql_type=code>=91 && code<=93?9:code;
  result.size=std::to_string(native.column_size);result.buffer=std::to_string(bytes);
  if(code==2 || code==3 || code==92 || code==93)result.scale=std::to_string(native.decimal_digits);
  else if(code==5 || code==4 || code==-5)result.scale="0";
  if(code==2 || code==3 || code==5 || code==4 || code==-5)result.radix="10";
  else if(code==7 || code==8)result.radix="2";
  if(code>=91 && code<=93)result.sub=std::to_string(code-90);
  if(code==1 || code==12)result.octets=std::to_string(*length);
  return result;
}
}

BackendResult<QueryResult> normalize_redshift_columns(std::string_view database,std::string_view schema,
    std::string_view table,const ColumnsCatalogRequest& request,BackendResult<QueryResult> input) {
  if (!input) return input.backend_error();
  const auto& source=*input;
  if (source.error) return *source.error;
  for (const auto& extra:source.additional_results)
    if (extra.error) return *extra.error;
  if(!schema_identifier(database) || !schema_identifier(schema) || !schema_identifier(table) || !redshift_column_names(request))return invalid_column_metadata("identity");
  if(!clean_schema_result(source))return invalid_column_metadata("structure");
  if(source.rows.size()>10000)return invalid_column_metadata("rows");
  if(!source.statement_kind || (*source.statement_kind!=StatementKind::Unknown && *source.statement_kind!=StatementKind::SelectCursor) ||
      (source.affected_rows!=0 && source.affected_rows!=source.rows.size()))return invalid_column_metadata("completion");
  if(input.session_snapshot().disposition==SessionDisposition::Retire)return invalid_column_metadata("snapshot");
  // The pinned helper and primary SHOW COLUMNS example identify these twelve
  // semantic fields and five optional physical attributes; no version/count guess.
  constexpr std::array<std::string_view,17> names{"database_name","schema_name","table_name","column_name",
      "ordinal_position","column_default","is_nullable","data_type","character_maximum_length",
      "numeric_precision","numeric_scale","remarks","sort_key_type","sort_key","dist_key","encoding","collation"};
  if(source.columns.size()<12 || source.columns.size()>names.size())return invalid_column_metadata("columns");
  std::array<std::size_t,12> index{};std::set<std::string_view> headers;
  for(std::size_t i=0;i<source.columns.size();++i) {
    const auto name=std::string_view(source.columns[i].name);const auto found=std::find(names.begin(),names.end(),name);
    if(found==names.end() || !headers.insert(name).second)return invalid_column_metadata("layout");
    const auto n=static_cast<std::size_t>(found-names.begin());if(n<index.size())index[n]=i;
  }
  for(std::size_t i=0;i<index.size();++i)if(!headers.contains(names[i]))return invalid_column_metadata("layout");
  for(const auto i:{0u,1u,2u,3u,5u,6u,7u,11u})if(!schema_text(source.columns[index[i]]))return invalid_column_metadata("type");
  for(const auto i:{4u,8u,9u,10u}) {
    const auto& native=source.columns[index[i]].normalized_type;
    if(!native || !native->known || (native->type!=ScalarType::SmallInt && native->type!=ScalarType::Integer && native->type!=ScalarType::BigInt))return invalid_column_metadata("type");
  }
  QueryResult output;
  constexpr std::array<const char*,18> outnames{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","COLUMN_NAME","DATA_TYPE","TYPE_NAME",
      "COLUMN_SIZE","BUFFER_LENGTH","DECIMAL_DIGITS","NUM_PREC_RADIX","NULLABLE","REMARKS","COLUMN_DEF","SQL_DATA_TYPE",
      "SQL_DATETIME_SUB","CHAR_OCTET_LENGTH","ORDINAL_POSITION","IS_NULLABLE"};
  const std::array<int,18> textsource{0,1,2,3,-1,7,-1,-1,-1,-1,-1,11,5,-1,-1,-1,-1,6};
  for(std::size_t i=0;i<outnames.size();++i) {
    NativeTypeInfo native;
    if(textsource[i]>=0){native=declared_character_metadata(*source.columns[index[static_cast<std::size_t>(textsource[i])]].normalized_type);native.type=ScalarType::VarChar;}
    else {const bool integer=i==6 || i==7 || i==15 || i==16;native={integer?ScalarType::Integer:ScalarType::SmallInt,integer?10u:5u,0,true};}
    output.columns.push_back({outnames[i],native});
  }
  std::set<std::string> identities;std::set<std::int32_t> ordinals;
  std::vector<std::pair<std::int32_t,ResultRow>> ordered;
  // Validation precedes filtering; foreign or malformed hidden rows cannot become
  // empty success. Preserve defaults/remarks as owning nullable cells, never logs.
  for(const auto& row:source.rows) {
    for(const auto i:{0u,1u,2u,3u,7u})if(!row[index[i]] || !schema_identifier(*row[index[i]]))return invalid_column_metadata("row-identity");
    if(*row[index[0]]!=database || *row[index[1]]!=schema || *row[index[2]]!=table)return invalid_column_metadata("foreign-identity");
    for(const auto i:{5u,6u,11u})if(row[index[i]] && (row[index[i]]->find('\0')!=std::string::npos || !rs::util::utf8_code_point_count(*row[index[i]])))return invalid_column_metadata("row-text");
    if(!identities.insert(*row[index[3]]).second)return invalid_column_metadata("duplicate");
    std::optional<std::int32_t> ordinal,length,precision,scale;
    if(!column_number(row[index[4]],ordinal) || !ordinal || *ordinal<1 || !ordinals.insert(*ordinal).second ||
       !column_number(row[index[8]],length) || !column_number(row[index[9]],precision) || !column_number(row[index[10]],scale) ||
       (length && *length<0) || (precision && *precision<0) || (scale && *scale<0))return invalid_column_metadata("dimensions");
    const auto dims=column_dimensions(*row[index[7]],length,precision,scale);if(!dims)return invalid_column_metadata("dimensions");
    const auto& nullable=row[index[6]];
    if(nullable && !nullable->empty() && *nullable!="YES" && *nullable!="NO")return invalid_column_metadata("nullable");
    if((request.catalog && *request.catalog!=database) || !table_like(*row[index[3]],request.column))continue;
    ordered.emplace_back(*ordinal,ResultRow{row[index[0]],row[index[1]],row[index[2]],row[index[3]],std::to_string(dims->code),
        row[index[7]],dims->size,dims->buffer,dims->scale,dims->radix,nullable && *nullable=="YES"?"1":nullable && *nullable=="NO"?"0":"2",
        row[index[11]],row[index[5]],std::to_string(dims->sql_type),dims->sub,dims->octets,std::to_string(*ordinal),nullable.value_or("")});
  }
  std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.first<b.first;});
  for(auto& value:ordered)output.rows.push_back(std::move(value.second));
  return BackendResult<QueryResult>{std::move(output),input.session_snapshot()};
}


namespace {
BackendError routine_timeout() {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::Timeout),"Routine metadata normalization deadline expired"};
  error.operation=BackendOperation::ExecuteCatalog;return error;
}
BackendError invalid_routine_metadata(const char* reason) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      std::string("Invalid Redshift routine metadata: ") + reason};
  error.error_class=BackendErrorClass::InvalidMetadata;
  error.operation=BackendOperation::ExecuteCatalog;
  return error;
}
std::string routine_quote(std::string_view value) {
  std::string output{"\""};
  for (const char ch:value) { if(ch=='"') output.push_back('"'); output.push_back(ch); }
  output.push_back('"'); return output;
}
bool routine_match(std::string_view value,const std::optional<std::string>& pattern,CatalogNameMatch match) {
  return match==CatalogNameMatch::Existing ? table_like(value,pattern) : pattern && value==*pattern;
}
bool routine_number(const ResultCell& value,std::optional<std::int32_t>& number) {
  return column_number(value,number);
}
bool routine_shape(const QueryResult& source,const SessionSnapshot& snapshot) {
  return clean_schema_result(source) && !source.error && source.rows.size()<=10000 &&
      source.statement_kind && (*source.statement_kind==StatementKind::Unknown ||
      *source.statement_kind==StatementKind::SelectCursor) &&
      (source.affected_rows==0 || source.affected_rows==source.rows.size()) &&
      snapshot.state==SessionState::Idle && snapshot.disposition==SessionDisposition::Reusable;
}
bool routine_integer(const ResultColumnMetadata& column) {
  return column.normalized_type && column.normalized_type->known &&
      (column.normalized_type->type==ScalarType::SmallInt || column.normalized_type->type==ScalarType::Integer ||
       column.normalized_type->type==ScalarType::BigInt);
}
std::optional<std::vector<std::string>> routine_arguments(std::string_view input) {
  // Fixed native spellings only; no type modifiers, identifiers or SQL fragments.
  constexpr std::string_view allowed[]{"smallint","int2","integer","int","int4","bigint","int8",
      "decimal","numeric","real","float4","double precision","float8","float","char","\"char\"",
      "character","nchar","bpchar","varchar","character varying","nvarchar","text","date","time",
      "time without time zone","timetz","time with time zone","timestamp","timestamp without time zone",
      "timestamptz","timestamp with time zone","interval","intervaly2m","interval year to month",
      "intervald2s","interval day to second","boolean","bool","hllsketch","super","varbyte","varbinary",
      "binary varying","geometry","geography","refcursor"};
  std::vector<std::string> output;
  if(input.empty()) return output;
  std::size_t start=0;
  while(start<input.size()) {
    const auto comma=input.find(',',start);
    auto token=input.substr(start,comma==std::string_view::npos?input.size()-start:comma-start);
    while(!token.empty() && token.front()==' ') token.remove_prefix(1);
    while(!token.empty() && token.back()==' ') token.remove_suffix(1);
    if(token.empty() || token.size()>64) return std::nullopt;
    std::string canonical;
    bool space=false;
    for(char ch:token) {
      if(ch==' ') {space=true;continue;}
      if(space && !canonical.empty()) canonical.push_back(' ');
      space=false;
      if(ch>='A' && ch<='Z') ch+= 'a'-'A';
      canonical.push_back(ch);
    }
    if(std::find(std::begin(allowed),std::end(allowed),canonical)==std::end(allowed))return std::nullopt;
    output.push_back(std::move(canonical));
    if(comma==std::string_view::npos) break;
    start=comma+1;
    if(start==input.size()) return std::nullopt;
  }
  return output;
}
std::size_t routine_cell_bytes(const ResultRow& row) {
  std::size_t bytes=0;
  for(const auto& cell:row) if(cell) {
    if(cell->size()>std::numeric_limits<std::size_t>::max()-bytes) return std::numeric_limits<std::size_t>::max();
    bytes+=cell->size();
  }
  return bytes;
}
bool routine_headers(QueryResult& output,bool columns,RedshiftRoutineBudget& budget) {
  constexpr std::string_view procedures[]{"PROCEDURE_CAT","PROCEDURE_SCHEM","PROCEDURE_NAME",
      "NUM_INPUT_PARAMS","NUM_OUTPUT_PARAMS","NUM_RESULT_SETS","REMARKS","PROCEDURE_TYPE"};
  constexpr std::string_view parameters[]{"PROCEDURE_CAT","PROCEDURE_SCHEM","PROCEDURE_NAME","COLUMN_NAME",
      "COLUMN_TYPE","DATA_TYPE","TYPE_NAME","COLUMN_SIZE","BUFFER_LENGTH","DECIMAL_DIGITS","NUM_PREC_RADIX",
      "NULLABLE","REMARKS","COLUMN_DEF","SQL_DATA_TYPE","SQL_DATETIME_SUB","CHAR_OCTET_LENGTH","ORDINAL_POSITION","IS_NULLABLE"};
  const std::span<const std::string_view> names=columns ? std::span<const std::string_view>{parameters} :
      std::span<const std::string_view>{procedures};
  if(!budget.description(names.size()))return false;
  for(std::size_t i=0;i<names.size();++i) {
    if(!budget.name(names[i]) || !budget.retain({0,0,names[i].size(),1,names[i].size()}))return false;
    const bool text=columns ? (i<4 || i==6 || i==12 || i==13 || i==18) : (i<3 || i==6);
    const bool integer=columns && (i==7 || i==8 || i==16 || i==17);
    output.columns.push_back({std::string(names[i]),NativeTypeInfo{text?ScalarType::VarChar:
        integer?ScalarType::Integer:ScalarType::SmallInt,text?0u:integer?10u:5u,0,true}});
  }
  return true;
}
void routine_text_capacities(QueryResult& output) {
  for(std::size_t c=0;c<output.columns.size();++c) {
    auto& type=output.columns[c].normalized_type;
    if(!type || type->type!=ScalarType::VarChar)continue;
    for(const auto& row:output.rows) if(row[c]) {
      const auto count=rs::util::utf8_code_point_count(*row[c]);
      if(!count)throw std::runtime_error("Invalid Redshift routine output encoding");
      type->column_size=std::max(type->column_size,static_cast<std::uint64_t>(*count));
    }
  }
}
std::optional<ColumnDimensions> routine_dimensions(std::string_view type,std::optional<std::int32_t> length,
    std::optional<std::int32_t> precision,std::optional<std::int32_t> scale) {
  std::string lower{type};
  for(auto& ch:lower)if(ch>='A' && ch<='Z')ch+='a'-'A';
  if(lower=="numeric" || lower=="decimal") {
    if((precision && (*precision<1 || *precision>38)) || (scale && (*scale<0 || *scale>38)) ||
        (precision && scale && *scale>*precision))return std::nullopt;
    ColumnDimensions d;d.code=lower=="numeric"?2:3;d.sql_type=d.code;d.radix="10";
    if(precision){d.size=std::to_string(*precision);d.buffer=std::to_string(*precision+2);}
    if(scale)d.scale=std::to_string(*scale);
    return d;
  }
  if(lower=="character" || lower=="character varying") {
    if(length && (*length==0 || *length<-1 || *length>65535))return std::nullopt;
    ColumnDimensions d;d.code=lower=="character"?1:12;d.sql_type=d.code;
    if(length && *length>0){d.size=d.buffer=d.octets=std::to_string(*length);}
    return d;
  }
  return column_dimensions(type,length,precision,scale);
}
} // namespace

bool RedshiftRoutineBudget::retain(RedshiftRoutineUsage extra) noexcept {
  const auto fits=[](std::size_t used,std::size_t add,std::size_t limit){return used<=limit && add<=limit-used;};
  if(!fits(used_.rows,extra.rows,result_.max_rows) || !fits(used_.cells,extra.cells,result_.max_cells) ||
      !fits(used_.bytes,extra.bytes,response_.max_wire_bytes) || !fits(used_.metadata,extra.metadata,result_.max_metadata_entries) ||
      !fits(used_.name_bytes,extra.name_bytes,result_.max_metadata_name_bytes))return false;
  used_.rows+=extra.rows;used_.cells+=extra.cells;used_.bytes+=extra.bytes;used_.metadata+=extra.metadata;used_.name_bytes+=extra.name_bytes;
  return true;
}
void RedshiftRoutineBudget::release(RedshiftRoutineUsage used) noexcept {
  used_.rows-=used.rows;used_.cells-=used.cells;used_.bytes-=used.bytes;used_.metadata-=used.metadata;used_.name_bytes-=used.name_bytes;
}
bool RedshiftRoutineBudget::exchanges_available(std::size_t count) const noexcept {
  return exchanges_<=result_.max_results && count<=result_.max_results-exchanges_;
}
bool RedshiftRoutineBudget::exchange() noexcept {
  if(!exchanges_available(1))return false;
  ++exchanges_;return true;
}
bool RedshiftRoutineBudget::name(std::string_view value) const noexcept {
  return value.size()<=result_.max_column_name_bytes;
}
bool RedshiftRoutineBudget::description(std::size_t count) const noexcept {
  return count<=result_.max_columns_per_description;
}
bool RedshiftRoutineBudget::arguments(std::size_t count) const noexcept {
  return count<=result_.max_metadata_entries;
}
RedshiftRoutineUsage redshift_routine_usage(const QueryResult& result) {
  RedshiftRoutineUsage used{result.rows.size(),0,0,result.columns.size()};
  const auto add=[](std::size_t& total,std::size_t n) {
    total=n>std::numeric_limits<std::size_t>::max()-total?std::numeric_limits<std::size_t>::max():total+n;
  };
  for(const auto& col:result.columns){add(used.bytes,col.name.size());add(used.name_bytes,col.name.size());}
  for(const auto& row:result.rows){add(used.cells,row.size());add(used.bytes,routine_cell_bytes(row));}
  return used;
}
RedshiftRoutineUsage redshift_routine_storage_usage(const std::vector<RedshiftRoutine>& routines) {
  RedshiftRoutineUsage used{};
  const auto add=[](std::size_t& total,std::size_t n) {
    total=n>std::numeric_limits<std::size_t>::max()-total?std::numeric_limits<std::size_t>::max():total+n;
  };
  for(const auto& routine:routines) {
    add(used.rows,1);add(used.cells,7);add(used.cells,routine.arguments.size());
    add(used.bytes,routine.name.size());add(used.name_bytes,routine.name.size());
    add(used.bytes,routine.signature.size());add(used.name_bytes,routine.signature.size());
    for(const auto& argument:routine.arguments) {
      add(used.bytes,argument.size());add(used.name_bytes,argument.size());
    }
    if(routine.return_type)add(used.bytes,routine.return_type->size());
    if(routine.remarks)add(used.bytes,routine.remarks->size());
  }
  return used;
}
BackendError redshift_routine_limit() {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit),"Redshift routine catalog budget exceeded"};
  error.operation=BackendOperation::ExecuteCatalog;
  // Used only in a passively verified Idle phase or after complete reusable
  // native results. Framing/timeouts keep their original terminal outcome.
  error.session_state=SessionState::Idle;error.disposition=SessionDisposition::Reusable;
  return error;
}
std::optional<std::string> redshift_routine_schema(const CatalogRequest& request) {
  return std::visit([](const auto& names)->std::optional<std::string> {
    using T=std::decay_t<decltype(names)>;
    if constexpr(std::is_same_v<T,ProceduresCatalogRequest> || std::is_same_v<T,ProcedureColumnsCatalogRequest>) {
      if(!names.schema)return std::nullopt;
      if(names.name_matches.schema!=CatalogNameMatch::Existing)return names.schema;
      std::string decoded;
      for(std::size_t i=0;i<names.schema->size();++i) {
        char ch=(*names.schema)[i];
        if(ch=='\\') {
          if(i+1==names.schema->size())return std::nullopt;
          const char next=(*names.schema)[i+1];
          // ODBC escapes quote %, _ and the escape itself. Before an
          // ordinary character the backslash remains a literal character.
          if(next=='%' || next=='_' || next=='\\')ch=(*names.schema)[++i];
        } else if(ch=='%' || ch=='_')return std::nullopt;
        decoded.push_back(ch);
      }
      return decoded;
    }
    return std::nullopt;
  },request);
}

RedshiftRoutineUsage redshift_routine_schema_usage(const QueryResult& source) {
  auto usage=redshift_routine_usage(source);
  const auto add_objects=[&](std::size_t count,std::size_t size) {
    const auto max=std::numeric_limits<std::size_t>::max();
    if(count>max/size || count*size>max-usage.bytes)usage.bytes=max;
    else usage.bytes+=count*size;
  };
  // Conservative backend admission model, not a measured allocator/RSS cap.
  // Native wire limits remain unchanged; simultaneous container storage is
  // additionally admitted before allocating a second owning identity vector.
  add_objects(source.columns.capacity(),sizeof(ResultColumnMetadata));
  add_objects(source.rows.capacity(),sizeof(ResultRow));
  for(const auto& row:source.rows)add_objects(row.capacity(),sizeof(ResultCell));
  return usage;
}

namespace {
BackendError invalid_wide_columns(const char* reason, SessionSnapshot snapshot = {}) {
  auto error = invalid_column_metadata(reason);
  error.session_state = snapshot.state;
  error.disposition = SessionDisposition::Retire;
  return error;
}
constexpr std::array<std::string_view,18> wide_column_headers{
    "TABLE_CAT","TABLE_SCHEM","TABLE_NAME","COLUMN_NAME","DATA_TYPE","TYPE_NAME",
    "COLUMN_SIZE","BUFFER_LENGTH","DECIMAL_DIGITS","NUM_PREC_RADIX","NULLABLE","REMARKS",
    "COLUMN_DEF","SQL_DATA_TYPE","SQL_DATETIME_SUB","CHAR_OCTET_LENGTH","ORDINAL_POSITION","IS_NULLABLE"};
// Component equation for the closed native SHOW admission model; no generic
// allocator, accounting framework, or provider-neutral API is introduced.
bool wide_storage_component(RedshiftRoutineUsage& model, std::size_t count,
    std::size_t size = 1, bool name = false) {
  auto& total = name ? model.name_bytes : model.bytes;
  const auto max = std::numeric_limits<std::size_t>::max();
  if (count > max / size || count * size > max - total) return false;
  total += count * size; return true;
}
std::string wide_literal(std::string_view identifier) {
  std::string out;
  out.reserve(identifier.size()*2);
  for (char ch : identifier) { if (ch == '\\' || ch == '%' || ch == '_') out.push_back('\\'); out.push_back(ch); }
  return out;
}
}
BackendResult<RedshiftColumnSchemas> redshift_column_schemas(std::string_view database,
    const std::optional<std::string>& schema_pattern,
    const BackendResult<QueryResult>& input,
    RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra:input->additional_results) if (extra.error) return *extra.error;
  const auto bad=[&](const char* why) {return invalid_wide_columns(why,input.session_snapshot());};
  if (!routine_shape(*input,input.session_snapshot()) || !schema_identifier(database))
    return bad("schema-completion");
  const auto layout=show_schema_indexes(*input);
  if (!layout) return bad("schema-layout");
  const auto& index=*layout;
  if (!schema_text(input->columns[index[0]]) || !schema_text(input->columns[index[1]]))
    return bad("schema-type");
  // Validate every returned identity before filtering or admitting a second
  // allocation. The native10000 row ceiling and original deadline bound this
  // borrowed duplicate check; no hidden malformed row becomes empty success.
  for (std::size_t r=0;r<input->rows.size();++r) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    const auto& row=input->rows[r];
    if (!row[index[0]] || !row[index[1]] || *row[index[0]]!=database ||
        !schema_identifier(*row[index[1]])) return bad("schema-identity");
    for (std::size_t previous=0;previous<r;++previous) {
      if (rs::util::Clock::now()>=deadline) return routine_timeout();
      if (input->rows[previous][index[1]]==row[index[1]]) return bad("schema-duplicate");
    }
  }
  if (!budget.description(input->columns.size())) return redshift_routine_limit();
  auto raw=redshift_routine_schema_usage(*input);
  for (const auto& column:input->columns) if (!budget.name(column.name)) return redshift_routine_limit();
  for (const auto& row:input->rows) {
    for (const auto n:{0u,1u}) {
      if (!budget.name(*row[index[n]]) ||
          !wide_storage_component(raw,row[index[n]]->size(),1,true)) return redshift_routine_limit();
    }
  }
  RedshiftRoutineUsage scratch{};
  if (!wide_storage_component(scratch,input->rows.size(),sizeof(std::string_view)))
    return redshift_routine_limit();
  scratch.metadata=input->rows.size();
  if (!budget.retain(raw) || !budget.retain(scratch)) return redshift_routine_limit();
  RedshiftColumnSchemas selected;selected.raw_retained=raw;
  {
    std::vector<std::string_view> identities;
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    identities.reserve(input->rows.size());
    for (const auto& row:input->rows) identities.emplace_back(*row[index[1]]);
    std::sort(identities.begin(),identities.end());
    std::size_t count=0,payload=0;
    for (const auto identity:identities) {
      if (rs::util::Clock::now()>=deadline) return routine_timeout();
      if (!table_like(identity,schema_pattern)) continue;
      if (!wide_storage_component(selected.retained,identity.size()) ||
          !wide_storage_component(selected.retained,identity.size(),1,true))
        return redshift_routine_limit();
      ++count;
    }
    payload=selected.retained.bytes;
    if (!wide_storage_component(selected.retained,count,sizeof(std::string)+1))
      return redshift_routine_limit();
    selected.retained.rows=selected.retained.cells=selected.retained.metadata=count;
    selected.retained.name_bytes=payload;
    if (!budget.retain(selected.retained)) return redshift_routine_limit();
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    selected.names.reserve(count);
    for (const auto identity:identities) {
      if (rs::util::Clock::now()>=deadline) return routine_timeout();
      if (table_like(identity,schema_pattern)) selected.names.emplace_back(identity);
    }
  } // Borrowed index destroyed; caller owns raw and selected name lifetimes.
  budget.release(scratch);
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  return BackendResult<RedshiftColumnSchemas>{std::move(selected),input.session_snapshot()};
}

BackendResult<RedshiftColumnTables> redshift_column_tables(std::string_view database,
    std::string_view schema, const std::optional<std::string>& table_pattern,
    const BackendResult<QueryResult>& input,
    RedshiftRoutineBudget& budget, rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra : input->additional_results) if (extra.error) return *extra.error;
  const auto bad = [&](const char* why) { return invalid_wide_columns(why, input.session_snapshot()); };
  if (!routine_shape(*input, input.session_snapshot()) || !schema_identifier(database) || !schema_identifier(schema))
    return bad("table-completion");
  constexpr std::array<std::string_view,11> fields{"database_name","schema_name","table_name","table_type",
      "remarks","table_acl","owner","last_altered_time","last_modified_time","dist_style","table_subtype"};
  std::array<std::size_t,6> index{}; std::array<bool,11> seen{};
  if (input->columns.size() < index.size() || input->columns.size() > fields.size()) return bad("table-layout");
  for (std::size_t c=0;c<input->columns.size();++c) {
    const auto field=std::find(fields.begin(),fields.end(),input->columns[c].name);
    if (field==fields.end()) return bad("table-layout");
    const auto n=static_cast<std::size_t>(field-fields.begin());
    if (seen[n]) return bad("table-layout");
    seen[n]=true; if (n<index.size()) index[n]=c;
  }
  for (std::size_t n=0;n<index.size();++n)
    if (!seen[n] || !schema_text(input->columns[index[n]])) return bad("table-type");
  // No allocations or budget classification until every identity is validated.
  // The closed native row bound makes the borrowed duplicate check finite.
  const auto types=catalog_table_types();
  for (std::size_t r=0;r<input->rows.size();++r) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    const auto& row=input->rows[r];
    for (const auto n:{0u,1u,2u,3u})
      if (!row[index[n]] || !schema_identifier(*row[index[n]])) return bad("table-identity");
    if (*row[index[0]]!=database || *row[index[1]]!=schema) return bad("table-foreign");
    if (std::find(types.begin(),types.end(),*row[index[3]])==types.end()) return bad("table-kind");
    for (const auto n:{4u,5u})
      if (row[index[n]] && (row[index[n]]->find('\0')!=std::string::npos ||
          !rs::util::utf8_code_point_count(*row[index[n]]))) return bad("table-text");
    for (std::size_t prior=0;prior<r;++prior) {
      if (rs::util::Clock::now()>=deadline) return routine_timeout();
      if (input->rows[prior][index[2]]==row[index[2]]) return bad("table-duplicate");
    }
  }
  // Match only after ALL native identities/completion have been validated.
  // Reuse the existing backend UTF8 LIKE contract unchanged. The raw response
  // remains live and charged, even for identities that are not selected.
  RedshiftRoutineUsage payload;
  std::size_t count=0;
  for (const auto& row:input->rows) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    if (!table_like(*row[index[2]],table_pattern)) continue;
    ++count;
    if (!wide_storage_component(payload,row[index[2]]->size())) return redshift_routine_limit();
  }
  // Admission covers the simultaneous raw native response and owning selection.
  RedshiftColumnTables selected;
  selected.raw_retained=redshift_routine_schema_usage(*input);
  selected.retained={count,count,payload.bytes,count,payload.bytes};
  if (!wide_storage_component(selected.retained,count,sizeof(std::string)+1) ||
      !budget.description(input->columns.size())) return redshift_routine_limit();
  for (const auto& column:input->columns) if (!budget.name(column.name)) return redshift_routine_limit();
  for (const auto& row:input->rows) if (!budget.name(*row[index[2]])) return redshift_routine_limit();
  if (!budget.retain(selected.raw_retained) || !budget.retain(selected.retained)) return redshift_routine_limit();
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  selected.names.reserve(count);
  for (const auto& row:input->rows) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    if (table_like(*row[index[2]],table_pattern)) selected.names.push_back(*row[index[2]]);
  }
  std::sort(selected.names.begin(),selected.names.end());
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  return BackendResult<RedshiftColumnTables>{std::move(selected),input.session_snapshot()};
}
RedshiftRoutineUsage redshift_columns_output_usage(const QueryResult& result) {
  auto usage=redshift_routine_schema_usage(result);
  for (const auto& row:result.rows) for (const auto n:{0u,1u,2u,3u})
    if (row[n] && !wide_storage_component(usage,row[n]->size(),1,true)) usage.name_bytes=std::numeric_limits<std::size_t>::max();
  return usage;
}
BackendResult<QueryResult> redshift_columns_headers(RedshiftRoutineBudget& budget) {
  RedshiftRoutineUsage charge{0,0,18*sizeof(ResultColumnMetadata),18,0};
  if (!budget.description(18)) return redshift_routine_limit();
  for (const auto name:wide_column_headers) {
    if (!budget.name(name) || !wide_storage_component(charge,name.size()) || !wide_storage_component(charge,name.size(),1,true))
      return redshift_routine_limit();
  }
  if (!budget.retain(charge)) return redshift_routine_limit();
  QueryResult output; output.columns.reserve(18);
  for (std::size_t i=0;i<18;++i) {
    const bool text=i<4 || i==5 || i==11 || i==12 || i==17;
    const bool integer=i==6 || i==7 || i==15 || i==16;
    output.columns.push_back({std::string(wide_column_headers[i]),NativeTypeInfo{text?ScalarType::VarChar:
        integer?ScalarType::Integer:ScalarType::SmallInt,text?0u:integer?10u:5u,0,true}});
  }
  return BackendResult<QueryResult>{std::move(output),{SessionState::Idle,SessionDisposition::Reusable}};
}
BackendResult<QueryResult> redshift_columns_part(std::string_view database,std::string_view schema,
    std::string_view table,const ColumnsCatalogRequest& request,BackendResult<QueryResult> input,
    RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra:input->additional_results) if (extra.error) return *extra.error;
  if (!routine_shape(*input,input.session_snapshot())) return invalid_wide_columns("column-completion",input.session_snapshot());
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  const auto raw=redshift_routine_schema_usage(*input);
  // Conservative pre-allocation peak for the existing closed projection: raw,
  // two row-vector indexes, 18 owning cells/row, identity/ordinal tree nodes,
  // numeric dimension scratch, header capacity (32), and exact LIKE carriers.
  // This is a backend storage admission model, not an allocator/RSS measurement.
  RedshiftRoutineUsage peak{input->rows.size(),input->rows.size()*18,0,input->rows.size()*2+input->columns.size()+18,0};
  if (!wide_storage_component(peak,raw.bytes,4) ||
      !wide_storage_component(peak,input->rows.size(),18*sizeof(ResultCell)+4*sizeof(ResultRow)+256) ||
      !wide_storage_component(peak,32,sizeof(ResultColumnMetadata)) || !wide_storage_component(peak,1024) ||
      !wide_storage_component(peak,database.size()+schema.size()+table.size(),8) ||
      (request.catalog && !wide_storage_component(peak,request.catalog->size(),2)) ||
      (request.column && !wide_storage_component(peak,request.column->size(),4))) return redshift_routine_limit();
  // Identifier/name admission is separate from arbitrary defaults/remarks.
  // Only the four identity fields are duplicated into output/identity indexes.
  peak.name_bytes=raw.name_bytes;
  for (std::size_t c=0;c<input->columns.size();++c) {
    const auto& name=input->columns[c].name;
    if (name!="database_name" && name!="schema_name" && name!="table_name" && name!="column_name") continue;
    for (const auto& row:input->rows)
      if (row[c] && !wide_storage_component(peak,row[c]->size(),2,true)) return redshift_routine_limit();
  }
  if (!wide_storage_component(peak,database.size()+schema.size()+table.size(),4,true) ||
      !wide_storage_component(peak,512,1,true)) return redshift_routine_limit();
  if (!budget.description(18) || !budget.retain(raw) || !budget.retain(peak)) return redshift_routine_limit();
  for (const auto& column:input->columns) if (!budget.name(column.name)) return redshift_routine_limit();
  for (std::size_t c=0;c<input->columns.size();++c) {
    const auto& name=input->columns[c].name;
    if (name!="database_name" && name!="schema_name" && name!="table_name" && name!="column_name") continue;
    for (const auto& row:input->rows) if (row[c] && !budget.name(*row[c])) return redshift_routine_limit();
  }
  BackendResult<QueryResult> part=invalid_wide_columns("projection");
  {
    ColumnsCatalogRequest exact;
    exact.catalog=request.catalog; exact.column=request.column;
    exact.schema=wide_literal(schema); exact.table=wide_literal(table);
    part=normalize_redshift_columns(database,schema,table,exact,std::move(input));
  } // Private carrier and consumed raw storage have been destroyed.
  budget.release(raw);
  if (!part) return part.backend_error();
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  const auto retained=redshift_columns_output_usage(*part);
  if (retained.rows>peak.rows || retained.cells>peak.cells || retained.bytes>peak.bytes ||
      retained.metadata>peak.metadata || retained.name_bytes>peak.name_bytes) return redshift_routine_limit();
  budget.release({peak.rows-retained.rows,peak.cells-retained.cells,peak.bytes-retained.bytes,
      peak.metadata-retained.metadata,peak.name_bytes-retained.name_bytes});
  return part;
}

BackendResult<QueryResult> redshift_special_columns_headers(
    RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  constexpr std::array<std::string_view,8> names{"SCOPE","COLUMN_NAME","DATA_TYPE","TYPE_NAME",
      "COLUMN_SIZE","BUFFER_LENGTH","DECIMAL_DIGITS","PSEUDO_COLUMN"};
  RedshiftRoutineUsage charge{0,0,8*sizeof(ResultColumnMetadata),8,0};
  if (!budget.description(names.size())) return redshift_routine_limit();
  for (const auto name:names) {
    if (!budget.name(name) || !wide_storage_component(charge,name.size()+1) ||
        !wide_storage_component(charge,name.size(),1,true)) return redshift_routine_limit();
  }
  if (!budget.retain(charge)) return redshift_routine_limit();
  QueryResult output;
  output.columns.reserve(names.size());
  for (std::size_t i=0;i<names.size();++i) {
    const bool text=i==1 || i==3;
    const bool integer=i==4 || i==5;
    output.columns.push_back({std::string(names[i]),NativeTypeInfo{text?ScalarType::VarChar:
        integer?ScalarType::Integer:ScalarType::SmallInt,text?0u:integer?10u:5u,0,true}});
  }
  output.statement_kind=StatementKind::SelectCursor;
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  return BackendResult<QueryResult>{std::move(output),{SessionState::Idle,SessionDisposition::Reusable}};
}

BackendResult<QueryResult> redshift_special_columns_result(
    const BackendResult<QueryResult>& keys,const BackendResult<QueryResult>& columns,
    bool require_non_nullable,RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if (!keys) return keys.backend_error();
  if (!columns) return columns.backend_error();
  if (keys->error) return *keys->error;
  if (columns->error) return *columns->error;
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  const auto malformed=[&] {return invalid_wide_columns("special-column-join",columns.session_snapshot());};
  // Inputs are normalized by the existing exact-table parsers, never raw SHOW
  // guesses. Validate their closed shape before borrowing any indexed cell.
  if (!clean_schema_result(*keys) || !clean_schema_result(*columns) || keys->error || columns->error ||
      keys.session_snapshot()!=SessionSnapshot{SessionState::Idle,SessionDisposition::Reusable} ||
      columns.session_snapshot()!=keys.session_snapshot() || keys->columns.size()!=6 || columns->columns.size()!=18)
    return malformed();
  constexpr std::array<std::string_view,6> key_headers{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","COLUMN_NAME","KEY_SEQ","PK_NAME"};
  constexpr std::array<std::string_view,18> column_headers{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","COLUMN_NAME",
      "DATA_TYPE","TYPE_NAME","COLUMN_SIZE","BUFFER_LENGTH","DECIMAL_DIGITS","NUM_PREC_RADIX","NULLABLE",
      "REMARKS","COLUMN_DEF","SQL_DATA_TYPE","SQL_DATETIME_SUB","CHAR_OCTET_LENGTH","ORDINAL_POSITION","IS_NULLABLE"};
  for (std::size_t i=0;i<key_headers.size();++i) {
    if (keys->columns[i].name!=key_headers[i]) return malformed();
  }
  for (std::size_t i=0;i<column_headers.size();++i) {
    if (columns->columns[i].name!=column_headers[i] || !columns->columns[i].normalized_type ||
        !columns->columns[i].normalized_type->known) return malformed();
  }
  const auto find_column=[&](const ResultRow& key)->const ResultRow* {
    const ResultRow* found=nullptr;
    for (const auto& column:columns->rows) {
      if (rs::util::Clock::now()>=deadline) return nullptr;
      if (column[3]!=key[3]) continue;
      if (found || column[0]!=key[0] || column[1]!=key[1] || column[2]!=key[2]) return nullptr;
      found=&column;
    }
    return found;
  };
  RedshiftRoutineUsage peak{};
  bool eligible=true;
  for (std::size_t i=0;i<keys->rows.size();++i) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    const auto& key=keys->rows[i];
    unsigned sequence=0;
    if (!key[3] || !key[4]) return malformed();
    const auto& text=*key[4];
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),sequence);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || sequence!=i+1) return malformed();
    const auto* column=find_column(key);
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    if (!column || !(*column)[4] || !(*column)[5] ||
        ((*column)[10]!=ResultCell{"0"} && (*column)[10]!=ResultCell{"1"} && (*column)[10]!=ResultCell{"2"}))
      return malformed();
    // Exclude the complete candidate, never only its nullable member. Unknown
    // nullability cannot satisfy SQL_NO_NULLS; SQL_NULLABLE retains it.
    if (require_non_nullable && (*column)[10]!=ResultCell{"0"}) eligible=false;
    if (!wide_storage_component(peak,1,sizeof(ResultRow)+8*sizeof(ResultCell)+64)) return redshift_routine_limit();
    for (const auto n:{3u,4u,5u,6u,7u,8u}) {
      if ((*column)[n] && !wide_storage_component(peak,(*column)[n]->size()+1,2)) return redshift_routine_limit();
    }
    if (!budget.name(*key[3]) || !wide_storage_component(peak,key[3]->size(),1,true)) return redshift_routine_limit();
  }
  if (!eligible) return redshift_special_columns_headers(budget,deadline);
  peak.rows=keys->rows.size();
  if (peak.rows>std::numeric_limits<std::size_t>::max()/8) return redshift_routine_limit();
  peak.cells=peak.rows*8;
  if (!budget.retain(peak)) return redshift_routine_limit();
  auto output=redshift_special_columns_headers(budget,deadline);
  if (!output) return output.backend_error();
  output->columns[1].normalized_type=columns->columns[3].normalized_type;
  output->columns[3].normalized_type=columns->columns[5].normalized_type;
  output->rows.reserve(keys->rows.size());
  for (const auto& key:keys->rows) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    const auto* column=find_column(key);
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    // Both borrowed inputs remain immutable throughout validation/projection.
    if (!column) return malformed();
    ResultRow row;
    row.reserve(8);
    row.emplace_back("0"); // SQL_SCOPE_CURROW, not a stronger lifetime.
    for (const auto n:{3u,4u,5u,6u,7u,8u}) row.push_back((*column)[n]);
    row.emplace_back("1"); // SQL_PC_NOT_PSEUDO: real declared-key columns.
    output->rows.push_back(std::move(row));
  }
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  return output;
}

// Borrowed database validation precedes every selection/allocation/budget classification.
BackendResult<RedshiftColumnSchemas> redshift_table_databases(bool visible,
    const BackendResult<QueryResult>& input,RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra:input->additional_results) if (extra.error) return *extra.error;
  const auto bad=[&](const char* why) {return invalid_wide_columns(why,input.session_snapshot());};
  if (!routine_shape(*input,input.session_snapshot())) return bad("table-database-completion");
  std::size_t identity=0;
  if (!visible) {
    if (input->columns.size()!=1 || input->columns[0].name!="database_name" ||
        !schema_text(input->columns[0]) || input->rows.size()!=1 ||
        input->statement_kind!=StatementKind::SelectCursor) return bad("table-database-lookup");
  } else {
    constexpr std::array<std::string_view,6> fields{"database_name","database_owner","database_type",
        "database_acl","parameters","database_isolation_level"};
    if (input->columns.size()!=fields.size() && input->columns.size()!=fields.size()+1)
      return bad("table-database-layout");
    for (const auto field:fields) {
      unsigned count=0;
      for (std::size_t c=0;c<input->columns.size();++c) if (input->columns[c].name==field) {
        ++count;if (field=="database_name") identity=c;
      }
      if (count!=1) return bad("table-database-layout");
    }
    // Keep the same exact optional named layout as the owning names projection.
    // Its value/type is unused; the raw response still receives its full charge.
    if (input->columns.size()==fields.size()+1 &&
        std::count_if(input->columns.begin(),input->columns.end(),[](const auto& column) {
          return column.name=="allow_connections";
        })!=1)
      return bad("table-database-layout");
    if (!schema_text(input->columns[identity])) return bad("table-database-type");
  }
  std::size_t bytes=0;
  for (std::size_t r=0;r<input->rows.size();++r) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    const auto& cell=input->rows[r][identity];
    if (!cell || !schema_identifier(*cell)) return bad("table-database-identity");
    for (std::size_t prior=0;prior<r;++prior) {
      if (rs::util::Clock::now()>=deadline) return routine_timeout();
      if (input->rows[prior][identity]==cell) return bad("table-database-duplicate");
    }
    if (cell->size()>std::numeric_limits<std::size_t>::max()-bytes) return redshift_routine_limit();
    bytes+=cell->size();
  }
  RedshiftColumnSchemas selected;const auto count=input->rows.size();
  selected.raw_retained=redshift_routine_schema_usage(*input);
  selected.retained={count,count,bytes,count,bytes};
  if (!wide_storage_component(selected.retained,count,sizeof(std::string)+1) ||
      !budget.description(input->columns.size())) return redshift_routine_limit();
  for (const auto& column:input->columns) if (!budget.name(column.name)) return redshift_routine_limit();
  for (const auto& row:input->rows) if (!budget.name(*row[identity])) return redshift_routine_limit();
  if (!budget.retain(selected.raw_retained) || !budget.retain(selected.retained)) return redshift_routine_limit();
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  selected.names.reserve(count);
  for (const auto& row:input->rows) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    selected.names.push_back(*row[identity]);
  }
  std::sort(selected.names.begin(),selected.names.end());
  return BackendResult<RedshiftColumnSchemas>{std::move(selected),input.session_snapshot()};
}
RedshiftRoutineUsage redshift_tables_output_usage(const QueryResult& result) {
  auto usage=redshift_routine_schema_usage(result);
  for (const auto& row:result.rows) for (std::size_t n=0;n<4;++n)
    if (row[n] && !wide_storage_component(usage,row[n]->size(),1,true))
      usage.name_bytes=std::numeric_limits<std::size_t>::max();
  return usage;
}
BackendResult<QueryResult> redshift_tables_headers(RedshiftRoutineBudget& budget) {
  constexpr std::array<std::string_view,5> fields{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","TABLE_TYPE","REMARKS"};
  RedshiftRoutineUsage charge{0,0,fields.size()*sizeof(ResultColumnMetadata),fields.size(),0};
  if (!budget.description(fields.size())) return redshift_routine_limit();
  for (const auto field:fields)
    if (!budget.name(field) || !wide_storage_component(charge,field.size()) ||
        !wide_storage_component(charge,field.size(),1,true)) return redshift_routine_limit();
  if (!budget.retain(charge)) return redshift_routine_limit();
  QueryResult result;result.columns.reserve(fields.size());
  for (const auto field:fields) result.columns.push_back({std::string(field),NativeTypeInfo{ScalarType::VarChar,0,0,true}});
  return BackendResult<QueryResult>{std::move(result),{SessionState::Idle,SessionDisposition::Reusable}};
}
BackendResult<QueryResult> redshift_tables_part(std::string_view database,std::string_view schema,
    const TablesCatalogRequest& request,BackendResult<QueryResult> input,RedshiftRoutineBudget& budget,
    rs::util::Deadline deadline,bool detailed_types) {
  if (!input) return input.backend_error();
  if (input->error) return *input->error;
  for (const auto& extra:input->additional_results) if (extra.error) return *extra.error;
  const auto bad=[&](const char* why) {return invalid_wide_columns(why,input.session_snapshot());};
  if (!routine_shape(*input,input.session_snapshot()) || !schema_identifier(database) || !schema_identifier(schema))
    return bad("table-completion");
  constexpr std::array<std::string_view,11> fields{"database_name","schema_name","table_name","table_type",
      "remarks","table_acl","owner","last_altered_time","last_modified_time","dist_style","table_subtype"};
  std::array<std::size_t,5> index{};std::array<bool,11> seen{};
  if (input->columns.size()<index.size() || input->columns.size()>fields.size()) return bad("table-layout");
  for (std::size_t c=0;c<input->columns.size();++c) {
    const auto found=std::find(fields.begin(),fields.end(),input->columns[c].name);
    if (found==fields.end()) return bad("table-layout");
    const auto n=static_cast<std::size_t>(found-fields.begin());
    if (seen[n]) return bad("table-layout");
    seen[n]=true;if (n<index.size()) index[n]=c;
  }
  for (std::size_t n=0;n<index.size();++n)
    if (!seen[n] || !schema_text(input->columns[index[n]])) return bad("table-type");
  const auto types=catalog_table_types();
  // Preserve five required fields and optional-layout acceptance. Validate every
  // identity before matching or admitting duplicate sets/owning projection storage.
  for (std::size_t r=0;r<input->rows.size();++r) {
    if (rs::util::Clock::now()>=deadline) return routine_timeout();
    const auto& row=input->rows[r];
    for (std::size_t n=0;n<4;++n)
      if (!row[index[n]] || !schema_identifier(*row[index[n]])) return bad("table-identity");
    if (*row[index[0]]!=database || *row[index[1]]!=schema) return bad("table-foreign");
    if (std::find(types.begin(),types.end(),*row[index[3]])==types.end()) return bad("table-kind");
    if (row[index[4]] && (row[index[4]]->find('\0')!=std::string::npos ||
        !rs::util::utf8_code_point_count(*row[index[4]]))) return bad("table-remarks");
    for (std::size_t prior=0;prior<r;++prior) {
      if (rs::util::Clock::now()>=deadline) return routine_timeout();
      if (input->rows[prior][index[2]]==row[index[2]]) return bad("table-duplicate");
    }
  }
  const auto raw=redshift_routine_schema_usage(*input);const auto count=input->rows.size();
  // Raw descriptors are charged separately. The normalizer simultaneously owns
  // one header-index node per native column, five live output descriptors and
  // one row-identity node per row. Unconstructed descriptor capacity remains
  // covered by the eight-slot byte reservation below, not metadata entries.
  RedshiftRoutineUsage peak{count,count*5,0,count+input->columns.size()+index.size(),0};
  if (!wide_storage_component(peak,raw.bytes,3) ||
      !wide_storage_component(peak,count,5*sizeof(ResultCell)+4*sizeof(ResultRow)+256) ||
      !wide_storage_component(peak,8,sizeof(ResultColumnMetadata)) || !wide_storage_component(peak,1024))
    return redshift_routine_limit();
  peak.name_bytes=raw.name_bytes;
  if (!wide_storage_component(peak,256,1,true)) return redshift_routine_limit();
  for (const auto& column:input->columns) if (!budget.name(column.name)) return redshift_routine_limit();
  for (const auto& row:input->rows) for (std::size_t n=0;n<4;++n) {
    if (!budget.name(*row[index[n]]) || !wide_storage_component(peak,row[index[n]]->size(),2,true))
      return redshift_routine_limit();
  }
  if (!budget.description(input->columns.size()) || !budget.description(5) ||
      !budget.retain(raw) || !budget.retain(peak)) return redshift_routine_limit();
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  BackendResult<QueryResult> part=bad("table-projection");
  {
    part=normalize_redshift_tables(database,schema,request,std::move(input),detailed_types);
  } // The normalizer destroyed the consumed raw response and duplicate set.
  budget.release(raw);
  if (!part) return part.backend_error();
  if (rs::util::Clock::now()>=deadline) return routine_timeout();
  const auto retained=redshift_tables_output_usage(*part);
  if (retained.rows>peak.rows || retained.cells>peak.cells || retained.bytes>peak.bytes ||
      retained.metadata>peak.metadata || retained.name_bytes>peak.name_bytes) return redshift_routine_limit();
  budget.release({peak.rows-retained.rows,peak.cells-retained.cells,peak.bytes-retained.bytes,
      peak.metadata-retained.metadata,peak.name_bytes-retained.name_bytes});
  return part;
}

BackendResult<RedshiftRoutineSchemas> normalize_redshift_routine_schemas(
    std::string_view database,const CatalogRequest& request,
    const BackendResult<QueryResult>& input,RedshiftRoutineBudget& budget,
    rs::util::Deadline deadline) {
  if(!input)return input.backend_error();
  if(input->error)return *input->error;
  for(const auto& extra:input->additional_results)if(extra.error)return *extra.error;
  if(rs::util::Clock::now()>=deadline)return routine_timeout();
  if(!routine_shape(*input,input.session_snapshot()) || input->columns.size()!=7 ||
      !schema_identifier(database))return invalid_routine_metadata("schemas-structure");
  const auto layout=show_schema_indexes(*input);
  if(!layout)return invalid_routine_metadata("schemas-layout");
  const auto& index=*layout;
  if(!schema_text(input->columns[index[0]]) || !schema_text(input->columns[index[1]]))
    return invalid_routine_metadata("schemas-identity-type");
  const auto* names=std::get_if<ProceduresCatalogRequest>(&request);
  const auto* parameters=std::get_if<ProcedureColumnsCatalogRequest>(&request);
  if(!names && !parameters)return invalid_routine_metadata("schemas-request");
  const auto& pattern=names?names->schema:parameters->schema;
  const auto match=names?names->name_matches.schema:parameters->name_matches.schema;
  const auto max=std::numeric_limits<std::size_t>::max();
  if(input->rows.size()>max/sizeof(std::string_view))return redshift_routine_limit();
  RedshiftRoutineUsage scratch{0,0,input->rows.size()*sizeof(std::string_view),input->rows.size(),0};
  if(pattern && match==CatalogNameMatch::Existing) {
    if(pattern->size()>(max-sizeof(std::string)-1)/2)return redshift_routine_limit();
    const auto bytes=pattern->size()*2+sizeof(std::string)+1;
    if(bytes>max-scratch.bytes)return redshift_routine_limit();
    scratch.bytes+=bytes;scratch.name_bytes=pattern->size()*2;
  }
  if(rs::util::Clock::now()>=deadline)return routine_timeout();
  if(!budget.retain(scratch))return redshift_routine_limit();
  RedshiftRoutineSchemas selected;
  {
    std::vector<std::string_view> identities;
    if(rs::util::Clock::now()>=deadline)return routine_timeout();
    identities.reserve(input->rows.size());
    for(const auto& row:input->rows) {
      if(rs::util::Clock::now()>=deadline)return routine_timeout();
      if(!row[index[0]] || !row[index[1]] || *row[index[0]]!=database ||
          !schema_identifier(*row[index[1]]))return invalid_routine_metadata("schemas-identity");
      if(!budget.name(*row[index[1]]))return redshift_routine_limit();
      identities.emplace_back(*row[index[1]]);
    }
    std::sort(identities.begin(),identities.end());
    if(rs::util::Clock::now()>=deadline)return routine_timeout();
    if(std::adjacent_find(identities.begin(),identities.end())!=identities.end())
      return invalid_routine_metadata("schemas-duplicate");
    // Lower only routine-schema patterns into the inherited backend matcher.
    // Shared table/object/column matching semantics remain untouched.
    std::optional<std::string> carrier;
    if(pattern && match==CatalogNameMatch::Existing) {
      if(rs::util::Clock::now()>=deadline)return routine_timeout();
      carrier.emplace();carrier->reserve(pattern->size()*2);
      for(std::size_t i=0;i<pattern->size();++i) {
        const char ch=(*pattern)[i];
        if(ch=='\\' && i+1<pattern->size()) {
          const char next=(*pattern)[i+1];
          if(next!='%' && next!='_' && next!='\\')carrier->push_back('\\');
          else {carrier->push_back(ch);carrier->push_back(next);++i;continue;}
        }
        carrier->push_back(ch);
      }
    }
    const auto matches=[&](std::string_view value) {
      return match==CatalogNameMatch::Existing?table_like(value,carrier):pattern && value==*pattern;
    };
    std::size_t count=0,payload=0;
    for(const auto identity:identities) {
      if(rs::util::Clock::now()>=deadline)return routine_timeout();
      if(!matches(identity))continue;
      if(identity.size()>max-payload)return redshift_routine_limit();
      payload+=identity.size();++count;
    }
    if(count>max/(sizeof(std::string)+1) || count*(sizeof(std::string)+1)>max-payload)
      return redshift_routine_limit();
    selected.retained={count,count,payload+count*(sizeof(std::string)+1),count,payload};
    if(!budget.retain(selected.retained))return redshift_routine_limit();
    if(rs::util::Clock::now()>=deadline)return routine_timeout();
    selected.names.reserve(count);
    for(const auto identity:identities) {
      if(rs::util::Clock::now()>=deadline)return routine_timeout();
      if(matches(identity))selected.names.emplace_back(identity);
    }
  } // Borrowed views and matcher carrier are destroyed before releasing charges.
  budget.release(scratch);
  if(rs::util::Clock::now()>=deadline)return routine_timeout();
  return BackendResult<RedshiftRoutineSchemas>{std::move(selected),input.session_snapshot()};
}
std::string redshift_show_routines_command(std::string_view database,std::string_view schema,bool function) {
  return std::string{"SHOW "}+(function?"FUNCTIONS":"PROCEDURES")+" FROM SCHEMA "+routine_quote(database)+"."+routine_quote(schema)+";";
}
std::string redshift_show_parameters_command(std::string_view database,std::string_view schema,const RedshiftRoutine& routine) {
  // arguments/signature come only from the closed normalizer below.
  return std::string{"SHOW PARAMETERS OF "}+(routine.function?"FUNCTION ":"PROCEDURE ")+
      routine_quote(database)+"."+routine_quote(schema)+"."+routine_quote(routine.name)+"("+routine.signature+");";
}
BackendResult<std::vector<RedshiftRoutine>> normalize_redshift_routines(std::string_view database,std::string_view schema,
    bool function,const CatalogRequest& request,BackendResult<QueryResult> input,RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if(!input)return input.backend_error();
  if(input->error)return *input->error;
  if(!routine_shape(*input,input.session_snapshot()))return invalid_routine_metadata("discovery-structure");
  const auto& source=*input;
  const std::array<std::string_view,7> names{"database_name","schema_name",function?"function_name":"procedure_name",
      "number_of_arguments","argument_list","return_type","remarks"};
  if(source.columns.size()!=names.size())return invalid_routine_metadata("discovery-columns");
  std::array<std::size_t,7> index{};
  for(std::size_t n=0;n<names.size();++n) {
    auto it=std::find_if(source.columns.begin(),source.columns.end(),[&](const auto& c){return c.name==names[n];});
    if(it==source.columns.end() || std::count_if(source.columns.begin(),source.columns.end(),[&](const auto& c){return c.name==names[n];})!=1)
      return invalid_routine_metadata("discovery-layout");
    index[n]=static_cast<std::size_t>(it-source.columns.begin());
    if(n==3?!routine_integer(*it):!schema_text(*it))return invalid_routine_metadata("discovery-type");
  }
  std::vector<RedshiftRoutine> output;
  for(const auto& row:source.rows) {
    if(rs::util::Clock::now()>=deadline)return routine_timeout();
    for(const auto n:{0u,1u,2u})if(!row[index[n]] || !schema_identifier(*row[index[n]]) || !budget.name(*row[index[n]]))
      return invalid_routine_metadata("discovery-identity");
    if(*row[index[0]]!=database || *row[index[1]]!=schema)return invalid_routine_metadata("discovery-foreign");
    for(const auto n:{4u,5u,6u})if(row[index[n]] && (row[index[n]]->find('\0')!=std::string::npos ||
        !rs::util::utf8_code_point_count(*row[index[n]])))return invalid_routine_metadata("discovery-text");
    std::optional<std::int32_t> count;
    if(!routine_number(row[index[3]],count) || !count || *count<0 || !row[index[4]])return invalid_routine_metadata("discovery-count");
    if(!budget.arguments(static_cast<std::size_t>(*count)))return redshift_routine_limit();
    const auto signature_bytes=row[index[4]]->size();
    const auto separators=static_cast<std::size_t>(std::count(row[index[4]]->begin(),row[index[4]]->end(),','));
    const auto elements=signature_bytes==0?0u:separators+1;
    if(elements!=static_cast<std::size_t>(*count))return invalid_routine_metadata("signature-count");
    // Charge an upper bound before creating owning type tokens and signature.
    // Canonical spelling removes space except at most one after each comma.
    if(signature_bytes>(std::numeric_limits<std::size_t>::max()-elements)/2)return redshift_routine_limit();
    const auto scratch_bound=signature_bytes*2+elements;
    if(!budget.retain({0,0,scratch_bound,0,scratch_bound}))return redshift_routine_limit();
    auto arguments=routine_arguments(*row[index[4]]);
    if(!arguments || arguments->size()!=static_cast<std::size_t>(*count))return invalid_routine_metadata("signature");
    std::string signature;
    for(const auto& type:*arguments){if(!signature.empty())signature+=", ";signature+=type;}
    std::size_t canonical_bytes=signature.size();
    for(const auto& arg:*arguments)canonical_bytes+=arg.size();
    budget.release({0,0,scratch_bound-canonical_bytes,0,scratch_bound-canonical_bytes});
    const auto duplicate=std::find_if(output.begin(),output.end(),[&](const auto& r){return r.name==*row[index[2]] && r.signature==signature;});
    if(duplicate!=output.end()) {
      if(duplicate->return_type!=row[index[5]] || duplicate->remarks!=row[index[6]])return invalid_routine_metadata("discovery-duplicate");
      budget.release({0,0,canonical_bytes,0,canonical_bytes});
      continue;
    }
    std::size_t bytes=row[index[2]]->size();
    if(row[index[5]])bytes+=row[index[5]]->size();
    if(row[index[6]])bytes+=row[index[6]]->size();
    if(!budget.retain({1,7+arguments->size(),bytes,0,row[index[2]]->size()}))return redshift_routine_limit();
    output.push_back({*row[index[2]],function,std::move(*arguments),std::move(signature),row[index[5]],row[index[6]]});
  }
  std::erase_if(output,[&](const auto& r) {
    const bool selected=std::visit([&](const auto& n) {
      using T=std::decay_t<decltype(n)>;
      if constexpr(std::is_same_v<T,ProceduresCatalogRequest> || std::is_same_v<T,ProcedureColumnsCatalogRequest>)
        return routine_match(r.name,n.procedure,n.name_matches.object);
      return false;
    },request);
    if(!selected) {
      std::size_t bytes=r.name.size()+r.signature.size();
      for(const auto& arg:r.arguments)bytes+=arg.size();
      if(r.return_type)bytes+=r.return_type->size();
      if(r.remarks)bytes+=r.remarks->size();
      budget.release({1,7+r.arguments.size(),bytes,0,r.name.size()+r.signature.size()+[&] {std::size_t n=0;for(const auto& arg:r.arguments)n+=arg.size();return n;}()});
    }
    return !selected;
  });
  if(rs::util::Clock::now()>=deadline)return routine_timeout();
  return BackendResult<std::vector<RedshiftRoutine>>{std::move(output),input.session_snapshot()};
}
BackendResult<QueryResult> redshift_routine_result(std::string_view database,std::string_view schema,
    const std::vector<RedshiftRoutine>& routines,bool columns,RedshiftRoutineBudget& budget) {
  QueryResult output;
  if(!routine_headers(output,columns,budget))return redshift_routine_limit();
  if(!columns) {
    // Execution supplies deterministic name/kind/signature order. Never copy
    // the whole owning discovery vector just to sort a second view.
    for(const auto& r:routines) {
      const auto bytes=database.size()+schema.size()+r.name.size()+1+(r.remarks?r.remarks->size():0);
      if(!budget.retain({1,8,bytes,0,database.size()+schema.size()+r.name.size()}))return redshift_routine_limit();
      output.rows.push_back({std::string(database),std::string(schema),r.name,
          std::nullopt,std::nullopt,std::nullopt,r.remarks,r.function?"2":"1"});
    }
  }
  routine_text_capacities(output);
  return BackendResult<QueryResult>{std::move(output),{SessionState::Idle,SessionDisposition::Reusable}};
}
BackendResult<QueryResult> normalize_redshift_parameters(std::string_view database,std::string_view schema,
    const RedshiftRoutine& routine,const ProcedureColumnsCatalogRequest& request,
    BackendResult<QueryResult> input,RedshiftRoutineBudget& budget,rs::util::Deadline deadline) {
  if(!input)return input.backend_error();
  if(input->error)return *input->error;
  if(!routine_shape(*input,input.session_snapshot()))return invalid_routine_metadata("parameter-structure");
  const auto& source=*input;
  const std::array<std::string_view,11> names{"database_name","schema_name",routine.function?"function_name":"procedure_name",
      "parameter_name","ordinal_position","parameter_type","data_type","character_maximum_length","numeric_precision","numeric_scale","remarks"};
  if(source.columns.size()<10 || source.columns.size()>11)return invalid_routine_metadata("parameter-columns");
  std::array<std::size_t,11> index{};index.fill(source.columns.size());
  for(std::size_t c=0;c<source.columns.size();++c) {
    const auto it=std::find(names.begin(),names.end(),source.columns[c].name);
    if(it==names.end())return invalid_routine_metadata("parameter-layout");
    const auto n=static_cast<std::size_t>(it-names.begin());
    if(index[n]!=source.columns.size())return invalid_routine_metadata("parameter-layout");
    index[n]=c;
    if((n==4 || n==7 || n==8 || n==9)?!routine_integer(source.columns[c]):!schema_text(source.columns[c]))
      return invalid_routine_metadata("parameter-type");
  }
  for(std::size_t n=0;n<10;++n)if(index[n]==source.columns.size())return invalid_routine_metadata("parameter-layout");
  QueryResult output;
  if(!routine_headers(output,true,budget))return redshift_routine_limit();
  std::set<std::int32_t> ordinals;
  std::size_t inputs=0;
  for(const auto& row:source.rows) {
    if(rs::util::Clock::now()>=deadline)return routine_timeout();
    for(const auto n:{0u,1u,2u,5u,6u})if(!row[index[n]] || !schema_identifier(*row[index[n]]) || !budget.name(*row[index[n]]))
      return invalid_routine_metadata("parameter-identity");
    if(*row[index[0]]!=database || *row[index[1]]!=schema || *row[index[2]]!=routine.name)return invalid_routine_metadata("parameter-foreign");
    const auto& name=row[index[3]];
    if(!name || name->find('\0')!=std::string::npos || !rs::util::utf8_code_point_count(*name) || !budget.name(*name))
      return invalid_routine_metadata("parameter-name");
    const auto& mode=*row[index[5]];
    const int code=mode=="IN"?1:mode=="INOUT"?2:mode=="OUT"?4:mode=="RETURN"?5:0;
    if(code==1 || code==2)++inputs;
    std::optional<std::int32_t> ordinal,length,precision,scale;
    if(!code || !routine_number(row[index[4]],ordinal) || !ordinal || *ordinal<0 || !ordinals.insert(*ordinal).second ||
        (code==5?(!routine.function || *ordinal!=0):*ordinal==0) ||
        !routine_number(row[index[7]],length) || !routine_number(row[index[8]],precision) || !routine_number(row[index[9]],scale))
      return invalid_routine_metadata("parameter-dimensions");
    if((length && *length<-1) || (precision && *precision<0) || (scale && *scale<0))
      return invalid_routine_metadata("parameter-dimensions");
    auto dimensions=routine_dimensions(*row[index[6]],length,precision,scale);
    if(!dimensions)return invalid_routine_metadata("parameter-dimensions");
    // View optional remarks until the output-copy budget has been admitted.
    const ResultCell absent_remarks{};
    const auto& remarks=index[10]==source.columns.size()?absent_remarks:row[index[10]];
    if(remarks && (remarks->find('\0')!=std::string::npos || !rs::util::utf8_code_point_count(*remarks)))return invalid_routine_metadata("parameter-text");
    if(!routine_match(*name,request.column,request.name_matches.member))continue;
    const auto& d=*dimensions;
    // Numeric rendering is bounded fixed scratch; charge all large borrowed
    // text before copying it into the owning output row.
    ResultCell mode_text=std::to_string(code),type_text=std::to_string(d.code),sql_text=std::to_string(d.sql_type),
        ordinal_text=std::to_string(*ordinal);
    const auto bytes=database.size()+schema.size()+routine.name.size()+name->size()+row[index[6]]->size()+1+
        (remarks?remarks->size():0)+routine_cell_bytes({mode_text,type_text,sql_text,ordinal_text,d.size,d.buffer,d.scale,d.radix,d.sub,d.octets});
    if(!budget.retain({1,19,bytes,0,database.size()+schema.size()+routine.name.size()+name->size()+row[index[6]]->size()}))return redshift_routine_limit();
    output.rows.push_back({std::string(database),std::string(schema),routine.name,name,std::move(mode_text),
        std::move(type_text),row[index[6]],d.size,d.buffer,d.scale,d.radix,"2",remarks,std::nullopt,
        std::move(sql_text),d.sub,d.octets,std::move(ordinal_text),""});
  }
  // The signature includes IN/INOUT, not OUT/RETURN rows. Validation uses all
  // native rows before the caller's column-name filter.
  if(inputs!=routine.arguments.size())return invalid_routine_metadata("parameter-signature-count");
  if(rs::util::Clock::now()>=deadline)return routine_timeout();
  routine_text_capacities(output);
  return BackendResult<QueryResult>{std::move(output),input.session_snapshot()};
}
void sort_redshift_parameters(QueryResult& result) {
  std::stable_sort(result.rows.begin(),result.rows.end(),[](const auto& a,const auto& b) {
    const auto rank=[](const auto& row){return row[4] && *row[4]=="5"?0:1;};
    if(std::tie(a[0],a[1],a[2])!=std::tie(b[0],b[1],b[2]))return std::tie(a[0],a[1],a[2])<std::tie(b[0],b[1],b[2]);
    if(rank(a)!=rank(b))return rank(a)<rank(b);
    std::optional<std::int32_t> x,y;routine_number(a[17],x);routine_number(b[17],y);
    return x.value_or(0)<y.value_or(0);
  });
  routine_text_capacities(result);
}

std::string redshift_schemas_query(bool visible_databases) {
  if (visible_databases)
    return "SELECT database_name::text AS table_cat, schema_name::text AS table_schem, "
        "NULL::text AS table_name, NULL::text AS table_type, NULL::text AS remarks "
        "FROM svv_all_schemas ORDER BY table_cat, table_schem";
  // SVV_REDSHIFT_SCHEMAS lists schemas accessible to the current user.
  // Restrict this SQL_ALL_SCHEMAS enumeration to the connected database;
  // catalog/ordinary table filters do not apply to this enumeration mode.
  return "SELECT NULL::text AS table_cat, schema_name::text AS table_schem, "
      "NULL::text AS table_name, NULL::text AS table_type, NULL::text AS "
      "remarks FROM svv_redshift_schemas WHERE database_name = current_database() "
      "ORDER BY table_schem";
}

std::string redshift_columns_query(const ColumnsCatalogRequest& input, bool visible_databases) {
  auto normalized = native_catalog_request(CatalogRequest{input});
  if (!normalized) throw std::invalid_argument("Invalid catalog name semantics");
  const auto& request = std::get<ColumnsCatalogRequest>(*normalized);

  // SVV_COLUMNS exposes Redshift's own dimensions without PostgreSQL domain,
  // LATERAL, OID or server-encoding helpers. Unsupported types remain unknown.
  // Every CASE qualifies the source type: Redshift permits lateral alias reuse.
  const std::string type = "LOWER(columns.data_type)";
  const std::string data_type =
      "CASE " + type + " WHEN 'boolean' THEN -7 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 4 WHEN 'bigint' THEN -5 WHEN 'real' THEN 7 "
      "WHEN 'double precision' THEN 8 WHEN 'numeric' THEN 2 WHEN 'decimal' THEN 3 "
      "WHEN 'character' THEN 1 WHEN 'character varying' THEN 12 "
      "WHEN 'varbyte' THEN -4 WHEN 'varbinary' THEN -4 WHEN 'binary varying' THEN -4 "
      "WHEN 'super' THEN -1 "
      "WHEN 'geometry' THEN -4 WHEN 'geography' THEN -4 "
      "WHEN 'date' THEN 91 WHEN 'time without time zone' THEN 92 "
      "WHEN 'time with time zone' THEN 92 WHEN 'timestamp without time zone' THEN 93 "
      "WHEN 'timestamp with time zone' THEN 93 ELSE 0 END";
  const std::string fraction =
      "CASE WHEN columns.datetime_precision > 0 THEN 1 + columns.datetime_precision ELSE 0 END";
  std::string query =
      "SELECT table_cat, table_schem, table_name, column_name, data_type, "
      "type_name, column_size, buffer_length, decimal_digits, num_prec_radix, "
      "nullable, remarks, column_def, sql_data_type, sql_datetime_sub, "
      "char_octet_length, ordinal_position, is_nullable FROM (SELECT "
      "columns.table_catalog::text AS table_cat, columns.table_schema::text AS table_schem, "
      "columns.table_name::text AS table_name, columns.column_name::text AS column_name, " +
      data_type + "::smallint AS data_type, columns.data_type::text AS type_name, "
      "CASE " + type + " WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 10 WHEN 'bigint' THEN 19 WHEN 'real' THEN 7 "
      "WHEN 'double precision' THEN 15 WHEN 'numeric' THEN columns.numeric_precision "
      "WHEN 'decimal' THEN columns.numeric_precision "
      "WHEN 'character' THEN columns.character_maximum_length "
      "WHEN 'character varying' THEN columns.character_maximum_length WHEN 'date' THEN 10 "
      "WHEN 'time without time zone' THEN 8 + " + fraction +
      " WHEN 'time with time zone' THEN 14 + " + fraction +
      " WHEN 'timestamp without time zone' THEN 19 + " + fraction +
      " WHEN 'timestamp with time zone' THEN 25 + " + fraction +
      " ELSE NULL END::integer AS column_size, "
      "CASE " + type + " WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 2 "
      "WHEN 'integer' THEN 4 WHEN 'bigint' THEN 8 WHEN 'real' THEN 4 "
      "WHEN 'double precision' THEN 8 WHEN 'numeric' THEN columns.numeric_precision + 2 "
      "WHEN 'decimal' THEN columns.numeric_precision + 2 "
      "WHEN 'character' THEN columns.character_maximum_length "
      "WHEN 'character varying' THEN columns.character_maximum_length "
      "WHEN 'date' THEN 6 WHEN 'time without time zone' THEN 6 "
      "WHEN 'time with time zone' THEN 6 WHEN 'timestamp without time zone' THEN 16 "
      "WHEN 'timestamp with time zone' THEN 16 ELSE NULL END::integer AS buffer_length, "
      "CASE WHEN " + type + " IN ('numeric','decimal') THEN columns.numeric_scale "
      "WHEN " + type + " IN ('smallint','integer','bigint') THEN 0 "
      "WHEN " + type + " IN ('time without time zone','time with time zone',"
      "'timestamp without time zone','timestamp with time zone') THEN columns.datetime_precision "
      "ELSE NULL END::smallint AS decimal_digits, "
      "CASE WHEN " + type + " IN ('smallint','integer','bigint','numeric','decimal') THEN 10 "
      "WHEN " + type + " IN ('real','double precision') THEN 2 ELSE NULL END::smallint AS num_prec_radix, "
      "CASE columns.is_nullable WHEN 'YES' THEN 1 WHEN 'NO' THEN 0 ELSE 2 END::smallint AS nullable, "
      "columns.remarks::text AS remarks, columns.column_default::text AS column_def, "
      "CASE WHEN " + type + " IN ('date','time without time zone','time with time zone',"
      "'timestamp without time zone','timestamp with time zone') THEN 9 ELSE " + data_type +
      " END::smallint AS sql_data_type, CASE " + type + " WHEN 'date' THEN 1 "
      "WHEN 'time without time zone' THEN 2 WHEN 'time with time zone' THEN 2 "
      "WHEN 'timestamp without time zone' THEN 3 WHEN 'timestamp with time zone' THEN 3 "
      "ELSE NULL END::smallint AS sql_datetime_sub, "
      "CASE WHEN " + type + " IN ('character','character varying') "
      "THEN columns.character_maximum_length ELSE NULL END::integer AS char_octet_length, "
      "columns.ordinal_position::integer AS ordinal_position, columns.is_nullable::text AS is_nullable "
      "FROM " + std::string(visible_databases ?
          "(SELECT database_name AS table_catalog, schema_name AS table_schema, table_name, "
          "column_name, ordinal_position, column_default, is_nullable, data_type, "
          "character_maximum_length, numeric_precision, numeric_scale, remarks, "
          "NULL::integer AS datetime_precision FROM svv_all_columns)" : "svv_columns") +
      " AS columns) AS odbcpp_columns WHERE 1=1";
  if (request.catalog) query += " AND table_cat = " + literal(*request.catalog);
  if (request.schema) query += " AND table_schem" + std::string(request.name_matches.schema == CatalogNameMatch::Existing ? " LIKE " : " = ") + literal(*request.schema);
  if (request.table) query += " AND table_name" + std::string(request.name_matches.object == CatalogNameMatch::Existing ? " LIKE " : " = ") + literal(*request.table);
  if (request.column) query += " AND column_name" + std::string(request.name_matches.member == CatalogNameMatch::Existing ? " LIKE " : " = ") + literal(*request.column);
  query += " ORDER BY table_cat, table_schem, table_name, ordinal_position";
  return query;
}

// Redshift has no indexes. The pinned AWS driver also returns an empty
// 13-column SQLStatistics result rather than PostgreSQL index metadata.
std::string redshift_statistics_query(const StatisticsCatalogRequest&) {
  return "SELECT CAST(NULL AS VARCHAR(128)) AS table_cat, "
      "CAST(NULL AS VARCHAR(128)) AS table_schem, "
      "CAST(NULL AS VARCHAR(128)) AS table_name, "
      "CAST(NULL AS SMALLINT) AS non_unique, "
      "CAST(NULL AS VARCHAR(128)) AS index_qualifier, "
      "CAST(NULL AS VARCHAR(128)) AS index_name, "
      "CAST(NULL AS SMALLINT) AS type, "
      "CAST(NULL AS SMALLINT) AS ordinal_position, "
      "CAST(NULL AS VARCHAR(128)) AS column_name, "
      "CAST(NULL AS VARCHAR(1)) AS asc_or_desc, "
      "CAST(NULL AS INTEGER) AS cardinality, "
      "CAST(NULL AS INTEGER) AS pages, "
      "CAST(NULL AS VARCHAR(128)) AS filter_condition WHERE 1=0";
}

// Modern AWS SHOW discovery exposes no row-version columns. This SELECT
// establishes the field families and empty result, not its local/no-I/O path
// or legacy xmin/oid behavior. Descriptor widths still need live qualification.
std::string redshift_row_version_query() {
  return "SELECT CAST(NULL AS SMALLINT) AS scope, "
      "CAST(NULL AS VARCHAR) AS column_name, "
      "CAST(NULL AS SMALLINT) AS data_type, "
      "CAST(NULL AS VARCHAR) AS type_name, "
      "CAST(NULL AS INTEGER) AS column_size, "
      "CAST(NULL AS INTEGER) AS buffer_length, "
      "CAST(NULL AS SMALLINT) AS decimal_digits, "
      "CAST(NULL AS SMALLINT) AS pseudo_column WHERE 1=0";
}

}  // namespace rs::core::database::postgres
