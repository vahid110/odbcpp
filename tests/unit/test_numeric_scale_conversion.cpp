#include <gtest/gtest.h>
#include "core/database/postgres/pg_backend_provider.h"
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "odbcpp/util/scaled_decimal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace {
using namespace rs::core::database;
using rs::util::Deadline;

TEST(ScaledDecimalTest, CallerPoliciesAndOutputBoundsAreIndependentOfOdbc) {
  rs::util::DecimalConversionIssue issue{};
  const auto decimal = rs::util::rescale_decimal("1e200", 1, -200, &issue);
  ASSERT_TRUE(decimal);
  EXPECT_EQ("1", decimal->coefficient);
  EXPECT_EQ(rs::util::DecimalConversionIssue::None, issue);
  EXPECT_EQ("1" + std::string(200, '0'),
      rs::util::format_scaled_decimal(*decimal, 201));
  EXPECT_FALSE(rs::util::format_scaled_decimal(*decimal, 200));
  EXPECT_FALSE(rs::util::rescale_decimal(
      "1", (std::numeric_limits<std::size_t>::max)(), 0, &issue));
  EXPECT_EQ(rs::util::DecimalConversionIssue::OutOfRange, issue);
  EXPECT_FALSE(rs::util::format_scaled_decimal(
      {"1", (std::numeric_limits<int>::min)(), false}, 32));
  EXPECT_FALSE(rs::util::format_scaled_decimal({"1x", 0, false}, 32));
  EXPECT_EQ("-0.0012", rs::util::format_scaled_decimal({"12", 4, true}, 7));
  EXPECT_FALSE(rs::util::format_scaled_decimal({"12", 4, true}, 6));
}

TEST(ScaledDecimalTest, ExponentPositionsDecideLossAndPreserveZero) {
  rs::util::DecimalConversionIssue issue{};
  const auto fraction = rs::util::rescale_decimal("12.045e1", 2, -1, &issue);
  ASSERT_TRUE(fraction);
  EXPECT_EQ("12", fraction->coefficient);
  EXPECT_EQ(rs::util::DecimalConversionIssue::FractionalTruncation, issue);
  EXPECT_FALSE(rs::util::rescale_decimal("12.345e1", 2, -1, &issue));
  EXPECT_EQ(rs::util::DecimalConversionIssue::WholeDigitsLost, issue);
  const auto zero = rs::util::rescale_decimal("-0.00e999999999999999999", 1, -128, &issue);
  ASSERT_TRUE(zero);
  EXPECT_EQ("0", zero->coefficient);
  EXPECT_FALSE(zero->negative);
  EXPECT_EQ(rs::util::DecimalConversionIssue::None, issue);
  EXPECT_EQ("0", rs::util::format_scaled_decimal(*zero, 1));
  for (const auto* invalid : {"1e+", "NaN", "Infinity", "0x1", "1 2", ""}) {
    EXPECT_FALSE(rs::util::rescale_decimal(invalid, 38, -128, &issue));
    EXPECT_EQ(rs::util::DecimalConversionIssue::InvalidCharacter, issue);
  }
}

struct Facts {
  QueryResult result;
  std::vector<QueryParameter> parameters;
  unsigned queries{}, disconnects{};
};

class NumericSession final : public IDatabaseConnection {
 public:
  explicit NumericSession(std::shared_ptr<Facts> facts) : facts_(std::move(facts)) {}
  BackendResult<void> connect(const ConnectionSettings&) override {
    connected_ = true;
    return BackendResult<void>{{SessionState::Idle, SessionDisposition::Reusable}};
  }
  void disconnect() override { connected_ = false; ++facts_->disconnects; }
  bool is_connected() const override { return connected_; }
  SessionState session_state() const override {
    return connected_ ? SessionState::Idle : SessionState::Disconnected;
  }
  std::string server_version() const override { return "8.0"; }
  BackendResult<QueryResult> execute_query(std::string_view, Deadline) override {
    ++facts_->queries;
    return BackendResult<QueryResult>{facts_->result,
        {SessionState::Idle, SessionDisposition::Reusable}};
  }
  BackendResult<QueryResult> execute_prepared(std::string_view,
      std::span<const QueryParameter> values, Deadline deadline) override {
    facts_->parameters.assign(values.begin(), values.end());
    return execute_query({}, deadline);
  }
 private:
  std::shared_ptr<Facts> facts_;
  bool connected_{};
};

class NumericProvider final : public IBackendProvider {
 public:
  explicit NumericProvider(std::shared_ptr<Facts> facts) : facts_(std::move(facts)),
      profile_({"redshift", "Amazon Redshift", "ODBCPP Redshift"},
               {"fixture", 5439, "fixture", false}, std::nullopt,
               postgres::PgCatalogProfile::Redshift) {}
  const BackendIdentity& identity() const noexcept override { return profile_.identity(); }
  const BackendConnectionDefaults& connection_defaults() const noexcept override {
    return profile_.connection_defaults();
  }
  const ISqlDialect& sql_dialect() const noexcept override { return profile_.sql_dialect(); }
  BackendCapabilities capabilities() const noexcept override { return profile_.capabilities(); }
  std::span<const TypeDefinition> type_catalog(std::string_view version = {}) const noexcept override {
    return profile_.type_catalog(version);
  }
  std::span<const TypeDefinition> result_type_catalog(std::string_view version = {}) const noexcept override {
    return profile_.result_type_catalog(version);
  }
  TransactionCapabilities transaction_capabilities() const noexcept override {
    return profile_.transaction_capabilities();
  }
  rs::util::Result<ConnectionSettings> resolve_connection_options(ConnectionOptions options) const override {
    return profile_.resolve_connection_options(std::move(options));
  }
  std::unique_ptr<IDatabaseConnection> create_session(
      std::unique_ptr<rs::core::transport::ITransport>) const override {
    return std::make_unique<NumericSession>(facts_);
  }
 private:
  std::shared_ptr<Facts> facts_;
  postgres::PgBackendProvider profile_;
};

class NumericScaleApiTest : public ::testing::Test {
 protected:
  std::shared_ptr<Facts> facts = std::make_shared<Facts>();
  SQLHENV env{};
  SQLHDBC dbc{};
  SQLHSTMT stmt{};
  void SetUp() override {
    facts->result.columns = {{"n", NativeTypeInfo{ScalarType::Decimal, 10, 2, true}}};
    facts->result.rows = {{"1"}};
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_ENV, nullptr, &env));
    ASSERT_EQ(SQL_SUCCESS, SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION,
        reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0));
    auto connection = std::make_unique<rs::odbc::ODBCConnection>(
        nullptr, std::make_shared<NumericProvider>(facts));
    dbc = reinterpret_cast<SQLHDBC>(connection.get());
    rs::odbc::HandleRegistry::instance().register_handle(dbc, std::move(connection), env);
    ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr,
        (SQLCHAR*)"SERVER=fixture;PORT=5439;DATABASE=fixture;UID=test;SSL=0", SQL_NTS,
        nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  }
  void TearDown() override {
    if (stmt) SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    if (dbc) { SQLDisconnect(dbc); SQLFreeHandle(SQL_HANDLE_DBC, dbc); }
    if (env) SQLFreeHandle(SQL_HANDLE_ENV, env);
  }
  SQLHDESC descriptor(SQLINTEGER attribute) {
    SQLHDESC result{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetStmtAttr(stmt, attribute, &result, 0, nullptr));
    return result;
  }
  void dimensions(SQLHDESC desc, SQLSMALLINT column, SQLSMALLINT precision,
                  SQLSMALLINT scale, void* data = nullptr) {
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(desc, column, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(SQL_C_NUMERIC), 0));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(desc, column, SQL_DESC_PRECISION,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(precision)), 0));
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(desc, column, SQL_DESC_SCALE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(scale)), 0));
    // Dimension edits unbind the pointer; it must be restored last.
    EXPECT_EQ(SQL_SUCCESS, SQLSetDescField(desc, column, SQL_DESC_DATA_PTR, data, 0));
  }
  std::string state() {
    SQLCHAR value[6]{};
    EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1,
        value, nullptr, nullptr, 0, nullptr));
    return reinterpret_cast<char*>(value);
  }
  void execute() { ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS)); }
  void bind_input(void* input, SQLLEN* indicator, SQLSMALLINT sql_type,
                  SQLULEN precision, SQLSMALLINT scale,
                  SQLSMALLINT c_precision, SQLSMALLINT c_scale) {
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_NUMERIC,
        sql_type, precision, scale, input, sizeof(SQL_NUMERIC_STRUCT), indicator));
    dimensions(descriptor(SQL_ATTR_APP_PARAM_DESC), 1, c_precision, c_scale, input);
  }
};

void expect_small_numeric(const SQL_NUMERIC_STRUCT& numeric, SQLSMALLINT precision,
                          SQLSMALLINT scale, unsigned magnitude, unsigned sign) {
  EXPECT_EQ(precision, numeric.precision);
  EXPECT_EQ(scale, numeric.scale);
  EXPECT_EQ(sign, numeric.sign);
  EXPECT_EQ(magnitude, numeric.val[0]);
  EXPECT_TRUE(std::all_of(std::begin(numeric.val) + 1, std::end(numeric.val),
                         [](SQLCHAR byte) { return byte == 0; }));
}

TEST_F(NumericScaleApiTest, ArdGetDataAndExplicitDefaultHaveIndependentPolicies) {
  facts->result.columns.assign(6, {"n", NativeTypeInfo{ScalarType::Decimal, 10, 2, true}});
  facts->result.rows = {{"12000", "-120.45", "-123.45", "123.45", std::nullopt, "42"}};
  ASSERT_NO_FATAL_FAILURE(execute());
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  const auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
  dimensions(ard, 1, 2, -3);
  dimensions(ard, 2, 2, -1);
  dimensions(ard, 3, 5, -1);
  dimensions(ard, 4, 2, -3);
  SQL_NUMERIC_STRUCT output{};
  SQLLEN length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_ARD_TYPE, &output, 0, &length));
  expect_small_numeric(output, 2, -3, 12, 1);
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), length);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 2, SQL_ARD_TYPE, &output, 0, &length));
  EXPECT_EQ("01S07", state());
  expect_small_numeric(output, 2, -1, 12, 0);
  std::memset(&output, 0x5a, sizeof(output));
  const auto original = output;
  length = 73;
  EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 3, SQL_ARD_TYPE, &output, 0, &length));
  EXPECT_EQ("22003", state());
  EXPECT_EQ(73, length);
  EXPECT_EQ(0, std::memcmp(&output, &original, sizeof(output)));
  dimensions(ard, 3, 5, 2);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_ARD_TYPE, &output, 0, &length));
  EXPECT_EQ(0x39, output.val[0]); EXPECT_EQ(0x30, output.val[1]);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 4, SQL_C_NUMERIC, &output, 0, &length));
  EXPECT_EQ("01S07", state());
  expect_small_numeric(output, 38, 0, 123, 1);
  std::memset(&output, 0x5a, sizeof(output));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 5, SQL_C_NUMERIC, &output, 0, &length));
  EXPECT_EQ(SQL_NULL_DATA, length);
  EXPECT_EQ(0, std::memcmp(&output, &original, sizeof(output)));
  SQLBIGINT neighbor{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 6, SQL_C_SBIGINT, &neighbor, 0, &length));
  EXPECT_EQ(42, neighbor);
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, BoundFetchUnalignedOutputStatusNullAndRecovery) {
  facts->result.columns.push_back({"neighbor", NativeTypeInfo{ScalarType::Integer, 10, 0, true}});
  facts->result.rows = {{"12000", "42"}, {"-120.45", "42"}, {"123.45", "42"},
                        {std::nullopt, "42"}, {"120.00", "42"}};
  std::array<unsigned char, sizeof(SQL_NUMERIC_STRUCT) + 2> storage{};
  storage.fill(0x5a);
  void* output = storage.data() + 1;
  SQLLEN length = 73;
  SQLUSMALLINT status = SQL_ROW_NOROW;
  SQLULEN fetched = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_NUMERIC, output, 0, &length));
  const auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
  dimensions(ard, 1, 2, -3, output);
  ASSERT_NO_FATAL_FAILURE(execute());
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  SQL_NUMERIC_STRUCT numeric{};
  std::memcpy(&numeric, output, sizeof(numeric));
  expect_small_numeric(numeric, 2, -3, 12, 1);
  EXPECT_EQ(SQL_ROW_SUCCESS, status); EXPECT_EQ(1u, fetched);
  dimensions(ard, 1, 2, -1, output);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt));
  EXPECT_EQ("01S07", state());
  std::memcpy(&numeric, output, sizeof(numeric));
  expect_small_numeric(numeric, 2, -1, 12, 0);
  EXPECT_EQ(SQL_ROW_SUCCESS_WITH_INFO, status);
  storage.fill(0x5a); length = 73;
  EXPECT_EQ(SQL_ERROR, SQLFetch(stmt));
  EXPECT_EQ("22003", state()); EXPECT_EQ(SQL_ROW_ERROR, status);
  EXPECT_EQ(1u, fetched); EXPECT_EQ(73, length);
  EXPECT_TRUE(std::all_of(storage.begin(), storage.end(), [](unsigned char c) { return c == 0x5a; }));
  SQLINTEGER neighbor{}; SQLLEN neighbor_length{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_SLONG, &neighbor, 0, &neighbor_length));
  EXPECT_EQ(42, neighbor);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(SQL_ROW_SUCCESS, status);
  EXPECT_TRUE(std::all_of(storage.begin(), storage.end(), [](unsigned char c) { return c == 0x5a; }));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  std::memcpy(&numeric, output, sizeof(numeric));
  expect_small_numeric(numeric, 2, -1, 12, 1);
  EXPECT_EQ(0x5a, storage.front()); EXPECT_EQ(0x5a, storage.back());
}

TEST_F(NumericScaleApiTest, IndicatorOnlyNumericBindingStillValidatesScaleAndNull) {
  facts->result.rows = {{"12000"}, {std::nullopt}, {"12300"}, {"12000"}};
  SQLLEN length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_NUMERIC, nullptr, 0, &length));
  const auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
  dimensions(ard, 1, 2, -3);
  ASSERT_NO_FATAL_FAILURE(execute());
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_NUMERIC_STRUCT)), length);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt)); EXPECT_EQ(SQL_NULL_DATA, length);
  length = 73;
  EXPECT_EQ(SQL_ERROR, SQLFetch(stmt)); EXPECT_EQ("22003", state()); EXPECT_EQ(73, length);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(SQL_NUMERIC_STRUCT)), length);
}

TEST_F(NumericScaleApiTest, ApdScaleInterpretsOwningUnalignedInputBeforeSqlTargetLimits) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQL_NUMERIC_STRUCT input{};
  input.precision = 1; input.scale = 0; input.sign = 1; input.val[0] = 12;
  std::array<unsigned char, sizeof(input) + 2> storage{};
  storage.fill(0x5a);
  std::memcpy(storage.data() + 1, &input, sizeof(input));
  SQLLEN length = sizeof(input);
  ASSERT_NO_FATAL_FAILURE(bind_input(storage.data() + 1, &length, SQL_NUMERIC, 5, 0, 2, -3));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(QueryParameterType::Numeric, facts->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"12000"}, facts->parameters[0].value);
  std::fill(storage.begin() + 1, storage.end() - 1, 0);
  EXPECT_EQ(std::optional<std::string>{"12000"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  input.sign = 0;
  std::memcpy(storage.data() + 1, &input, sizeof(input));
  ASSERT_NO_FATAL_FAILURE(bind_input(storage.data() + 1, &length, SQL_INTEGER, 10, 0, 2, -3));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(QueryParameterType::Int32, facts->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"-12000"}, facts->parameters[0].value);
  EXPECT_EQ(0x5a, storage.front()); EXPECT_EQ(0x5a, storage.back());
  EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), length);
}

TEST_F(NumericScaleApiTest, ValidNegativeCScaleCanStillOverflowSqlTargetAndRecover) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQL_NUMERIC_STRUCT input{};
  input.sign = 1; input.val[0] = 0x39; input.val[1] = 0x30;
  const auto original = input;
  SQLLEN length = sizeof(input);
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  SQLULEN processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_NUMERIC, 5, 2, 5, -1));
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
  EXPECT_EQ(0u, facts->queries); EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
  EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_NUMERIC, 6, 0, 5, -1));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"123450"}, facts->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  input.sign = 2; length = SQL_NULL_DATA;
  const auto poison = input;
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_NUMERIC, 6, 0, 5, -129));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_FALSE(facts->parameters[0].value);
  EXPECT_EQ(SQL_NULL_DATA, length);
  EXPECT_EQ(0, std::memcmp(&input, &poison, sizeof(input)));
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, SiblingResultsOwnValuesAndRecoverIndependently) {
  SQLHSTMT sibling{};
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling));
  struct SiblingGuard {
    SQLHSTMT handle;
    ~SiblingGuard() { SQLFreeHandle(SQL_HANDLE_STMT, handle); }
  } guard{sibling};
  SQL_NUMERIC_STRUCT first{}, second{};
  SQLLEN first_length = 73, second_length = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_NUMERIC, &first, 0, &first_length));
  dimensions(descriptor(SQL_ATTR_APP_ROW_DESC), 1, 2, -3, &first);
  SQLHDESC sibling_ard{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(sibling, SQL_ATTR_APP_ROW_DESC,
      &sibling_ard, 0, nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(sibling, 1, SQL_C_NUMERIC, &second, 0, &second_length));
  dimensions(sibling_ard, 1, 1, -5, &second);
  facts->result.rows = {{"12000"}, {"12300"}, {"12000"}};
  ASSERT_NO_FATAL_FAILURE(execute());
  facts->result.rows = {{"900000"}, {std::nullopt}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling, (SQLCHAR*)"SELECT n", SQL_NTS));
  facts->result.rows = {{"7"}};  // Neither cursor may borrow this mutable provider storage.
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  expect_small_numeric(first, 2, -3, 12, 1);
  std::memset(&first, 0x5a, sizeof(first));
  const auto first_poison = first;
  first_length = 73;
  ASSERT_EQ(SQL_ERROR, SQLFetch(stmt));
  EXPECT_EQ("22003", state());
  EXPECT_EQ(73, first_length);
  EXPECT_EQ(0, std::memcmp(&first, &first_poison, sizeof(first)));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling));
  expect_small_numeric(second, 1, -5, 9, 1);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  expect_small_numeric(first, 2, -3, 12, 1);
  std::memset(&second, 0x5a, sizeof(second));
  const auto second_poison = second;
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling));
  EXPECT_EQ(SQL_NULL_DATA, second_length);
  EXPECT_EQ(0, std::memcmp(&second, &second_poison, sizeof(second)));
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, BitInputPairRefusalKeepsSiblingSnapshotAndMaskCanaries) {
  SQLHSTMT sibling{};
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling));
  struct Owner { SQLHSTMT handle; ~Owner() { SQLFreeHandle(SQL_HANDLE_STMT, handle); } } owner{sibling};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling, (SQLCHAR*)"SELECT n", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLCHAR bytes[]{0xa5, 1, 0x5a}; SQLLEN length = 0;
  char text[] = "2030-01-02";
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_TYPE_DATE, 10, 0, text, sizeof(text), &length));
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_BIT), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, bytes + 1, 0));
  struct Status { SQLUSMALLINT before{81}, value{71}, after{82}; } status;
  struct Progress { SQLULEN before{83}, value{91}, after{84}; } progress;
  SQLUSMALLINT operation = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status.value, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &progress.value, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_OPERATION_PTR, &operation, 0));
  const auto queries = facts->queries;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY024", state());
  EXPECT_EQ(71, status.value); EXPECT_EQ(91u, progress.value);
  operation = SQL_PARAM_PROCEED;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("07006", state());
  EXPECT_EQ(SQL_PARAM_ERROR, status.value); EXPECT_EQ(1u, progress.value);
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(0u, facts->disconnects);
  EXPECT_EQ(81, status.before); EXPECT_EQ(82, status.after);
  EXPECT_EQ(83u, progress.before); EXPECT_EQ(84u, progress.after);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BIT,
      SQL_INTEGER, 10, 0, bytes + 1, 0, &length));
  facts->result.rows = {{"poison"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"1"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling));
  SQLINTEGER output = 71; SQLLEN output_length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(sibling, 1, SQL_C_SLONG, &output, 0, &output_length));
  EXPECT_EQ(1, output); EXPECT_EQ(sizeof(output), output_length);
  EXPECT_EQ(0xa5, bytes[0]); EXPECT_EQ(1, bytes[1]); EXPECT_EQ(0x5a, bytes[2]);
}

}  // namespace

