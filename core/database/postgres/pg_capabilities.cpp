#include "pg_database_connection.h"

namespace rs::core::database::postgres {
BackendCapabilities PgDatabaseConnection::capabilities() const {
  BackendCapabilities result;
#ifdef ODBCPP_ENABLE_REDSHIFT
  // Preserve the existing build identity; live Redshift acceptance is separate.
  result.dbms_name = "Amazon Redshift";
#else
  result.dbms_name = "PostgreSQL";
#endif
  result.identifier_quote = "\"";
  result.catalog_separator = ".";
  result.catalog_term = "database";
  result.schema_term = "schema";
  result.table_term = "table";
  result.procedure_term = "procedure";
  result.pattern_escape = "\\";
  result.max_identifier_length = 63;
  result.identifier_case = IdentifierCase::Lower;
  result.quoted_identifier_case = IdentifierCase::Sensitive;
  result.null_collation = NullCollation::High;
  result.correlation_names = CorrelationNames::Any;
  result.group_by = GroupBySupport::Unrelated;
  result.catalog_names = result.column_aliases = result.describe_parameters = true;
  result.order_by_expressions = result.integrity = result.like_escape = true;
  result.order_by_requires_select = result.read_only = false;
  result.outer_joins = result.procedures = result.non_nullable_columns = true;
  result.create_index = result.drop_index = true;
  result.insert_literals = result.insert_searched = result.select_into = true;
  result.sql92_entry = result.union_distinct = result.union_all = true;
  result.schema_in_dml = result.schema_in_procedures = true;
  result.schema_in_table_definitions = result.schema_in_index_definitions = true;
  result.schema_in_privileges = true;
  return result;
}
} // namespace rs::core::database::postgres
