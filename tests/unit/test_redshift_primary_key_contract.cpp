#include "core/database/postgres/redshift_primary_key_contract.h"
#include <gtest/gtest.h>

using namespace rs::core::database;
using namespace rs::core::database::postgres;

namespace {
RedshiftPrimaryKeyCommandPlan plan() { return {"db", "odbcpp_fixture", "parent"}; }
QueryResult show_keys(ScalarType integer = ScalarType::SmallInt) {
  QueryResult result;
  // Actual SHOW order differs from the ODBC output field order.
  for (const auto* name : {"database_name", "schema_name", "table_name", "pk_name", "column_name", "key_seq"})
    result.columns.push_back({name, NativeTypeInfo{
        std::string(name) == "key_seq" ? integer : ScalarType::VarChar, 0, 0, true}});
  result.rows = {{"db", "odbcpp_fixture", "parent", "parent_pkey", "key_b", "1"},
                 {"db", "odbcpp_fixture", "parent", "parent_pkey", "key_a", "2"}};
  return result;
}
void blocked(QueryResult input) {
  const auto result = normalize_redshift_primary_keys(plan(), std::move(input));
  ASSERT_FALSE(result);
  EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
  EXPECT_EQ("Invalid Redshift primary-key metadata", result.error_message());
  EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
}
}  // namespace

TEST(RedshiftPrimaryKeyContract, ExactPlanOwnsNamesWithoutRenderingSql) {
  std::string database = "db'\"_%";
  auto result = redshift_primary_key_plan(database, "schema", "table");
  ASSERT_TRUE(result);
  database.clear();
  EXPECT_EQ("db'\"_%", result->database);
  for (const auto& missing : {std::optional<std::string>{}, std::optional<std::string>{""},
      std::optional<std::string>{std::string("secret\0suffix", 13)},
      std::optional<std::string>{std::string("\xff", 1)}}) {
    EXPECT_FALSE(redshift_primary_key_plan(missing, "schema", "table"));
    EXPECT_FALSE(redshift_primary_key_plan("db", missing, "table"));
    EXPECT_FALSE(redshift_primary_key_plan("db", "schema", missing));
  }
}

TEST(RedshiftPrimaryKeyContract, CompositeOrderMetadataAndOwnership) {
  for (auto family : {ScalarType::SmallInt, ScalarType::Integer}) {
    auto input = show_keys(family);
    const SessionSnapshot snapshot{SessionState::Idle, SessionDisposition::Reusable};
    auto result = normalize_redshift_primary_keys(plan(), BackendResult<QueryResult>{input, snapshot});
    ASSERT_TRUE(result);
    input.rows.clear(); input.columns.clear();
    ASSERT_EQ(2u, result->rows.size());
    EXPECT_EQ((ResultRow{"db", "odbcpp_fixture", "parent", "key_b", "1", "parent_pkey"}), result->rows[0]);
    EXPECT_EQ("key_a", result->rows[1][3]);
    EXPECT_EQ(snapshot, result.session_snapshot());
    const std::vector<std::string> names{"TABLE_CAT", "TABLE_SCHEM", "TABLE_NAME", "COLUMN_NAME", "KEY_SEQ", "PK_NAME"};
    ASSERT_EQ(names.size(), result->columns.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
      EXPECT_EQ(names[i], result->columns[i].name);
      ASSERT_TRUE(result->columns[i].normalized_type);
      const auto type = *result->columns[i].normalized_type;
      EXPECT_TRUE(type.known);
      EXPECT_EQ(i == 4 ? ScalarType::SmallInt : ScalarType::VarChar, type.type);
      EXPECT_EQ(i == 4 ? 5u : 0u, type.column_size);
    }
    EXPECT_TRUE(result->additional_results.empty());
  }
}

TEST(RedshiftPrimaryKeyContract, PreparedIdentifierMetadataIsValidatedWithoutBecomingCatalogOutput) {
  auto input = show_keys();
  input.normalized_parameter_types = {{ScalarType::Char, 0, 0, true},
      {ScalarType::VarChar, 0, 0, true}, {ScalarType::LongVarChar, 0, 0, true}};
  const SessionSnapshot snapshot{SessionState::Idle, SessionDisposition::Reusable};
  auto result = normalize_redshift_primary_keys(plan(), BackendResult<QueryResult>{input, snapshot});
  ASSERT_TRUE(result); EXPECT_EQ(snapshot, result.session_snapshot());
  EXPECT_TRUE(result->normalized_parameter_types.empty());
  EXPECT_EQ("key_b", result->rows[0][3]);
  input.normalized_parameter_types[1].known = false; blocked(input);
  input.normalized_parameter_types[1] = {ScalarType::Integer, 0, 0, true}; blocked(input);
  for (std::size_t count : {1u, 2u, 4u}) {
    input.normalized_parameter_types.assign(count, {ScalarType::VarChar, 0, 0, true}); blocked(input);
  }
}

TEST(RedshiftPrimaryKeyContract, EmptyAndNullableConstraintNameAndSourceOrder) {
  auto input = show_keys(); input.rows.clear();
  auto empty = normalize_redshift_primary_keys(plan(), input);
  ASSERT_TRUE(empty); EXPECT_TRUE(empty->rows.empty()); EXPECT_EQ(6u, empty->columns.size());
  input = show_keys();
  for (auto& row : input.rows) row[3] = std::nullopt;
  std::swap(input.rows[0], input.rows[1]);
  auto nullable = normalize_redshift_primary_keys(plan(), input);
  ASSERT_TRUE(nullable);
  EXPECT_EQ("2", nullable->rows[0][4]);
  EXPECT_FALSE(nullable->rows[0][5]);
  input.rows[1][3] = "other";
  blocked(input);
}

