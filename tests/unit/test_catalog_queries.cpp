#include <gtest/gtest.h>
#include "core/database/database_factory.h"
#include "core/database/generic_database_connection.h"
#include "core/database/postgres/pg_database_connection.h"
#include "core/database/postgres/pg_backend_provider.h"
#include "tests/mock_protocol_parser.h"

using namespace rs::core::database;

TEST(CatalogQueryTest, GenericBackendHasNoCatalogFacetWithoutUnsupportedStubs) {
  GenericDatabaseConnection backend(std::make_unique<odbcpp::test::MockProtocolParser>());
  const IDatabaseConnection& session = backend;
  EXPECT_EQ(nullptr, session.catalog_queries());
  EXPECT_FALSE(session.is_connected());
}

TEST(CatalogQueryTest, TablePatternsPreserveOmittedEmptyAndQuotedValues) {
  auto backend = DatabaseFactory::create_connection();
  TablesCatalogRequest request;
  auto query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_EQ(std::string::npos, query->find(" AND table_cat LIKE"));
  request.catalog = "";
  request.schema = "s'chema";
  request.table = "x\\_%";
  query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find(" AND table_cat LIKE ''"));
  EXPECT_NE(std::string::npos, query->find(" AND table_schem LIKE 's''chema'"));
  EXPECT_NE(std::string::npos, query->find(" AND table_name LIKE 'x\\_%'"));
  EXPECT_FALSE(backend->is_connected());
}

TEST(CatalogQueryTest, TableTypesDistinguishAllNoneAndExplicitList) {
  auto backend = DatabaseFactory::create_connection();
  TablesCatalogRequest request;
  auto query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_EQ(std::string::npos, query->find(" AND table_type IN"));
  EXPECT_EQ(std::string::npos, query->find(" AND FALSE"));
  request.types = std::vector<std::string>{};
  query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find(" AND FALSE"));
  request.types = std::vector<std::string>{"TABLE", "VIEW"};
  query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find(" AND table_type IN ('TABLE','VIEW')"));
}

TEST(CatalogQueryTest, EnumerationModesDoNotApplyOrdinaryFilters) {
  auto backend = DatabaseFactory::create_connection();
  TablesCatalogRequest request;
  request.catalog = "ignored";
  request.schema = "ignored";
  request.table = "ignored";
  request.types = std::vector<std::string>{};
  for (const auto mode : {TablesCatalogRequest::Mode::Catalogs,
       TablesCatalogRequest::Mode::Schemas, TablesCatalogRequest::Mode::TableTypes}) {
    request.mode = mode;
    const auto query = backend->catalog_queries()->catalog_query(request);
    ASSERT_FALSE(query.has_error());
    EXPECT_EQ(std::string::npos, query->find("ignored"));
    EXPECT_EQ(std::string::npos, query->find(" AND FALSE"));
    EXPECT_NE(std::string::npos, query->find("table_cat"));
    EXPECT_NE(std::string::npos, query->find("remarks"));
  }
}

TEST(CatalogQueryTest, PrimaryKeyFiltersUseLiteralNames) {
  auto backend = DatabaseFactory::create_connection();
  const auto query = backend->catalog_queries()->catalog_query(PrimaryKeysCatalogRequest{
      "", "s'%", "t'_%"});
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("keys.table_catalog = ''"));
  EXPECT_NE(std::string::npos, query->find("keys.table_schema = 's''%'"));
  EXPECT_NE(std::string::npos, query->find("keys.table_name = 't''_%'"));
  EXPECT_EQ(std::string::npos, query->find(" LIKE "));
  EXPECT_TRUE(query->ends_with("ORDER BY table_cat, table_schem, table_name, key_seq"));
}

