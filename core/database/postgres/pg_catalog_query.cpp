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

std::string catalog_data_type_sql(const std::string& type_oid) {
  return "CASE " + type_oid +
      " WHEN 16 THEN -7 WHEN 17 THEN -3 WHEN 18 THEN 1 "
      "WHEN 20 THEN -5 WHEN 21 THEN 5 WHEN 23 THEN 4 "
      "WHEN 25 THEN -1 WHEN 700 THEN 7 WHEN 701 THEN 8 "
      "WHEN 1042 THEN 1 WHEN 1043 THEN 12 WHEN 1082 THEN 91 "
      "WHEN 1083 THEN 92 WHEN 1266 THEN 92 WHEN 1114 THEN 93 "
      "WHEN 1184 THEN 93 WHEN 1700 THEN 2 ELSE 12 END::smallint";
}

std::string catalog_type_name_sql(const std::string& type_oid) {
  return "CASE " + type_oid +
      " WHEN 1083 THEN 'time' WHEN 1114 THEN 'timestamp' "
      "ELSE pg_catalog.format_type(" + type_oid + ", NULL) END::text";
}

std::string catalog_base_type_join_sql(const std::string& type_oid) {
  return " CROSS JOIN LATERAL (WITH RECURSIVE domain_chain AS ("
      "SELECT oid, typbasetype, typtypmod AS type_modifier "
      "FROM pg_catalog.pg_type WHERE oid = " +
      type_oid +
      " UNION ALL SELECT base.oid, base.typbasetype, "
      "CASE WHEN chain.type_modifier >= 0 THEN chain.type_modifier "
      "ELSE base.typtypmod END "
      "FROM domain_chain AS chain JOIN pg_catalog.pg_type AS base "
      "ON base.oid = chain.typbasetype) "
      "SELECT oid, type_modifier FROM domain_chain WHERE typbasetype = 0) "
      "AS resolved_type";
}

std::string catalog_character_length_sql(const std::string& fallback) {
  return "CASE WHEN resolved_type.type_modifier >= 4 THEN "
      "resolved_type.type_modifier - 4 ELSE " + fallback + " END";
}

std::string catalog_character_octet_length_sql(const std::string& fallback) {
  return "CASE WHEN resolved_type.type_modifier >= 4 THEN "
      "(resolved_type.type_modifier - 4) * "
      "pg_catalog.pg_encoding_max_length(pg_catalog.pg_char_to_encoding("
      "current_setting('server_encoding'))) ELSE " + fallback + " END";
}

std::string catalog_numeric_precision_sql(const std::string& fallback) {
  return "CASE WHEN resolved_type.type_modifier >= 4 THEN "
      "((resolved_type.type_modifier - 4) >> 16) & 65535 ELSE " +
      fallback + " END";
}

std::string catalog_numeric_buffer_length_sql(const std::string& fallback) {
  return "CASE WHEN resolved_type.type_modifier >= 4 THEN "
      "(((resolved_type.type_modifier - 4) >> 16) & 65535) + 2 "
      "ELSE " + fallback + " END";
}

