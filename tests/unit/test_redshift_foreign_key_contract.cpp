#include "core/database/postgres/redshift_foreign_key_contract.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>

using namespace rs::core::database;
using namespace rs::core::database::postgres;

namespace {
using Direction = RedshiftForeignKeyDirection;
RedshiftForeignKeyTable parent() { return {"db", "fixture", "parent"}; }
RedshiftForeignKeyTable child() { return {"db", "fixture", "child"}; }
RedshiftForeignKeyCommandPlan plan(Direction direction = Direction::Imported) {
  return {direction, direction == Direction::Imported ? std::nullopt : std::optional{parent()},
      direction == Direction::Exported ? std::nullopt : std::optional{child()}};
}
constexpr std::array<const char*, 14> input_names{"pk_database_name", "pk_schema_name", "pk_table_name", "pk_column_name",
    "fk_database_name", "fk_schema_name", "fk_table_name", "fk_column_name", "key_seq", "update_rule",
    "delete_rule", "fk_name", "pk_name", "deferrability"};
constexpr std::array<const char*, 14> output_names{"PKTABLE_CAT", "PKTABLE_SCHEM", "PKTABLE_NAME", "PKCOLUMN_NAME",
    "FKTABLE_CAT", "FKTABLE_SCHEM", "FKTABLE_NAME", "FKCOLUMN_NAME", "KEY_SEQ", "UPDATE_RULE", "DELETE_RULE",
    "FK_NAME", "PK_NAME", "DEFERRABILITY"};
bool numeric(std::size_t i) { return i == 8 || i == 9 || i == 10 || i == 13; }
QueryResult show_keys(ScalarType integer = ScalarType::SmallInt, ScalarType text = ScalarType::VarChar) {
  QueryResult result;
  for (std::size_t i = 0; i < input_names.size(); ++i)
    result.columns.push_back({input_names[i], NativeTypeInfo{numeric(i) ? integer : text, 0, 0, true}});
  result.rows = {
      {"db", "fixture", "parent", "key_b", "db", "fixture", "child", "ref_b", "1", std::nullopt, std::nullopt, "child_fk", "parent_pk", std::nullopt},
      {"db", "fixture", "parent", "key_a", "db", "fixture", "child", "ref_a", "2", std::nullopt, std::nullopt, "child_fk", "parent_pk", std::nullopt}};
  return result;
}
void blocked(QueryResult input, Direction direction = Direction::Imported) {
  auto result = normalize_redshift_foreign_keys(plan(direction), std::move(input));
  ASSERT_FALSE(result);
  EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
  EXPECT_EQ("Invalid Redshift foreign-key metadata", result.error_message());
  EXPECT_EQ(SessionDisposition::Retire, result.session_snapshot().disposition);
}
}  // namespace

TEST(RedshiftForeignKeyContract, DirectionsRequireExactOwningIdentities) {
  for (auto direction : {Direction::Imported, Direction::Exported, Direction::Both}) {
    auto values = plan(direction);
    auto admitted = redshift_foreign_key_plan(direction, values.primary, values.foreign);
    ASSERT_TRUE(admitted);
    if (values.foreign) { values.foreign->table.clear(); EXPECT_EQ("child", admitted->foreign->table); }
    if (values.primary) { values.primary->table.clear(); EXPECT_EQ("parent", admitted->primary->table); }
  }
  EXPECT_FALSE(redshift_foreign_key_plan(Direction::Imported, parent(), child()));
  EXPECT_FALSE(redshift_foreign_key_plan(Direction::Exported, parent(), child()));
  EXPECT_FALSE(redshift_foreign_key_plan(Direction::Both, parent(), std::nullopt));
  EXPECT_FALSE(redshift_foreign_key_plan(Direction::Both, std::nullopt, child()));
  EXPECT_FALSE(redshift_foreign_key_plan(Direction::Imported, std::nullopt, std::nullopt));
  EXPECT_FALSE(redshift_foreign_key_plan(Direction::Exported, std::nullopt, std::nullopt));
  EXPECT_FALSE(redshift_foreign_key_plan(static_cast<Direction>(99), parent(), child()));
  for (const auto& bad : {std::string{}, std::string("x\0secret", 8), std::string("\xff", 1)}) {
    for (int field = 0; field < 3; ++field) {
      auto value = parent();
      (field == 0 ? value.database : field == 1 ? value.schema : value.table) = bad;
      EXPECT_FALSE(redshift_foreign_key_plan(Direction::Exported, value, std::nullopt));
    }
  }
  auto quoted = child(); quoted.table = "literal'\"_%";
  auto admitted = redshift_foreign_key_plan(Direction::Imported, std::nullopt, quoted);
  ASSERT_TRUE(admitted); EXPECT_EQ(quoted.table, admitted->foreign->table);
}