TEST(CatalogQueryTest, ForeignKeyFiltersAndOrderingFollowRequestedSide) {
  auto backend = DatabaseFactory::create_connection();
  ForeignKeysCatalogRequest request;
  request.foreign_table = "f'_%";
  auto query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("fk_tables.relname = 'f''_%'"));
  EXPECT_EQ(std::string::npos, query->find("pk_tables.relname ="));
  EXPECT_TRUE(query->ends_with("ORDER BY pktable_cat, pktable_schem, pktable_name, key_seq"));
  request.primary_table = "p'_%";
  request.primary_catalog = "";
  request.primary_schema = "";
  request.foreign_catalog = "db'";
  request.foreign_schema = "fs'";
  query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("pk_tables.relname = 'p''_%'"));
  EXPECT_NE(std::string::npos, query->find("current_database() = ''"));
  EXPECT_NE(std::string::npos, query->find("pk_namespaces.nspname = ''"));
  EXPECT_NE(std::string::npos, query->find("current_database() = 'db'''"));
  EXPECT_NE(std::string::npos, query->find("fk_namespaces.nspname = 'fs'''"));
  EXPECT_EQ(std::string::npos, query->find(" LIKE "));
  EXPECT_TRUE(query->ends_with("ORDER BY fktable_cat, fktable_schem, fktable_name, key_seq"));
}

TEST(CatalogQueryTest, PostgreSQLColumnsKeepLiteralCatalogAndPatternFilters) {
  auto backend = std::make_unique<postgres::PgDatabaseConnection>();
  const auto all = backend->catalog_queries()->catalog_query(ColumnsCatalogRequest{});
  ASSERT_FALSE(all.has_error());
  EXPECT_EQ(std::string::npos, all->find(" AND table_cat ="));
  const auto query = backend->catalog_queries()->catalog_query(ColumnsCatalogRequest{
      "db'%", "", "t\\_%", "c'%"});
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find(" AND table_cat = 'db''%'"));
  EXPECT_NE(std::string::npos, query->find(" AND table_schem LIKE ''"));
  EXPECT_NE(std::string::npos, query->find(" AND table_name LIKE 't\\_%'"));
  EXPECT_NE(std::string::npos, query->find(" AND column_name LIKE 'c''%'"));
  EXPECT_TRUE(query->ends_with("ORDER BY table_cat, table_schem, table_name, ordinal_position"));
}

TEST(CatalogQueryTest, StatisticsKeepLiteralNamesAndUniqueFilter) {
  auto backend = std::make_unique<postgres::PgDatabaseConnection>();
  StatisticsCatalogRequest request{"", "s'%", "t'_%", false};
  auto query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("current_database() = ''"));
  EXPECT_NE(std::string::npos, query->find("namespaces.nspname = 's''%'"));
  EXPECT_NE(std::string::npos, query->find("tables.relname = 't''_%'"));
  EXPECT_EQ(std::string::npos, query->find(" LIKE "));
  EXPECT_EQ(std::string::npos, query->find(" AND indexes.indisunique"));
  request.unique_only = true;
  query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find(" AND indexes.indisunique"));
  EXPECT_TRUE(query->ends_with("ORDER BY non_unique, type, index_qualifier, index_name, ordinal_position"));
}

TEST(CatalogQueryTest, ProceduresKeepOmittedEmptyAndEscapedPatterns) {
  auto backend = DatabaseFactory::create_connection();
  const auto all = backend->catalog_queries()->catalog_query(ProceduresCatalogRequest{});
  ASSERT_FALSE(all.has_error());
  EXPECT_EQ(std::string::npos, all->find(" AND current_database() ="));
  const auto query = backend->catalog_queries()->catalog_query(ProceduresCatalogRequest{
      "db'%", "", "p'\\_%"});
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("current_database() = 'db''%'"));
  EXPECT_NE(std::string::npos, query->find("namespaces.nspname LIKE ''"));
  EXPECT_NE(std::string::npos, query->find("procedures.proname LIKE 'p''\\_%'"));
  EXPECT_TRUE(query->ends_with("ORDER BY procedure_cat, procedure_schem, procedure_name"));
}

