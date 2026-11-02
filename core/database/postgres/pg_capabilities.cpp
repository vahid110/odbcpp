#include "pg_backend_provider.h"

namespace rs::core::database::postgres {
namespace {
SqlCapabilities common_sql_capabilities() noexcept {
  SqlCapabilities result;
  result.aggregate_all = true;
  result.aggregate_average = true;
  result.aggregate_count = true;
  result.aggregate_distinct = true;
  result.aggregate_maximum = true;
  result.aggregate_minimum = true;
  result.aggregate_sum = true;
  result.batch_command = true;
  result.batch_individual_counts = true;
  result.batch_select = true;
  result.date_literal = true;
  result.datetime_current_date = true;
  result.join_cross = true;
  result.join_full = true;
  result.join_inner = true;
  result.join_inner_with_outer = true;
  result.join_left = true;
  result.join_natural = true;
  result.join_nested = true;
  result.join_right = true;
  result.join_unordered_columns = true;
  result.numeric_character_length = true;
  result.numeric_extract = true;
  result.numeric_octet_length = true;
  result.numeric_position = true;
  result.predicate_between = true;
  result.predicate_comparison = true;
  result.predicate_exists = true;
  result.predicate_in = true;
  result.predicate_like = true;
  result.predicate_not_null = true;
  result.predicate_null = true;
  result.scalar_current_date = true;
  result.scalar_current_time = true;
  result.scalar_current_timestamp = true;
  result.scalar_if_null = true;
  result.scalar_lower = true;
  result.scalar_upper = true;
  result.string_lower = true;
  result.string_substring = true;
  result.string_trim_both = true;
  result.string_trim_leading = true;
  result.string_trim_trailing = true;
  result.string_upper = true;
  result.subquery_comparison = true;
  result.subquery_exists = true;
  result.subquery_in = true;
  result.time_literal = true;
  result.timestamp_literal = true;
  result.value_case = true;
  result.value_cast = true;
  result.value_coalesce = true;
  result.value_nullif = true;
  return result;
}
}  // namespace

BackendCapabilities pg_backend_capabilities(std::string_view display_name) noexcept {
  BackendCapabilities result;
  result.dbms_name = display_name;
  result.identifier_quote = "\"";
  result.catalog_separator = ".";
  result.catalog_term = "database";
  result.schema_term = "schema";
  result.table_term = "table";
  result.procedure_term = "procedure";
  result.pattern_escape = "\\";
  result.keywords =
      "ANALYSE,ANALYZE,ARRAY,ASYMMETRIC,CURRENT_CATALOG,CURRENT_ROLE,"
      "CURRENT_SCHEMA,DO,ILIKE,LATERAL,LIMIT,LOCALTIME,LOCALTIMESTAMP,OFFSET,"
      "PLACING,RETURNING,SYMMETRIC,VARIADIC,VERBOSE,WINDOW";
  result.max_identifier_length = 63;
  result.max_columns_in_table = 1600;
  result.max_columns_in_select = 1600;
  result.identifier_case = IdentifierCase::Lower;
  result.quoted_identifier_case = IdentifierCase::Sensitive;
  result.null_collation = NullCollation::High;
  result.correlation_names = CorrelationNames::Any;
  result.group_by = GroupBySupport::Unrelated;
  result.info_schema_support = InfoSchemaSupport::Full;
  result.catalog_names = result.column_aliases = result.describe_parameters = true;
  result.order_by_expressions = result.integrity = result.like_escape = true;
  result.order_by_requires_select = result.read_only = false;
  result.procedures = result.non_nullable_columns = true;
  result.create_table = result.create_table_commit_preserve =
      result.create_table_commit_delete =
          result.create_table_column_collation = true;
  result.drop_table = result.create_view =
      result.create_view_check_option = result.drop_view = true;
  result.alter_table = result.alter_table_column_collation = true;
  result.create_schema = result.drop_schema = true;
  result.create_index = result.drop_index = true;
  result.insert_literals = result.insert_searched = result.select_into = true;
  result.sql92_entry = result.union_distinct = result.union_all = true;
  result.schema_in_dml = result.schema_in_procedures = true;
  result.schema_in_table_definitions = result.schema_in_index_definitions = true;
  result.schema_in_privileges = true;
  result.sql = common_sql_capabilities();
  result.sql.datetime_current_time = true;
  result.sql.datetime_current_timestamp = true;
  result.sql.numeric_bit_length = true;
  result.sql.predicate_overlaps = true;
  result.sql.predicate_quantified = true;
  result.sql.row_null = true;
  result.sql.row_subquery = true;
  result.sql.row_value = true;
  result.sql.subquery_correlated = true;
  result.sql.subquery_quantified = true;
  return result;
}

BackendCapabilities redshift_backend_capabilities(std::string_view display_name) noexcept {
  auto result = pg_backend_capabilities(display_name);
  // Redshift keywords per AWS documentation (https://docs.aws.amazon.com/redshift/latest/dg/r_pg_keywords.html)
  // excluding standard ODBC 3.0 reserved keywords.
  result.keywords =
      "AES128,AES256,ALLOWOVERWRITE,ANALYSE,ANALYZE,ARRAY,AZ64,BACKUP,BINARY,"
      "BLANKSASNULL,BYTEDICT,BZIP2,CREDENTIALS,CURRENT_USER_ID,DEFLATE,DEFRAG,"
      "DELTA,DELTA32K,DISABLE,DO,EMPTYASNULL,ENABLE,ENCODE,ENCRYPTION,EXPLICIT,"
      "FREEZE,GLOBALDICT256,GLOBALDICT64K,GZIP,IGNORE,ILIKE,ISNULL,LIMIT,"
      "LOCALTIME,LOCALTIMESTAMP,LUN,LUNS,LZO,LZOP,MINUS,MOSTLY16,MOSTLY32,"
      "MOSTLY8,NEW,NOTNULL,NULLS,OFF,OFFLINE,OFFSET,OID,OLD,PARALLEL,PARTITION,"
      "PERCENT,PERMISSIONS,PIVOT,PLACING,RAW,READRATIO,RECOVER,REJECTLOG,"
      "RESORT,RESPECT,RESTORE,SIMILAR,SYSDATE,SYSTEM,TAG,TDES,TEXT255,TEXT32K,"
      "TOP,TRUNCATECOLUMNS,UNNEST,UNPIVOT,VERBOSE,WALLET,WITHOUT";
  // Redshift identifiers allow 127 bytes; PostgreSQL indexes do not exist.
  result.max_identifier_length = 127;
  result.info_schema_support = InfoSchemaSupport::Partial;
  result.create_table_commit_preserve = false;
  result.create_table_commit_delete = false;
  result.create_table_column_collation = false;
  result.create_view_check_option = false;
  result.alter_table_column_collation = false;
  result.create_index = result.drop_index = false;
  result.schema_in_index_definitions = false;
  // SQL-92 Entry requires an available SERIALIZABLE isolation option. The
  // native database mode is not selected by session isolation aliases.
  result.sql92_entry = false;
  // Start from the common table, not PostgreSQL-only claims. Native pass-through
  // is not proof of a scalar escape; Redshift translations are declared below.
  result.sql = common_sql_capabilities();
  result.sql.scalar_day = true;
  result.sql.scalar_hour = true;
  result.sql.scalar_locate = true;
  result.sql.scalar_log10 = true;
  result.sql.scalar_minute = true;
  result.sql.scalar_month = true;
  result.sql.scalar_natural_log = true;
  result.sql.scalar_second = true;
  result.sql.scalar_trimmed_length = true;
  result.sql.scalar_weekday = true;
  result.sql.scalar_year = true;
  result.sql.scalar_yearday = true;
  return result;
}

} // namespace rs::core::database::postgres