namespace {
TEST_F(NumericScaleApiTest, FloatingBinaryResultLiteralWidthsRoundingSpecialsRetryAndNull) {
  for (const auto type : {ScalarType::Real, ScalarType::Double}) {
    const bool real = type == ScalarType::Real;
    const SQLLEN width = real ? 4 : 8;
    SCOPED_TRACE(width);
    const SQLREAL expected_real[] = {1.5f, -42.5f, 0.0f, -0.0f,
        (std::numeric_limits<SQLREAL>::max)(), -(std::numeric_limits<SQLREAL>::max)(),
        std::numeric_limits<SQLREAL>::denorm_min(), -std::numeric_limits<SQLREAL>::denorm_min(),
        1.0f, 0x1.000002p+0f, std::numeric_limits<SQLREAL>::infinity(),
        -std::numeric_limits<SQLREAL>::infinity()};
    const SQLDOUBLE expected_double[] = {1.5, -42.5, 0.0, -0.0,
        (std::numeric_limits<SQLDOUBLE>::max)(), -(std::numeric_limits<SQLDOUBLE>::max)(),
        std::numeric_limits<SQLDOUBLE>::denorm_min(), -std::numeric_limits<SQLDOUBLE>::denorm_min(),
        1.0, 0x1.0000000000001p+0, std::numeric_limits<SQLDOUBLE>::infinity(),
        -std::numeric_limits<SQLDOUBLE>::infinity()};
    const std::vector<std::optional<std::string>> texts = {"1.5", "-42.5", "0", "-0",
        real ? "3.4028234663852886e38" : "1.7976931348623157e308",
        real ? "-3.4028234663852886e38" : "-1.7976931348623157e308",
        real ? "1.401298464324817e-45" : "4.9406564584124654e-324",
        real ? "-1.401298464324817e-45" : "-4.9406564584124654e-324",
        real ? "1.000000059604644775390625" : "1.00000000000000011102230246251565404236316680908203125",
        real ? "1.000000059604644775390626" : "1.00000000000000011102230246251565404236316680908203126",
        "Infinity", "-Infinity", "NaN", std::nullopt};
    facts->result.columns = {{"n", NativeTypeInfo{type, real ? 7u : 15u, 0, true}}};
    facts->result.rows.clear();
    for (const auto& text : texts) facts->result.rows.push_back({text});
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS));
    const auto queries = facts->queries;
    for (std::size_t row = 0; row < texts.size(); ++row) {
      SCOPED_TRACE(row);
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      std::array<unsigned char, 12> output; output.fill(0xa5); const auto before = output;
      SQLLEN length = 79;
      if (texts[row]) {
        for (SQLLEN capacity = 0; capacity < width; ++capacity) {
          SCOPED_TRACE(capacity);
          ASSERT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_BINARY, output.data() + 1, capacity, &length));
          EXPECT_EQ("22003", state()); EXPECT_EQ(before, output); EXPECT_EQ(79, length);
        }
      }
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY,
          texts[row] ? output.data() + 1 : reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)()),
          texts[row] ? width + 1 : 0, &length));
      if (!texts[row]) { EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(before, output); }
      else {
        EXPECT_EQ(width, length);
        if (row == 12) {
          // Owning canonical NaN, not an assertion about a backend wire payload.
          if (real) { SQLREAL value{}; std::memcpy(&value, output.data() + 1, 4); EXPECT_TRUE(std::isnan(value)); }
          else { SQLDOUBLE value{}; std::memcpy(&value, output.data() + 1, 8); EXPECT_TRUE(std::isnan(value)); }
        } else if (real) EXPECT_EQ(0, std::memcmp(&expected_real[row], output.data() + 1, 4));
        else EXPECT_EQ(0, std::memcmp(&expected_double[row], output.data() + 1, 8));
        EXPECT_EQ(0xa5, output.front());
        for (std::size_t byte = width + 1; byte < output.size(); ++byte) EXPECT_EQ(0xa5, output[byte]);
      }
      const auto completed = output; length = 83;
      ASSERT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, SQL_C_BINARY, output.data() + 1, width, &length));
      EXPECT_EQ(completed, output); EXPECT_EQ(83, length);
    }
    EXPECT_EQ(queries, facts->queries);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, FloatingBinaryResultBoundIndicatorOnlyValueValidationAndRecovery) {
  for (const auto type : {ScalarType::Real, ScalarType::Double}) {
    const bool real = type == ScalarType::Real; const SQLLEN width = real ? 4 : 8;
    SCOPED_TRACE(width);
    facts->result.columns = {{"n", NativeTypeInfo{type, real ? 7u : 15u, 0, true}}};
    struct Status { SQLUSMALLINT before{81}, value{71}, after{82}; } status;
    struct Count { SQLULEN before{83}, value{91}, after{84}; } count;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &status.value, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &count.value, 0));
    for (bool indicator_only : {false, true}) {
      const std::vector<std::optional<std::string>> texts = {"-42.5", "-42.5", "bad",
          real ? "1e40" : "1e400", real ? "-1e40" : "-1e400",
          real ? "1e-100" : "1e-1000", real ? "-1e-100" : "-1e-1000", std::nullopt, "-0"};
      for (std::size_t scenario = 0; scenario < texts.size(); ++scenario) {
        SCOPED_TRACE(scenario);
        SCOPED_TRACE(indicator_only);
        facts->result.rows = {{texts[scenario]}};
        std::array<unsigned char, 12> output; output.fill(0xa5); const auto before = output;
        SQLLEN length = 79;
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_BINARY,
            indicator_only ? nullptr : output.data() + 1, scenario == 0 ? width - 1 : width + 1, &length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS));
        const bool failure = (scenario >= 2 && scenario <= 6) || (scenario == 0 && !indicator_only);
        ASSERT_EQ(failure ? SQL_ERROR : SQL_SUCCESS, SQLFetch(stmt));
        EXPECT_EQ(1u, count.value); EXPECT_EQ(failure ? SQL_ROW_ERROR : SQL_ROW_SUCCESS, status.value);
        if (failure) {
          EXPECT_EQ(scenario == 2 ? "22018" : "22003", state()); EXPECT_EQ(before, output); EXPECT_EQ(79, length);
        } else if (!texts[scenario]) { EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(before, output); }
        else {
          EXPECT_EQ(width, length);
          if (indicator_only) EXPECT_EQ(before, output);
          else if (real) { const SQLREAL expected = scenario == 8 ? -0.0f : -42.5f; EXPECT_EQ(0, std::memcmp(&expected, output.data() + 1, 4)); }
          else { const SQLDOUBLE expected = scenario == 8 ? -0.0 : -42.5; EXPECT_EQ(0, std::memcmp(&expected, output.data() + 1, 8)); }
          EXPECT_EQ(0xa5, output.front());
          for (std::size_t byte = width + 1; byte < output.size(); ++byte) EXPECT_EQ(0xa5, output[byte]);
        }
        EXPECT_EQ(81, status.before); EXPECT_EQ(82, status.after); EXPECT_EQ(83u, count.before); EXPECT_EQ(84u, count.after);
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
      }
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0));
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, FloatingBinaryResultGetDataErrorsLeaveRetryAndFollowingRowUsable) {
  for (const auto type : {ScalarType::Real, ScalarType::Double}) {
    const bool real = type == ScalarType::Real; const SQLLEN width = real ? 4 : 8;
    facts->result.columns = {{"n", NativeTypeInfo{type, real ? 7u : 15u, 0, true}}};
    for (const auto& bad : std::vector<std::string>{"bad", real ? "1e40" : "1e400",
             real ? "-1e40" : "-1e400", real ? "1e-100" : "1e-1000", real ? "-1e-100" : "-1e-1000"}) {
      SCOPED_TRACE(bad);
      facts->result.rows = {{bad}, {"1.5"}};
      ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS));
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      std::array<unsigned char, 12> output; output.fill(0xa5); const auto before = output; SQLLEN length = 79;
      for (SQLLEN capacity : {SQLLEN(0), width}) {
        ASSERT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_BINARY, output.data() + 1, capacity, &length));
        EXPECT_EQ(bad == "bad" ? "22018" : "22003", state()); EXPECT_EQ(before, output); EXPECT_EQ(79, length);
      }
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY, output.data() + 1, width, &length));
      EXPECT_EQ(width, length);
      if (real) { const SQLREAL expected = 1.5f; EXPECT_EQ(0, std::memcmp(&expected, output.data() + 1, 4)); }
      else { const SQLDOUBLE expected = 1.5; EXPECT_EQ(0, std::memcmp(&expected, output.data() + 1, 8)); }
      ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    }
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, FloatingBinaryResultArdNullPrecedencePreparedOwnershipAndRefill) {
  facts->result.columns = {{"real", NativeTypeInfo{ScalarType::Real, 7, 0, true}},
      {"double", NativeTypeInfo{ScalarType::Double, 15, 0, true}}};
  facts->result.normalized_parameter_types = {{ScalarType::Real, 7, 0, true}};
  facts->result.rows = {{"-42.5", "1.5"}, {std::nullopt, std::nullopt}};
  SQLHSTMT sibling{}; ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling));
  struct Owner { SQLHSTMT h; ~Owner() { SQLFreeHandle(SQL_HANDLE_STMT, h); } } owner{sibling};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling, (SQLCHAR*)"SELECT n", SQL_NTS));
  auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
  for (SQLSMALLINT col = 1; col <= 2; ++col)
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, col, SQL_DESC_CONCISE_TYPE, (SQLPOINTER)SQL_C_BINARY, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLREAL input = 1.5f; SQLLEN input_length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_FLOAT, SQL_REAL, 7, 0, &input, 0, &input_length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); input = 999;
  ASSERT_EQ(1u, facts->parameters.size()); EXPECT_EQ(std::optional<std::string>{"1.5"}, facts->parameters[0].value);
  facts->result.rows = {{"7.5", "9.25"}}; const auto queries = facts->queries;
  for (auto handle : {stmt, sibling}) {
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(handle));
    for (SQLUSMALLINT col = 1; col <= 2; ++col) {
      const SQLLEN width = col == 1 ? 4 : 8; const auto target = handle == stmt ? SQL_ARD_TYPE : SQL_C_BINARY;
      std::array<unsigned char, 12> output; output.fill(0xa5); const auto before = output; SQLLEN length = 79;
      ASSERT_EQ(SQL_ERROR, SQLGetData(handle, col, target, output.data() + 1, -1, &length));
      SQLCHAR diagnostic[6]{}; ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, handle, 1, diagnostic, nullptr, nullptr, 0, nullptr));
      EXPECT_STREQ("HY090", reinterpret_cast<char*>(diagnostic)); EXPECT_EQ(before, output); EXPECT_EQ(79, length);
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(handle, col, target, output.data() + 1, width, &length));
      EXPECT_EQ(width, length);
      if (col == 1) { const SQLREAL expected = -42.5f; EXPECT_EQ(0, std::memcmp(&expected, output.data() + 1, 4)); }
      else { const SQLDOUBLE expected = 1.5; EXPECT_EQ(0, std::memcmp(&expected, output.data() + 1, 8)); }
      const auto completed = output; length = 83;
      ASSERT_EQ(SQL_NO_DATA, SQLGetData(handle, col, target, output.data() + 1, width, &length));
      EXPECT_EQ(completed, output); EXPECT_EQ(83, length);
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(handle));
    for (SQLUSMALLINT col = 1; col <= 2; ++col) {
      const auto target = handle == stmt ? SQL_ARD_TYPE : SQL_C_BINARY; SQLLEN length = 79;
      ASSERT_EQ(SQL_ERROR, SQLGetData(handle, col, target, nullptr, 0, &length));
      SQLCHAR diagnostic[6]{}; ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, handle, 1, diagnostic, nullptr, nullptr, 0, nullptr));
      EXPECT_STREQ("HY009", reinterpret_cast<char*>(diagnostic));
      unsigned char untouched = 0xa5;
      ASSERT_EQ(SQL_ERROR, SQLGetData(handle, col, target, &untouched, 0, nullptr));
      ASSERT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, handle, 1, diagnostic, nullptr, nullptr, 0, nullptr));
      EXPECT_STREQ("22002", reinterpret_cast<char*>(diagnostic)); EXPECT_EQ(0xa5, untouched);
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(handle, col, target,
          reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)()), 0, &length));
      EXPECT_EQ(SQL_NULL_DATA, length);
    }
  }
  EXPECT_EQ(queries, facts->queries);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE)); input = -1.5f;
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  SQLREAL real{}; SQLDOUBLE dbl{}; SQLLEN real_length = 79, double_length = 81;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_DEFAULT, &real, 0, &real_length));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_DEFAULT, &dbl, 0, &double_length));
  EXPECT_EQ(7.5f, real); EXPECT_EQ(9.25, dbl); EXPECT_EQ(4, real_length); EXPECT_EQ(8, double_length);
  EXPECT_EQ(0u, facts->disconnects);
}
}  // namespace

