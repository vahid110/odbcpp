#include <gtest/gtest.h>
#include "core/database/database_factory.h"
#include "core/database/generic_database_connection.h"
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

TEST(CatalogQueryTest, ColumnsKeepLiteralCatalogAndPatternFilters) {
  auto backend = DatabaseFactory::create_connection();
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
  auto backend = DatabaseFactory::create_connection();
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
  auto backend = DatabaseFactory::create_connection();
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