TEST(RedshiftForeignKeyContract, AllDirectionsNormalizeOwningFieldsDefaultsAndSnapshots) {
  for (auto direction : {Direction::Imported, Direction::Exported, Direction::Both}) {
    for (auto integer : {ScalarType::SmallInt, ScalarType::Integer}) {
      for (auto text : {ScalarType::Char, ScalarType::VarChar, ScalarType::LongVarChar}) {
        auto input = show_keys(integer, text);
        // Remap schema order rather than assuming SHOW descriptor order.
        std::reverse(input.columns.begin(), input.columns.end());
        for (auto& row : input.rows) std::reverse(row.begin(), row.end());
        const SessionSnapshot snapshot{SessionState::Transaction, SessionDisposition::ResetRequired};
        auto result = normalize_redshift_foreign_keys(plan(direction), BackendResult<QueryResult>{input, snapshot});
        ASSERT_TRUE(result);
        input.columns.clear(); input.rows.clear();
        ASSERT_EQ(14u, result->columns.size()); ASSERT_EQ(2u, result->rows.size());
        EXPECT_EQ(snapshot, result.session_snapshot());
        EXPECT_EQ("key_b", result->rows[0][3]); EXPECT_EQ("ref_b", result->rows[0][7]);
        EXPECT_EQ("1", result->rows[0][8]); EXPECT_EQ("key_a", result->rows[1][3]);
        for (std::size_t i = 0; i < output_names.size(); ++i) {
          EXPECT_EQ(output_names[i], result->columns[i].name);
          ASSERT_TRUE(result->columns[i].normalized_type);
          const auto metadata = *result->columns[i].normalized_type;
          EXPECT_TRUE(metadata.known);
          EXPECT_EQ(numeric(i) ? ScalarType::SmallInt : ScalarType::VarChar, metadata.type);
          EXPECT_EQ(numeric(i) ? 5u : 0u, metadata.column_size);
          EXPECT_EQ(0, metadata.decimal_digits);
        }
        for (auto& row : result->rows) {
          EXPECT_EQ("3", row[9]); EXPECT_EQ("3", row[10]); EXPECT_EQ("7", row[13]);
        }
      }
    }
  }
}

TEST(RedshiftForeignKeyContract, NamedGroupsRestartSequenceAndPreserveSourceOrder) {
  auto input = show_keys();
  auto other = input.rows;
  for (auto& row : other) { row[11] = "other_fk"; row[12] = std::nullopt; }
  input.rows = {input.rows[1], other[0], input.rows[0], other[1]};
  auto result = normalize_redshift_foreign_keys(plan(), input);
  ASSERT_TRUE(result); ASSERT_EQ(4u, result->rows.size());
  EXPECT_EQ("2", result->rows[0][8]); EXPECT_EQ("other_fk", result->rows[1][11]);
  EXPECT_FALSE(result->rows[1][12]); EXPECT_FALSE(result->rows[3][12]);
  // Constraint names have table-local scope on exported results.
  input = show_keys(); other = input.rows;
  for (auto& row : other) row[6] = "another_child";
  input.rows.insert(input.rows.end(), other.begin(), other.end());
  EXPECT_TRUE(normalize_redshift_foreign_keys(plan(Direction::Exported), input));
}