TEST(CatalogQueryTest, ProcedureColumnsKeepLiteralCatalogAndColumnPatterns) {
  auto backend = DatabaseFactory::create_connection();
  const auto all = backend->catalog_queries()->catalog_query(ProcedureColumnsCatalogRequest{});
  ASSERT_FALSE(all.has_error());
  EXPECT_EQ(std::string::npos, all->find(" AND columns.column_name LIKE"));
  const auto query = backend->catalog_queries()->catalog_query(ProcedureColumnsCatalogRequest{
      "db'%", "s\\_%", "p'%", ""});
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("columns.procedure_cat = 'db''%'"));
  EXPECT_NE(std::string::npos, query->find("columns.procedure_schem LIKE 's\\_%'"));
  EXPECT_NE(std::string::npos, query->find("columns.procedure_name LIKE 'p''%'"));
  EXPECT_NE(std::string::npos, query->find("columns.column_name LIKE ''"));
  EXPECT_TRUE(query->ends_with("ORDER BY procedure_cat, procedure_schem, procedure_name, column_type, ordinal_position"));
}

TEST(CatalogQueryTest, SpecialColumnsKeepScopeNullabilityAndLiteralNames) {
  // These assertions describe PostgreSQL's index and row-version contract.
  auto backend = std::make_unique<postgres::PgDatabaseConnection>();
  SpecialColumnsCatalogRequest request;
  request.catalog = "";
  request.schema = "s'%";
  request.table = "t'_%";
  auto query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("current_database() = ''"));
  EXPECT_NE(std::string::npos, query->find("schemas.nspname = 's''%'"));
  EXPECT_NE(std::string::npos, query->find("tables.relname = 't''_%'"));
  EXPECT_EQ(std::string::npos, query->find("AND NOT key_attributes.attnotnull"));
  request.require_non_nullable = true;
  query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("AND NOT key_attributes.attnotnull"));
  for (const auto scope : {SpecialColumnsCatalogRequest::Scope::Transaction,
                          SpecialColumnsCatalogRequest::Scope::Session}) {
    request.scope = scope;
    query = backend->catalog_queries()->catalog_query(request);
    ASSERT_FALSE(query.has_error());
    EXPECT_TRUE(query->ends_with("WHERE FALSE"));
    EXPECT_NE(std::string::npos, query->find("AS pseudo_column"));
    EXPECT_EQ(std::string::npos, query->find("pg_catalog"));
  }
  request.scope = SpecialColumnsCatalogRequest::Scope::CurrentRow;
  request.identifier = SpecialColumnsCatalogRequest::Identifier::RowVersion;
  query = backend->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_TRUE(query->ends_with("WHERE FALSE"));
}

TEST(CatalogQueryTest, ExplicitRedshiftProfileIsolatesColumnsFromPostgresDomainSql) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection pg;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  // Execution is exposed only by the explicit Redshift profile; other catalog
  // requests still use their independently selected query contracts.
  EXPECT_EQ(nullptr, pg.catalog_execution());
  EXPECT_NE(nullptr, redshift.catalog_execution());
  const auto pg_query = pg.catalog_query(ColumnsCatalogRequest{});
  const auto rs_query = redshift.catalog_query(ColumnsCatalogRequest{});
  ASSERT_FALSE(pg_query.has_error());
  ASSERT_FALSE(rs_query.has_error());
  EXPECT_NE(std::string::npos, pg_query->find("WITH RECURSIVE domain_chain"));
  EXPECT_NE(std::string::npos, rs_query->find("FROM svv_columns AS columns"));
  for (const auto* forbidden : {"domain_chain", "LATERAL", "pg_catalog", "udt_name"}) {
    EXPECT_EQ(std::string::npos, rs_query->find(forbidden));
  }
  EXPECT_NE(std::string::npos, rs_query->find("ELSE 0 END::smallint AS data_type"));
  EXPECT_NE(std::string::npos, rs_query->find("WHEN 'NO' THEN 0 ELSE 2 END::smallint AS nullable"));
  EXPECT_NE(std::string::npos, rs_query->find("WHEN 'integer' THEN 4"));
  EXPECT_NE(std::string::npos, rs_query->find("THEN columns.character_maximum_length ELSE NULL END::integer AS char_octet_length"));
  EXPECT_EQ(*pg.catalog_query(TablesCatalogRequest{}), *redshift.catalog_query(TablesCatalogRequest{}));
  EXPECT_FALSE(pg.is_connected());
  EXPECT_FALSE(redshift.is_connected());
}

