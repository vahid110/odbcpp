#include <gtest/gtest.h>

#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "tests/test_handle_helpers.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

class ShutdownProbe final : public rs::odbc::ODBCHandle {
 public:
  ShutdownProbe() : ODBCHandle(rs::odbc::HandleType::Environment) {}

  ~ShutdownProbe() override {
    // Reentry must see a live, empty registry. This also proves that process
    // shutdown releases registered objects instead of leaking the singleton.
    if (rs::odbc::HandleRegistry::instance().has_children(SQL_NULL_HANDLE)) {
      std::abort();
    }
    std::fputs("registry shutdown released objects\n", stderr);
  }
};

bool allocate_unconnected_handles(SQLHENV& environment, SQLHDBC& connection,
                                  SQLHSTMT& statement) {
  if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment) !=
      SQL_SUCCESS) {
    return false;
  }
  if (SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                    reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0) !=
      SQL_SUCCESS) {
    return false;
  }
  if (SQLAllocHandle(SQL_HANDLE_DBC, environment, &connection) != SQL_SUCCESS) {
    return false;
  }
  // The test helper exercises actual statement/implicit-descriptor ownership
  // without connecting to a database or bypassing public API login checks.
  statement = odbcpp::test::make_statement(connection);
  return statement != SQL_NULL_HSTMT;
}

}  // namespace

TEST(HandleRegistryShutdownDeathTest, ProcessExitReleasesLiveStatementDescriptors) {
  ASSERT_EXIT(
      {
        SQLHENV environment = SQL_NULL_HENV;
        SQLHDBC connection = SQL_NULL_HDBC;
        SQLHSTMT statement = SQL_NULL_HSTMT;
        if (!allocate_unconnected_handles(environment, connection, statement)) {
          std::exit(2);
        }
        auto& registry = rs::odbc::HandleRegistry::instance();
        if (registry.child_handles(statement,
                                   rs::odbc::HandleType::Descriptor).size() != 4) {
          std::exit(3);
        }
        auto probe = std::make_unique<ShutdownProbe>();
        const auto probe_handle = reinterpret_cast<SQLHANDLE>(probe.get());
        registry.register_handle(probe_handle, std::move(probe));
        // Exercise C++ static destruction, unlike _Exit or Google's default
        // death-test child exit. Deliberately leave all handles registered.
        std::exit(0);
      },
      ::testing::ExitedWithCode(0), "registry shutdown released objects");
}

TEST(HandleRegistryShutdownTest, ExplicitFreeStillReleasesImplicitDescriptors) {
  SQLHENV environment = SQL_NULL_HENV;
  SQLHDBC connection = SQL_NULL_HDBC;
  SQLHSTMT statement = SQL_NULL_HSTMT;
  ASSERT_TRUE(allocate_unconnected_handles(environment, connection, statement));
  auto& registry = rs::odbc::HandleRegistry::instance();
  const auto descriptors = registry.child_handles(
      statement, rs::odbc::HandleType::Descriptor);
  ASSERT_EQ(4U, descriptors.size());

  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, statement));
  EXPECT_EQ(nullptr, registry.get_handle(statement));
  for (const auto descriptor : descriptors) {
    EXPECT_EQ(nullptr, registry.get_handle(descriptor));
  }
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_DBC, connection));
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
}