namespace {
// Independent small base-256 inputs, without the production decimal parser.
SQL_NUMERIC_STRUCT target_rescale_input(unsigned coefficient, SQLCHAR sign = 1) {
  SQL_NUMERIC_STRUCT value{};
  value.precision = 1; value.scale = 0; value.sign = sign;
  value.val[0] = static_cast<SQLCHAR>(coefficient & 0xffu);
  value.val[1] = static_cast<SQLCHAR>(coefficient >> 8);
  return value;
}

TEST_F(NumericScaleApiTest, KnownNumericTargetsTruncateFractionTowardZeroAcrossAliases) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  struct Case {
    unsigned coefficient; SQLCHAR sign;
    SQLSMALLINT c_precision, c_scale, sql_precision, sql_scale;
    const char* expected;
  };
  const Case cases[] = {
      {12345, 1, 5, 3, 4, 2, "12.34"},
      {12345, 0, 5, 3, 4, 2, "-12.34"},
      {12300, 1, 5, 4, 3, 2, "1.2300"},
      {1, 1, 3, 3, 1, 1, "0.0"},
      {1, 0, 3, 3, 1, 1, "0.0"},
      {0, 0, 3, 3, 1, 1, "0.000"},
      {12, 1, 2, -3, 5, 0, "12000"},
      {1230, 1, 4, 2, 5, 3, "12.30"},
      {1230, 0, 4, 2, 5, 3, "-12.30"}};
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  SQLULEN processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  for (const SQLSMALLINT alias : {SQLSMALLINT{SQL_NUMERIC}, SQLSMALLINT{SQL_DECIMAL}}) {
    for (const auto& item : cases) {
      SCOPED_TRACE(alias);
      SCOPED_TRACE(item.expected);
      auto input = target_rescale_input(item.coefficient, item.sign);
      const auto original = input;
      SQLLEN length = sizeof(input);
      ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, alias,
          item.sql_precision, item.sql_scale, item.c_precision, item.c_scale));
      const auto queries = facts->queries;
      ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));  // Input fractional warning is optional; selected policy is success.
      ASSERT_EQ(1u, facts->parameters.size());
      EXPECT_EQ(QueryParameterType::Numeric, facts->parameters[0].type);
      EXPECT_FALSE(facts->parameters[0].binary_input);
      EXPECT_EQ(std::optional<std::string>{item.expected}, facts->parameters[0].value);
      EXPECT_EQ(queries + 1, facts->queries);
      EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
      EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), length);
      EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
      SQLCHAR diagnostic[6]{};
      EXPECT_EQ(SQL_NO_DATA, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1,
          diagnostic, nullptr, nullptr, 0, nullptr));
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    }
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, TargetRescaleRetainsWholeAndCRepresentationRefusalsBeforeDispatch) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto input = target_rescale_input(12345);
  SQLLEN length = sizeof(input);
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  SQLULEN processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_NUMERIC, 4, 2, 5, 3));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  ASSERT_EQ(std::optional<std::string>{"12.34"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  struct Case {
    unsigned coefficient; SQLCHAR sign;
    SQLSMALLINT c_precision, c_scale, sql_precision, sql_scale;
  };
  const Case refused[] = {
      {12345, 1, 5, 2, 4, 2},  // Whole123 cannot fit two SQL whole digits.
      {12, 1, 2, -3, 4, 0},    // Valid C12000 still exceeds SQL capacity.
      {12345, 1, 0, 3, 4, 2},
      {12345, 1, 39, 3, 4, 2},
      {12345, 1, 2, 3, 4, 2},
      {12345, 1, 5, -129, 4, 2},
      {12345, 2, 5, 3, 4, 2},
      {100, 1, 2, 0, 5, 0}};   // Coefficient violates C precision before target conversion.
  for (const auto& item : refused) {
    SCOPED_TRACE(item.coefficient);
      SCOPED_TRACE(item.c_precision);
      SCOPED_TRACE(item.c_scale);
    input = target_rescale_input(item.coefficient, item.sign);
    const auto original = input;
    length = sizeof(input); status = SQL_PARAM_UNUSED; processed = 99;
    ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_NUMERIC,
        item.sql_precision, item.sql_scale, item.c_precision, item.c_scale));
    const auto queries = facts->queries;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
    EXPECT_EQ(queries, facts->queries);
    ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(std::optional<std::string>{"12.34"}, facts->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(input)), length);
    EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
  }
  input = target_rescale_input(23456);
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_DECIMAL, 4, 2, 5, 3));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"23.45"}, facts->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, RescalePreservesDescriptorAuthoritiesUnalignedOwnershipAndNullPoison) {
  facts->result.normalized_parameter_types = {NativeTypeInfo{ScalarType::Decimal, 4, 2, true}};
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto input = target_rescale_input(12345);  // Structp1/s0 disagrees with captured APDp5/s3.
  std::array<unsigned char, sizeof(input) + 2> storage{};
  storage.fill(0x5a); std::memcpy(storage.data() + 1, &input, sizeof(input));
  SQLLEN length = 99;  // Fixed C numeric indicator length is ignored, not rewritten.
  ASSERT_NO_FATAL_FAILURE(bind_input(storage.data() + 1, &length, SQL_DECIMAL, 4, 2, 5, 3));
  const auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  const auto ipd = descriptor(SQL_ATTR_IMP_PARAM_DESC);
  const auto dimensions_unchanged = [&] {
    SQLSMALLINT precision = -1, scale = -1, type = -1;
    SQLPOINTER data{};
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_PRECISION, &precision, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_SCALE, &scale, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_DATA_PTR, &data, 0, nullptr));
    EXPECT_EQ(5, precision); EXPECT_EQ(3, scale); EXPECT_EQ(storage.data() + 1, data);
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ipd, 1, SQL_DESC_CONCISE_TYPE, &type, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ipd, 1, SQL_DESC_PRECISION, &precision, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ipd, 1, SQL_DESC_SCALE, &scale, 0, nullptr));
    EXPECT_EQ(SQL_DECIMAL, type); EXPECT_EQ(4, precision); EXPECT_EQ(2, scale);
  };
  ASSERT_NO_FATAL_FAILURE(dimensions_unchanged());
  const auto before = storage;
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"12.34"}, facts->parameters[0].value);
  EXPECT_EQ(before, storage); EXPECT_EQ(99, length);
  ASSERT_NO_FATAL_FAILURE(dimensions_unchanged());
  std::fill(storage.begin() + 1, storage.end() - 1, 0);
  EXPECT_EQ(std::optional<std::string>{"12.34"}, facts->parameters[0].value);
  EXPECT_EQ(0x5a, storage.front()); EXPECT_EQ(0x5a, storage.back());
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  storage.fill(0x5a);  // Invalid unused struct/sign must remain unread for NULL.
  const auto null_poison = storage;
  length = SQL_NULL_DATA;
  ASSERT_NO_FATAL_FAILURE(bind_input(storage.data() + 1, &length, SQL_DECIMAL, 4, 2, 0, -129));
  const auto queries = facts->queries;
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(queries + 1, facts->queries);
  ASSERT_EQ(1u, facts->parameters.size()); EXPECT_FALSE(facts->parameters[0].value);
  EXPECT_EQ(null_poison, storage); EXPECT_EQ(SQL_NULL_DATA, length);
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, RescaledPreparedRefillAndLocalFailurePreserveOwningSibling) {
  facts->result.normalized_parameter_types = {NativeTypeInfo{ScalarType::Decimal, 4, 2, true}};
  struct SiblingGuard {
    SQLHSTMT handle{};
    ~SiblingGuard() { if (handle) SQLFreeHandle(SQL_HANDLE_STMT, handle); }
  } sibling;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling.handle));
  facts->result.columns = {{"neighbor", NativeTypeInfo{ScalarType::Integer, 10, 0, true}}};
  facts->result.rows = {{"41"}};
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto input = target_rescale_input(12345);
  SQLLEN length = sizeof(input);
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_NUMERIC, 4, 2, 5, 3));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  const auto first_owned_parameter = facts->parameters[0].value;
  ASSERT_EQ(std::optional<std::string>{"12.34"}, first_owned_parameter);
  input = {};
  EXPECT_EQ(first_owned_parameter, facts->parameters[0].value);
  SQLINTEGER first = -1, second = -1; SQLLEN output_length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_SLONG, &first, 0, &output_length));
  EXPECT_EQ(41, first);
  facts->result.rows = {{"99"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling.handle, (SQLCHAR*)"SELECT sibling", SQL_NTS));
  facts->result.rows = {{"777"}};
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  input = target_rescale_input(12345);
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_NUMERIC, 4, 2, 5, 2));
  const auto queries = facts->queries;
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(first_owned_parameter, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling.handle));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(sibling.handle, 1, SQL_C_SLONG, &second, 0, &output_length));
  EXPECT_EQ(99, second); EXPECT_EQ(41, first);
  ASSERT_EQ(SQL_NO_DATA, SQLFetch(sibling.handle));
  facts->result.rows = {{"43"}};
  input = target_rescale_input(12345, 0);
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_DECIMAL, 4, 2, 5, 3));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"-12.34"}, facts->parameters[0].value);
  input = {}; facts->result.rows = {{"888"}};
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_SLONG, &first, 0, &output_length));
  EXPECT_EQ(43, first); EXPECT_EQ(99, second);
  EXPECT_EQ(std::optional<std::string>{"-12.34"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  length = SQL_NULL_DATA; facts->result.rows = {{"44"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size()); EXPECT_FALSE(facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_SLONG, &first, 0, &output_length));
  EXPECT_EQ(44, first); EXPECT_EQ(99, second); EXPECT_EQ(SQL_NULL_DATA, length);
  EXPECT_EQ(0u, facts->disconnects);
}
}  // namespace

namespace {
class CharacterNumericApiTest : public NumericScaleApiTest {
 protected:
  std::array<char, 48> narrow{};
  std::array<SQLWCHAR, 48> wide{};
  SQLLEN input_length{SQL_NTS};
  void bind_text(const char* text, SQLSMALLINT c_type, SQLSMALLINT sql_type,
                 SQLULEN precision = 4, SQLSMALLINT scale = 2) {
    ASSERT_LT(std::strlen(text), 40u);
    narrow.fill(0x5a); wide.fill(static_cast<SQLWCHAR>(0x5a));
    std::memcpy(narrow.data() + 1, text, std::strlen(text) + 1);
    for (std::size_t i = 0; i <= std::strlen(text); ++i)
      wide[i + 1] = static_cast<SQLWCHAR>(text[i]);
    void* data = c_type == SQL_C_CHAR ? static_cast<void*>(narrow.data() + 1)
                                    : static_cast<void*>(wide.data() + 1);
    const SQLLEN bytes = c_type == SQL_C_CHAR ? 40 : 40 * sizeof(SQLWCHAR);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT,
        c_type, sql_type, precision, scale, data, bytes, &input_length));
  }
  void prepare_text() {
    facts->result.normalized_parameter_types = {NativeTypeInfo{ScalarType::Decimal, 4, 2, true}};
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  }
};

TEST_F(CharacterNumericApiTest, KnownTargetWholeAndFractionLossAcrossAnsiWideAliases) {
  ASSERT_NO_FATAL_FAILURE(prepare_text());
  SQLUSMALLINT status = SQL_PARAM_UNUSED; SQLULEN processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  for (const SQLSMALLINT c_type : {SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_WCHAR}}) {
    for (const SQLSMALLINT sql_type : {SQLSMALLINT{SQL_NUMERIC}, SQLSMALLINT{SQL_DECIMAL}}) {
      for (const auto* text : {"12.345", "-12.345", "123.45", "+123.45", " 12.345 "}) {
        SCOPED_TRACE(c_type);
        SCOPED_TRACE(sql_type);
        SCOPED_TRACE(text);
        input_length = SQL_NTS; status = SQL_PARAM_UNUSED; processed = 99;
        ASSERT_NO_FATAL_FAILURE(bind_text(text, c_type, sql_type));
        const auto old_narrow = narrow; const auto old_wide = wide;
        const auto queries = facts->queries;
        EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22001", state());
        EXPECT_EQ(queries, facts->queries); EXPECT_TRUE(facts->parameters.empty());
        EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
        EXPECT_EQ(SQL_NTS, input_length); EXPECT_EQ(old_narrow, narrow); EXPECT_EQ(old_wide, wide);
      }
    }
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(CharacterNumericApiTest, FittingZeroBlankAndUnknownDimensionsKeepOwningText) {
  ASSERT_NO_FATAL_FAILURE(prepare_text());
  for (const SQLSMALLINT c_type : {SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_WCHAR}}) {
    for (const SQLSMALLINT sql_type : {SQLSMALLINT{SQL_NUMERIC}, SQLSMALLINT{SQL_DECIMAL}}) {
      for (const auto* text : {"12.34", "-12.34", " 12.34 ", "12.3400", "0.000", "+0.00"}) {
        SCOPED_TRACE(text);
        input_length = SQL_NTS;
        ASSERT_NO_FATAL_FAILURE(bind_text(text, c_type, sql_type));
        const auto old_narrow = narrow; const auto old_wide = wide;
        ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(1u, facts->parameters.size());
        EXPECT_EQ(std::optional<std::string>{text}, facts->parameters[0].value);
        EXPECT_EQ(old_narrow, narrow); EXPECT_EQ(old_wide, wide); EXPECT_EQ(SQL_NTS, input_length);
        narrow.fill(0); wide.fill(0);
        EXPECT_EQ(std::optional<std::string>{text}, facts->parameters[0].value);
        ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
      }
      input_length = SQL_NTS;
      ASSERT_NO_FATAL_FAILURE(bind_text("123.456", c_type, sql_type, 0, 0));
      ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
      ASSERT_EQ(1u, facts->parameters.size());
      EXPECT_EQ(std::optional<std::string>{"123.456"}, facts->parameters[0].value);
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    }
  }
}

TEST_F(CharacterNumericApiTest, MalformedNullAndUnsupportedTargetPoliciesRemainDistinct) {
  ASSERT_NO_FATAL_FAILURE(prepare_text());
  for (const SQLSMALLINT c_type : {SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_WCHAR}}) {
    for (const auto* text : {"12x", "", "NaN", "1e+"}) {
      input_length = SQL_NTS;
      ASSERT_NO_FATAL_FAILURE(bind_text(text, c_type, SQL_NUMERIC));
      const auto queries = facts->queries;
      EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22018", state());
      EXPECT_EQ(queries, facts->queries);
    }
    input_length = SQL_NTS;
    ASSERT_NO_FATAL_FAILURE(bind_text("1.234", c_type, SQL_NUMERIC, 39, 2));
    const auto queries = facts->queries;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
    EXPECT_EQ(queries, facts->queries);
    input_length = SQL_NTS;
    ASSERT_NO_FATAL_FAILURE(bind_text("1.0", c_type, SQL_DECIMAL, 1, 2));
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
    EXPECT_EQ(queries, facts->queries);
    input_length = SQL_NULL_DATA;
    ASSERT_NO_FATAL_FAILURE(bind_text("not a literal", c_type, SQL_NUMERIC));
    const auto old_narrow = narrow; const auto old_wide = wide;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_FALSE(facts->parameters[0].value); EXPECT_EQ(SQL_NULL_DATA, input_length);
    EXPECT_EQ(old_narrow, narrow); EXPECT_EQ(old_wide, wide);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
}

TEST_F(CharacterNumericApiTest, RefusedCharacterInputRecoversWithoutChangingSiblingSnapshot) {
  struct Sibling { SQLHSTMT handle{}; ~Sibling() { if (handle) SQLFreeHandle(SQL_HANDLE_STMT, handle); } } sibling;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling.handle));
  ASSERT_NO_FATAL_FAILURE(prepare_text());
  facts->result.rows = {{"41"}};
  ASSERT_NO_FATAL_FAILURE(bind_text("12.34", SQL_C_CHAR, SQL_NUMERIC));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(1u, facts->parameters.size());
  const auto first_parameter = facts->parameters[0].value;
  narrow.fill(0);
  SQLINTEGER first = -1, second = -1; SQLLEN length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_SLONG, &first, 0, &length));
  EXPECT_EQ(41, first);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  facts->result.rows = {{"99"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling.handle, (SQLCHAR*)"SELECT sibling", SQL_NTS));
  facts->result.rows = {{"777"}};
  ASSERT_NO_FATAL_FAILURE(bind_text("123.45", SQL_C_WCHAR, SQL_DECIMAL));
  const auto queries = facts->queries;
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22001", state());
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(first_parameter, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling.handle));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(sibling.handle, 1, SQL_C_SLONG, &second, 0, &length));
  EXPECT_EQ(99, second); EXPECT_EQ(41, first);
  ASSERT_EQ(SQL_NO_DATA, SQLFetch(sibling.handle));
  facts->result.rows = {{"43"}};
  ASSERT_NO_FATAL_FAILURE(bind_text("-12.34", SQL_C_WCHAR, SQL_DECIMAL));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"-12.34"}, facts->parameters[0].value);
  wide.fill(0); facts->result.rows = {{"888"}};
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_SLONG, &first, 0, &length));
  EXPECT_EQ(43, first); EXPECT_EQ(99, second);
  EXPECT_EQ(std::optional<std::string>{"-12.34"}, facts->parameters[0].value);
  EXPECT_EQ(0u, facts->disconnects);
}
}  // namespace