TEST(CatalogQueryTest, RedshiftColumnsPreserveExactOutputShapeAndPatternSemantics) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  const auto all = redshift.catalog_query(ColumnsCatalogRequest{});
  ASSERT_FALSE(all.has_error());
  EXPECT_TRUE(all->starts_with("SELECT table_cat, table_schem, table_name, column_name, data_type, "
      "type_name, column_size, buffer_length, decimal_digits, num_prec_radix, "
      "nullable, remarks, column_def, sql_data_type, sql_datetime_sub, "
      "char_octet_length, ordinal_position, is_nullable FROM"));
  EXPECT_EQ(std::string::npos, all->find(" AND table_cat ="));
  const auto filtered = redshift.catalog_query(ColumnsCatalogRequest{"db'%", "", "t\\_%", "c'%"});
  ASSERT_FALSE(filtered.has_error());
  EXPECT_NE(std::string::npos, filtered->find(" AND table_cat = 'db''%'"));
  EXPECT_NE(std::string::npos, filtered->find(" AND table_schem LIKE ''"));
  EXPECT_NE(std::string::npos, filtered->find(" AND table_name LIKE 't\\\\_%'"));
  EXPECT_NE(std::string::npos, filtered->find(" AND column_name LIKE 'c''%'"));
  EXPECT_TRUE(filtered->ends_with("ORDER BY table_cat, table_schem, table_name, ordinal_position"));
}

TEST(CatalogQueryTest, RedshiftProviderPropagatesProfileAndEscapesLiteralBackslashes) {
  using namespace rs::core::database::postgres;
  PgBackendProvider provider({"redshift", "Amazon Redshift", "ODBCPP Redshift"},
      {"localhost", 5432, "postgres", true}, std::nullopt, PgCatalogProfile::Redshift);
  auto session = provider.create_session(nullptr);
  ASSERT_NE(nullptr, session->catalog_queries());
  ColumnsCatalogRequest request;
  request.column = "v\\%lue";
  auto query = session->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("FROM svv_columns AS columns"));
  EXPECT_NE(std::string::npos, query->find("column_name LIKE 'v\\\\%lue'"));
  request.column = "v\\_lue";
  query = session->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("column_name LIKE 'v\\\\_lue'"));
  request.catalog = "a\\'b";
  request.column = "x\\\\y";
  query = session->catalog_queries()->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("table_cat = 'a\\\\''b'"));
  EXPECT_NE(std::string::npos, query->find("column_name LIKE 'x\\\\\\\\y'"));
}

TEST(CatalogQueryTest, RedshiftStatisticsAvoidsPostgresIndexesAndPreservesProfileIsolation) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection pg;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  StatisticsCatalogRequest request;
  const auto original = pg.catalog_query(request);
  const auto selected = redshift.catalog_query(request);
  ASSERT_TRUE(original); ASSERT_TRUE(selected);
  EXPECT_NE(std::string::npos, original->find("pg_catalog.pg_index"));
  EXPECT_EQ(std::string::npos, selected->find("pg_catalog"));
  EXPECT_EQ(std::string::npos, selected->find("LATERAL"));
  EXPECT_TRUE(selected->ends_with("WHERE 1=0"));
  size_t previous = 0;
  for (const auto* field : {"VARCHAR(128)) AS table_cat", "VARCHAR(128)) AS table_schem",
      "VARCHAR(128)) AS table_name", "SMALLINT) AS non_unique",
      "VARCHAR(128)) AS index_qualifier", "VARCHAR(128)) AS index_name",
      "SMALLINT) AS type", "SMALLINT) AS ordinal_position", "VARCHAR(128)) AS column_name",
      "VARCHAR(1)) AS asc_or_desc", "INTEGER) AS cardinality", "INTEGER) AS pages",
      "VARCHAR(128)) AS filter_condition"}) {
    const auto position = selected->find(field);
    ASSERT_NE(std::string::npos, position);
    EXPECT_GE(position, previous);
    previous = position + std::string(field).size();
  }
  request.catalog = "other_database";
  request.schema = "quoted'schema";
  request.table = "sample";
  request.unique_only = true;
  EXPECT_EQ(*selected, *redshift.catalog_query(request));
  EXPECT_EQ(*original, *pg.catalog_query(StatisticsCatalogRequest{}));
}

