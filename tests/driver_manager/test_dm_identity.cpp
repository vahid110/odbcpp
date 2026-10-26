#include "tests/test_dm_expectations.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace odbcpp::test::dm;
constexpr std::string_view kHash =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr QualifiedUnixManager kQualified{
    "2.3.14", "/qualified/libodbc.so", kHash, "Linux", 8, 2, 2, 3};
constexpr ObservedManager kObserved{
    "03.52.0002.0003", "/qualified/libodbc.so", kHash, "Linux", 8, 2, 2, 3};

TEST(DriverManagerIdentity, ExactQualifiedBuildAndProfileMatch) {
  EXPECT_TRUE(qualified_unixodbc2314(kQualified, kObserved));
  auto qualified = kQualified;
  auto observed = kObserved;
  qualified.platform = "Darwin";
  observed.platform = "Darwin";
  EXPECT_TRUE(qualified_unixodbc2314(qualified, observed));
}

TEST(DriverManagerIdentity, SameSqlDmVersionCannotEnrollUnknownOrOtherBuild) {
  EXPECT_FALSE(qualified_unixodbc2314({}, kObserved));
  auto qualified = kQualified;
  qualified.version = "2.3.13";
  EXPECT_FALSE(qualified_unixodbc2314(qualified, kObserved));
  auto observed = kObserved;
  observed.library_sha256 =
      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  EXPECT_FALSE(qualified_unixodbc2314(kQualified, observed));
  observed = kObserved;
  observed.library_path = "/other/libodbc.so";
  EXPECT_FALSE(qualified_unixodbc2314(kQualified, observed));
}

TEST(DriverManagerIdentity, SameSqlDmVersionCannotSelectAnotherAbiOrEnvironment) {
  auto observed = kObserved;
  observed.platform = "Darwin";
  EXPECT_FALSE(qualified_unixodbc2314(kQualified, observed));
  observed = kObserved;
  observed.pointer_bytes = 4;
  EXPECT_FALSE(qualified_unixodbc2314(kQualified, observed));
  observed = kObserved;
  observed.sqlwchar_bytes = 4;
  EXPECT_FALSE(qualified_unixodbc2314(kQualified, observed));
  observed = kObserved;
  observed.driver_sqlwchar_bytes = 4;
  EXPECT_FALSE(qualified_unixodbc2314(kQualified, observed));
  observed = kObserved;
  observed.odbc_version = 2;
  EXPECT_FALSE(qualified_unixodbc2314(kQualified, observed));
}

TEST(DriverManagerIdentity, ActualProviderAndNullBufferRouteShareQualification) {
  // Exercise the diagnostic API as well as the version observer's allocation
  // and SQLGetInfo calls, with no driver registration or database connection.
  SQLHENV environment = SQL_NULL_HENV;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment));
  EXPECT_EQ(SQL_SUCCESS, SQLSetEnvAttr(environment, SQL_ATTR_ODBC_VERSION,
                                      reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
  SQLCHAR state[6]{};
  EXPECT_EQ(SQL_NO_DATA, SQLGetDiagRec(SQL_HANDLE_ENV, environment, 1,
                                      state, nullptr, nullptr, 0, nullptr));
  EXPECT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_ENV, environment));
  const auto observed_path = record_observed_manager_profile();
  ASSERT_FALSE(observed_path.empty());
  const std::filesystem::path observed_file{
      std::u8string(observed_path.begin(), observed_path.end())};
  EXPECT_TRUE(observed_file.is_absolute());
  EXPECT_TRUE(std::filesystem::is_regular_file(observed_file));
  ASSERT_FALSE(manager_version().empty());
  EXPECT_EQ(SQL_OV_ODBC3, manager_odbc_profile().odbc_version);
  EXPECT_TRUE(sizeof(SQLWCHAR) == 2 || sizeof(SQLWCHAR) == 4);
  if (!configured_unix_manager().version.empty()) {
    const auto file = loaded_manager_file_identity();
    EXPECT_EQ(configured_unix_manager().library_path, file.path);
    EXPECT_EQ(configured_unix_manager().library_sha256, file.sha256);
    EXPECT_TRUE(qualified_manager_version(kUnixodbc2314));
    EXPECT_NE(nullptr, entry(Deviation::ManagerOdbcVersion));
    EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
              null_buffer_diag_expectation(Origin::Manager, false).result);
  } else {
    EXPECT_FALSE(qualified_manager_version(kUnixodbc2314));
    if (manager_version() == kUnixodbc2314) {
      EXPECT_EQ(nullptr, entry(Deviation::ManagerOdbcVersion));
      EXPECT_EQ(SQL_SUCCESS,
                null_buffer_diag_expectation(Origin::Manager, false).result);
      EXPECT_EQ(SQL_SUCCESS,
                null_buffer_diag_expectation(Origin::Driver, true).result);
    }
  }
}

TEST(DriverManagerIdentity, ByteComparisonRejectsDifferentBuildAndLength) {
  const auto directory = std::filesystem::temp_directory_path() /
      ("odbcpp-dm-identity-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  ASSERT_TRUE(std::filesystem::create_directory(directory));
  struct Cleanup {
    std::filesystem::path directory;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove(directory / "actual", error);
      std::filesystem::remove(directory / "qualified", error);
      std::filesystem::remove(directory, error);
    }
  } cleanup{directory};
  const auto actual = directory / "actual", qualified = directory / "qualified";
  const auto write = [](const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
  };
  std::string bytes(32769, 'a');
  write(actual, bytes);
  write(qualified, bytes);
  EXPECT_TRUE(same_manager_file_bytes(actual.string(), qualified.string()));
  bytes[16384] = 'b';
  write(actual, bytes);
  EXPECT_FALSE(same_manager_file_bytes(actual.string(), qualified.string()));
  bytes[16384] = 'a';
  bytes.push_back('a');
  write(actual, bytes);
  EXPECT_FALSE(same_manager_file_bytes(actual.string(), qualified.string()));
  write(actual, "");
  EXPECT_FALSE(same_manager_file_bytes(actual.string(), qualified.string()));
  EXPECT_FALSE(same_manager_file_bytes((directory / "missing").string(), qualified.string()));
}
}  // namespace