TEST(RedshiftForeignKeyContract, BothFiltersOnlyAfterValidatingEveryGroup) {
  auto input = show_keys(); auto excluded = input.rows;
  for (auto& row : excluded) { row[2] = "other_parent"; row[11] = "excluded_fk"; }
  input.rows.insert(input.rows.end(), excluded.begin(), excluded.end());
  auto selected = normalize_redshift_foreign_keys(plan(Direction::Both), input);
  ASSERT_TRUE(selected); ASSERT_EQ(2u, selected->rows.size());
  EXPECT_EQ("parent", selected->rows[0][2]);
  EXPECT_TRUE(normalize_redshift_foreign_keys(plan(Direction::Imported), input));
  blocked(input, Direction::Exported);
  input.rows.back()[8] = "1"; // Excluded group's duplicate sequence must block.
  blocked(input, Direction::Both);
  input = show_keys(); for (auto& row : input.rows) row[2] = "other_parent";
  auto empty = normalize_redshift_foreign_keys(plan(Direction::Both), input);
  ASSERT_TRUE(empty); EXPECT_TRUE(empty->rows.empty()); EXPECT_EQ(14u, empty->columns.size());
}

TEST(RedshiftForeignKeyContract, EmptyResultsAndExplicitLegalRules) {
  auto input = show_keys(); input.rows.clear();
  auto empty = normalize_redshift_foreign_keys(plan(), input);
  ASSERT_TRUE(empty); EXPECT_EQ(14u, empty->columns.size()); EXPECT_TRUE(empty->rows.empty());
  for (int action = 0; action <= 4; ++action) {
    for (int deferrability = 5; deferrability <= 7; ++deferrability) {
      input = show_keys();
      for (auto& row : input.rows) { row[9] = std::to_string(action); row[10] = std::to_string(action); row[13] = std::to_string(deferrability); }
      auto result = normalize_redshift_foreign_keys(plan(), input);
      ASSERT_TRUE(result); EXPECT_EQ(std::to_string(action), result->rows[0][9]);
      EXPECT_EQ(std::to_string(deferrability), result->rows[0][13]);
    }
  }
}

TEST(RedshiftForeignKeyContract, MalformedRequiredCellsSequencesAndEnumsBlock) {
  for (std::size_t column : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 11u}) {
    auto input = show_keys(); input.rows.back()[column] = std::nullopt; blocked(input);
    input = show_keys(); input.rows.back()[column] = ""; blocked(input);
    input = show_keys(); input.rows.back()[column] = std::string("x\0secret", 8); blocked(input);
    input = show_keys(); input.rows.back()[column] = std::string("\xff", 1); blocked(input);
  }
  for (std::size_t column : {8u, 9u, 10u, 13u}) {
    for (const auto value : {"", "-1", "+1", " 1", "1x", "999999999999999", "32768"}) {
      auto input = show_keys(); input.rows.back()[column] = value; blocked(input);
    }
  }
  for (auto sequence : {"0", "1", "3"}) { auto input = show_keys(); input.rows.back()[8] = sequence; blocked(input); }
  for (std::size_t column : {9u, 10u}) { auto input = show_keys(); input.rows.back()[column] = "5"; blocked(input); }
  for (auto value : {"0", "4", "8"}) { auto input = show_keys(); input.rows.back()[13] = value; blocked(input); }
}

TEST(RedshiftForeignKeyContract, InconsistentGroupsPairsAndTargetsBlock) {
  for (std::size_t column : {0u, 1u, 2u, 12u}) {
    auto input = show_keys(); input.rows.back()[column] = "other"; blocked(input);
  }
  auto input = show_keys(); input.rows.back()[12] = std::nullopt; blocked(input);
  input = show_keys(); input.rows.back()[12] = ""; blocked(input);
  input = show_keys(); input.rows.back()[12] = std::string("\xff", 1); blocked(input);
  input = show_keys(); input.rows.back()[3] = "key_b"; input.rows.back()[7] = "ref_b"; blocked(input);
  for (std::size_t column : {9u, 10u, 13u}) {
    input = show_keys(); input.rows.back()[column] = column == 13 ? "5" : "0"; blocked(input);
  }
  for (std::size_t column : {4u, 5u, 6u}) {
    input = show_keys(); input.rows.back()[column] = "foreign"; blocked(input);
  }
}