TEST(CatalogQueryTest, RedshiftRowVersionHasModernEmptyContractAcrossValidOptions) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection pg;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  SpecialColumnsCatalogRequest request;
  request.identifier = SpecialColumnsCatalogRequest::Identifier::RowVersion;
  request.catalog = "quoted'database"; request.schema = "quoted'schema";
  request.table = "quoted'table";
  const auto selected = redshift.catalog_query(request);
  ASSERT_TRUE(selected);
  EXPECT_TRUE(selected->ends_with("WHERE 1=0"));
  EXPECT_EQ(std::string::npos, selected->find("quoted"));
  EXPECT_EQ(std::string::npos, selected->find("pg_catalog"));
  size_t previous = 0;
  for (const auto* field : {"SMALLINT) AS scope", "VARCHAR) AS column_name",
      "SMALLINT) AS data_type", "VARCHAR) AS type_name", "INTEGER) AS column_size",
      "INTEGER) AS buffer_length", "SMALLINT) AS decimal_digits",
      "SMALLINT) AS pseudo_column"}) {
    const auto position = selected->find(field);
    ASSERT_NE(std::string::npos, position);
    EXPECT_GE(position, previous); previous = position + std::string(field).size();
  }
  for (const auto scope : {SpecialColumnsCatalogRequest::Scope::CurrentRow,
       SpecialColumnsCatalogRequest::Scope::Transaction,
       SpecialColumnsCatalogRequest::Scope::Session}) {
    for (const bool non_nullable : {false, true}) {
      request.scope = scope; request.require_non_nullable = non_nullable;
      EXPECT_EQ(*selected, *redshift.catalog_query(request));
      const auto original = pg.catalog_query(request);
      ASSERT_TRUE(original);
      EXPECT_NE(std::string::npos, original->find("::text AS column_name"));
      EXPECT_TRUE(original->ends_with("WHERE FALSE"));
    }
  }
}

