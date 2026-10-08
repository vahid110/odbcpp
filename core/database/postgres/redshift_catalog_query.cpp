#include "redshift_catalog_query.h"
#include "core/database/result_validation.h"
#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <charconv>
#include <limits>
#include "pg_protocol_parser.h"

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
    if(textsource[i]>=0){native=*source.columns[index[static_cast<std::size_t>(textsource[i])]].normalized_type;native.type=ScalarType::VarChar;}
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