std::string catalog_numeric_scale_sql(const std::string& fallback) {
  const std::string encoded_scale =
      "((resolved_type.type_modifier - 4) & 2047)";
  const auto signed_scale = [](const std::string& value) {
    return "CASE WHEN (" + value + ") BETWEEN 1024 AND 2047 "
        "THEN (" + value + ") - 2048 ELSE " + value + " END";
  };
  return std::string("CASE WHEN resolved_type.type_modifier >= 4 THEN ") +
      signed_scale(encoded_scale) + " ELSE " +
      signed_scale(fallback) + " END";
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

std::string build_query(const ColumnsCatalogRequest& request) {
  const auto character_length =
      catalog_character_length_sql("character_maximum_length");
  const auto character_octet_length =
      catalog_character_octet_length_sql("character_octet_length");
  const auto numeric_precision =
      catalog_numeric_precision_sql("numeric_precision");
  const auto numeric_scale = catalog_numeric_scale_sql("numeric_scale");
  std::string query =
      "SELECT table_cat, table_schem, table_name, column_name, data_type, "
      "type_name, column_size, buffer_length, decimal_digits, "
      "num_prec_radix, nullable, remarks, column_def, sql_data_type, "
      "sql_datetime_sub, char_octet_length, ordinal_position, is_nullable "
      "FROM (SELECT current_database()::text AS table_cat, "
      "table_schema::text AS table_schem, table_name::text AS table_name, "
      "column_name::text AS column_name, " +
      catalog_data_type_sql("resolved_type.oid") + " AS data_type, "
      "COALESCE(columns.domain_name, " +
      catalog_type_name_sql("types.oid") + ")::text AS type_name, "
      "CASE resolved_type.oid "
      "WHEN 16 THEN 1 WHEN 17 THEN 1073741824 WHEN 18 THEN 1 "
      "WHEN 20 THEN 19 WHEN 21 THEN 5 WHEN 23 THEN 10 "
      "WHEN 25 THEN 1073741824 WHEN 700 THEN 7 WHEN 701 THEN 15 "
      "WHEN 1042 THEN " + character_length +
      " WHEN 1043 THEN " + character_length +
      " WHEN 1700 THEN " + numeric_precision +
      " WHEN 1082 THEN 10 WHEN 2950 THEN 36 ELSE CASE data_type "
      "WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 10 WHEN 'bigint' THEN 19 "
      "WHEN 'real' THEN 7 WHEN 'double precision' THEN 15 "
      "WHEN 'numeric' THEN numeric_precision "
      "WHEN 'decimal' THEN numeric_precision "
      "WHEN 'character' THEN character_maximum_length "
      "WHEN 'character varying' THEN character_maximum_length "
      "WHEN 'text' THEN 1073741824 WHEN 'bytea' THEN 1073741824 "
      "WHEN 'date' THEN 10 "
      "WHEN 'time without time zone' THEN 8 + "
      "CASE WHEN datetime_precision > 0 THEN 1 + datetime_precision "
      "ELSE 0 END "
      "WHEN 'time with time zone' THEN 14 + "
      "CASE WHEN datetime_precision > 0 THEN 1 + datetime_precision "
      "ELSE 0 END "
      "WHEN 'timestamp without time zone' THEN 19 + "
      "CASE WHEN datetime_precision > 0 THEN 1 + datetime_precision "
      "ELSE 0 END "
      "WHEN 'timestamp with time zone' THEN 25 + "
      "CASE WHEN datetime_precision > 0 THEN 1 + datetime_precision "
      "ELSE 0 END "
      "ELSE character_maximum_length END END::integer AS column_size, "
      "CASE resolved_type.oid "
      "WHEN 16 THEN 1 WHEN 17 THEN 1073741824 WHEN 18 THEN 1 "
      "WHEN 20 THEN 8 WHEN 21 THEN 2 WHEN 23 THEN 4 "
      "WHEN 25 THEN 1073741824 WHEN 700 THEN 4 WHEN 701 THEN 8 "
      "WHEN 1042 THEN " + character_octet_length +
      " WHEN 1043 THEN " + character_octet_length +
      " WHEN 1700 THEN (" + numeric_precision + ") + 2 "
      "WHEN 1082 THEN 6 WHEN 2950 THEN 36 ELSE CASE data_type "
      "WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 2 "
      "WHEN 'integer' THEN 4 WHEN 'bigint' THEN 8 "
      "WHEN 'real' THEN 4 WHEN 'double precision' THEN 8 "
      "WHEN 'numeric' THEN numeric_precision + 2 "
      "WHEN 'decimal' THEN numeric_precision + 2 "
      "WHEN 'character' THEN character_octet_length "
      "WHEN 'character varying' THEN character_octet_length "
      "WHEN 'text' THEN 1073741824 WHEN 'bytea' THEN 1073741824 "
      "WHEN 'date' THEN 6 "
      "WHEN 'time without time zone' THEN 6 "
      "WHEN 'time with time zone' THEN 6 "
      "WHEN 'timestamp without time zone' THEN 16 "
      "WHEN 'timestamp with time zone' THEN 16 ELSE NULL END "
      "END::integer "
      "AS buffer_length, "
      "CASE WHEN resolved_type.oid = 1700 THEN " + numeric_scale +
      " WHEN data_type IN ('numeric', 'decimal') THEN numeric_scale "
      "WHEN data_type IN ('time without time zone', 'time with time zone', "
      "'timestamp without time zone', 'timestamp with time zone') "
      "THEN datetime_precision WHEN resolved_type.oid IN (20, 21, 23) "
      "THEN 0 ELSE NULL END::smallint AS decimal_digits, "
      "CASE WHEN resolved_type.oid IN (20, 21, 23) THEN 10 "
      "WHEN resolved_type.oid IN (700, 701) THEN 2 "
      "WHEN resolved_type.oid = 1700 THEN 10 "
      "WHEN data_type IN ('real', "
      "'double precision', 'numeric', 'decimal') THEN numeric_precision_radix "
      "ELSE NULL END::smallint AS num_prec_radix, "
      "CASE is_nullable WHEN 'YES' THEN 1 ELSE 0 END::smallint AS nullable, "
      "NULL::text AS remarks, column_default::text AS column_def, "
      "CASE WHEN resolved_type.oid IN (1082, 1083, 1266, 1114, 1184) "
      "THEN 9 ELSE " + catalog_data_type_sql("resolved_type.oid") +
      " END::smallint AS sql_data_type, "
      "CASE resolved_type.oid WHEN 1082 THEN 1 "
      "WHEN 1083 THEN 2 WHEN 1266 THEN 2 "
      "WHEN 1114 THEN 3 WHEN 1184 THEN 3 "
      "ELSE NULL END::smallint "
      "AS sql_datetime_sub, "
      "CASE WHEN resolved_type.oid = 2950 THEN 36 "
      "WHEN resolved_type.oid IN (17, 25) THEN 1073741824 "
      "WHEN resolved_type.oid IN (1042, 1043) THEN " +
      character_octet_length +
      " "
      "WHEN data_type IN ('character', 'character varying') "
      "THEN character_octet_length WHEN data_type = 'bytea' "
      "THEN 1073741824 ELSE NULL END::integer AS char_octet_length, "
      "ordinal_position::integer AS ordinal_position, "
      "is_nullable::text AS is_nullable "
      "FROM information_schema.columns AS columns "
      "JOIN pg_catalog.pg_namespace AS type_schemas "
      "ON type_schemas.nspname = columns.udt_schema "
      "JOIN pg_catalog.pg_type AS types "
      "ON types.typnamespace = type_schemas.oid "
      "AND types.typname = columns.udt_name" +
      catalog_base_type_join_sql("types.oid") + ") "
      "AS odbcpp_columns WHERE 1=1";
  if (request.catalog) {
    query += " AND table_cat = " + quote_catalog_literal(*request.catalog);
  }
  if (request.schema) {
    query += " AND table_schem LIKE " + quote_catalog_literal(*request.schema);
  }
  if (request.table) {
    query += " AND table_name LIKE " + quote_catalog_literal(*request.table);
  }
  if (request.column) {
    query += " AND column_name LIKE " + quote_catalog_literal(*request.column);
  }
  query += " ORDER BY table_cat, table_schem, table_name, ordinal_position";
  return query;
}

std::string build_query(const StatisticsCatalogRequest& request) {
  std::string query =
      "SELECT current_database()::text AS table_cat, "
      "namespaces.nspname::text AS table_schem, "
      "tables.relname::text AS table_name, "
      "CASE WHEN indexes.indisunique THEN 0 ELSE 1 END::smallint "
      "AS non_unique, NULL::text AS index_qualifier, "
      "index_names.relname::text AS index_name, "
      "CASE WHEN indexes.indisclustered THEN 1 "
      "WHEN index_methods.amname = 'hash' THEN 2 ELSE 3 END::smallint "
      "AS type, "
      "index_columns.ordinality::smallint AS ordinal_position, "
      "COALESCE(columns.attname::text, pg_get_indexdef("
      "indexes.indexrelid, index_columns.ordinality::integer, false)) "
      "AS column_name, "
      "CASE WHEN index_columns.ordinality <= indexes.indnkeyatts "
      "AND pg_index_column_has_property(indexes.indexrelid, "
      "index_columns.ordinality::integer, 'orderable') THEN "
      "CASE WHEN pg_index_column_has_property(indexes.indexrelid, "
      "index_columns.ordinality::integer, 'desc') THEN 'D' ELSE 'A' END "
      "ELSE NULL END::text AS asc_or_desc, NULL::integer AS cardinality, "
      "index_names.relpages::integer AS pages, "
      "pg_get_expr(indexes.indpred, indexes.indrelid)::text "
      "AS filter_condition FROM pg_catalog.pg_index AS indexes "
      "JOIN pg_catalog.pg_class AS tables "
      "ON tables.oid = indexes.indrelid "
      "JOIN pg_catalog.pg_namespace AS namespaces "
      "ON namespaces.oid = tables.relnamespace "
      "JOIN pg_catalog.pg_class AS index_names "
      "ON index_names.oid = indexes.indexrelid "
      "JOIN pg_catalog.pg_am AS index_methods "
      "ON index_methods.oid = index_names.relam "
      "CROSS JOIN LATERAL unnest(indexes.indkey) WITH ORDINALITY "
      "AS index_columns(attribute_number, ordinality) "
      "LEFT JOIN pg_catalog.pg_attribute AS columns "
      "ON columns.attrelid = tables.oid "
      "AND columns.attnum = index_columns.attribute_number "
      "WHERE indexes.indisvalid AND indexes.indislive "
      "AND tables.relname = " + quote_catalog_literal(request.table);
  if (request.catalog) {
    query += " AND current_database() = " +
        quote_catalog_literal(*request.catalog);
  }
  if (request.schema) {
    query += " AND namespaces.nspname = " +
        quote_catalog_literal(*request.schema);
  }
  if (request.unique_only) query += " AND indexes.indisunique";
  query +=
      " ORDER BY non_unique, type, index_qualifier, index_name, "
      "ordinal_position";
  return query;
}

std::string build_query(const ProceduresCatalogRequest& request) {
  std::string query =
      "SELECT current_database()::text AS procedure_cat, "
      "namespaces.nspname::text AS procedure_schem, "
      "procedures.proname::text AS procedure_name, "
      "CASE WHEN procedures.proargmodes IS NULL THEN procedures.pronargs "
      "ELSE (SELECT count(*) FROM unnest(procedures.proargmodes) AS mode "
      "WHERE mode::text IN ('i', 'b', 'v')) END::smallint "
      "AS num_input_params, "
      "CASE WHEN procedures.proargmodes IS NULL THEN 0 "
      "ELSE (SELECT count(*) FROM unnest(procedures.proargmodes) AS mode "
      "WHERE mode::text IN ('o', 'b', 't')) END::smallint "
      "AS num_output_params, -1::smallint AS num_result_sets, "
      "obj_description(procedures.oid, 'pg_proc')::text AS remarks, "
      "CASE WHEN procedures.prokind = 'p' THEN 1 ELSE 2 END::smallint "
      "AS procedure_type FROM pg_catalog.pg_proc AS procedures "
      "JOIN pg_catalog.pg_namespace AS namespaces "
      "ON namespaces.oid = procedures.pronamespace "
      "WHERE procedures.prokind IN ('f', 'p')";
  if (request.catalog) {
    query += " AND current_database() = " +
        quote_catalog_literal(*request.catalog);
  }
  if (request.schema) {
    query += " AND namespaces.nspname LIKE " +
        quote_catalog_literal(*request.schema);
  }
  if (request.procedure) {
    query += " AND procedures.proname LIKE " +
        quote_catalog_literal(*request.procedure);
  }
  query +=
      " ORDER BY procedure_cat, procedure_schem, procedure_name";
  return query;
}

std::string build_query(const ProcedureColumnsCatalogRequest& request) {
  const std::string base_type_oid =
      "resolved_type.oid";
  const auto character_length = catalog_character_length_sql("0");
  const auto character_octet_length = catalog_character_octet_length_sql("0");
  const auto numeric_precision = catalog_numeric_precision_sql("0");
  const auto numeric_buffer_length = catalog_numeric_buffer_length_sql("0");
  const auto numeric_scale = catalog_numeric_scale_sql("NULL");
  std::string query =
      "WITH routine_columns AS (SELECT current_database()::text "
      "AS procedure_cat, namespaces.nspname::text AS procedure_schem, "
      "procedures.proname::text AS procedure_name, "
      "COALESCE(procedures.proargnames[arguments.ordinality], '')::text "
      "AS column_name, "
      "CASE COALESCE(procedures.proargmodes[arguments.ordinality], 'i') "
      "WHEN 'i' THEN 1 WHEN 'v' THEN 1 WHEN 'b' THEN 2 "
      "WHEN 't' THEN 3 WHEN 'o' THEN 4 ELSE 0 END::smallint "
      "AS column_type, arguments.type_oid::oid AS type_oid, "
      "arguments.ordinality::integer AS ordinal_position "
      "FROM pg_catalog.pg_proc AS procedures "
      "JOIN pg_catalog.pg_namespace AS namespaces "
      "ON namespaces.oid = procedures.pronamespace "
      "CROSS JOIN LATERAL unnest(CASE WHEN procedures.proallargtypes "
      "IS NOT NULL THEN procedures.proallargtypes "
      "ELSE procedures.proargtypes::oid[] END) WITH ORDINALITY "
      "AS arguments(type_oid, ordinality) "
      "WHERE procedures.prokind IN ('f', 'p') UNION ALL "
      "SELECT current_database()::text, namespaces.nspname::text, "
      "procedures.proname::text, ''::text, 5::smallint, "
      "procedures.prorettype, 0::integer "
      "FROM pg_catalog.pg_proc AS procedures "
      "JOIN pg_catalog.pg_namespace AS namespaces "
      "ON namespaces.oid = procedures.pronamespace "
      "WHERE procedures.prokind = 'f' AND procedures.prorettype <> 2278 "
      "AND (procedures.proargmodes IS NULL OR NOT EXISTS "
      "(SELECT 1 FROM unnest(procedures.proargmodes) AS mode "
      "WHERE mode::text IN ('o', 'b', 't')))) "
      "SELECT columns.procedure_cat, columns.procedure_schem, "
      "columns.procedure_name, columns.column_name, columns.column_type, " +
      catalog_data_type_sql(base_type_oid) + " AS data_type, " +
      catalog_type_name_sql("types.oid") + " AS type_name, "
      "CASE " + base_type_oid +
      " WHEN 2950 THEN 36 WHEN 16 THEN 1 WHEN 17 THEN 1073741824 "
      "WHEN 18 THEN 1 WHEN 20 THEN 19 WHEN 21 THEN 5 WHEN 23 THEN 10 "
      "WHEN 25 THEN 1073741824 WHEN 700 THEN 7 WHEN 701 THEN 15 "
      "WHEN 1042 THEN " + character_length +
      " WHEN 1043 THEN " + character_length +
      " WHEN 1082 THEN 10 "
      "WHEN 1083 THEN 15 WHEN 1266 THEN 21 WHEN 1114 THEN 29 "
      "WHEN 1184 THEN 35 WHEN 1700 THEN " + numeric_precision +
      " ELSE 0 END::integer "
      "AS column_size, CASE " + base_type_oid +
      " WHEN 2950 THEN 36 WHEN 16 THEN 1 WHEN 17 THEN 1073741824 "
      "WHEN 18 THEN 1 WHEN 20 THEN 8 WHEN 21 THEN 2 WHEN 23 THEN 4 "
      "WHEN 25 THEN 1073741824 WHEN 700 THEN 4 WHEN 701 THEN 8 "
      "WHEN 1042 THEN " + character_octet_length +
      " WHEN 1043 THEN " + character_octet_length +
      " WHEN 1082 THEN 6 "
      "WHEN 1083 THEN 6 WHEN 1266 THEN 6 WHEN 1114 THEN 16 "
      "WHEN 1184 THEN 16 WHEN 1700 THEN " + numeric_buffer_length +
      " ELSE 0 END::integer "
      "AS buffer_length, CASE " + base_type_oid +
      " WHEN 20 THEN 0 WHEN 21 THEN 0 "
      "WHEN 23 THEN 0 WHEN 700 THEN 6 WHEN 701 THEN 15 "
      "WHEN 1083 THEN 6 WHEN 1266 THEN 6 WHEN 1114 THEN 6 "
      "WHEN 1184 THEN 6 WHEN 1700 THEN " + numeric_scale +
      " ELSE NULL END::smallint AS decimal_digits, "
      "CASE WHEN " + base_type_oid +
      " IN (20, 21, 23, 700, 701, 1700) THEN 10 "
      "ELSE NULL END::smallint AS num_prec_radix, 2::smallint AS nullable, "
      "NULL::text AS remarks, NULL::text AS column_def, "
      "CASE WHEN " + base_type_oid +
      " IN (1082, 1083, 1266, 1114, 1184) THEN 9 ELSE " +
      catalog_data_type_sql(base_type_oid) +
      " END::smallint AS sql_data_type, "
      "CASE " + base_type_oid +
      " WHEN 1082 THEN 1 WHEN 1083 THEN 2 WHEN 1266 THEN 2 "
      "WHEN 1114 THEN 3 WHEN 1184 THEN 3 ELSE NULL END::smallint "
      "AS sql_datetime_sub, CASE " + base_type_oid +
      " WHEN 2950 THEN 36 WHEN 17 THEN 1073741824 "
      "WHEN 18 THEN 1 WHEN 25 THEN 1073741824 "
      "WHEN 1042 THEN " + character_octet_length +
      " WHEN 1043 THEN " + character_octet_length +
      " ELSE NULL END::integer AS char_octet_length, "
      "columns.ordinal_position, ''::text AS is_nullable "
      "FROM routine_columns AS columns JOIN pg_catalog.pg_type AS types "
      "ON types.oid = columns.type_oid" +
      catalog_base_type_join_sql("types.oid") + " WHERE 1=1";
  if (request.catalog) {
    query += " AND columns.procedure_cat = " +
        quote_catalog_literal(*request.catalog);
  }
  if (request.schema) {
    query += " AND columns.procedure_schem LIKE " +
        quote_catalog_literal(*request.schema);
  }
  if (request.procedure) {
    query += " AND columns.procedure_name LIKE " +
        quote_catalog_literal(*request.procedure);
  }
  if (request.column) {
    query += " AND columns.column_name LIKE " +
        quote_catalog_literal(*request.column);
  }
  query +=
      " ORDER BY procedure_cat, procedure_schem, procedure_name, "
      "column_type, ordinal_position";
  return query;
}

std::string build_query(const SpecialColumnsCatalogRequest& request) {
  if (request.identifier == SpecialColumnsCatalogRequest::Identifier::RowVersion ||
      request.scope != SpecialColumnsCatalogRequest::Scope::CurrentRow) {
    return std::string(
        "SELECT NULL::smallint AS scope, ''::text AS column_name, "
        "0::smallint AS data_type, NULL::text AS type_name, "
        "0::integer AS column_size, 0::integer AS buffer_length, "
        "NULL::smallint AS decimal_digits, 2::smallint AS pseudo_column "
        "WHERE FALSE");
  }

  const auto character_length = catalog_character_length_sql(
      "CASE WHEN columns.data_type IN ('character', 'character varying') "
      "THEN columns.character_maximum_length ELSE 0 END");
  const auto character_octet_length = catalog_character_octet_length_sql(
      "CASE WHEN columns.data_type IN ('character', 'character varying') "
      "THEN columns.character_octet_length ELSE 0 END");
  const auto numeric_precision = catalog_numeric_precision_sql(
      "CASE WHEN columns.data_type IN ('numeric', 'decimal') "
      "THEN columns.numeric_precision ELSE 0 END");
  const auto numeric_buffer_length = catalog_numeric_buffer_length_sql(
      "CASE WHEN columns.data_type IN ('numeric', 'decimal') "
      "THEN columns.numeric_precision + 2 ELSE 0 END");
  const auto numeric_scale = catalog_numeric_scale_sql("columns.numeric_scale");

  std::string query =
      "WITH candidate_indexes AS ("
      "SELECT tables.oid AS table_oid, indexes.indexrelid, "
      "row_number() OVER (PARTITION BY tables.oid ORDER BY "
      "indexes.indisprimary DESC, indexes.indnkeyatts, "
      "indexes.indexrelid)::integer AS candidate_rank "
      "FROM pg_catalog.pg_class AS tables "
      "JOIN pg_catalog.pg_namespace AS schemas "
      "ON schemas.oid = tables.relnamespace "
      "JOIN pg_catalog.pg_index AS indexes "
      "ON indexes.indrelid = tables.oid "
      "WHERE tables.relkind IN ('r', 'p') "
      "AND tables.relname = " + quote_catalog_literal(request.table) +
      " AND indexes.indisunique AND indexes.indisvalid "
      "AND indexes.indisready AND indexes.indpred IS NULL "
      "AND indexes.indexprs IS NULL AND indexes.indnkeyatts > 0 "
      "AND NOT EXISTS (SELECT 1 FROM unnest(indexes.indkey) "
      "WITH ORDINALITY AS key_numbers(attnum, ordinal_position) "
      "WHERE key_numbers.ordinal_position <= indexes.indnkeyatts "
      "AND key_numbers.attnum <= 0)";
  if (request.catalog) {
    query += " AND current_database() = " +
        quote_catalog_literal(*request.catalog);
  }
  if (request.schema) {
    query += " AND schemas.nspname = " +
        quote_catalog_literal(*request.schema);
  }
  if (request.require_non_nullable) {
    query +=
        " AND NOT EXISTS (SELECT 1 FROM unnest(indexes.indkey) "
        "WITH ORDINALITY AS key_numbers(attnum, ordinal_position) "
        "JOIN pg_catalog.pg_attribute AS key_attributes "
        "ON key_attributes.attrelid = tables.oid "
        "AND key_attributes.attnum = key_numbers.attnum "
        "WHERE key_numbers.ordinal_position <= indexes.indnkeyatts "
        "AND NOT key_attributes.attnotnull)";
  }
  query +=
      "), chosen_indexes AS ("
      "SELECT table_oid, indexrelid FROM candidate_indexes "
      "WHERE candidate_rank = 1), key_columns AS ("
      "SELECT chosen_indexes.table_oid, "
      "attributes.attname::text AS column_name, "
      "key_numbers.ordinal_position "
      "FROM chosen_indexes "
      "JOIN pg_catalog.pg_index AS indexes "
      "ON indexes.indexrelid = chosen_indexes.indexrelid "
      "CROSS JOIN LATERAL unnest(indexes.indkey) WITH ORDINALITY "
      "AS key_numbers(attnum, ordinal_position) "
      "JOIN pg_catalog.pg_attribute AS attributes "
      "ON attributes.attrelid = chosen_indexes.table_oid "
      "AND attributes.attnum = key_numbers.attnum "
      "WHERE key_numbers.ordinal_position <= indexes.indnkeyatts) "
      "SELECT 0::smallint AS scope, "
      "key_columns.column_name AS column_name, " +
      catalog_data_type_sql("resolved_type.oid") + " AS data_type, "
      "COALESCE(columns.domain_name, " +
      catalog_type_name_sql("types.oid") + ")::text AS type_name, "
      "CASE resolved_type.oid "
      "WHEN 16 THEN 1 WHEN 17 THEN 1073741824 WHEN 18 THEN 1 "
      "WHEN 20 THEN 19 WHEN 21 THEN 5 WHEN 23 THEN 10 "
      "WHEN 25 THEN 1073741824 WHEN 700 THEN 7 WHEN 701 THEN 15 "
      "WHEN 1042 THEN " + character_length +
      " WHEN 1043 THEN " + character_length +
      " WHEN 1700 THEN " + numeric_precision +
      " WHEN 1082 THEN 10 WHEN 2950 THEN 36 ELSE CASE columns.data_type "
      "WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 5 "
      "WHEN 'integer' THEN 10 WHEN 'bigint' THEN 19 WHEN 'real' THEN 7 "
      "WHEN 'double precision' THEN 15 "
      "WHEN 'numeric' THEN columns.numeric_precision "
      "WHEN 'decimal' THEN columns.numeric_precision "
      "WHEN 'character' THEN columns.character_maximum_length "
      "WHEN 'character varying' THEN columns.character_maximum_length "
      "WHEN 'text' THEN 1073741824 WHEN 'bytea' THEN 1073741824 "
      "WHEN 'date' THEN 10 "
      "WHEN 'time without time zone' THEN 8 + "
      "CASE WHEN columns.datetime_precision > 0 "
      "THEN 1 + columns.datetime_precision ELSE 0 END "
      "WHEN 'time with time zone' THEN 14 + "
      "CASE WHEN columns.datetime_precision > 0 "
      "THEN 1 + columns.datetime_precision ELSE 0 END "
      "WHEN 'timestamp without time zone' THEN 19 + "
      "CASE WHEN columns.datetime_precision > 0 "
      "THEN 1 + columns.datetime_precision ELSE 0 END "
      "WHEN 'timestamp with time zone' THEN 25 + "
      "CASE WHEN columns.datetime_precision > 0 "
      "THEN 1 + columns.datetime_precision ELSE 0 END "
      "ELSE 0 END "
      "END::integer AS column_size, "
      "CASE resolved_type.oid "
      "WHEN 16 THEN 1 WHEN 17 THEN 1073741824 WHEN 18 THEN 1 "
      "WHEN 20 THEN 8 WHEN 21 THEN 2 WHEN 23 THEN 4 "
      "WHEN 25 THEN 1073741824 WHEN 700 THEN 4 WHEN 701 THEN 8 "
      "WHEN 1042 THEN " + character_octet_length +
      " WHEN 1043 THEN " + character_octet_length +
      " WHEN 1700 THEN " + numeric_buffer_length +
      " WHEN 1082 THEN 6 WHEN 2950 THEN 36 ELSE CASE columns.data_type "
      "WHEN 'boolean' THEN 1 WHEN 'smallint' THEN 2 WHEN 'integer' THEN 4 "
      "WHEN 'bigint' THEN 8 WHEN 'real' THEN 4 "
      "WHEN 'double precision' THEN 8 "
      "WHEN 'numeric' THEN columns.numeric_precision + 2 "
      "WHEN 'decimal' THEN columns.numeric_precision + 2 "
      "WHEN 'character' THEN columns.character_octet_length "
      "WHEN 'character varying' THEN columns.character_octet_length "
      "WHEN 'text' THEN 1073741824 WHEN 'bytea' THEN 1073741824 "
      "WHEN 'date' THEN 6 WHEN 'time without time zone' THEN 6 "
      "WHEN 'time with time zone' THEN 6 "
      "WHEN 'timestamp without time zone' THEN 16 "
      "WHEN 'timestamp with time zone' THEN 16 ELSE 0 END "
      "END::integer "
      "AS buffer_length, CASE WHEN resolved_type.oid = 1700 THEN " +
      numeric_scale + " WHEN columns.data_type IN "
      "('numeric', 'decimal') THEN columns.numeric_scale "
      "WHEN columns.data_type IN ('time without time zone', "
      "'time with time zone', 'timestamp without time zone', "
      "'timestamp with time zone') THEN columns.datetime_precision "
      "WHEN resolved_type.oid IN (20, 21, 23) THEN 0 "
      "ELSE NULL END::smallint AS decimal_digits, "
      "1::smallint AS pseudo_column "
      "FROM key_columns "
      "JOIN pg_catalog.pg_class AS tables "
      "ON tables.oid = key_columns.table_oid "
      "JOIN pg_catalog.pg_namespace AS schemas "
      "ON schemas.oid = tables.relnamespace "
      "JOIN information_schema.columns AS columns "
      "ON columns.table_catalog = current_database() "
      "AND columns.table_schema = schemas.nspname "
      "AND columns.table_name = tables.relname "
      "AND columns.column_name = key_columns.column_name "
      "JOIN pg_catalog.pg_namespace AS type_schemas "
      "ON type_schemas.nspname = columns.udt_schema "
      "JOIN pg_catalog.pg_type AS types "
      "ON types.typnamespace = type_schemas.oid "
      "AND types.typname = columns.udt_name" +
      catalog_base_type_join_sql("types.oid") + " "
      "ORDER BY key_columns.table_oid, key_columns.ordinal_position";
  return query;
}

} // namespace

rs::util::Result<std::string> PgDatabaseConnection::catalog_query(
    const CatalogRequest& request) const {
  return std::visit([](const auto& catalog) { return build_query(catalog); }, request);
}

} // namespace rs::core::database::postgres