TEST(CatalogQueryTest, RedshiftColumnsKeepQualifiedDimensionFamiliesAndUnknownFallback) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  PgDatabaseConnection pg;
  const auto query = redshift.catalog_query(ColumnsCatalogRequest{});
  const auto pg_query = pg.catalog_query(ColumnsCatalogRequest{});
  ASSERT_TRUE(query);
  ASSERT_TRUE(pg_query);
  // Literal query boundaries: these assertions do not execute a server CASE or
  // qualify native SVV dimensions. Source qualification prevents alias reuse.
  for (const char* expected : {
      "CASE LOWER(columns.data_type) WHEN 'boolean' THEN -7 WHEN 'smallint' THEN 5",
      "WHEN 'numeric' THEN 2 WHEN 'decimal' THEN 3",
      "ELSE 0 END::smallint AS data_type, columns.data_type::text AS type_name",
      "WHEN 'numeric' THEN columns.numeric_precision WHEN 'decimal' THEN columns.numeric_precision",
      "WHEN 'numeric' THEN columns.numeric_precision + 2 WHEN 'decimal' THEN columns.numeric_precision + 2",
      "CASE WHEN LOWER(columns.data_type) IN ('numeric','decimal') THEN columns.numeric_scale",
      "WHEN LOWER(columns.data_type) IN ('smallint','integer','bigint') THEN 0",
      "THEN columns.datetime_precision ELSE NULL END::smallint AS decimal_digits",
      "THEN 10 WHEN LOWER(columns.data_type) IN ('real','double precision') THEN 2 ELSE NULL END::smallint AS num_prec_radix",
      "WHEN 'character varying' THEN columns.character_maximum_length",
      "THEN columns.character_maximum_length ELSE NULL END::integer AS char_octet_length",
      "WHEN 'date' THEN 6 WHEN 'time without time zone' THEN 6 WHEN 'time with time zone' THEN 6",
      "WHEN 'timestamp without time zone' THEN 16 WHEN 'timestamp with time zone' THEN 16 ELSE NULL END::integer AS buffer_length",
      "ELSE NULL END::integer AS column_size",
      "WHEN 'timestamp with time zone' THEN 3 ELSE NULL END::smallint AS sql_datetime_sub"}) {
    SCOPED_TRACE(expected);
    EXPECT_NE(std::string::npos, query->find(expected));
  }
  for (const char* temporal : {
      "WHEN 'time without time zone' THEN 8 + ",
      "WHEN 'time with time zone' THEN 14 + ",
      "WHEN 'timestamp without time zone' THEN 19 + ",
      "WHEN 'timestamp with time zone' THEN 25 + "}) {
    const std::string expected = std::string(temporal) +
        "CASE WHEN columns.datetime_precision > 0 THEN 1 + columns.datetime_precision ELSE 0 END";
    EXPECT_NE(std::string::npos, query->find(expected));
  }
  EXPECT_EQ(std::string::npos, query->find("CASE data_type"));
  EXPECT_EQ(std::string::npos, query->find("LOWER(data_type)"));
  EXPECT_EQ(std::string::npos, query->find("WHEN 'varbyte'"));
  EXPECT_NE(std::string::npos, pg_query->find("WITH RECURSIVE domain_chain"));
  EXPECT_EQ(std::string::npos, pg_query->find("FROM svv_columns"));
  EXPECT_FALSE(redshift.is_connected());
  EXPECT_FALSE(pg.is_connected());
}

TEST(CatalogQueryTest, RedshiftSchemasUseAccessibleCurrentDatabaseViewAndFiveFieldShape) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  TablesCatalogRequest request;
  request.mode = TablesCatalogRequest::Mode::Schemas;
  const auto query = redshift.catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_EQ(*query,
      "SELECT NULL::text AS table_cat, schema_name::text AS table_schem, "
      "NULL::text AS table_name, NULL::text AS table_type, NULL::text AS "
      "remarks FROM svv_redshift_schemas WHERE database_name = current_database() "
      "ORDER BY table_schem");
  // The view's user visibility supplies the privilege boundary. No owner-only
  // predicate or cross-database expansion replaces it.
  EXPECT_EQ(std::string::npos, query->find("schema_owner"));
  EXPECT_EQ(std::string::npos, query->find("information_schema.schemata"));
  request.catalog = "foreign'database";
  request.schema = "ignored'schema";
  request.table = "ignored'table";
  request.types = std::vector<std::string>{};
  const auto filtered = redshift.catalog_query(request);
  ASSERT_FALSE(filtered.has_error());
  EXPECT_EQ(*query, *filtered);
  EXPECT_FALSE(redshift.is_connected());
}

TEST(CatalogQueryTest, SchemaEnumerationPreservesPostgresSqlAndNonSchemaTableModes) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection postgres(nullptr, std::nullopt, PgCatalogProfile::PostgreSQL);
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  TablesCatalogRequest request;
  request.mode = TablesCatalogRequest::Mode::Schemas;
  const auto schemas = postgres.catalog_query(request);
  ASSERT_FALSE(schemas.has_error());
  EXPECT_EQ(*schemas,
      "SELECT NULL::text AS table_cat, schema_name::text AS table_schem, "
      "NULL::text AS table_name, NULL::text AS table_type, NULL::text AS "
      "remarks FROM information_schema.schemata ORDER BY table_schem");
  request.catalog = "db'";
  request.schema = "s'";
  request.table = "t'";
  request.types = std::vector<std::string>{"TABLE"};
  for (const auto mode : {TablesCatalogRequest::Mode::Tables,
       TablesCatalogRequest::Mode::Catalogs, TablesCatalogRequest::Mode::TableTypes}) {
    request.mode = mode;
    const auto pg = postgres.catalog_query(request);
    const auto rs = redshift.catalog_query(request);
    ASSERT_FALSE(pg.has_error());
    ASSERT_FALSE(rs.has_error());
    EXPECT_EQ(*pg, *rs);
    EXPECT_EQ(std::string::npos, rs->find("svv_redshift_schemas"));
  }
}