namespace {
class FixedNumericCompletionApiTest : public NumericScaleApiTest {
 protected:
  void position(ScalarType type, const char* text) {
    const bool exact = type == ScalarType::Numeric || type == ScalarType::Decimal;
    const std::uint64_t size = exact ? 38 : type == ScalarType::Boolean ? 1 :
        type == ScalarType::SmallInt ? 5 : type == ScalarType::Integer ? 10 :
        type == ScalarType::BigInt ? 19 : type == ScalarType::Real ? 7 : 15;
    facts->result.columns = {{"n", NativeTypeInfo{type, size,
        static_cast<std::int16_t>(exact ? 3 : 0), true}}};
    facts->result.rows = {{std::string(text)}};
    ASSERT_NO_FATAL_FAILURE(execute());
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  }
  SQLSMALLINT exposed_type() {
    SQLCHAR name[16]{};
    SQLSMALLINT name_length{}, type{}, scale{}, nullable{};
    SQLULEN size{};
    EXPECT_EQ(SQL_SUCCESS, SQLDescribeCol(stmt, 1, name, sizeof(name),
        &name_length, &type, &size, &scale, &nullable));
    return type;
  }
  void expect_completed(SQLHSTMT handle, SQLUSMALLINT column,
                        SQLSMALLINT target = SQL_C_CHAR) {
    std::array<unsigned char, 64> output{};
    output.fill(0x5a);
    const auto poison = output;
    SQLLEN length = 73;
    EXPECT_EQ(SQL_NO_DATA, SQLGetData(handle, column, target,
        output.data(), output.size(), &length));
    EXPECT_EQ(poison, output);
    EXPECT_EQ(73, length);
    SQLCHAR diagnostic[6]{};
    EXPECT_EQ(SQL_NO_DATA, SQLGetDiagRec(SQL_HANDLE_STMT, handle, 1,
        diagnostic, nullptr, nullptr, 0, nullptr));
  }
};

TEST_F(FixedNumericCompletionApiTest, NumericCharacterFirstResultsCompleteAcrossExposedFamilies) {
  struct Scenario {
    ScalarType type;
    SQLSMALLINT sql;
    const char* text;
    SQLLEN capacity;
    const char* expected;
    SQLRETURN result;
  };
  const Scenario scenarios[] = {
      {ScalarType::Numeric, SQL_NUMERIC, "12.345", 5, "12.3", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Decimal, SQL_DECIMAL, "-12.345", 6, "-12.3", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Real, SQL_REAL, "12.345", 5, "12.3", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Double, SQL_DOUBLE, "-12.345", 6, "-12.3", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Decimal, SQL_DECIMAL, "0.001", 3, "0.", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Decimal, SQL_DECIMAL, "12.345", 16, "12.345", SQL_SUCCESS},
      {ScalarType::SmallInt, SQL_SMALLINT, "-12", 16, "-12", SQL_SUCCESS},
      {ScalarType::Integer, SQL_INTEGER, "42", 16, "42", SQL_SUCCESS},
      {ScalarType::BigInt, SQL_BIGINT, "9223372036854775807", 24,
          "9223372036854775807", SQL_SUCCESS},
      {ScalarType::Boolean, SQL_BIT, "1", 16, "1", SQL_SUCCESS},
      {ScalarType::Boolean, SQL_BIT, "0", 16, "0", SQL_SUCCESS},
      {ScalarType::Real, SQL_REAL, "1.25e+2", 16, "1.25e+2", SQL_SUCCESS},
      {ScalarType::Double, SQL_DOUBLE, "NaN", 16, "NaN", SQL_SUCCESS},
      {ScalarType::Double, SQL_DOUBLE, "Infinity", 16, "Infinity", SQL_SUCCESS},
      {ScalarType::Double, SQL_DOUBLE, "-Infinity", 16, "-Infinity", SQL_SUCCESS}};
  SQLUSMALLINT row_status = SQL_ROW_NOROW;
  SQLULEN fetched = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &row_status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &fetched, 0));
  for (const auto& scenario : scenarios) {
    SCOPED_TRACE(scenario.text);
    SCOPED_TRACE(scenario.sql);
    ASSERT_NO_FATAL_FAILURE(position(scenario.type, scenario.text));
    EXPECT_EQ(scenario.sql, exposed_type());
    std::array<unsigned char, 34> output{};
    output.fill(0x5a);
    SQLLEN length = 73;
    ASSERT_EQ(scenario.result, SQLGetData(stmt, 1, SQL_C_CHAR,
        output.data() + 1, scenario.capacity, &length));
    EXPECT_EQ(static_cast<SQLLEN>(std::strlen(scenario.text)), length);
    EXPECT_STREQ(scenario.expected, reinterpret_cast<const char*>(output.data() + 1));
    EXPECT_EQ(0x5a, output.front());
    EXPECT_EQ(0x5a, output[static_cast<std::size_t>(scenario.capacity) + 1]);
    EXPECT_EQ(SQL_ROW_SUCCESS, row_status);
    EXPECT_EQ(1u, fetched);
    if (scenario.result == SQL_SUCCESS_WITH_INFO) {
      EXPECT_EQ("01004", state());
    }
    ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
    EXPECT_EQ(SQL_ROW_SUCCESS, row_status);
    EXPECT_EQ(1u, fetched);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(FixedNumericCompletionApiTest, WideArdCompletionPreservesUnalignedBytesAndDescriptorAuthority) {
  for (const auto type : {ScalarType::Numeric, ScalarType::Decimal,
                         ScalarType::Real, ScalarType::Double}) {
    SCOPED_TRACE(static_cast<int>(type));
    for (const SQLSMALLINT target : {SQLSMALLINT{SQL_C_WCHAR}, SQLSMALLINT{SQL_ARD_TYPE}}) {
      SCOPED_TRACE(target);
      ASSERT_NO_FATAL_FAILURE(position(type, "12.345"));
      const auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
      ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, 1, SQL_DESC_CONCISE_TYPE,
          reinterpret_cast<SQLPOINTER>(SQL_C_WCHAR), 0));
      SQLSMALLINT concise_type{};
      ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ard, 1, SQL_DESC_CONCISE_TYPE,
          &concise_type, 0, nullptr));
      EXPECT_EQ(SQL_C_WCHAR, concise_type);
      std::array<unsigned char, 5 * sizeof(SQLWCHAR) + 2> output{};
      output.fill(0x5a);
      SQLLEN length = 73;
      ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, target,
          output.data() + 1, 5 * sizeof(SQLWCHAR), &length));
      EXPECT_EQ("01004", state());
      EXPECT_EQ(static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)), length);
      const std::array<SQLWCHAR, 5> expected{
          static_cast<SQLWCHAR>('1'), static_cast<SQLWCHAR>('2'),
          static_cast<SQLWCHAR>('.'), static_cast<SQLWCHAR>('3'), 0};
      EXPECT_EQ(0, std::memcmp(output.data() + 1, expected.data(), sizeof(expected)));
      EXPECT_EQ(0x5a, output.front());
      EXPECT_EQ(0x5a, output.back());
      ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1, target));
      concise_type = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ard, 1, SQL_DESC_CONCISE_TYPE,
          &concise_type, 0, nullptr));
      EXPECT_EQ(SQL_C_WCHAR, concise_type);
      SQLPOINTER data = reinterpret_cast<SQLPOINTER>(1);
      ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(ard, 1, SQL_DESC_DATA_PTR,
          &data, 0, nullptr));
      EXPECT_EQ(nullptr, data);
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    }
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(FixedNumericCompletionApiTest, NumericWholeRefusalsAndNullRemainRetryableWithoutMutation) {
  for (const SQLSMALLINT target : {SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_WCHAR}}) {
    SCOPED_TRACE(target);
    ASSERT_NO_FATAL_FAILURE(position(ScalarType::Decimal, "12.345"));
    const SQLLEN unit = target == SQL_C_WCHAR ? sizeof(SQLWCHAR) : 1;
    std::array<unsigned char, 64> output{};
    output.fill(0x5a);
    const auto poison = output;
    for (const SQLLEN capacity : {SQLLEN{0}, unit, 2 * unit}) {
      SCOPED_TRACE(capacity);
      SQLLEN length = 73;
      EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, target,
          output.data() + 1, capacity, &length));
      EXPECT_EQ("22003", state());
      EXPECT_EQ(poison, output);
      EXPECT_EQ(73, length);
    }
    if (target == SQL_C_WCHAR) {
      SQLLEN length = 73;
      EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, target,
          output.data() + 1, unit - 1, &length));
      EXPECT_EQ("22003", state());
      EXPECT_EQ(poison, output);
      EXPECT_EQ(73, length);
    }
    SQLLEN length = 73;
    EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, target,
        output.data() + 1, -1, &length));
    EXPECT_EQ("HY090", state());
    EXPECT_EQ(poison, output);
    EXPECT_EQ(73, length);
    EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, target, nullptr, 5 * unit, &length));
    EXPECT_EQ("HY009", state());
    EXPECT_EQ(73, length);
    ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, target,
        output.data() + 1, 5 * unit, &length));
    EXPECT_EQ("01004", state());
    EXPECT_EQ(6 * unit, length);
    if (target == SQL_C_CHAR) {
      EXPECT_STREQ("12.3", reinterpret_cast<const char*>(output.data() + 1));
    } else {
      const std::array<SQLWCHAR, 5> expected{
          static_cast<SQLWCHAR>('1'), static_cast<SQLWCHAR>('2'),
          static_cast<SQLWCHAR>('.'), static_cast<SQLWCHAR>('3'), 0};
      EXPECT_EQ(0, std::memcmp(output.data() + 1, expected.data(), sizeof(expected)));
    }
    EXPECT_EQ(0x5a, output.front());
    EXPECT_EQ(0x5a, output[static_cast<std::size_t>(5 * unit) + 1]);
    ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1, target));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));

    for (const auto* literal : {"1.25e+2", "NaN", "Infinity", "-Infinity"}) {
      SCOPED_TRACE(literal);
      ASSERT_NO_FATAL_FAILURE(position(ScalarType::Double, literal));
      output.fill(0x5a);
      length = 73;
      EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, target,
          output.data() + 1, 3 * unit, &length));
      EXPECT_EQ("22003", state());
      EXPECT_EQ(poison, output);
      EXPECT_EQ(73, length);
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, target,
          output.data() + 1, 16 * unit, &length));
      EXPECT_EQ(static_cast<SQLLEN>(std::strlen(literal)) * unit, length);
      ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1, target));
      ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
    }

    facts->result.columns = {
        {"n", NativeTypeInfo{ScalarType::Decimal, 38, 3, true}},
        {"neighbor", NativeTypeInfo{ScalarType::Integer, 10, 0, true}}};
    facts->result.rows = {{std::nullopt, "42"}};
    ASSERT_NO_FATAL_FAILURE(execute());
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
    output.fill(0x5a);
    EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, target,
        output.data() + 1, 0, nullptr));
    EXPECT_EQ("22002", state());
    EXPECT_EQ(poison, output);
    length = 73;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, target,
        output.data() + 1, 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
    EXPECT_EQ(poison, output);
    ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1, target));
    SQLINTEGER neighbor = -1;
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_SLONG,
        &neighbor, 0, &length));
    EXPECT_EQ(42, neighbor);
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(neighbor)), length);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(FixedNumericCompletionApiTest, VariableChunksAndOwningSiblingRefetchRefillStayIndependent) {
  facts->result.columns = {
      {"n", NativeTypeInfo{ScalarType::Decimal, 38, 3, true}},
      {"text", NativeTypeInfo{ScalarType::VarChar, 6, 0, true}},
      {"bytes", NativeTypeInfo{ScalarType::Binary, 6, 0, true}},
      {"time", NativeTypeInfo{ScalarType::Time, 12, 3, true}}};
  facts->result.rows = {{"12.345", "12.345", "abcdef", "12:34:56.789"},
                        {"23.456", "23.456", "ghijkl", "23:45:12.345"}};
  struct Sibling {
    SQLHSTMT handle{};
    ~Sibling() { if (handle) SQLFreeHandle(SQL_HANDLE_STMT, handle); }
  } sibling;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling.handle));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT n, text, bytes, time", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  facts->result.rows = {{"99.876", "99.876", "uvwxyz", "01:02:03.456"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling.handle, (SQLCHAR*)"SELECT sibling", SQL_NTS));
  facts->result.rows = {{"777.777", "777.777", "poison", "00:00:00.000"}};
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  char narrow[16]{};
  SQLLEN length = 73;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_CHAR, narrow, 5, &length));
  EXPECT_EQ("01004", state());
  EXPECT_STREQ("12.3", narrow);
  EXPECT_EQ(6, length);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));

  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 2, SQL_C_CHAR, narrow, 5, &length));
  EXPECT_EQ("01004", state());
  EXPECT_STREQ("12.3", narrow);
  EXPECT_EQ(6, length);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_CHAR, narrow, 3, &length));
  EXPECT_STREQ("45", narrow);
  EXPECT_EQ(2, length);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 2));

  // Switching columns restarts the previous column, including a completed numeric.
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_CHAR, narrow, 5, &length));
  EXPECT_STREQ("12.3", narrow);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
  std::array<SQLWCHAR, 5> wide{};
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 2, SQL_C_WCHAR,
      wide.data(), sizeof(wide), &length));
  EXPECT_EQ("01004", state());
  EXPECT_EQ(static_cast<SQLLEN>(6 * sizeof(SQLWCHAR)), length);
  const std::array<SQLWCHAR, 5> first{
      static_cast<SQLWCHAR>('1'), static_cast<SQLWCHAR>('2'),
      static_cast<SQLWCHAR>('.'), static_cast<SQLWCHAR>('3'), 0};
  EXPECT_EQ(first, wide);
  wide.fill(static_cast<SQLWCHAR>(0x5a));
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 2, SQL_C_WCHAR,
      wide.data(), 3 * sizeof(SQLWCHAR), &length));
  EXPECT_EQ(static_cast<SQLLEN>(2 * sizeof(SQLWCHAR)), length);
  EXPECT_EQ(static_cast<SQLWCHAR>('4'), wide[0]);
  EXPECT_EQ(static_cast<SQLWCHAR>('5'), wide[1]);
  EXPECT_EQ(0, wide[2]);
  EXPECT_EQ(static_cast<SQLWCHAR>(0x5a), wide[3]);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 2, SQL_C_WCHAR));

  std::array<char, 3> bytes{};
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 3, SQL_C_BINARY,
      bytes.data(), bytes.size(), &length));
  EXPECT_EQ("01004", state());
  EXPECT_EQ((std::array<char, 3>{'a', 'b', 'c'}), bytes);
  EXPECT_EQ(6, length);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_BINARY,
      bytes.data(), bytes.size(), &length));
  EXPECT_EQ((std::array<char, 3>{'d', 'e', 'f'}), bytes);
  EXPECT_EQ(3, length);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 3, SQL_C_BINARY));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 4, SQL_C_CHAR, narrow, 9, &length));
  EXPECT_EQ("01004", state());
  EXPECT_STREQ("12:34:56", narrow);
  EXPECT_EQ(12, length);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 4));

  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling.handle));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(sibling.handle, 1, SQL_C_CHAR, narrow, 5, &length));
  EXPECT_STREQ("99.8", narrow);
  ASSERT_NO_FATAL_FAILURE(expect_completed(sibling.handle, 1));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_CHAR, narrow, 5, &length));
  EXPECT_STREQ("23.4", narrow);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
  ASSERT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));

  facts->result.rows = {{"34.567", "34.567", "rstuvw", "02:03:04.567"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  facts->result.rows = {{"888.888", "888.888", "poison", "00:00:00.000"}};
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_CHAR, narrow, 5, &length));
  EXPECT_STREQ("34.5", narrow);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
  ASSERT_NO_FATAL_FAILURE(expect_completed(sibling.handle, 1));
  EXPECT_EQ(0u, facts->disconnects);
}
}  // namespace

namespace {
// Test-side base256 input construction; expected decimal strings stay literal.
SQL_NUMERIC_STRUCT approximate_numeric_input(std::uint64_t coefficient,
                                             SQLCHAR sign = 1) {
  SQL_NUMERIC_STRUCT input{};
  input.precision = 1; input.scale = 0; input.sign = sign;
  for (std::size_t i = 0; i < sizeof(coefficient); ++i) {
    input.val[i] = static_cast<SQLCHAR>(coefficient & 0xffu);
    coefficient >>= 8;
  }
  return input;
}

class CnumericApproximateRangeApiTest : public NumericScaleApiTest {
 protected:
  SQLHSTMT sibling{};
  void TearDown() override {
    if (sibling) SQLFreeHandle(SQL_HANDLE_STMT, sibling);
    NumericScaleApiTest::TearDown();
  }
};

TEST_F(CnumericApproximateRangeApiTest, RealOverflowRefusesBeforeDispatchAndRepairs) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto input = approximate_numeric_input(1);
  SQLLEN length = 73;
  SQLUSMALLINT status = SQL_PARAM_UNUSED;
  SQLULEN processed = 99;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_REAL, 0, 0, 1, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"1"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  struct Refusal { std::uint64_t coefficient; SQLSMALLINT precision, scale; SQLCHAR sign; };
  const Refusal refused[] = {
      {1, 1, -100, 1}, {1, 1, -100, 0},
      {35, 2, -37, 1}, {35, 2, -37, 0},
      {34028236, 8, -31, 1}, {34028236, 8, -31, 0},
      {1, 1, -128, 1}};
  for (const auto& item : refused) {
    SCOPED_TRACE(item.coefficient);
    SCOPED_TRACE(item.scale);
    SCOPED_TRACE(item.sign);
    input = approximate_numeric_input(item.coefficient, item.sign);
    const auto original = input;
    length = 73; status = SQL_PARAM_UNUSED; processed = 99;
    ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_REAL, 0, 0,
        item.precision, item.scale));
    const auto queries = facts->queries;
    EXPECT_EQ(SQL_ERROR, SQLExecute(stmt));
    EXPECT_EQ("22003", state());
    EXPECT_EQ(queries, facts->queries);
    ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(std::optional<std::string>{"1"}, facts->parameters[0].value);
    EXPECT_EQ(QueryParameterType::Float32, facts->parameters[0].type);
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(73, length);
    EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
    // Also releases the unexpected result in the original failing baseline.
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  input = approximate_numeric_input(42);
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_REAL, 0, 0, 2, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
  SQLCHAR diagnostic[6]{};
  EXPECT_EQ(SQL_NO_DATA, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1,
      diagnostic, nullptr, nullptr, 0, nullptr));
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(CnumericApproximateRangeApiTest, FittingRoundedSubnormalAndDoubleAliasesKeepExactText) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  struct Fit {
    SQLSMALLINT target, precision, scale;
    std::uint64_t coefficient; SQLCHAR sign;
    std::string expected;
  };
  const std::string minimum = "0." + std::string(37, '0') + "1";
  const Fit fits[] = {
      {SQL_REAL, 1, 0, 1, 1, "1"}, {SQL_REAL, 1, 0, 1, 0, "-1"},
      {SQL_REAL, 3, 3, 0, 0, "0.000"},
      {SQL_REAL, 5, 3, 12345, 1, "12.345"},
      {SQL_REAL, 5, 3, 12345, 0, "-12.345"},
      {SQL_REAL, 38, 38, 1, 1, minimum},
      {SQL_REAL, 38, 38, 1, 0, "-" + minimum},
      {SQL_REAL, 2, -37, 34, 1, "34" + std::string(37, '0')},
      // Above mathematical FLT_MAX, but rounds to a finite float by the frozen rule.
      {SQL_REAL, 8, -31, 34028235, 1, "34028235" + std::string(31, '0')},
      {SQL_REAL, 8, -31, 34028235, 0, "-34028235" + std::string(31, '0')},
      {SQL_FLOAT, 1, -100, 1, 1, "1" + std::string(100, '0')},
      {SQL_FLOAT, 1, -100, 1, 0, "-1" + std::string(100, '0')},
      {SQL_DOUBLE, 1, -128, 1, 1, "1" + std::string(128, '0')},
      {SQL_DOUBLE, 38, 38, 1, 1, minimum}};
  for (const auto& item : fits) {
    SCOPED_TRACE(item.target);
    SCOPED_TRACE(item.expected);
    auto input = approximate_numeric_input(item.coefficient, item.sign);
    const auto original = input;
    SQLLEN length = 1;  // Fixed-size numeric C input ignores this nonNULL length.
    ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, item.target, 0, 0,
        item.precision, item.scale));
    const auto queries = facts->queries;
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(item.target == SQL_REAL ? QueryParameterType::Float32 : QueryParameterType::Float64,
        facts->parameters[0].type);
    EXPECT_EQ(std::optional<std::string>{item.expected}, facts->parameters[0].value);
    EXPECT_FALSE(facts->parameters[0].binary_input);
    EXPECT_EQ(queries + 1, facts->queries);
    EXPECT_EQ(1, length); EXPECT_EQ(0, std::memcmp(&input, &original, sizeof(input)));
    const auto owned = facts->parameters;
    std::memset(&input, 0xff, sizeof(input));
    EXPECT_EQ(std::optional<std::string>{item.expected}, owned[0].value);
    SQLCHAR diagnostic[6]{};
    EXPECT_EQ(SQL_NO_DATA, SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1,
        diagnostic, nullptr, nullptr, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(CnumericApproximateRangeApiTest, ApdUnalignedFixedLengthsNullPoisonAndInvalidRecovery) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto input = approximate_numeric_input(12345);
  // Struct dimensions intentionally disagree with the authoritative APD.
  input.precision = 1; input.scale = -100;
  std::array<unsigned char, sizeof(input) + 2> storage{};
  storage.fill(0x5a); std::memcpy(storage.data() + 1, &input, sizeof(input));
  SQLLEN length = 0;
  const auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  for (const SQLLEN fixed_length : {SQLLEN{0}, SQLLEN{1}, SQLLEN{SQL_NTS}, SQLLEN{-9}}) {
    length = fixed_length;
    const auto before = storage;
    ASSERT_NO_FATAL_FAILURE(bind_input(storage.data() + 1, &length, SQL_REAL, 0, 0, 5, 3));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(std::optional<std::string>{"12.345"}, facts->parameters[0].value);
    EXPECT_EQ(before, storage); EXPECT_EQ(fixed_length, length);
    SQLSMALLINT precision = 0, scale = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_PRECISION, &precision, 0, nullptr));
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_SCALE, &scale, 0, nullptr));
    EXPECT_EQ(5, precision); EXPECT_EQ(3, scale);
    ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  }
  std::fill(storage.begin() + 1, storage.end() - 1, 0xff);
  length = SQL_NULL_DATA;
  const auto poison = storage;
  ASSERT_NO_FATAL_FAILURE(bind_input(storage.data() + 1, &length, SQL_REAL, 0, 0, 0, -129));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_FALSE(facts->parameters[0].value);
  EXPECT_EQ(QueryParameterType::Float32, facts->parameters[0].type);
  EXPECT_EQ(poison, storage); EXPECT_EQ(SQL_NULL_DATA, length);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  length = 0;
  // Restore an explicitly valid binding after the poisoned NULL/invalid-APD
  // control, so this step isolates malformed C fields instead of missing binding.
  ASSERT_NO_FATAL_FAILURE(bind_input(storage.data() + 1, &length, SQL_REAL, 0, 0, 5, 3));
  const auto queries = facts->queries;
  EXPECT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(poison, storage);
  EXPECT_FALSE(facts->parameters[0].value);
  input = approximate_numeric_input(12);
  std::memcpy(storage.data() + 1, &input, sizeof(input));
  ASSERT_NO_FATAL_FAILURE(bind_input(storage.data() + 1, &length, SQL_DOUBLE, 0, 0, 2, -3));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"12000"}, facts->parameters[0].value);
  EXPECT_EQ(0x5a, storage.front()); EXPECT_EQ(0x5a, storage.back());
  EXPECT_EQ(0, length); EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(CnumericApproximateRangeApiTest, RefusedSiblingAndPreparedRefillPreserveOwningSnapshots) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto input = approximate_numeric_input(12345);
  SQLLEN length = sizeof(input);
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_REAL, 0, 0, 5, 3));
  facts->result.rows = {{"7.25"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  const auto parent_parameters = facts->parameters;
  ASSERT_EQ(1u, parent_parameters.size());
  EXPECT_EQ(std::optional<std::string>{"12.345"}, parent_parameters[0].value);
  std::memset(&input, 0xff, sizeof(input));
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(sibling, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto other = approximate_numeric_input(1);
  SQLLEN other_length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(sibling, 1, SQL_PARAM_INPUT, SQL_C_NUMERIC,
      SQL_REAL, 0, 0, &other, sizeof(other), &other_length));
  SQLHDESC sibling_apd{};
  ASSERT_EQ(SQL_SUCCESS, SQLGetStmtAttr(sibling, SQL_ATTR_APP_PARAM_DESC, &sibling_apd, 0, nullptr));
  ASSERT_NO_FATAL_FAILURE(dimensions(sibling_apd, 1, 1, -100, &other));
  facts->result.rows = {{"99"}};
  const auto queries = facts->queries;
  EXPECT_EQ(SQL_ERROR, SQLExecute(sibling));
  SQLCHAR state_value[6]{};
  EXPECT_EQ(SQL_SUCCESS, SQLGetDiagRec(SQL_HANDLE_STMT, sibling, 1,
      state_value, nullptr, nullptr, 0, nullptr));
  EXPECT_STREQ("22003", reinterpret_cast<char*>(state_value));
  EXPECT_EQ(queries, facts->queries);
  EXPECT_EQ(std::optional<std::string>{"12.345"}, parent_parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(sibling, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(sibling, 1, SQL_PARAM_INPUT, SQL_C_NUMERIC,
      SQL_DOUBLE, 0, 0, &other, sizeof(other), &other_length));
  ASSERT_NO_FATAL_FAILURE(dimensions(sibling_apd, 1, 1, -100, &other));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(sibling));
  const auto sibling_parameters = facts->parameters;
  ASSERT_EQ(1u, sibling_parameters.size());
  EXPECT_EQ(std::optional<std::string>{"1" + std::string(100, '0')}, sibling_parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling));
  SQLDOUBLE sibling_value = -1; SQLLEN bytes = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(sibling, 1, SQL_C_DOUBLE, &sibling_value, 0, &bytes));
  EXPECT_DOUBLE_EQ(99, sibling_value);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  SQLDOUBLE parent_value = -1; bytes = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_DOUBLE, &parent_value, 0, &bytes));
  EXPECT_DOUBLE_EQ(7.25, parent_value);
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt));
  input = approximate_numeric_input(42);
  ASSERT_NO_FATAL_FAILURE(bind_input(&input, &length, SQL_REAL, 0, 0, 2, 0));
  facts->result.rows = {{"11"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_EQ(std::optional<std::string>{"12.345"}, parent_parameters[0].value);
  EXPECT_EQ(std::optional<std::string>{"1" + std::string(100, '0')}, sibling_parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  parent_value = -1; bytes = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_DOUBLE, &parent_value, 0, &bytes));
  EXPECT_DOUBLE_EQ(11, parent_value);
  EXPECT_EQ(0u, facts->disconnects);
}
}  // namespace