TEST(RedshiftForeignKeyContract, MalformedStructureAndMetadataNeverPublishPartialSuccess) {
  auto input = show_keys(); input.columns[0].name = input.columns[1].name; blocked(input);
  input = show_keys(); input.columns[0].name = "unexpected"; blocked(input);
  input = show_keys(); input.columns.pop_back(); blocked(input);
  input = show_keys(); input.rows.back().pop_back(); blocked(input);
  for (std::size_t column : {0u, 8u, 9u, 10u, 13u}) {
    input = show_keys(); input.columns[column].normalized_type.reset(); blocked(input);
    input = show_keys(); input.columns[column].normalized_type->known = false; blocked(input);
    input = show_keys(); input.columns[column].normalized_type->type = ScalarType::Binary; blocked(input);
  }
  input = show_keys(); input.cell_errors.push_back({1, 3}); blocked(input);
  input = show_keys(); input.additional_results.push_back(QueryResult{}); blocked(input);
  input = show_keys(); input.normalized_parameter_types.push_back(NativeTypeInfo{}); blocked(input);
  input = show_keys(); input.affected_rows = 1; blocked(input);
  auto invalid_plan = plan(); invalid_plan.foreign.reset();
  EXPECT_FALSE(normalize_redshift_foreign_keys(invalid_plan, show_keys()));
}

TEST(RedshiftForeignKeyContract, NativeAndDeferredFailuresPreserveAllOwningDetails) {
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "private server diagnostic"};
  error.native_state = "42501"; error.native_code = 17;
  error.operation = BackendOperation::ExecutePrepared;
  error.session_state = SessionState::Transaction; error.disposition = SessionDisposition::ResetRequired;
  error.retry_safe = false;
  for (int form = 0; form < 3; ++form) {
    auto input = show_keys();
    if (form == 1) input.error = error;
    if (form == 2) { QueryResult deferred; deferred.error = error; input.additional_results.push_back(deferred); }
    auto result = normalize_redshift_foreign_keys(plan(), form == 0 ? BackendResult<QueryResult>{error} : BackendResult<QueryResult>{input});
    ASSERT_FALSE(result);
    error.message = "mutated caller storage";
    EXPECT_EQ("private server diagnostic", result.error_message()); EXPECT_EQ(error.code, result.error());
    EXPECT_EQ(error.error_class, result.backend_error().error_class);
    EXPECT_EQ(error.native_state, result.backend_error().native_state); EXPECT_EQ(error.native_code, result.backend_error().native_code);
    EXPECT_EQ(error.operation, result.backend_error().operation); EXPECT_EQ(error.retry_safe, result.backend_error().retry_safe);
    EXPECT_EQ((SessionSnapshot{error.session_state, error.disposition}), result.session_snapshot());
    error.message = "private server diagnostic";
  }
}


TEST(RedshiftForeignKeyContract, PreparedNameMetadataIsAbsentOrExactlyThreeKnownTextInputs) {
  for (auto type : {ScalarType::Char, ScalarType::VarChar, ScalarType::LongVarChar}) {
    auto source = show_keys();
    source.normalized_parameter_types.assign(3, NativeTypeInfo{type, 0, 0, true});
    auto result = normalize_redshift_foreign_keys(plan(), source);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->normalized_parameter_types.empty());
    EXPECT_EQ("1", result->rows[0][8]); EXPECT_EQ("3", result->rows[0][9]);
    source.normalized_parameter_types.clear(); source.rows.clear();
    EXPECT_EQ("child_fk", result->rows[0][11]);
  }
  for (unsigned variant = 0; variant < 4; ++variant) {
    auto source = show_keys();
    source.normalized_parameter_types.assign(3, NativeTypeInfo{ScalarType::VarChar, 0, 0, true});
    if (variant == 0) source.normalized_parameter_types.pop_back();
    if (variant == 1) source.normalized_parameter_types.push_back(source.normalized_parameter_types[0]);
    if (variant == 2) source.normalized_parameter_types[1].known = false;
    if (variant == 3) source.normalized_parameter_types[1].type = ScalarType::Integer;
    ASSERT_NO_FATAL_FAILURE(blocked(std::move(source)));
  }
}