TEST(CatalogQueryTest, SchemaEnumerationDoesNotDependOnPrimaryKeyCatalogMode) {
  using namespace rs::core::database::postgres;
  class ModeSession final : public PgDatabaseConnection {
  public:
    explicit ModeSession(RedshiftCatalogMode mode)
        : PgDatabaseConnection(nullptr, std::nullopt, PgCatalogProfile::Redshift), mode_(mode) {}
    RedshiftCatalogMode catalog_mode() const noexcept override { return mode_; }
  private:
    RedshiftCatalogMode mode_;
  };
  ModeSession show(RedshiftCatalogMode::Show);
  ModeSession legacy(RedshiftCatalogMode::Legacy);
  TablesCatalogRequest request;
  request.mode = TablesCatalogRequest::Mode::Schemas;
  const auto a = show.catalog_query(request);
  const auto b = legacy.catalog_query(request);
  ASSERT_FALSE(a.has_error());
  ASSERT_FALSE(b.has_error());
  EXPECT_EQ(*a, *b);
  EXPECT_NE(std::string::npos, a->find("FROM svv_redshift_schemas"));
  EXPECT_EQ(std::string::npos, a->find("SHOW"));
}

TEST(CatalogQueryTest, ShowSchemaSelectionDoesNotAlterGeneratedLegacyQueryContract) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  PgDatabaseConnection postgres(nullptr, std::nullopt, PgCatalogProfile::PostgreSQL);
  TablesCatalogRequest schemas; schemas.mode = TablesCatalogRequest::Mode::Schemas;
  EXPECT_TRUE(redshift.selects_catalog_request(schemas));
  EXPECT_FALSE(postgres.selects_catalog_request(schemas));
  // The generator remains callable and byte-stable for LEGACY; SHOW execution
  // selection belongs to the execution facet, not to this SQL builder.
  const auto sql = redshift.catalog_query(schemas);
  ASSERT_FALSE(sql.has_error());
  EXPECT_NE(std::string::npos, sql->find("FROM svv_redshift_schemas"));
  EXPECT_EQ(std::string::npos, sql->find("SHOW"));
  EXPECT_FALSE(redshift.selects_catalog_request(TablesCatalogRequest{}));
  EXPECT_FALSE(redshift.selects_catalog_request(ColumnsCatalogRequest{}));
}