namespace {
class FixedTemporalCompletionApiTest : public FixedNumericCompletionApiTest {
 protected:
  void temporal_position(ScalarType type, const char* text) {
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    facts->result.columns = {{"t", NativeTypeInfo{type, 35, 3, true}}};
    facts->result.rows = {{text ? std::optional<std::string>{text} : std::nullopt}};
    ASSERT_NO_FATAL_FAILURE(execute());
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  }
};

TEST_F(FixedTemporalCompletionApiTest, NarrowFirstPrefixesAndFullValuesComplete) {
  struct Scenario {
    ScalarType type; SQLSMALLINT sql; const char* text; SQLLEN capacity;
    const char* expected; SQLRETURN result;
  };
  const Scenario scenarios[] = {
      {ScalarType::Time, SQL_TYPE_TIME, "12:34:56.789", 9, "12:34:56", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Time, SQL_TYPE_TIME, "12:34:56.789", 11, "12:34:56.7", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Timestamp, SQL_TYPE_TIMESTAMP, "2026-10-12 12:34:56.123456", 20,
       "2026-10-12 12:34:56", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Timestamp, SQL_TYPE_TIMESTAMP, "2026-10-12 12:34:56.123456", 23,
       "2026-10-12 12:34:56.12", SQL_SUCCESS_WITH_INFO},
      {ScalarType::Time, SQL_TYPE_TIME, "12:34:56.789", 13, "12:34:56.789", SQL_SUCCESS},
      {ScalarType::Timestamp, SQL_TYPE_TIMESTAMP, "2026-10-12 12:34:56.123456", 27,
       "2026-10-12 12:34:56.123456", SQL_SUCCESS},
      {ScalarType::Time, SQL_TYPE_TIME, "12:34:56", 9, "12:34:56", SQL_SUCCESS},
      {ScalarType::Timestamp, SQL_TYPE_TIMESTAMP, "2026-10-12 12:34:56", 20,
       "2026-10-12 12:34:56", SQL_SUCCESS}};
  for (const auto& test : scenarios) {
    SCOPED_TRACE(test.text);
    SCOPED_TRACE(test.capacity);
    ASSERT_NO_FATAL_FAILURE(temporal_position(test.type, test.text));
    EXPECT_EQ(test.sql, exposed_type());
    std::array<unsigned char, 66> output{}; output.fill(0x5a);
    SQLLEN length = 73;
    ASSERT_EQ(test.result, SQLGetData(stmt, 1, SQL_C_CHAR, output.data() + 1,
        test.capacity, &length));
    EXPECT_STREQ(test.expected, reinterpret_cast<const char*>(output.data() + 1));
    EXPECT_EQ(static_cast<SQLLEN>(std::strlen(test.text)), length);
    EXPECT_EQ(0x5a, output.front());
    EXPECT_EQ(0x5a, output[static_cast<std::size_t>(test.capacity) + 1]);
    if (test.result == SQL_SUCCESS_WITH_INFO) {
      EXPECT_EQ("01004", state());
    }
    ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
  }
}

TEST_F(FixedTemporalCompletionApiTest, ArdAndUnalignedWideUseByteLengthsAndComplete) {
  struct Scenario { ScalarType type; const char* text; const char* prefix; std::size_t units; };
  const Scenario scenarios[] = {
      {ScalarType::Time, "12:34:56.789", "12:34:56", 9},
      {ScalarType::Timestamp, "2026-10-12 12:34:56.789", "2026-10-12 12:34:56", 20}};
  for (const auto& test : scenarios) {
    for (const bool use_ard : {false, true}) {
      for (const SQLSMALLINT type : {SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_WCHAR}}) {
        SCOPED_TRACE(test.text);
        SCOPED_TRACE(use_ard);
        SCOPED_TRACE(type);
        ASSERT_NO_FATAL_FAILURE(temporal_position(test.type, test.text));
        const auto unit = type == SQL_C_WCHAR ? sizeof(SQLWCHAR) : std::size_t{1};
        if (use_ard) {
          ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(
            descriptor(SQL_ATTR_APP_ROW_DESC), 1, SQL_DESC_CONCISE_TYPE,
            reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(type)), 0));
        }
        const SQLSMALLINT target = use_ard ? SQL_ARD_TYPE : type;
        std::array<unsigned char, 64 * sizeof(SQLWCHAR) + 4> output{}; output.fill(0x5a);
        std::array<unsigned char, sizeof(SQLLEN) + 2> indicator{}; indicator.fill(0x5a);
        const SQLLEN poison_length = 73;
        std::memcpy(indicator.data() + 1, &poison_length, sizeof(poison_length));
        const auto capacity = test.units * unit + (type == SQL_C_WCHAR ? 1 : 0);
        ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, target,
            output.data() + 1, static_cast<SQLLEN>(capacity),
            reinterpret_cast<SQLLEN*>(indicator.data() + 1)));
        EXPECT_EQ("01004", state());
        SQLLEN length{}; std::memcpy(&length, indicator.data() + 1, sizeof(length));
        EXPECT_EQ(static_cast<SQLLEN>(std::strlen(test.text) * unit), length);
        if (type == SQL_C_WCHAR) {
          std::array<SQLWCHAR, 64> expected{};
          for (std::size_t i = 0; i < std::strlen(test.prefix); ++i)
            expected[i] = static_cast<SQLWCHAR>(test.prefix[i]);
          EXPECT_EQ(0, std::memcmp(output.data() + 1, expected.data(), test.units * unit));
        } else {
          EXPECT_STREQ(test.prefix, reinterpret_cast<const char*>(output.data() + 1));
        }
        EXPECT_EQ(0x5a, output.front()); EXPECT_EQ(0x5a, output[capacity + 1]);
        if (type == SQL_C_WCHAR) {
          EXPECT_EQ(0x5a, output[test.units * unit + 1]);
        }
        EXPECT_EQ(0x5a, indicator.front()); EXPECT_EQ(0x5a, indicator.back());
        output.fill(0x5a); indicator.fill(0x5a);
        std::memcpy(indicator.data() + 1, &poison_length, sizeof(poison_length));
        const auto output_poison = output; const auto indicator_poison = indicator;
        EXPECT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, target, output.data() + 1,
            static_cast<SQLLEN>(capacity), reinterpret_cast<SQLLEN*>(indicator.data() + 1)));
        EXPECT_EQ(output_poison, output); EXPECT_EQ(indicator_poison, indicator);
      }
    }
  }
}

TEST_F(FixedTemporalCompletionApiTest, WholeCapacityAndNullErrorsRetryBeforeCompletion) {
  struct Scenario { ScalarType type; const char* text; SQLLEN units; };
  const Scenario scenarios[] = {
      {ScalarType::Time, "12:34:56.789", 9},
      {ScalarType::Timestamp, "2026-10-12 12:34:56.789", 20}};
  for (const auto& test : scenarios) {
    for (const SQLSMALLINT type : {SQLSMALLINT{SQL_C_CHAR}, SQLSMALLINT{SQL_C_WCHAR}}) {
      const SQLLEN unit = type == SQL_C_WCHAR ? sizeof(SQLWCHAR) : 1;
      for (const SQLLEN capacity : {SQLLEN{0}, unit - 1, (test.units - 1) * unit, SQLLEN{-1}}) {
        SCOPED_TRACE(test.text);
        SCOPED_TRACE(capacity);
        ASSERT_NO_FATAL_FAILURE(temporal_position(test.type, test.text));
        std::array<unsigned char, 128> output{}; output.fill(0x5a);
        const auto poison = output; SQLLEN length = 73;
        EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, type, output.data() + 1, capacity, &length));
        EXPECT_EQ(capacity < 0 ? "HY090" : "22003", state());
        EXPECT_EQ(poison, output); EXPECT_EQ(73, length);
        EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, type, nullptr, test.units * unit, &length));
        EXPECT_EQ("HY009", state()); EXPECT_EQ(73, length);
        ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, type,
            output.data() + 1, test.units * unit, &length));
        EXPECT_EQ("01004", state());
        EXPECT_EQ(static_cast<SQLLEN>(std::strlen(test.text)) * unit, length);
        ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1, type));
      }
      ASSERT_NO_FATAL_FAILURE(temporal_position(test.type, nullptr));
      std::array<unsigned char, 128> output{}; output.fill(0x5a);
      const auto poison = output;
      EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, type, output.data() + 1, 0, nullptr));
      EXPECT_EQ("22002", state()); EXPECT_EQ(poison, output);
      SQLLEN length = 73;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, type, output.data() + 1, 0, &length));
      EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(poison, output);
      ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1, type));
    }
    const std::string zoned = std::string(test.text) + "+02";
    ASSERT_NO_FATAL_FAILURE(temporal_position(test.type, zoned.c_str()));
    std::array<char, 64> output{}; output.fill('Z'); const auto poison = output;
    SQLLEN length = 73;
    EXPECT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_CHAR, output.data(), test.units, &length));
    EXPECT_EQ("22003", state()); EXPECT_EQ(poison, output); EXPECT_EQ(73, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_CHAR, output.data(), output.size(), &length));
    EXPECT_EQ(zoned, output.data()); EXPECT_EQ(static_cast<SQLLEN>(zoned.size()), length);
    ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
  }
}

TEST_F(FixedTemporalCompletionApiTest, VariableChunksBindingAndOwningRefillStayIndependent) {
  facts->result.columns = {
      {"t", NativeTypeInfo{ScalarType::Time, 15, 3, true}},
      {"s", NativeTypeInfo{ScalarType::Timestamp, 26, 3, true}},
      {"v", NativeTypeInfo{ScalarType::VarChar, 26, 0, true}},
      {"b", NativeTypeInfo{ScalarType::Binary, 6, 0, true}},
      {"d", NativeTypeInfo{ScalarType::Date, 10, 0, true}}};
  facts->result.rows = {{"12:34:56.789", "2026-10-12 12:34:56.789", "12:34:56.789", "abcdef", "2026-10-12"},
                        {"23:45:56.123", "2026-10-13 23:45:56.123", "23:45:56.123", "ghijkl", "2026-10-13"}};
  std::array<char, 9> bound{}; SQLLEN bound_length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_CHAR, bound.data(), bound.size(), &bound_length));
  ASSERT_NO_FATAL_FAILURE(execute());
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt));
  EXPECT_STREQ("12:34:56", bound.data()); EXPECT_EQ(12, bound_length);
  std::array<char, 64> output{}; SQLLEN length = 73;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_CHAR, output.data(), 9, &length));
  EXPECT_STREQ("12:34:56", output.data()); EXPECT_EQ(12, length);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 3, SQL_C_CHAR, output.data(), 9, &length));
  EXPECT_STREQ("12:34:56", output.data()); EXPECT_EQ(12, length);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_CHAR, output.data(), 5, &length));
  EXPECT_STREQ(".789", output.data()); EXPECT_EQ(4, length);
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 3));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_CHAR, output.data(), 9, &length));
  EXPECT_STREQ("12:34:56", output.data());
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
  std::array<char, 3> bytes{};
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 4, SQL_C_BINARY, bytes.data(), bytes.size(), &length));
  EXPECT_EQ((std::array<char, 3>{'a', 'b', 'c'}), bytes); EXPECT_EQ(6, length);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 4, SQL_C_BINARY, bytes.data(), bytes.size(), &length));
  EXPECT_EQ((std::array<char, 3>{'d', 'e', 'f'}), bytes); EXPECT_EQ(3, length);
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 5, SQL_C_CHAR, output.data(), output.size(), &length));
  EXPECT_STREQ("2026-10-12", output.data()); ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 5));
  struct Sibling { SQLHSTMT handle{}; ~Sibling() { if (handle) SQLFreeHandle(SQL_HANDLE_STMT, handle); } } sibling;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling.handle));
  facts->result.columns = {{"s", NativeTypeInfo{ScalarType::Timestamp, 26, 3, true}}};
  facts->result.rows = {{"2030-01-02 01:02:03.456"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling.handle, (SQLCHAR*)"SELECT sibling", SQL_NTS));
  facts->result.rows = {{"poison"}};
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling.handle));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(sibling.handle, 1, SQL_C_CHAR, output.data(), 20, &length));
  EXPECT_STREQ("2030-01-02 01:02:03", output.data());
  ASSERT_NO_FATAL_FAILURE(expect_completed(sibling.handle, 1));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLFetch(stmt));
  EXPECT_STREQ("23:45:56", bound.data()); EXPECT_EQ(12, bound_length);
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 2, SQL_C_CHAR, output.data(), 20, &length));
  EXPECT_STREQ("2026-10-13 23:45:56", output.data());
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 2));
  ASSERT_EQ(SQL_NO_DATA, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLCloseCursor(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT refill", SQL_NTS));
  facts->result.columns = {{"t", NativeTypeInfo{ScalarType::Time, 15, 3, true}}};
  facts->result.rows = {{"02:03:04.567"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); facts->result.rows = {{"poison"}};
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_CHAR, output.data(), 9, &length));
  EXPECT_STREQ("02:03:04", output.data());
  ASSERT_NO_FATAL_FAILURE(expect_completed(stmt, 1));
  ASSERT_NO_FATAL_FAILURE(expect_completed(sibling.handle, 1)); EXPECT_EQ(0u, facts->disconnects);
}
TEST_F(NumericScaleApiTest, ApdTemporalPairMutationRejectsBeforeDispatchAndRepairs) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  SQLLEN length = SQL_NTS;
  SQLUSMALLINT status = 71;
  SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  char text[] = "42";
  SQLWCHAR wide[] = {'4', '3', 0};
  SQL_DATE_STRUCT date{2026, 10, 12};
  SQL_TIME_STRUCT time{12, 34, 56};
  SQL_TIMESTAMP_STRUCT timestamp{2026, 10, 12, 12, 34, 56, 0};
  struct Input { SQLSMALLINT type; void* data; };
  const Input temporal[] = {{SQL_C_TYPE_DATE, &date}, {SQL_C_DATE, &date},
      {SQL_C_TYPE_TIME, &time}, {SQL_C_TIME, &time},
      {SQL_C_TYPE_TIMESTAMP, &timestamp}, {SQL_C_TIMESTAMP, &timestamp}};
  for (const auto& input : temporal) {
    SCOPED_TRACE(input.type);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
        SQL_INTEGER, 10, 0, text, sizeof(text), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
    const auto retained = facts->parameters[0].value;
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    // This scalar fixture publishes no parameter metadata; restore the SQL hint
    // after its successful control before testing a descriptor-only C retype.
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
        SQL_INTEGER, 10, 0, text, sizeof(text), &length));
    EXPECT_EQ(SQL_ERROR, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, input.type,
        SQL_INTEGER, 10, 0, input.data, 0, &length));
    EXPECT_EQ("07006", state());
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(input.type)), 0));
    SQLPOINTER pointer = text;
    ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_DATA_PTR, &pointer, 0, nullptr));
    EXPECT_EQ(nullptr, pointer);
    ASSERT_EQ(SQL_ERROR, SQLExecute(stmt));
    EXPECT_EQ("07009", state()); // Missing non-NULL binding precedes pair eligibility.
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, input.data, 0));
    const unsigned before = facts->queries;
    status = 71; processed = 91;
    const auto result = SQLExecute(stmt);
    EXPECT_EQ(SQL_ERROR, result);
    if (result == SQL_ERROR) { EXPECT_EQ("07006", state()); }
    EXPECT_EQ(before, facts->queries);
    EXPECT_EQ(retained, facts->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(2026, date.year); EXPECT_EQ(12, time.hour); EXPECT_EQ(0u, timestamp.fraction);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_WCHAR,
        SQL_INTEGER, 10, 0, wide, sizeof(wide), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(std::optional<std::string>{"43"}, facts->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  SQLINTEGER number = 42; length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT,
      SQL_INTEGER, 10, 0, &number, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  for (const SQLSMALLINT target : {SQLSMALLINT(SQL_CHAR), SQLSMALLINT(SQL_TYPE_DATE), SQLSMALLINT(SQL_TYPE_TIMESTAMP)}) {
    SCOPED_TRACE(target);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_TYPE_DATE,
        target, 32, 0, &date, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(std::optional<std::string>{target == SQL_TYPE_TIMESTAMP
        ? "2026-10-12 00:00:00" : "2026-10-12"}, facts->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
}

TEST_F(NumericScaleApiTest, ApdNumericPairMutationRejectsBeforeFormattingAndRepairs) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  char date_text[32] = "2026-10-12";
  SQL_NUMERIC_STRUCT numeric{}; numeric.precision = 1; numeric.sign = 1; numeric.val[0] = 7;
  SQLLEN length = SQL_NTS;
  for (const SQLSMALLINT target : {SQLSMALLINT(SQL_TYPE_DATE), SQLSMALLINT(SQL_VARBINARY)}) {
    SCOPED_TRACE(target);
    char binary_text[32] = "37";
    auto* text = target == SQL_TYPE_DATE ? date_text : binary_text;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
        target, 32, 0, text, 32, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, facts->parameters.size());
    const auto retained = facts->parameters[0].value;
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
        target, 32, 0, text, 32, &length));
    ASSERT_EQ(SQL_ERROR, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_NUMERIC,
        target, 32, 0, &numeric, sizeof(numeric), &length));
    EXPECT_EQ("07006", state());
    dimensions(apd, 1, 1, 0, &numeric);
    const unsigned before = facts->queries;
    const auto result = SQLExecute(stmt);
    EXPECT_EQ(SQL_ERROR, result);
    if (result == SQL_ERROR) { EXPECT_EQ("07006", state()); }
    EXPECT_EQ(before, facts->queries); EXPECT_EQ(retained, facts->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    numeric.sign = 2; // Unsupported pair wins over malformed payload.
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
        target, 32, 0, text, 32, &length));
    dimensions(apd, 1, 1, 0, &numeric);
    const auto malformed = SQLExecute(stmt);
    EXPECT_EQ(SQL_ERROR, malformed);
    if (malformed == SQL_ERROR) { EXPECT_EQ("07006", state()); }
    EXPECT_EQ(before, facts->queries);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    numeric.sign = 1;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
        target, 32, 0, text, 32, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(retained, facts->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_NUMERIC,
      SQL_INTEGER, 10, 0, &numeric, sizeof(numeric), &length));
  dimensions(apd, 1, 1, 0, &numeric);
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"7"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  numeric.sign = 2;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_NUMERIC,
      SQL_INTEGER, 10, 0, &numeric, sizeof(numeric), &length));
  dimensions(apd, 1, 1, 0, &numeric);
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("22003", state());
}