TEST(RedshiftPrimaryKeyContract, AcceptedTextFamiliesPreserveExactNames) {
  for (const auto family : {ScalarType::Char, ScalarType::VarChar, ScalarType::LongVarChar}) {
    auto input = show_keys();
    for (auto& column : input.columns)
      if (column.name != "key_seq") column.normalized_type->type = family;
    auto result = normalize_redshift_primary_keys(plan(), std::move(input));
    ASSERT_TRUE(result);
    EXPECT_EQ("key_b", result->rows[0][3]);
    EXPECT_EQ("parent_pkey", result->rows[1][5]);
  }
}

TEST(RedshiftPrimaryKeyContract, SignedSequenceBoundaryAndOwningRowLimit) {
  auto input = show_keys();
  input.rows.clear();
  for (int sequence = 1; sequence <= 32767; ++sequence)
    input.rows.push_back({"db", "odbcpp_fixture", "parent", "parent_pkey",
        "key_" + std::to_string(sequence), std::to_string(sequence)});
  auto result = normalize_redshift_primary_keys(plan(), input);
  ASSERT_TRUE(result);
  ASSERT_EQ(32767u, result->rows.size());
  EXPECT_EQ("32767", result->rows.back()[4]);
  input.rows.push_back({"db", "odbcpp_fixture", "parent", "parent_pkey", "overflow", "32768"});
  blocked(std::move(input));
  EXPECT_EQ("key_32767", result->rows.back()[3]);
}

TEST(RedshiftPrimaryKeyContract, MalformedSequencesAndDuplicateKeysBlockWholeResult) {
  for (const auto value : {"", "0", "-1", "+1", "1x", " 1", "32768", "999999999999999999", "3", "1"}) {
    auto input = show_keys(); input.rows[1][5] = value; blocked(input);
  }
  auto input = show_keys(); input.rows[1][5] = std::nullopt; blocked(input);
  input = show_keys(); input.rows[1][4] = "key_b"; blocked(input);
  input = show_keys(); input.rows[1][3] = "other_key"; blocked(input);
}

TEST(RedshiftPrimaryKeyContract, MissingForeignAndMalformedIdentifiersBlock) {
  for (std::size_t i : {0u, 1u, 2u, 4u}) {
    auto input = show_keys(); input.rows[0][i] = std::nullopt; blocked(input);
    input = show_keys(); input.rows[0][i] = ""; blocked(input);
    input = show_keys(); input.rows[0][i] = std::string("x\0secret", 8); blocked(input);
    input = show_keys(); input.rows[0][i] = std::string("\xff", 1); blocked(input);
  }
  auto input = show_keys(); input.rows[0][0] = "foreign"; blocked(input);
  input = show_keys(); input.rows[0][1] = "foreign"; blocked(input);
  input = show_keys(); input.rows[0][2] = "foreign"; blocked(input);
  input = show_keys(); input.rows[0][3] = ""; blocked(input);
  auto bad_plan = plan(); bad_plan.database.clear();
  EXPECT_FALSE(normalize_redshift_primary_keys(bad_plan, show_keys()));
}

TEST(RedshiftPrimaryKeyContract, MalformedStructureAndUnknownMetadataBlock) {
  auto input = show_keys(); input.columns[0].name = "schema_name"; blocked(input);
  input = show_keys(); input.columns.pop_back(); blocked(input);
  input = show_keys(); input.rows[1].pop_back(); blocked(input);
  input = show_keys(); input.columns[0].normalized_type.reset(); blocked(input);
  input = show_keys(); input.columns[0].normalized_type->known = false; blocked(input);
  input = show_keys(); input.columns[5].normalized_type->known = false; blocked(input);
  input = show_keys(); input.columns[0].normalized_type->type = ScalarType::Binary; blocked(input);
  input = show_keys(); input.columns[5].normalized_type->type = ScalarType::VarChar; blocked(input);
  input = show_keys(); input.cell_errors.push_back({0, 0}); blocked(input);
  input = show_keys(); input.additional_results.push_back(QueryResult{}); blocked(input);
  input = show_keys(); input.normalized_parameter_types.push_back(NativeTypeInfo{}); blocked(input);
  input = show_keys(); input.affected_rows = 1; blocked(input);
}

TEST(RedshiftPrimaryKeyContract, ServerAndDeferredFailuresRemainOwningFailures) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "server diagnostic"};
  error.native_state = "42501"; error.native_code = 9;
  error.operation = BackendOperation::ExecutePrepared;
  error.session_state = SessionState::Transaction;
  error.disposition = SessionDisposition::ResetRequired;
  error.retry_safe = false;
  for (int form = 0; form < 3; ++form) {
    auto input = show_keys();
    if (form == 1) input.error = error;
    if (form == 2) { QueryResult deferred; deferred.error = error; input.additional_results.push_back(deferred); }
    auto result = normalize_redshift_primary_keys(plan(), form == 0 ? BackendResult<QueryResult>{error} : BackendResult<QueryResult>{input});
    ASSERT_FALSE(result);
    EXPECT_EQ(error.message, result.error_message());
    EXPECT_EQ(error.code, result.error());
    EXPECT_EQ(error.native_state, result.backend_error().native_state);
    EXPECT_EQ(error.native_code, result.backend_error().native_code);
    EXPECT_EQ(error.operation, result.backend_error().operation);
    EXPECT_EQ(error.retry_safe, result.backend_error().retry_safe);
    EXPECT_EQ(error.disposition, result.session_snapshot().disposition);
  }
}