TEST(CatalogQueryTest, RedshiftUnicodeMetadataPatternsPreserveUtf8AndEscapes) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr, std::nullopt, PgCatalogProfile::Redshift);
  PgDatabaseConnection postgres;
  ColumnsCatalogRequest request{"é表😀'\"\\_%", "é表😀'\"\\_%",
      "é表😀'\"\\_%", "é表😀'\"\\_%"};
  const auto query = redshift.catalog_query(request);
  ASSERT_FALSE(query.has_error());
  const std::string owned = *query;
  EXPECT_NE(std::string::npos, owned.find("FROM svv_columns AS columns"));
  EXPECT_NE(std::string::npos, owned.find(" AND table_cat = 'é表😀''\"\\\\_%'"));
  EXPECT_NE(std::string::npos, owned.find(" AND table_schem LIKE 'é表😀''\"\\\\_%'"));
  EXPECT_NE(std::string::npos, owned.find(" AND table_name LIKE 'é表😀''\"\\\\_%'"));
  EXPECT_NE(std::string::npos, owned.find(" AND column_name LIKE 'é表😀''\"\\\\_%'"));
  EXPECT_TRUE(owned.starts_with("SELECT table_cat, table_schem, table_name, column_name, data_type, "
      "type_name, column_size, buffer_length, decimal_digits, num_prec_radix, "
      "nullable, remarks, column_def, sql_data_type, sql_datetime_sub, "
      "char_octet_length, ordinal_position, is_nullable FROM"));
  EXPECT_TRUE(owned.ends_with("ORDER BY table_cat, table_schem, table_name, ordinal_position"));
  const auto pg_query = postgres.catalog_query(request);
  ASSERT_FALSE(pg_query.has_error());
  EXPECT_EQ(std::string::npos, pg_query->find("FROM svv_columns"));
  request.catalog = "poison"; request.schema.reset(); request.table.reset(); request.column.reset();
  EXPECT_EQ(*query, owned);
  TablesCatalogRequest schemas;
  schemas.mode = TablesCatalogRequest::Mode::Schemas;
  const auto unfiltered = redshift.catalog_query(schemas);
  schemas.catalog = "é表😀'\"\\_%"; schemas.schema = "é表😀'\"\\_%";
  schemas.table = "é表😀'\"\\_%";
  const auto filtered = redshift.catalog_query(schemas);
  ASSERT_FALSE(unfiltered.has_error()); ASSERT_FALSE(filtered.has_error());
  EXPECT_EQ(*unfiltered, *filtered);
  EXPECT_FALSE(redshift.is_connected()); EXPECT_FALSE(postgres.is_connected());
}

TEST(CatalogQueryTest, ShowTablesOnlyLiteralSchemaSelectionPreservesGeneratedQueries) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr,std::nullopt,PgCatalogProfile::Redshift),pg;
  TablesCatalogRequest request;request.schema="literal\\_schema";
  EXPECT_TRUE(redshift.selects_catalog_request(request));EXPECT_FALSE(pg.selects_catalog_request(request));
  const auto generated=redshift.catalog_query(request);ASSERT_FALSE(generated.has_error());
  EXPECT_NE(std::string::npos,generated->find("FROM information_schema.tables"));EXPECT_EQ(std::string::npos,generated->find("SHOW"));
  for(const auto& schema:{"wild%","wild_","trailing\\",""}){request.schema=schema;EXPECT_FALSE(redshift.selects_catalog_request(request));}
  request.schema.reset();EXPECT_FALSE(redshift.selects_catalog_request(request));
  for(auto mode:{TablesCatalogRequest::Mode::Catalogs,TablesCatalogRequest::Mode::Schemas,TablesCatalogRequest::Mode::TableTypes}){
    request.mode=mode;request.schema="literal";EXPECT_EQ(mode==TablesCatalogRequest::Mode::Schemas,redshift.selects_catalog_request(request));
  }
}

TEST(CatalogQueryTest, ShowColumnsLiteralSchemaAndTableKeepOtherGeneratedQueriesStable) {
  using namespace rs::core::database::postgres;
  PgDatabaseConnection redshift(nullptr,std::nullopt,PgCatalogProfile::Redshift),postgres;
  ColumnsCatalogRequest input{std::nullopt,"literal\\_schema","literal\\%table",std::nullopt};
  EXPECT_TRUE(redshift.selects_catalog_request(input));EXPECT_FALSE(postgres.selects_catalog_request(input));
  auto generated=redshift.catalog_query(input);ASSERT_FALSE(generated.has_error());EXPECT_NE(std::string::npos,generated->find("FROM svv_columns AS columns"));EXPECT_EQ(std::string::npos,generated->find("SHOW"));
  for(const auto* pattern:{"wild%","wild_","dangling\\",""}){auto other=input;other.schema=pattern;EXPECT_FALSE(redshift.selects_catalog_request(other));other=input;other.table=pattern;EXPECT_FALSE(redshift.selects_catalog_request(other));}
  input.schema.reset();EXPECT_FALSE(redshift.selects_catalog_request(input));input.schema="literal";input.table.reset();EXPECT_FALSE(redshift.selects_catalog_request(input));
}
