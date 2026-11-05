#include "pg_backend_provider.h"

namespace rs::core::database::postgres {
namespace {
void set_sql_conversion_capabilities(BackendCapabilities& result, bool redshift) noexcept {
  using Type = SqlConversionType;
  constexpr std::array numeric_types{Type::SmallInteger, Type::Integer, Type::BigInteger,
                                     Type::Real, Type::Double, Type::Numeric};
  std::uint32_t numeric = 0;
  for (const auto type : numeric_types) numeric |= sql_conversion_bit(type);
  const auto character = sql_conversion_bit(Type::Character);
  const auto binary = sql_conversion_bit(Type::Binary);
  const auto boolean = sql_conversion_bit(Type::Boolean);
  const auto date = sql_conversion_bit(Type::Date);
  const auto time = sql_conversion_bit(Type::Time);
  const auto timestamp = sql_conversion_bit(Type::Timestamp);
  const auto uuid = sql_conversion_bit(Type::Uuid);
  auto& targets = result.conversions.targets;
  targets.fill(0);
  for (const auto type : numeric_types) targets[static_cast<std::size_t>(type)] = numeric | character;
  targets[static_cast<std::size_t>(Type::Character)] =
      character | numeric | binary | date | time | timestamp | (redshift ? 0u : boolean | uuid);
  targets[static_cast<std::size_t>(Type::Boolean)] = boolean | (redshift ? 0u : character);
  targets[static_cast<std::size_t>(Type::Binary)] = binary | character;
  targets[static_cast<std::size_t>(Type::Date)] = date | timestamp | character;
  targets[static_cast<std::size_t>(Type::Time)] = time | character;
  targets[static_cast<std::size_t>(Type::Timestamp)] = timestamp | date | time | character;
  targets[static_cast<std::size_t>(Type::Uuid)] = redshift ? 0u : uuid | character;
  result.timestamp_add_intervals = 0;
  for (const auto unit : {SqlIntervalUnit::Second, SqlIntervalUnit::Minute, SqlIntervalUnit::Hour,
       SqlIntervalUnit::Day, SqlIntervalUnit::Week, SqlIntervalUnit::Month, SqlIntervalUnit::Quarter, SqlIntervalUnit::Year}) {
    result.timestamp_add_intervals |= sql_interval_bit(unit);
  }
  result.timestamp_diff_intervals = result.timestamp_add_intervals & ~sql_interval_bit(SqlIntervalUnit::Week);
}
} // namespace

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
  result.outer_joins = result.procedures = result.non_nullable_columns = true;
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
  set_sql_conversion_capabilities(result, false);
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
  // CONVERT and TIMESTAMPADD/TIMESTAMPDIFF are translated for Redshift too,
  // but no Redshift statement behind them has run live yet (the generated SQL
  // uses REGEXP_REPLACE with a lookahead, VARCHAR(MAX) and VARBYTE casts).
  // Report them only after the Redshift live acceptance passes (#144); until
  // then applications see no support and do not emit these escapes. The
  // PostgreSQL capabilities this starts from report them, so clear them here.
  result.conversions.targets.fill(0);
  result.timestamp_add_intervals = 0;
  result.timestamp_diff_intervals = 0;
  return result;
}

} // namespace rs::core::database::postgres
