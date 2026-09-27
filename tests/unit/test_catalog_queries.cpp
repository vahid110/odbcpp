#include <gtest/gtest.h>
#include "core/database/database_factory.h"
#include "core/database/generic_database_connection.h"
#include "tests/mock_protocol_parser.h"

using namespace rs::core::database;

TEST(CatalogQueryTest, GenericBackendReportsUnsupportedWithoutConnection) {
  GenericDatabaseConnection backend(std::make_unique<odbcpp::test::MockProtocolParser>());
  for (const CatalogRequest& request : {CatalogRequest{TablesCatalogRequest{}},
       CatalogRequest{PrimaryKeysCatalogRequest{}},
       CatalogRequest{ForeignKeysCatalogRequest{}}}) {
    const auto query = backend.catalog_query(request);
    ASSERT_TRUE(query.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature),
              query.error());
  }
  EXPECT_FALSE(backend.is_connected());
}

TEST(CatalogQueryTest, TablePatternsPreserveOmittedEmptyAndQuotedValues) {
  auto backend = DatabaseFactory::create_connection();
  TablesCatalogRequest request;
  auto query = backend->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_EQ(std::string::npos, query->find(" AND table_cat LIKE"));
  request.catalog = "";
  request.schema = "s'chema";
  request.table = "x\\_%";
  query = backend->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find(" AND table_cat LIKE ''"));
  EXPECT_NE(std::string::npos, query->find(" AND table_schem LIKE 's''chema'"));
  EXPECT_NE(std::string::npos, query->find(" AND table_name LIKE 'x\\_%'"));
  EXPECT_FALSE(backend->is_connected());
}

TEST(CatalogQueryTest, TableTypesDistinguishAllNoneAndExplicitList) {
  auto backend = DatabaseFactory::create_connection();
  TablesCatalogRequest request;
  auto query = backend->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_EQ(std::string::npos, query->find(" AND table_type IN"));
  EXPECT_EQ(std::string::npos, query->find(" AND FALSE"));
  request.types = std::vector<std::string>{};
  query = backend->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find(" AND FALSE"));
  request.types = std::vector<std::string>{"TABLE", "VIEW"};
  query = backend->catalog_query(request);
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
    const auto query = backend->catalog_query(request);
    ASSERT_FALSE(query.has_error());
    EXPECT_EQ(std::string::npos, query->find("ignored"));
    EXPECT_EQ(std::string::npos, query->find(" AND FALSE"));
    EXPECT_NE(std::string::npos, query->find("table_cat"));
    EXPECT_NE(std::string::npos, query->find("remarks"));
  }
}

TEST(CatalogQueryTest, PrimaryKeyFiltersUseLiteralNames) {
  auto backend = DatabaseFactory::create_connection();
  const auto query = backend->catalog_query(PrimaryKeysCatalogRequest{
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
  auto query = backend->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("fk_tables.relname = 'f''_%'"));
  EXPECT_EQ(std::string::npos, query->find("pk_tables.relname ="));
  EXPECT_TRUE(query->ends_with("ORDER BY pktable_cat, pktable_schem, pktable_name, key_seq"));
  request.primary_table = "p'_%";
  request.primary_catalog = "";
  request.primary_schema = "";
  request.foreign_catalog = "db'";
  request.foreign_schema = "fs'";
  query = backend->catalog_query(request);
  ASSERT_FALSE(query.has_error());
  EXPECT_NE(std::string::npos, query->find("pk_tables.relname = 'p''_%'"));
  EXPECT_NE(std::string::npos, query->find("current_database() = ''"));
  EXPECT_NE(std::string::npos, query->find("pk_namespaces.nspname = ''"));
  EXPECT_NE(std::string::npos, query->find("current_database() = 'db'''"));
  EXPECT_NE(std::string::npos, query->find("fk_namespaces.nspname = 'fs'''"));
  EXPECT_EQ(std::string::npos, query->find(" LIKE "));
  EXPECT_TRUE(query->ends_with("ORDER BY fktable_cat, fktable_schem, fktable_name, key_seq"));
}