TEST_F(NumericScaleApiTest, ApdPairNullUnalignedAndSpanPrecedence) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  SQL_DATE_STRUCT date{2026, 10, 12};
  std::array<unsigned char, sizeof(date) + 2> unaligned{};
  unaligned.front() = 0xa5; unaligned.back() = 0x5a;
  std::memcpy(unaligned.data() + 1, &date, sizeof(date));
  const auto original = unaligned;
  char text[] = "42";
  SQLLEN indicator = SQL_NULL_DATA;
  SQLUSMALLINT status = 71; SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_INTEGER, 10, 0, text, sizeof(text), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_TYPE_DATE), 0));
  // NULL may legitimately have no DataPtr. Illegal pair still must be diagnosed.
  const unsigned before = facts->queries;
  const auto result = SQLExecute(stmt);
  EXPECT_EQ(SQL_ERROR, result);
  if (result == SQL_ERROR) { EXPECT_EQ("07006", state()); }
  EXPECT_EQ(before, facts->queries);
  EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  auto* poison = reinterpret_cast<SQLPOINTER>((std::numeric_limits<std::uintptr_t>::max)());
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_TYPE_DATE,
      SQL_TYPE_DATE, 10, 0, poison, 0, &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size()); EXPECT_FALSE(facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  indicator = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, unaligned.data() + 1, 0));
  for (const SQLLEN fixed_length : {SQLLEN(0), SQLLEN(1), SQLLEN(SQL_NTS), SQLLEN(-9)}) {
    indicator = fixed_length;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_TYPE_DATE,
        SQL_TYPE_DATE, 10, 0, unaligned.data() + 1, 0, &indicator));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(std::optional<std::string>{"2026-10-12"}, facts->parameters[0].value);
    EXPECT_EQ(original, unaligned);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  indicator = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_TYPE_DATE,
      SQL_TYPE_DATE, 10, 0, unaligned.data() + 1, 0, &indicator));
  SQLULEN offset = (std::numeric_limits<SQLULEN>::max)();
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, &offset, 0));
  const unsigned span_before = facts->queries; status = 71; processed = 91;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(span_before, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
  EXPECT_EQ(original, unaligned);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, nullptr, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"2026-10-12"}, facts->parameters[0].value);
}

TEST_F(NumericScaleApiTest, BinaryIntegerLiteralLengthRefusalsAndRecovery) {
  static_assert(sizeof(SQLINTEGER) == 4);
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLINTEGER integer = 42;
  std::array<unsigned char, sizeof(SQLINTEGER) + 3> bytes{};
  bytes.front() = 0xa5; bytes.back() = 0x5a;
  std::memcpy(bytes.data() + 1, &integer, sizeof(integer));
  const auto original = bytes;
  SQLLEN length = sizeof(integer);
  SQLUSMALLINT status = 71; SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  for (const SQLLEN wrong : {SQLLEN(0), SQLLEN(1), SQLLEN(3), SQLLEN(5)}) {
    SCOPED_TRACE(wrong);
    length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
        SQL_INTEGER, 10, 0, &integer, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    const auto retained = facts->parameters[0].value;
    length = wrong; status = 71; processed = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_INTEGER, 10, 0, bytes.data() + 1, sizeof(integer) + 1, &length));
    const unsigned before = facts->queries;
    const auto result = SQLExecute(stmt);
    EXPECT_EQ(SQL_ERROR, result);
    if (result == SQL_ERROR) { EXPECT_EQ("22003", state()); }
    EXPECT_EQ(before, facts->queries); EXPECT_EQ(retained, facts->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(wrong, length); EXPECT_EQ(original, bytes);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    length = sizeof(integer);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_INTEGER, 10, 0, bytes.data() + 1, sizeof(integer), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
    EXPECT_EQ(QueryParameterType::Int32, facts->parameters[0].type);
    EXPECT_FALSE(facts->parameters[0].binary_input);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  for (const SQLLEN invalid : {SQLLEN(SQL_NTS), SQLLEN(-9)}) {
    SCOPED_TRACE(invalid); length = invalid;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_INTEGER, 10, 0, bytes.data() + 1, sizeof(integer), &length));
    const unsigned before = facts->queries;
    ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY090", state());
    EXPECT_EQ(before, facts->queries); EXPECT_EQ(original, bytes);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr,
      (SQLCHAR*)"SERVER=fixture;PORT=5439;DATABASE=fixture;UID=test;SSL=0;MAXPARAMETERBYTES=3;MAXPARAMETERTOTALBYTES=3",
      SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  length = 5;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_INTEGER, 10, 0, bytes.data() + 1, 5, &length));
  const unsigned before = facts->queries;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY000", state());
  EXPECT_EQ(before, facts->queries); EXPECT_EQ(original, bytes);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  integer = 7; length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_INTEGER, 10, 0, &integer, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"7"}, facts->parameters[0].value);
}

TEST_F(NumericScaleApiTest, BinaryIntegerExactHostBytesUnalignedNullAndAuthority) {
  static_assert(sizeof(SQLINTEGER) == 4);
  static_assert((std::numeric_limits<SQLINTEGER>::min)() == -2147483647 - 1);
  static_assert((std::numeric_limits<SQLINTEGER>::max)() == 2147483647);
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLLEN length = sizeof(SQLINTEGER);
  std::array<unsigned char, sizeof(SQLINTEGER) + 2> bytes{};
  for (const SQLINTEGER value : {SQLINTEGER(0), SQLINTEGER(42), SQLINTEGER(-42),
      (std::numeric_limits<SQLINTEGER>::min)(), (std::numeric_limits<SQLINTEGER>::max)()}) {
    SCOPED_TRACE(value); bytes.front() = 0xa5; bytes.back() = 0x5a;
    std::memcpy(bytes.data() + 1, &value, sizeof(value)); const auto original = bytes;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_INTEGER, 10, 0, bytes.data() + 1, sizeof(value), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(QueryParameterType::Int32, facts->parameters[0].type);
    EXPECT_EQ(std::optional<std::string>{std::to_string(value)}, facts->parameters[0].value);
    EXPECT_FALSE(facts->parameters[0].binary_input); EXPECT_EQ(original, bytes);
    const auto retained = facts->parameters[0].value;
    std::fill(bytes.begin() + 1, bytes.end() - 1, 0xee);
    EXPECT_EQ(retained, facts->parameters[0].value);
    EXPECT_EQ(0xa5, bytes.front()); EXPECT_EQ(0x5a, bytes.back());
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  SQLINTEGER number = 42; std::memcpy(bytes.data() + 1, &number, sizeof(number));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_INTEGER, 10, 0, bytes.data() + 1, sizeof(number), nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  SQLLEN indicator = 1, octets = sizeof(number);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_INTEGER, 10, 0, bytes.data() + 1, sizeof(number), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_OCTET_LENGTH_PTR, &octets, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_EQ(1, indicator); EXPECT_EQ(static_cast<SQLLEN>(sizeof(number)), octets);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  auto* poison = reinterpret_cast<SQLPOINTER>((std::numeric_limits<std::uintptr_t>::max)());
  indicator = SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_INTEGER, 10, 0, poison, 0, &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_OCTET_LENGTH_PTR, poison, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_FALSE(facts->parameters[0].value); EXPECT_FALSE(facts->parameters[0].binary_input);
  EXPECT_EQ(QueryParameterType::Int32, facts->parameters[0].type);
  EXPECT_EQ(SQL_NULL_DATA, indicator);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_OCTET_LENGTH_PTR, nullptr, 0));
  char text[] = "42"; indicator = SQL_NTS;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_INTEGER, 10, 0, text, sizeof(text), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_BINARY), 0));
  SQLPOINTER cleared = text;
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_DATA_PTR, &cleared, 0, nullptr));
  EXPECT_EQ(nullptr, cleared);
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("07009", state());
  indicator = sizeof(number);
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_OCTET_LENGTH, reinterpret_cast<SQLPOINTER>(sizeof(number)), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, bytes.data() + 1, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  indicator = 1;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT,
      SQL_INTEGER, 10, 0, &number, 0, &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  indicator = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_VARBINARY, sizeof(number), 0, bytes.data() + 1, sizeof(number), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_TRUE(facts->parameters[0].value); EXPECT_TRUE(facts->parameters[0].value->empty());
  EXPECT_TRUE(facts->parameters[0].binary_input);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  indicator = sizeof(number);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_VARBINARY, sizeof(number), 0, bytes.data() + 1, sizeof(number), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{std::string(reinterpret_cast<char*>(bytes.data() + 1), sizeof(number))}, facts->parameters[0].value);
  EXPECT_TRUE(facts->parameters[0].binary_input); EXPECT_EQ(QueryParameterType::Binary, facts->parameters[0].type);
}

TEST_F(NumericScaleApiTest, BinaryIntegerDeferredLayoutSiblingAndReuse) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLUSMALLINT status = 71; SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  auto* token = reinterpret_cast<SQLPOINTER>((std::numeric_limits<std::uintptr_t>::max)());
  SQLLEN length{};
  for (const SQLLEN deferred : {SQLLEN(SQL_DATA_AT_EXEC), SQLLEN(SQL_LEN_DATA_AT_EXEC(4))}) {
    SCOPED_TRACE(deferred); length = deferred; status = 71; processed = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_INTEGER, 10, 0, token, 0, &length));
    const unsigned before = facts->queries;
    ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
    EXPECT_EQ(before, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
    EXPECT_EQ(deferred, length);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  struct Sibling { SQLHSTMT handle{}; ~Sibling() { if (handle) SQLFreeHandle(SQL_HANDLE_STMT, handle); } } sibling;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling.handle));
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling.handle, (SQLCHAR*)"SELECT sibling", SQL_NTS));
  SQLINTEGER number = -42;
  std::array<unsigned char, sizeof(number) + 2> bytes{};
  std::array<unsigned char, sizeof(SQLLEN) + 2> lengths{};
  bytes.front() = lengths.front() = 0xa5; bytes.back() = lengths.back() = 0x5a;
  std::memcpy(bytes.data() + 1, &number, sizeof(number)); length = sizeof(number);
  std::memcpy(lengths.data() + 1, &length, sizeof(length));
  const auto original = bytes; const auto original_lengths = lengths;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_INTEGER, 10, 0, bytes.data() + 1, sizeof(number), reinterpret_cast<SQLLEN*>(lengths.data() + 1)));
  SQLULEN offset = (std::numeric_limits<SQLULEN>::max)();
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, &offset, 0));
  const unsigned before = facts->queries; status = 71; processed = 91;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(before, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
  EXPECT_EQ(original, bytes); EXPECT_EQ(original_lengths, lengths);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, nullptr, 0));
  facts->result.rows = {{"poison"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"-42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  EXPECT_EQ(original, bytes); EXPECT_EQ(original_lengths, lengths);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling.handle));
  SQLINTEGER output = 71; SQLLEN output_length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(sibling.handle, 1, SQL_C_SLONG, &output, 0, &output_length));
  EXPECT_EQ(1, output); EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), output_length);
  EXPECT_EQ(0u, facts->disconnects);
}


// Literal public baseline proposal; append inside the existing anonymous namespace.
// No execution or qualification is claimed. Root owns application and build.
TEST_F(NumericScaleApiTest, NumericTemporalInputLeadRejectsBindAndEffectiveApdBeforeDispatch) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLINTEGER number = 42;
  SQLLEN length = 0;
  const auto before_bind = facts->queries;
  const auto bound = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
      SQL_TYPE_DATE, 10, 0, &number, 0, &length);
  EXPECT_EQ(SQL_ERROR, bound);
  if (bound == SQL_ERROR) { EXPECT_EQ("07006", state()); }
  EXPECT_EQ(before_bind, facts->queries);

  char date_text[] = "2026-10-12";
  length = SQL_NTS;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_TYPE_DATE, 10, 0, date_text, sizeof(date_text), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"2026-10-12"}, facts->parameters[0].value);
  const auto retained = facts->parameters[0].value;
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  // Successful recording-provider controls publish no parameter metadata;
  // rebind the original SQL hint before making only an APD C-type edit.
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_TYPE_DATE, 10, 0, date_text, sizeof(date_text), &length));
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_SLONG), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, &number, 0));
  SQLUSMALLINT status = 71;
  SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  const auto before_execute = facts->queries;
  const auto executed = SQLExecute(stmt);
  EXPECT_EQ(SQL_ERROR, executed);
  if (executed == SQL_ERROR) { EXPECT_EQ("07006", state()); }
  EXPECT_EQ(before_execute, facts->queries);
  EXPECT_EQ(SQL_PARAM_ERROR, status);
  EXPECT_EQ(1u, processed);
  EXPECT_EQ(retained, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_TYPE_DATE, 10, 0, date_text, sizeof(date_text), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(retained, facts->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status);
  EXPECT_EQ(1u, processed);
  EXPECT_EQ(0u, facts->disconnects);
}

