#include "redshift_catalog_query.h"
#include "core/database/result_validation.h"
#include <algorithm>
#include <array>
#include <set>
#include <tuple>

namespace rs::core::database::postgres {
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
bool clean_schema_result(const QueryResult& source) {
  return valid_result_structure(source) && source.additional_results.empty() &&
      source.cell_errors.empty() && source.normalized_parameter_types.empty();
}
}  // namespace

BackendResult<std::string> redshift_schema_database(BackendResult<QueryResult> input) {
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
  return BackendResult<std::string>{*source.rows[0][0], input.session_snapshot()};
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
  constexpr std::array<std::string_view, 7> names{
      "database_name", "schema_name", "schema_owner", "schema_type",
      "schema_acl", "source_database", "schema_option"};
  std::array<std::size_t, 7> indexes{};
  for (std::size_t expected = 0; expected < names.size(); ++expected) {
    unsigned matches = 0;
    for (std::size_t actual = 0; actual < source.columns.size(); ++actual) {
      if (source.columns[actual].name == names[expected]) {
        ++matches; indexes[expected] = actual;
      }
    }
    if (matches != 1) return invalid_schema_metadata("show-layout");
  }
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
      source.columns[indexes[1]].normalized_type->column_size;
  for (const auto& schema : schemas)
    output.rows.push_back({std::nullopt, schema, std::nullopt, std::nullopt, std::nullopt});
  return BackendResult<QueryResult>{std::move(output), input.session_snapshot()};
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
    std::string_view schema, const TablesCatalogRequest& request, BackendResult<QueryResult> input) {
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
  constexpr std::array<std::string_view,6> types{"EXTERNAL TABLE","LOCAL TEMPORARY","SYSTEM TABLE","SYSTEM VIEW","TABLE","VIEW"};
  std::set<std::string> identities;
  QueryResult output;
  constexpr std::array<const char*,5> outnames{"TABLE_CAT","TABLE_SCHEM","TABLE_NAME","TABLE_TYPE","REMARKS"};
  for (std::size_t i = 0; i < indexes.size(); ++i) {
    auto type = *source.columns[indexes[i]].normalized_type; type.type = ScalarType::VarChar;
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
    if (!table_like(database,request.catalog) || !table_like(*row[indexes[2]],request.table) ||
        (request.types && std::find(request.types->begin(),request.types->end(),*row[indexes[3]])==request.types->end())) continue;
    output.rows.push_back({row[indexes[0]],row[indexes[1]],row[indexes[2]],row[indexes[3]],row[indexes[4]]});
  }
  std::sort(output.rows.begin(),output.rows.end(),[](const auto& a,const auto& b) {
    return std::tie(a[3],a[0],a[1],a[2]) < std::tie(b[3],b[0],b[1],b[2]);
  });
  return BackendResult<QueryResult>{std::move(output),input.session_snapshot()};
}

std::string redshift_schemas_query() {
  // SVV_REDSHIFT_SCHEMAS lists schemas accessible to the current user.
  // Restrict this SQL_ALL_SCHEMAS enumeration to the connected database;
  // catalog/ordinary table filters do not apply to this enumeration mode.
  return "SELECT NULL::text AS table_cat, schema_name::text AS table_schem, "
      "NULL::text AS table_name, NULL::text AS table_type, NULL::text AS "
      "remarks FROM svv_redshift_schemas WHERE database_name = current_database() "
      "ORDER BY table_schem";
}

std::string redshift_columns_query(const ColumnsCatalogRequest& request) {
  // SVV_COLUMNS exposes Redshift's own dimensions without PostgreSQL domain,
  // LATERAL, OID or server-encoding helpers. Unsupported types remain unknown.
  // Every CASE qualifies the source type: Redshift permits lateral alias reuse.
  const std::string type = "LOWER(columns.data_type)";
  const std::string data_type =
      "CASE " + type + " WHEN 'boolean' THEN -7 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 4 WHEN 'bigint' THEN -5 WHEN 'real' THEN 7 "
      "WHEN 'double precision' THEN 8 WHEN 'numeric' THEN 2 WHEN 'decimal' THEN 3 "
      "WHEN 'character' THEN 1 WHEN 'character varying' THEN 12 "
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
      "FROM svv_columns AS columns) AS odbcpp_columns WHERE 1=1";
  if (request.catalog) query += " AND table_cat = " + literal(*request.catalog);
  if (request.schema) query += " AND table_schem LIKE " + literal(*request.schema);
  if (request.table) query += " AND table_name LIKE " + literal(*request.table);
  if (request.column) query += " AND column_name LIKE " + literal(*request.column);
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
