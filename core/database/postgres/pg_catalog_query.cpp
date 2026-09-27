#include "pg_database_connection.h"

namespace rs::core::database::postgres {
namespace {

std::string quote_catalog_literal(const std::string& value) {
  std::string quoted{"'"};
  quoted.reserve(value.size() + 2);
  for (const char ch : value) {
    if (ch == '\'') quoted.push_back('\'');
    quoted.push_back(ch);
  }
  quoted.push_back('\'');
  return quoted;
}

std::string build_query(const TablesCatalogRequest& request) {
  if (request.mode == TablesCatalogRequest::Mode::Catalogs) {
    return std::string(
        "SELECT current_database()::text AS table_cat, NULL::text AS "
        "table_schem, NULL::text AS table_name, NULL::text AS table_type, "
        "NULL::text AS remarks");
  }
  if (request.mode == TablesCatalogRequest::Mode::Schemas) {
    return std::string(
        "SELECT NULL::text AS table_cat, schema_name::text AS table_schem, "
        "NULL::text AS table_name, NULL::text AS table_type, NULL::text AS "
        "remarks FROM information_schema.schemata ORDER BY table_schem");
  }
  if (request.mode == TablesCatalogRequest::Mode::TableTypes) {
    return std::string(
        "SELECT NULL::text AS table_cat, NULL::text AS table_schem, "
        "NULL::text AS table_name, table_type, NULL::text AS remarks FROM "
        "(VALUES ('TABLE'::text), ('VIEW'::text), ('SYSTEM TABLE'::text), "
        "('FOREIGN TABLE'::text), ('LOCAL TEMPORARY'::text)) "
        "AS supported(table_type) ORDER BY table_type");
  }
  std::string query =
      "SELECT table_cat, table_schem, table_name, table_type, NULL::text AS "
      "remarks FROM (SELECT current_database()::text AS table_cat, "
      "table_schema::text AS table_schem, table_name::text AS table_name, "
      "CASE WHEN table_type = 'VIEW' THEN 'VIEW' WHEN table_schema IN "
      "('pg_catalog', 'information_schema') THEN 'SYSTEM TABLE' WHEN "
      "table_type = 'BASE TABLE' THEN 'TABLE' WHEN "
      "table_type = 'LOCAL TEMPORARY' THEN 'LOCAL TEMPORARY' WHEN "
      "table_type = 'FOREIGN' THEN 'FOREIGN TABLE' ELSE table_type END::text "
      "AS table_type FROM information_schema.tables) AS odbcpp_tables WHERE 1=1";
  if (request.catalog) {
    query += " AND table_cat LIKE " + quote_catalog_literal(*request.catalog);
  }
  if (request.schema) {
    query += " AND table_schem LIKE " + quote_catalog_literal(*request.schema);
  }
  if (request.table) {
    query += " AND table_name LIKE " + quote_catalog_literal(*request.table);
  }
  if (request.types) {
    const auto& types = *request.types;
    if (types.empty()) {
      query += " AND FALSE";
    } else {
      query += " AND table_type IN (";
      for (std::size_t i = 0; i < types.size(); ++i) {
        if (i != 0) query += ',';
        query += quote_catalog_literal(types[i]);
      }
      query += ')';
    }
  }
  query += " ORDER BY table_type, table_cat, table_schem, table_name";
  return query;
}

std::string build_query(const PrimaryKeysCatalogRequest& request) {
  std::string query =
      "SELECT current_database()::text AS table_cat, "
      "keys.table_schema::text AS table_schem, "
      "keys.table_name::text AS table_name, "
      "keys.column_name::text AS column_name, "
      "keys.ordinal_position::smallint AS key_seq, "
      "constraints.constraint_name::text AS pk_name "
      "FROM information_schema.table_constraints AS constraints "
      "JOIN information_schema.key_column_usage AS keys "
      "ON constraints.constraint_catalog = keys.constraint_catalog "
      "AND constraints.constraint_schema = keys.constraint_schema "
      "AND constraints.constraint_name = keys.constraint_name "
      "AND constraints.table_catalog = keys.table_catalog "
      "AND constraints.table_schema = keys.table_schema "
      "AND constraints.table_name = keys.table_name "
      "WHERE constraints.constraint_type = 'PRIMARY KEY' "
      "AND keys.table_name = " + quote_catalog_literal(request.table);
  if (request.catalog) {
    query += " AND keys.table_catalog = " +
        quote_catalog_literal(*request.catalog);
  }
  if (request.schema) {
    query += " AND keys.table_schema = " +
        quote_catalog_literal(*request.schema);
  }
  query +=
      " ORDER BY table_cat, table_schem, table_name, key_seq";
  return query;
}

std::string build_query(const ForeignKeysCatalogRequest& request) {
  std::string query =
      "SELECT current_database()::text AS pktable_cat, "
      "pk_namespaces.nspname::text AS pktable_schem, "
      "pk_tables.relname::text AS pktable_name, "
      "pk_columns.attname::text AS pkcolumn_name, "
      "current_database()::text AS fktable_cat, "
      "fk_namespaces.nspname::text AS fktable_schem, "
      "fk_tables.relname::text AS fktable_name, "
      "fk_columns.attname::text AS fkcolumn_name, "
      "key_columns.ordinality::smallint AS key_seq, "
      "CASE fk_constraints.confupdtype WHEN 'c' THEN 0 "
      "WHEN 'r' THEN 1 WHEN 'n' THEN 2 "
      "WHEN 'a' THEN 3 WHEN 'd' THEN 4 "
      "ELSE 3 END::smallint AS update_rule, "
      "CASE fk_constraints.confdeltype WHEN 'c' THEN 0 "
      "WHEN 'r' THEN 1 WHEN 'n' THEN 2 "
      "WHEN 'a' THEN 3 WHEN 'd' THEN 4 "
      "ELSE 3 END::smallint AS delete_rule, "
      "fk_constraints.conname::text AS fk_name, "
      "pk_constraints.conname::text AS pk_name, "
      "CASE WHEN NOT fk_constraints.condeferrable THEN 7 "
      "WHEN fk_constraints.condeferred THEN 5 "
      "ELSE 6 END::smallint AS deferrability "
      "FROM pg_catalog.pg_constraint AS fk_constraints "
      "JOIN pg_catalog.pg_class AS fk_tables "
      "ON fk_tables.oid = fk_constraints.conrelid "
      "JOIN pg_catalog.pg_namespace AS fk_namespaces "
      "ON fk_namespaces.oid = fk_tables.relnamespace "
      "JOIN pg_catalog.pg_class AS pk_tables "
      "ON pk_tables.oid = fk_constraints.confrelid "
      "JOIN pg_catalog.pg_namespace AS pk_namespaces "
      "ON pk_namespaces.oid = pk_tables.relnamespace "
      "JOIN pg_catalog.pg_constraint AS pk_constraints "
      "ON pk_constraints.conrelid = fk_constraints.confrelid "
      "AND pk_constraints.contype = 'p' "
      "AND pk_constraints.conkey @> fk_constraints.confkey "
      "AND pk_constraints.conkey <@ fk_constraints.confkey "
      "CROSS JOIN LATERAL unnest(fk_constraints.conkey, "
      "fk_constraints.confkey) WITH ORDINALITY "
      "AS key_columns(fk_attribute, pk_attribute, ordinality) "
      "JOIN pg_catalog.pg_attribute AS fk_columns "
      "ON fk_columns.attrelid = fk_constraints.conrelid "
      "AND fk_columns.attnum = key_columns.fk_attribute "
      "JOIN pg_catalog.pg_attribute AS pk_columns "
      "ON pk_columns.attrelid = fk_constraints.confrelid "
      "AND pk_columns.attnum = key_columns.pk_attribute "
      "WHERE fk_constraints.contype = 'f'";
  if (request.primary_catalog) {
    query += " AND current_database() = " +
        quote_catalog_literal(*request.primary_catalog);
  }
  if (request.primary_schema) {
    query += " AND pk_namespaces.nspname = " +
        quote_catalog_literal(*request.primary_schema);
  }
  if (request.primary_table) {
    query += " AND pk_tables.relname = " +
        quote_catalog_literal(*request.primary_table);
  }
  if (request.foreign_catalog) {
    query += " AND current_database() = " +
        quote_catalog_literal(*request.foreign_catalog);
  }
  if (request.foreign_schema) {
    query += " AND fk_namespaces.nspname = " +
        quote_catalog_literal(*request.foreign_schema);
  }
  if (request.foreign_table) {
    query += " AND fk_tables.relname = " +
        quote_catalog_literal(*request.foreign_table);
  }
  if (request.primary_table) {
    query +=
        " ORDER BY fktable_cat, fktable_schem, fktable_name, key_seq";
  } else {
    query +=
        " ORDER BY pktable_cat, pktable_schem, pktable_name, key_seq";
  }
  return query;
}

} // namespace

rs::util::Result<std::string> PgDatabaseConnection::catalog_query(
    const CatalogRequest& request) const {
  return std::visit([](const auto& catalog) { return build_query(catalog); }, request);
}

} // namespace rs::core::database::postgres