// Unrun public test proposals for the frozen contract; no implementation change.
// Append inside test_numeric_scale_conversion.cpp's existing anonymous namespace.
TEST_F(NumericScaleApiTest, NumericTemporalInputAllOrdinaryFamiliesRejectAndLegalControlsRetainBindings) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLSCHAR tiny = -7;
  SQLCHAR utiny = 255;
  SQLSMALLINT small_value = -32768;
  SQLUSMALLINT usmall = 65535;
  SQLINTEGER number = 42;
  SQLUINTEGER unumber = 4294967295u;
  SQLBIGINT big = -9223372036854775807LL;
  SQLUBIGINT ubig = 18446744073709551615ULL;
  SQLREAL real = 1.25f;
  SQLDOUBLE wide = 2.5;
  struct Input { SQLSMALLINT type; void* data; const char* text; };
  const Input inputs[] = {
      {SQL_C_STINYINT, &tiny, "-7"}, {SQL_C_TINYINT, &tiny, "-7"},
      {SQL_C_UTINYINT, &utiny, "255"}, {SQL_C_SSHORT, &small_value, "-32768"},
      {SQL_C_SHORT, &small_value, "-32768"}, {SQL_C_USHORT, &usmall, "65535"},
      {SQL_C_SLONG, &number, "42"}, {SQL_C_LONG, &number, "42"},
      {SQL_C_ULONG, &unumber, "4294967295"},
      {SQL_C_SBIGINT, &big, "-9223372036854775807"},
      {SQL_C_UBIGINT, &ubig, "18446744073709551615"},
      {SQL_C_FLOAT, &real, "1.25"}, {SQL_C_DOUBLE, &wide, "2.5"}};
  const SQLSMALLINT targets[] = {SQL_TYPE_DATE, SQL_TYPE_TIME, SQL_TYPE_TIMESTAMP};
  SQLLEN length = 0;
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  SQLUSMALLINT status = 71;
  SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  char previous[] = "keep";
  for (const auto& input : inputs) {
    SCOPED_TRACE(input.type);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, input.type,
        SQL_VARCHAR, 32, 0, input.data, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(std::optional<std::string>{input.text}, facts->parameters[0].value);
    EXPECT_EQ(QueryParameterType::Text, facts->parameters[0].type);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    for (const auto target : targets) {
      SCOPED_TRACE(target);
      length = SQL_NTS;
      ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
          SQL_VARCHAR, 32, 0, previous, sizeof(previous), &length));
      const auto queries = facts->queries;
      status = 71; processed = 91;
      const auto result = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, input.type,
          target, 32, 0, input.data, 0, &length);
      EXPECT_EQ(SQL_ERROR, result);
      if (result == SQL_ERROR) { EXPECT_EQ("07006", state()); }
      EXPECT_EQ(queries, facts->queries);
      EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
      SQLSMALLINT c_type = 0;
      SQLPOINTER pointer = nullptr;
      ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
          &c_type, 0, nullptr));
      ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_DATA_PTR,
          &pointer, 0, nullptr));
      EXPECT_EQ(SQL_C_CHAR, c_type); EXPECT_EQ(previous, pointer);
    }
    length = 0;
  }
  // Pair eligibility is independent of nonfinite numeric payload validity.
  real = (std::numeric_limits<SQLREAL>::infinity)();
  wide = (std::numeric_limits<SQLDOUBLE>::quiet_NaN)();
  for (const auto& input : {Input{SQL_C_FLOAT, &real, ""}, Input{SQL_C_DOUBLE, &wide, ""}}) {
    for (const auto target : targets) {
      const auto result = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, input.type,
          target, 32, 0, input.data, 0, &length);
      EXPECT_EQ(SQL_ERROR, result);
      if (result == SQL_ERROR) { EXPECT_EQ("07006", state()); }
    }
  }
  SQL_DATE_STRUCT date{2030, 1, 2};
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT,
      SQL_TYPE_DATE, 10, 0, &date, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"2030-01-02"}, facts->parameters[0].value);
  EXPECT_EQ(QueryParameterType::Date, facts->parameters[0].type);
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, NumericTemporalInputEffectiveApdNullMasksFixedLengthsAndRecovery) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  struct Sibling {
    SQLHSTMT handle{};
    ~Sibling() { if (handle) SQLFreeHandle(SQL_HANDLE_STMT, handle); }
  } sibling;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling.handle));
  facts->result.columns = {{"saved", NativeTypeInfo{ScalarType::Integer, 10, 0, true}}};
  facts->result.rows = {{"73"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling.handle, (SQLCHAR*)"SELECT saved", SQL_NTS));
  facts->result.rows = {{"99"}}; // The sibling must keep its earlier owning result.
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  SQLINTEGER number = 42;
  SQLLEN length = 0;
  std::array<unsigned char, sizeof(number) + 2> packed{};
  packed.front() = 0xa5; packed.back() = 0x5a;
  std::memcpy(packed.data() + 1, &number, sizeof(number));
  const auto original = packed;
  SQLUSMALLINT status = 71, operation = SQL_PARAM_PROCEED;
  SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_OPERATION_PTR, &operation, 0));
  for (const SQLLEN fixed_length : {SQLLEN(0), SQLLEN(1), SQLLEN(SQL_NTS), SQLLEN(-9)}) {
    length = fixed_length;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG,
        SQL_INTEGER, 10, 0, packed.data() + 1, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
    EXPECT_EQ(QueryParameterType::Int32, facts->parameters[0].type);
    EXPECT_EQ(original, packed);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  struct Target { SQLSMALLINT type; const char* text; };
  const Target targets[] = {{SQL_TYPE_DATE, "2030-01-02"},
      {SQL_TYPE_TIME, "03:04:05"}, {SQL_TYPE_TIMESTAMP, "2030-01-02 03:04:05"}};
  SQLREAL real = 1.25f;
  SQLDOUBLE wide = (std::numeric_limits<SQLDOUBLE>::quiet_NaN)();
  struct Input { SQLSMALLINT type; void* data; };
  const Input inputs[] = {{SQL_C_SLONG, packed.data() + 1},
      {SQL_C_FLOAT, &real}, {SQL_C_DOUBLE, &wide}};
  const auto retype = [&](const Target& target, const Input& input) {
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
        target.type, 32, 0, const_cast<char*>(target.text), 32, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(static_cast<std::intptr_t>(input.type)), 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, input.data, 0));
  };
  for (const auto& target : targets) {
    for (const auto& input : inputs) {
      SCOPED_TRACE(target.type);
      SCOPED_TRACE(input.type);
      for (const SQLLEN marker : {SQLLEN(0), SQLLEN(SQL_NULL_DATA)}) {
        length = marker;
        const auto actual = marker == SQL_NULL_DATA ? Input{input.type,
            reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)())} : input;
        ASSERT_NO_FATAL_FAILURE(retype(target, actual));
        const auto queries = facts->queries;
        const auto retained = facts->parameters[0].value;
        const auto retained_type = facts->parameters[0].type;
        status = 71; processed = 91;
        const auto result = SQLExecute(stmt);
        EXPECT_EQ(SQL_ERROR, result);
        if (result == SQL_ERROR) { EXPECT_EQ("07006", state()); }
        EXPECT_EQ(queries, facts->queries);
        EXPECT_EQ(retained, facts->parameters[0].value);
        EXPECT_EQ(retained_type, facts->parameters[0].type);
        EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
        EXPECT_EQ(original, packed);
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
      }
    }
  }
  length = 0;
  ASSERT_NO_FATAL_FAILURE(retype(targets[0], inputs[0]));
  operation = SQL_PARAM_IGNORE;
  const auto queries = facts->queries;
  status = 71; processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(SQL_PARAM_UNUSED, status); EXPECT_EQ(0u, processed);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  operation = SQL_PARAM_PROCEED;
  // Fixed numeric deferred admission precedes ordinary pair materialization.
  length = SQL_DATA_AT_EXEC; status = 71; processed = 91;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
  length = 0;
  SQLULEN offset = (std::numeric_limits<SQLULEN>::max)();
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, &offset, 0));
  status = 71; processed = 91;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, nullptr, 0));
  SQL_DATE_STRUCT date{2030, 1, 2};
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT,
      SQL_TYPE_DATE, 10, 0, &date, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"2030-01-02"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  length = SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT,
      SQL_TYPE_DATE, 10, 0, reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)()), 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); EXPECT_FALSE(facts->parameters[0].value);
  EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed); EXPECT_EQ(original, packed);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling.handle));
  SQLINTEGER saved = 0; SQLLEN saved_length = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(sibling.handle, 1, SQL_C_SLONG, &saved, 0, &saved_length));
  EXPECT_EQ(73, saved); EXPECT_EQ(static_cast<SQLLEN>(sizeof(saved)), saved_length);
  EXPECT_EQ(SQL_NO_DATA, SQLFetch(sibling.handle));
  EXPECT_EQ(0u, facts->disconnects);
}


TEST_F(NumericScaleApiTest, BinaryBigintLiteralLengthRefusalAndOwningRecovery) {
  static_assert(sizeof(SQLBIGINT) == 8);
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLBIGINT number = -42;
  std::array<unsigned char, sizeof(SQLBIGINT) + 3> bytes{};
  bytes.front() = 0xa5; bytes.back() = 0x5a;
  std::memcpy(bytes.data() + 1, &number, sizeof(number));
  const auto original = bytes;
  SQLLEN length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT,
      SQL_BIGINT, 19, 0, &number, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(std::optional<std::string>{"-42"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  const auto retained = facts->parameters[0].value;
  length = 7;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(number), &length));
  const auto before = facts->queries;
  const auto refused = SQLExecute(stmt);
  EXPECT_EQ(SQL_ERROR, refused);
  if (refused == SQL_ERROR) { EXPECT_EQ("22003", state()); }
  EXPECT_EQ(before, facts->queries);
  EXPECT_EQ(retained, facts->parameters[0].value);
  EXPECT_EQ(original, bytes);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  length = sizeof(number);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(number), &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(QueryParameterType::Int64, facts->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"-42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  EXPECT_EQ(original, bytes);
  std::fill(bytes.begin() + 1, bytes.end() - 1, 0xee);
  EXPECT_EQ(std::optional<std::string>{"-42"}, facts->parameters[0].value);
  EXPECT_EQ(0xa5, bytes.front()); EXPECT_EQ(0x5a, bytes.back());
}

TEST_F(NumericScaleApiTest, BinaryBigintLiteralLengthRefusalsAndRecovery) {
  static_assert(sizeof(SQLBIGINT) == 8);
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLBIGINT integer = 42;
  std::array<unsigned char, sizeof(SQLBIGINT) + 3> bytes{};
  bytes.front() = 0xa5; bytes.back() = 0x5a;
  std::memcpy(bytes.data() + 1, &integer, sizeof(integer));
  const auto original = bytes;
  SQLLEN length = sizeof(integer);
  SQLUSMALLINT status = 71; SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  for (const SQLLEN wrong : {SQLLEN(0), SQLLEN(1), SQLLEN(7), SQLLEN(9)}) {
    SCOPED_TRACE(wrong);
    length = 0;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT,
        SQL_BIGINT, 19, 0, &integer, 0, &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    const auto retained = facts->parameters[0].value;
    length = wrong; status = 71; processed = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(integer) + 1, &length));
    const unsigned before = facts->queries;
    const auto result = SQLExecute(stmt);
    EXPECT_EQ(SQL_ERROR, result);
    if (result == SQL_ERROR) { EXPECT_EQ("22003", state()); }
    EXPECT_EQ(before, facts->queries); EXPECT_EQ(retained, facts->parameters[0].value);
    EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    EXPECT_EQ(wrong, length); EXPECT_EQ(original, bytes);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    length = sizeof(integer);
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(integer), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
    EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
    EXPECT_EQ(QueryParameterType::Int64, facts->parameters[0].type);
    EXPECT_FALSE(facts->parameters[0].binary_input);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  for (const SQLLEN invalid : {SQLLEN(SQL_NTS), SQLLEN(-9)}) {
    SCOPED_TRACE(invalid); length = invalid;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(integer), &length));
    const unsigned before = facts->queries;
    ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY090", state());
    EXPECT_EQ(before, facts->queries); EXPECT_EQ(original, bytes);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
  ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
  ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr,
      (SQLCHAR*)"SERVER=fixture;PORT=5439;DATABASE=fixture;UID=test;SSL=0;MAXPARAMETERBYTES=7;MAXPARAMETERTOTALBYTES=7",
      SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  length = 9;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_BIGINT, 19, 0, bytes.data() + 1, 9, &length));
  const unsigned before = facts->queries;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY000", state());
  EXPECT_EQ(before, facts->queries); EXPECT_EQ(original, bytes);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  integer = 7; length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT,
      SQL_BIGINT, 19, 0, &integer, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"7"}, facts->parameters[0].value);
  const SQLBIGINT minimum = (std::numeric_limits<SQLBIGINT>::min)();
  std::memcpy(bytes.data() + 1, &minimum, sizeof(minimum));
  const auto minimum_bytes = bytes;
  const char* configurations[] = {
      "SERVER=fixture;PORT=5439;DATABASE=fixture;UID=test;SSL=0;MAXPARAMETERBYTES=19;MAXPARAMETERTOTALBYTES=20",
      "SERVER=fixture;PORT=5439;DATABASE=fixture;UID=test;SSL=0;MAXPARAMETERBYTES=20;MAXPARAMETERTOTALBYTES=19",
      "SERVER=fixture;PORT=5439;DATABASE=fixture;UID=test;SSL=0;MAXPARAMETERBYTES=20;MAXPARAMETERTOTALBYTES=20"};
  for (std::size_t index = 0; index != 3; ++index) {
    SCOPED_TRACE(index);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeHandle(SQL_HANDLE_STMT, stmt)); stmt = nullptr;
    ASSERT_EQ(SQL_SUCCESS, SQLDisconnect(dbc));
    ASSERT_EQ(SQL_SUCCESS, SQLDriverConnect(dbc, nullptr,
        (SQLCHAR*)configurations[index], SQL_NTS, nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT));
    ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));
    ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
    length = sizeof(minimum); status = 71; processed = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(minimum), &length));
    const auto before_normalization = facts->queries;
    if (index < 2) {
      ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HY000", state());
      EXPECT_EQ(before_normalization, facts->queries);
      EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
    } else {
      ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
      EXPECT_EQ(std::optional<std::string>{"-9223372036854775808"}, facts->parameters[0].value);
      EXPECT_FALSE(facts->parameters[0].binary_input);
      EXPECT_EQ(QueryParameterType::Int64, facts->parameters[0].type);
      EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
    }
    EXPECT_EQ(static_cast<SQLLEN>(sizeof(minimum)), length); EXPECT_EQ(minimum_bytes, bytes);
  }
}

TEST_F(NumericScaleApiTest, BinaryBigintExactHostBytesUnalignedNullAndAuthority) {
  static_assert(sizeof(SQLBIGINT) == 8);
  static_assert((std::numeric_limits<SQLBIGINT>::min)() == -9223372036854775807LL - 1);
  static_assert((std::numeric_limits<SQLBIGINT>::max)() == 9223372036854775807LL);
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLLEN length = sizeof(SQLBIGINT);
  std::array<unsigned char, sizeof(SQLBIGINT) + 2> bytes{};
  for (const SQLBIGINT value : {SQLBIGINT(0), SQLBIGINT(42), SQLBIGINT(-42),
      (std::numeric_limits<SQLBIGINT>::min)(), (std::numeric_limits<SQLBIGINT>::max)()}) {
    SCOPED_TRACE(value); bytes.front() = 0xa5; bytes.back() = 0x5a;
    std::memcpy(bytes.data() + 1, &value, sizeof(value)); const auto original = bytes;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(value), &length));
    ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(1u, facts->parameters.size());
    EXPECT_EQ(QueryParameterType::Int64, facts->parameters[0].type);
    EXPECT_EQ(std::optional<std::string>{std::to_string(value)}, facts->parameters[0].value);
    EXPECT_FALSE(facts->parameters[0].binary_input); EXPECT_EQ(original, bytes);
    const auto retained = facts->parameters[0].value;
    std::fill(bytes.begin() + 1, bytes.end() - 1, 0xee);
    EXPECT_EQ(retained, facts->parameters[0].value);
    EXPECT_EQ(0xa5, bytes.front()); EXPECT_EQ(0x5a, bytes.back());
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  SQLBIGINT number = 42; std::memcpy(bytes.data() + 1, &number, sizeof(number));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(number), nullptr));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  SQLLEN indicator = 1, octets = sizeof(number);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(number), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_OCTET_LENGTH_PTR, &octets, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_EQ(1, indicator); EXPECT_EQ(static_cast<SQLLEN>(sizeof(number)), octets);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  auto* poison = reinterpret_cast<SQLPOINTER>((std::numeric_limits<std::uintptr_t>::max)());
  indicator = SQL_NULL_DATA;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_BIGINT, 19, 0, poison, 0, &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_OCTET_LENGTH_PTR, poison, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_FALSE(facts->parameters[0].value); EXPECT_FALSE(facts->parameters[0].binary_input);
  EXPECT_EQ(QueryParameterType::Int64, facts->parameters[0].type);
  EXPECT_EQ(SQL_NULL_DATA, indicator);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_OCTET_LENGTH_PTR, nullptr, 0));
  char text[] = "42"; indicator = SQL_NTS;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_BIGINT, 19, 0, text, sizeof(text), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_BINARY), 0));
  SQLPOINTER cleared = text;
  ASSERT_EQ(SQL_SUCCESS, SQLGetDescField(apd, 1, SQL_DESC_DATA_PTR, &cleared, 0, nullptr));
  EXPECT_EQ(nullptr, cleared);
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("07009", state());
  indicator = sizeof(number);
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_OCTET_LENGTH, reinterpret_cast<SQLPOINTER>(sizeof(number)), 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, bytes.data() + 1, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  indicator = 1;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT,
      SQL_BIGINT, 19, 0, &number, 0, &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  indicator = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_VARBINARY, sizeof(number), 0, bytes.data() + 1, sizeof(number), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_TRUE(facts->parameters[0].value); EXPECT_TRUE(facts->parameters[0].value->empty());
  EXPECT_TRUE(facts->parameters[0].binary_input);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  indicator = sizeof(number);
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_VARBINARY, sizeof(number), 0, bytes.data() + 1, sizeof(number), &indicator));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{std::string(reinterpret_cast<char*>(bytes.data() + 1), sizeof(number))}, facts->parameters[0].value);
  EXPECT_TRUE(facts->parameters[0].binary_input); EXPECT_EQ(QueryParameterType::Binary, facts->parameters[0].type);
}

TEST_F(NumericScaleApiTest, BinaryBigintDeferredLayoutSiblingAndReuse) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLUSMALLINT status = 71; SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  auto* token = reinterpret_cast<SQLPOINTER>((std::numeric_limits<std::uintptr_t>::max)());
  SQLLEN length{};
  for (const SQLLEN deferred : {SQLLEN(SQL_DATA_AT_EXEC), SQLLEN(SQL_LEN_DATA_AT_EXEC(8))}) {
    SCOPED_TRACE(deferred); length = deferred; status = 71; processed = 91;
    ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
        SQL_BIGINT, 19, 0, token, 0, &length));
    const unsigned before = facts->queries;
    ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
    EXPECT_EQ(before, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
    EXPECT_EQ(deferred, length);
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  struct Sibling { SQLHSTMT handle{}; ~Sibling() { if (handle) SQLFreeHandle(SQL_HANDLE_STMT, handle); } } sibling;
  ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling.handle));
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling.handle, (SQLCHAR*)"SELECT sibling", SQL_NTS));
  SQLBIGINT number = -42;
  std::array<unsigned char, sizeof(number) + 2> bytes{};
  std::array<unsigned char, sizeof(SQLLEN) + 2> lengths{};
  bytes.front() = lengths.front() = 0xa5; bytes.back() = lengths.back() = 0x5a;
  std::memcpy(bytes.data() + 1, &number, sizeof(number)); length = sizeof(number);
  std::memcpy(lengths.data() + 1, &length, sizeof(length));
  const auto original = bytes; const auto original_lengths = lengths;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BINARY,
      SQL_BIGINT, 19, 0, bytes.data() + 1, sizeof(number), reinterpret_cast<SQLLEN*>(lengths.data() + 1)));
  SQLULEN offset = (std::numeric_limits<SQLULEN>::max)();
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, &offset, 0));
  const unsigned before = facts->queries; status = 71; processed = 91;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(before, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
  EXPECT_EQ(original, bytes); EXPECT_EQ(original_lengths, lengths);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, nullptr, 0));
  facts->result.rows = {{"poison"}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(std::optional<std::string>{"-42"}, facts->parameters[0].value);
  EXPECT_FALSE(facts->parameters[0].binary_input);
  EXPECT_EQ(original, bytes); EXPECT_EQ(original_lengths, lengths);
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(sibling.handle));
  SQLINTEGER output = 71; SQLLEN output_length = 73;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(sibling.handle, 1, SQL_C_SLONG, &output, 0, &output_length));
  EXPECT_EQ(1, output); EXPECT_EQ(static_cast<SQLLEN>(sizeof(output)), output_length);
  EXPECT_EQ(0u, facts->disconnects);
}




