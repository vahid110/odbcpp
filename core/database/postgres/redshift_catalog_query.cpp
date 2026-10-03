#include "redshift_catalog_query.h"

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
