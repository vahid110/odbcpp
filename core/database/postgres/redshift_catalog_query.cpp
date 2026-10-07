#include "redshift_catalog_query.h"
#include "core/database/result_validation.h"
#include <algorithm>
#include <array>
#include <set>

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
BackendError invalid_schema_metadata() {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
      "Invalid Redshift schema metadata"};
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
  if (!clean_schema_result(source) || source.columns.size() != 1 ||
      source.columns[0].name != "database_name" || !schema_text(source.columns[0]) ||
      source.rows.size() != 1 || source.affected_rows != 1 || !source.rows[0][0] ||
      !schema_identifier(*source.rows[0][0]) ||
      input.session_snapshot().disposition == SessionDisposition::Retire)
    return invalid_schema_metadata();
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
  if (!schema_identifier(database) || !clean_schema_result(source) ||
      source.columns.size() != 7 || source.rows.size() > 10000 ||
      (source.affected_rows != 0 && source.affected_rows != source.rows.size()))
    return invalid_schema_metadata();
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
    if (matches != 1) return invalid_schema_metadata();
  }
  if (!schema_text(source.columns[indexes[0]]) ||
      !schema_text(source.columns[indexes[1]])) return invalid_schema_metadata();
  std::set<std::string> schemas;
  for (const auto& row : source.rows) {
    const auto& db = row[indexes[0]];
    const auto& schema = row[indexes[1]];
    if (!db || !schema || *db != database || !schema_identifier(*schema) ||
        !schemas.insert(*schema).second) return invalid_schema_metadata();
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