// Effective APD inputs must obey the same pair policy as SQLBindParameter.
TEST_F(NumericScaleApiTest, BitInputPairEffectiveApdSixTargetsRejectBeforeMaterialAndNullDispatch) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLCHAR bytes[]{0x5a, 1, 0xa5}; SQLLEN length = 0;
  SQLUSMALLINT status = 71; SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BIT,
      SQL_INTEGER, 10, 0, bytes + 1, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(QueryParameterType::Int32, facts->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"1"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  const auto retained = facts->parameters[0];
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  char text[] = "2030-01-02 03:04:05";
  for (const SQLSMALLINT target : {SQLSMALLINT(SQL_TYPE_DATE), SQLSMALLINT(SQL_TYPE_TIME),
       SQLSMALLINT(SQL_TYPE_TIMESTAMP), SQLSMALLINT(SQL_BINARY), SQLSMALLINT(SQL_VARBINARY),
       SQLSMALLINT(SQL_LONGVARBINARY)}) {
    SCOPED_TRACE(target);
    for (const int scenario : {0, 1, 2}) {
      SCOPED_TRACE(scenario); bytes[1] = scenario == 1 ? 255 : 1;
      length = scenario == 2 ? SQL_NULL_DATA : 0;
      // Keep the declared target legal at bind; only the effective APD C type changes.
      ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
          target, 32, 0, text, sizeof(text), &length));
      ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
          reinterpret_cast<SQLPOINTER>(SQL_C_BIT), 0));
      void* payload = scenario == 2
          ? reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)())
          : bytes + 1;
      ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, payload, 0));
      const auto queries = facts->queries;
      status = 71; processed = 91;
      const auto refused = SQLExecute(stmt);
      EXPECT_EQ(SQL_ERROR, refused);
      if (refused == SQL_ERROR) { EXPECT_EQ("07006", state()); }
      EXPECT_EQ(queries, facts->queries); EXPECT_EQ(SQL_PARAM_ERROR, status); EXPECT_EQ(1u, processed);
      ASSERT_EQ(1u, facts->parameters.size());
      EXPECT_EQ(retained.type, facts->parameters[0].type); EXPECT_EQ(retained.value, facts->parameters[0].value);
      EXPECT_EQ(0x5a, bytes[0]); EXPECT_EQ(scenario == 1 ? 255 : 1, bytes[1]); EXPECT_EQ(0xa5, bytes[2]);
      EXPECT_EQ(scenario == 2 ? SQL_NULL_DATA : 0, length);
      ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
      // Refill the original legal binding even after an original baseline's success.
      bytes[1] = 1; length = 0;
      ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_BIT,
          SQL_INTEGER, 10, 0, bytes + 1, 0, &length));
      ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
      EXPECT_EQ(retained.type, facts->parameters[0].type); EXPECT_EQ(retained.value, facts->parameters[0].value);
      EXPECT_EQ(SQL_PARAM_SUCCESS, status); EXPECT_EQ(1u, processed);
      ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
    }
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, BitInputPairMaskBindingDeferredOffsetAndDefaultPrecedenceRemainDistinct) {
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  char date_text[] = "2030-01-02"; SQLLEN length = 0;
  SQLCHAR bytes[]{0x5a, 1, 0xa5};
  SQLUSMALLINT status = 71, operation = SQL_PARAM_IGNORE; SQLULEN processed = 91;
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_STATUS_PTR, &status, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_OPERATION_PTR, &operation, 0));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR,
      SQL_TYPE_DATE, 10, 0, date_text, sizeof(date_text), &length));
  auto apd = descriptor(SQL_ATTR_APP_PARAM_DESC);
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_CONCISE_TYPE,
      reinterpret_cast<SQLPOINTER>(SQL_C_BIT), 0));
  // Ignored inputs must not read even an invalid ordinary payload address.
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, reinterpret_cast<void*>(1), 0));
  const auto queries = facts->queries;
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(SQL_PARAM_UNUSED, status); EXPECT_EQ(0u, processed);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, bytes + 1, 0));
  operation = SQL_PARAM_PROCEED; length = SQL_DATA_AT_EXEC; status = 71; processed = 91;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
  length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, nullptr, 0));
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("07009", state());
  EXPECT_EQ(queries, facts->queries);
  ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(apd, 1, SQL_DESC_DATA_PTR, bytes + 1, 0));
  SQLULEN offset = (std::numeric_limits<SQLULEN>::max)();
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, &offset, 0));
  status = 71; processed = 91;
  ASSERT_EQ(SQL_ERROR, SQLExecute(stmt)); EXPECT_EQ("HYC00", state());
  EXPECT_EQ(queries, facts->queries); EXPECT_EQ(71, status); EXPECT_EQ(91u, processed);
  ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, nullptr, 0));
  SQL_DATE_STRUCT date{2030, 1, 2};
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT,
      SQL_TYPE_DATE, 10, 0, &date, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  ASSERT_EQ(1u, facts->parameters.size());
  EXPECT_EQ(QueryParameterType::Date, facts->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"2030-01-02"}, facts->parameters[0].value);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_DEFAULT,
      SQL_BIT, 1, 0, bytes + 1, 0, &length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt));
  EXPECT_EQ(QueryParameterType::Boolean, facts->parameters[0].type);
  EXPECT_EQ(std::optional<std::string>{"1"}, facts->parameters[0].value);
  EXPECT_EQ(0x5a, bytes[0]); EXPECT_EQ(1, bytes[1]); EXPECT_EQ(0xa5, bytes[2]);
  EXPECT_EQ(0u, facts->disconnects);
}

}  // namespace

namespace {
TEST_F(NumericScaleApiTest, SignedIntegerBinaryResultLiteralWidthsRetryCompletionAndNull) {
  struct Case { ScalarType type; SQLLEN width; const char* minimum; const char* maximum; };
  for (const auto& c : {Case{ScalarType::SmallInt, 2, "-32768", "32767"},
       Case{ScalarType::Integer, 4, "-2147483648", "2147483647"},
       Case{ScalarType::BigInt, 8, "-9223372036854775808", "9223372036854775807"}}) {
    SCOPED_TRACE(c.width);
    facts->result.columns = {{"n", NativeTypeInfo{c.type, 19, 0, true}}};
    facts->result.rows = {{c.minimum}, {"0"}, {c.maximum}, {std::nullopt}};
    ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS));
    for (int row = 0; row < 4; ++row) {
      ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
      std::array<unsigned char, 12> output; output.fill(0xa5); SQLLEN length = 79;
      const auto before = output;
      if (row < 3) {
        for (SQLLEN capacity = 0; capacity < c.width; ++capacity) {
          ASSERT_EQ(SQL_ERROR, SQLGetData(stmt, 1, SQL_C_BINARY, output.data() + 1, capacity, &length));
          EXPECT_EQ("22003", state()); EXPECT_EQ(before, output); EXPECT_EQ(79, length);
        }
      }
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 1, SQL_C_BINARY,
          row == 3 ? reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)()) : output.data() + 1,
          row == 3 ? 0 : c.width + 1, &length));
      if (row == 3) { EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(before, output); }
      else {
        std::array<unsigned char, 8> expected{};
        if (c.width == 2) { SQLSMALLINT value = row == 0 ? -32768 : row == 1 ? 0 : 32767; std::memcpy(expected.data(), &value, 2); }
        if (c.width == 4) { SQLINTEGER value = row == 0 ? (std::numeric_limits<SQLINTEGER>::min)() : row == 1 ? 0 : (std::numeric_limits<SQLINTEGER>::max)(); std::memcpy(expected.data(), &value, 4); }
        if (c.width == 8) { SQLBIGINT value = row == 0 ? (std::numeric_limits<SQLBIGINT>::min)() : row == 1 ? 0 : (std::numeric_limits<SQLBIGINT>::max)(); std::memcpy(expected.data(), &value, 8); }
        EXPECT_EQ(c.width, length);
        for (SQLLEN byte = 0; byte < c.width; ++byte) EXPECT_EQ(expected[byte], output[byte + 1]);
        EXPECT_EQ(0xa5, output[0]);
        for (std::size_t byte = c.width + 1; byte < output.size(); ++byte) EXPECT_EQ(0xa5, output[byte]);
      }
      const auto completed = output; length = 83;
      ASSERT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, SQL_C_BINARY, output.data() + 1, c.width, &length));
      EXPECT_EQ(completed, output); EXPECT_EQ(83, length);
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  }
  EXPECT_EQ(0u, facts->disconnects);
}
}  // namespace

namespace {
TEST_F(NumericScaleApiTest, SignedIntegerBinaryResultBoundAndIndicatorOnlyValidateBeforePublication) {
  struct Case { ScalarType type; SQLLEN width; const char* overflow; const char* underflow; };
  for (const auto& c : {Case{ScalarType::SmallInt, 2, "32768", "-32769"},
       Case{ScalarType::Integer, 4, "2147483648", "-2147483649"}, Case{ScalarType::BigInt, 8, "9223372036854775808", "-9223372036854775809"}}) {
    SCOPED_TRACE(c.width);
    facts->result.columns = {{"n", NativeTypeInfo{c.type, 19, 0, true}}};
    struct Status { SQLUSMALLINT before{81}, value{71}, after{82}; } status;
    struct Count { SQLULEN before{83}, value{91}, after{84}; } count;
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, &status.value, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, &count.value, 0));
    for (bool indicator_only : {false, true}) {
      for (int scenario = 0; scenario < 6; ++scenario) {
        SCOPED_TRACE(scenario);
        SCOPED_TRACE(indicator_only);
        facts->result.rows = {{scenario == 5 ? std::optional<std::string>{} :
            std::optional<std::string>{scenario == 2 ? c.overflow : scenario == 3 ? c.underflow : scenario == 4 ? "bad" : "-42"}}};
        std::array<unsigned char, 12> output; output.fill(0xa5); SQLLEN length = 79;
        const auto before = output;
        ASSERT_EQ(SQL_SUCCESS, SQLBindCol(stmt, 1, SQL_C_BINARY,
            indicator_only ? nullptr : output.data() + 1,
            scenario == 0 ? c.width - 1 : c.width + 1, &length));
        ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS));
        const auto result = SQLFetch(stmt);
        const bool failure = scenario == 2 || scenario == 3 || scenario == 4 || (scenario == 0 && !indicator_only);
        ASSERT_EQ(failure ? SQL_ERROR : SQL_SUCCESS, result);
        EXPECT_EQ(1u, count.value);
        EXPECT_EQ(failure ? SQL_ROW_ERROR : SQL_ROW_SUCCESS, status.value);
        if (failure) {
          EXPECT_EQ(scenario == 4 ? "22018" : "22003", state());
          EXPECT_EQ(before, output); EXPECT_EQ(79, length);
        } else if (scenario == 5) {
          EXPECT_EQ(SQL_NULL_DATA, length); EXPECT_EQ(before, output);
        } else {
          EXPECT_EQ(c.width, length);
          if (indicator_only) EXPECT_EQ(before, output);
          else {
            std::array<unsigned char, 8> expected{};
            if (c.width == 2) { SQLSMALLINT v = -42; std::memcpy(expected.data(), &v, 2); }
            if (c.width == 4) { SQLINTEGER v = -42; std::memcpy(expected.data(), &v, 4); }
            if (c.width == 8) { SQLBIGINT v = -42; std::memcpy(expected.data(), &v, 8); }
            for (SQLLEN byte = 0; byte < c.width; ++byte) EXPECT_EQ(expected[byte], output[byte + 1]);
            EXPECT_EQ(0xa5, output[0]);
            for (std::size_t byte = c.width + 1; byte < output.size(); ++byte) EXPECT_EQ(0xa5, output[byte]);
          }
        }
        EXPECT_EQ(81, status.before); EXPECT_EQ(82, status.after); EXPECT_EQ(83u, count.before); EXPECT_EQ(84u, count.after);
        ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
      }
    }
    ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_UNBIND));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROW_STATUS_PTR, nullptr, 0));
    ASSERT_EQ(SQL_SUCCESS, SQLSetStmtAttr(stmt, SQL_ATTR_ROWS_FETCHED_PTR, nullptr, 0));
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, SignedIntegerBinaryResultArdAuthorityRangeAndNullPrecedence) {
  facts->result.columns = {{"small", NativeTypeInfo{ScalarType::SmallInt, 5, 0, true}},
      {"integer", NativeTypeInfo{ScalarType::Integer, 10, 0, true}},
      {"big", NativeTypeInfo{ScalarType::BigInt, 19, 0, true}}};
  facts->result.rows = {{"bad", "2147483648", "9223372036854775808"}};
  auto ard = descriptor(SQL_ATTR_APP_ROW_DESC);
  for (SQLSMALLINT col = 1; col <= 3; ++col) {
    ASSERT_EQ(SQL_SUCCESS, SQLSetDescField(ard, col, SQL_DESC_CONCISE_TYPE,
        reinterpret_cast<SQLPOINTER>(SQL_C_BINARY), 0));
  }
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  std::array<unsigned char, 12> output; output.fill(0xa5); const auto before = output; SQLLEN length = 79;
  for (SQLUSMALLINT col = 1; col <= 3; ++col) {
    ASSERT_EQ(SQL_ERROR, SQLGetData(stmt, col, SQL_ARD_TYPE, output.data() + 1, 8, &length));
    EXPECT_EQ(col == 1 ? "22018" : "22003", state()); EXPECT_EQ(before, output); EXPECT_EQ(79, length);
  }
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE));
  facts->result.rows = {{"-42", "-42", "-42"}, {std::nullopt, std::nullopt, std::nullopt}};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  for (SQLUSMALLINT col = 1; col <= 3; ++col) {
    const SQLLEN width = col == 1 ? 2 : col == 2 ? 4 : 8;
    output.fill(0xa5); length = 79;
    ASSERT_EQ(SQL_ERROR, SQLGetData(stmt, col, SQL_ARD_TYPE, output.data() + 1, -1, &length));
    EXPECT_EQ("HY090", state()); EXPECT_EQ(before, output); EXPECT_EQ(79, length);
    ASSERT_EQ(SQL_ERROR, SQLGetData(stmt, col, SQL_ARD_TYPE, output.data() + 1, width - 1, &length));
    EXPECT_EQ("22003", state()); EXPECT_EQ(before, output); EXPECT_EQ(79, length);
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, col, SQL_ARD_TYPE, output.data() + 1, width, &length));
    EXPECT_EQ(width, length);
    const auto completed = output; length = 83;
    ASSERT_EQ(SQL_NO_DATA, SQLGetData(stmt, col, SQL_ARD_TYPE, output.data() + 1, width, &length));
    EXPECT_EQ(completed, output); EXPECT_EQ(83, length);
  }
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  for (SQLUSMALLINT col = 1; col <= 3; ++col) {
    ASSERT_EQ(SQL_ERROR, SQLGetData(stmt, col, SQL_ARD_TYPE, nullptr, 0, &length)); EXPECT_EQ("HY009", state());
    ASSERT_EQ(SQL_ERROR, SQLGetData(stmt, col, SQL_ARD_TYPE, output.data(), 0, nullptr)); EXPECT_EQ("22002", state());
    ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, col, SQL_ARD_TYPE,
        reinterpret_cast<void*>((std::numeric_limits<std::uintptr_t>::max)()), 0, &length));
    EXPECT_EQ(SQL_NULL_DATA, length);
  }
  EXPECT_EQ(0u, facts->disconnects);
}

TEST_F(NumericScaleApiTest, SignedIntegerBinaryResultPreparedOwnsSnapshotSiblingAndRefill) {
  facts->result.columns = {{"small", NativeTypeInfo{ScalarType::SmallInt, 5, 0, true}},
      {"integer", NativeTypeInfo{ScalarType::Integer, 10, 0, true}},
      {"big", NativeTypeInfo{ScalarType::BigInt, 19, 0, true}}};
  facts->result.normalized_parameter_types = {{ScalarType::BigInt, 19, 0, true}};
  facts->result.rows = {{"-42", "-42", "-42"}};
  SQLHSTMT sibling{}; ASSERT_EQ(SQL_SUCCESS, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &sibling));
  struct Owner { SQLHSTMT h; ~Owner() { SQLFreeHandle(SQL_HANDLE_STMT, h); } } owner{sibling};
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(sibling, (SQLCHAR*)"SELECT n", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLPrepare(stmt, (SQLCHAR*)"SELECT ?", SQL_NTS));
  SQLBIGINT input = 42; SQLLEN input_length = 0;
  ASSERT_EQ(SQL_SUCCESS, SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 19, 0, &input, 0, &input_length));
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); input = 999;
  facts->result.rows = {{"7", "8", "9"}};
  ASSERT_EQ(1u, facts->parameters.size()); EXPECT_EQ(std::optional<std::string>{"42"}, facts->parameters[0].value);
  const auto queries = facts->queries;
  for (auto handle : {stmt, sibling}) {
    ASSERT_EQ(SQL_SUCCESS, SQLFetch(handle));
    for (SQLUSMALLINT col = 1; col <= 3; ++col) {
      const SQLLEN width = col == 1 ? 2 : col == 2 ? 4 : 8;
      std::array<unsigned char, 10> output{}; SQLLEN length = 79;
      ASSERT_EQ(SQL_SUCCESS, SQLGetData(handle, col, SQL_C_BINARY, output.data() + 1, width, &length));
      std::array<unsigned char, 8> expected{};
      if (width == 2) { SQLSMALLINT v = -42; std::memcpy(expected.data(), &v, 2); }
      if (width == 4) { SQLINTEGER v = -42; std::memcpy(expected.data(), &v, 4); }
      if (width == 8) { SQLBIGINT v = -42; std::memcpy(expected.data(), &v, 8); }
      for (SQLLEN byte = 0; byte < width; ++byte) EXPECT_EQ(expected[byte], output[byte + 1]);
      EXPECT_EQ(width, length);
    }
  }
  EXPECT_EQ(queries, facts->queries);
  ASSERT_EQ(SQL_SUCCESS, SQLFreeStmt(stmt, SQL_CLOSE)); input = -42;
  ASSERT_EQ(SQL_SUCCESS, SQLExecute(stmt)); ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  SQLBIGINT output = 79; SQLLEN length = 81;
  ASSERT_EQ(SQL_SUCCESS, SQLGetData(stmt, 3, SQL_C_DEFAULT, &output, 0, &length));
  EXPECT_EQ(9, output); EXPECT_EQ(8, length);
  EXPECT_EQ(0u, facts->disconnects);
}
}  // namespace

namespace {
TEST_F(NumericScaleApiTest, SignedIntegerBinaryResultCheckedValueWarningStillCompletesOnce) {
  facts->result.columns = {{"n", NativeTypeInfo{ScalarType::SmallInt, 5, 0, true}}};
  facts->result.rows = {{"1.5"}}; // Preserve the existing checked-converter warning policy.
  ASSERT_EQ(SQL_SUCCESS, SQLExecDirect(stmt, (SQLCHAR*)"SELECT n", SQL_NTS));
  ASSERT_EQ(SQL_SUCCESS, SQLFetch(stmt));
  unsigned char output[4]{0xa5, 0xa5, 0xa5, 0xa5}; SQLLEN length = 79;
  ASSERT_EQ(SQL_SUCCESS_WITH_INFO, SQLGetData(stmt, 1, SQL_C_BINARY, output + 1, 2, &length));
  EXPECT_EQ("01S07", state()); EXPECT_EQ(2, length);
  const SQLSMALLINT expected = 1; EXPECT_EQ(0, std::memcmp(&expected, output + 1, 2));
  EXPECT_EQ(0xa5, output[0]); EXPECT_EQ(0xa5, output[3]);
  length = 83;
  ASSERT_EQ(SQL_NO_DATA, SQLGetData(stmt, 1, SQL_C_BINARY, output + 1, 2, &length));
  EXPECT_EQ(83, length); EXPECT_EQ(0, std::memcmp(&expected, output + 1, 2));
}
}  // namespace
