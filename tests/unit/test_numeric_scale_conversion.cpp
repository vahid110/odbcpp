#include <gtest/gtest.h>
#include "core/database/postgres/pg_backend_provider.h"
#include "odbc/odbc_api.h"
#include "odbc/odbc_handles.h"
#include "odbcpp/util/scaled_decimal.h"

#include <algorithm>
#include <array>
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