TEST(RedshiftForeignKeyContract, CapturedShowNullTextRulesNormalizeOwningCompositeMetadata) {
  QueryResult source;
  source.columns = {
      {"pk_database_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"pk_schema_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"pk_table_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"pk_column_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"fk_database_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"fk_schema_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"fk_table_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"fk_column_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"key_seq", NativeTypeInfo{ScalarType::SmallInt, 5u, 0, true}},
      {"fk_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"pk_name", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"update_rule", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"delete_rule", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}},
      {"deferrability", NativeTypeInfo{ScalarType::VarChar, 65535u, 0, true}}};
  source.rows = {
      {"odbcpp_pilot", "odbcpp_fixture", "m2_catalog_parent_20261003_c01", "key_b", "odbcpp_pilot", "odbcpp_fixture", "m2_catalog_child_20261003_c01", "ref_b", "1", "m2_catalog_child_20261003_c01_ref_b_fkey", "m2_catalog_parent_20261003_c01_pkey", std::nullopt, std::nullopt, std::nullopt},
      {"odbcpp_pilot", "odbcpp_fixture", "m2_catalog_parent_20261003_c01", "key_a", "odbcpp_pilot", "odbcpp_fixture", "m2_catalog_child_20261003_c01", "ref_a", "2", "m2_catalog_child_20261003_c01_ref_b_fkey", "m2_catalog_parent_20261003_c01_pkey", std::nullopt, std::nullopt, std::nullopt}};
  source.normalized_parameter_types.assign(3, NativeTypeInfo{ScalarType::VarChar, 0, 0, true});
  const RedshiftForeignKeyCommandPlan requested{Direction::Imported, std::nullopt,
      RedshiftForeignKeyTable{"odbcpp_pilot", "odbcpp_fixture", "m2_catalog_child_20261003_c01"}};
  const SessionSnapshot snapshot{SessionState::Transaction, SessionDisposition::ResetRequired};
  auto result = normalize_redshift_foreign_keys(requested, BackendResult<QueryResult>{source, snapshot});
  ASSERT_TRUE(result);
  source.columns.clear(); source.rows.clear(); source.normalized_parameter_types.clear();
  ASSERT_EQ(14u, result->columns.size()); ASSERT_EQ(2u, result->rows.size());
  EXPECT_EQ(snapshot, result.session_snapshot());
  EXPECT_TRUE(result->normalized_parameter_types.empty());
  EXPECT_EQ("key_b", result->rows[0][3]); EXPECT_EQ("ref_b", result->rows[0][7]);
  EXPECT_EQ("key_a", result->rows[1][3]); EXPECT_EQ("ref_a", result->rows[1][7]);
  EXPECT_EQ("1", result->rows[0][8]); EXPECT_EQ("2", result->rows[1][8]);
  for (std::size_t i = 0; i < output_names.size(); ++i) {
    EXPECT_EQ(output_names[i], result->columns[i].name);
    ASSERT_TRUE(result->columns[i].normalized_type);
    EXPECT_TRUE(result->columns[i].normalized_type->known);
    EXPECT_EQ(numeric(i) ? ScalarType::SmallInt : ScalarType::VarChar,
        result->columns[i].normalized_type->type);
  }
  for (const auto& row : result->rows) {
    EXPECT_EQ("3", row[9]); EXPECT_EQ("3", row[10]); EXPECT_EQ("7", row[13]);
    EXPECT_EQ("m2_catalog_child_20261003_c01_ref_b_fkey", row[11]);
    EXPECT_EQ("m2_catalog_parent_20261003_c01_pkey", row[12]);
  }
}

TEST(RedshiftForeignKeyContract, NullTextRulesUseMatchedColumnsAndAdmitEmptyRows) {
  for (auto type : {ScalarType::Char, ScalarType::VarChar, ScalarType::LongVarChar}) {
    auto source = show_keys();
    for (std::size_t i : {9u, 10u, 13u}) source.columns[i].normalized_type->type = type;
    std::reverse(source.columns.begin(), source.columns.end());
    for (auto& row : source.rows) std::reverse(row.begin(), row.end());
    auto result = normalize_redshift_foreign_keys(plan(), source);
    ASSERT_TRUE(result); ASSERT_EQ(2u, result->rows.size());
    EXPECT_EQ("3", result->rows[0][9]); EXPECT_EQ("3", result->rows[1][10]);
    EXPECT_EQ("7", result->rows[1][13]);
    source.rows.clear();
    auto empty = normalize_redshift_foreign_keys(plan(), source);
    ASSERT_TRUE(empty); EXPECT_TRUE(empty->rows.empty()); EXPECT_EQ(14u, empty->columns.size());
  }
}

TEST(RedshiftForeignKeyContract, NullTextRuleExceptionNeverAdmitsKeySequenceOrValues) {
  auto source = show_keys(); source.columns[8].normalized_type->type = ScalarType::VarChar;
  ASSERT_NO_FATAL_FAILURE(blocked(source));
  source.rows.clear(); ASSERT_NO_FATAL_FAILURE(blocked(source));
  for (std::size_t i : {9u, 10u, 13u}) {
    for (auto type : {ScalarType::Char, ScalarType::VarChar, ScalarType::LongVarChar}) {
      source = show_keys(); source.columns[i].normalized_type->type = type;
      for (auto& row : source.rows) row[i] = i == 13 ? "7" : "3";
      ASSERT_NO_FATAL_FAILURE(blocked(source)); // No numeric-string conversion.
      source.rows[0][i] = std::nullopt;
      ASSERT_NO_FATAL_FAILURE(blocked(source)); // One NULL does not license a mixed column.
      source.rows[1][i] = std::nullopt;
      EXPECT_TRUE(normalize_redshift_foreign_keys(plan(), source));
      source.columns[i].normalized_type->known = false;
      ASSERT_NO_FATAL_FAILURE(blocked(source));
      source.columns[i].normalized_type->known = true;
      source.columns[i].normalized_type->type = ScalarType::Binary;
      ASSERT_NO_FATAL_FAILURE(blocked(source));
    }
  }
}

TEST(RedshiftForeignKeyContract, NullTextRulesStillRequireValidStructureAndRecover) {
  auto source = show_keys();
  for (std::size_t i : {9u, 10u, 13u}) source.columns[i].normalized_type->type = ScalarType::VarChar;
  auto malformed = source; malformed.rows.back().pop_back();
  ASSERT_NO_FATAL_FAILURE(blocked(malformed));
  malformed = source; malformed.columns[9].name = malformed.columns[10].name;
  ASSERT_NO_FATAL_FAILURE(blocked(malformed));
  malformed = source; malformed.cell_errors.push_back({1, 9});
  ASSERT_NO_FATAL_FAILURE(blocked(malformed));
  auto recovered = normalize_redshift_foreign_keys(plan(), source);
  ASSERT_TRUE(recovered); EXPECT_EQ("3", recovered->rows[0][9]); EXPECT_EQ("7", recovered->rows[1][13]);
  source = show_keys(); source.rows[0][9] = "5";
  ASSERT_NO_FATAL_FAILURE(blocked(source)); // Existing numeric enum bounds remain strict.
  EXPECT_TRUE(normalize_redshift_foreign_keys(plan(), show_keys()));
}
